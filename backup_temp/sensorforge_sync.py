#!/usr/bin/env python3
"""SensorForge Sync API v1 client.

Downloads finalized AVI/MKV/SRT files from SensorForge. The SensorForge server
stays stateless; this client decides what is already present locally.
Interrupted files are kept as <name>.part and resumed with HTTP Range.

No third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import base64
import getpass
import http.client
import json
import os
from pathlib import Path
import socket
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


DEFAULT_BASE_URL = "http://192.168.4.1"
READ_CHUNK = 1024 * 1024
API_TIMEOUT_SECONDS = 10
DOWNLOAD_TIMEOUT_SECONDS = 30

CLIENT_VERSION_MAJOR = 1
CLIENT_VERSION_MINOR = 4

def _automatic_client_version() -> str:
    """Build-like client version derived from this script's file timestamp."""
    try:
        stamp = Path(__file__).resolve().stat().st_mtime
        tm = time.localtime(stamp)
        build = time.strftime("%Y%m%d%H%M%S", tm)
    except OSError:
        build = "unknown"
    return f"{CLIENT_VERSION_MAJOR}.{CLIENT_VERSION_MINOR}.{build}"


CLIENT_VERSION = _automatic_client_version()


class SensorForgeError(RuntimeError):
    pass


class SensorForgeBusy(SensorForgeError):
    pass


class SensorForgeClient:
    def __init__(self, base_url: str, username: str, password: str) -> None:
        self.base_url = base_url.rstrip("/")
        token = base64.b64encode(f"{username}:{password}".encode("utf-8")).decode("ascii")
        self.auth_header = f"Basic {token}"

    def _request(
        self,
        path: str,
        *,
        headers: dict[str, str] | None = None,
        timeout: int = API_TIMEOUT_SECONDS,
        method: str = "GET",
    ):
        request_headers = {
            "Authorization": self.auth_header,
            "Accept": "application/json",
            "User-Agent": f"SensorForge-Python-Sync/{CLIENT_VERSION}",
            "X-SensorForge-Client-Version": CLIENT_VERSION,
            "Connection": "close",
        }
        if headers:
            request_headers.update(headers)

        req = Request(self.base_url + path, headers=request_headers, data=(b"" if method == "POST" else None), method=method)
        return urlopen(req, timeout=timeout)

    def request_json(self, path: str, *, method: str = "GET") -> dict[str, Any]:
        try:
            with self._request(path, method=method) as response:
                raw = response.read()
                return json.loads(raw.decode("utf-8"))
        except HTTPError as exc:
            if exc.code == 401:
                raise SensorForgeError("Anmeldung abgelehnt (HTTP 401).") from exc
            if exc.code == 409:
                raise SensorForgeBusy("SensorForge ist momentan mit Recording/Storage beschäftigt.") from exc
            body = exc.read().decode("utf-8", errors="replace")
            raise SensorForgeError(f"API HTTP {exc.code}: {body}") from exc
        except (URLError, socket.timeout, ConnectionError) as exc:
            raise SensorForgeError(f"SensorForge nicht erreichbar: {exc}") from exc
        except json.JSONDecodeError as exc:
            raise SensorForgeError("Ungültige JSON-Antwort von SensorForge.") from exc

    def get_json(self, path: str) -> dict[str, Any]:
        return self.request_json(path, method="GET")

    def status(self) -> dict[str, Any]:
        return self.get_json("/api/v1/status")

    def exclusive(self, enable: bool) -> dict[str, Any]:
        query = urlencode({"enable": "1" if enable else "0"})
        return self.request_json(f"/api/v1/exclusive?{query}", method="POST")

    def exclusive_status(self) -> dict[str, Any]:
        return self.get_json("/api/v1/exclusive")

    def test_sd_read(self, remote_path: str, byte_count: int) -> dict[str, Any]:
        query = urlencode({"path": remote_path, "bytes": str(byte_count)})
        return self.get_json(f"/api/v1/test_sd_read?{query}")

    def test_sd_spi(self, remote_path: str, byte_count: int, mhz: int) -> dict[str, Any]:
        query = urlencode({"path": remote_path, "bytes": str(byte_count), "mhz": str(mhz)})
        return self.get_json(f"/api/v1/test_sd_spi?{query}")

    def test_wifi(self, byte_count: int) -> tuple[int, float]:
        query = urlencode({"bytes": str(byte_count)})
        headers = {"Accept": "application/octet-stream"}
        started = time.monotonic()
        received = 0

        try:
            with self._request(
                f"/api/v1/test_wifi?{query}",
                headers=headers,
                timeout=DOWNLOAD_TIMEOUT_SECONDS,
            ) as response:
                while True:
                    block = response.read(READ_CHUNK)
                    if not block:
                        break
                    received += len(block)
        except HTTPError as exc:
            if exc.code == 401:
                raise SensorForgeError("Anmeldung abgelehnt (HTTP 401).") from exc
            if exc.code == 409:
                raise SensorForgeBusy("WLAN-Test wegen Recording vorübergehend blockiert.") from exc
            body = exc.read().decode("utf-8", errors="replace")
            raise SensorForgeError(f"WLAN-Test HTTP {exc.code}: {body}") from exc
        except (URLError, socket.timeout, ConnectionError, OSError, http.client.HTTPException) as exc:
            raise SensorForgeError(f"WLAN-Test abgebrochen: {exc}") from exc

        elapsed = max(time.monotonic() - started, 0.000001)
        return received, elapsed

    def days(self) -> list[str]:
        data = self.get_json("/api/v1/days")
        days = data.get("days", [])
        if not isinstance(days, list):
            raise SensorForgeError("Ungültige days-Antwort.")
        return [str(day) for day in days]

    def files(self, day: str) -> list[dict[str, Any]]:
        query = urlencode({"day": day})
        data = self.get_json(f"/api/v1/files?{query}")
        files = data.get("files", [])
        if not isinstance(files, list):
            raise SensorForgeError("Ungültige files-Antwort.")
        return files

    def open_file(self, remote_path: str, offset: int):
        query = urlencode({"path": remote_path})
        headers = {
            "Accept": "application/octet-stream",
        }
        if offset > 0:
            headers["Range"] = f"bytes={offset}-"

        try:
            return self._request(
                f"/api/v1/file?{query}",
                headers=headers,
                timeout=DOWNLOAD_TIMEOUT_SECONDS,
            )
        except HTTPError as exc:
            if exc.code == 401:
                raise SensorForgeError("Anmeldung abgelehnt (HTTP 401).") from exc
            if exc.code == 409:
                raise SensorForgeBusy("Download wegen Recording/Storage vorübergehend blockiert.") from exc
            if exc.code == 416:
                raise SensorForgeError("Server hat den Resume-Offset abgelehnt (HTTP 416).") from exc
            body = exc.read().decode("utf-8", errors="replace")
            raise SensorForgeError(f"Download HTTP {exc.code}: {body}") from exc
        except (URLError, socket.timeout, ConnectionError) as exc:
            raise SensorForgeError(f"Download-Verbindung fehlgeschlagen: {exc}") from exc


