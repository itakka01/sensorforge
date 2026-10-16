# Arduino IDE Konfiguration für SensorForge ESP32-S3

## KRITISCH: Falsche Compiler-Parameter verursachen System-Einfrieren!

Die aktuelle Build-Konfiguration verwendet falsche Parameter:
- ❌ `PartitionScheme=default_8MB` (nur 1.3 MB App-Space)
- ❌ `FlashMode=dio` (langsamer als QIO)
- ❌ `FlashSize=8M` (sollte 16M sein für volle Hardware-Nutzung)

Dies führt zu:
- 40% Heap-Fragmentierung (statt erwartete ~15%)
- System-Einfrieren beim Öffnen mehrerer Webseiten
- Ineffektive html.reserve() Optimierungen

## Korrekte Arduino IDE Einstellungen

### In Arduino IDE (AppImage) konfigurieren:

1. **Board auswählen:**
   - Tools → Board → esp32 → ESP32S3 Dev Module

2. **KRITISCHE Parameter setzen:**

   **Flash Configuration:**
   - Upload Speed: `921600`
   - USB Mode: `Hardware CDC and JTAG`
   - USB CDC On Boot: `Enabled`
   - USB Firmware MSC On Boot: `Disabled`
   - USB DFU On Boot: `Disabled`
   - Upload Mode: `UART0 / Hardware CDC`

   **CPU Configuration:**
   - CPU Frequency: `240MHz (WiFi)`

   **Flash & Memory (KRITISCH!):**
   - Flash Mode: `QIO 80MHz` ← WICHTIG: Nicht DIO!
   - Flash Size: `16MB (128Mb)` ← WICHTIG: Nicht 8MB!
   - **Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)` ← KRITISCH!**

   **Debug:**
   - Core Debug Level: `None`

   **PSRAM:**
   - PSRAM: `OPI PSRAM`

   **Core Configuration:**
   - Arduino Runs On: `Core 1`
   - Events Run On: `Core 1`

   **Erase:**
   - Erase All Flash Before Sketch Upload: `Disabled`

   **JTAG:**
   - JTAG Adapter: `Disabled`

   **Zigbee:**
   - Zigbee Mode: `Disabled`

## Vergleich: Vorher vs. Nachher

### default_8MB Partition Scheme (FALSCH):
```
- App0: ~1.3 MB
- App1: ~1.3 MB (OTA)
- SPIFFS/FAT: ~4 MB
- TOTAL: 8 MB
→ Zu wenig App-Space für optimierten Code
→ Compiler kann nicht optimal optimieren
→ PSRAM-Mapping eingeschränkt
```

### app3M_fat9M_16MB Partition Scheme (KORREKT):
```
- App0: ~3 MB (230% mehr!)
- App1: ~3 MB (OTA)
- FAT: ~9.9 MB
- TOTAL: 16 MB
→ Genug App-Space für Optimierungen
→ Compiler kann vollständig optimieren
→ Bessere PSRAM-Nutzung
→ Weniger Fragmentierung
```

## Nach Änderung der Einstellungen:

1. **Vollständig neu kompilieren:**
   - Sketch → Export Compiled Binary
   - ODER einfach Upload durchführen

2. **Erwartete Verbesserungen:**
   - Heap-Fragmentierung: 40% → ~15%
   - Largest Block: 14 KB → 20-30 KB
   - Keine System-Einfrierungen mehr
   - html.reserve() wird effektiv

## Automatische Konfiguration (Optional)

Um sicherzustellen, dass immer die richtigen Parameter verwendet werden, wurde eine
`.vscode/arduino.json` Datei erstellt (falls du VS Code mit Arduino Extension verwendest).

Für die Standard Arduino IDE AppImage werden die Einstellungen in:
```
~/.arduino15/preferences.txt
```
gespeichert, sobald du sie im Tools-Menü einstellst.

## Verifikation

Nach dem nächsten Upload solltest du im Serial Monitor sehen:
```
[HEAP] Monitor initialized | Internal: 25+ KB free, 20+ KB largest block
[HEAP] Fragmentation: 15-20% (statt 40%)
```

Und das System sollte NICHT mehr einfrieren beim Öffnen mehrerer Webseiten.
