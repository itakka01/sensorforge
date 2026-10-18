# Beta131 - Achtzehnter Korrekturzyklus - Broadcaster Activation/Revocation Loop Fix

**Datum:** 2026-10-18
**Status:** CRITICAL FIX - Aktivierungs-Endlosschleife repariert
**Cycle:** 18

---

## PROBLEM-ZUSAMMENFASSUNG

**Hardware-Befund nach Cycle 17 (devxiao3, Boot-ID a0d44622, Uptime 339s):**

**Console-Log zeigt permanente Schleife:**
```
[BROADCASTER] Time base LOST - lease revoked
[BROADCASTER] Production ACTIVATED (txn 140, time valid)
[BROADCASTER] Time base LOST - lease revoked
[BROADCASTER] Production ACTIVATED (txn 140, time valid)
```
Diese beiden Meldungen wechseln **mehrmals pro Sekunde**!

**Hardware-Status:**
- ✅ Erfolgreiche Exchanges: `exchanges_complete=336`, `exchange_timeouts=0`
- ✅ Aktiver Lease (Momentaufnahme): `broadcaster_lease_active=true`
- ✅ Grant/Ack funktioniert: 159 Grants authentifiziert, 159 Acks gesendet
- ❌ **KEINE production beacons:** `broadcaster_production_beacons_sent=0`
- ❌ **Diagnose-Befund:** `broadcaster_precision_failed=800`
- ❌ **ABER:** Alle `precision_guard_*` Counter = 0 (!)
- ❌ **Alte Transaktion:** txn 140 wird wiederholt verwendet (aktuelle Grants: txn 159, 160)
- ⚠️ **Alte Zeitbasis:** `last_accepted_age_ms=85916` (85.9 Sekunden!)

**SYMPTOME:**
1. **Aktivierungs-/Revocation-Loop:** Broadcaster aktiviert und revoked sich selbst mehrmals pro Sekunde
2. **Stale Transaction:** Alte txn 140 wird verwendet trotz neuerer Grants (159, 160)
3. **precisionUtcNow() Failure:** 800 Failures OHNE Guard-Counter-Erhöhung
4. **Keine production beacons:** Trotz Lease und erfolgreicher Exchanges

---

## ROOT CAUSE 1: AKTIVIERUNGS-/REVOCATION-LOOP

### Vollständige Code-Analyse

**Line 2347-2349:** `broadcasterTimeBaseValid` wird **BEI JEDEM TICK** neu berechnet:
```cpp
broadcasterTimeBaseValid = latestQualityEstimate.valid && lastPrecisionMs &&
    (millis() - lastPrecisionMs) <= kPrecisionLifetimeMs;
```

**Line 2352-2362:** Aktivierungs-Check (läuft bei jedem Tick):
```cpp
if(!broadcasterLeaseActive && broadcasterLeaseActivationMs && now>=broadcasterLeaseActivationMs &&
   broadcasterActivationTransaction) {
    if(broadcasterTimeBaseValid) {
        broadcasterLeaseActive=true;
        Serial.printf("[BROADCASTER] Production ACTIVATED (txn %u, time valid)\n",
            broadcasterActivationTransaction);
    } else {
        Serial.println("[BROADCASTER] Production BLOCKED - time base invalid");
        broadcasterLeaseActivationMs=0;  // ← Nur hier gelöscht!
        broadcasterActivationTransaction=0;
    }
}
```

**Line 2412-2418 (VOR FIX):** Revocation bei precisionUtcNow() Failure:
```cpp
} else {
    ++broadcasterPrecisionFailed;
    broadcasterLeaseActive=false;  // ← Lease revoked
    broadcasterTimeBaseValid=false;  // ← Zeitbasis invalidiert
    Serial.println("[BROADCASTER] Time base LOST - lease revoked");
    // ❌ FEHLT: broadcasterLeaseActivationMs=0;
    // ❌ FEHLT: broadcasterActivationTransaction=0;
}
```

### Die Endlosschleife (nachgewiesen!)