def safe_server_name(name: str) -> str:
    if not name or Path(name).name != name or name in {".", ".."}:
        raise SensorForgeError(f"Unsicherer Dateiname vom Server: {name!r}")
    return name


def safe_day(day: str) -> str:
    if day == "fallback":
        return day
    if len(day) == 8 and day.isdigit():
        return day
    raise SensorForgeError(f"Ungültiger Tag vom Server: {day!r}")


def safe_device_id(device_id: str) -> str:
    value = device_id.strip()
    if not value:
        raise SensorForgeError("API liefert keine device_id; SensorForge-Firmware aktualisieren.")

    # cfg_hostname is the canonical device ID. Keep normal hostname characters
    # unchanged and map any filesystem-hostile/legacy characters deterministically.
    safe = "".join(c if (c.isalnum() or c in "-_.") else "_" for c in value)
    safe = safe.strip(" .")
    if not safe or safe in {".", ".."}:
        raise SensorForgeError(f"Ungültige device_id vom Server: {device_id!r}")
    return safe


def local_day_name(day: str) -> str:
    day = safe_day(day)
    if day == "fallback":
        return day
    return f"{day[6:8]}_{day[4:6]}_{day[0:4]}"


def wait_for_json(call, retries: int, retry_seconds: float):
    failures = 0
    while True:
        try:
            return call()
        except SensorForgeBusy:
            # Recording/storage busy is an expected operational state, not a
            # failed API attempt. Wait until SensorForge becomes available.
            time.sleep(retry_seconds)
            continue
        except SensorForgeError:
            if failures >= retries:
                raise
            failures += 1
            time.sleep(retry_seconds)


