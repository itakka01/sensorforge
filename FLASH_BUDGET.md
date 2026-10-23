# Flash-Budget und Partitions-Limit (Stand 2026-10-23, nach v87-beta139)

## 1. Befund

Die Arduino-IDE meldete nach der Gruppenverwaltung **98,3 % (Freenove)**. Die Zahl bezieht sich auf
das falsche Limit:

| | Wert |
|---|---|
| IDE-Menü „Partition Scheme“ (laut IDE-Log seit Wochen) | `default_8MB` → Größenprüfung gegen **3.342.336 B** |
| Tatsächlich geflashte Partitionstabelle | `partitions.csv` im Sketch-Ordner (seit 2026-10-16, Commit db22560) → OTA-Slots **3.932.160 B** (0x3C0000) |

Warum beides gleichzeitig gilt: arduino-esp32 3.3.12 kopiert in `recipe.hooks.prebuild.3` eine
`partitions.csv` aus dem Sketch-Ordner **über** die Tabelle der Menüauswahl. Jeder IDE-Upload
schreibt daraus `partitions.bin` an 0x8000. Die Größenprüfung der IDE
(`upload.maximum_size`) kommt aber weiterhin aus dem Menü. Die Geräte haben also längst
3,75-MB-Slots, die IDE hätte den Build aber schon ab 3,2 MB abgelehnt.

Aktuelle Belegung der Partitionen (8 MB Flash, unverändert):

| Partition | Offset | Größe |
|---|---|---|
| nvs | 0x9000 | 20 KB |
| otadata | 0xE000 | 8 KB |
| app0 / app1 | 0x10000 / 0x3D0000 | je 3.932.160 B (3,75 MB) |
| spiffs (LittleFS) | 0x790000 | 384 KB |
| coredump | 0x7F0000 | 64 KB |

Die Tabelle ist voll belegt; ohne kleinere LittleFS- oder Coredump-Partition gibt es keinen
weiteren Platz. Die Firmware selbst prüft bereits gegen die echte Partition: Die
Systemübersicht zeigt „Firmware / App-Partition“, und SD-Update, Web-Upload und Package-Fetch
prüfen gegen `esp_ota_get_next_update_partition()->size`.

## 2. Ursachen der Größe (Freenove-ELF, Gruppenverwaltung enthalten)

- `.flash.text` 2,03 MB und `.flash.rodata` 1,07 MB. Den Hauptteil von rodata machen
  zusammengeführte String-Literale aus (HTML, CSS, JS, JSON-Schlüssel der WebConfig).
- Größte Projektmodule (Code und eigene Daten): `webconfig.cpp` ca. 500 KB,
  `webconfig_cluster.cpp` ca. 140 KB, `cluster.cpp` ca. 125 KB, `config.cpp` ca. 110 KB,
  `sensorforge.ino` ca. 105 KB, `sync_api.cpp` ca. 100 KB, `webplayer.cpp` ca. 100 KB,
  `web_sd_maintenance.cpp` ca. 95 KB.
- ESP-IDF und WLAN-Stack (net80211, lwIP, mbedTLS, wpa_supplicant, PHY) zusammen ca. 0,7 MB.
  Diese Teile sind vorkompiliert und mit Sketch-Mitteln nicht verkleinerbar.
- Build-Konfiguration ist bereits auf Größe optimiert: `-Os`
  (`CONFIG_COMPILER_OPTIMIZATION_SIZE`), `DebugLevel=none`, `--gc-sections`. Es gibt keinen
  übersehenen Debug- oder `-O2`-Build und keine doppelt eingebundenen großen Tabellen
  (geprüft per `nm`).

## 3. Lösung (umgesetzt)

Die Partitionierung, die Firmware und die Gerätedaten bleiben unverändert. Korrigiert wird
die **Größenprüfung**, damit sie zur tatsächlich geflashten Tabelle passt:

1. `tools/install_ide_partition_scheme.sh`
   - Installiert den Menüpunkt **„SensorForge 3.75MB APP / 384KB SPIFFS“** über
     `boards.local.txt` in jedem installierten esp32-Core. `boards.txt` wird nicht bearbeitet.
   - Das Limit wird aus `partitions.csv` gelesen und kopiert die Tabelle als
     `tools/partitions/sensorforge.csv`.
   - Das Skript ist idempotent. Nach jedem Core-Update erneut ausführen.
   - In der aktuellen Installation existiert der Menüpunkt bereits (am 16.10. von Hand in
     `boards.txt` eingetragen). Er ist nur nicht ausgewählt.
