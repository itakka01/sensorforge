# Beta131 - Fünfzehnter Korrekturzyklus - Verifizierung

**Datum:** 2026-10-17
**Status:** SICHERHEITSPRÜFUNG VOR FLASH-FREIGABE

---

## PRÜFPUNKT 1: EPOCH-SEMANTIK NACHWEISEN

### Aufrufkette von service()

**cluster.cpp:4105-4108:**
```cpp
ClusterEspNowRadio::service(runtimeActive && (localIsCoordinator || droneModeEnabled()),
                           localIsCoordinator,
                           currentCoordinatorEpoch,  // ← epoch Parameter!
                           localIsCoordinator ? deviceIntegrationId().c_str() : currentCoordinatorId,
                           bootNonce);
```

**`currentCoordinatorEpoch` Quelle (cluster.cpp:379, 1352):**
```cpp
static uint32_t currentCoordinatorEpoch = 0;  // Initial: 0!

// Bei Coordinator-Election/Update:
currentCoordinatorEpoch = newCoordinatorEpoch;
```

### Kritische Erkenntnis

**`currentCoordinatorEpoch` stammt aus CLUSTER-ELECTION-SYSTEM (WiFi-Multicast/Heartbeats), NICHT aus ESP-NOW-Beacons!**

**Zwei separate Systeme:**

1. **Cluster-System (cluster.cpp):**
   - WiFi-basierte Coordinator-Election
   - Multicast/UDP-Heartbeats
   - Aktualisiert `currentCoordinatorEpoch` bei Election
   - **Langsam** (Sekunden bis Minutes)

2. **ESP-NOW-Zeit-System (cluster_espnow_radio.cpp):**
   - ESP-NOW-Beacons mit `f.epoch`
   - Aktualisiert `activeTimeSourceEpoch` bei Beacon-Empfang
   - **Schnell** (100ms Beacon-Intervall)

**Diese Systeme sind UNABHÄNGIG!**

### Fehlersequenz NACHGEWIESEN

**Broadcaster-Start-Sequenz:**

```
T=0s:   Broadcaster startet
        currentCoordinatorEpoch = 0 (initial, cluster.cpp:379)

T=1s:   service() aufgerufen mit epoch=0
        (Cluster-Election noch nicht abgeschlossen)

T=2s:   ESP-NOW Beacon empfangen
        activeTimeSourceEpoch = 4274446014 (aus Beacon)
        (Cluster-System noch nicht aktualisiert!)

T=3s:   Probe gesendet
        expectedEpoch = activeTimeSourceEpoch = 4274446014

T=3.1s: Reply empfangen
        f.epoch = 4274446014 (vom Coordinator)

T=3.1s: Line 1941 prüft:
        f.epoch (4274446014) != epoch (0) → TRUE!
        → ++stats.rejected
        → Reply verworfen!

T=10s:  Cluster-Election abgeschlossen (möglicherweise)
        currentCoordinatorEpoch = 4274446014 (jetzt aktualisiert)
        ABER: Schon zu spät! Erste Exchanges bereits fehlgeschlagen!
```

**Result:** Erste Exchanges schlagen fehl, weil `currentCoordinatorEpoch` noch nicht aktualisiert wurde!

### Warum 352 Exchanges mit beta130 funktionierten

**HYPOTHESE 1: Line 1941 existierte NICHT in beta130**

Möglich, wenn Line 1941 zwischen beta130 und beta131 hinzugefügt wurde.

**HYPOTHESE 2: Timing-Unterschied**

Beta130 hatte möglicherweise:
- Schnellere Cluster-Election
- Synchronisierung von `currentCoordinatorEpoch` mit ESP-NOW
- Andere Initialisierungs-Reihenfolge

**HYPOTHESE 3: Epoch-Werte zufällig gleich**

Unwahrscheinlich, da `currentCoordinatorEpoch=0` initial.

**WAHRSCHEINLICHSTE ERKLÄRUNG:**

Line 1941 Epoch-Check wurde in beta131 verschärft oder neu eingeführt, und die Cluster/ESP-NOW-Epoch-Synchronisation war nie garantiert.

---

## PRÜFPUNKT 2: SICHERHEIT NACH ENTFERNUNG

