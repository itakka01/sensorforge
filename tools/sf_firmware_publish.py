#!/usr/bin/env python3
"""SensorForge: Firmware signieren und im LAN für das Cluster-Update bereitstellen.

WAS DAS SKRIPT MACHT
  1. Sammeln: nimmt die angegebenen .bin-Dateien oder sucht selbst den
     neuesten Build je Board in
       ../sensorforge_builds/<board>/sensorforge.ino.bin   (tools/flash_board.sh)
       ~/.cache/arduino/sketches/*/sensorforge.ino.bin      (Arduino IDE)
     Board und Version stehen im Image (Marker). Ohne --any-release werden nur
     Builds der Version aus sensorforge_version.h genommen.
  2. Signieren: legt je Build ein Paket an
       ../sensorforge_builds/ota/firmware/<board>/<version>.sfw
     Ein unverändertes Image wird nicht neu signiert. Alte Pakete bleiben liegen.
  3. Manifest: schreibt ../sensorforge_builds/ota/manifest.json mit GENAU EINEM
     Eintrag je Board – normalerweise der höchsten vorhandenen Version.
  4. Ausliefern: startet einen Webserver (Port 8765), der nur das Manifest und
     die Pakete liefert, und zeigt die Manifest-URL. Diese URL am Coordinator
     unter "Cluster koordinieren -> Firmware im Cluster" eintragen.

WAS DIE GERÄTE PRÜFEN
  Ein Gerät installiert ein Paket nur mit gültiger Signatur, für sein eigenes
  Board und nicht älter als die installierte Version. Ältere Versionen nur,
  wenn am Coordinator "Ältere Version erlauben" angehakt ist. Darum genügt
  unverschlüsseltes http:// im LAN.

VERSION FESTLEGEN (PIN) – z. B. FÜR EIN DOWNGRADE
  Normalerweise steht im Manifest die neueste Version je Board. Mit
    --pin v87-beta158
  steht stattdessen für jedes Board genau diese Version im Manifest (das
  Paket muss im Ordner liegen; Boards ohne diese Version fehlen dann im
  Manifest und werden beim Rollout übersprungen bzw. melden "keine neuere
  Version"). Der Pin wird in ../sensorforge_builds/ota/pin.txt GESPEICHERT
  und gilt auch für spätere Läufe – auch für die automatischen Läufe aus
  tools/flash_board.sh –, bis er mit --unpin aufgehoben wird. Jeder Lauf
  zeigt deutlich an, ob ein Pin aktiv ist.
  Zurück auf eine ältere Version im Cluster:
    1. python3 tools/sf_firmware_publish.py --pin v87-beta158 --serve-only
    2. am Coordinator "Ältere Version erlauben" anhaken, Nodes aktualisieren
    3. danach: python3 tools/sf_firmware_publish.py --unpin --no-serve

BEISPIELE
  python3 tools/sf_firmware_publish.py                  sammeln, signieren, ausliefern
  python3 tools/sf_firmware_publish.py --no-serve       nur signieren + Manifest
  python3 tools/sf_firmware_publish.py --serve-only     vorhandene Pakete ausliefern
  python3 tools/sf_firmware_publish.py a.bin b.bin      genau diese Builds nehmen
  python3 tools/sf_firmware_publish.py --list           Pakete im Ordner und Pin anzeigen
  python3 tools/sf_firmware_publish.py --pin v87-beta158 --no-serve
  python3 tools/sf_firmware_publish.py --unpin --no-serve

  tools/flash_board.sh legt das Paket nach jedem Build selbst an
  (tools/flash_board.sh all --ota-only baut alle Boards nur für WLAN).
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import http.server
import json
import os
import re
import socket
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
import sensorforge_firmware_sign as signer  # noqa: E402

DEFAULT_OUT = REPO.parent / "sensorforge_builds" / "ota"
DEFAULT_KEY = REPO / "sensorforge_firmware_private_DEV.pem"
DEFAULT_PORT = 8765
BOARDS = sorted(signer.BOARD_MARKERS)


def repo_release() -> str:
    text = (REPO / "sensorforge_version.h").read_text(encoding="utf-8")
    m = re.search(r'#define\s+SENSORFORGE_RELEASE_TAG\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("SENSORFORGE_RELEASE_TAG not found in sensorforge_version.h")
    return m.group(1)


def release_key(tag: str):
    """Same order as firmware_package_format.h: vNN-betaMM < vNN (stable)."""
    m = re.fullmatch(r"v(\d+)(?:-beta(\d+))?", tag)
    if not m:
        return None
    major = int(m.group(1))
    return (major, 1, 0) if m.group(2) is None else (major, 0, int(m.group(2)))


def inspect(path: Path):
    """(board, release) of an image, or None if it is not a SensorForge image."""
    try:
        image = path.read_bytes()
        if not image or image[0] != 0xE9:
            return None
        return signer.detect_board(image), signer.detect_release(image)
    except (OSError, ValueError):
        return None


def collect_images(explicit: list[str], want_release: str | None) -> list[tuple[str, Path]]:
    """(board, path): every given file, or automatically the newest build per board."""
    if explicit:
        out = []
        for name in explicit:
            path = Path(name)
            if not path.is_file():
                raise SystemExit(f"nicht gefunden: {path}")
            info = inspect(path)
            if not info:
                raise SystemExit(f"{path}: keine SensorForge-Board-/Versionskennung (erst ab v87-beta158)")
            if want_release and info[1] != want_release:
                raise SystemExit(f"{path}: Version {info[1]}, erwartet {want_release} (sonst --any-release)")
            out.append((info[0], path))
        return out
    candidates = sorted((REPO.parent / "sensorforge_builds").glob("*/sensorforge.ino.bin"))
    candidates += sorted(Path.home().glob(".cache/arduino/sketches/*/sensorforge.ino.bin"))
    chosen: dict[str, tuple[float, Path]] = {}
    for path in candidates:
        info = inspect(path)
        if not info or (want_release and info[1] != want_release):
            continue
        mtime = path.stat().st_mtime
        if info[0] not in chosen or mtime > chosen[info[0]][0]:
            chosen[info[0]] = (mtime, path)
    return [(board, path) for board, (_, path) in sorted(chosen.items())]


def lan_ip() -> str:
    """Address of the interface that routes into the LAN (no packet is sent)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 9))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


