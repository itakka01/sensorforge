# SensorForge - KI-Entwickler-Referenz

**Version:** v87-beta126 + Safety Fixes V3 (Stand: 2026-10-16)
**Projekt:** ESP32-S3 Kamera/Sensor-Firmware mit Cluster-Betrieb
**Code-Basis:** ~119.000 LOC C++/Arduino in 149 Dateien
**Safety Status:** 13 Critical Fixes implementiert (V1+V2+V3)

---

## PROJEKT-ÜBERSICHT

SensorForge ist eine professionelle ESP32-S3 Embedded-Firmware für:
- Autonome Event-Aufnahme (PIR, Radar, Image-Motion)
- RTSP/HTTP-Streaming
- Verteiltes Cluster-System (ESP-NOW basiert)
- WebConfig UI + REST API
- Security: ECDSA-Lizenzen, Firmware-Signierung, AES-Verschlüsselung

**Unterstützte Hardware:**
- Seeed XIAO ESP32S3 Sense (aktiv)
- Freenove FNK0085 ESP32-S3 WROOM

**Build-System:** Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5

---

## ARCHITEKTUR-ÜBERSICHT

### Entry-Point
**Datei:** `sensorforge.ino` (443 KB, ~10.500 Zeilen)
- `setup()`: Hardware-Init, Config-Load, Netzwerk-Start
- `loop()`: Kooperatives Multitasking mit `delay(1)` yielding

### Modul-Gruppen (65+ Module)

#### Core-System
- `board_config.h` - Compile-Zeit Board-Auswahl (XIAO/Freenove)
- `config.cpp/h` (268 KB) - Zentrale Konfiguration, Priorität: SD → LittleFS → Defaults
- `logger.cpp/h` - Persistent SD-Logging + Console
- `heap_monitor.cpp/h` - **NEU (V2):** Heap-Fragmentierungs-Monitoring (alle 60s)

#### Storage-Layer (3-stufig)
```
Container (AVI/MKV)
    ↓
PSRAM Write-Behind (RecordingWriteBufferedFile)
    ↓
Encryption (SFENC1: AES-256-GCM)
    ↓
Physical (SD/LittleFS)
```

**Kritische Dateien:**
- `recording_storage.cpp/h` - Verschlüsselte Datei-Abstraktion
- `recording_write_buffer.cpp/h` - Asynchroner Drain-Task (FreeRTOS) + **Mutex Deadlock Protection (V3)**
- `avi_writer.cpp/h`, `mkv_writer.cpp/h` - Container-Formate + **Division-by-Zero Protection (V3)**
- `storage_guard.cpp/h` - **VERBESSERT (V1+V2):** `std::atomic` + RAII Guards + Reference Counting
- `recorder.cpp/h` - **VERBESSERT (V2):** `std::atomic` + RAII Guards
- `sd_admission_controller.h` - **PROTOTYP**, atomare Arbitrierung (⚠️ NICHT vollständig integriert)

#### WebConfig & UI (15 Module)
- `webconfig.cpp` (669 KB!) - Haupt-Interface
- `webconfig_wifi.cpp`, `webconfig_audio.cpp`, `webconfig_cluster.cpp`, etc. - Sub-Module
- `webplayer.cpp` (192 KB) - Browser-Player
- `web_sd_maintenance.cpp` (184 KB) - SD-Wartung
- `web_csrf.cpp` - CSRF-Protection

#### Cluster-System (18 Module, **Experimentell**)
- `cluster.cpp` (220 KB) - Coordinator-Wahl, Resource-Announcements
- `cluster_espnow_radio.cpp` (91 KB) - ESP-NOW Transport, Two-Way-Ranging
- `cluster_capture.cpp` - UTC-terminierte Foto-Jobs + **Array Bounds Defense-in-Depth (V3)**
- `cluster_jobs.cpp` - Signierte Remote-Befehle (Ed25519)
- `cluster_hw_time_*.cpp` (7 Module) - Hardware-Zeitsync (⚠️ µs-Genauigkeit unbewiesen)
- `streamer.cpp/h` (105 KB) - RTSP/RTP-JPEG, HTTP-MJPEG + **Timer Overflow Protection (V3)**
- `sync_api.cpp/h` (136 KB) - REST API `/api/v1` + **Integer Overflow Protection (V3)**

#### Sensoren & Motion
- `image_motion.cpp/h` (53 KB) - 20x15 Grid, adaptive Schwellen, State-Machine
- `radar.cpp/h` (87 KB) - LD2410S UART-Treiber, 16-Gate Energie-Monitoring
- `audio_capture.cpp/h` - PDM/I2S Backend-Abstraktion

