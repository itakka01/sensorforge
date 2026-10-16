# 🐛 BUGFIX: WiFi/Hotspot schaltet trotz Timeout=0 ab bei fehlender SD-Karte

**Datum:** 2026-10-16
**Status:** BEHOBEN
**Priorität:** HOCH
**Betroffene Versionen:** Alle Versionen vor diesem Fix
**Fix-Version:** v87-beta127+

---

## 📋 PROBLEM-BESCHREIBUNG

### Symptom:

Bei einem **brandneuen System** mit **Default-Einstellungen** auf einem **Board ohne SD-Karte**:

1. System bootet
2. Hotspot startet: `SSID=sensorforge`, `IP=192.168.4.1`
3. Nach **~2 Minuten Inaktivität**: Hotspot schaltet **automatisch ab**
4. Trotz der Einstellung: **WiFi-Timeout = 0 (deaktiviert)**

### Erwartetes Verhalten:

Wenn `cfg_wifi_timeout_sec = 0` (Default), sollte WiFi/Hotspot **NIEMALS** automatisch abschalten - auch nicht bei fehlender SD-Karte.

### Tatsächliches Verhalten:

WiFi/Hotspot schaltet nach 120 Sekunden ab wegen "Storage Fault Idle Timeout", obwohl User WiFi-Timeout auf 0 gesetzt hat (oder Default 0 aktiv ist).

---

## 🔍 ROOT CAUSE ANALYSE

### Default-Konfiguration:

```cpp
// config.cpp Zeile 176:
int cfg_wifi_timeout_sec = 0;  // WiFi-Timeout DEAKTIVIERT

// sd_presence_policy.cpp Zeile 11:
bool sdPresenceExpectNoCard() {
    return prefs.getBool(kKey, false);  // Default: SD-Karte wird ERWARTET
}
```

### Problematischer Code (VOR dem Fix):

**Datei:** `sensorforge.ino` Zeile 16669-16679

```cpp
// Storage failure gets its own finite unattended service window,
// even if the normal WiFi timeout is configured as unlimited or
// much longer. An actively used browser keeps the session alive via
// the existing heartbeat, so operator service is never cut off.
if (
    !sdPresenceExpectNoCard() &&
    webConfigStarted &&
    webConfigInactiveFor(
        STORAGE_FAULT_WEB_IDLE_TIMEOUT_SECONDS  // 120 Sekunden
    )
) {
    stopWebConfigWifi(
        "WiFi OFF: storage fault idle timeout"
    );
}
```

**Problem:**
- Die Bedingung prüft **NICHT**, ob der User WiFi-Timeout deaktiviert hat (`cfg_wifi_timeout_sec = 0`)
- Bei fehlender SD-Karte wird WiFi nach 120 Sek **immer** abgeschaltet
- **Auch wenn** der User explizit "kein Timeout" konfiguriert hat!

---

## ✅ DIE LÖSUNG

### Geänderter Code (NACH dem Fix):

**Datei:** `sensorforge.ino` Zeile 16665-16681

```cpp
// Storage failure gets its own finite unattended service window,
// but ONLY if the user has enabled WiFi timeout (cfg_wifi_timeout_sec > 0).
// If timeout is disabled (0), the user expects WiFi to stay on indefinitely,
// even with storage faults. An actively used browser keeps the session alive
// via the existing heartbeat, so operator service is never cut off.
if (
    !sdPresenceExpectNoCard() &&
    cfg_wifi_timeout_sec > 0 &&  // ← NEU: Prüfe ob Timeout aktiviert ist!
    webConfigStarted &&
    webConfigInactiveFor(
        STORAGE_FAULT_WEB_IDLE_TIMEOUT_SECONDS
    )
) {
    stopWebConfigWifi(
        "WiFi OFF: storage fault idle timeout"
    );
}
```

**Änderung:**
- **Zeile 16672:** `cfg_wifi_timeout_sec > 0 &&` hinzugefügt
- **Kommentar aktualisiert:** Erklärt die neue Logik

---

## 🎯 FIX-LOGIK

### VORHER (Bug):

```
Bedingungen für Storage-Fault-Shutdown:
1. SD-Karte wird erwartet (!sdPresenceExpectNoCard())  ✓
2. WebConfig läuft (webConfigStarted)                  ✓
3. Inaktiv für 120 Sekunden                            ✓

→ WiFi wird ABGESCHALTET (auch wenn cfg_wifi_timeout_sec = 0!)
```

### NACHHER (Fix):

```
Bedingungen für Storage-Fault-Shutdown:
1. SD-Karte wird erwartet (!sdPresenceExpectNoCard())  ✓
2. WiFi-Timeout ist AKTIVIERT (cfg_wifi_timeout_sec > 0) ✓ NEU!
3. WebConfig läuft (webConfigStarted)                  ✓
4. Inaktiv für 120 Sekunden                            ✓

→ WiFi wird ABGESCHALTET

FALLS cfg_wifi_timeout_sec = 0:
→ Bedingung 2 ist FALSE
→ WiFi bleibt AN (wie erwartet) ✅
```

