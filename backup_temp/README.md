README.md
ESP32-S3 Kamera-System

Überblick
Dieses Projekt ist ein motion-getriggertes Kamera-System für ESP32-S3 Boards.
Es nimmt MJPEG-AVI-Videos auf, speichert sie auf SD-Karte und bietet ein Webinterface zur Konfiguration und Diagnose.
Unterstützte Boards:
Freenove ESP32-S3 WROOM Kamera-Board
Seeed Studio XIAO ESP32S3 Sense OV3660
Die Umschaltung erfolgt über platformio.ini.

Funktionen
Bewegungserkennung über PIR
Automatische Aufnahme als MJPEG-AVI
Snapshot-Funktion
LED-Anzeige während Aufnahme
Logdatei auf SD
Webinterface zur Konfiguration
SD-Status und SD-Formatierung
AVI-Dateibrowser und Download
System-Info
Board-Info
PSRAM-Test
SD-Benchmark
Reboot-Funktion

Projektstruktur
include    Header-Dateien
lib        Module camera, logger, webconfig, avi_writer, config
src/main.cpp    Hauptprogramm
config.txt      Konfiguration auf SD
platformio.ini  Board-Auswahl

config.txt auf SD-Karte
Beispiel:

camera=OV3660
resolution=640x480
fps=15
quality=10
post_record_ms=5000
led_enabled=1
wifi_mode=on_missing_time
wifi_ssid=DEIN_WLAN
wifi_pass=PASSWORT
debug_enabled=1
log_file=/log.txt

Änderungen werden erst nach Neustart aktiv.

Board-Auswahl in platformio.ini
Zwei Environments:

env:freenove
board esp32-s3-devkitc-1
build_flags -DBOARD_FREENOVE

env:xiao
board seeed_xiao_esp32s3
build_flags -DBOARD_XIAO

Build und Upload:

Freenove:
pio run -e freenove
pio run -e freenove -t upload

XIAO:
pio run -e xiao
pio run -e xiao -t upload

VSCode: Environment auswählen, Build, Upload.

Flash-Anleitung
Board per USB-C anschließen
Projekt in VSCode öffnen
Passendes Environment auswählen
Build ausführen
Upload ausführen
Seriellen Monitor öffnen mit 115200 Baud
SD-Karte einlegen, FAT32, config.txt vorhanden
PIR auslösen, Aufnahme startet
IP-Adresse im seriellen Monitor ablesen
Webinterface im Browser öffnen mit http://IP

Bedienung
PIR löst Aufnahme aus
Aufnahme stoppt nach post_record_ms
AVI-Dateien liegen auf SD
Logdatei liegt auf SD
Webinterface zeigt Diagnose und Tools
Reboot über Webinterface möglich

Fehlerbehebung
Kamera schwarz:
Falsches Environment
Falsche Kamera-Pins
PSRAM nicht aktiv

SD-Fehler:
Falscher CS-Pin
SD-Karte defekt
Formatierung nötig

Webinterface nicht erreichbar:
WLAN-Daten falsch
wifi_mode=never gesetzt

Aufnahme stoppt sofort:
post_record_ms zu niedrig

Hinweise
SD-Karte muss FAT32 sein
config.txt muss im Root liegen
Board-spezifische Pins werden automatisch gesetzt
Webinterface ist nur aktiv, wenn WLAN verbunden ist