def download_one(
    client: SensorForgeClient,
    entry: dict[str, Any],
    local_day_dir: Path,
    retries: int,
    retry_seconds: float,
) -> tuple[bool, int, float]:
    name = safe_server_name(str(entry.get("name", "")))
    remote_path = str(entry.get("path", ""))
    remote_size = int(entry.get("size", -1))

    if remote_size < 0 or not remote_path.startswith("/"):
        raise SensorForgeError(f"Ungültiger Manifest-Eintrag: {entry!r}")

    final_path = local_day_dir / name
    part_path = local_day_dir / f"{name}.part"

    if final_path.exists() and final_path.stat().st_size == remote_size:
        return False, 0, 0.0

    local_day_dir.mkdir(parents=True, exist_ok=True)

    if part_path.exists() and part_path.stat().st_size > remote_size:
        part_path.unlink()

    started = time.monotonic()
    starting_size = part_path.stat().st_size if part_path.exists() else 0
    failures_without_progress = 0
    last_busy_notice = 0.0

    while True:
        offset = part_path.stat().st_size if part_path.exists() else 0

        if offset == remote_size:
            os.replace(part_path, final_path)
            elapsed = max(time.monotonic() - started, 0.000001)
            return True, remote_size - starting_size, elapsed

        try:
            response = client.open_file(remote_path, offset)
        except SensorForgeBusy:
            now = time.monotonic()
            if last_busy_notice == 0.0 or now - last_busy_notice >= 10.0:
                print(f"    Recording/Storage aktiv – Resume bei {offset:,} Byte folgt ...")
                last_busy_notice = now
            time.sleep(retry_seconds)
            continue
        except SensorForgeError as exc:
            failures_without_progress += 1
            if failures_without_progress > retries:
                raise
            print(f"    Verbindung unterbrochen – Resume bei {offset:,} Byte folgt ...")
            time.sleep(retry_seconds)
            continue

        before_transfer = offset

        try:
            status = getattr(response, "status", response.getcode())

            if offset > 0 and status == 200:
                # A server that ignored Range must never be appended to an old .part.
                response.close()
                part_path.unlink(missing_ok=True)
                failures_without_progress += 1
                if failures_without_progress > retries:
                    raise SensorForgeError("Server ignoriert HTTP Range.")
                continue

            if offset > 0 and status != 206:
                response.close()
                raise SensorForgeError(f"Unerwarteter Resume-Status HTTP {status}.")

            mode = "ab" if offset > 0 else "wb"
            with response, open(part_path, mode) as out:
                while True:
                    try:
                        block = response.read(READ_CHUNK)
                    except http.client.IncompleteRead as exc:
                        if exc.partial:
                            out.write(exc.partial)
                        break
                    if not block:
                        break
                    out.write(block)

        except (socket.timeout, URLError, ConnectionError, OSError, http.client.HTTPException) as exc:
            current_size = part_path.stat().st_size if part_path.exists() else 0
            if current_size <= before_transfer:
                failures_without_progress += 1
                if failures_without_progress > retries:
                    raise SensorForgeError(f"Download abgebrochen: {exc}") from exc

        current_size = part_path.stat().st_size if part_path.exists() else 0

        if current_size == remote_size:
            os.replace(part_path, final_path)
            elapsed = max(time.monotonic() - started, 0.000001)
            return True, remote_size - starting_size, elapsed

        if current_size > remote_size:
            part_path.unlink(missing_ok=True)
            raise SensorForgeError(f"Lokale Teildatei ist größer als Remote-Datei: {name}")

        if current_size > before_transfer:
            # Any real progress resets the transport failure budget. Recording
            # may preempt the same large file arbitrarily often without making
            # the sync fail permanently.
            failures_without_progress = 0
        else:
            failures_without_progress += 1
            if failures_without_progress > retries:
                raise SensorForgeError(f"Download ohne Fortschritt: {name}")

        print(f"    Transfer unterbrochen – {current_size:,}/{remote_size:,} Byte, Resume ...")
        time.sleep(retry_seconds)


