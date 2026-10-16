# Beta 131 - Kritische Probleme und Korrekturen

**Datum:** 2026-10-16
**Status:** KORREKTUREN ERFORDERLICH

---

## 🔴 KRITISCHE PROBLEME IDENTIFIZIERT

### Problem 1: Race Condition bei Senderübergabe (KRITISCH!)

**Aktueller Code (FEHLERHAFT):**
```cpp
// Zeile 1230-1237: Broadcaster aktiviert SOFORT
broadcasterLeaseActive=true;
broadcasterLeaseExpiresMs=now+m.leaseMs;

// Zeile 1239-1244: Broadcaster sendet Ack DANACH
m.action=ClusterTimeGrantProtocol::Action::Ack;
esp_now_send(item.mac,wire,sizeof(wire));

// Zeile 1484: Production-Beacons werden SOFORT gesendet
if(broadcasterLeaseActive) {
    // Sendet SOFORT!
}
```

**Timing-Ablauf (PROBLEM):**
1. T+0ms: Coordinator sendet Grant
2. T+5ms: Broadcaster empfängt Grant → **aktiviert SOFORT Production-Beacons** (10 Hz = alle 100ms)
3. T+6ms: Broadcaster sendet Ack
4. T+10ms: Broadcaster sendet ersten Production-Beacon
5. T+15ms: Coordinator empfängt Ack → stoppt erst JETZT seine Beacons
6. T+0-1000ms: Coordinator sendet noch seinen 10s-Beacon

**→ BEIDE SENDEN GLEICHZEITIG!**

**Auswirkung:** Nodes empfangen konkurrierende Beacons, estimation wird beschädigt.

**KORREKTUR:**
Broadcaster wartet nach Ack-Sendung eine Guard-Zeit, bevor Production-Beacons starten:

```cpp
// Nach Ack-Sendung: Setze delayed activation
broadcasterLeaseActivationMs = now + 500;  // 500ms Guard-Zeit
broadcasterLeaseExpiresMs = now + m.leaseMs;

// In serviceDiagnosticBroadcaster():
if(broadcasterLeaseActivationMs && now >= broadcasterLeaseActivationMs) {
    broadcasterLeaseActive = true;
    broadcasterLeaseActivationMs = 0;
    Serial.println("[BROADCASTER] Production beacons ACTIVATED after guard time");
}
```

---

### Problem 2: Broadcaster hat keine präzise Zeitbasis (KRITISCH!)

**Aktueller Code (FEHLERHAFT):**
```cpp
void serviceDiagnosticBroadcaster(...) {
    diagnosticMode=true;  // Broadcaster ist im Diagnostic-Mode
    wasCoordinator=false;

    // Production-Beacons verwenden nur esp_timer_get_time()
    prodBeacon.timeUs=esp_timer_get_time();
}
```

**Problem:**
- Broadcaster ist im `diagnosticMode`, ruft `precisionUtcNow()` NICHT auf
- Broadcaster synchronisiert sich NICHT über ESP-NOW mit Coordinator
- `esp_timer_get_time()` ist monoton, aber NICHT cluster-synchronisiert!
- Broadcaster sendet Beacons ohne präzise Zeitbasis

**Auswirkung:** Nodes können sich nicht präzise mit Broadcaster synchronisieren, weil seine Zeitbasis unpräzise ist.

**KORREKTUR:**
Broadcaster muss sich ZUERST als normaler Node mit Coordinator synchronisieren, BEVOR er Production-Beacons sendet:

```cpp
// Broadcaster braucht:
1. Probe/Reply/Final-Exchange mit Coordinator durchführen
2. Valide latestQualityEstimate bekommen
3. Dann erst Production-Beacons senden mit präziser Zeitbasis

// Production-Beacon sollte verwenden:
prodBeacon.timeUs = coordinatorMonotonicEstimate + offsetUs + driftCorrection;
```

**ABER:** Das ist komplexer, weil Broadcaster gleichzeitig Node (empfängt) und Coordinator (sendet) sein muss.

**EINFACHERE LÖSUNG:**
Broadcaster sollte sich im NORMALEN Mode (nicht diagnostic) befinden und reguläre ESP-NOW-Synchronisation durchführen. Nur die Beacon-SENDUNG ist speziell.

---

### Problem 3: Enrollment-System überengineered

**Aktueller Code (UNNÖTIG KOMPLEX):**
```cpp
const bool keyConfigured = (strlen(SF_GRANT_PAIR_KEY_HEX) == 64);
const uint64_t enrolledCoordinatorTag = idTag(SF_GRANT_COORDINATOR_ID);
const uint64_t enrolledBroadcasterTag = idTag(SF_GRANT_BROADCASTER_ID);
```

**Problem:**
- Separates Enrollment-System mit manueller Konfiguration
- User sagt: "cluster_grant_pair_config.h war bereits einmal verworfen"
- Cluster hat BEREITS Authentifizierung (clusterTimeGrantSign/Verify)
- Grant/Ack sind bereits HMAC-gesichert mit Cluster-Key!

**Bestehende Sicherheit:**
- Grant/Ack verwenden `clusterTimeGrantSign/Verify`
- Diese nutzen PSA-Cluster-Credentials
- Jedes authentifizierte Grant ist bereits sicher!

**KORREKTUR:**
Enrollment-Prüfung ENTFERNEN. Stattdessen:
1. Grant/Ack-HMAC ist ausreichend (bereits implementiert)
2. Broadcaster-Rolle wird über `time_broadcaster` Mode identifiziert
3. Coordinator prüft nur: "Hat Broadcaster-Rolle UND authentifiziertes Ack"

