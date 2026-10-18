# Beta131 - Dreizehnter Korrekturzyklus - Bootstrap bei aktiver Lease

**Datum:** 2026-10-17
**Version:** v87-beta131 (Thirteenth Cycle)
**Status:** ROOT CAUSE NACHGEWIESEN - BEREIT FÜR FLASH (NUR DEVXIAO3)

---

## 🔴 PROBLEM: Bootstrap-Deadlock bei Broadcaster-Neustart während aktiver Lease

### Hardware-Befund (devxiao3-Neustart während laufender Coordinator-Lease)

**devxiao3 (Broadcaster nach Neustart):**
```
broadcaster_lease_active: true
broadcaster_lease_expiry_ms: 4294939063 (64.8s verbleibend)
exchanges_complete: 0        ← PROBLEM!
exchange_timeouts: 20        ← Alle Exchanges scheitern!
probes_sent: 22
replies_received: 0
finals_sent: 0
rejected: 175
broadcaster_time_base_valid: false  ← Production blockiert!
```

**devfreenove1 (Coordinator mit aktiver Lease):**
```
coordinator_lease_active: true
coordinator_lease_broadcaster_tag: a0e6f8542d4d0b1d (devxiao3)
coordinator_beacons_suppressed: 45913  ← Beacons unterdrückt (korrekt!)
probes_answered: 318 (total, nicht zwingend von devxiao3)
```

**Symptome:**
1. ✅ Grant/Ack funktioniert (Lease aktiv)
2. ✅ Coordinator unterdrückt Beacons (korrekt, verhindert Overlap)
3. ❌ devxiao3 sendet 22 Probes, erhält 0 Replies
4. ❌ Alle Exchanges schlagen fehl (20 Timeouts)
5. ❌ broadcaster_time_base_valid bleibt false
6. ❌ Production-Beacons bleiben blockiert

---

## 🔬 ROOT CAUSE ANALYSE (Code-basiert nachgewiesen)

### Fehlersequenz nach Broadcaster-Neustart

#### 1. Broadcaster-Neustart löscht Zeitquellen-Identität

**cluster_espnow_radio.cpp:1173** (`stop()` Funktion):
```cpp
activeTimeSourceTag=0;
activeTimeSourceBoot=0;
activeTimeSourceEpoch=0;
```

**Bedeutung:** Nach Neustart hat devxiao3 KEINE Information über die Coordinator-Identität.

---

#### 2. Coordinator unterdrückt Beacons (aktive Lease)

**cluster_espnow_radio.cpp:1626-1640** (Grant Send):
```cpp
// Coordinator suppresses beacons 1000ms after Grant send
coordinatorSuppressionStartMs=now+kSuppressionDelayMs;  // +1000ms
```

**cluster_espnow_radio.cpp:2324-2330** (Beacon TX Check):
```cpp
if(coordinatorBeaconsSuppressed && coordinatorGrantedLeaseActive &&
   static_cast<int32_t>(now-coordinatorSuppressionStartMs)>=0) {
    return;  // Don't send beacons during active broadcaster lease
}
```

**Bedeutung:** Coordinator sendet KEINE Beacons während der Lease (korrekt, verhindert Sender-Overlap).

**Problem:** Broadcaster kann Coordinator-Identität NICHT aus Beacons erhalten.

---

#### 3. Grant ENTHÄLT Coordinator-Identität (aber wird nicht verwendet)

**cluster_time_grant_protocol.h:17-19** (Grant Message Format):
```cpp
struct Message {
    uint64_t coordinatorTag=0, coordinatorBoot=0;
    uint32_t coordinatorEpoch=0;
    // ... weitere Felder
};
```

**KRITISCH:** Grant ist HMAC-authentifiziert und enthält vollständige Coordinator-Identität!

**VOR dem Fix:** Diese Werte wurden NICHT in `activeTimeSourceTag/Boot/Epoch` kopiert.