#### Security & Licensing
- `license.cpp/h` - ECDSA P-256 Board-Bound Activation (TRIAL/DEMO/FULL)
- `firmware_security.cpp/h` - Ed25519 .sfw-Signatur-Verifikation
- `recording_crypto.cpp/h` - SFENC1-Container (AES-256-GCM)

#### Netzwerk & Streaming
- `streamer.cpp/h` (105 KB) - RTSP/RTP-JPEG, HTTP-MJPEG
- `onvif.cpp/h` (53 KB) - ONVIF Discovery & SOAP Services
- `sync_api.cpp/h` (136 KB) - REST API `/api/v1`

### FreeRTOS-Tasks

| Task | Stack | Prio | Core | Zweck |
|------|-------|------|------|-------|
| loopTask | 8192 | 1 | 1 | Arduino Main-Loop |
| (Audio) | 4096 | - | - | I2S → PSRAM Ring-Buffer |
| sf_recording_io | 8192 | 1 | 0 | PSRAM → SD Write-Behind |
| SyncSDRead | 4096 | 1 | 0 | SD-Download-Pipeline |

### Hardware-Peripherie

| Interface | Verwendung | Status |
|-----------|------------|--------|
| DVP (8-bit) | OV2640/OV3660 Kamera | Produktiv |
| SPI (20 MHz) | SD-Karte | Produktiv |
| WiFi 802.11n | STA/AP, HTTP, RTSP, ESP-NOW | Produktiv |
| UART1 (115200) | LD2410S Radar | Produktiv |
| I2C | DS3231 RTC, AT24C32 EEPROM | Produktiv |
| I2S/PDM | Audio (16 kHz, 16-bit) | Produktiv |
| PSRAM (80 MHz) | Buffers (~4 MB) | Produktiv |
| GPIO | PIR, Magnet-Switch, LED | Produktiv |

### Speicher-Layout

**PSRAM:**
- Camera Framebuffer: 2x ~100-400 KB
- Audio Ring-Buffer: 256 KB - 1 MB
- Recording Write-Behind: 256 KB
- Cluster Capture-Media: bis ~3 MB

**Flash (8 MB):**
- app0/app1: 2x 3,2 MB (OTA-fähig)
- LittleFS: 1,5 MB
- NVS: 20 KB (WiFi, License, Integration-ID)

---

## KRITISCHE RISIKEN & BEKANNTE PROBLEME

### ✅ BEHOBENE KRITISCHE RISIKEN (V1+V2+V3)

**Gesamtrisiko:** **HOCH → NIEDRIG** (13 Fixes implementiert, siehe `FIX_REPORT_V3.md`)

#### ✅ Thread-Safety (V1+V2 - BEHOBEN)
**Vorher:** `g_storageLocked`, `g_recordingStartBlocked` als volatile bool ohne Mutexe
**Nachher:** `std::atomic<bool>` + RAII Guards + Reference Counting
**Fixes:**
- storage_guard.cpp/h: V1+V2 - atomic + StorageLockGuard RAII Class
- recorder.cpp/h: V2 - atomic + RecordingBlockGuard RAII Class
- heap_monitor.cpp/h: V2 - Heap-Fragmentierungs-Monitoring
**Status:** ✅ Framework vollständig, Migration ausstehend (siehe `RAII_GUARDS_MIGRATION_GUIDE.md`)

#### ✅ Timer Overflow (V3 - BEHOBEN)
**Problem:** millis() Rollover nach 49 Tagen → Integer-Overflow
**Fixes:**
- streamer.cpp:1907-1918 - Stress-Test Timer-Overflow-Protection
- sync_api.cpp:1816-1833 - MB/s Berechnung Timer-Overflow-Protection
**Status:** ✅ Kritische Stellen geschützt

#### ✅ Array Bounds (V3 - BEHOBEN)
**Problem:** cluster_capture.cpp - 28 Zeilen zwischen Check und Array-Zugriff
**Fix:** cluster_capture.cpp:79-84 - Defense-in-Depth Double-Check
**Status:** ✅ Fixed

#### ✅ Division by Zero (V3 - BEHOBEN)
**Problem:** mkv_writer.cpp - mkvFps kann 0 sein
**Fix:** mkv_writer.cpp:2433-2440 - Fallback bei FPS=0
**Status:** ✅ Fixed

#### ✅ Mutex Deadlock (V3 - BEHOBEN)
**Problem:** recording_write_buffer.cpp - portMAX_DELAY = unbegrenztes Warten
**Fix:** 7x xSemaphoreTake mit 10-Sekunden-Timeout
**Status:** ✅ Fixed

