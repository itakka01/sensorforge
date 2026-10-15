#!/usr/bin/env bash
set -u

OUT_SUFFIX="_whatsapp"
FPS=10
CRF=23
PRESET="medium"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "FEHLER: ffmpeg wurde nicht gefunden."
    exit 1
fi

if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -qE '^[[:space:]]*V.*libx264'; then
    echo "FEHLER: Deine ffmpeg-Version enthält keinen libx264 Encoder."
    echo "Benötigt wird H.264/libx264 für maximale WhatsApp-Kompatibilität."
    exit 1
fi

shopt -s nullglob
files=( *.avi )

if [ ${#files[@]} -eq 0 ]; then
    echo "Keine AVI-Dateien im aktuellen Verzeichnis gefunden."
    exit 0
fi

ok=0
skip=0
fail=0

echo "============================================================"
echo "SensorForge AVI -> WhatsApp MP4"
echo "Dateien : ${#files[@]}"
echo "Codec   : H.264 / libx264"
echo "FPS     : ${FPS}"
echo "CRF     : ${CRF}"
echo "============================================================"

for src in "${files[@]}"; do
    base="${src%.avi}"
    dst="${base}${OUT_SUFFIX}.mp4"

    echo
    echo "------------------------------------------------------------"
    echo "Quelle : ./$src"
    echo "Ziel   : ./$dst"

    if [ -s "$dst" ]; then
        echo "SKIP   : Zieldatei existiert bereits."
        ((skip++))
        continue
    fi

    rm -f -- "$dst"

    if ffmpeg -hide_banner -loglevel warning -stats \
        -i "$src" \
        -map 0:v:0 \
        -an \
        -vf "scale=trunc(iw/2)*2:trunc(ih/2)*2,fps=${FPS},format=yuv420p" \
        -c:v libx264 \
        -preset "$PRESET" \
        -crf "$CRF" \
        -profile:v baseline \
        -level 3.1 \
        -movflags +faststart \
        "$dst"
    then
        if [ -s "$dst" ]; then
            echo "OK     : $dst"
            ((ok++))
        else
            echo "FEHLER : ffmpeg meldete Erfolg, Zieldatei ist aber leer."
            rm -f -- "$dst"
            ((fail++))
        fi
    else
        echo "FEHLER : Konvertierung fehlgeschlagen."
        rm -f -- "$dst"
        ((fail++))
    fi
done

echo
echo "============================================================"
echo "Fertig: $ok erfolgreich, $skip uebersprungen, $fail fehlgeschlagen."
echo "============================================================"

if [ "$fail" -gt 0 ]; then
    exit 2
fi

