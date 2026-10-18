# Beta131 - Siebzehnter Korrekturzyklus - Broadcaster Production Beacon Diagnostics

**Datum:** 2026-10-18
**Status:** DIAGNOSIS - Kompakte Diagnose-Counter für precisionUtcNow() Blockade
**Cycle:** 17

---

## PROBLEM-ZUSAMMENFASSUNG

**Hardware-Befund nach Unified Firmware Flash (alle 4 Geräte beta131):**

**devxiao3 (Broadcaster) nach 292 Sekunden:**
- ✅ Erfolgreiche Zeit-Synchronisation: `exchanges_complete=80`, `exchange_timeouts=0`
- ✅ Aktiver Lease: `broadcaster_lease_active=true`, `broadcaster_lease_remaining_ms=1631`
- ✅ Präzisionsschätzung akzeptiert: `precision_recently_accepted=true`
- ✅ Drift valid: `drift_valid=true`
- ✅ Grant/Ack funktioniert: 129 Grants authentifiziert, 129 Acks gesendet
- ❌ **KEINE production beacons:** `broadcaster_production_beacons_sent=0`
- ❌ Serie 19.4s alt: `last_series_estimate_age_ms=19435`
- Aktive Zeitquelle: `active_time_source=6e5bd8e7a95c0236`

**SYMPTOM:**
Broadcaster hat ALLE Voraussetzungen erfüllt (Zeitbasis, Lease, Drift), sendet aber KEINE production beacons!

---

## ROOT CAUSE ANALYSE

### Production Beacon TX-Pfad (cluster_espnow_radio.cpp)

**Bedingung 1 - Line 2316-2317:** `broadcasterTimeBaseValid` wird gesetzt:
```cpp
broadcasterTimeBaseValid = latestQualityEstimate.valid && lastPrecisionMs &&
    (millis() - lastPrecisionMs) <= kPrecisionLifetimeMs;
```

**Bedingung 2 - Line 2354:** Production beacon Guard:
```cpp
if(broadcasterLeaseActive && broadcasterTimeBaseValid) {
```
- `broadcasterLeaseActive=true` ✅ (Hardware bestätigt)
- `broadcasterTimeBaseValid=???` (NICHT im JSON exportiert!)

**Bedingung 3 - Line 2362:** `precisionUtcNow()` erfolgreich:
```cpp
if(precisionUtcNow(epoch, id, clusterUtcUs, clusterAgeMs, clusterRttUs)) {
```

**Bedingung 4 - Line 2389:** Counter-Inkrement:
```cpp
++broadcasterProductionBeaconsSent;
```

**SCHLUSSFOLGERUNG:**
Da `broadcasterProductionBeaconsSent=0`, wurde Line 2389 NIE erreicht!

**Mögliche Ursachen:**
1. **`broadcasterTimeBaseValid=false`** → Line 2354 schlägt fehl
2. **`precisionUtcNow()` gibt `false` zurück** → Line 2362 schlägt fehl

### precisionUtcNow() Guards (Line 1248-1272)

`precisionUtcNow()` hat 10+ Guards die fehlschlagen können:

1. `!initialized` - Radio nicht initialisiert
2. `wasCoordinator` - **VERDÄCHTIG!** Device war Coordinator
3. `epoch != lastEpoch` - Epoch-Mismatch
4. `!coordinatorId` - Keine Coordinator-ID
5. `idTag(coordinatorId) != lastTag` - Tag-Mismatch (Zeitquelle falsch!)
6. `!latestQualityEstimate.valid` - Präzisionsschätzung ungültig
7. `!referenceUtcUs` - Keine Referenz-UTC
8. `!lastPrecisionMs` - Keine Präzisions-Zeit
9. `!referenceReceivedNodeMono` - Keine Referenz-Monotonic
10. `(millis()-lastPrecisionMs)>kPrecisionLifetimeMs` - Präzision abgelaufen (>120s)

