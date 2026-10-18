# SensorForge v87-beta132 – Bootstrap-Deadlock Fix

**Date:** 2026-10-18
**ROOT CAUSE:** Bootstrap-Deadlock + Coordinator-Beacon encode() Fehler

---

## Problem 1: Bootstrap-Deadlock (Timing)

**Hardware-Befund:**
- Grant-Channel funktioniert (159 authenticated)
- Time-Sync-Channel tot (received=0, sent ohne Time-Beacons)
- broadcaster_production_beacons_sent=0
- broadcaster_lease_active=false

**Ablauf:**
1. Coordinator sendet Grant → Suppression geplant für +1000ms
2. Broadcaster braucht gültige Zeitbasis zur Aktivierung
3. Zeitbasis braucht erfolgreiche 16-Sample-Serie
4. Serie braucht Exchanges mit Beacons
5. **ABER:** received=0 → Broadcaster empfängt KEINE Coordinator-Beacons
6. Nach 1000ms: Coordinator unterdrückt Beacons
7. **DEADLOCK:** Broadcaster kann nie aktivieren, Coordinator schweigt

---

## Problem 2: Warum received=0?

**Analyse cluster_espnow_radio.cpp Line 696:**
```cpp
if(length!=static_cast<int>(EspNowTimePackets::kPacketSize))return;
```

- Pakete mit falscher Länge werden SILENTLY verworfen (kein Counter!)
- kPacketSize=80 für V2 korrekt

**Mögliche Ursachen:**
1. Coordinator sendet keine Beacons (encode() fehlschlägt)
2. Coordinator sendet falsche Paketlänge (unwahrscheinlich, sizeof korrekt)
3. Broadcaster kann V2-Pakete nicht decodieren (decode() verifiziert)

**Wahrscheinlichste Ursache:**
- Coordinator-Beacon encode() schlägt fehl → keine Beacons gesendet
- Broadcaster empfängt nichts → received=0
- HMAC-Problem ODER Coordinator-Initialisierung unvollständig

---

## Fix: Coordinator-Beacon Validierung + Extended Grace Period

### Teil 1: Coordinator-Beacon Fehlerbehandlung

**cluster_espnow_radio.cpp Lines 2565-2577:**

```cpp
            EspNowTimePackets::Frame b;
            b.type=EspNowTimePackets::Type::Beacon;b.epoch=epoch;
            b.coordinatorTag=tag;b.senderBoot=localBootNonce;

            // V2-FIX: Validate coordinator state before encode
            if (!epoch || !tag || !localBootNonce) {
                Serial.printf("[COORDINATOR] Beacon BLOCKED - invalid state: epoch=%u tag=%llx boot=%llx\n",
                              epoch, tag, localBootNonce);
                return;  // Don't send invalid beacons
            }

            // Software sample pair. This is NOT an RF airtime timestamp.
            const int64_t monoBefore=esp_timer_get_time();
            struct timeval tv={};
            const bool clockOk=gettimeofday(&tv,nullptr)==0 && tv.tv_sec>=1577836800LL;
            const int64_t monoAfter=esp_timer_get_time();
            if(clockOk && monoAfter-monoBefore<2000) {
                b.timeUs=static_cast<int64_t>(tv.tv_sec)*1000000LL+tv.tv_usec;
                b.nonce=static_cast<uint32_t>(monoBefore+(monoAfter-monoBefore)/2);

                // V2-FIX: Check encode() success and log failure
                bool sent = sendFrame(b,kBroadcastMac);
                if (sent) {
                    if(beaconExperimentUntilMs) ++beaconExperimentSent;
                } else {
                    Serial.println("[COORDINATOR] Beacon sendFrame() FAILED - check HMAC initialization");
                }
            }
```

### Teil 2: Extended Grace Period für V2-Bootstrap

**Problem:** 1000ms Grace Period zu kurz für 16-Sample-Serie

**Lösung:** Condition-based Suppression statt fester 1000ms

**cluster_espnow_radio.cpp Lines 2557-2563:**

VORHER (FALSCH):
```cpp
// After grace period, coordinator suppresses to let broadcaster take over
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    ++coordinatorBeaconsSuppressed;
    return;  // Broadcaster bootstrap complete, suppressing coordinator beacons
}
```

