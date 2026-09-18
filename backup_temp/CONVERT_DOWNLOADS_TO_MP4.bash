#!/bin/bash

# Konfiguration
MIRROR=true       # true = horizontal spiegeln, false = nicht spiegeln
CRF=18
PRESET=medium
OUTDIR="mp4"

mkdir -p "$OUTDIR"

for f in *.avi; do
    base="${f%.avi}"
    srt="${base}.srt"

    if [ ! -f "$srt" ]; then
        echo "Keine Untertitel gefunden für: $f"
        continue
    fi

    echo "Konvertiere: $f"

    if [ "$MIRROR" = true ]; then
        FILTER="hflip,subtitles='$srt'"
    else
        FILTER="subtitles='$srt'"
    fi

    ffmpeg -n -i "$f" \
        -vf "$FILTER" \
        -c:v libx264 \
        -preset "$PRESET" \
        -crf "$CRF" \
        -pix_fmt yuv420p \
        -movflags +faststart \
        "$OUTDIR/${base}.mp4"
done

