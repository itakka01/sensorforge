#!/bin/bash

# ------------------------------------------------------------
# Arduino IDE Start-Script
#
# Verwendung:
#   ./start_ide.sh
#
# Das Script kann von jedem Verzeichnis aus gestartet werden.
#
# Arduino IDE liegt unter:
#   $HOME/ESP32_Board/
#
# Das Script prüft automatisch, ob Ubuntu unprivilegierte
# User-Namespaces per AppArmor einschränkt.
#
# Falls ja:
#   Arduino IDE wird mit --no-sandbox gestartet.
#
# Falls nein:
#   Arduino IDE wird normal gestartet.
# ------------------------------------------------------------

ARDUINO_DIR="$HOME/ESP32_Board"
IDE="$ARDUINO_DIR/arduino-ide_2.3.10_Linux_64bit.AppImage"

echo "----------------------------------------"
echo "Arduino IDE Start"
echo "Rechner: $(hostname)"
echo "IDE: $IDE"
echo "----------------------------------------"

# Prüfen, ob das AppImage vorhanden ist
if [ ! -f "$IDE" ]; then
    echo "FEHLER: Arduino IDE wurde nicht gefunden:"
    echo "$IDE"
    exit 1
fi

RESTRICTED=$(sysctl -n kernel.apparmor_restrict_unprivileged_userns 2>/dev/null || echo 0)

if [ "$RESTRICTED" = "1" ]; then
    echo "AppArmor User-Namespaces sind eingeschränkt."
    echo "Starte Arduino IDE mit --no-sandbox"
    echo "----------------------------------------"

    exec "$IDE" --no-sandbox
else
    echo "Keine AppArmor-Einschränkung erkannt."
    echo "Starte Arduino IDE normal"
    echo "----------------------------------------"

    exec "$IDE"
fi



