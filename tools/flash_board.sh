#!/usr/bin/env bash
# =============================================================
# SensorForge: Board auswählen, bauen und per USB flashen
# =============================================================
#
# Verwendung:
#   tools/flash_board.sh                      Menü (Board, Port, Bestätigung)
#   tools/flash_board.sh diymore              Board vorgeben, Port per Menü
#   tools/flash_board.sh diymore /dev/ttyACM0 Board und Port vorgeben
#   tools/flash_board.sh diymore --ota-only   nur bauen + WLAN-Paket, nicht flashen
#   tools/flash_board.sh all --ota-only       alle drei Boards bauen + WLAN-Pakete
#   tools/flash_board.sh --compileall         dasselbe (Kurzform)
#   Optionen: --rebuild  immer neu bauen
#             --monitor  nach dem Flashen ohne Rückfrage seriellen Monitor starten
#             --ota-only nicht flashen, nur Build + WLAN-Paket
#             --no-ota   kein WLAN-Paket erzeugen
#             --compileall (auch -compileall) = all --ota-only
#
# Was das Skript tut:
#   1. Kopiert die Quelldateien des Sketch-Ordners nach
#      ../sensorforge_builds/.work/<board>/ und stellt dort board_config.h
#      auf das gewählte Board. Die board_config.h im Sketch-Ordner bleibt
#      unverändert.
#   2. Baut mit dem arduino-cli der Arduino IDE (gleicher Core, gleiche
#      Einstellungen wie in der IDE, ohne "Erase All Flash").
#      Ergebnis: ../sensorforge_builds/<board>/ (außerhalb des Git-Repos).
#      Unveränderte Quellen werden nicht erneut gebaut.
#   3. Prüft die Größe gegen die echte Partition (tools/check_firmware_size.py).
#   4. Legt das signierte WLAN-Update-Paket an (tools/sf_firmware_publish.py):
#      ../sensorforge_builds/ota/firmware/<board>/<release>.sfw und
#      ../sensorforge_builds/ota/manifest.json (neueste Version je Board).
#      und kopiert Manifest + Pakete in den lokalen Apache
#      (/var/www/html/sensorforge-ota, URL .../sensorforge-ota/manifest.json).
#   5. Flasht Bootloader, Partitionstabelle, boot_app0 und App - genau wie die
#      IDE. Kein Löschen des Flash, nie merged.bin: Konfiguration, WLAN-Daten,
#      Integration-ID (NVS) und LittleFS bleiben erhalten.
#
# Vor dem Flashen den seriellen Monitor der Arduino IDE schließen
# (sonst ist der Port belegt).
# =============================================================

set -u

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT_ROOT=${SF_BUILD_ROOT:-$(dirname "$REPO")/sensorforge_builds}
CLI_CONFIG=${ARDUINO_CLI_CONFIG:-$HOME/.arduinoIDE/arduino-cli.yaml}
CLI_CACHE=$HOME/.cache/sensorforge/arduino-cli
BAUD=921600
MONITOR_BAUD=115200

# Same options as the Arduino IDE uses today (Tools menu), EraseFlash=none.
FQBN_BASE="esp32:esp32:esp32s3:UploadSpeed=${BAUD},USBMode=hwcdc,CDCOnBoot=cdc,CPUFreq=240,FlashMode=qio,FlashSize=8M,DebugLevel=none,PSRAM=opi,LoopCore=1,EventsCore=1,EraseFlash=none"

BOARDS=(diymore xiao freenove)

board_define() {
    case $1 in
        diymore) echo BOARD_DIYMORE_S3CAM ;;
        xiao) echo BOARD_XIAO ;;
        freenove) echo BOARD_FREENOVE ;;
    esac
}

board_title() {
    case $1 in
        diymore) echo "diymore ESP32-S3 CAM (OV5640)   c001, c002, ..." ;;
        xiao) echo "Seeed XIAO ESP32S3 Sense        devxiao1/2/3" ;;
        freenove) echo "Freenove FNK0085 (OV3660)       devfreenove1" ;;
    esac
}

# Expected physical flash size (MB) as reported by esptool flash-id.
board_flash_mb() {
    case $1 in
        diymore) echo 16 ;;
        *) echo 8 ;;
    esac
}

# ---------------------------------------------------------------- helpers

USE_WHIPTAIL=0
if command -v whiptail >/dev/null 2>&1 && [ -t 0 ] && [ -t 1 ]; then
    USE_WHIPTAIL=1
fi

die() {
    echo "FEHLER: $*" >&2
    exit 1
}

info() {
    echo
    echo "==> $*"
}

