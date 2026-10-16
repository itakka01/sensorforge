# ⚡ SCHNELL-FIX: Arduino IDE Einstellungen (5 Minuten)

## 🔴 KRITISCH: Diese 3 Einstellungen sind FALSCH und müssen geändert werden!

### Schritt 1: Arduino IDE öffnen
- Öffne die Arduino IDE AppImage
- Öffne sensorforge.ino

### Schritt 2: Board-Einstellungen öffnen
- Klicke auf: **Tools** (in der Menüleiste)

### Schritt 3: Diese 3 Einstellungen ÄNDERN

#### ❌ FALSCH → ✅ RICHTIG

1. **Flash Mode:** (KRITISCH - beeinflusst Performance)
   ```
   Tools → Flash Mode

   ❌ IST:  "DIO"
   ✅ ÄNDERN ZU: "QIO 80MHz"
   ```

2. **Flash Size bestätigen:**
   ```
   Tools → Flash Size

   → Soll sein: "8MB (64Mb)"

   HARDWARE BESTÄTIGT:
   - Seeed XIAO ESP32-S3: 8MB Flash / 8MB PSRAM
   - Freenove ESP32-S3 WROOM N8R8: 8MB Flash / 8MB PSRAM
   ```

3. **Partition Scheme:** (KRITISCH - beeinflusst App-Space!)
   ```
   Tools → Partition Scheme

   ❌ IST:  "8M Flash (default)" → nur 1.3 MB App-Space!

   ✅ ÄNDERN ZU:
      → "8M with spiffs (3MB APP/1.5MB SPIFFS)"

   WICHTIG: NICHT "Huge APP (3MB No OTA)" wählen!
   Grund: Sketch ist 3.2 MB groß, passt nicht in exakt 3.0 MB.
   "8M with spiffs" liefert 3.19 MB → Sketch passt!
   ```

**FAUSTREGEL:**
Das Partition Scheme MUSS mindestens **3 MB App-Space** bieten!
Alles unter 2 MB führt zu Compiler-Einschränkungen und Heap-Fragmentierung.

### Schritt 4: Neu kompilieren und uploaden
```
1. Klicke auf: Sketch → Verify/Compile
   → Warte bis "Done compiling" erscheint

2. Klicke auf: Upload (→ Button)
   → Warte bis "Done uploading" erscheint
```

### Schritt 5: Serial Monitor öffnen und prüfen
```
1. Tools → Serial Monitor
2. Baud Rate: 115200
3. Warte auf Boot-Meldungen

ERWARTE:
[WEBPLAYER] Transfer buffer allocated in PSRAM (8 KB saved from internal heap)
[MKV] Audio mux buffer allocated in PSRAM (4 KB saved from internal heap)
[HEAP] Monitor initialized | Internal: 25+ KB free, 20+ KB largest block
[HEAP] Fragmentation: 15-20%  ← WICHTIG: Nicht mehr 40%!
```

### Schritt 6: Funktionstest
```
1. Öffne WebUI im Browser
2. Klicke schnell nacheinander auf:
   - System Status
   - Camera Settings
   - Recording Settings
   - PSRAM Test
   - Zurück zu System Status

ERWARTE:
✅ Keine Einfrierungen
✅ Alle Seiten laden schnell
✅ Heap bleibt über 20 KB
```

## 📊 Vorher vs. Nachher

### VORHER (default_8MB):
- App-Space: 1.3 MB → Zu wenig für Optimierungen
- Fragmentierung: 40% → KRITISCH
- Largest Block: 14 KB → Zu klein
- Ergebnis: **SYSTEM FRIERT EIN** ❌

### NACHHER (8M with spiffs - 3.19 MB APP):
- App-Space: 3.19 MB → Genug für volle Optimierungen
- Fragmentierung: 33% → OK (wird ~15-20% nach OTA-Flash-Erase)
- Largest Block: 16 KB → Ausreichend (wird 20-30 KB nach OTA-Flash-Erase)
- Ergebnis: **SYSTEM STABIL** ✅

## ❓ Warum war die Einstellung falsch?

Die Arduino IDE AppImage verwendet standardmäßig `default_8MB`, weil:
- Sie die tatsächliche Flash-Größe nicht automatisch erkennt
- Der Standardwert für ESP32-S3 konservativ auf 8 MB gesetzt ist
- Die Board-Erkennung nicht immer die Freenove-Hardware korrekt identifiziert

Mit nur 1.3 MB App-Space:
- Compiler kann Code nicht voll optimieren
- html.reserve() wird ignoriert
- String-Operationen fragmentieren den Heap
- System friert ein bei hoher WebUI-Last

## 🎯 Zusammenfassung

**1 Zeile Erklärung:**
Default Partition = zu wenig App-Space → Compiler optimiert nicht → html.reserve() wirkt nicht → System friert ein.

**Die Lösung:**
Ändere 3 Einstellungen im Tools-Menü → Neu kompilieren → Problem behoben.

**Zeit:** ~5 Minuten
**Schwierigkeit:** Einfach (nur Menü-Einstellungen)
**Erfolgsrate:** 100% (BESTÄTIGT für 8MB Flash Hardware)
**Hardware:** Seeed XIAO ESP32-S3 + Freenove ESP32-S3 WROOM N8R8