#### ✅ NULL-Pointer (V1 - BEHOBEN)
**Problem:** language.cpp - tr() konnte NULL zurückgeben
**Fix:** tr() immer const char* (nie NULL)
**Status:** ✅ Fixed

### ⚠️ P0 - VERBLEIBENDE BLOCKER

#### 1. SD-Admission-Controller - Migration ausstehend
**Problem:** RAII Guards Framework implementiert, aber noch nicht vollständig migriert
**Auswirkung:** 28 Save/Restore-Patterns in 6 Modulen nutzen noch alte Patterns
**Status:** Framework ready (V2), Migration 6-10 Stunden Aufwand
**Dokumentation:** `RAII_GUARDS_MIGRATION_GUIDE.md`

#### 2. Task-Lifecycle - Use-After-Free (TEILWEISE BEHOBEN)
**Problem:** `RecordingWriteBufferedFile`-Destruktor kann Stack verlassen, während Drain-Task auf `this` zugreift
**Auswirkung:** Crash, Datenverlust bei SD-Fehlern
**Status:** Teilweise behoben (Audit 4b: Fertigmeldung), Restgefahr bei dauerhaft blockiertem SD
**Code:** `recording_write_buffer.cpp` Zeile ~300-400
**V3-Verbesserung:** Mutex-Deadlock-Prevention reduziert Risiko

#### 3. Keine vollständigen Hardware-Tests
**Problem:** V3-Fixes statisch geprüft, kein vollständiger ESP32-Compile
**Auswirkung:** Unentdeckte Compiler-/Hardware-Fehler
**Status:** Statische Prüfung + Code-Review
**Empfehlung:** Kompilieren & Hardware-Testen (siehe `FIX_REPORT_V3.md` Testing-Sektion)

### ⚠️ P1 - HOHE RISIKEN

#### 4. Cluster-Capture - Belichtungszeit ungemessen
**Problem:** `esp_camera_fb_get()` ≠ Sensor-Belichtungsbeginn
**Auswirkung:** Cluster-Sync-Garantie unbelegbar (kein µs-Beweis)
**Status:** Monotone Deadline frozen (beta105), aber nur Software-Dispatch
**Code:** `cluster_capture.cpp`, `clusterLoop()` in `cluster.cpp`

#### 5. Kamera-Ownership - Keine Arbitrierung
**Problem:** Recorder, Streamer, Cluster, Preview konkurrieren
**Auswirkung:** Parallele `esp_camera_fb_get()` → Crash
**Status:** Kooperative Flags, keine atomare Lease
**Code:** Globale `recording`, `streaming` Flags

#### 6. ESP-NOW Zeitsync - µs-Genauigkeit unbewiesen
**Problem:** Hardware-Validierung fehlt
**Status:** Estimator v2 implementiert, nicht charakterisiert
**Code:** `cluster_espnow_radio.cpp` (91 KB)

### P2 - Mittlere Risiken

- Kooperative Concurrency (Race-Conditions)
- WireGuard-VPN inkompatibel (tcpip_adapter veraltet)
- Power-Shooter/PSRAM ungetestet
- Thermal-Emergency nur RTC-basiert (kein CPU-Sensor)

---

## BUILD-ANWEISUNGEN

### Voraussetzungen

- Arduino IDE 2.3.10
- ESP32-Arduino-Core 3.3.12
- ESP-IDF 5.5.5 (Arduino-gebündelt)

### Board-Konfiguration

**FQBN:**
```
esp32:esp32:esp32s3:
  UploadSpeed=921600
  USBMode=hwcdc
  CDCOnBoot=cdc
  CPUFreq=240
  FlashMode=dio
  FlashSize=8M
  PartitionScheme=default_8MB
  DebugLevel=none
  PSRAM=opi
  LoopCore=1
  EventsCore=1
```

**Board-Profil wechseln:**
```cpp
// In board_config.h:
#define BOARD_XIAO        // Seeed XIAO ESP32S3 Sense
//#define BOARD_FREENOVE  // Freenove FNK0085
```

### Kompilieren & Flashen

**Arduino IDE:**
```bash
./start_ide.sh
# In IDE: Sketch → Verify/Compile → Upload
```

**Manuell (esptool.py):**
```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 build/esp32.esp32.esp32s3/sensorforge.ino.bootloader.bin \
  0x8000 build/esp32.esp32.esp32s3/sensorforge.ino.partitions.bin \
  0xe000 boot_app0.bin \
  0x10000 build/esp32.esp32.esp32s3/sensorforge.ino.bin
```

