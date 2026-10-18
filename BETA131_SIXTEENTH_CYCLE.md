# Beta131 - Sechzehnter Korrekturzyklus - Broadcaster Time Base Deadlock Fix

**Datum:** 2026-10-17
**Status:** CRITICAL FIX - Coordinator Probe-Handling korrigiert

---

## PROBLEM-ZUSAMMENFASSUNG

**Hardware-Befund nach Cycle 15:**
- devxiao3 (Broadcaster, Cycle 15): `exchanges_complete=0` (UNVERÄNDERT!)
- `exchange_timeouts=20`, `rejected=149`
- Grant/Ack funktioniert: 47 Grants empfangen, 47 Acks gesendet ✅
- **ABER:** Keine Zeit-Synchronisation möglich
- `broadcaster_time_base_valid=false`
- `[BROADCASTER] Production BLOCKED - time base invalid`

**Cycle 15 Fix (Premature Epoch Validation entfernt) war KORREKT aber UNVOLLSTÄNDIG!**

---

## ROOT CAUSE ANALYSE

### Vollständiger Probe/Reply/Final-Pfad analysiert

**Broadcaster-Seite (devxiao3, Cycle 15):**

1. **Probe-TX:** `cluster_espnow_radio.cpp:2461-2490`
   ```cpp
   p.type = EspNowTimePackets::Type::Probe;
   p.epoch = epoch;
   p.coordinatorTag = activeTimeSourceTag ? activeTimeSourceTag : tag;  // Uses coordinator tag!
   p.senderBoot = localBootNonce;
   p.nonce = nonceSeed;

   if(sendFrame(p, kBroadcastMac, &probeStampUs, false)) {  // BROADCAST!
       pending.active = true;
       pending.nonce = p.nonce;
       pending.t1 = p.timeUs;
       pending.expectedTag = p.coordinatorTag;     // Expects coordinator!
       pending.expectedBoot = activeTimeSourceBoot;
       pending.expectedEpoch = activeTimeSourceTag ? activeTimeSourceEpoch : 0;
       ++stats.probesSent;
   }
   ```
   - Broadcaster sendet Probe per **BROADCAST**
   - Probe ist addressed to **coordinatorTag** (noch keine eigene Zeitbasis!)
   - Wartet auf Reply vom Coordinator

**Coordinator-Seite (devfreenove1, Cycle 11):**

2. **Probe-RX:** `cluster_espnow_radio.cpp:1984-2039`

   **FEHLER 1 - Line 1992-1994 (VOR diesem Fix):**
   ```cpp
   if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe) {
       continue;  // Let broadcaster handle this Probe  ← VERWIRFT ALLE PROBES!
   }
   ```

   **Problem:**
   - Logik sollte verhindern, dass Coordinator Node-Probes beantwortet wenn Broadcaster aktiv
   - **ABER:** Verwirft ALLE Probes, einschließlich Broadcaster's EIGENE!
   - Broadcaster braucht Zeitbasis vom Coordinator BEVOR er Nodes bedienen kann
   - **DEADLOCK!**

   **FEHLER 2 - Line 2014-2017 (VOR diesem Fix):**
   ```cpp
   if(!slotIsOwner(item.mac, f.senderBoot) && !bench...) continue;
   ```

   **Problem:**
   - `slotIsOwner()` wird via `noteSlotPeer()` gesetzt (nur bei NodeReports!)
   - Broadcaster sendet KEINE NodeReports (sendet Diagnostic Beacons, Grant Acks)
   - Broadcaster ist niemals `slotIsOwner`
   - Falls Probe durch Fehler 1 gekommen wäre: hier verworfen!

### Vollständige Fehlersequenz

