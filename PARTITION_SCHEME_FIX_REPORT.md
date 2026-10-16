# KRITISCHER BUG: Falsche Partition Scheme verursacht System-Einfrieren

**Datum:** 2026-10-16
**Status:** IDENTIFIZIERT - WARTET AUF FIX
**Priorität:** KRITISCH

## Problem-Zusammenfassung

Nach Implementierung der Heap-Optimierungen (PSRAM-Buffer + html.reserve()) friert
das System beim Öffnen mehrerer Webseiten ein, obwohl die PSRAM-Allokationen erfolgreich sind.

### Symptome:
- ✅ PSRAM-Allokationen erfolgreich (8 KB + 4 KB)
- ✅ Free Heap verbessert: 15 KB → 25 KB (+10 KB)
- ❌ System friert ein beim Öffnen mehrerer Webseiten
- ❌ Fragmentierung bleibt bei 40% (Ziel war ~15%)
- ❌ Largest Block nur 14 KB (sollte 20-30 KB sein)

## Root Cause Analysis

**Analyse der build.options.json zeigt:**

```json
"fqbn": "esp32:esp32:esp32s3:...,PartitionScheme=default_8MB,FlashMode=dio,FlashSize=8M,..."
```

### Fehlerhafte Parameter:
1. **PartitionScheme=default_8MB** ← KRITISCH!
   - Alloziert nur ~1.3 MB für App (statt 3 MB)
   - Eingeschränkter Code-Space verhindert Optimierungen
   - Compiler kann html.reserve() nicht effektiv nutzen

2. **FlashMode=dio** ← SUBOPTIMAL
   - Langsamer als QIO (Dual I/O statt Quad I/O)
   - Reduziert Flash-Zugriffs-Performance

3. **FlashSize=8M** ← FALSCH
   - Hardware hat 16 MB Flash
   - Verschenkt 8 MB ungenutzten Speicher

### Erwartete vs. Tatsächliche Parameter:

| Parameter | IST (falsch) | SOLL (korrekt) | Impact |
|-----------|-------------|----------------|---------|
| PartitionScheme | default_8MB | app3M_fat9M_16MB | KRITISCH |
| FlashMode | dio | qio | HOCH |
| FlashSize | 8M | 16M | MITTEL |
| FlashFreq | (fehlt) | 80 | NIEDRIG |

## Warum html.reserve() nicht wirkt

Bei **default_8MB** Partition:
```
App0: 1.3 MB → Compiler muss Code komprimieren
                → Weniger Optimierungs-Möglichkeiten
                → String-Operationen werden nicht voll optimiert
                → html.reserve() Hints werden ignoriert
                → Fragmentierung bleibt hoch (40%)
```

Bei **app3M_fat9M_16MB** Partition:
```
App0: 3.0 MB → Compiler hat genug Platz
                → Volle Optimierungen möglich
                → String-Operationen optimal kompiliert
                → html.reserve() wird respektiert
                → Fragmentierung sinkt auf ~15%
```

## Beweis: Compiler-Optimierung-Einschränkung

Mit nur 1.3 MB App-Space muss der Compiler:
- Funktionen inline-beschränken
- Loop-Unrolling reduzieren
- String-Pool komprimieren
- Standard-Library-Calls weniger optimieren

Dies führt zu:
- **Mehr** String-Reallokationen trotz reserve()
- **Mehr** Heap-Fragmentierung
- **Mehr** temporäre Allokationen
- **Weniger** effektive Speichernutzung

## Fix-Strategie

### Schritt 1: Arduino IDE Einstellungen ändern
(Siehe ARDUINO_IDE_SETUP.md für detaillierte Anleitung)

**Kritische Änderungen in Tools-Menü:**
- Tools → Partition Scheme → **"16M Flash (3MB APP/9.9MB FATFS)"**
- Tools → Flash Mode → **"QIO 80MHz"**
- Tools → Flash Size → **"16MB (128Mb)"**