PIN_FILE = "pin.txt"


def read_pin(out: Path) -> str | None:
    try:
        pin = (out / PIN_FILE).read_text(encoding="ascii").strip()
    except OSError:
        return None
    return pin or None


def list_packages(out: Path, public_key) -> dict[str, list[str]]:
    """Valid packages per board, oldest first."""
    found: dict[str, list[str]] = {}
    for sfw in sorted((out / "firmware").glob("*/*.sfw")):
        try:
            info = signer.verify_package(sfw.read_bytes(), public_key)
        except Exception:  # noqa: BLE001
            continue
        if release_key(info["release"]):
            found.setdefault(info["board"], []).append(info["release"])
    for releases in found.values():
        releases.sort(key=release_key)
    return found


def write_manifest(out: Path, base_url: str, public_key, pin: str | None) -> list[dict]:
    """One entry per board: the newest package, or the pinned release only."""
    best: dict[str, dict] = {}
    for sfw in sorted((out / "firmware").glob("*/*.sfw")):
        try:
            info = signer.verify_package(sfw.read_bytes(), public_key)
        except Exception as exc:  # noqa: BLE001 - report and skip a broken file
            print(f"  übersprungen (passt nicht zum Schlüssel): {sfw}: {exc}")
            continue
        key = release_key(info["release"])
        if key is None:
            print(f"  übersprungen (Version nicht vergleichbar): {sfw}")
            continue
        if pin and info["release"] != pin:
            continue
        old = best.get(info["board"])
        if old is None or key > release_key(old["version"]):
            best[info["board"]] = {
                "board": info["board"],
                "channel": "stable" if key[1] else "beta",
                "version": info["release"],
                "url": f"{base_url}/firmware/{info['board']}/{sfw.name}",
                "size": sfw.stat().st_size,
                "sha256": hashlib.sha256(sfw.read_bytes()).hexdigest(),
            }
    releases = [best[b] for b in sorted(best)]
    tmp = out / "manifest.json.tmp"
    tmp.write_text(json.dumps({"schema": 1, "releases": releases}, indent=2) + "\n", encoding="utf-8")
    tmp.replace(out / "manifest.json")
    return releases