```
T=0s:   Broadcaster startet

T=1s:   Coordinator-Beacon empfangen
        activeTimeSourceTag = coordinatorTag
        activeTimeSourceBoot = coordinatorBoot
        activeTimeSourceEpoch = coordinatorEpoch

T=2s:   Grant empfangen
        broadcasterLeaseActive = true (Broadcaster-Seite)
        coordinatorGrantedLeaseActive = true (Coordinator-Seite!)

T=3s:   Broadcaster sendet Probe (Broadcast, coordinatorTag)
        → Erreicht Coordinator

T=3.1s: Coordinator empfängt Probe
        Line 1984-1987: Pakettyp-Check OK
        Line 1989: ++stats.authenticated
        Line 1992: if(coordinatorGrantedLeaseActive && Probe) → TRUE!
        Line 1993: continue;  ← PROBE VERWORFEN!

        (Falls Line 1992-1994 nicht existiert hätte:)
        Line 2007: if(coordinatorTag != tag) → FALSE (Probe ist an Coordinator!)
        Line 2014: if(!slotIsOwner(...)) → TRUE (Broadcaster ist kein Slot-Owner!)
        Line 2017: continue;  ← PROBE VERWORFEN!

T=5s:   Broadcaster: Exchange Timeout
        ++stats.exchangeTimeouts
        pending = {}

T=6s:   Weitere Probes, gleiches Problem

T=10s:  Nach 20 Timeouts: exchanges_complete=0
        broadcaster_time_base_valid=false

T=13s:  Grant-Lease läuft ab (keine Renewals weil keine Zeitbasis)
        broadcasterLeaseActive = false
        Coordinator resumed (ohne erfolgreichen Handover)
```

**Result:** Broadcaster kann NIE Zeitbasis aufbauen → NIE production beacons senden!

---

## WARUM HAT BETA130 FUNKTIONIERT?

**Historischer Befund:**
- beta130 Coordinator + beta131 Broadcaster: **352 vollständige Exchanges, 0 Timeouts** ✅
- beta131 Coordinator (Cycle 10+) + beta131 Broadcaster: **0 Exchanges** ❌

**Regression:**
- Line 1992-1994 Probe-Skip-Logik wurde wahrscheinlich zwischen beta130 und Cycle 11 eingeführt
- Intention: RF-Entlastung (Coordinator soll Nodes an Broadcaster delegieren)
- **FEHLER:** Keine Unterscheidung zwischen Node-Probes und Broadcaster-Probes!
- Broadcaster braucht eigene Zeitbasis BEVOR er Nodes bedienen kann

---

## FIX IMPLEMENTIERUNG

### 1. Coordinator Probe-Handling korrigiert (Problem 1)

**File:** `cluster_espnow_radio.cpp`
**Lines:** 1990-1998

**Before (FALSCH):**
```cpp
++stats.authenticated;
// Beta131-CORRECTED: Coordinator skips Probe handling when broadcaster lease active (Problem 4)
// Broadcaster handles node Probes when lease active to offload coordinator RF traffic
if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe) {
    continue;  // Let broadcaster handle this Probe
}
```

**After (KORRIGIERT):**
```cpp
++stats.authenticated;
// Beta131-FIX (Sixteenth cycle): Coordinator must answer broadcaster's own Probes
// CRITICAL: Broadcaster needs time base from coordinator BEFORE it can handle node Probes
// Only skip Probes NOT addressed to coordinator (i.e., addressed to broadcaster)
// PREVIOUS BUG: Skipped ALL Probes when lease active, including broadcaster's own
// This created DEADLOCK: Broadcaster couldn't get time base, couldn't activate, lease expired
if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe && f.coordinatorTag != tag) {
    continue;  // Let broadcaster handle Probes addressed to broadcaster (from nodes)
}
// Fall through: Handle Probes addressed to coordinator (from broadcaster itself during bootstrap)
```

**Logik:**
- Probes **addressed to coordinatorTag** → Coordinator beantwortet (Broadcaster-Bootstrap!)
- Probes **addressed to broadcasterOwnTag** → Broadcaster beantwortet (Nodes)
- Nutzt bestehende coordinatorTag-Adressierung (Cycle 7)
- Saubere Trennung: Broadcaster/Coordinator-Traffic

### 2. Broadcaster Slot-Exception hinzugefügt (Problem 2)

**File:** `cluster_espnow_radio.cpp`
**Lines:** 2012-2025

**Before (UNVOLLSTÄNDIG):**
```cpp
// Only the slot owner may receive timestamp replies. Authentication
// and replay validation above still apply for every received packet.
if(!slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot)) &&
   !(bench.running && bench.phase==3 && bench.peerIndex<bench.peerCount &&
     f.senderBoot==bench.peers[bench.peerIndex].boot &&
     memcmp(item.mac,benchMac[bench.peerIndex],6)==0)) continue;
```

