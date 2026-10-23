# Partition-Upgrade-Anleitung: 3.2 MB → 3.75 MB

**Datum:** 2026-10-16
**Grund:** Flash 95% voll (3.18 MB von 3.34 MB genutzt)
**Lösung:** App-Partitionen vergrößern, SPIFFS reduzieren

---

## ÄNDERUNGEN

### Alte Partition-Tabelle (default_8MB)
```
nvs:      20 KB
otadata:  8 KB
app0:     3.2 MB   ← ZU KLEIN
app1:     3.2 MB   ← ZU KLEIN
spiffs:   1.5 MB   ← OVERKILL (Configs sind nur KB-groß)
coredump: 64 KB
Total:    8 MB
```

### Neue Partition-Tabelle (partitions.csv)
```
nvs:      20 KB    (unverändert)
otadata:  8 KB     (unverändert)
app0:     3.75 MB  (+550 KB) ✅
app1:     3.75 MB  (+550 KB) ✅
spiffs:   384 KB   (-1.1 MB)
coredump: 64 KB    (unverändert)
Total:    8 MB
```

**Gewinn:** +550 KB pro App-Partition (17% mehr Platz!)

---

## ⚠️ WICHTIG: DATENSICHERUNG VOR UPGRADE

### Flash-Erase löscht ALLE Daten!

**Folgende Daten gehen verloren:**
- ❌ `/config.txt` (WiFi-Credentials, alle Einstellungen)
- ❌ `/license.dat` (Lizenz-Aktivierung)
- ❌ NVS (integration_id, bekannte Cluster-Credentials)
- ❌ SPIFFS (Logs, Transport-State)

**SD-Karte bleibt erhalten** (falls vorhanden)

---

## SCHRITT-FÜR-SCHRITT-ANLEITUNG

### PHASE 1: BACKUP (30 Min für alle 4 Boards)

#### 1.1 Config sichern

**Für jedes Board:**
```bash
# Browser öffnen:
http://devfreenove1.local/
http://devxiao1.local/
http://devxiao2.local/
http://devxiao3.local/

# Auf jeder WebConfig-Seite:
# 1. Runterscrollen zu "Konfiguration"
# 2. Button "Config herunterladen" klicken
# 3. Als devfreenove1_config.txt speichern (etc.)
```

**Alternative (SD-Karte vorhanden):**
```bash
# SD-Karte aus Board entnehmen
# /config.txt von SD kopieren
# Sicherer als WebConfig-Download!
```

#### 1.2 License-Codes notieren

**Falls Boards aktiviert sind:**
```bash
# WebConfig öffnen → "Lizenz" Sektion
# Activation-Code notieren (beginnt mit "SF1-" oder "SF2-")
# Oder: /license.dat von LittleFS sichern (nur wenn SD fehlt)
```

#### 1.3 Integration-IDs sichern (optional)

**Für Cluster-Nodes wichtig:**
```bash
# Browser: http://devxiao1.local/api/v1/identity

# Response:
{
  "integration_id": "a1b2c3d4-..."  # ← NOTIEREN!
}

# Für alle 4 Boards wiederholen
```

**Warum:** Integration-ID = Cluster-Node-Identity
Nach Flash-Erase werden neue UUIDs generiert → Cluster erkennt Boards als "neu"

#### 1.4 Cluster-Credentials sichern

```bash
# WebConfig → /cluster Seite
# Cluster-Name, Cluster-ID, Passwort notieren
```

---

### PHASE 2: ARDUINO IDE VORBEREITEN (5 Min)

#### 2.1 Partition-Datei prüfen

```bash
ls -lh /home/itakka/ESP32_Board/sensorforge/partitions.csv

# Sollte zeigen:
# -rw-r--r-- 1 user user 567 Oct 16 XX:XX partitions.csv
```

#### 2.2 Arduino IDE starten

```bash
cd /home/itakka/ESP32_Board/sensorforge
./start_ide.sh
```

#### 2.3 Board-Einstellungen anpassen

**Aktualisiert 2026-10-23 (siehe `FLASH_BUDGET.md`):**

```bash
tools/install_ide_partition_scheme.sh   # legt den Menüpunkt per boards.local.txt an
```

Danach Arduino IDE neu starten und **Tools → Partition Scheme → „SensorForge 3.75MB APP / 384KB SPIFFS“** wählen.
Die `partitions.csv` im Sketch-Ordner wird unabhängig vom Menü immer geflasht; der Menüpunkt sorgt nur dafür, dass die IDE gegen das richtige Limit (3.932.160 B) prüft.