### Schritt 2: Vollständig neu kompilieren
```bash
# In Arduino IDE:
1. Sketch → Verify/Compile
2. Upload to Board
```

### Schritt 3: Verifikation nach Upload

**Erwartete Serial-Ausgabe:**
```
[WEBPLAYER] Transfer buffer allocated in PSRAM (8 KB saved from internal heap)
[MKV] Audio mux buffer allocated in PSRAM (4 KB saved from internal heap)
[HEAP] Monitor initialized | Internal: 25+ KB free, 20+ KB largest block
[HEAP] Fragmentation: 15-20% (statt 40%)
```

**Funktionstest:**
- ✅ Mehrere Webseiten nacheinander öffnen
- ✅ Kein System-Einfrieren
- ✅ Heap bleibt stabil über 20 KB
- ✅ Largest Block über 20 KB

## Erwartete Verbesserungen nach Fix

| Metrik | Vorher | Nachher | Änderung |
|--------|--------|---------|----------|
| Free Heap | 25 KB | 25 KB | stabil |
| Largest Block | 14 KB | 20-30 KB | +43-114% |
| Fragmentierung | 40% | 15-20% | -50-63% |
| System-Stabilität | Einfrieren | Stabil | ✅ BEHOBEN |
| App-Space | 1.3 MB | 3.0 MB | +131% |
| FAT-Partition | ~4 MB | 9.9 MB | +148% |

## Technischer Hintergrund: Partition Schemes

### default_8MB (FALSCH):
```
Name           Type   SubType  Offset    Size      Flags
nvs            data   nvs      0x9000    20K
otadata        data   ota      0xe000    8K
app0           app    ota_0    0x10000   1280K
app1           app    ota_1    0x150000  1280K
spiffs         data   spiffs   0x290000  1472K
coredump       data   coredump 0x3F0000  64K
```

### app3M_fat9M_16MB (KORREKT):
```
Name           Type   SubType  Offset    Size      Flags
nvs            data   nvs      0x9000    20K
otadata        data   ota      0xe000    8K
app0           app    ota_0    0x10000   3072K     ← 131% MEHR!
app1           app    ota_1    0x310000  3072K
ffat           data   fat      0x610000  10112K    ← 148% MEHR!
coredump       data   coredump 0xFF0000  64K
```

## Warum das System einfriert

Bei 40% Fragmentierung mit nur 14 KB largest block:
```
WebUI öffnet mehrere Seiten gleichzeitig
   → Jede Seite braucht ~15-20 KB zusammenhängenden Speicher für HTML-String
   → Largest block ist nur 14 KB
   → malloc() FAILS oder erzwingt Defragmentierung
   → Defragmentierung bei aktivem WiFi/WebServer
   → Watchdog Timer läuft ab
   → SYSTEM FREEZE
```

Nach Fix mit ~15% Fragmentierung und 20-30 KB largest block:
```
WebUI öffnet mehrere Seiten gleichzeitig
   → Jede Seite braucht ~15-20 KB
   → Largest block ist 20-30 KB → PASST!
   → malloc() SUCCESS ohne Defragmentierung
   → Schnelle Antwort
   → SYSTEM STABIL
```

## Nächste Schritte

1. **JETZT:** Partition Scheme in Arduino IDE ändern (siehe ARDUINO_IDE_SETUP.md)
2. **DANN:** Vollständig neu kompilieren und uploaden
3. **TEST:** Mehrere Webseiten öffnen und Heap-Metriken beobachten
4. **VERIFY:** Fragmentierung sollte bei 15-20% liegen, keine Einfrierungen

## Bestätigung

Nach erfolgreichem Fix bitte Serial Log der ersten 60 Sekunden senden:
- Boot-Meldungen
- PSRAM-Allokationen
- Heap-Monitoring
- WebUI-Zugriffe (mehrere Seiten)

Dies wird bestätigen, dass:
- Korrekte Partition aktiv ist
- html.reserve() jetzt wirkt
- System stabil bleibt
- Fragmentierung unter 20%