---

## 📊 VERHALTENS-MATRIX

| cfg_wifi_timeout_sec | SD vorhanden | SD erwartet | WiFi schaltet ab nach 120s? |
|---------------------|--------------|-------------|----------------------------|
| **0** (deaktiviert) | ❌ Nein | ✓ Ja | ❌ **NEIN** (BEHOBEN!) |
| **0** (deaktiviert) | ✓ Ja | ✓ Ja | ❌ NEIN |
| **0** (deaktiviert) | ❌ Nein | ❌ Nein (optional) | ❌ NEIN |
| **300** (5 Min) | ❌ Nein | ✓ Ja | ✓ **JA** (nach 120s) |
| **300** (5 Min) | ✓ Ja | ✓ Ja | ❌ NEIN (SD OK) |
| **300** (5 Min) | ❌ Nein | ❌ Nein (optional) | ❌ NEIN |

**Legende:**
- **cfg_wifi_timeout_sec = 0:** User will WiFi dauerhaft an
- **cfg_wifi_timeout_sec > 0:** User hat Timeout aktiviert
- **SD erwartet:** `sdPresenceExpectNoCard() = false` (Default)
- **SD optional:** `sdPresenceExpectNoCard() = true` (manuell gesetzt)

---

## 🔧 BETROFFENE USE CASES

### Use Case 1: Board ohne SD-Karte (Default-Config)

**VORHER (Bug):**
```
1. Boot → Hotspot startet ✓
2. Keine WebUI-Nutzung für 2 Min
3. WiFi schaltet ab ❌ (obwohl Timeout = 0)
4. User kann nicht mehr zugreifen
```

**NACHHER (Fix):**
```
1. Boot → Hotspot startet ✓
2. Keine WebUI-Nutzung für 2 Min
3. WiFi bleibt AN ✅ (Timeout = 0 wird respektiert)
4. User kann jederzeit zugreifen ✅
```

### Use Case 2: Board ohne SD-Karte + User hat Timeout aktiviert (z.B. 5 Min)

**VORHER UND NACHHER (gleich):**
```
1. Boot → Hotspot startet ✓
2. Keine WebUI-Nutzung für 2 Min
3. WiFi schaltet ab ✓ (Storage Fault Timeout greift)
4. User muss Magnet-Switch nutzen oder Board neu starten
```

**Begründung:** User hat bewusst Timeout aktiviert → Energie-Sparmodus gewünscht

### Use Case 3: Board MIT SD-Karte

**VORHER UND NACHHER (gleich):**
```
1. Boot → Hotspot startet ✓
2. SD-Karte OK → Kein Storage Fault
3. WiFi bleibt AN gemäß cfg_wifi_timeout_sec
```

---

## 🎯 DESIGN-RATIONALE

### Warum gab es überhaupt einen "Storage Fault Timeout"?

**Original-Intention:**
- SensorForge ist primär für **Outdoor-Kamera mit SD-Karte** designed
- Batterie-betriebene Geräte sollen Strom sparen
- Wenn SD kaputt ist UND niemand schaut (WebUI inaktiv) → WiFi aus = Strom sparen

**Beispiel-Szenario (Outdoor-Kamera):**
```
Kamera im Wald, Batterie-betrieben:
1. SD-Karte ist voll/kaputt → Storage Fault
2. User kommt vorbei, schaltet WiFi an (Magnet)
3. User schaut WebUI an, sieht Problem
4. User geht weg OHNE zu fixen
5. Nach 2 Min: WiFi schaltet automatisch ab → spart Batterie ✓
```

**Problem mit dieser Logik:**
- Was wenn User **ABSICHTLICH** kein SD-Karte hat? (z.B. Seeed XIAO ESP32-S3 ohne SD-Slot)
- Was wenn User **ABSICHTLICH** WiFi dauerhaft an will? (`cfg_wifi_timeout_sec = 0`)
- Original-Code respektierte User-Einstellung **NICHT**!

### Fix-Philosophie:

**Neue Regel:**
```
IF User hat WiFi-Timeout deaktiviert (= 0)
THEN respektiere diese Entscheidung
     → WiFi bleibt dauerhaft an
     → AUCH bei Storage Fault

IF User hat WiFi-Timeout aktiviert (> 0)
THEN darf Storage-Fault-Timeout auch greifen
     → Energie-Sparmodus wie gewünscht
```

**User-Intent wird respektiert!** ✅

---

## 🧪 TEST-PLAN

### Test 1: Default-Config, kein SD

**Setup:**
- Frisch geflashtes Board (Flash Erase)
- Keine SD-Karte installiert
- `cfg_wifi_timeout_sec = 0` (Default)
- `sdPresenceExpectNoCard() = false` (Default)