⚠️ **Nicht „Custom“ wählen:** Dieser Menüpunkt prüft gegen 16 MB, ein zu großes Image würde unbemerkt in app1 hineinragen.

---

### PHASE 3: FLASH-PROZESS (15 Min pro Board)

#### 3.1 Board anschließen

```bash
# USB-Verbindung zu devfreenove1
ls /dev/ttyACM*
# Sollte zeigen: /dev/ttyACM0 (oder ACM1, etc.)
```

#### 3.2 Flash komplett löschen

**WICHTIG:** Partition-Wechsel erfordert vollständiges Erase!

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash
```

**Ausgabe:**
```
Erasing flash (this may take a while)...
Chip erase completed successfully in 10.5s
```

#### 3.3 Firmware mit neuer Partition flashen

**Methode A: Arduino IDE**
```
1. sensorforge.ino öffnen
2. Board-Profil prüfen (board_config.h: BOARD_FREENOVE für devfreenove1)
3. Tools → Port → /dev/ttyACM0
4. Sketch → Upload
```

**Methode B: esptool (manuell)**
```bash
# Zuerst kompilieren (Arduino IDE: Sketch → Export Compiled Binary)

esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     build/esp32.esp32.esp32s3/sensorforge.ino.bootloader.bin \
  0x8000  build/esp32.esp32.esp32s3/sensorforge.ino.partitions.bin \
  0xe000  ~/.arduino15/packages/esp32/hardware/esp32/3.3.12/tools/partitions/boot_app0.bin \
  0x10000 build/esp32.esp32.esp32s3/sensorforge.ino.bin
```

#### 3.4 Boot prüfen

```bash
# Serial Monitor öffnen (115200 baud)
# Erwartung:
# - Boot-Banner mit v87-beta105
# - "No SD card" (normal beim ersten Boot ohne Config)
# - WiFi Hotspot startet (SensorForge-XXXXXX)
```

---

### PHASE 4: KONFIGURATION WIEDERHERSTELLEN (10 Min pro Board)

#### 4.1 WiFi-Hotspot verbinden

```bash
# WLAN-Netz suchen: "SensorForge-XXXXXX"
# Passwort: (Default aus Code, meist leer oder "sensorforge")

# Browser: http://192.168.4.1/
```

#### 4.2 Config hochladen

**Methode A: WebConfig Upload**
```
1. WebConfig öffnen
2. Runterscrollen zu "Konfiguration"
3. "Config-Datei hochladen"
4. devfreenove1_config.txt auswählen
5. Upload → Board neustartet
```

**Methode B: SD-Karte (falls vorhanden)**
```bash
# Gesicherte config.txt auf SD-Karte kopieren
# SD in Board einlegen
# Board neu starten
# Config wird automatisch von SD geladen
```

#### 4.3 Lizenz reaktivieren (falls nötig)

```bash
# WebConfig → Lizenz-Sektion
# Activation-Code (SF1-XXX oder SF2-XXX) eingeben
# Aktivieren
```

#### 4.4 Integration-ID prüfen

```bash
# Browser: http://devfreenove1.local/api/v1/identity

# Neue UUID wurde generiert!
# Falls Cluster-Node: Cluster neu joinen (siehe 4.5)
```

#### 4.5 Cluster neu joinen

**Coordinator (devfreenove1):**
```
1. WebConfig → /cluster
2. Cluster-Name/ID/Passwort eingeben (aus Backup)
3. Policy: "Preferred Coordinator"
4. Speichern
```

**Nodes (devxiao1/2/3):**
```
1. WebConfig → /cluster
2. Gleicher Cluster-Name/ID/Passwort wie Coordinator
3. Policy: "Auto" (oder "Node" für devxiao3 falls dediziert)
4. Speichern
```

**Erwartung:**
- Alle 4 Boards im Cluster sichtbar
- devfreenove1 als Coordinator gewählt
- Neue Integration-IDs → neue Node-Identitäten!

---

### PHASE 5: ALLE 4 BOARDS UPGRADEN (1-2 Stunden)

**Reihenfolge:**

1. ✅ **devfreenove1** (Coordinator) zuerst
   - board_config.h: `#define BOARD_FREENOVE`
   - Kompilieren → Flash-Erase → Upload → Config-Restore

2. ✅ **devxiao1** (Node)
   - board_config.h: `#define BOARD_XIAO`
   - Kompilieren → Flash-Erase → Upload → Config-Restore