def sync_once(
    client: SensorForgeClient,
    destination: Path,
    retries: int,
    retry_seconds: float,
) -> None:
    status = wait_for_json(client.status, retries, retry_seconds)
    raw_device_id = str(status.get("device_id", ""))
    device_id = safe_device_id(raw_device_id)
    device_destination = destination / device_id

    print(
        f"Client {CLIENT_VERSION} | Host API {status.get('api_version', '?')} | "
        f"SensorForge {status.get('core_version', '?')} | "
        f"Device={raw_device_id} | "
        f"Recording={'ja' if status.get('recording') else 'nein'} | "
        f"Sync={'bereit' if status.get('sync_available') else 'belegt'} | "
        f"Exclusive={'ja' if status.get('exclusive_active') else 'nein'}"
    )

    days = wait_for_json(client.days, retries, retry_seconds)
    if not days:
        print("Keine Aufnahmen vorhanden.")
        return

    total_new_bytes = 0
    total_elapsed = 0.0
    downloaded_count = 0
    skipped_count = 0

    # Oldest day first; this makes a first full sync deterministic.
    normal_days = sorted([safe_day(day) for day in days if day != "fallback"])
    if "fallback" in days:
        normal_days.append("fallback")

    for day in normal_days:
        entries = wait_for_json(lambda d=day: client.files(d), retries, retry_seconds)
        if not entries:
            continue

        local_folder = local_day_name(day)
        print(f"{day} -> {local_folder}: {len(entries)} Datei(en)")
        local_day_dir = device_destination / local_folder

        for entry in entries:
            name = safe_server_name(str(entry.get("name", "")))
            remote_size = int(entry.get("size", 0))
            final_path = local_day_dir / name

            if final_path.exists() and final_path.stat().st_size == remote_size:
                skipped_count += 1
                continue

            print(f"  -> {name} ({remote_size / (1024 * 1024):.2f} MB)")
            changed, new_bytes, elapsed = download_one(
                client,
                entry,
                local_day_dir,
                retries,
                retry_seconds,
            )

            if changed:
                downloaded_count += 1
                total_new_bytes += new_bytes
                total_elapsed += elapsed
                speed = (new_bytes / (1024 * 1024)) / elapsed if elapsed > 0 else 0.0
                print(f"     fertig | {speed:.2f} MB/s")
            else:
                skipped_count += 1

    aggregate_speed = (
        (total_new_bytes / (1024 * 1024)) / total_elapsed
        if total_elapsed > 0
        else 0.0
    )

    print(
        f"Sync fertig: {downloaded_count} neu/fortgesetzt, "
        f"{skipped_count} bereits vorhanden, "
        f"{total_new_bytes / (1024 * 1024):.2f} MB übertragen"
        + (f", Ø {aggregate_speed:.2f} MB/s" if total_elapsed > 0 else "")
    )


def select_test_video(
    client: SensorForgeClient,
    retries: int,
    retry_seconds: float,
    minimum_bytes: int,
) -> dict[str, Any]:
    days = wait_for_json(client.days, retries, retry_seconds)
    largest_video: dict[str, Any] | None = None

    ordered_days = sorted([safe_day(day) for day in days if day != "fallback"], reverse=True)
    if "fallback" in days:
        ordered_days.append("fallback")

    for day in ordered_days:
        entries = wait_for_json(lambda d=day: client.files(d), retries, retry_seconds)
        for entry in entries:
            name = str(entry.get("name", "")).lower()
            if not (name.endswith(".avi") or name.endswith(".mkv")):
                continue

            size = int(entry.get("size", 0))
            if largest_video is None or size > int(largest_video.get("size", 0)):
                largest_video = entry

            if size >= minimum_bytes:
                return entry

    if largest_video is None:
        raise SensorForgeError("Kein AVI/MKV für den SD-Lesetest vorhanden.")

    return largest_video