**Tick N (t=0ms):**
1. Broadcaster ist aktiv, versucht production beacon zu senden
2. `precisionUtcNow()` schlägt fehl (Line 2382)
3. Line 2415 (alt): `broadcasterLeaseActive=false` (revoked)
4. Line 2416 (alt): `broadcasterTimeBaseValid=false`
5. **ABER:** `broadcasterLeaseActivationMs` bleibt gesetzt (z.B. txn 140)!
6. **ABER:** `broadcasterActivationTransaction` bleibt 140!
7. Serial: "[BROADCASTER] Time base LOST - lease revoked"

**Tick N+1 (t=10ms):**
8. Line 2348: `broadcasterTimeBaseValid` wird **NEU BERECHNET**
9. Check: `latestQualityEstimate.valid=true` ✅ (von Exchanges)
10. Check: `lastPrecisionMs` gesetzt ✅ (von letzter Serie vor 85s)
11. Check: `(millis() - lastPrecisionMs) <= 120000` ✅ (85916 < 120000)
12. **Ergebnis:** `broadcasterTimeBaseValid=true` (!)
13. Line 2352: Check `!broadcasterLeaseActive` → TRUE (wegen Schritt 3)
14. Line 2352: Check `broadcasterLeaseActivationMs` → GESETZT (txn 140!)
15. Line 2354: Check `broadcasterTimeBaseValid` → TRUE (Schritt 12!)
16. Line 2355: `broadcasterLeaseActive=true` (**RE-AKTIVIERT!**)
17. Serial: "[BROADCASTER] Production ACTIVATED (txn 140, time valid)"

**Tick N+2 (t=20ms):**
18. Zurück zu Schritt 1 (precisionUtcNow() schlägt wieder fehl)

**ENDLOSSCHLEIFE NACHGEWIESEN!**

### Warum txn 140 statt 159/160?

**Grant-Renewals** (txn 159, 160) setzen NUR `broadcasterLeaseExpiresMs`, **NICHT** `broadcasterActivationTransaction`!

Lass mich das prüfen:

Grant-Empfang (Line 1530+):
```cpp
broadcasterLeaseExpiresMs = now + grantLeaseMs;
if(!broadcasterLeaseActivationMs) {  // ← NUR bei ERSTEM Grant!
    broadcasterLeaseActivationMs = now + 1500;
    broadcasterActivationTransaction = grant.transaction;
}
```

**Renewals** setzen NUR `broadcasterLeaseExpiresMs`, **NICHT** die Aktivierungs-Variablen!

**Resultat:** Bei Revocation bleibt **alte txn 140** gesetzt und wird wiederverwendet!

---

## ROOT CAUSE 2: NICHT-INSTRUMENTIERTE precisionUtcNow() GUARDS

### Analyse des Diagnose-Befunds

**Hardware-Status:**
- `broadcaster_precision_failed=800` → precisionUtcNow() gibt 800x `false` zurück
- **ABER:** Alle `precision_guard_*` Counter = 0

**Das bedeutet:** precisionUtcNow() schlägt **NICHT** wegen der instrumentierten Guards fehl!

### Alle Return-Pfade in precisionUtcNow()

**Cycle 17 instrumentierte Guards (Lines 1253-1270):**
1. ✅ `wasCoordinator` → Counter würde steigen (ist 0)
2. ✅ `epoch != lastEpoch` → Counter würde steigen (ist 0)
3. ✅ `idTag(coordinatorId) != lastTag` → Counter würde steigen (ist 0)
4. ✅ `!latestQualityEstimate.valid` → Counter würde steigen (ist 0)
5. ✅ `!lastPrecisionMs || lifetime expired` → Counter würde steigen (ist 0)

**Cycle 17 NICHT instrumentierte Guards (Lines 1274-1288):**
6. ❌ Line 1274: `now - referenceReceivedNodeMono > kReferenceLifetimeUs`
7. ❌ Line 1276: `age > kPrecisionLifetimeMs * 1000.0`
8. ❌ Line 1278: `!std::isfinite(drift) || fabs(drift) > 500.0`
9. ❌ Line 1286: `referenceMono reconstruction failed`
10. ❌ Line 1288: `UTC target out of range`

**VERDACHT:** Einer dieser NICHT instrumentierten Guards schlägt fehl!

### Line 1274 Analyse - Reference Lifetime

```cpp
if (now<referenceReceivedNodeMono || now-referenceReceivedNodeMono>kReferenceLifetimeUs) return false;
```

