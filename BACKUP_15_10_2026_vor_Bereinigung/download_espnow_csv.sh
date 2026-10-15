
#!/usr/bin/env bash
set -euo pipefail

# SensorForge ESP-NOW Broadcast-Test
# Aufruf: bash download_espnow_csv.sh

USER_ID="ja1"
PASSWORD="sensor4337"

COORDINATOR="devfreenove1"
NODES=(devxiao1 devxiao2 devxiao3)

INTERVAL_MS=200
TEST_DURATION_S=15
WAIT_S=17

ENDPOINT="cluster_espnow_shared_events_csv"
TIMESTAMP="$(date +%Y-%m-%d_%H-%M-%S)"
OUTPUT_DIR="${1:-./reports/sensorforge_csv_${TIMESTAMP}}"

mkdir -p "$OUTPUT_DIR"
chmod 700 "$OUTPUT_DIR"

# Zugangsdaten für curl vorbereiten
CURL_CONFIG="$(mktemp)"
chmod 600 "$CURL_CONFIG"

cleanup() {
    rm -f "$CURL_CONFIG"
}
trap cleanup EXIT

ESCAPED_USER="${USER_ID//\\/\\\\}"
ESCAPED_USER="${ESCAPED_USER//\"/\\\"}"
ESCAPED_PASS="${PASSWORD//\\/\\\\}"
ESCAPED_PASS="${ESCAPED_PASS//\"/\\\"}"

printf 'user = "%s:%s"\n' \
    "$ESCAPED_USER" "$ESCAPED_PASS" > "$CURL_CONFIG"

unset PASSWORD ESCAPED_USER ESCAPED_PASS

echo "=================================="
echo " SensorForge ESP-NOW Broadcast-Test"
echo "=================================="
echo "Coordinator: $COORDINATOR"
echo "Intervall:   ${INTERVAL_MS} ms"
echo "Dauer:       ${TEST_DURATION_S} s"
echo "Ausgabe:     $OUTPUT_DIR"
echo

# sleep "$WAIT_S"

# CSV-Dateien herunterladen
echo
echo "Lade Messdaten herunter..."

SUCCESS=0

for NODE in "${NODES[@]}"; do
    TARGET="$OUTPUT_DIR/${NODE}_${TIMESTAMP}.csv"
    TEMP="${TARGET}.part"

    printf '%s: ' "$NODE"

    if HTTP_CODE="$(curl --config "$CURL_CONFIG" \
        --silent --show-error \
        --connect-timeout 5 --max-time 20 --retry 1 \
        --output "$TEMP" \
        --write-out '%{http_code}' \
        "http://${NODE}.local/${ENDPOINT}")"; then

        if [[ "$HTTP_CODE" == "200" && -s "$TEMP" ]] &&
           head -n 1 "$TEMP" | grep -Eqi 'seq|beacon|coordinator'; then

            mv "$TEMP" "$TARGET"

            BYTES="$(wc -c < "$TARGET" | tr -d ' ')"

            printf 'OK (%s Bytes)\n' "$BYTES"
            SUCCESS=$((SUCCESS + 1))
        else
            printf 'FEHLER (HTTP %s / CSV ungültig)\n' "$HTTP_CODE"
            rm -f "$TEMP"
        fi
    else
        printf 'FEHLER beim Download\n'
        rm -f "$TEMP"
    fi
done

echo
echo "=================================="
echo "Ergebnis: $SUCCESS von ${#NODES[@]} Dateien"
echo "Verzeichnis: $OUTPUT_DIR"
echo "=================================="

if (( SUCCESS != ${#NODES[@]} )); then
    exit 1
fi

echo "Alle Messdaten erfolgreich gespeichert."