# menu <title> <text> <tag> <item> [<tag> <item> ...] -> prints chosen tag
menu() {
    local title=$1 text=$2
    shift 2
    if [ "$USE_WHIPTAIL" = 1 ]; then
        local n=$(( $# / 2 ))
        whiptail --title "$title" --menu "$text" $(( n + 10 )) 78 "$n" "$@" 3>&1 1>&2 2>&3
        return
    fi
    local tags=() items=()
    while [ $# -gt 0 ]; do
        tags+=("$1")
        items+=("$1  $2")
        shift 2
    done
    echo "$title - $text" >&2
    local PS3="Auswahl (Nummer): " choice
    select choice in "${items[@]}"; do
        if [ -n "${choice:-}" ]; then
            echo "${tags[$((REPLY - 1))]}"
            return 0
        fi
    done
    return 1
}

# confirm <title> <text>
confirm() {
    if [ "$USE_WHIPTAIL" = 1 ]; then
        whiptail --title "$1" --yesno "$2" 20 78
        return
    fi
    echo "$1"
    echo "$2"
    local answer
    read -r -p "Fortfahren? [j/N] " answer
    [[ $answer =~ ^[jJyY]$ ]]
}

arduino_data_dir() {
    local dir=""
    if [ -f "$CLI_CONFIG" ]; then
        dir=$(sed -n 's/^[[:space:]]*data:[[:space:]]*//p' "$CLI_CONFIG" | head -1)
    fi
    echo "${dir:-$HOME/.arduino15}"
}

find_cli() {
    if [ -n "${ARDUINO_CLI:-}" ] && [ -x "$ARDUINO_CLI" ]; then
        echo "$ARDUINO_CLI"
        return 0
    fi
    if command -v arduino-cli >/dev/null 2>&1; then
        command -v arduino-cli
        return 0
    fi
    if [ -x "$CLI_CACHE" ]; then
        echo "$CLI_CACHE"
        return 0
    fi
    # Extract only arduino-cli from the Arduino IDE AppImage (once).
    local app
    app=$(ls -1t "$(dirname "$REPO")"/arduino-ide_*.AppImage 2>/dev/null | head -1)
    [ -n "$app" ] || return 1
    echo "arduino-cli wird einmalig aus $(basename "$app") entnommen ..." >&2
    local tmp
    tmp=$(mktemp -d)
    (cd "$tmp" && "$app" --appimage-extract 'resources/app/lib/backend/resources/arduino-cli' >/dev/null 2>&1)
    local extracted=$tmp/squashfs-root/resources/app/lib/backend/resources/arduino-cli
    if [ -x "$extracted" ]; then
        mkdir -p "$(dirname "$CLI_CACHE")"
        cp "$extracted" "$CLI_CACHE"
        rm -rf "$tmp"
        echo "$CLI_CACHE"
        return 0
    fi
    rm -rf "$tmp"
    return 1
}

find_esptool() {
    local data=$1
    ls -1d "$data"/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | sort -V | tail -1
}

partition_scheme() {
    local data=$1
    if cat "$data"/packages/esp32/hardware/esp32/*/boards.txt \
           "$data"/packages/esp32/hardware/esp32/*/boards.local.txt 2>/dev/null |
       grep -q '^esp32s3\.menu\.PartitionScheme\.sensorforge='; then
        echo sensorforge
    else
        # Same flashed table (sketch partitions.csv overrides the menu);
        # only the compiler's size limit is stricter.
        echo default_8MB
    fi
}

# Code files that the Arduino build compiles from the sketch folder.
source_files() {
    find "$REPO" -maxdepth 1 -type f \
        \( -name '*.ino' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' \
           -o -name '*.hpp' -o -name '*.S' -o -name 'partitions.csv' \
           -o -name 'build_opt.h' -o -name 'bootloader.bin' \) -printf '%f\n' | sort
}

# Patched board_config.h content for the chosen board on stdout.
patched_board_config() {
    local define
    define=$(board_define "$1")
    sed -E 's@^#define (BOARD_FREENOVE|BOARD_XIAO|BOARD_DIYMORE_S3CAM)[[:space:]]*$@//#define \1@; s@^//[[:space:]]*#define '"$define"'[[:space:]]*$@#define '"$define"'@' \
        "$REPO/board_config.h"
}

# Mirrors the code files into the work sketch; keeps mtimes of unchanged
# files so arduino-cli can reuse its object cache.
sync_work_sketch() {
    local board=$1 work=$2
    mkdir -p "$work"
    local f
    while read -r f; do
        [ "$f" = board_config.h ] && continue
        if ! cmp -s "$REPO/$f" "$work/$f"; then
            cp -p "$REPO/$f" "$work/$f"
        fi
    done < <(source_files)
    # Remove files that no longer exist in the sketch folder.
    for f in "$work"/*; do
        [ -f "$f" ] || continue
        local name
        name=$(basename "$f")
        [ "$name" = board_config.h ] && continue
        [ -f "$REPO/$name" ] || rm -f "$f"
    done
    local tmp
    tmp=$(mktemp)
    patched_board_config "$board" > "$tmp"
    if ! cmp -s "$tmp" "$work/board_config.h"; then
        cp "$tmp" "$work/board_config.h"
    fi
    rm -f "$tmp"

    local active
    active=$(grep -E '^#define (BOARD_FREENOVE|BOARD_XIAO|BOARD_DIYMORE_S3CAM)$' "$work/board_config.h")
    [ "$active" = "#define $(board_define "$board")" ] ||
        die "board_config.h konnte nicht eindeutig auf $(board_define "$board") gestellt werden (gefunden: ${active:-nichts})."
}

source_hash() {
    local board=$1 fqbn=$2
    {
        echo "$board $fqbn"
        (cd "$REPO" && source_files | xargs -d '\n' sha1sum)
    } | sha1sum | cut -d' ' -f1
}

list_ports() {
    local p
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [ -e "$p" ] || continue
        local serial="" model=""
        if command -v udevadm >/dev/null 2>&1; then
            serial=$(udevadm info -q property -n "$p" 2>/dev/null | sed -n 's/^ID_SERIAL_SHORT=//p')
            model=$(udevadm info -q property -n "$p" 2>/dev/null | sed -n 's/^ID_MODEL=//p')
        fi
        echo "$p|${model:-unbekannt}${serial:+  (Seriennr./MAC $serial)}"
    done
}

# ---------------------------------------------------------------- arguments

BOARD=""
PORT=""
REBUILD=0
AUTO_MONITOR=0
OTA_ONLY=0
NO_OTA=0
for arg in "$@"; do
    case $arg in
        --rebuild) REBUILD=1 ;;
        --monitor) AUTO_MONITOR=1 ;;
        --ota-only) OTA_ONLY=1 ;;
        --no-ota) NO_OTA=1 ;;
        --compileall|-compileall) BOARD=all; OTA_ONLY=1 ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        /dev/*) PORT=$arg ;;
        diymore|xiao|freenove|all) BOARD=$arg ;;
        *) die "Unbekanntes Argument: $arg (erlaubt: diymore|xiao|freenove|all, /dev/tty..., --rebuild, --monitor, --ota-only, --no-ota, --compileall)" ;;
    esac
done
[ "$OTA_ONLY" = 1 ] && [ "$NO_OTA" = 1 ] && die "--ota-only und --no-ota schließen sich aus."

if [ "$BOARD" = all ]; then
    [ "$OTA_ONLY" = 1 ] || die "'all' nur zusammen mit --ota-only (geflasht wird immer ein Board)."
    for b in "${BOARDS[@]}"; do
        "${BASH_SOURCE[0]}" "$b" --ota-only $([ "$REBUILD" = 1 ] && echo --rebuild) || die "$b fehlgeschlagen."
    done
    echo
    echo "Alle Boards gebaut und als WLAN-Paket bereit."
    echo "Manifest-URL für \"Firmware im Cluster\" am Coordinator:"
    echo "    $(python3 "$REPO/tools/sf_firmware_publish.py" --url)"
    exit 0
fi

# ---------------------------------------------------------------- tools

DATA_DIR=$(arduino_data_dir)
CLI=$(find_cli) || die "arduino-cli nicht gefunden. Arduino IDE AppImage in $(dirname "$REPO") erwartet oder ARDUINO_CLI=/pfad/arduino-cli setzen."
ESPTOOL=$(find_esptool "$DATA_DIR")
[ -n "$ESPTOOL" ] || die "esptool nicht gefunden unter $DATA_DIR/packages/esp32/tools/esptool_py/"
command -v python3 >/dev/null 2>&1 || die "python3 fehlt (für die Größenprüfung)."

# ---------------------------------------------------------------- board

if [ -z "$BOARD" ]; then
    items=()
    for b in "${BOARDS[@]}"; do
        items+=("$b" "$(board_title "$b")")
    done
    BOARD=$(menu "SensorForge flashen" "Welches Board soll $([ "$OTA_ONLY" = 1 ] && echo gebaut || echo geflasht) werden?" "${items[@]}") || exit 1
fi
[ -n "$(board_define "$BOARD")" ] || die "Unbekanntes Board: $BOARD"

FQBN="${FQBN_BASE},PartitionScheme=$(partition_scheme "$DATA_DIR")"
OUT=$OUT_ROOT/$BOARD
WORK=$OUT_ROOT/.work/$BOARD/sensorforge
LOG=$OUT_ROOT/build_$BOARD.log
mkdir -p "$OUT"

# ---------------------------------------------------------------- build

HASH=$(source_hash "$BOARD" "$FQBN")
NEED_BUILD=1
if [ "$REBUILD" = 0 ] && [ -f "$OUT/.source_hash" ] && [ -f "$OUT/sensorforge.ino.bin" ] &&
   [ "$(cat "$OUT/.source_hash")" = "$HASH" ] && [ "$OTA_ONLY" = 1 ]; then
    # Package only: an up-to-date build needs no question.
    NEED_BUILD=0
    echo "    $BOARD: Build vom aktuellen Quellstand vorhanden."
elif [ "$REBUILD" = 0 ] && [ -f "$OUT/.source_hash" ] && [ -f "$OUT/sensorforge.ino.bin" ] &&
   [ "$(cat "$OUT/.source_hash")" = "$HASH" ]; then
    built_at=$(date -r "$OUT/sensorforge.ino.bin" '+%d.%m.%Y %H:%M')
    choice=$(menu "Build vorhanden" "Für $BOARD gibt es einen Build vom aktuellen Quellstand ($built_at)." \
        use "Vorhandenen Build flashen (schnell)" \
        rebuild "Trotzdem neu bauen") || exit 1
    [ "$choice" = use ] && NEED_BUILD=0
fi

if [ "$NEED_BUILD" = 1 ]; then
    info "Baue $BOARD ($(board_define "$BOARD"))"
    echo "    Erster Build eines Boards dauert einige Minuten, danach deutlich kürzer."
    echo "    Vollständige Ausgabe: $LOG"
    sync_work_sketch "$BOARD" "$WORK"
    rm -f "$OUT/.source_hash"
    # Compiler output goes to the log only; the terminal shows progress.
    "$CLI" compile --config-file "$CLI_CONFIG" --fqbn "$FQBN" --build-path "$OUT" \
        --warnings default "$WORK" > "$LOG" 2>&1 &
    build_pid=$!
    trap 'kill "$build_pid" 2>/dev/null; echo; echo "Abgebrochen."; exit 1' INT TERM
    started=$(date +%s)
    while kill -0 "$build_pid" 2>/dev/null; do
        printf '\r    läuft ... %d s ' $(( $(date +%s) - started ))
        sleep 2
    done
    wait "$build_pid"
    status=$?
    trap - INT TERM
    printf '\r    fertig nach %d s       \n' $(( $(date +%s) - started ))
    if [ "$status" != 0 ]; then
        echo
        grep -E 'error:|Error|FEHLER|fatal' "$LOG" | head -20
        die "Build fehlgeschlagen (Exit $status). Details: $LOG"
    fi
    grep -E '^(Sketch uses|Global variables|Der Sketch verwendet|Globale Variablen)' "$LOG" | sed 's/^/    /'
    warnings=$(grep -c 'warning:' "$LOG")
    echo "    Compiler-Warnungen: $warnings (bis zu 7 bekannte, harmlose; nur neu übersetzte Dateien zählen; Details im Log)"
    echo "$HASH" > "$OUT/.source_hash"
fi

info "Größenprüfung"
python3 "$REPO/tools/check_firmware_size.py" "$OUT" || die "Größenprüfung fehlgeschlagen - nicht geflasht."

for f in sensorforge.ino.bootloader.bin sensorforge.ino.partitions.bin boot_app0.bin sensorforge.ino.bin flash_args; do
    [ -f "$OUT/$f" ] || die "Build unvollständig, $f fehlt in $OUT (mit --rebuild neu bauen)."
done

# ---------------------------------------------------------------- WLAN-Update-Paket

if [ "$NO_OTA" = 0 ]; then
    info "WLAN-Update-Paket ($BOARD)"
    # Signs only if the image changed; the manifest always lists the newest
    # release per board. A failure here never blocks the USB flash.
    python3 "$REPO/tools/sf_firmware_publish.py" "$OUT/sensorforge.ino.bin" --no-serve 2>&1 | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] || echo "    WARNUNG: WLAN-Paket nicht erzeugt (siehe oben); USB-Flash ist davon unabhängig."
fi

if [ "$OTA_ONLY" = 1 ]; then
    info "Fertig: $BOARD gebaut, WLAN-Paket bereit (nicht geflasht)."
    echo "    Manifest-URL: $(python3 "$REPO/tools/sf_firmware_publish.py" --url)"
    exit 0
fi

# ---------------------------------------------------------------- port

if [ -z "$PORT" ]; then
    mapfile -t ports < <(list_ports)
    [ "${#ports[@]}" -gt 0 ] || die "Kein serieller Port gefunden (/dev/ttyACM*, /dev/ttyUSB*). Board per USB anschließen."
    items=()
    for line in "${ports[@]}"; do
        items+=("${line%%|*}" "${line#*|}")
    done
    PORT=$(menu "Port wählen" "An welchem Port hängt das $BOARD-Board?" "${items[@]}") || exit 1
fi
[ -e "$PORT" ] || die "Port $PORT existiert nicht."

if command -v fuser >/dev/null 2>&1 && fuser "$PORT" >/dev/null 2>&1; then
    confirm "Port belegt" "$PORT wird gerade von einem anderen Programm benutzt (z. B. serieller Monitor der Arduino IDE).

Bitte den Monitor schließen und dann 'Ja' wählen." || exit 1
fi

# ---------------------------------------------------------------- safety check

info "Prüfe angeschlossenes Board an $PORT"
ID_OUT=$("$ESPTOOL" --chip esp32s3 --port "$PORT" --baud "$BAUD" flash-id 2>&1)
ID_STATUS=$?
echo "$ID_OUT" | grep -E 'Chip type|Chip is|MAC|Detected flash size' || true
[ "$ID_STATUS" = 0 ] || die "Board an $PORT antwortet nicht (Port belegt oder falsches Gerät?).
$(echo "$ID_OUT" | tail -5)"

FLASH_MB=$(echo "$ID_OUT" | sed -n 's/.*Detected flash size: \([0-9]*\)MB.*/\1/p' | head -1)
MAC=$(echo "$ID_OUT" | sed -n 's/^MAC: *//p' | head -1)
EXPECTED_MB=$(board_flash_mb "$BOARD")
WARNING=""
if [ -n "$FLASH_MB" ] && [ "$FLASH_MB" != "$EXPECTED_MB" ]; then
    WARNING="

ACHTUNG: Das angeschlossene Board hat ${FLASH_MB} MB Flash, ein $BOARD-Board hat ${EXPECTED_MB} MB.
Wahrscheinlich ist das falsche Board angeschlossen oder ausgewählt!"
fi

SIZE=$(stat -c %s "$OUT/sensorforge.ino.bin")
BUILT=$(date -r "$OUT/sensorforge.ino.bin" '+%d.%m.%Y %H:%M')
confirm "Jetzt flashen?" "Board:   $(board_title "$BOARD")
Port:    $PORT${MAC:+   (MAC $MAC)}
Flash:   ${FLASH_MB:-?} MB
Build:   $BUILT, App $SIZE Bytes

Es wird NICHT gelöscht: Konfiguration, WLAN, Integration-ID
und LittleFS bleiben erhalten.$WARNING" || { echo "Abgebrochen, nichts geflasht."; exit 1; }

# ---------------------------------------------------------------- flash

info "Flashe $BOARD an $PORT"
read -r -a FLASH_OPTS < <(head -1 "$OUT/flash_args")
mapfile -t FLASH_PARTS < <(tail -n +2 "$OUT/flash_args")
PART_ARGS=()
for line in "${FLASH_PARTS[@]}"; do
    [ -n "$line" ] || continue
    addr=${line%% *}
    file=${line#* }
    case $file in
        *merged*) die "flash_args enthält merged.bin - abgebrochen." ;;
    esac
    PART_ARGS+=("$addr" "$OUT/$file")
done

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud "$BAUD" \
    --before default-reset --after hard-reset \
    write-flash "${FLASH_OPTS[@]}" "${PART_ARGS[@]}" || die "Flashen fehlgeschlagen."

info "Fertig: $BOARD an $PORT geflasht."

# ---------------------------------------------------------------- monitor

if [ "$AUTO_MONITOR" = 1 ] || confirm "Serieller Monitor" "Seriellen Monitor jetzt starten?

Die Ausgabe wird zusätzlich in $OUT_ROOT/logs/ gespeichert.
Beenden mit Strg+C."; then
    mkdir -p "$OUT_ROOT/logs"
    MON_LOG=$OUT_ROOT/logs/${BOARD}_$(date '+%Y-%m-%d_%H%M%S').log
    info "Serieller Monitor $PORT @ $MONITOR_BAUD - Log: $MON_LOG (Strg+C beendet)"
    sleep 1
    "$CLI" monitor --config-file "$CLI_CONFIG" -p "$PORT" -c "baudrate=$MONITOR_BAUD" --quiet 2>&1 | tee "$MON_LOG"
fi