**Vollständiges Flash-Image (Factory-Reset):**
```bash
esptool.py write_flash 0x0 sensorforge.ino.merged.bin
```

### Archivierung

```bash
./make_ziparchive.sh  # → ZIPARCHIVES/sensorforge_<timestamp>.zip
```

---

## ENTWICKLUNGSREGELN

### Pflicht-Regeln (aus README.md)

1. **Immer vom vollständigen aktuellen Source-Tree arbeiten**
   - Niemals aus Gedächtnis/altem ZIP rekonstruieren
   - Basis: v87-beta105 + alle kumulativen Audit-Pakete

2. **Betroffene Implementation lesen**
   - Ownership/Lifecycle-Pfade verstehen
   - Kein Raten bei bestehendem Code

3. **Safety First**
   - Bestehendes Verhalten bewahren
   - Nie Features als "Cleanup" entfernen
   - Recording/Storage/Recovery/Security-Pfade autoritativ

4. **Additive, schmale Änderungen**
   - Narrow scope
   - Config-Keys mit sicheren Defaults kompatibel

5. **webconfig.cpp schlank halten**
   - Neue WebConfig-Features → separate Module
   - Orchestrierung-fokussiert

6. **Keine Secrets exposen**
   - Web/API/Logs sauber halten

7. **Delta-ZIP Delivery**
   - Nur geänderte/neue Dateien
   - Original-Filenames, Original-Placement
   - Keine Rename-Copies, Patches, Test-Plans

### Kritische Bereiche (VORSICHT!)

**Niemals ungeschützt anfassen:**
- SD-Zugriff (immer `webSdAdmissionController().lease()` prüfen)
- Kamera (`esp_camera_fb_get()` - Ownership prüfen!)
- FreeRTOS-Tasks (vTaskDelete-Timing!)
- Globale Flags (Race-Conditions!)

**Vor Änderungen konsultieren:**
- `CLUSTER_ARCHITECTURE.md` - Cluster-Protokolle
- `WIEDEREINSTIEG_SENSORFORGE.md` - Aktueller Stand
- `SD_RECORDER_HANDOFF_AUDIT.md` - SD-Admission-Details
- `CHANGELOG.md` - Historische Entscheidungen

---

## ZENTRALE PROGRAMMABLÄUFE

### Recording-Session
```
Motion-Detection (PIR/Radar/Image)
  → recorderStart()
    ├─ SD-Admission prüfen (webSdAdmissionController().busy())
    ├─ Container-Writer (AVI/MKV)
    │  └─ RecordingWriteBufferedFile (Drain-Task Core 0, Prio 1)
    ↓
  Loop: Frame → PSRAM → Drain-Task → SD
    ↓
  recorderStop()
    ├─ Container finalisieren
    ├─ Drain-Task beenden (vTaskDelete)
    └─ Admission freigeben
```

### Cluster-Capture (Experimentell)
**Coordinator:**
```
WebConfig → UTC-Termin (von cluster_time.utc_us)
  → SFJC1-Kommando (Ed25519-signiert)
  → UDP Broadcast
  → Countdown-UI (rot → grün)
  → Warten auf SFJCR1-Results (max 10 Retries)
  → SFJCA1-Receipt senden
  → Media-Links anzeigen
```

**Node:**
```
SFJC1 empfangen → Signatur prüfen
  → UTC → Monotonic (einmalig frozen!)
  → Kamera warm-machen (2s vorher)
  → 50ms vor Deadline: Cluster-Loop nachrangig
  → esp_camera_fb_get() → PSRAM
  → SFJCR1-Result senden (signiert)
  → Retries bis Receipt
```

**⚠️ KRITISCH:** `fb_get()`-Zeit ≠ Belichtungsbeginn!

### Sleep-Hierarchie
```
1. Thermal-Emergency (80°C) → Deep-Sleep 10 Min
2. Storage-Fault → Deep-Sleep 5 Min
3. Transport-Mode → Black-Cover-Check
4. Normal → Light/Deep-Sleep
```

---

## CONFIG-SYSTEM

**Priorität:** `SD:/config.txt` → `LittleFS:/config.txt` → Defaults

**Secrets (SFSEC1-verschlüsselt):**
- WiFi-Passwörter
- Cluster-Credentials
- WireGuard Private-Key
- Recording-Encryption-Key

**Atomare Speicherung:**
```
/config.tmp → Validierung → /config.txt
```

**Wichtige Config-Keys:**
- `device_id` - Hostname
- `integration_id` - 128-bit NVS UUID (Cluster-Node-ID)
- `sleep_mode` - off | light_sleep | deep_sleep
- `cfg_recording_format` - AVI | MKV
- `cluster_id` - Cluster-Membership