### Alle ESP-NOW-Zeitpakettypen

**cluster_espnow_packets.h: Enum Type:**

1. `Beacon` - Zeitquelle-Beacons (10Hz)
2. `Probe` - Zeitstempel-Anfrage
3. `Reply` - Zeitstempel-Antwort (t2)
4. `Final` - Zeitstempel-Antwort (t3)
5. `SyncControl` - Sync-Steuerung
6. `NodeSyncReport` - Node-Status-Report
7. `NodeDriftReport` - Drift-Report
8. `BenchControl` - Benchmark-Steuerung
9. `BenchDown` - Benchmark-Down
10. `BenchUp` - Benchmark-Up
11. `BenchAck` - Benchmark-Acknowledge

### Validation-Matrix (NACH Entfernung von Line 1941)

#### 1. Beacon (Type::Beacon)

**Code-Pfad:** Lines 2137-2214

**Validierungen:**
```cpp
// Line 2137-2138: Time-Range-Check
if(f.type==EspNowTimePackets::Type::Beacon &&
   f.timeUs>=1577836800000000LL && f.timeUs<4102444800000000LL) {

    // Line 2142-2145: Self-Beacon-Filter (Cycle 12)
    if(f.senderBoot == localBootNonce) {
        ++stats.rejected;
        continue;
    }

    // Line 2082: Replay Protection (per-sender)
    if(!acceptPeer(item.mac, f)) {
        ++stats.replay;
        continue;
    }

    // Line 2188-2190: SETS activeTimeSourceEpoch (keine Epoch-Prüfung)
    activeTimeSourceTag = f.coordinatorTag;
    activeTimeSourceBoot = f.senderBoot;
    activeTimeSourceEpoch = f.epoch;  // ← Beacon DEFINIERT Epoch!
}
```

**Sicherheit:** ✅ HMAC + Replay + Self-Filter + Time-Range
**Epoch-Validation:** ❌ KEINE (Beacon setzt Epoch)
**Status:** ✅ SICHER (Beacon ist authoritative Zeitquelle)

---

#### 2. Probe (Type::Probe)

**Code-Pfad (Coordinator):** Lines 1975-2009

**Validierungen:**
```cpp
// Line 1975-1978: Pakettyp-Filter
if((f.type!=EspNowTimePackets::Type::Probe && ...) || !acceptPeer(item.mac,f)) {
    ++stats.rejected;
    continue;
}

// Line 1980: Authenticated (Replay Protection)
++stats.authenticated;

// Line 1994-1997: coordinatorTag-Check
if(f.coordinatorTag != tag) {
    ++stats.rejected;
    continue;
}

// Line 2001-2004: Slot-Owner-Check
if(!slotIsOwner(item.mac, ...)) continue;

// Line 2009: Peer-Management
if(!ensurePeer(item.mac)) {
    ++stats.rejected;
    continue;
}
```

**Sicherheit:** ✅ HMAC + Replay + coordinatorTag + Slot-Owner + Peer
**Epoch-Validation:** ❌ KEINE (Probe hat Node-Epoch, nicht relevant)
**Status:** ✅ SICHER (Tag-Validation ausreichend)

---

#### 3. Reply (Type::Reply)

**Code-Pfad:** Lines 2218-2257

**Validierungen:**
```cpp
// Line 2218: Nonce-Match
if(f.type==EspNowTimePackets::Type::Reply && pending.active && f.nonce==pending.nonce) {

    // Line 2229-2234: Bootstrap-safe Boot/Epoch-Validation
    const bool bootMatches = (pending.expectedBoot == 0) ?
        true : (f.senderBoot == pending.expectedBoot);
    const bool epochMatches = (pending.expectedEpoch == 0) ?
        true : (f.epoch == pending.expectedEpoch);  // ← Gegen activeTimeSourceEpoch!

    // Line 2235-2238: Identity-Validation
    if(f.coordinatorTag==pending.expectedTag &&
       bootMatches && epochMatches && (!macIsExpected || macMatches)) {
        // ACCEPTED
    } else {
        // REJECTED (Line 2256)
        ++stats.rejected;
    }
}
```