**Wahrscheinlichste Ursachen:**

**Hypothese 1: `wasCoordinator=true`**
- `precisionUtcNow()` wurde für NODE-Rolle entwickelt (Line 1244)
- Check verhindert dass Coordinators ihre eigene WiFi-Zeit als "Präzision" verwenden
- **ABER:** Broadcaster ist AUCH Coordinator (wegen Grant-System!)
- Broadcaster ruft `service(allowed, false, ...)` auf → sollte `wasCoordinator=false` setzen
- **UNSICHER** ob zweiter Aufruf mit `coordinator=true` erfolgt

**Hypothese 2: `latestQualityEstimate.valid=false`**
- Source-Switch-Logik (Line 2210) löscht `latestQualityEstimate` bei Quellenwechsel
- Wenn Coordinator weiterhin Beacons sendet WÄHREND Broadcaster aktiv ist:
  - Broadcaster empfängt Coordinator-Beacons
  - Source-Switch-Logik denkt "neue Quelle!"
  - Löscht `latestQualityEstimate` → `broadcasterTimeBaseValid=false`
- **PLAUSIBEL** wenn Coordinator Beacons NICHT unterdrückt

**Hypothese 3: `idTag(coordinatorId) != lastTag`**
- Broadcaster ruft `precisionUtcNow(epoch, id, ...)` mit Coordinator's ID auf
- `lastTag` sollte Coordinator's Tag sein (gesetzt via `service()`)
- **ABER:** Wenn `activeTimeSourceTag` != `lastTag` (Zeitquellen-Inkonsistenz)
- Mismatch würde `precisionUtcNow()` blockieren

**OHNE DIAGNOSE UNMÖGLICH ZU BESTIMMEN!**

---

## LÖSUNG: KOMPAKTE DIAGNOSE-COUNTER

**Strategie:** Statt spekulativen Fix, präzise Diagnose für EINEN Hardware-Test!

### Neue Diagnose-Counter (7 Stück)

#### 1. `broadcasterTimeBaseInvalid` (uint32_t)
- **Inkrementiert:** Line 2402 - Wenn `broadcasterLeaseActive && !broadcasterTimeBaseValid`
- **Bedeutung:** Lease aktiv, aber Zeitbasis ungültig (Bedingung 2 schlägt fehl)
- **Falls >0:** `broadcasterTimeBaseValid=false` ist das Problem!

#### 2. `broadcasterPrecisionFailed` (uint32_t)
- **Inkrementiert:** Line 2394 - Wenn `precisionUtcNow()` `false` zurückgibt
- **Bedeutung:** Zeitbasis gültig, aber `precisionUtcNow()` schlägt fehl (Bedingung 3 schlägt fehl)
- **Falls >0:** Ein Guard in `precisionUtcNow()` blockiert!

#### 3. `precisionGuardWasCoordinator` (uint32_t)
- **Inkrementiert:** Line 1254 - Guard `if(wasCoordinator)`
- **Bedeutung:** Broadcaster wird als Coordinator erkannt
- **Falls >0:** **ROOT CAUSE ist wasCoordinator-Check!**

#### 4. `precisionGuardEpochMismatch` (uint32_t)
- **Inkrementiert:** Line 1258 - Guard `if(epoch != lastEpoch)`
- **Bedeutung:** Epoch-Mismatch zwischen Anforderung und gespeicherter Zeitquelle
- **Falls >0:** Epoch-Inkonsistenz!

#### 5. `precisionGuardTagMismatch` (uint32_t)
- **Inkrementiert:** Line 1262 - Guard `if(idTag(coordinatorId) != lastTag)`
- **Bedeutung:** Coordinator-ID stimmt nicht mit gespeicherter Zeitquelle überein
- **Falls >0:** Zeitquellen-Inkonsistenz (Coordinator vs. Broadcaster Tag!)