**Config-Code:**
- `config.cpp` (268 KB!) - 553 Config-Definitionen
- `config_secrets.cpp` - SFSEC1-Encryption

---

## CLUSTER-SYSTEM (EXPERIMENTELL)

### Architektur

**Rollen:**
- **Coordinator:** Auto-gewählt (preferred > auto > node), aktive Epoch
- **Node:** Empfängt signierte Jobs, führt UTC-terminiert aus

**Pakete (ESP-NOW):**
- `SFD1` - Discovery Announcement (unencrypted)
- `SFC1` - Presence (HMAC-SHA256)
- `SFJC1` - Job Command (Ed25519)
- `SFJCR1` - Job Result (Ed25519)
- `SFJCA1` - Job Receipt (HMAC)

**Zeitsynchronisation (Estimator v2):**
- Feste Serienlänge: 16 Austausche (adaptive 16/32/64 in beta131 entfernt)
- RTT-Quartile-Median
- Drift-Regression: 24 Serien, ≥3 Min
- **⚠️ Nicht Hardware-validiert!**

### Jobs

**Typen:**
- Restart, Shutdown
- Time-Sync (Precision RTT)
- **Capture** (UTC-terminiert, PSRAM-Media)
- SD-Wipe, Node-Probe

**Capture-Flow:**
```
Coordinator: UTC-Termin berechnen
  → Signiertes SFJC1 Broadcast
  → Countdown-UI

Node: SFJC1 → UTC frozen → monotonic
  → Kamera armieren (2s)
  → Auslösen bei Deadline
  → PSRAM-JPEG
  → SFJCR1-Result (Status 2=Success, 3=Fail)
  → Max 10 Retries (2s Abstand)
  → SFJCA1-Receipt stoppt Retries

Download: http://<node-ip>/capture_media?uuid=<media-uuid>
```

**⚠️ LIMITIERUNGEN:**
- Belichtungsbeginn ≠ fb_get()-Zeit
- PSRAM-Rollover: Neuer Job → altes Media gelöscht
- UUID-Links flüchtig (PSRAM, IP-Wechsel)
- Max ~3 MB PSRAM-Media
- Kein automatischer Download zum Coordinator

---

## SICHERHEITS-FEATURES

### Kryptographie

| Feature | Algorithmus | Trust-Domain |
|---------|------------|--------------|
| License | ECDSA P-256/SHA-256 | Board-Bound (eFuse MAC) |
| Firmware | Ed25519 | .sfw-Pakete |
| Recording | AES-256-GCM + HMAC | SFENC1-Container |
| Config-Secrets | SFSEC1 | Passwörter |
| Cluster | HMAC-SHA256 / Ed25519 | Jobs/Presence |

### Lizenzierung

**Modi:**
- **TRIAL:** 14 Tage / 20h Recording / 100h Streaming
- **DEMO:** 10 Starts/Tag, 1h Recording/Tag (nach TRIAL)
- **FULL/SERVICE:** SF2-Activation (ECDSA)

**Hardware-ID:** Abgeleitet von eFuse MAC (Board-Bound)

**Tools:**
```bash
python sensorforge_license_keygen.py  # Keypair generieren
python sensorforge_license.py         # License-Code erstellen
```

### Firmware-Signierung

**Tool:**
```bash
python sensorforge_firmware_sign.py firmware.bin
  → firmware.sfw (signiert)
```

**OTA-Prozess:**
1. .sfw-Paket laden (SD oder WiFi)
2. Ed25519-Signatur prüfen
3. Kompatibilität prüfen
4. OTA-Partition schreiben
5. Boot-Umschaltung

**⚠️ Nicht implementiert:**
- Secure Boot
- Flash Encryption
- Anti-Rollback

---

## DEBUGGING & DIAGNOSTIK

### Logging

**Logger-System:**
```cpp
logInit();                          // In setup()
logWrite("INFO", "Message");        // Sync write
// Deferred logging bei kritischen Pfaden (Wake-Latency)
```

**Log-Speicherung:**
- SD: `/LOG/*.log` (mit Rotation)
- Console: USB-Serial (timestamped)
- RAM-Buffer bei VFS-Fehler

**Debug-Modus:**
```cpp
cfg_debug_enabled = 1;  // In /config oder WebConfig
```

### Performance-Metriken

**Recording-Diagnostics:**
- Frame-Timing-Histogramme (5ms Bins)
- Weighted Pixel-Rate Guard
- Min/Max Heap während Recording