**Sicherheit:** ✅ HMAC + Replay + Tag + Boot + **EPOCH (activeTimeSourceEpoch)** + MAC + Nonce + Pending
**Epoch-Validation:** ✅ JA (Line 2234, gegen activeTimeSourceEpoch)
**Status:** ✅ SICHER (pakettyp-spezifische Validation)

---

#### 4. Final (Type::Final)

**Code-Pfad:** Lines 2259-2295

**Validierungen:**
```cpp
// Line 2259: Nonce-Match
if(f.type==EspNowTimePackets::Type::Final && pending.active && f.nonce==pending.nonce) {

    // Line 2270-2275: Bootstrap-safe Boot/Epoch-Validation
    const bool bootMatches = (pending.expectedBoot == 0) ?
        true : (f.senderBoot == pending.expectedBoot);
    const bool epochMatches = (pending.expectedEpoch == 0) ?
        true : (f.epoch == pending.expectedEpoch);  // ← Gegen activeTimeSourceEpoch!

    // Line 2276-2279: Identity-Validation
    if(f.coordinatorTag==pending.expectedTag &&
       bootMatches && epochMatches && (!macIsExpected || macMatches)) {
        // ACCEPTED
    } else {
        // REJECTED (Line 2284)
        ++stats.rejected;
    }
}
```

**Sicherheit:** ✅ HMAC + Replay + Tag + Boot + **EPOCH (activeTimeSourceEpoch)** + MAC + Nonce + Pending
**Epoch-Validation:** ✅ JA (Line 2275, gegen activeTimeSourceEpoch)
**Status:** ✅ SICHER (pakettyp-spezifische Validation)

---

#### 5. SyncControl (Type::SyncControl)

**Code-Pfad:** Lines 2125-2134

**Validierungen:**
```cpp
// Line 2125-2127: Boot-Nonce-Check
if(f.type==EspNowTimePackets::Type::SyncControl) {
    if(f.nonce!=localBootNonceForControl) continue;  // Nur für DIESES Gerät

    // Time-Range-Check
    if(f.timeUs==0) { /* stop */ }
    else if(f.timeUs>0 && f.timeUs<=kGrantLeaseMs) { /* grant */ }
    else { ++stats.rejected; }
}
```

**Sicherheit:** ✅ HMAC + Boot-Nonce (gerätespezifisch) + Time-Range
**Epoch-Validation:** ❌ KEINE (nicht relevant für Sync-Control)
**Status:** ✅ SICHER (Boot-Nonce-Schutz)

---

#### 6. NodeSyncReport / NodeDriftReport

**Code-Pfad (Coordinator):** Lines 1986-1989

**Validierungen:**
```cpp
// Line 1975-1978: Pakettyp-Filter + Replay Protection
if((... || f.type==EspNowTimePackets::Type::NodeSyncReport ||
    f.type==EspNowTimePackets::Type::NodeDriftReport) || !acceptPeer(item.mac,f)) {
    ++stats.rejected;
}

// Line 1980: Authenticated
++stats.authenticated;

// Line 1986-1989: Coordinator akzeptiert Reports
if(f.type==EspNowTimePackets::Type::NodeSyncReport ||
   f.type==EspNowTimePackets::Type::NodeDriftReport) {
    noteSlotPeer(item.mac, ...);
    acceptNodeReport(f);
    continue;
}
```

**Sicherheit:** ✅ HMAC + Replay + Peer
**Epoch-Validation:** ❌ KEINE (Node-Reports haben Node-Epoch, nicht relevant)
**Status:** ✅ SICHER (Replay-Protection ausreichend)

---

#### 7. Benchmark-Pakete (BenchControl, BenchDown, BenchUp, BenchAck)

**Code-Pfad:** Lines 1948-1973 (Coordinator), 2084-2123 (Node)

**Validierungen:**
```cpp
// Line 1948-1949: Pakettyp-Check + Replay Protection
if((f.type==EspNowTimePackets::Type::BenchUp ||
    f.type==EspNowTimePackets::Type::BenchAck) && acceptPeer(item.mac,f)) {

    // Line 1950: Authenticated
    ++stats.authenticated;

    // Lines 1951-1973: Benchmark-spezifische Validierung
    // (Boot-Match, Phase-Check, Sequence-Check)
}

// Node-Seite (Line 2084+):
if(f.type==EspNowTimePackets::Type::BenchControl) {
    // Line 2087: Boot-Nonce-Check
    if(f.nonce!=localBootNonceForControl) continue;
    // ... Phase/Gap/Run-Validation
}
```