#### 6. `precisionGuardNotValid` (uint32_t)
- **Inkrementiert:** Line 1266 - Guard `if(!latestQualityEstimate.valid)`
- **Bedeutung:** Präzisionsschätzung ungültig (gelöscht durch Source-Switch?)
- **Falls >0:** **ROOT CAUSE ist Source-Switch clearing latestQualityEstimate!**

#### 7. `precisionGuardOther` (uint32_t)
- **Inkrementiert:** Line 1250, 1270 - Andere Guards (Initialisierung, Lifetime)
- **Bedeutung:** Basis-Initialisierungs- oder Lifetime-Fehler
- **Falls >0:** Edge-Case-Problem

---

## IMPLEMENTIERUNG

### Dateien geändert

#### 1. cluster_espnow_radio.h (Lines 114-121)
```cpp
// Beta131-DIAGNOSIS (Seventeenth cycle): Production beacon blocking diagnostics
uint32_t broadcasterTimeBaseInvalid=0;  // Line 2342 blocked: !broadcasterTimeBaseValid
uint32_t broadcasterPrecisionFailed=0;  // Line 2350 blocked: precisionUtcNow() returned false
uint32_t precisionGuardWasCoordinator=0;  // precisionUtcNow guard: wasCoordinator
uint32_t precisionGuardEpochMismatch=0;  // precisionUtcNow guard: epoch != lastEpoch
uint32_t precisionGuardTagMismatch=0;  // precisionUtcNow guard: idTag(coordinatorId) != lastTag
uint32_t precisionGuardNotValid=0;  // precisionUtcNow guard: !latestQualityEstimate.valid
uint32_t precisionGuardOther=0;  // precisionUtcNow guard: other conditions
```

#### 2. cluster_espnow_radio.cpp (Lines 169-172)
```cpp
// Beta131-DIAGNOSIS (Seventeenth cycle): Production beacon blocking diagnostics
uint32_t broadcasterTimeBaseInvalid=0,broadcasterPrecisionFailed=0;
uint32_t precisionGuardWasCoordinator=0,precisionGuardEpochMismatch=0;
uint32_t precisionGuardTagMismatch=0,precisionGuardNotValid=0,precisionGuardOther=0;
```

#### 3. cluster_espnow_radio.cpp (Lines 1248-1272)
**VORHER:** Monolithische if-Bedingung (alle Guards zusammen)
```cpp
if (!initialized || wasCoordinator || epoch != lastEpoch ||
    !coordinatorId || idTag(coordinatorId) != lastTag ||
    !latestQualityEstimate.valid || !referenceUtcUs ||
    !lastPrecisionMs || !referenceReceivedNodeMono ||
    (uint32_t)(millis()-lastPrecisionMs)>kPrecisionLifetimeMs) return false;
```

**NACHHER:** Aufgebrochene Guards mit spezifischen Countern
```cpp
// Beta131-DIAGNOSIS (Seventeenth cycle): Detailed guard tracking
if (!initialized || !coordinatorId || !referenceUtcUs || !referenceReceivedNodeMono) {
    ++precisionGuardOther;  // Basic initialization/validity
    return false;
}
if (wasCoordinator) {
    ++precisionGuardWasCoordinator;
    return false;
}
if (epoch != lastEpoch) {
    ++precisionGuardEpochMismatch;
    return false;
}
if (idTag(coordinatorId) != lastTag) {
    ++precisionGuardTagMismatch;
    return false;
}
if (!latestQualityEstimate.valid) {
    ++precisionGuardNotValid;
    return false;
}
if (!lastPrecisionMs || (uint32_t)(millis()-lastPrecisionMs)>kPrecisionLifetimeMs) {
    ++precisionGuardOther;  // Precision lifetime expired
    return false;
}
```