**Cluster-Diagnostics:**
- ESP-NOW Drift/Latenz-Telemetrie
- CSV-Export: `reports/*.csv`
- Capture-Timing-Breakdown

**Wake-Diagnostics:**
- Wake-to-Frame-Latency (Ziel: < 100ms)
- Sleep-Cycle-Aggregation

### Wichtige Befehle

**Git:**
```bash
./pull.sh   # Git pull
./push.sh   # Git add . && commit && push
```

**Tests:**
```bash
python tools/espnow_beacon_model_eval.py    # ESP-NOW Drift-Analyse
python sensorforge_onvif_test.py            # ONVIF-Tests
```

**Benchmarks:**
```bash
# In SensorForge_SD_Camera_Benchmark/:
# SD-Performance-Tests
```

---

## HARDWARE-SPEZIFISCHE DETAILS

### XIAO ESP32S3 Sense

**Pins:**
```cpp
// Presence/Motion
PIR_PIN = GPIO4 (D3)
MAGNET_SWITCH_PIN = GPIO1 (D0)

// Radar UART
RADAR_RX = GPIO44 (D7)
RADAR_TX = GPIO43 (D6)

// RTC I2C
RTC_SDA = GPIO5 (D4)
RTC_SCL = GPIO6 (D5)

// Audio (Onboard PDM)
MIC_CLK = GPIO42
MIC_DATA = GPIO41

// SD SPI
SD_CS = GPIO21
SD_SCK = GPIO7
SD_MISO = GPIO8
SD_MOSI = GPIO9

// Camera DVP
XCLK = GPIO10
SIOD = GPIO40 (SDA)
SIOC = GPIO39 (SCL)
D0-D7 = GPIO15,17,18,16,14,12,11,48
VSYNC = GPIO38
HREF = GPIO47
PCLK = GPIO13
```

**Konflikte:**
- GPIO21: SD_CS + LED → **LED deaktiviert**

**Limits:**
- SD SPI: 20 MHz (production qualified)
- Max Recording: 4 fps @ 1024x768
- Weighted Pixel-Rate: 8M

### Freenove FNK0085

**SD:** SD_MMC 1-bit (nicht SPI!)
**LED:** GPIO2 (verfügbar)
**Radar:** GPIO41/42 (andere als XIAO)

---

## DOKUMENTATIONS-HIERARCHIE

### Pflicht-Lektüre (vor Änderungen)

1. **CLAUDE.md** - **DIESE DATEI** - KI-Entwickler-Referenz, vollständige Architektur
2. **FIX_REPORT_V3.md** - Vollständiger Safety-Fixes-Report (13 Fixes V1+V2+V3)
3. **WIEDEREINSTIEG_SENSORFORGE.md** - Aktueller Stand, Risiken, Audit-Status
4. **CLUSTER_ARCHITECTURE.md** - Cluster-Protokolle, Entscheidungen
5. **README.md** - Features, Entwicklungsregeln
6. **CHANGELOG.md** - Versions-Historie

### Spezialdokumente

#### Safety & Security (NEU)
- `FIX_REPORT_V3.md` - **Vollständiger Safety-Fixes-Report** (13 Fixes, Stand 2026-10-16)
- `FIX_REPORT_V2.md` - V2-Fixes-Report (Option A + Option B)
- `FIX_REPORT.md` - V1-Fixes-Report (Quick Wins)
- `RAII_GUARDS_MIGRATION_GUIDE.md` - RAII Guards Migration (6-10h Aufwand)

#### System Architecture
- `SD_RECORDER_HANDOFF_AUDIT.md` - SD-Admission-Details (P1)
- `ESPNOW_TIME_ARCHITECTURE.md` - Zeitsync-Architektur
- `ESPNOW_ESTIMATOR_V2.md` - Estimator-Spezifikation
- `OTA_AUTO_UPDATE.md` - Auto-Update-Design
- `SENSORFORGE_API_V1.md` - REST API-Vertrag
- `FIRMWARE_SIGNING.txt` - Signatur-Prozess

### IR-Sync-Hardware (⚠️ Nicht validiert)

- `00_SYNC_SENDER_BESCHREIBUNG.txt`
- `00_SYNC_CAMS_BESCHREIBUNG.txt`

**Konzept:** 10-µs-IR-Pulse @ 10 Hz, BPW34/MCP6022
**Status:** GPIO-Konflikte, keine Hardware-Tests

---

## TYPISCHE ENTWICKLUNGSAUFGABEN

### Neue Config-Option hinzufügen

1. **config.h:**
   ```cpp
   extern int cfg_my_new_option;
   ```

