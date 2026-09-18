#!/bin/bash

MAX_BYTES=$((512 * 1024 * 1024 * 1024))
LABEL="sensordisk"

echo
echo "=== Mögliche SD-/USB-Datenträger bis 512 GiB ==="
echo

# Nur:
# - keine loop-Devices
# - maximal 512 GiB
# - removable ODER USB/MMC
mapfile -t ALLOWED_DEVICES < <(
    lsblk -b -d -e 7 -n -o NAME,SIZE,RM,TRAN | \
    awk -v max="$MAX_BYTES" '
        $2 <= max && ($3 == 1 || $4 == "usb" || $4 == "mmc") {
            print "/dev/" $1
        }
    '
)

if [ ${#ALLOWED_DEVICES[@]} -eq 0 ]; then
    echo "Keine geeigneten Datenträger gefunden."
    exit 1
fi

for DEV in "${ALLOWED_DEVICES[@]}"; do
    lsblk -d -o NAME,SIZE,MODEL,RM,TRAN "$DEV"
done

echo
read -r -p "DEVICE eingeben (z.B. /dev/sde): " DEVICE

# Prüfen, ob DEVICE tatsächlich in der angezeigten Liste enthalten ist
VALID=0

for DEV in "${ALLOWED_DEVICES[@]}"; do
    if [ "$DEVICE" = "$DEV" ]; then
        VALID=1
        break
    fi
done

if [ "$VALID" -ne 1 ]; then
    echo
    echo "FEHLER: $DEVICE ist kein freigegebener Datenträger."
    echo "Es dürfen nur oben angezeigte Geräte verwendet werden."
    exit 1
fi

# Partitionsnamen bestimmen:
# /dev/sde      -> /dev/sde1
# /dev/mmcblk0  -> /dev/mmcblk0p1
if [[ "$DEVICE" =~ [0-9]$ ]]; then
    PARTITION="${DEVICE}p1"
else
    PARTITION="${DEVICE}1"
fi

echo
echo "=== Befehle für $DEVICE ==="
echo
echo "ACHTUNG: Diese Befehle löschen den kompletten Datenträger!"
echo

echo "sudo umount $PARTITION"
echo "sudo wipefs -a $DEVICE"

# ersten 10MB wirklich nullen (falls vorher GPT oder LUKS drauf war)
echo "# this is only a partial erase"
echo "#sudo dd if=/dev/zero of=$DEVICE bs=1M count=10"

#echo "# this is a full erase"
#echo "sudo dd if=/dev/zero of=$DEVICE bs=4M status=progress"


echo "sudo parted $DEVICE --script mklabel msdos"
echo "sudo parted $DEVICE --script mkpart primary fat32 1MiB 100%"
echo "sudo partprobe $DEVICE"
echo "sudo mkfs.fat -F 32 -n $LABEL $PARTITION"
echo "sync"

echo
echo "# Danach Kontrolle:"
echo "lsblk -o NAME,SIZE,MODEL,FSTYPE,LABEL,MOUNTPOINTS"