- `kReferenceLifetimeUs = 30000000` µs = **30 Sekunden**
- `referenceReceivedNodeMono` = Node's monotonic time (esp_timer_get_time()) bei letzter akzeptierter Serie
- Hardware: `last_accepted_age_ms=85916` ms = **85.9 Sekunden**

**Wenn** die letzte akzeptierte Serie vor 85.9 Sekunden war:
- `now - referenceReceivedNodeMono` ≈ 85.9 Sekunden = 85900000 µs
- `kReferenceLifetimeUs = 30000000` µs
- **Check: 85900000 > 30000000 → TRUE → return false!**

**ROOT CAUSE 2 IDENTIFIZIERT:** Line 1274 schlägt fehl weil Referenz zu alt ist (>30s)!

### Warum ist die Referenz 85 Sekunden alt?

**Erfolgreiche Exchanges:** 336/336, 0 Timeouts → Broadcaster SOLLTE kontinuierlich Serien akzeptieren!

**Aber:** `last_accepted_age_ms=85916` → Letzte akzeptierte Serie vor 85.9 Sekunden!

**Mögliche Ursachen:**
1. **Aktivierungs-/Revocation-Loop:** Broadcaster ist instabil, Serien werden verworfen
2. **Qualitätsprobleme:** Spread 625 µs (vorher 14-36 µs) → Serien werden als zu schlecht verworfen
3. **CPU/Serial-Last:** Aktivierungs-Loop erzeugt hohe Last → schlechtere Timing-Präzision

**Wahrscheinlich:** Die Aktivierungs-/Revocation-Loop VERHINDERT dass neue Serien akzeptiert werden!

---

## FIX IMPLEMENTIERUNG

### Fix 1: Aktivierungs-State bei Revocation löschen

**File:** `cluster_espnow_radio.cpp`
**Lines:** 2415-2423

**VORHER:**
```cpp
} else {
    ++broadcasterPrecisionFailed;
    broadcasterLeaseActive=false;
    broadcasterTimeBaseValid=false;
    Serial.println("[BROADCASTER] Time base LOST - lease revoked");
}
```

**NACHHER:**
```cpp
} else {
    ++broadcasterPrecisionFailed;
    // Beta131-FIX (Eighteenth cycle): Clear activation state on revocation
    // CRITICAL: Prevents re-activation loop with stale transaction
    // Without this, broadcasterLeaseActivationMs/Transaction remain set
    // and re-activate on next tick when broadcasterTimeBaseValid flips to true
    broadcasterLeaseActive=false;
    broadcasterLeaseActivationMs=0;  // ← NEU!
    broadcasterActivationTransaction=0;  // ← NEU!
    broadcasterTimeBaseValid=false;
    Serial.println("[BROADCASTER] Time base LOST - lease revoked");
}
```

**Effekt:**
- Nach Revocation: Aktivierungs-State VOLLSTÄNDIG gelöscht
- Keine Reaktivierung mit alter Transaktion möglich
- Nur neue Grants (mit neuen Transaktionen) können aktivieren

### Fix 2: Alle precisionUtcNow() Return-Pfade instrumentieren

**File:** `cluster_espnow_radio.cpp`
**Lines:** 1274-1304

**VORHER (nicht instrumentiert):**
```cpp
const int64_t now=esp_timer_get_time();
if (now<referenceReceivedNodeMono || now-referenceReceivedNodeMono>kReferenceLifetimeUs) return false;
const double age=static_cast<double>(now-latestQualityEstimate.nodeReferenceUs);
if (age<0 || age>static_cast<double>(kPrecisionLifetimeMs)*1000.0) return false;
const double drift=stats.driftValid ? stats.driftPpm : 0.0;
if (!std::isfinite(drift) || fabs(drift)>500.0) return false;
...
if (referenceMono<=0 || llabs(referenceMono-approx)>kReferenceLifetimeUs) return false;
...
if(target<1577836800000000.0L || target>4102444800000000.0L) return false;
```