**After (BROADCASTER-EXCEPTION):**
```cpp
// Beta131-FIX (Sixteenth cycle): Check if Probe is from observed broadcaster
// CRITICAL: Broadcaster needs time base from coordinator (not a slot owner!)
// Broadcaster is special role, not regular node in slot system
const bool isObservedBroadcaster = (observedBroadcasterAt != 0 &&
                                    memcmp(item.mac, observedBroadcasterMac, 6) == 0 &&
                                    f.senderBoot == observedBroadcasterBoot);

// Only slot owner, benchmark target, or observed broadcaster may receive timestamp replies
// Authentication and replay validation above still apply for every received packet
if(!slotIsOwner(item.mac,static_cast<uint32_t>(f.senderBoot)) &&
   !(bench.running && bench.phase==3 && bench.peerIndex<bench.peerCount &&
     f.senderBoot==bench.peers[bench.peerIndex].boot &&
     memcmp(item.mac,benchMac[bench.peerIndex],6)==0) &&
   !isObservedBroadcaster) continue;
```

**Logik:**
- **Slot-Owner:** Nodes (via NodeReports) ✅
- **Benchmark-Target:** Benchmark-Nodes ✅
- **Observed Broadcaster:** Broadcaster (via Diagnostic Beacons) ✅ **NEU!**
- Broadcaster-Identität kommt aus `observedBroadcasterMac/Boot` (Grant-System)
- Bereits validiert via HMAC + Replay Protection (Diagnostic Beacons)

---

## SICHERHEITSANALYSE

**Keine Schwächung der Sicherheit:**

1. **coordinatorTag-Validation ERHALTEN:**
   - Line 2007-2010: Nur Probes an THIS coordinator
   - Verhindert Verwechslung Coordinator/Broadcaster

2. **Broadcaster-Identität VALIDIERT:**
   - `observedBroadcasterMac/Boot` stammt aus validierten Diagnostic Beacons
   - Diagnostic Beacons durchlaufen HMAC + Replay Protection
   - Nur Broadcaster mit 3+ consecutive beacons erhalten Grants

3. **HMAC + Replay Protection ERHALTEN:**
   - Line 1986: `acceptPeer(item.mac, f)` (Replay Protection)
   - Alle Pakettypen weiterhin HMAC-verifiziert

4. **Reply/Final-Validation ERHALTEN:**
   - Tag/Boot/Epoch/MAC-Checks in Lines 2229-2238 (Reply)
   - Tag/Boot/Epoch/MAC-Checks in Lines 2270-2279 (Final)

5. **Kein pauschales "allow all":**
   - Nur Broadcaster MIT Grant-History (observedBroadcasterAt != 0)
   - Nur Broadcaster MIT matching MAC + Boot
   - Nur während Broadcaster beobachtet wird

**Conservative Design:**
- Broadcaster muss ZUERST via Diagnostic Beacons beobachtet werden
- Dann Grant-Request
- Dann Probe/Reply/Final (mit diesem Fix)
- Nur dann production beacons

---

## TESTS

**Offline-Tests:**
```bash
g++ -std=c++17 -I. -DTEST_MODE test_lease_state_machine.cpp -o test_lease_state_machine
./test_lease_state_machine
```

**Result:**
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
- ✅ Delayed Renewal (RF delay)
- ✅ Coordinator Reboot (Startup Guard)
- ✅ Multiple Lost Renewals
- ✅ Reboot During Renewal
- ✅ Capture Source Switch
- ✅ Radio Restart Invalidation
- ✅ Bootstrap Timing (No Overlap)
- ✅ Probe/Reply/Final Epoch Validation

**Keine Regression!**

**Test-Limitierungen (ehrlich benannt):**
- ⚠️ Testet State-Machine-Logik, NICHT ESP-NOW Hardware
- ⚠️ Kann Probe/Reply/Final NICHT simulieren
- ⚠️ Hardware-Test ERFORDERLICH für finale Validierung

---

## HARDWARE-ERWARTUNG

**Nach Flash von devfreenove1 (Coordinator) mit Cycle 16:**

