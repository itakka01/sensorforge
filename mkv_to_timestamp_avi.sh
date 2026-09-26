#!/usr/bin/env bash
set -Eeuo pipefail

# SensorForge Sparse-MKV -> compact AVI with burned-in real capture timestamps.
#
# Requirements:
#   ffmpeg, ffprobe, python3
#   ffmpeg must include the "subtitles" filter (libass).
#
# Intended for SensorForge continuous-shooter Sparse MKVs.
# The source MKV's Matroska DateUTC/creation_time plus each video's real frame PTS
# are used to reconstruct the wall-clock capture time of every stored frame.
#
# Output playback is intentionally compact: every stored source frame appears once
# at VIEW_FPS (default 4 fps). Long rejected-frame gaps are therefore NOT expanded
# into long still-image sections. The burned-in timestamp always shows the real
# original capture time.
#
# Optional environment variables:
#   TZ_NAME=Europe/Vienna   timezone used for the burned-in wall-clock time
#   VIEW_FPS=4              compact output playback rate
#   MJPEG_QUALITY=3         ffmpeg MJPEG q:v (lower = higher quality, 2..5 useful)
#
# Example:
#   chmod +x sensorforge_mkv_to_timestamp_avi.sh
#   ./sensorforge_mkv_to_timestamp_avi.sh
#
# Override example:
#   TZ_NAME=Europe/Vienna VIEW_FPS=6 ./sensorforge_mkv_to_timestamp_avi.sh

TZ_NAME="${TZ_NAME:-Europe/Vienna}"
VIEW_FPS="${VIEW_FPS:-4}"
MJPEG_QUALITY="${MJPEG_QUALITY:-3}"

for cmd in ffmpeg ffprobe python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "FEHLER: '$cmd' wurde nicht gefunden." >&2
        exit 1
    fi
done

ffmpeg_filters="$(ffmpeg -hide_banner -filters 2>/dev/null)"
if ! grep -qE '[[:space:]]subtitles[[:space:]]' <<<"$ffmpeg_filters"; then
    echo "FEHLER: Diese ffmpeg-Version besitzt keinen subtitles/libass-Filter." >&2
    exit 1
fi

# ffmpeg compatibility: -fps_mode exists only in newer releases. Older
# Ubuntu/Debian builds use -vsync. Detect this once at startup.
ffmpeg_help_full="$(ffmpeg -hide_banner -h full 2>/dev/null || true)"
if grep -q -- '-fps_mode' <<<"$ffmpeg_help_full"; then
    sync_args=(-fps_mode cfr)
    sync_label="-fps_mode cfr"
else
    sync_args=(-vsync cfr)
    sync_label="-vsync cfr (legacy ffmpeg)"
fi