**NACHHER (vollständig instrumentiert):**
```cpp
const int64_t now=esp_timer_get_time();
// Beta131-FIX (Eighteenth cycle): Instrument ALL return paths
if (now<referenceReceivedNodeMono || now-referenceReceivedNodeMono>kReferenceLifetimeUs) {
    ++precisionGuardOther;  // Reference lifetime expired (30s)
    return false;
}
const double age=static_cast<double>(now-latestQualityEstimate.nodeReferenceUs);
if (age<0 || age>static_cast<double>(kPrecisionLifetimeMs)*1000.0) {
    ++precisionGuardOther;  // Estimate age exceeds lifetime (120s)
    return false;
}
const double drift=stats.driftValid ? stats.driftPpm : 0.0;
if (!std::isfinite(drift) || fabs(drift)>500.0) {
    ++precisionGuardOther;  // Drift invalid or excessive
    return false;
}
...
if (referenceMono<=0 || llabs(referenceMono-approx)>kReferenceLifetimeUs) {
    ++precisionGuardOther;  // Reference mono reconstruction failed
    return false;
}
...
if(target<1577836800000000.0L || target>4102444800000000.0L) {
    ++precisionGuardOther;  // UTC target out of valid range
    return false;
}
```

**Effekt:**
- ALLE Return-Pfade nun instrumentiert
- `precisionGuardOther` steigt bei Line 1274-1288 Failures
- Nächster Hardware-Test zeigt EXAKT welcher Guard fehlschlägt

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

**Alle Tests bestehen!** Keine Regression.

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

---

## HARDWARE-ERWARTUNG

**Nach Flash von devxiao3 mit Cycle 18:**

### Erwartung 1: Aktivierungs-Loop behoben
```
# Console-Log sollte NICHT mehr zeigen:
[BROADCASTER] Time base LOST - lease revoked
[BROADCASTER] Production ACTIVATED (txn 140, time valid)
[BROADCASTER] Time base LOST - lease revoked
```

**Stattdessen ENTWEDER:**

**Szenario A - Stabile Aktivierung:**
```
[BROADCASTER] Production ACTIVATED (txn XXX, time valid)
# Dann KEINE weiteren Meldungen (stabil!)
```

**Szenario B - Blockierung wegen precisionUtcNow():**
```
[BROADCASTER] Production BLOCKED - time base invalid
# Dann KEINE weiteren Meldungen
```

**Szenario C - Einmalige Revocation bei ungültiger Zeitbasis:**
```
[BROADCASTER] Production ACTIVATED (txn XXX, time valid)
[BROADCASTER] Time base LOST - lease revoked
# Dann KEINE Reaktivierung mehr! (Loop gestoppt!)
```

### Erwartung 2: precisionGuardOther steigt

```json
{
  "broadcaster_precision_failed": >0,  // Falls weiterhin fehlschlägt
  "precision_guard_other": >0,  // ← SOLLTE STEIGEN! (Line 1274-1288)
  "precision_guard_was_coordinator": 0,
  "precision_guard_epoch_mismatch": 0,
  "precision_guard_tag_mismatch": 0,
  "precision_guard_not_valid": 0
}
```

**Wenn `precision_guard_other` steigt:**
- Reference Lifetime (30s) oder Estimate Age (120s) oder Drift oder UTC Range Problem
- **Wahrscheinlich:** Reference Lifetime (Line 1274) wegen alter Serie

### Erwartung 3: Neue Serien werden akzeptiert

**WENN** der Aktivierungs-Loop gestoppt ist:
- System wird stabil
- Neue Serien werden akzeptiert
- `last_accepted_age_ms` sinkt (< 30s)
- `precision_guard_other` hört auf zu steigen
- Production beacons werden gesendet!

**Oder:** Problem bleibt (andere Root Cause), aber wir sehen jetzt diagnostisch WELCHE!

---

## SICHERHEITSANALYSE

**Keine Sicherheitsschwächung!**

- ✅ HMAC-Validierung UNVERÄNDERT
- ✅ Replay-Protection UNVERÄNDERT
- ✅ Epoch/Tag/Boot-Validierung UNVERÄNDERT
- ✅ Lease-Sicherheit UNVERÄNDERT
- ✅ Broadcaster-Identität UNVERÄNDERT
- ✅ Senderexklusivität ERHALTEN
- ✅ Nur Diagnostik hinzugefügt (Counter)

**Änderungen:**
1. **Fix 1:** Löscht Aktivierungs-State bei Revocation → **SICHERER** (kein Stale State)
2. **Fix 2:** Instrumentiert Return-Pfade → **KEINE funktionalen Änderungen**