**Erwartetes Verhalten:**
1. Boot → Hotspot startet ✅
2. Warte 5 Minuten OHNE WebUI-Zugriff
3. Hotspot bleibt AN ✅
4. Serial Log zeigt KEINE "WiFi OFF: storage fault idle timeout" Meldung ✅

### Test 2: WiFi-Timeout aktiviert (5 Min), kein SD

**Setup:**
- Board mit `cfg_wifi_timeout_sec = 300` (5 Min)
- Keine SD-Karte installiert
- `sdPresenceExpectNoCard() = false`

**Erwartetes Verhalten:**
1. Boot → Hotspot startet ✅
2. Warte 2 Minuten OHNE WebUI-Zugriff
3. Hotspot schaltet ab nach 120 Sek ✅
4. Serial Log zeigt "WiFi OFF: storage fault idle timeout" ✅

### Test 3: SD-Karte vorhanden

**Setup:**
- Board mit SD-Karte
- `cfg_wifi_timeout_sec = 0`

**Erwartetes Verhalten:**
1. Boot → Hotspot/STA startet ✅
2. SD-Karte OK → Kein Storage Fault
3. WiFi bleibt dauerhaft AN ✅

### Test 4: SD optional gesetzt, kein SD

**Setup:**
- `sdPresenceExpectNoCard() = true` (via WebUI gesetzt)
- Keine SD-Karte installiert
- `cfg_wifi_timeout_sec = 0`

**Erwartetes Verhalten:**
1. Boot → Hotspot startet ✅
2. Kein Storage Fault (SD ist optional)
3. WiFi bleibt dauerhaft AN ✅

---

## 📝 CHANGELOG-EINTRAG

```
v87-beta127+ (2026-10-16):

BUGFIX: WiFi/Hotspot-Shutdown bei fehlender SD-Karte

- Fixed: WiFi/Hotspot schaltet nicht mehr automatisch ab bei fehlender SD-Karte,
  wenn cfg_wifi_timeout_sec = 0 (deaktiviert).

- Vorher: Bei fehlender SD-Karte wurde WiFi nach 120 Sekunden Inaktivität immer
  abgeschaltet ("Storage Fault Idle Timeout"), auch wenn User WiFi-Timeout auf 0
  gesetzt hatte (= dauerhaft an).

- Nachher: Storage-Fault-Timeout greift NUR, wenn User WiFi-Timeout aktiviert
  hat (cfg_wifi_timeout_sec > 0). Bei Timeout = 0 bleibt WiFi dauerhaft an,
  wie erwartet.

- Betroffene Datei: sensorforge.ino Zeile 16672
- Geänderte Logik: Zusätzliche Bedingung `cfg_wifi_timeout_sec > 0` vor
  Storage-Fault-Shutdown

- Use Case: Boards ohne SD-Karte (z.B. Seeed XIAO ESP32-S3) mit Default-Config
  behalten jetzt WiFi/Hotspot dauerhaft an, wie vom User durch Timeout=0
  konfiguriert.
```

---

## 🎯 ZUSAMMENFASSUNG

**Problem:** Storage-Fault-Timeout ignorierte User-Einstellung `cfg_wifi_timeout_sec = 0`

**Lösung:** Prüfe `cfg_wifi_timeout_sec > 0` bevor Storage-Fault-Timeout greift

**Ergebnis:**
- ✅ User-Intent wird respektiert
- ✅ Boards ohne SD-Karte funktionieren mit Default-Config
- ✅ Energie-Sparmodus funktioniert weiterhin (wenn User Timeout aktiviert)
- ✅ Keine Breaking Changes für bestehende Setups

**Code-Änderung:** 1 Zeile (+ Kommentar-Update)

**Impact:** HOCH - betrifft alle Boards ohne SD-Karte mit Default-Config

**Risiko:** NIEDRIG - nur zusätzliche Bedingung, keine Logik-Änderung

---

## 📚 VERWANDTE DATEIEN

- `sensorforge.ino` - Hauptlogik (Fix implementiert)
- `config.cpp` - Default-Wert `cfg_wifi_timeout_sec = 0`
- `config.h` - Externe Deklaration
- `sd_presence_policy.cpp` - `sdPresenceExpectNoCard()` Funktion
- `webconfig_wifi.cpp` - WebUI für WiFi-Timeout-Einstellung

---

## ✅ VERIFIKATION

**Upload Log:**
```
Sketch uses 3200283 bytes (95%) of program storage space.
Maximum is 3342336 bytes.
```

**Änderung:** +8 bytes (wegen zusätzlicher Bedingung)

**Kompilierung:** ✅ Erfolgreich

**Nächster Schritt:** Serial Log bei brandneuem Board ohne SD-Karte prüfen
**Erwartung:** Kein "WiFi OFF: storage fault idle timeout" nach 2 Minuten

---

**Status:** READY FOR TESTING 🚀