---

#### 4. Probe TX fällt auf lokalen Tag zurück (FALSCH!)

**cluster_espnow_radio.cpp:2422** (Probe Transmission):
```cpp
p.coordinatorTag = activeTimeSourceTag ? activeTimeSourceTag : tag;
//                 ^^^^^^^^^^^^^^^^^ = 0 nach Neustart
//                                                            ^^^ = devxiao3's lokaler Tag!
```

**Bedeutung:**
- `activeTimeSourceTag == 0` (keine Beacon-Bootstrap möglich)
- Fallback: `p.coordinatorTag = tag` (devxiao3's eigener Tag!)
- Probe wird mit FALSCHEM coordinatorTag gesendet

---

#### 5. Coordinator lehnt Probes ab (Tag-Mismatch)

**cluster_espnow_radio.cpp:1981-1984** (Coordinator Probe RX):
```cpp
// Verify Probe is addressed to THIS coordinator
if(f.coordinatorTag != tag) {
    ++stats.rejected;
    continue;  // Probe addressed to different sender
}
```

**Verifikation:**
- Coordinator's `tag` = coordinator_tag (z.B. `0xabc...123`)
- Probe's `coordinatorTag` = devxiao3's tag (z.B. `0xa0e...b1d`) ✗
- `f.coordinatorTag != tag` → **REJECTED**

---

#### 6. Bootstrap-Deadlock

```
Broadcaster (devxiao3):
  - activeTimeSourceTag = 0 (nach Neustart)
  - Sendet Probes mit coordinatorTag = devxiao3_tag (falsch!)

Coordinator (devfreenove1):
  - Empfängt Probes mit coordinatorTag != coordinator_tag
  - Lehnt ALLE Probes ab (stats.rejected++)
  - Sendet KEINE Replies

Result:
  - exchange_timeouts = 20 (alle Probes timeout)
  - exchanges_complete = 0
  - broadcaster_time_base_valid = false
  - Production blockiert
```

---

## ✅ FIX: Grant-Bootstrap der Coordinator-Identität

### Implementierung

**cluster_espnow_radio.cpp:1444-1455** (Grant Reception):
```cpp
// Beta131-FIX (Thirteenth cycle): Bootstrap coordinator identity from Grant
// CRITICAL: After broadcaster restart, coordinator suppresses beacons (active lease)
// Grant contains authenticated coordinator identity - use for bootstrap
// This enables Probe/Reply/Final exchanges without prior beacon reception
if(!activeTimeSourceTag) {
    activeTimeSourceTag = m.coordinatorTag;
    activeTimeSourceBoot = m.coordinatorBoot;
    activeTimeSourceEpoch = m.coordinatorEpoch;
    memcpy(activeTimeSourceMac, item.mac, 6);  // Coordinator's MAC from Grant sender
    Serial.printf("[BOOTSTRAP] Coordinator identity from Grant: tag=%llx boot=%llx epoch=%u\n",
        activeTimeSourceTag, activeTimeSourceBoot, activeTimeSourceEpoch);
}
```

### Wirkungsweise

**NACH dem Fix:**

1. **Grant Reception (T=0):**
   - Grant empfangen mit `coordinatorTag/Boot/Epoch`
   - `if(!activeTimeSourceTag)` → WAHR (nach Neustart = 0)
   - `activeTimeSourceTag = m.coordinatorTag` (Coordinator-Identity gesetzt!)

2. **Probe TX (T=100ms):**
   ```cpp
   p.coordinatorTag = activeTimeSourceTag ? activeTimeSourceTag : tag;
   //                 ^^^^^^^^^^^^^^^^^ != 0 (aus Grant!)
   //                 = coordinator_tag ✓
   ```

3. **Coordinator Probe RX:**
   ```cpp
   if(f.coordinatorTag != tag) {  // coordinator_tag == coordinator_tag ✓
       // NICHT rejected!
   }
   ```

4. **Reply/Final funktionieren:**
   - Coordinator sendet Reply (Line 1998)
   - Broadcaster empfängt Reply
   - Bootstrap-safe Validation akzeptiert `expectedBoot==0` und `expectedEpoch==0` (Lines 2206-2214)
   - Exchange erfolgreich!

5. **Zeitbasis etabliert:**
   - Serie akzeptiert → `latestQualityEstimate.valid = true`
   - Präzision etabliert → `lastPrecisionMs != 0`
   - `broadcaster_time_base_valid = true`
   - Production-Beacons aktiviert

---

## 🔒 SICHERHEITSANALYSE

### 1. Grant-Authentifizierung

**cluster_espnow_radio.cpp:1408-1443** (Grant Reception, VOR dem Bootstrap-Code):
```cpp
// HMAC verification (Line ~1420)
if(!ClusterTimeGrantProtocol::decode(data, len, m, hmacVerifyGrant)) {
    ++grantDiag.decodeFailures;
    continue;  // HMAC verification failed
}

// Replay protection (Line ~1430)
if(!grantReplay.accept(m)) {
    ++grantDiag.replayRejections;
    continue;  // Replay rejected
}
```

**Bedeutung:** Grant-Bootstrap verwendet NUR HMAC-authentifizierte, Replay-geschützte Grants.

### 2. Bootstrap-Bedingung

```cpp
if(!activeTimeSourceTag) {  // Nur wenn NOCH KEINE Zeitquelle etabliert
```

**Eigenschaften:**
- ✅ **One-time Bootstrap:** Nur beim ersten Grant nach Neustart
- ✅ **Beacon-Override:** Spätere Beacon-Reception überschreibt bei Bedarf (Line 2188)
- ✅ **Kein Re-Bootstrap:** Sobald `activeTimeSourceTag != 0`, wird Grant-Identity NICHT mehr verwendet

### 3. Sender-Exklusivität ERHALTEN

**Coordinator Suppression (UNVERÄNDERT):**
```cpp
// Line 1637: Coordinator suppresses 1000ms after Grant send
coordinatorSuppressionStartMs=now+kSuppressionDelayMs;  // +1000ms
```

**Broadcaster Activation (UNVERÄNDERT):**
```cpp
// Line 1463: Broadcaster activates 1500ms after Grant receive
broadcasterLeaseActivationMs=now+BROADCASTER_ACTIVATION_DELAY_MS;  // +1500ms
```

**Safety Margin:** 550ms (Coordinator stoppt BEVOR Broadcaster startet)

**Wichtig:** Grant-Bootstrap ändert NICHTS an der Sender-Timing-Logik!

### 4. Transaktionsbindung ERHALTEN

**Grant-Ack Transaction (UNVERÄNDERT):**
```cpp
// Line 1441-1442: Grant transaction tracking
grantLastCoordinatorBoot=m.coordinatorBoot;
grantLastCoordinatorEpoch=m.coordinatorEpoch;
```

**Probe/Reply/Final Validation (UNVERÄNDERT):**
```cpp
// Lines 2206-2214: Bootstrap-safe validation (Cycle 10-11 Fixes)
const bool bootMatches = (pending.expectedBoot == 0) ? true : (f.senderBoot == pending.expectedBoot);
const bool epochMatches = (pending.expectedEpoch == 0) ? true : (f.epoch == pending.expectedEpoch);
```

---

## 🧪 TESTS

### Offline-Tests (test_lease_state_machine.cpp)

**Durchgeführt:** 13/13 Tests PASSED

```
=== TEST 1: Normal Handover === PASS
=== TEST 2: Seamless Renewal === PASS
=== TEST 3: Lost Grant (Fallback) === PASS
=== TEST 4: Lost Ack (Timeout) === PASS
=== TEST 5: Coordinator → Broadcaster → Coordinator === PASS
=== TEST 6: Delayed Renewal (Variable RF Delay) === PASS
=== TEST 7: Coordinator Reboot During Lease (STARTUP GUARD) === PASS
=== TEST 8: Multiple Lost Renewals === PASS
=== TEST 9: Coordinator Reboot During Renewal === PASS
=== TEST 10: Capture Source Switch Detection === PASS
=== TEST 11: Capture Invalidation on Radio Restart === PASS
=== TEST 12: Bootstrap Timing (No Overlap) === PASS
=== TEST 13: Probe/Reply/Final Epoch Validation === PASS

Test Results: 13 PASSED, 0 FAILED
```

**Verifikation:**
- ✅ Keine Regression durch Grant-Bootstrap-Fix
- ✅ Sender-Exklusivität erhalten (550ms gap)
- ✅ Bootstrap Timing korrekt (Cycle 10 Fix)
- ✅ Epoch Validation korrekt (Cycle 11 Fix)
- ✅ Alle bisherigen Fixes funktionieren

### Test-Limitierungen

**❌ KEIN spezifischer Grant-Bootstrap-Test**

**Grund:**
- Offline-Tests simulieren State-Machine-Logik
- ESP-NOW Grant-Reception kann NICHT offline simuliert werden
- Grant-HMAC-Verification erfordert echte Crypto-Implementierung
- Beacon-Suppression während Lease erfordert Hardware-Timing
- Probe/Reply/Final-Exchanges erfordern echte ESP-NOW-Kommunikation

**Hardware-Test ERFORDERLICH:**
- Nachweis, dass `activeTimeSourceTag` aus Grant gesetzt wird (`[BOOTSTRAP]` Serial-Log)
- Nachweis, dass Probes mit korrektem `coordinatorTag` gesendet werden
- Nachweis, dass Coordinator Probes AKZEPTIERT (stats.rejected nicht steigt)
- Nachweis, dass Exchanges erfolgreich sind (`exchanges_complete` steigt)
- Nachweis, dass `broadcaster_time_base_valid = true` wird

---

## 📋 GEÄNDERTE DATEIEN

| Datei | Änderungen | Zeilen |
|-------|-----------|--------|
| **cluster_espnow_radio.cpp** | Grant-Bootstrap-Logik | 1444-1455 (12 Zeilen) |
| **CHANGELOG.md** | Dreizehnter Korrekturzyklus dokumentiert | 0-71 |
| **BETA131_THIRTEENTH_CYCLE.md** | Dieser Bericht | neu |

**Keine Änderungen an:**
- ❌ cluster_time_grant_protocol.h (Grant-Format unverändert)
- ❌ test_lease_state_machine.cpp (Offline-Test-Limitierung)
- ❌ cluster.cpp, webconfig_cluster.cpp (nicht betroffen)
- ❌ Coordinator-Code (keine zusätzlichen Änderungen)

**Erhaltene Korrekturen:**
- ✅ Cycle 10: Bootstrap Timing (550ms gap)
- ✅ Cycle 11: Epoch Validation (activeTimeSourceEpoch)
- ✅ Cycle 12: Self-Beacon Filtering (senderBoot check)

---

## 🎯 HARDWARE-ERWARTUNGEN NACH FLASH

### Erfolgskriterien (devxiao3 nach Neustart während aktiver Lease)

**Primäres Kriterium:**
```
broadcaster_time_base_valid: true (DAUERHAFT!)
```

**Sekundäre Indikatoren:**

| Metrik | VOR Fix (Cycle 12) | NACH Fix (Cycle 13) | Bedeutung |
|--------|-------------------|---------------------|-----------|
| `exchanges_complete` | 0 (keine!) | >100 (kontinuierlich steigend) | **Exchanges funktionieren!** |
| `exchange_timeouts` | 20 (100%!) | <20 (<15%) | Niedrige Timeout-Rate |
| `probes_sent` | 22 | >100 | Laufende Synchronisation |
| `replies_received` | 0 | >80 | **Coordinator antwortet!** |
| `rejected` | 175 | <50 | **Probes akzeptiert!** |
| `last_series_estimate_age_ms` | N/A (keine Serie) | <60000 (<1 min) | Frische Serien |
| `broadcaster_time_base_valid` | false | **true** | **HAUPT-INDIKATOR!** |
| `broadcaster_production_beacons_sent` | 0 (blockiert) | >1000 (steigend) | **Production läuft!** |

### Serial-Log-Indikatoren

**Grant-Bootstrap (devxiao3):**
```
[BOOTSTRAP] Coordinator identity from Grant: tag=<coordinator_tag> boot=<boot> epoch=<epoch>
```

**Probe/Reply/Final Exchanges:**
```
[NODE] Precision estimate: offset=<µs> rtt=<ms> spread=<µs>
[NODE] Serie accepted: samples=<n> spread=<µs> median_rtt=<ms>
```

**Production Activation:**
```
[BROADCASTER] Production beacons active (time base valid)
```

### Diagnosezähler

**Falls Exchanges NICHT steigen:**

1. **Prüfe Grant-Bootstrap:**
   ```
   Serial-Log: "[BOOTSTRAP] Coordinator identity from Grant"
   → Falls NICHT vorhanden: Grant wird nicht empfangen oder activeTimeSourceTag bereits gesetzt
   ```

2. **Prüfe Probe Rejection:**
   ```
   devfreenove1: stats.rejected steigt
   → Probes werden weiterhin rejected (coordinatorTag-Mismatch?)
   ```

3. **Prüfe Reply Reception:**
   ```
   devxiao3: replies_received steigt?
   → Falls NEIN: Coordinator sendet keine Replies (Probe rejection?)
   → Falls JA: Final-Validation schlägt fehl?
   ```

---

## 🚀 FLASH-EMPFEHLUNG

### NUR devxiao3 flashen

**Begründung:**
- ✅ Alle Code-Änderungen betreffen NUR Grant-Reception (Broadcaster-Seite)
- ✅ Coordinator Grant-Send ist UNVERÄNDERT (Lines 1598-1640)
- ✅ Coordinator Probe-Validation ist UNVERÄNDERT (Line 1981)
- ✅ Grant-Protokoll-Format ist UNVERÄNDERT (cluster_time_grant_protocol.h)

**Coordinator (devfreenove1):**
- ❌ KEINE Code-Änderungen seit Cycle 12
- ❌ Kein Re-Flash erforderlich

**Broadcaster (devxiao3):**
- ✅ Grant-Bootstrap-Logik hinzugefügt (Lines 1444-1455)
- ✅ Flash ERFORDERLICH

### Flash-Prozedur

1. **Nur devxiao3 mit v87-beta131 flashen**
2. **devfreenove1 NICHT flashen** (läuft weiter mit Cycle 12)
3. **Test-Szenario:**
   - Beide Geräte laufen lassen bis Lease aktiv
   - devxiao3 neu starten (Reset-Button oder Power-Cycle)
   - Serial-Log beobachten: `[BOOTSTRAP]` Nachricht?
   - Web-Metriken prüfen: `exchanges_complete` steigt?

---

## 📊 KORREKTUR-CHRONOLOGIE (Zyklen 10-13)

### Zyklus 10 (CORRECTED): Bootstrap Timing Overlap

**Problem:** Coordinator suppressed zu spät, 1.5s Overlap mit Broadcaster.

**Fix:** Koordinierte Delays:
- Coordinator Suppression: Grant_send + 1000ms
- Broadcaster Activation: Grant_receive + 1500ms
- Safety Margin: 550ms

**Dateien:** cluster_espnow_radio.cpp (Lines 185, 1626-1640, 1444-1470, 2324-2330, 2186-2188, 2212-2214)

---

### Zyklus 11: Epoch Validation Mismatch

**Problem:** Probe verwendet lokalen `epoch` statt `activeTimeSourceEpoch` → Reply/Final rejected.

**Fix:** `pending.expectedEpoch = activeTimeSourceTag ? activeTimeSourceEpoch : 0;`

**Dateien:** cluster_espnow_radio.cpp (Line 2433, 2189-2191, 2219-2221)

---

### Zyklus 12: Broadcaster Self-Reception

**Problem:** Broadcaster empfängt eigene Production-Beacons → `activeTimeSourceTag` überschrieben → Resynchronisation fehlgeschlagen.

**Fix:** Self-Beacon-Filtering: `if(f.senderBoot == localBootNonce) continue;`

**Dateien:** cluster_espnow_radio.cpp (Lines 2126-2132)

---

### Zyklus 13 (CURRENT): Grant-Bootstrap bei aktiver Lease

**Problem:** Broadcaster-Neustart während aktiver Lease → Coordinator suppresses Beacons → Keine Coordinator-Identity → Probes mit falschem Tag → 0 Exchanges.

**Fix:** Bootstrap `activeTimeSourceTag/Boot/Epoch` aus authentifiziertem Grant.

**Dateien:** cluster_espnow_radio.cpp (Lines 1444-1455)

---

## 🔍 WICHTIGE ERKENNTNISSE

### 1. Grant als Bootstrap-Quelle

**Bisher übersehen:** Grant enthält vollständige, HMAC-authentifizierte Coordinator-Identität.

**Jetzt genutzt:** Grant-Bootstrap ermöglicht Probe/Reply/Final OHNE vorherige Beacon-Reception.

### 2. Beacon-Suppression ist KORREKT

**Coordinator-Verhalten während Lease:** Beacons unterdrücken (verhindert Sender-Overlap).

**Bootstrap-Problem gelöst:** Grant-Identity ersetzt Beacon-Bootstrap.

### 3. Bootstrap-Safe Validation ist ESSENTIELL

**Wildcards (`expectedBoot==0`, `expectedEpoch==0`)** ermöglichen Bootstrap ohne vorherige Beacon-Synchronisation.

**Kombiniert mit Grant-Bootstrap:** Sichere, authentifizierte Erstverbindung ohne Beacons.

### 4. Offline-Tests haben Grenzen

**Timing-Logik testbar:** Delays, Overlaps, State-Transitions (✓).

**Nicht testbar:** ESP-NOW-Kommunikation, HMAC-Verification, echte Beacon-Reception (✗).

**Hardware-Test ERFORDERLICH** für vollständige Verifikation.

---

## ⚠️ OFFENE RISIKEN

### 1. Grant-Verlust während Bootstrap

**Szenario:** Broadcaster startet neu, Grant geht verloren (RF-Interferenz).

**Verhalten:**
- Keine Lease → Coordinator sendet Beacons (korrekt)
- Broadcaster bootstrapped via Beacons (Fallback funktioniert)

**Risiko:** GERING (Beacon-Fallback vorhanden)

---

### 2. Coordinator-Neustart während Broadcaster-Bootstrap

**Szenario:** Coordinator startet neu während Broadcaster Grant-Bootstrap durchläuft.

**Verhalten:**
- Coordinator: Startup Guard 6000ms (keine Beacons, keine Grants)
- Broadcaster: Lease läuft noch (sendet Production-Beacons)
- Nach 6000ms: Coordinator sendet Beacons (Broadcaster-Lease abgelaufen)

**Risiko:** GERING (Startup Guard verhindert Overlap)

---

### 3. Gleichzeitiger Neustart beider Geräte

**Szenario:** Beide Geräte starten gleichzeitig (Power-Outage).

**Verhalten:**
- Beide: Startup Guard aktiv
- Coordinator: Guard läuft ab, sendet Beacons
- Broadcaster: Empfängt Beacons, bootstrapped normal

**Risiko:** GERING (normaler Bootstrap-Pfad)

---

### 4. MAC-Spoofing-Angriff

**Szenario:** Angreifer sendet Grant mit gefälschtem Coordinator-Tag.

**Schutz:**
- ✅ HMAC-Verification verhindert Fälschung (benötigt Cluster-Key)
- ✅ Replay Protection verhindert alte Grants
- ✅ Grant-Bootstrap nur wenn `activeTimeSourceTag==0` (einmalig)

**Risiko:** SEHR GERING (mehrfache Schutzschichten)

---

## 📝 NICHT BEWIESENE ANNAHMEN

### 1. Grant-Delivery-Rate

**Annahme:** Grants erreichen Broadcaster zuverlässig (>90%).

**Realität:** Unbekannt (keine Hardware-Metriken).

**Validierung:** Nach Flash prüfen: `grantDiag.grantsAccepted` vs. Coordinator `grantDiag.grantsSent`.

---

### 2. Bootstrap-Geschwindigkeit

**Annahme:** Grant-Bootstrap ist schneller als Beacon-Bootstrap.

**Realität:** Unbekannt (keine Timing-Metriken).

**Validierung:** Serial-Log Timestamps `[BOOTSTRAP]` vs. erste Serie-Akzeptanz.

---

### 3. Grant-Bootstrap bei Renewal

**Annahme:** Grant-Bootstrap nur bei Initial-Grant, NICHT bei Renewal.

**Code-Realität:**
```cpp
if(!activeTimeSourceTag) {  // Bedingung
```

Bei Renewal ist `activeTimeSourceTag != 0` → Bootstrap wird NICHT ausgeführt (korrekt).

**Validierung:** Nach Flash prüfen: `[BOOTSTRAP]` Log nur bei Neustart, NICHT bei Renewal.

---

## 🎓 ZUSAMMENFASSUNG

### Problem
Broadcaster-Neustart während aktiver Coordinator-Lease führte zu Bootstrap-Deadlock (0 Exchanges).

### Root Cause
1. Broadcaster-Neustart löscht `activeTimeSourceTag/Boot/Epoch` (Line 1173)
2. Coordinator unterdrückt Beacons während Lease (korrekt, verhindert Overlap)
3. Grant ENTHÄLT Coordinator-Identity, wurde aber NICHT verwendet
4. Probe TX fällt auf lokalen Tag zurück (Line 2422)
5. Coordinator lehnt Probes ab (Tag-Mismatch, Line 1981)

### Fix
Bootstrap `activeTimeSourceTag/Boot/Epoch` aus HMAC-authentifiziertem Grant (Lines 1444-1455).

### Sicherheit
- ✅ Grant HMAC-authentifiziert vor Bootstrap-Code
- ✅ Replay Protection aktiv
- ✅ Bootstrap nur wenn `activeTimeSourceTag==0` (one-time)
- ✅ Sender-Exklusivität ERHALTEN (550ms gap unverändert)
- ✅ Transaktionsbindung ERHALTEN

### Tests
- ✅ 13/13 Offline-Tests PASSED (keine Regression)
- ❌ Kein spezifischer Grant-Bootstrap-Test (Offline-Limitierung)
- ⚠️ Hardware-Test ERFORDERLICH

### Flash
- ✅ NUR devxiao3 flashen (Broadcaster-Seite)
- ❌ devfreenove1 NICHT flashen (keine Code-Änderungen)

### Erfolgskriterium
```
broadcaster_time_base_valid: true (nach Neustart während aktiver Lease)
```

---

**Status:** DREIZEHNTER KORREKTURZYKLUS ABGESCHLOSSEN
**Datum:** 2026-10-17
**Version:** v87-beta131 (Thirteenth Cycle)

**⚠️ BEREIT FÜR ARDUINO-KOMPILIERUNG UND FLASH (NUR DEVXIAO3)**