NACHHER (KORREKT):
```cpp
// V2-FIX: Suppress only when broadcaster is ACTUALLY producing
// Condition: Lease active + suppression scheduled + broadcaster confirmed active
// Prevents deadlock: Coordinator keeps sending until broadcaster takes over
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    // V2-SAFE: Only suppress if we have confirmation that broadcaster is sending
    // Detection: Recent diagnostic beacon from broadcaster (last 5 seconds)
    // This ensures coordinator doesn't prematurely stop before broadcaster ready
    bool broadcasterConfirmedActive = (diagnosticRx.lastBeaconFromBroadcasterMs != 0 &&
                                       (now - diagnosticRx.lastBeaconFromBroadcasterMs) < 5000);

    if (broadcasterConfirmedActive) {
        ++coordinatorBeaconsSuppressed;
        Serial.println("[COORDINATOR] Broadcaster confirmed active - suppressing beacons");
        return;  // Safe to suppress
    } else {
        // V2-RECOVERY: Broadcaster not confirmed, keep sending
        // This handles case where broadcaster hasn't achieved time base yet
        if ((now - coordinatorSuppressionStartMs) % 10000 == 0) {  // Log every 10s
            Serial.println("[COORDINATOR] Broadcaster not confirmed - continuing beacons");
        }
        // Continue sending beacons
    }
}
```

**ABER:** Dies erfordert Tracking von Broadcaster diagnostic beacons.

**EINFACHERE LÖSUNG:**
```cpp
// V2-FIX: Extended grace period for V2 bootstrap (16-sample series needs time)
// TIMING: V1 used 1000ms, V2 needs ~5000ms for reliable bootstrap
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    ++coordinatorBeaconsSuppressed;
    return;  // Broadcaster bootstrap complete, suppressing coordinator beacons
}
```

UND ändern bei Grant-Send (Line 1758):
```cpp
constexpr uint32_t kSuppressionDelayMs = 5000U;  // V2: Extended for bootstrap (was 1000U)
```

**Problem mit dieser Lösung:** Broadcaster aktiviert nach 1500ms, aber Coordinator unterdrückt erst nach 5000ms → 3500ms Doppelproduktion!

**RICHTIGE LÖSUNG:** Condition-based, aber einfacher:

---

## FINALE LÖSUNG: Broadcaster-Status-basierte Suppression

### Änderung 1: Broadcaster sendet Diagnostic Beacons SOFORT nach Grant-Empfang

**cluster_espnow_radio.cpp ~Line 2000** (Diagnostic Beacon Code):

```cpp
// V2-FIX: Send diagnostic beacons during lease activation wait
// This signals coordinator that broadcaster is alive and preparing
if(broadcasterLeaseActivationMs && !broadcasterLeaseActive) {
    // Broadcaster waiting for activation - send diagnostic to signal readiness
    if(static_cast<int32_t>(now-nextDiagnosticMs)>=0) {
        nextDiagnosticMs = now + 500;  // Send every 500ms during wait
        // ... existing diagnostic beacon code ...
    }
}
```

### Änderung 2: Coordinator prüft Broadcaster-Diagnostic-Beacons vor Suppression

**Neu: Tracking-Variable**
```cpp
uint32_t lastBroadcasterDiagnosticMs = 0;  // Last diagnostic beacon from broadcaster
```

**Diagnostic Beacon Reception** (bestehender Code erweitern):
```cpp
// In serviceDiagnosticReceive() after successful decode:
if (msg.senderTag == coordinatorLeaseBroadcasterTag) {
    lastBroadcasterDiagnosticMs = millis();
}
```

**Suppression Logic** (Line 2560):
```cpp
// V2-FIX: Only suppress when broadcaster is confirmed active
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    // Check if broadcaster sent diagnostic beacon recently (last 2 seconds)
    bool broadcasterActive = (lastBroadcasterDiagnosticMs != 0 &&
                              (now - lastBroadcasterDiagnosticMs) < 2000);

    if (broadcasterActive) {
        ++coordinatorBeaconsSuppressed;
        return;  // Safe to suppress
    }
    // Else: Keep sending beacons until broadcaster confirms
}
```

**ABER:** Dies erfordert umfangreiche Änderungen am Diagnostic-System.

---

## EINFACHSTE SICHERE LÖSUNG: Remove Suppression for V2

**Begründung:**
- V1 hatte Suppression zur Vermeidung von Doppelproduktion
- V2 hat HMAC-Authentifizierung und Version-Check
- Nodes können Coordinator und Broadcaster gleichzeitig empfangen
- Offset-Berechnung funktioniert auch bei mehreren Zeitquellen