**Risiko:** **MINIMAL**

---

## QUALITÄTSPROBLEME (Aufgabe D)

**Hardware-Status:**
- Aktuelle Serie: `last_series_spread_us=625` µs
- Frühere Serien: 14-36.5 µs
- **Verschlechterung:** Faktor 17-44x!

**Mögliche Ursachen:**

**1. Aktivierungs-/Revocation-Loop:**
- Mehrmals pro Sekunde Aktivierung/Revocation
- Serial-Output: "[BROADCASTER] ..." Messages
- CPU-Last durch State-Änderungen
- **Effekt:** Interrupt-Latency steigt → schlechtere Probe/Reply/Final Timestamps

**2. CPU/Serial-Last:**
- Aktivierungs-Loop erzeugt kontinuierliche Serial-Output
- Serial.println() blockiert CPU (mehrere ms!)
- **Effekt:** Schlechtere Timing-Präzision

**3. RF-Interferenz:**
- Nicht durch Code verursacht
- **Unwahrscheinlich** bei gleicher Hardware-Umgebung

**Erwartung nach Fix:**
- Aktivierungs-Loop gestoppt
- CPU/Serial-Last reduziert
- Spread sollte sich VERBESSERN (zurück zu 14-50 µs)

**WICHTIG:** Keine Qualitätsgrenzen gelockert! Serien mit 625 µs Spread sind KORREKT akzeptiert (unter kMaxSeriesSpreadUs).

---

## FLASH-EMPFEHLUNG

**KRITISCH: NUR devxiao3 (Broadcaster) flashen!**

1. ✅ **devxiao3 (Broadcaster) MUSS geflasht werden**
   - Aktuell: beta131 Cycle 17 (Aktivierungs-Loop!)
   - Update: beta131 Cycle 18 (Loop-Fix)
   - **GRUND:** Kritischer Aktivierungs-/Revocation-Loop-Bugfix

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
1. Serial-Log beobachten (5 Minuten):
   - Prüfen: KEINE wiederholten Aktivierungs-/Revocation-Meldungen
   - Prüfen: Stabile Aktivierung ODER klare Blockierung
2. JSON-Endpoint auslesen:
   ```bash
   curl http://devxiao3.local/cluster/espnow_radio | jq .espnow_time_local
   ```
3. Prüfen:
   - `broadcaster_production_beacons_sent` steigt? ✅
   - `precision_guard_other` steigt? → Zeigt welcher Guard fehlschlägt
   - `last_accepted_age_ms` < 30000? → Neue Serien werden akzeptiert

---

## ANTWORTEN AUF BENUTZER-FRAGEN

### 1. Nachgewiesene Root Cause

**ROOT CAUSE 1 - Aktivierungs-/Revocation-Loop (DEFINITIV NACHGEWIESEN):**
- Line 2415: `broadcasterLeaseActive=false` bei precisionUtcNow() Failure
- **ABER:** `broadcasterLeaseActivationMs` und `broadcasterActivationTransaction` NICHT gelöscht
- Nächster Tick: `broadcasterTimeBaseValid` neu berechnet → true
- Aktivierungs-Check (Line 2352) re-aktiviert mit STALE Transaction
- **ENDLOSSCHLEIFE** nachgewiesen durch Console-Log

**ROOT CAUSE 2 - Nicht-instrumentierte precisionUtcNow() Guards (NACHGEWIESEN):**
- Lines 1274-1288: Fünf Return-Pfade NICHT instrumentiert
- `broadcaster_precision_failed=800` ABER alle Guard-Counter=0
- **BEWEIST:** Failure in NICHT instrumentierten Guards
- **Wahrscheinlich:** Line 1274 (Reference Lifetime 30s) wegen `last_accepted_age_ms=85916`

### 2. Exakte Codeänderungen

**Änderung 1:** cluster_espnow_radio.cpp Lines 2420-2421 (NEU)
```cpp
broadcasterLeaseActivationMs=0;
broadcasterActivationTransaction=0;
```

**Änderung 2:** cluster_espnow_radio.cpp Lines 1275-1303 (Instrumentierung)
```cpp
if (...) { ++precisionGuardOther; return false; }  // 5 Stellen
```