class Handler(http.server.SimpleHTTPRequestHandler):
    """Read-only: manifest.json and firmware/<board>/<file>.sfw, no listings."""

    ALLOWED = re.compile(r"^/(manifest\.json|firmware/[a-z]+/[A-Za-z0-9._+-]+\.sfw)$")

    def do_GET(self):  # noqa: N802
        if not self.ALLOWED.match(self.path.split("?", 1)[0]):
            self.send_error(404)
            return
        super().do_GET()

    def do_HEAD(self):  # noqa: N802
        self.do_GET()

    def list_directory(self, path):  # never list
        self.send_error(404)
        return None

    def guess_type(self, path):
        return "application/json" if str(path).endswith(".json") else "application/octet-stream"

    def log_message(self, fmt, *args):
        sys.stderr.write(f"[{self.log_date_time_string()}] {self.client_address[0]} {fmt % args}\n")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("images", nargs="*",
                    help="Build-Dateien (.bin); ohne Angabe wird der neueste Build je Board gesucht")
    ap.add_argument("--key", default=os.environ.get("SF_FIRMWARE_KEY", str(DEFAULT_KEY)),
                    help=f"privater Signierschlüssel (Standard: $SF_FIRMWARE_KEY oder {DEFAULT_KEY.name})")
    ap.add_argument("--out", default=str(DEFAULT_OUT),
                    help=f"Paket- und Server-Ordner (Standard: {DEFAULT_OUT})")
    ap.add_argument("--host", default=None,
                    help="Adresse in den URLs des Manifests (Standard: LAN-Adresse dieses PCs)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"HTTP-Port (Standard: {DEFAULT_PORT})")
    ap.add_argument("--any-release", action="store_true",
                    help="auch Builds nehmen, deren Version nicht sensorforge_version.h entspricht")
    ap.add_argument("--no-serve", action="store_true", help="nur signieren und Manifest schreiben, nicht ausliefern")
    ap.add_argument("--serve-only", action="store_true",
                    help="nichts neu signieren; Manifest neu schreiben und ausliefern")
    ap.add_argument("--pin", metavar="VERSION",
                    help="Manifest auf genau diese Version festlegen (gespeichert, gilt bis --unpin); "
                         "z. B. für ein Downgrade, siehe unten")
    ap.add_argument("--unpin", action="store_true", help="gespeicherten Pin aufheben: wieder neueste Version je Board")
    ap.add_argument("--list", action="store_true", help="vorhandene Pakete je Board und den Pin anzeigen, dann beenden")
    args = ap.parse_args()
    if args.pin and args.unpin:
        ap.error("--pin und --unpin schließen sich aus")
    if args.pin and not release_key(args.pin):
        ap.error(f"--pin {args.pin}: Version im Format vNN-betaNNN oder vNN erwartet")

    out = Path(args.out).resolve()
    (out / "firmware").mkdir(parents=True, exist_ok=True)
    key_path = Path(args.key)
    private_key = signer.load_private_key(key_path)
    public_key = private_key.public_key()

    if args.list:
        pin = read_pin(out)
        print(f"Pakete in {out / 'firmware'}:")
        for board, releases in sorted(list_packages(out, public_key).items()):
            print(f"  {board:9} {', '.join(releases)}")
        print(f"Pin: {pin} (Manifest nur mit dieser Version)" if pin else "Pin: keiner (Manifest = neueste Version je Board)")
        return 0

    if args.unpin:
        (out / PIN_FILE).unlink(missing_ok=True)
        print("Pin aufgehoben: Manifest enthält wieder die neueste Version je Board.")
    if args.pin:
        have = {b for b, r in list_packages(out, public_key).items() if args.pin in r}
        if not have:
            raise SystemExit(f"Kein Paket der Version {args.pin} im Ordner (--list zeigt die vorhandenen).")
        (out / PIN_FILE).write_text(args.pin + "\n", encoding="ascii")
        print(f"Pin gesetzt auf {args.pin} (Boards mit Paket: {', '.join(sorted(have))}).")
    pin = read_pin(out)

    if key_path.name.endswith("_DEV.pem"):
        print(f"Hinweis: signiert mit dem Entwicklungsschlüssel {key_path.name} (liegt im Repository).")
    host = args.host or lan_ip()
    base_url = f"http://{host}:{args.port}"

    if not args.serve_only:
        want = None if args.any_release else repo_release()
        images = collect_images(args.images, want)
        if not images:
            print(f"Keine SensorForge-Builds gefunden{f' für Version {want}' if want else ''}.")
            print("Erst bauen (Arduino IDE oder tools/flash_board.sh) oder .bin-Dateien angeben.")
            if not any((out / "firmware").glob("*/*.sfw")):
                return 1
        for board, path in images:
            release = inspect(path)[1]
            target = out / "firmware" / board / f"{release}.sfw"
            target.parent.mkdir(parents=True, exist_ok=True)
            image_sha = hashlib.sha256(path.read_bytes()).digest()
            if target.is_file() and target.read_bytes()[16:48] == image_sha:
                print(f"Gleich     {board:9} {release:14} schon als Paket vorhanden (Image unverändert)")
            else:
                info = signer.sign_file(path, private_key, target, board)
                print(f"Signiert   {board:9} {info['release']:14} {info['size']:>9} B  <- {path}")
            newer = [p.stem for p in target.parent.glob("*.sfw")
                     if release_key(p.stem) and release_key(release) and release_key(p.stem) > release_key(release)]
            if newer and not pin:
                print(f"           Hinweis: neuere Pakete für {board} vorhanden ({', '.join(sorted(newer))});"
                      f" im Manifest bleibt die neueste")

    releases = write_manifest(out, base_url, public_key, pin)
    print(f"\nManifest: {out / 'manifest.json'}")
    if pin:
        print(f"  *** PIN AKTIV: nur Version {pin} (aufheben mit --unpin) ***")
    for r in releases:
        print(f"  {r['board']:9} {r['version']:14} {r['size']:>9} B  {r['url']}")
    missing = [b for b in BOARDS if b not in {r['board'] for r in releases}]
    if missing:
        print(f"  (kein Paket für: {', '.join(missing)})")
    if args.no_serve:
        return 0

    print(f"\nManifest-URL für \"Firmware im Cluster\" am Coordinator:\n  {base_url}/manifest.json")
    print("Liefert nur Manifest und Pakete aus (Strg+C beendet). Abrufe der Geräte stehen unten.\n")
    handler = functools.partial(Handler, directory=str(out))
    with http.server.ThreadingHTTPServer(("0.0.0.0", args.port), handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nBeendet.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