2. `tools/check_firmware_size.py <build-pfad>...`
   - Prüft das Image gegen die App-Slot-Größe aus `partitions.csv`.
   - Prüft, dass die erzeugte `partitions.bin` exakt `partitions.csv` entspricht.
   - Warnt unter 10 % freiem Platz und endet bei Überschreitung mit Exit 1.
   - Rein lesend.
3. Dokumentation: FQBN `PartitionScheme=sensorforge` in `CLAUDE.md`, Speicherlayout dort
   korrigiert.

**In der IDE einmalig umstellen:** Werkzeuge → Partition Scheme → „SensorForge 3.75MB APP / 384KB SPIFFS“.

## 4. Vorher / nachher

Gleicher Quellstand (inkl. Gruppenverwaltung):

| Board | Image | Limit vorher (default_8MB) | frei vorher | Limit nachher (sensorforge) | frei nachher |
|---|---|---|---|---|---|
| XIAO | 3.266.736 B | 3.342.336 B (97,7 %) | 75.600 B | 3.932.160 B (83,1 %) | 665.424 B |
| Freenove | 3.286.480 B | 3.342.336 B (98,3 %) | 55.856 B | 3.932.160 B (83,6 %) | 645.680 B |
| diymore | 3.275.168 B | 3.342.336 B (98,0 %) | 67.168 B | 3.932.160 B (83,3 %) | 656.992 B |

- Die Image-Größen sind Dateigrößen von `sensorforge.ino.bin`. Die IDE-Anzeige „Sketch verwendet“
  ist wenige hundert Byte kleiner.
- Die erzeugte Partitionstabelle ist mit beiden Menüoptionen byte-identisch.

## 5. Folgen für Geräte, OTA, LittleFS und SD

- Für Geräte, die seit dem 16.10. per USB/IDE geflasht wurden, ändert sich nichts: Tabelle,
  LittleFS-Inhalt, NVS (WLAN, Lizenz, Integration-ID, Gruppen) und SD bleiben.
- **Prüfen pro Gerät:** In der Systemübersicht muss die App-Partition **3.932.160 B**
  (3,75 MB) zeigen.
- Ein Gerät, das zuletzt **vor** dem 16.10. per USB geflasht und danach nur per OTA
  aktualisiert wurde, hätte noch die alte Tabelle (3.342.336 B).
  - Ein OTA-Image über 3,2 MB wird dort sauber abgelehnt (Größenprüfung vor
    `Update.begin()`), das Gerät bleibt lauffähig.
  - Der nächste USB-Upload schreibt die neue Tabelle, und dabei geht der interne
    LittleFS-Inhalt verloren (Offset ändert sich). `config.txt` auf SD und NVS bleiben.
  - Vorgehen siehe `PARTITION_UPGRADE_GUIDE.md`.
- Ab einer Firmware > 3.342.336 B kann **kein** Gerät mit alter Tabelle mehr per OTA
  aktualisiert werden. Bis dahin sind alle Geräte über die Systemübersicht zu prüfen.
- Coredump (64 KB) und LittleFS (384 KB) bleiben unverändert. LittleFS hält die interne
  `config.txt`-Kopie, die SD-lose Betriebsfähigkeit und Lizenz- bzw. Fallback-Daten. Eine
  weitere Verkleinerung würde den LittleFS-Inhalt löschen und brächte höchstens ca. 0,2 MB
  pro Slot. Darauf wurde bewusst verzichtet.

## 6. Einschränkungen und künftige Optionen

- Die Menüeinstellung liegt außerhalb des Repos.
  - Nach einem esp32-Core-Update muss `tools/install_ide_partition_scheme.sh` erneut laufen.
  - Wird der Menüpunkt vergessen, ist das gefahrlos: Die IDE ist dann nur strenger als nötig
    (3,2-MB-Limit).
- Der umgekehrte Fehler wäre gefährlich und ist **nicht zulässig**: Menü „Custom“
  (16-MB-Limit) ohne Prüfung. Ein Image größer als der Slot würde in app1 hineinragen.
  `check_firmware_size.py` erkennt das.
- 8-MB-Boards (XIAO, Freenove) sind mit 3,75-MB-Slots am Ende der Partitionierung. Für dauerhaft
  mehr Platz wären Code-Maßnahmen nötig. Größte Hebel:
  - Statische Seiten gzip-komprimiert ausliefern (`Content-Encoding: gzip`), z. B.
    `PLAYER_PAGE` 65 KB.
  - Häufig wiederholte HTML/JS-Bausteine der WebConfig zusammenfassen.
  - Beides ändert die Weboberfläche und ist erst mit eigenem Test sinnvoll.
- diymore hat 16 MB Flash, wird aber bewusst mit der gemeinsamen 8-MB-Tabelle gebaut
  (keine Board-Sonderlösung).