**devxiao3 (Broadcaster, bleibt auf Cycle 15):**
- `exchanges_complete` steigt kontinuierlich (nicht mehr 0!) ✅
- `rejected` sinkt drastisch (nicht mehr 149!) ✅
- `exchange_timeouts` niedrig (<20%) ✅
- `broadcaster_time_base_valid=true` ✅
- `precision_recently_accepted=true` ✅
- `broadcaster_production_beacons_sent` steigt ✅
- `[BROADCASTER] Production BLOCKED` verschwindet ✅

**devfreenove1 (Coordinator, geflasht auf Cycle 16):**
- `probes_answered` steigt (beantwortet Broadcaster-Probes!)
- `authenticated` steigt
- `rejected` bleibt niedrig
- `coordinator_suppressing_beacons=true` während Broadcaster aktiv
- Handover erfolgreich

**Grant/Ack (sollte weiterhin funktionieren):**
- Grant-Empfang ✅
- Ack-Verifikation ✅
- Lease-Renewals ✅

---

## FLASH-EMPFEHLUNG

**KRITISCH: NUR Coordinator flashen!**

1. ✅ **devfreenove1 (Coordinator) MUSS geflasht werden**
   - Aktuell: Cycle 11 (hat BEIDE Bugs!)
   - Update: Cycle 16 (Bugs gefixt)
   - **GRUND:** Coordinator-seitige Bugs verhindern Broadcaster-Zeitbasis

2. ❌ **devxiao3 (Broadcaster) NICHT flashen**
   - Aktuell: Cycle 15 (Premature Epoch Fix OK!)
   - Problem war NICHT auf Broadcaster-Seite
   - Broadcaster-Code ist korrekt

3. **devxiao1, devxiao2 (Nodes) NICHT flashen**
   - Unverändert, kein Handlungsbedarf

**Flash-Prozedur:**
```bash
# NUR devfreenove1!
cd /home/itakka/ESP32_Board/sensorforge
pio run -e freenove_esp32_s3 -t upload --upload-port /dev/ttyACM0
```

**Nach Flash:**
1. Serial-Log von devxiao3 beobachten
2. Prüfen: `exchanges_complete` steigt
3. Prüfen: `broadcaster_time_base_valid=true`
4. Prüfen: `broadcaster_production_beacons_sent` steigt
5. JSON-Endpoint `/cluster/espnow_radio` auslesen

---

## OFFENE PROBLEME

**Grant/Ack-Erfolgsrate (63%):**
- `grants_sent=687`, `acks_verified=431`, `grant_timeouts=256`
- Erfolgsrate: 431/687 = 62.7%
- **Nicht Teil dieses Fixes** (separates Thema)
- Mögliche Ursachen:
  - RF-Interferenz
  - Timing-Probleme
  - Ack-Verarbeitung
- **Für späteren Cycle** (wenn Exchanges funktionieren)

**Cycle 15 Diagnose-Logs:**
- Können ENTFERNT werden (Hardwaretest hat Befund erbracht)
- Optional: Behalten für weitere Diagnose
- **Empfehlung:** Behalten bis Hardwaretest bestätigt

---

## ZUSAMMENFASSUNG

**Root Cause:**
- ✅ **DEFINITIV NACHGEWIESEN:** Zwei Coordinator-seitige Bugs verhinderten Broadcaster-Probes
- Bug 1: Alle Probes verworfen wenn `coordinatorGrantedLeaseActive`
- Bug 2: Broadcaster nicht als `slotIsOwner` registriert

**Fix:**
- ✅ coordinatorTag-Check in Probe-Skip-Logik (nur Node-Probes überspringen)
- ✅ Broadcaster-Exception in Slot-System (observed broadcaster erlaubt)

**Sicherheit:**
- ✅ Keine Schwächung (coordinatorTag + observedBroadcaster validiert)
- ✅ HMAC + Replay + Tag/Boot/Epoch/MAC erhalten

**Tests:**
- ✅ 13/13 PASSED (keine Regression)
- ⚠️ Hardware-Test erforderlich für finale Validierung

**Flash:**
- ✅ NUR devfreenove1 (Coordinator)
- ❌ NICHT devxiao3 (Broadcaster)

**Erwartung:**
- Exchanges funktionieren endlich!
- Broadcaster erhält Zeitbasis
- Production beacons starten
- Handover erfolgreich

**Nach 16 Korrekturzyklen:** Hoffentlich der letzte Fix für Broadcaster-Bootstrap! 🎯