#### 4. cluster_espnow_radio.cpp (Lines 2392-2403)
```cpp
} else {
    // Beta131-DIAGNOSIS (Seventeenth cycle): Track precisionUtcNow() failure
    ++broadcasterPrecisionFailed;
    broadcasterLeaseActive=false;
    broadcasterTimeBaseValid=false;
    Serial.println("[BROADCASTER] Time base LOST - lease revoked");
}
...
} else if(broadcasterLeaseActive && !broadcasterTimeBaseValid) {
    // Beta131-DIAGNOSIS (Seventeenth cycle): Track time base invalid (but lease active)
    ++broadcasterTimeBaseInvalid;
}
```

#### 5. cluster_espnow_radio.cpp (Lines 1752-1760)
Diagnostics-Export in `diagnostics()` Funktion

#### 6. cluster.cpp (Lines 5032-5040)
JSON-Export der neuen Diagnose-Counter

---

## TESTS

**Offline-Tests:**
```bash
./test_lease_state_machine
```

**Ergebnis:**
```
==============================================
Test Results: 13 PASSED, 0 FAILED
==============================================
```

**Test-Coverage:**
- ✅ Normal Handover
- ✅ Seamless Renewal
- ✅ Lost Grant Fallback
- ✅ Lost Ack Timeout
- ✅ Source Switching
- ✅ Delayed Renewal
- ✅ Coordinator Reboot
- ✅ Multiple Lost Renewals
- ✅ Reboot During Renewal
- ✅ Capture Source Switch
- ✅ Radio Restart Invalidation
- ✅ Bootstrap Timing
- ✅ Probe/Reply/Final Epoch Validation

**Keine Regression!** Alle bestehenden Tests bestehen weiterhin.

**Limitierung:**
- ⚠️ Tests simulieren NICHT `precisionUtcNow()` Logik (nur Lease-System)
- ⚠️ Hardware-Test ERFORDERLICH für finale Diagnose

---

## HARDWARE-ERWARTUNG

**Nach Flash von devxiao3 mit Cycle 17:**

### Diagnose-Szenario 1: wasCoordinator-Problem
```json
{
  "broadcaster_time_base_invalid": 0,
  "broadcaster_precision_failed": >0,
  "precision_guard_was_coordinator": >0,  ← ROOT CAUSE!
  "precision_guard_epoch_mismatch": 0,
  "precision_guard_tag_mismatch": 0,
  "precision_guard_not_valid": 0,
  "precision_guard_other": 0
}
```
**Interpretation:** `wasCoordinator=true` blockiert `precisionUtcNow()`
**Fix:** Broadcaster-Exception in `wasCoordinator`-Check (ähnlich Cycle 16 Slot-Exception)

### Diagnose-Szenario 2: Source-Switch-Problem
```json
{
  "broadcaster_time_base_invalid": 0,
  "broadcaster_precision_failed": >0,
  "precision_guard_was_coordinator": 0,
  "precision_guard_epoch_mismatch": 0,
  "precision_guard_tag_mismatch": 0,
  "precision_guard_not_valid": >0,  ← ROOT CAUSE!
  "precision_guard_other": 0
}
```
**Interpretation:** `latestQualityEstimate.valid=false` (durch Source-Switch gelöscht)
**Fix:** Coordinator-Beacon-Suppression prüfen oder Source-Switch-Logik für Broadcaster anpassen

### Diagnose-Szenario 3: Tag-Mismatch-Problem
```json
{
  "broadcaster_time_base_invalid": 0,
  "broadcaster_precision_failed": >0,
  "precision_guard_was_coordinator": 0,
  "precision_guard_epoch_mismatch": 0,
  "precision_guard_tag_mismatch": >0,  ← ROOT CAUSE!
  "precision_guard_not_valid": 0,
  "precision_guard_other": 0
}
```
**Interpretation:** `idTag(coordinatorId) != lastTag` (Zeitquellen-Inkonsistenz)
**Fix:** Broadcaster sollte Coordinator's Tag als `lastTag` haben, nicht eigenen