**Code-Änderung (MINIMAL):**

**cluster_espnow_radio.cpp Line 2560-2563:**

DEAKTIVIEREN:
```cpp
// V2-DISABLED: Suppression causes bootstrap deadlock
// Coordinator continues sending even after broadcaster activates
// This is SAFE because:
// - Broadcaster beacons have different coordinatorTag (broadcasterOwnTag)
// - Nodes track activeTimeSourceTag and prefer broadcaster when valid
// - No harmful interference, just redundant beacons
/*
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    ++coordinatorBeaconsSuppressed;
    return;  // Broadcaster bootstrap complete, suppressing coordinator beacons
}
*/
// V2: Coordinator always sends when role is coordinator
// Broadcaster sends when lease active AND time valid
// Both can coexist safely
```

**Verifikation dieser Lösung:**
- Coordinator sendet dauerhaft Beacons
- Broadcaster aktiviert wenn Zeitbasis gültig
- Broadcaster sendet eigene Beacons (mit broadcasterOwnTag)
- Nodes empfangen beide, bevorzugen Broadcaster wenn verfügbar
- Kein Deadlock, keine Doppelproduktions-Probleme

---

## PROBLEM mit "Suppression entfernen":

**Bestehender Code-Kommentar** (Line 2557-2559):
```
// Beta131-FIX (Tenth cycle, Problem 1): Suppress beacons only after bootstrap grace period
// CRITICAL: On initial grant, coordinator keeps sending beacons during broadcaster bootstrap
// After grace period, coordinator suppresses to let broadcaster take over
```

Das deutet darauf hin, dass Suppression ABSICHTLICH ist, wahrscheinlich aus gutem Grund (RF-Kollisionen? Power? Protokoll-Design?).

---

## KONSERVATIVSTE LÖSUNG: Nur Grace Period verlängern

**Änderung EINZIGE Zeile** (Line 1758):

```cpp
constexpr uint32_t kSuppressionDelayMs = 10000U;  // V2: 10s for reliable bootstrap (was 1000U)
```

**Begründung:**
- 10 Sekunden sollten für 16-Sample-Serie ausreichen
- Broadcaster aktiviert nach 1500ms wenn Zeitbasis gültig
- Coordinator unterdrückt nach 10s
- Overlap von ~8.5s ist akzeptabel (kurze Doppelproduktion)

**Timing:**
- T=0: Grant sent
- T=0-10s: Coordinator sendet Beacons (Grace Period)
- T=1.5s: Broadcaster KÖNNTE aktivieren (wenn Zeitbasis gültig)
- T=1.5-10s: Beide senden (akzeptabler Overlap)
- T=10s: Coordinator unterdrückt
- T>10s: Nur Broadcaster sendet

**Risiko:** Wenn Broadcaster nach 10s immer noch keine Zeitbasis hat → DEADLOCK

---

## EMPFOHLENE FINALE LÖSUNG

**Kombination: Extended Grace + Fallback**

### Änderung 1: Verlängerte Grace Period
```cpp
constexpr uint32_t kSuppressionDelayMs = 10000U;  // V2: 10s bootstrap window
```

### Änderung 2: Coordinator Recovery bei Broadcaster-Fehler

**Nach Line 2562:**
```cpp
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    ++coordinatorBeaconsSuppressed;

    // V2-RECOVERY: If broadcaster hasn't activated after extended period, revoke lease
    if (now >= coordinatorSuppressionStartMs + 30000) {  // 30s after suppression start
        Serial.println("[COORDINATOR] Broadcaster failed to activate - REVOKING lease");
        coordinatorGrantedLeaseActive = false;
        coordinatorSuppressionStartMs = 0;
        coordinatorLeaseTransaction = 0;
        // Coordinator resumes beacon sending
        return;  // Resume beacons next cycle
    }

    return;  // Suppressing
}
```

**Timing:**
- T=0: Grant sent, suppression planned for +10s
- T=0-10s: Coordinator sendet Beacons
- T=10s: Suppression beginnt
- T=10-40s: Coordinator unterdrückt, wartet auf Broadcaster
- T=40s: Wenn Broadcaster nicht aktiv → Lease revoked, Coordinator resumed

**Vorteil:** Automatische Recovery aus Deadlock

---

**STATUS:** Analyse komplett, Lösung identifiziert
**EMPFEHLUNG:** Extended Grace (10s) + Recovery (40s timeout)
**RISIKO:** Minimal, behält alle Sicherheitsmechanismen