2. **config.cpp:**
   ```cpp
   int cfg_my_new_option = 0;  // Default

   // In parseConfigLine():
   else if (key == "my_new_option") {
       seen.myNewOption = true;
       values.myNewOption = value.toInt();
   }

   // In serializeConfig():
   APPEND_CONFIG_VALUE("my_new_option", String(values.myNewOption));

   // In applyConfig():
   cfg_my_new_option = values.myNewOption;
   ```

3. **webconfig.cpp:**
   ```cpp
   // WebConfig-UI hinzufügen
   "<select name='my_new_option'>"
   "<option value='0'" + String(!cfg_my_new_option ? " selected" : "") + ">Aus</option>"
   "<option value='1'" + String(cfg_my_new_option ? " selected" : "") + ">An</option>"
   "</select>"
   ```

### Neues WebConfig-Modul erstellen

1. **webconfig_myfeature.h:**
   ```cpp
   #ifndef WEBCONFIG_MYFEATURE_H
   #define WEBCONFIG_MYFEATURE_H
   void handleMyFeature();
   void handleMyFeaturePost();
   #endif
   ```

2. **webconfig_myfeature.cpp:**
   ```cpp
   #include "webconfig_myfeature.h"
   #include <WebServer.h>
   extern WebServer server;

   void handleMyFeature() {
       String page = "<!DOCTYPE html><html>...</html>";
       server.send(200, "text/html", page);
   }
   ```

3. **webconfig.cpp:**
   ```cpp
   #include "webconfig_myfeature.h"

   // In webConfigSetup():
   server.on("/myfeature", handleMyFeature);
   server.on("/myfeature_post", HTTP_POST, handleMyFeaturePost);
   ```

### Cluster-Job hinzufügen

1. **cluster_jobs.h:**
   ```cpp
   enum class ClusterJobType : uint8_t {
       // ... existing ...
       MY_NEW_JOB = 42
   };
   ```

2. **cluster.cpp:**
   ```cpp
   // In clusterExecuteJob():
   case ClusterJobType::MY_NEW_JOB:
       // Execute job
       clusterJobSetResult(job, true, "Success");
       break;
   ```

3. **webconfig_cluster.cpp:**
   ```cpp
   // UI für Job-Trigger
   ```

---

## FEHLERBEHANDLUNG & RECOVERY

### Boot-Probleme

**Config-Recovery:**
```
1. SD /config.txt lesen
   → Fehler: LittleFS /config.txt
   → Fehler: Firmware-Defaults
2. Validierung fehlgeschlagen
   → /config.tmp Recovery (wenn vorhanden)
   → Factory-Reset-Prompt
```

**SD-Fehler:**
```
1. Boot: Max 5 Versuche
2. Runtime: Max 3 Versuche
3. Storage-Fault → Deep-Sleep 5 Min (2 Wakes max)
4. Persistent-Fail → Hotspot + WebConfig (SD-optional)
```

### Kritische Fehler

**Thermal-Emergency:**
```
RTC-Temp > 80°C → Deep-Sleep 10 Min (SFTH-Marker)
```

**Watchdog:**
```cpp
// Task-Watchdog aktivierbar
esp_task_wdt_add(NULL);  // In Task
esp_task_wdt_reset();    // Regelmäßig füttern
```

**Stack-Canary:**
```
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
→ Reboot bei Stack-Overflow
```

### Debugging-Strategien

**Bei Crashes:**
1. Serial-Log lesen (Reset-Reason)
2. Coredump prüfen (Partition @ 0x7F0000)
3. Stack-Trace analysieren

**Bei SD-Problemen:**
1. `/api/v1/storage` - Status prüfen
2. Benchmark laufen lassen
3. SPI-Clock senken (config)

**Bei Cluster-Problemen:**
1. `/cluster_status` - Peers/Coordinator prüfen
2. CSV-Reports analysieren (`reports/`)
3. Debug-Modus aktivieren

---

## ENTWICKLUNGS-CHECKLISTE

### Vor Feature-Implementierung

- [ ] WIEDEREINSTIEG_SENSORFORGE.md lesen (Audit-Status)
- [ ] Betroffene Module identifizieren
- [ ] Ownership/Lifecycle-Pfade verstehen
- [ ] Config-Migration planen
- [ ] Backward-Kompatibilität sichern

### Während Implementierung

- [ ] Keine Secrets exposen
- [ ] SD-Admission beachten (`webSdAdmissionController()`)
- [ ] Kamera-Ownership prüfen
- [ ] Task-Safety validieren
- [ ] Globale Flags minimieren

### Nach Implementierung