def run_benchmarks(
    client: SensorForgeClient,
    retries: int,
    retry_seconds: float,
    test_bytes: int,
) -> None:
    status = wait_for_json(client.status, retries, retry_seconds)
    print(
        f"Client {CLIENT_VERSION} | Host API {status.get('api_version', '?')} | "
        f"SensorForge {status.get('core_version', '?')}"
    )

    selected_video = select_test_video(
        client,
        retries,
        retry_seconds,
        test_bytes,
    )

    remote_path = str(selected_video.get("path", ""))
    remote_size = int(selected_video.get("size", 0))
    sd_bytes = min(max(test_bytes, 1), max(remote_size, 1))

    print(f"SD-Test: {remote_path} | {sd_bytes / (1024 * 1024):.2f} MB")
    sd = wait_for_json(
        lambda: client.test_sd_read(remote_path, sd_bytes),
        retries,
        retry_seconds,
    )
    print(
        f"  SD -> RAM: {float(sd.get('mb_per_s', 0.0)):.3f} MB/s "
        f"({int(sd.get('bytes_read', 0)):,} Byte, {int(sd.get('elapsed_ms', 0))} ms)"
    )

    print(f"WLAN-Test: {test_bytes / (1024 * 1024):.2f} MB RAM -> TCP")
    failures = 0
    while True:
        try:
            received, elapsed = client.test_wifi(test_bytes)
            speed = (received / (1024 * 1024)) / elapsed if elapsed > 0 else 0.0
            print(f"  RAM -> WLAN: {speed:.3f} MB/s ({received:,} Byte, {elapsed:.3f} s)")
            if received != test_bytes:
                print(f"  WARNUNG: erwartet {test_bytes:,} Byte, empfangen {received:,} Byte")
            return
        except SensorForgeBusy:
            time.sleep(retry_seconds)
            continue
        except SensorForgeError:
            if failures >= retries:
                raise
            failures += 1
            time.sleep(retry_seconds)


def run_spi_sweep(
    client: SensorForgeClient,
    retries: int,
    retry_seconds: float,
    test_bytes: int,
    frequencies_mhz: list[int],
) -> None:
    status = wait_for_json(client.status, retries, retry_seconds)
    print(
        f"Client {CLIENT_VERSION} | Host API {status.get('api_version', '?')} | "
        f"SensorForge {status.get('core_version', '?')}"
    )

    selected_video = select_test_video(
        client,
        retries,
        retry_seconds,
        test_bytes,
    )
    remote_path = str(selected_video.get("path", ""))
    remote_size = int(selected_video.get("size", 0))
    sd_bytes = min(max(test_bytes, 1), max(remote_size, 1))

    print(f"SD-SPI Sweep: {remote_path} | {sd_bytes / (1024 * 1024):.2f} MB je Takt")

    for mhz in frequencies_mhz:
        result = wait_for_json(
            lambda f=mhz: client.test_sd_spi(remote_path, sd_bytes, f),
            retries,
            retry_seconds,
        )

        mount_ok = bool(result.get("mount_ok"))
        read_ok = bool(result.get("read_ok"))
        restore_ok = bool(result.get("restore_ok"))
        speed = float(result.get("mb_per_s", 0.0))
        elapsed_ms = int(result.get("elapsed_ms", 0))

        state = "OK" if mount_ok and read_ok and restore_ok else "FEHLER"
        print(
            f"  {mhz:>2} MHz -> {speed:.3f} MB/s | {elapsed_ms} ms | {state} "
            f"| restore={'OK' if restore_ok else 'FEHLER'}"
        )

        if bool(result.get("reboot_required")):
            raise SensorForgeError(
                "SD konnte nach dem SPI-Test nicht auf den normalen Takt zurückgesetzt werden; "
                "SensorForge bitte neu starten."
            )