---

### Problem 4: Node Source-Switching zu restriktiv

**Aktueller Code (FEHLERHAFT):**
```cpp
const bool keyConfigured = (strlen(SF_GRANT_PAIR_KEY_HEX) == 64);
const uint64_t enrolledBroadcasterTag = keyConfigured ? idTag(SF_GRANT_BROADCASTER_ID) : 0;
const bool fromBroadcaster = (keyConfigured && f.coordinatorTag == enrolledBroadcasterTag);

if(!fromCoordinator && !fromBroadcaster){
    ++stats.rejected;
    continue;
}
```

**Problem:**
- Wenn `SF_GRANT_PAIR_KEY_HEX` leer (Default), ist `enrolledBroadcasterTag = 0`
- Nodes können KEINE Broadcaster-Beacons akzeptieren!
- System funktioniert nur mit manueller Enrollment-Konfiguration

**KORREKTUR:**
Nodes sollten Beacons von JEDEM authentifizierten Sender akzeptieren, der denselben Epoch hat:

```cpp
// Beacon ist bereits HMAC-verifiziert (Zeile 1555)
// Epoch-Prüfung ist bereits erfolgt (Zeile 1556)
// Das ist ausreichend!

// Prüfe nur: Ist es der aktuelle Coordinator ODER ein anderer authentifizierter Sender?
const bool fromCurrentCoordinator = (f.coordinatorTag == tag);
const bool fromOtherAuthenticatedSource = (f.coordinatorTag != tag);

// BEIDE sind OK, solange HMAC und Epoch stimmen
```

---

### Problem 5: precisionUtcNow() Aufruf-Kontext

**Potentielles Problem:**
```cpp
// cluster.cpp ruft vermutlich auf:
ClusterEspNowRadio::precisionUtcNow(epoch, coordinatorId, utcUs, age, rtt);
                                           ^^^^^^^^^^^
                                           Immer Coordinator-ID?
```

**Wenn Broadcaster aktiv ist:**
- `lastTag` ist jetzt Broadcaster-Tag
- Aber Aufruf verwendet vermutlich Coordinator-ID
- `idTag(coordinatorId) != lastTag` → Prüfung schlägt fehl!

**KORREKTUR:**
`precisionUtcNow()` sollte prüfen: "Ist coordinatorId der AKTUELLE Quell-Tag?"

```cpp
// Statt:
if(idTag(coordinatorId) != lastTag) return false;

// Besser:
if(idTag(coordinatorId) != activeTimeSourceTag && activeTimeSourceTag != 0) {
    // coordinatorId passt nicht zur aktiven Quelle
    return false;
}
```

---

## 🔧 KORREKTUR-PRIORITÄTEN

### Priorität 1 (MUSS):
1. **Timing-Race beheben** - Guard-Zeit nach Ack
2. **Broadcaster-Zeitbasis** - Entweder volle Synchronisation ODER klar dokumentierte Limitation

### Priorität 2 (SOLLTE):
3. **Enrollment vereinfachen** - Bestehende Cluster-Auth nutzen
4. **Node Source-Switching** - Robuster machen

### Priorität 3 (KANN):
5. **precisionUtcNow() Kontext** - Aktuelle Quelle prüfen

---

## 📋 ENTSCHEIDUNGEN ERFORDERLICH

### Option A: Broadcaster mit voller Synchronisation (KOMPLEX)
- Broadcaster führt Probe/Reply/Final mit Coordinator durch
- Broadcaster hat `latestQualityEstimate` wie normale Node
- Broadcaster sendet Production-Beacons mit präziser Zeitbasis
- **Vorteil:** Präzise Zeitbasis garantiert
- **Nachteil:** Broadcaster ist gleichzeitig Sender und Empfänger (komplex!)

### Option B: Broadcaster ohne Synchronisation (EINFACH, aber LIMITIERT)
- Broadcaster sendet Beacons nur mit `esp_timer_get_time()` (monoton)
- Nodes können relative Sync erreichen (250 µs Ziel)
- Absolute UTC-Genauigkeit limitiert durch `gettimeofday()` (~1-10ms)
- **Vorteil:** Einfache Implementierung
- **Nachteil:** User-Anforderung sagt "Relative Sync ≤ 250 µs" - reicht das?

### Option C: Hybrid (PRAKTISCH)
- Broadcaster synchronisiert sich PASSIV mit Coordinator-Beacons
- Broadcaster sendet Production-Beacons basierend auf dieser Sync
- **Vorteil:** Beste Balance
- **Nachteil:** Broadcaster muss beide Modi gleichzeitig bedienen

---

## ⚠️ EMPFEHLUNG

**Kurzfristig (für beta131-Korrektur):**
1. **Timing-Race beheben** (Guard-Zeit 500ms)
2. **Enrollment entfernen** (Cluster-Auth nutzen)
3. **Node Source-Switching robuster** (HMAC+Epoch reicht)
4. **Broadcaster-Zeitbasis:** Dokumentierte Limitation
   - Production-Beacons nur mit monotoner Zeit
   - User-Test mit GPIO-Kabel entscheidet, ob ausreichend
   - Wenn nicht: Option C in beta132

**Begründung:**
- Timing-Race ist kritischer Fehler → MUSS behoben werden
- Enrollment war Overengineering → Vereinfachen
- Broadcaster-Zeitbasis: Test auf Hardware zeigt, ob Problem besteht
- Pragmatischer Ansatz: Funktionierende Basis, dann iterativ verbessern

---

**Status:** KORREKTUREN WERDEN DURCHGEFÜHRT