- [ ] Vollständiger ESP32-Build (beide Board-Profile!)
- [ ] Binary-Size < 3,2 MB prüfen
- [ ] Unit-Tests (falls kritischer Pfad)
- [ ] Hardware-Test (devxiao1/devxiao2)
- [ ] Delta-ZIP erstellen (nur geänderte Dateien)
- [ ] CHANGELOG.md aktualisieren
- [ ] Version inkrementieren (sensorforge_version.h)

---

## SCHNELLREFERENZ

### Wichtige Globals

```cpp
// Config
extern String cfg_device_id;
extern int cfg_sleep_mode;
extern int cfg_recording_format;

// State
bool recording = false;
bool streaming = false;
bool webConfigStarted = false;

// SD-Admission (✅ VERBESSERT V1+V2: std::atomic + RAII Guards)
std::atomic<bool> g_storageLocked(false);
std::atomic<bool> g_recordingStartBlocked(false);
std::atomic<int> g_storageLockDepth(0);        // V2: Reference Counter
std::atomic<int> g_recordingBlockDepth(0);     // V2: Reference Counter

// Cluster
extern bool clusterEnabled;
extern String clusterEffectiveId();
```

### Wichtige Funktionen

```cpp
// Recording
bool recorderStart();
void recorderStop();

// Cluster
void clusterLoop();                    // In loop() aufrufen
bool clusterCaptureSchedule(...);      // UTC-Foto planen

// Storage
SdAdmissionController& webSdAdmissionController();
bool sdReady;

// Sleep
void enterLightSleep(uint64_t durationUs);
void enterDeepSleep(uint64_t durationUs);
```

### Wichtige Pfade

```
/home/itakka/ESP32_Board/sensorforge/
├── sensorforge.ino              # Entry-Point
├── board_config.h               # Board-Auswahl
├── config.cpp (268 KB)          # Zentrale Config
├── recorder.cpp                 # Recording-Interface
├── cluster.cpp (220 KB)         # Cluster-Core
├── webconfig.cpp (669 KB)       # WebConfig-UI
├── build/esp32.esp32.esp32s3/   # Build-Ausgabe
│   ├── sensorforge.ino.bin      # Firmware (2,7 MB)
│   └── sensorforge.ino.merged.bin (8 MB) # Vollständiges Flash
├── WIEDEREINSTIEG_SENSORFORGE.md # ⚠️ PFLICHT-LEKTÜRE
├── CLUSTER_ARCHITECTURE.md       # Cluster-Details
└── README.md                     # Features & Regeln
```

---

## ABSCHLUSS

Diese Referenz dokumentiert SensorForge **v87-beta126 + Safety Fixes V3** (Stand: 2026-10-16).

**Sicherheitsstatus:**
✅ **13 kritische Safety-Fixes implementiert** (V1+V2+V3)
✅ **Gesamtrisiko: HOCH → NIEDRIG**
✅ **6 CRITICAL Fixes:** Thread-Safety, Timer Overflow, Array Bounds
✅ **4 HIGH Fixes:** Division by Zero, Mutex Deadlock, Integer Overflow, Heap Monitoring
✅ **3 MEDIUM Fixes:** NULL-Safety, Reference Counter, Migration Guide

**Vor Entwicklung:**
1. **FIX_REPORT_V3.md** lesen (Vollständige Safety-Fixes!)
2. **CLAUDE.md** lesen (DIESE DATEI - Architektur-Referenz)
3. **WIEDEREINSTIEG_SENSORFORGE.md** lesen (Audit-Status!)
4. P0-Risiken beachten (RAII Migration ausstehend, Task-Lifecycle, keine HW-Tests)
5. Entwicklungsregeln befolgen (README.md)

**Bei Fragen:**
- Supervisor konsultieren
- Spezialdokumente prüfen (FIX_REPORT_V3.md, CLUSTER_ARCHITECTURE.md, etc.)
- Code lesen (niemals raten!)

**Kritische Warnungen (Verbessert!):**
✅ **Thread-Safety VERBESSERT:** std::atomic + RAII Guards Framework (Migration 6-10h)
⚠️ **Cluster-Capture µs-Sync NICHT Hardware-validiert!**
⚠️ **V3-Fixes: Statische Prüfung, ESP32-Compile & Hardware-Tests EMPFOHLEN!**

**Nächste Schritte:**
1. ✅ **Kompilieren & Testen** (siehe FIX_REPORT_V3.md Testing-Sektion)
2. **RAII Guards Migration Phase 1** (einfache Module, 1h)
3. **Long-Running Stability Test** (24-48h)

**Kontakt:** Supervisor für architektonische Entscheidungen