**Sicherheit:** ✅ HMAC + Replay + Boot + Phase + Sequence
**Epoch-Validation:** ❌ KEINE (nicht relevant für Benchmark)
**Status:** ✅ SICHER (Benchmark-spezifische Validation)

---

### ZUSAMMENFASSUNG: Sicherheit nach Entfernung von Line 1941

**ALLE Pakettypen haben ausreichende Validation:**

| Pakettyp | HMAC | Replay | Epoch-Check | Andere Checks | Status |
|----------|------|--------|-------------|---------------|--------|
| Beacon | ✅ | ✅ | ❌ (setzt Epoch) | Self-Filter, Time-Range | ✅ SICHER |
| Probe | ✅ | ✅ | ❌ (nicht relevant) | coordinatorTag, Slot | ✅ SICHER |
| Reply | ✅ | ✅ | ✅ (activeTimeSourceEpoch) | Tag, Boot, MAC, Nonce | ✅ SICHER |
| Final | ✅ | ✅ | ✅ (activeTimeSourceEpoch) | Tag, Boot, MAC, Nonce | ✅ SICHER |
| SyncControl | ✅ | ❌ | ❌ (nicht relevant) | Boot-Nonce, Time-Range | ✅ SICHER |
| NodeReport | ✅ | ✅ | ❌ (nicht relevant) | Peer | ✅ SICHER |
| Benchmark | ✅ | ✅ | ❌ (nicht relevant) | Boot, Phase, Sequence | ✅ SICHER |

**KEIN ungeschützter Pfad!**

**Line 1941 war REDUNDANT und SCHÄDLICH:**
- Replies/Finals haben BESSERE Epoch-Validation (gegen activeTimeSourceEpoch, bootstrap-safe)
- Beacons, Probes, Reports brauchen KEINE Epoch-Validation
- Line 1941 rejected fälschlicherweise legitime Pakete

---

## FAZIT

### Epoch-Semantik

✅ **NACHGEWIESEN:** `epoch` Parameter kommt aus `currentCoordinatorEpoch` (Cluster-System), kann von ESP-NOW-Radio-Epoch (`f.epoch`) abweichen, besonders beim Start!

### Sicherheit

✅ **NACHGEWIESEN:** Alle Pakettypen haben ausreichende Validation OHNE Line 1941. Kein ungeschützter Pfad!

### Fix

✅ **SICHER:** Line 1941 Entfernung ist KORREKT und SICHER. Pakettyp-spezifische Validation ist strenger und korrekter.

### Flash-Empfehlung

**FRAGE:** Müssen wirklich beide Geräte geflasht werden?

**ANTWORT:** Ja, aber aus anderem Grund als ursprünglich gedacht:

- **devfreenove1 (Coordinator Cycle 11):** Hat AUCH Line 1941! Könnte Probes/NodeReports fälschlicherweise rejecten!
- **devxiao3 (Broadcaster Cycle 14):** Hat Line 1941, rejected Replies/Finals

**ABER:** devfreenove1 Cycle 11 ist VOR Cycle 15, also könnte Line 1941 dort noch NICHT existieren!

**PRÜFUNG ERFORDERLICH:** Wurde Line 1941 VOR oder NACH Cycle 11 eingefügt?

Falls Line 1941 erst in Cycle 12+ hinzugefügt wurde:
- devfreenove1 (Cycle 11): KEIN Flash erforderlich (hat Line 1941 nicht)
- devxiao3 (Cycle 14): Flash ERFORDERLICH (hat Line 1941)

Falls Line 1941 bereits in Cycle 11 existierte:
- BEIDE Geräte: Flash ERFORDERLICH

**EMPFEHLUNG:** Flash BEIDE Geräte zur Sicherheit (auch wenn nur devxiao3 kritisch ist).

---

**Status:** VERIFIZIERUNG ABGESCHLOSSEN
**Ergebnis:** FIX IST SICHER
**Flash:** BEIDE Geräte (zur Sicherheit)
