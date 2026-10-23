#!/usr/bin/env bash
# Installs the "SensorForge 3.75MB APP / 384KB SPIFFS" entry for
# Tools -> Partition Scheme into every installed arduino-esp32 core.
#
# Why: the sketch-folder partitions.csv is always flashed (it overrides the
# menu table), but the IDE checks the image size against the *menu* scheme.
# With "default_8MB" the IDE stops at 3,342,336 bytes although the devices
# have 3,932,160-byte OTA slots. This entry gives the IDE the real limit.
#
# Uses boards.local.txt (does not edit boards.txt) and is idempotent. Run it
# again after installing or updating the esp32 core. Changes no device and
# no partition layout: the generated table is byte-identical to partitions.csv.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_ROOT="${ARDUINO15:-$HOME/.arduino15}/packages/esp32/hardware/esp32"
CSV="$REPO/partitions.csv"
BEGIN="# >>> SensorForge partition scheme (tools/install_ide_partition_scheme.sh)"
END="# <<< SensorForge partition scheme"

[ -f "$CSV" ] || { echo "missing $CSV" >&2; exit 1; }
# Slot size from partitions.csv, so the IDE limit can never drift from it.
SLOT=""
for size in $(awk -F, '!/^[[:space:]]*#/ && $2 ~ /app/ {gsub(/[[:space:]]/,"",$5); print $5}' "$CSV"); do
    size=$((size))
    if [ -z "$SLOT" ] || [ "$size" -lt "$SLOT" ]; then SLOT=$size; fi
done
[ -n "$SLOT" ] || { echo "no app partition in $CSV" >&2; exit 1; }

found=0
for core in "$CORE_ROOT"/*/; do
    [ -f "$core/boards.txt" ] || continue
    found=1
    install -m 0644 "$CSV" "$core/tools/partitions/sensorforge.csv"
    local_txt="$core/boards.local.txt"
    touch "$local_txt"
    # Replace a previous block, keep everything else in boards.local.txt.
    awk -v b="$BEGIN" -v e="$END" '$0==b{skip=1} !skip{print} $0==e{skip=0}' "$local_txt" > "$local_txt.tmp"
    {
        cat "$local_txt.tmp"
        echo "$BEGIN"
        echo "esp32s3.menu.PartitionScheme.sensorforge=SensorForge 3.75MB APP / 384KB SPIFFS"
        echo "esp32s3.menu.PartitionScheme.sensorforge.build.partitions=sensorforge"
        echo "esp32s3.menu.PartitionScheme.sensorforge.upload.maximum_size=$SLOT"
        echo "$END"
    } > "$local_txt"
    rm -f "$local_txt.tmp"
    echo "installed in $core (maximum_size=$SLOT)"
done
[ "$found" = 1 ] || { echo "no esp32 core found under $CORE_ROOT" >&2; exit 1; }
echo "Restart the Arduino IDE, then select Tools -> Partition Scheme -> SensorForge 3.75MB APP / 384KB SPIFFS."