### Diagnose-Szenario 4: Zeitbasis ungültig
```json
{
  "broadcaster_time_base_invalid": >0,  ← ROOT CAUSE!
  "broadcaster_precision_failed": 0,
  "precision_guard_was_coordinator": 0,
  ...
}
```
**Interpretation:** `broadcasterTimeBaseValid=false` BEVOR `precisionUtcNow()` gerufen wird
**Fix:** `latestQualityEstimate.valid` oder `lastPrecisionMs` Problem (andere Ursache als precisionUtcNow)

---

## SICHERHEIT

**Keine Sicherheitsänderungen!**

- ✅ KEINE Logik-Änderungen (nur Diagnose-Counter)
- ✅ KEINE Guards entfernt oder geschwächt
- ✅ KEINE neuen Funktionen
- ✅ HMAC, Replay, Epoch/Tag/Boot-Validation UNVERÄNDERT
- ✅ Lease-Sicherheit UNVERÄNDERT
- ✅ Broadcaster-Identität UNVERÄNDERT
- ✅ Counter sind read-only (keine Schreibzugriffe via HTTP)

**Risiko:** **MINIMAL** (nur Counter-Inkremente, keine funktionalen Änderungen)

---

## FLASH-EMPFEHLUNG

**KRITISCH: NUR devxiao3 (Broadcaster) flashen!**

1. ✅ **devxiao3 (Broadcaster) MUSS geflasht werden**
   - Aktuell: beta131 Unified + Minimal Diagnostics
   - Update: beta131 Cycle 17 (Diagnose-Counter)
   - **GRUND:** Diagnose-Counter für `precisionUtcNow()` Blockade

2. ❌ **devfreenove1 (Coordinator) NICHT flashen**
   - Aktuell: beta131 Cycle 16 (funktioniert!)
   - Keine Änderungen für Coordinator

3. ❌ **devxiao1, devxiao2 (Nodes) NICHT flashen**
   - Aktuell: beta131 Unified
   - Keine Änderungen für Nodes

**Flash-Prozedur:**
```bash
cd /home/itakka/ESP32_Board/sensorforge
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM0
```

**Nach Flash:**
1. Broadcaster 5 Minuten laufen lassen
2. JSON-Endpoint auslesen:
   ```bash
   curl http://devxiao3.local/cluster/espnow_radio | jq .espnow_time_local
   ```
3. Diagnose-Counter analysieren (siehe Szenarien oben)
4. Basierend auf Counter-Werten: Gezielten Fix implementieren

---

## ZUSAMMENFASSUNG

**Problem:**
- ✅ Broadcaster: Zeitbasis valid, Lease aktiv, Grants funktionieren
- ❌ **KEINE production beacons gesendet** (`broadcasterProductionBeaconsSent=0`)

**Ursache (UNBEKANNT ohne Diagnose):**
- Entweder `broadcasterTimeBaseValid=false` ODER `precisionUtcNow()` schlägt fehl
- `precisionUtcNow()` hat 10+ Guards - welcher blockiert?

**Lösung:**
- ✅ 7 kompakte Diagnose-Counter hinzugefügt
- ✅ Jeder Guard in `precisionUtcNow()` separat instrumentiert
- ✅ KEINE Logik-Änderungen (nur Counter)
- ✅ Ein einziger Hardware-Test identifiziert ROOT CAUSE!

**Tests:**
- ✅ 13/13 PASSED (keine Regression)

**Flash:**
- ✅ NUR devxiao3 (Broadcaster)
- ❌ NICHT devfreenove1, devxiao1, devxiao2

**Erwartung:**
- Diagnose-Counter zeigen EXAKT welcher Guard blockiert
- Gezielter Fix in Cycle 18 basierend auf Diagnose-Befund
- Endlich production beacons vom Broadcaster! 🎯

---

**Nach 17 Korrekturzyklen:** Präzise Diagnose statt Spekulation - der nächste Fix wird der richtige sein!
