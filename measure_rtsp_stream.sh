#!/usr/bin/env bash

URL="rtsp://devxiao1.local:554/stream"
DURATION="${1:-60}"

echo "Teste RTSP-Stream für ${DURATION} Sekunden..."
echo "URL: $URL"
echo

START=$(date +%s)

OUTPUT=$(ffmpeg \
  -hide_banner \
  -loglevel info \
  -rtsp_transport tcp \
  -i "$URL" \
  -t "$DURATION" \
  -an \
  -f null - 2>&1)

END=$(date +%s)
ELAPSED=$((END - START))

echo "$OUTPUT"
echo
echo "=============================="
echo "AUSWERTUNG"
echo "=============================="

LAST=$(echo "$OUTPUT" | grep -o 'frame=[^[:cntrl:]]*' | tail -n 1)

echo "Laufzeit: ${ELAPSED} s"
echo "Letzte ffmpeg-Statistik:"
echo "$LAST"

FRAMES=$(echo "$LAST" | sed -n 's/.*frame=[[:space:]]*\([0-9]*\).*/\1/p')

if [[ -n "$FRAMES" && "$ELAPSED" -gt 0 ]]; then
  AVG_FPS=$(awk "BEGIN {printf \"%.2f\", $FRAMES / $ELAPSED}")
  EXPECTED=$(awk "BEGIN {printf \"%.0f\", $DURATION * 4}")

  echo
  echo "Empfangene Frames: $FRAMES"
  echo "Durchschnittliche FPS: $AVG_FPS"
  echo "Erwartung bei 4 fps: ca. $EXPECTED Frames"
fi