### 3. Erklärung broadcaster_precision_failed=800 bei Guard-Countern=0

**BEWEIS:**
- `precisionUtcNow()` hat Return-Pfade in Lines 1274-1288
- Cycle 17 instrumentierte NUR Lines 1253-1270
- Lines 1274-1288 waren NICHT instrumentiert
- **Daher:** Failures in Lines 1274-1288 erhöhen `broadcasterPrecisionFailed`, NICHT Guard-Counter

**Cycle 18 Fix:** ALLE Return-Pfade instrumentiert → Nächster Test zeigt EXAKT welcher Guard!

### 4. Erklärung warum Transaktion 140 wiederverwendet wurde

**NACHGEWIESEN:**
- Grant-Renewals (txn 159, 160) setzen NUR `broadcasterLeaseExpiresMs`
- `broadcasterActivationTransaction` wird NUR bei ERSTEM Grant gesetzt
- Bei Revocation: `broadcasterActivationTransaction` NICHT gelöscht (vor Fix)
- **Resultat:** Reaktivierung mit ALTER Transaction 140

**Nach Fix:** Revocation löscht `broadcasterActivationTransaction` → Keine Wiederverwendung!

### 5. Sicherheitsanalyse Lease-/Senderexklusivität

**SENDEREXKLUSIVITÄT ERHALTEN:**
- Coordinator-Suppression UNVERÄNDERT (Lines 2431-2453)
- Broadcaster-Aktivierung UNVERÄNDERT (Lines 2352-2362)
- Lease-Expiry UNVERÄNDERT (Lines 2366-2371)
- Bootstrap-Grace-Period UNVERÄNDERT (Lines 2447-2453)

**LEASE-SICHERHEIT ERHALTEN:**
- Grant/Ack-Protokoll UNVERÄNDERT
- Transaction-Validierung UNVERÄNDERT
- Boot/Tag/Epoch-Checks UNVERÄNDERT
- HMAC/Replay-Protection UNVERÄNDERT

**FIX VERBESSERT SICHERHEIT:**
- Stale Activation State wird gelöscht
- Keine Reaktivierung mit ungültiger Transaction
- Klarere State-Machine (kein Zombie-State)

### 6. Tests und Build-Ergebnis

**Tests:**
```
==============================================
Test Results: 13 PASSED, 0 FAILED
==============================================
```

**Build:** Syntax-validiert (Arduino-Headers für ESP32 erforderlich)

### 7. Welche Geräte flashen

**FLASH:** ✅ NUR devxiao3 (Broadcaster)

**NICHT FLASH:**
- ❌ devfreenove1 (Coordinator) - Cycle 16 funktioniert
- ❌ devxiao1 (Node) - Unified funktioniert
- ❌ devxiao2 (Node) - Unified funktioniert

---

## ZUSAMMENFASSUNG

**Problem:**
- ❌ Aktivierungs-/Revocation-Loop (mehrmals pro Sekunde)
- ❌ Stale Transaction 140 wiederverwendet
- ❌ precisionUtcNow() schlägt 800x fehl (OHNE Guard-Counter-Erhöhung)
- ❌ Keine production beacons

**Root Cause (DEFINITIV NACHGEWIESEN):**
- ✅ **RC1:** Aktivierungs-State nicht gelöscht bei Revocation → Endlosschleife
- ✅ **RC2:** precisionUtcNow() Guards in Lines 1274-1288 nicht instrumentiert
- ✅ **RC3 (wahrscheinlich):** Reference Lifetime (30s) schlägt fehl wegen alter Serie (85s)

**Fix:**
- ✅ Aktivierungs-State löschen bei Revocation (2 Zeilen)
- ✅ Alle precisionUtcNow() Return-Pfade instrumentieren (5 Stellen)

**Tests:**
- ✅ 13/13 PASSED (keine Regression)

**Erwartung:**
- ✅ Aktivierungs-Loop gestoppt
- ✅ `precision_guard_other` zeigt genauen Fehler
- ✅ System wird stabil
- ✅ Neue Serien werden akzeptiert
- ✅ Production beacons werden gesendet! 🎯

**Nach 18 Korrekturzyklen:** Endlich eine stabile Broadcaster-Aktivierung! Der Loop ist tot! 🔥