3. ✅ **devxiao2** (Node)
   - Gleicher Build wie devxiao1
   - Flash-Erase → Upload → Config-Restore

4. ✅ **devxiao3** (Time Broadcaster)
   - Gleicher Build wie devxiao1/2 (außer Custom-Code vorhanden)
   - Flash-Erase → Upload → Config-Restore

**Nach jedem Board:**
- Serial-Log prüfen (keine Crashes)
- WebConfig erreichbar
- Cluster-Membership prüfen

---

## VALIDIERUNG

### Nach Upgrade aller 4 Boards:

#### 1. Flash-Nutzung prüfen

```bash
# Build-Output sollte zeigen:
Sketch uses XXXXX bytes (XX%) of program storage space. Maximum is 3932160 bytes.

# Erwartung: <85% statt 95%!
```

#### 2. Cluster-Health

```bash
# http://devfreenove1.local/cluster
# Erwartung:
# - 4 Peers sichtbar
# - devfreenove1 ist Coordinator
# - Heartbeats < 10s
```

#### 3. Funktions-Test

**Basic:**
- [ ] Recording auf devxiao1 (falls SD + Kamera)
- [ ] WebConfig auf allen Boards
- [ ] RTSP-Stream (falls Kamera)

**Cluster:**
- [ ] Cluster-Capture (devxiao1 einzeln)
- [ ] Multi-Node-Capture (alle 3 Nodes)
- [ ] Time-Sync funktioniert

---

## TROUBLESHOOTING

### Problem: "Partition table does not fit in configured flash size"

**Ursache:** partitions.csv Offsets falsch

**Lösung:**
```bash
# Partition-Summe prüfen:
python3 << EOF
partitions = [
    ("nvs",      0x9000,   0x5000),
    ("otadata",  0xe000,   0x2000),
    ("app0",     0x10000,  0x3C0000),
    ("app1",     0x3D0000, 0x3C0000),
    ("spiffs",   0x790000, 0x60000),
    ("coredump", 0x7F0000, 0x10000),
]
for name, offset, size in partitions:
    print(f"{name:10} offset={hex(offset):8} size={hex(size):8} end={hex(offset+size):8}")
print(f"Total: {hex(0x7F0000 + 0x10000)} (should be 0x800000 for 8MB)")
EOF
```

### Problem: Boot-Loop nach Flash

**Ursache:** Falscher Bootloader oder app0-Offset

**Lösung:**
```bash
# Boot-Log lesen:
screen /dev/ttyACM0 115200

# Partition-Tabelle im Flash prüfen:
esptool.py --chip esp32s3 --port /dev/ttyACM0 read_flash 0x8000 0x1000 partition.bin
python3 ~/.arduino15/packages/esp32/hardware/esp32/3.3.12/tools/gen_esp32part.py partition.bin
```

### Problem: Config-Upload schlägt fehl

**Ursache:** Alte Config enthält ungültige Keys

**Lösung:**
```bash
# Config manuell editieren:
# - Ungültige Zeilen entfernen
# - Oder: Factory-Reset + manuelle Neukonfiguration
```

---

## ROLLBACK (falls nötig)

### Zur alten Partition zurück:

```bash
# 1. Flash erneut löschen
esptool.py --chip esp32s3 --port /dev/ttyACM0 erase_flash

# 2. Arduino IDE: Tools → Partition Scheme → "Default 8MB with SPIFFS"
# 3. Sketch → Upload

# 4. Config-Backup wiederherstellen
```

**Nachteil:** Flash wieder 95% voll!

---

## ZUSAMMENFASSUNG

**Was passiert:**
- ✅ App-Partitionen: 3.2 MB → 3.75 MB (+17%)
- ✅ Flash-Nutzung: 95% → ~81%
- ✅ Puffer für Audit-Pakete & Features: +550 KB
- ⚠️ SPIFFS: 1.5 MB → 384 KB (ausreichend für Configs)
- ⚠️ Alle Daten gehen verloren (Backup erforderlich!)

**Zeitaufwand:**
- Backup: 30 Min
- Flash pro Board: 15 Min
- Total: ~2 Stunden für 4 Boards

**Kritisch:**
- ⚠️ Config-Backup VORHER!
- ⚠️ License-Codes notieren!
- ⚠️ Integration-IDs werden NEU generiert → Cluster neu joinen!

---

**Bei Fragen während des Upgrades: STOPP und Supervisor fragen!**