shopt -s nullglob nocaseglob
files=(./*.mkv)
shopt -u nocaseglob

if ((${#files[@]} == 0)); then
    echo "Keine MKV-Dateien im aktuellen Verzeichnis gefunden."
    exit 0
fi

echo "SensorForge MKV -> AVI"
echo "Zeitzone : $TZ_NAME"
echo "AVI-FPS  : $VIEW_FPS"
echo "FFmpeg   : $sync_label"
echo

ok=0
failed=0
skipped=0

for input in "${files[@]}"; do
    base="${input%.*}"
    output="${base}_timestamp.avi"

    echo "------------------------------------------------------------"
    echo "Quelle : $input"
    echo "Ziel   : $output"

    if [[ -e "$output" ]]; then
        echo "SKIP   : Zieldatei existiert bereits."
        ((skipped+=1))
        continue
    fi

    tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/sensorforge_mkv2avi.XXXXXX")"
    meta="$tmpdir/meta.json"
    srt="$tmpdir/timestamps.srt"

    cleanup_one() {
        rm -rf "$tmpdir"
    }

    if ! ffprobe \
        -v error \
        -select_streams v:0 \
        -show_entries 'format_tags=creation_time:frame=best_effort_timestamp_time,pts_time,pkt_dts_time' \
        -of json \
        "$input" > "$meta"; then
        echo "FEHLER : ffprobe konnte die MKV-Datei nicht lesen."
        echo "         Falls sie direkt von einer verschluesselten SD kopiert wurde,"
        echo "         zuerst ueber SensorForge transparent entschluesselt herunterladen."
        cleanup_one
        ((failed+=1))
        continue
    fi

    if ! python3 - "$meta" "$srt" "$TZ_NAME" <<'PY'
import json
import sys
from datetime import datetime, timezone, timedelta

try:
    from zoneinfo import ZoneInfo
except ImportError:
    print("FEHLER: Python zoneinfo fehlt (Python >= 3.9 erforderlich).", file=sys.stderr)
    raise SystemExit(2)

meta_path, srt_path, tz_name = sys.argv[1:4]

with open(meta_path, "r", encoding="utf-8") as fh:
    data = json.load(fh)

tags = (data.get("format") or {}).get("tags") or {}
creation = tags.get("creation_time") or tags.get("CREATION_TIME")

if not creation:
    print("FEHLER: Keine Matroska DateUTC/creation_time in der MKV gefunden.", file=sys.stderr)
    raise SystemExit(3)

creation = creation.strip()
if creation.endswith("Z"):
    creation = creation[:-1] + "+00:00"

try:
    base = datetime.fromisoformat(creation)
except ValueError:
    print(f"FEHLER: creation_time nicht lesbar: {creation}", file=sys.stderr)
    raise SystemExit(4)

if base.tzinfo is None:
    base = base.replace(tzinfo=timezone.utc)

try:
    local_tz = ZoneInfo(tz_name)
except Exception:
    print(f"FEHLER: Unbekannte Zeitzone: {tz_name}", file=sys.stderr)
    raise SystemExit(5)

pts = []
for frame in data.get("frames") or []:
    value = None
    for key in ("best_effort_timestamp_time", "pts_time", "pkt_dts_time"):
        raw = frame.get(key)
        if raw not in (None, "N/A", ""):
            try:
                value = float(raw)
                break
            except (TypeError, ValueError):
                pass
    if value is not None and value >= 0:
        pts.append(value)

if not pts:
    print("FEHLER: Keine Video-Frame-Zeitstempel gefunden.", file=sys.stderr)
    raise SystemExit(6)

# Keep ffprobe order. SensorForge Sparse MKVs are monotonic; reject obviously
# broken/non-monotonic metadata rather than silently inventing a timeline.
for a, b in zip(pts, pts[1:]):
    if b < a:
        print("FEHLER: Nicht-monotone Frame-Zeitstempel gefunden.", file=sys.stderr)
        raise SystemExit(7)


def srt_time(seconds: float) -> str:
    if seconds < 0:
        seconds = 0.0
    total_ms = int(round(seconds * 1000.0))
    hours, rem = divmod(total_ms, 3_600_000)
    minutes, rem = divmod(rem, 60_000)
    secs, millis = divmod(rem, 1000)
    return f"{hours:02d}:{minutes:02d}:{secs:02d},{millis:03d}"


with open(srt_path, "w", encoding="utf-8", newline="\n") as out:
    for i, p in enumerate(pts):
        # Keep the text active until immediately before the next real source frame.
        # There are no decoded frames inside SensorForge's sparse gaps anyway.
        if i + 1 < len(pts):
            end = pts[i + 1] - 0.001
        else:
            end = p + 1.0
        if end <= p:
            end = p + 0.100

        wall = (base + timedelta(seconds=p)).astimezone(local_tz)
        tenth = wall.microsecond // 100_000
        text = wall.strftime("%Y-%m-%d %H:%M:%S") + f".{tenth}"

        out.write(f"{i + 1}\n")
        out.write(f"{srt_time(p)} --> {srt_time(end)}\n")
        out.write(text + "\n\n")

print(f"{len(pts)} Frames | DateUTC={base.isoformat()} | TZ={tz_name}")
PY
    then
        echo "FEHLER : Zeitinformationen konnten nicht rekonstruiert werden."
        cleanup_one
        ((failed+=1))
        continue
    fi

    # The subtitle overlay is applied while the ORIGINAL sparse PTS are still
    # present. Only afterwards do we compact the frames to a regular CFR AVI.
    # Therefore the burned-in text retains the true capture time even though the
    # viewing AVI no longer contains the long sparse gaps.
    filter="subtitles=$srt:force_style='Alignment=3,MarginR=18,MarginV=18,FontSize=22,Outline=2,Shadow=0',setpts=N/(${VIEW_FPS}*TB)"

    if ffmpeg \
        -hide_banner \
        -loglevel warning \
        -stats \
        -n \
        -i "$input" \
        -an \
        -vf "$filter" \
        -r "$VIEW_FPS" \
        "${sync_args[@]}" \
        -c:v mjpeg \
        -q:v "$MJPEG_QUALITY" \
        -pix_fmt yuvj420p \
        -metadata comment="SensorForge review AVI; real capture timestamp burned into image" \
        "$output"; then
        echo "OK     : $output"
        ((ok+=1))
    else
        echo "FEHLER : ffmpeg-Konvertierung fehlgeschlagen."
        rm -f "$output"
        ((failed+=1))
    fi

    cleanup_one
done

echo
echo "============================================================"
echo "Fertig: $ok erfolgreich, $skipped uebersprungen, $failed fehlgeschlagen."

if ((failed > 0)); then
    exit 2
fi