def parse_spi_frequencies(text: str) -> list[int]:
    allowed = {4, 8, 12, 16, 20, 24, 32, 40}
    values: list[int] = []

    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            value = int(part)
        except ValueError as exc:
            raise SensorForgeError(f"Ungültige SPI-Frequenz: {part!r}") from exc
        if value not in allowed:
            raise SensorForgeError("SPI-Test erlaubt nur 4,8,12,16,20,24,32,40 MHz.")
        if value not in values:
            values.append(value)

    if not values:
        raise SensorForgeError("Keine SPI-Testfrequenz angegeben.")

    return values


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=f"SensorForge Sync API v1 Client {CLIENT_VERSION}")
    parser.add_argument("--host", default=DEFAULT_BASE_URL, help=f"SensorForge Basis-URL (Default: {DEFAULT_BASE_URL})")
    parser.add_argument("--user", default=os.environ.get("SENSORFORGE_USER"), help="Web/API Benutzername")
    parser.add_argument("--password", default=os.environ.get("SENSORFORGE_PASSWORD"), help="Web/API Passwort (besser weglassen und Prompt verwenden)")
    parser.add_argument("--dest", default="sensorforge_downloads", help="Lokales Zielverzeichnis")
    parser.add_argument("--retries", type=int, default=20, help="Wiederholungen bei echten Verbindungsfehlern ohne Fortschritt")
    parser.add_argument("--retry-seconds", type=float, default=2.0, help="Pause zwischen Wiederholungen")
    parser.add_argument("--watch", type=float, default=0.0, metavar="SECONDS", help=">0: dauerhaft laufen und alle N Sekunden erneut synchronisieren")
    parser.add_argument("--exclusive", action="store_true", help="Während Sync/Benchmark neue Aufnahmen per API-Lease blockieren")
    parser.add_argument("--exclusive-control", choices=["on", "off", "status"], help="Exclusive-Lease nur schalten/anzeigen und danach beenden")
    parser.add_argument("--benchmark", action="store_true", help="SD- und WLAN-Durchsatztests ausführen")
    parser.add_argument("--spi-sweep", action="store_true", help="SD-SPI-Laufzeittest durchführen; Exclusive wird automatisch aktiviert")
    parser.add_argument("--spi-frequencies", default="4,8,12,16,20,24,32,40", help="SPI-Testfrequenzen in MHz, kommagetrennt (Default: 4,8,12,16,20,24,32,40)")
    parser.add_argument("--test-mb", type=int, default=1, help="Testgröße für Benchmarks in MiB (Default: 1, Server-Maximum 64)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    username = args.user or input("SensorForge Benutzer: ").strip()
    if not username:
        print("Benutzername fehlt.", file=sys.stderr)
        return 2

    password = args.password
    if password is None:
        password = getpass.getpass("SensorForge Passwort: ")

    client = SensorForgeClient(args.host, username, password)
    destination = Path(args.dest).expanduser().resolve()
    retries = max(args.retries, 0)
    retry_seconds = max(args.retry_seconds, 0.1)
    test_bytes = min(max(args.test_mb, 1), 64) * 1024 * 1024
    try:
        spi_frequencies = parse_spi_frequencies(args.spi_frequencies)
    except SensorForgeError as exc:
        print(f"FEHLER: {exc}", file=sys.stderr)
        return 2

    if args.exclusive_control:
        try:
            if args.exclusive_control == "status":
                state = client.exclusive_status()
            else:
                state = client.exclusive(args.exclusive_control == "on")
            print(
                f"Client {CLIENT_VERSION} | Host API {state.get('api_version', '?')} | "
                f"Exclusive={'AN' if state.get('exclusive_active') else 'AUS'} | "
                f"Rest={int(state.get('remaining_seconds', 0))} s | "
                f"Lease={int(state.get('lease_seconds', 60))} s"
            )
            return 0
        except SensorForgeError as exc:
            print(f"FEHLER: {exc}", file=sys.stderr)
            return 1

    if not args.benchmark and not args.spi_sweep:
        print(f"Ziel: {destination}")

    def run_one_operation() -> None:
        lease_enabled = False
        try:
            needs_exclusive = args.exclusive or args.spi_sweep
            if needs_exclusive:
                state = wait_for_json(lambda: client.exclusive(True), retries, retry_seconds)
                lease_enabled = bool(state.get("exclusive_active"))
                print(f"API Exclusive aktiv | Lease {int(state.get('lease_seconds', 60))} s")

            ran_test = False
            if args.spi_sweep:
                run_spi_sweep(
                    client,
                    retries,
                    retry_seconds,
                    test_bytes,
                    spi_frequencies,
                )
                ran_test = True

            if args.benchmark:
                run_benchmarks(client, retries, retry_seconds, test_bytes)
                ran_test = True

            if not ran_test:
                sync_once(client, destination, retries, retry_seconds)
        finally:
            if lease_enabled:
                try:
                    client.exclusive(False)
                    print("API Exclusive freigegeben")
                except SensorForgeError as exc:
                    print(f"WARNUNG: Exclusive konnte nicht explizit freigegeben werden: {exc}", file=sys.stderr)
                    print("Die SensorForge-Lease läuft automatisch nach spätestens 60 s aus.", file=sys.stderr)

    if args.watch <= 0 or args.benchmark or args.spi_sweep:
        try:
            run_one_operation()
            return 0
        except KeyboardInterrupt:
            return 130
        except SensorForgeError as exc:
            print(f"FEHLER: {exc}", file=sys.stderr)
            return 1

    while True:
        try:
            run_one_operation()
        except KeyboardInterrupt:
            return 130
        except SensorForgeError as exc:
            print(f"Sync momentan nicht möglich: {exc}")

        time.sleep(max(args.watch, 1.0))


if __name__ == "__main__":
    raise SystemExit(main())


