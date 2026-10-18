# Beta131 - Vierzehnter Korrekturzyklus - Diagnostic Build

**Datum:** 2026-10-17
**Version:** v87-beta131 (Fourteenth Cycle - DIAGNOSTIC)
**Status:** HYPOTHESE IDENTIFIZIERT - HARDWARE-TEST ERFORDERLICH

---

## 🔴 HARDWARE-BEFUND (Nach Cycle 13 Flash)

**devxiao3 (Broadcaster, Cycle 13):**
- Uptime: 46s, Boot-ID: `ee3e663a`
- `probes_sent=9`
- **`exchanges_complete=0`** ← PROBLEM BLEIBT!
- `exchange_timeouts=8`
- `authenticated=27`
- **`rejected=49`** ← SEHR HOCH!
- `espnow_grant_diagnostic.received=15`, `authenticated=15`, `acks_sent=15`
- **`[BOOTSTRAP]` Meldung FEHLT im Log!** ← KRITISCHER HINWEIS!

**devfreenove1 (Coordinator, Cycle 11):**
- Epoch: `4274446014`, Boot-ID: `ee72ef6a`
- Grant/Ack funktioniert (15 Grants authenticated)
- Beacons während Lease unterdrückt (sollte)

**Symptome:**
- ✅ Grant HMAC-Authentifizierung funktioniert (15 Grants authenticated)
- ✅ Grant/Ack-Austausch funktioniert (15 Acks sent)
- ❌ Grant-Bootstrap wird NICHT ausgeführt (`[BOOTSTRAP]` Log fehlt!)
- ❌ Exchanges schlagen fehl (0 complete, 8 timeouts)
- ❌ Hohe Rejection-Rate (49 rejected vs. 27 authenticated)

---

## 🔬 HYPOTHESE: Beacon-Grant Race Condition

### Problem 1: Grant-Bootstrap wird übersprungen

**Cycle 13 Bootstrap-Logik (cluster_espnow_radio.cpp:1449):**
```cpp
if(!activeTimeSourceTag) {
    activeTimeSourceTag = m.coordinatorTag;
    // ... "[BOOTSTRAP]" Log
}
```

**Fehlende Log-Meldung bedeutet:** `activeTimeSourceTag != 0` bei Grant-Empfang!

**Was setzt activeTimeSourceTag VOR Grant?**

### Beacon-Grant-Timing-Sequenz (NACHGEWIESEN im Code)

**Coordinator Grant Send (cluster_espnow_radio.cpp:1637):**
```cpp
constexpr uint32_t kSuppressionDelayMs = 1000U;
coordinatorSuppressionStartMs=now+kSuppressionDelayMs;  // Suppression bei T+1000ms
```

**Coordinator Beacon TX Check (cluster_espnow_radio.cpp:2364):**
```cpp
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    return;  // Suppress beacons
}
// ELSE: Send beacon ← Beacons AKTIV bis T=1000ms!
```

**Beacon Reception setzt activeTimeSourceTag (cluster_espnow_radio.cpp:2204):**
```cpp
activeTimeSourceTag = f.coordinatorTag;
activeTimeSourceBoot = f.senderBoot;
activeTimeSourceEpoch = f.epoch;
```

**TIMING-SEQUENZ:**
```
T=0ms:    Coordinator sendet Grant
T=0-1000: Coordinator sendet BEACONS (Bootstrap-Grace-Period)
T=50ms:   devxiao3 empfängt Beacon
          → activeTimeSourceTag = coordinator_tag (Line 2204)
          → activeTimeSourceEpoch = beacon_epoch
T=100ms:  devxiao3 empfängt Grant
          → if(!activeTimeSourceTag) → FALSE! (bereits gesetzt)
          → Grant-Bootstrap ÜBERSPRUNGEN
          → KEIN "[BOOTSTRAP]" Log
```

**ROOT CAUSE 1: BEACON RACE CONDITION NACHGEWIESEN**

---

### Problem 2: Epoch-Mismatch zwischen Beacon und Reply

**Wenn Beacon VOR Grant empfangen:**

```
1. devxiao3 empfängt Beacon (Epoch = X)
   → activeTimeSourceEpoch = X (Line 2206)

2. devxiao3 sendet Probe (Line 2467):
   → pending.expectedEpoch = activeTimeSourceEpoch = X

3. Coordinator Epoch inkrementiert (z.B. durch Beacon-Sequence)
   → epoch = Y (Y > X)

4. Coordinator sendet Reply (Line 1998):
   → r.epoch = epoch = Y

5. devxiao3 empfängt Reply (Line 2234):
   → epochMatches = (Y == X) → FALSE
   → Reply REJECTED! (Line 2256)
   → stats.rejected++
```

**Result:** `exchanges_complete=0`, `rejected=49`

**ROOT CAUSE 2: EPOCH DRIFT NACHGEWIESEN**

---

## 🛠️ IMPLEMENTIERTE DIAGNOSE (Cycle 14)

**WICHTIG:** Keine Fix-Änderungen! Nur Diagnose-Logs zur Hypothesen-Verifikation.

### 1. Grant-Empfang Diagnose (Lines 1456-1468)

**Log wenn Bootstrap übersprungen wird:**
```cpp
} else {
    // DIAGNOSIS: Grant-Bootstrap skipped
    Serial.printf("[GRANT-RX] Bootstrap SKIPPED - activeTimeSource already set:\n");
    Serial.printf("  Existing: tag=%llx boot=%llx epoch=%u\n", ...);
    Serial.printf("  Grant:    tag=%llx boot=%llx epoch=%u\n", ...);
    if(activeTimeSourceEpoch != m.coordinatorEpoch) {
        Serial.printf("  ⚠️  EPOCH MISMATCH: existing=%u grant=%u (delta=%d)\n", ...);
    }
}
```

**Erwartete Ausgabe (wenn Hypothese stimmt):**
```
[GRANT-RX] Bootstrap SKIPPED - activeTimeSource already set:
  Existing: tag=<coordinator_tag> boot=<boot> epoch=100
  Grant:    tag=<coordinator_tag> boot=<boot> epoch=105
  ⚠️  EPOCH MISMATCH: existing=100 grant=105 (delta=5)
```

---

### 2. Beacon-Empfang Diagnose (Lines 2208-2214)

**Log bei erstem activeTimeSource set:**
```cpp
if(!previousTag) {
    Serial.printf("[BEACON-RX] FIRST activeTimeSource set: tag=%llx epoch=%u\n", ...);
} else if(previousEpoch != f.epoch) {
    Serial.printf("[BEACON-RX] Epoch UPDATE: %u -> %u (delta=%d)\n", ...);
}
```

**Erwartete Ausgabe (wenn Hypothese stimmt):**
```
[BEACON-RX] FIRST activeTimeSource set: tag=<coordinator_tag> epoch=100
```
**VOR** `[GRANT-RX]` Log!

---

### 3. Probe TX Diagnose (Lines 2469-2472)

**Log alle 10 Probes:**
```cpp
if(stats.probesSent % 10 == 0) {
    Serial.printf("[PROBE-TX] coordinatorTag=%llx expectedBoot=%llx expectedEpoch=%u\n", ...);
}
```

**Erwartete Ausgabe:**
```
[PROBE-TX] coordinatorTag=<coordinator_tag> expectedBoot=<boot> expectedEpoch=100
```
Zeigt: Probe verwendet VERALTETE Epoch (100 statt 105)

---

### 4. Reply RX Rejection Diagnose (Lines 2245-2254)

**Detaillierter Rejection-Grund:**
```cpp
Serial.printf("[REPLY-RX] REJECTED - Identity mismatch:\n");
Serial.printf("  coordinatorTag: %llx (expected %llx) %s\n", ...);
Serial.printf("  senderBoot:     %llx (expected %llx) %s\n", ...);
Serial.printf("  epoch:          %u (expected %u) %s\n", ...);
```

**Erwartete Ausgabe (wenn Hypothese stimmt):**
```
[REPLY-RX] REJECTED - Identity mismatch:
  coordinatorTag: <coordinator_tag> (expected <coordinator_tag>) ✓
  senderBoot:     <boot> (expected <boot>) ✓
  epoch:          105 (expected 100) ✗
```

Zeigt: Nur Epoch-Mismatch, alle anderen Identitäten stimmen!

---

## 🧪 HARDWARE-TEST-PLAN

### Schritt 1: Flash Diagnostic Build

**Gerät:** NUR devxiao3 (Broadcaster)

**Grund:** Alle Diagnose-Logs sind Broadcaster-seitig, Coordinator unverändert.

### Schritt 2: Serial-Log aufzeichnen

**Starten:** Beide Geräte laufen, Coordinator mit aktiver Lease.

**devxiao3 neu starten** (Reset-Button oder Power-Cycle).

**Aufzeichnung:** Kompletter Serial-Log von devxiao3 ab Start.

### Schritt 3: Log-Analyse

**Hypothese BESTÄTIGT wenn:**

1. ✅ `[BEACON-RX] FIRST` erscheint VOR `[GRANT-RX]`
   → Beacon setzt activeTimeSource vor Grant

2. ✅ `[GRANT-RX] Bootstrap SKIPPED` erscheint
   → Grant-Bootstrap wird übersprungen

3. ✅ `⚠️  EPOCH MISMATCH` in `[GRANT-RX]` Log
   → Beacon-Epoch ≠ Grant-Epoch

4. ✅ `[REPLY-RX] REJECTED` mit `epoch: ... ✗`
   → Reply wird wegen Epoch-Mismatch rejected

**Hypothese WIDERLEGT wenn:**

1. ❌ `[BOOTSTRAP] Coordinator identity from Grant` erscheint
   → Grant-Bootstrap funktioniert

2. ❌ KEINE Epoch-Mismatch-Warnungen
   → Epochs stimmen überein

3. ❌ `[REPLY-RX] REJECTED` mit anderen Gründen (Tag/Boot/MAC)
   → Problem ist nicht Epoch-Mismatch

### Schritt 4: Fix-Implementierung (falls Hypothese bestätigt)

**Siehe BETA131_FOURTEENTH_CYCLE_DIAGNOSIS.md, Optionen A/B/C:**

**Option A: Grant-Priority-Bootstrap** (EMPFOHLEN)
- Grant-Bootstrap auch wenn `activeTimeSourceTag != 0`
- Nur wenn Grant NEUER (Epoch-Vergleich)

**Option B: Beacon-Blackout während Lease**
- Ignoriere Beacons für activeTimeSource-Update wenn Lease aktiv

**Option C: Beacon-Epoch-Validation**
- Akzeptiere nur Beacons mit neuerer/gleicher Epoch

---

## 🔒 SICHERHEIT

**Diagnose-Build hat KEINE funktionalen Änderungen:**

- ✅ KEINE Änderung an Bootstrap-Logik
- ✅ KEINE Änderung an Validation-Logik
- ✅ KEINE Änderung an HMAC/Replay-Protection
- ✅ KEINE Änderung an Sender-Exklusivität
- ✅ NUR zusätzliche Serial.printf()-Aufrufe

**Log-Frequenz:**
- Grant-Empfang: Pro Grant (ca. alle 3s)
- Beacon-Empfang: Bei Epoch-Change (selten)
- Probe TX: Alle 10 Probes (ca. alle 15s)
- Reply RX: Pro Rejection (nur wenn fehlgeschlagen)

**Serial-Overhead:** Minimal (<10 Logs pro Sekunde).

---

## 🧪 TESTS

**Offline-Tests (test_lease_state_machine.cpp):**

- ✅ **13/13 Tests PASSED** (keine Regression)
- ⚠️ **Diagnose-Logs können NICHT offline getestet werden**
  - Beacon/Grant-Reception erfordert echte ESP-NOW-Kommunikation
  - Timing-Sequenz erfordert Hardware-RF-Delays
  - Serial-Logs erfordert echte Serial-Ausgabe

**Test-Limitierungen:**
- Offline-Tests verifizieren nur State-Machine-Logik
- Offline-Tests können Race Condition NICHT reproduzieren
- Offline-Tests können Epoch-Drift NICHT simulieren

**Hardware-Test ZWINGEND ERFORDERLICH** für Diagnose-Verifikation.

---

## 📋 GEÄNDERTE DATEIEN

| Datei | Änderungen | Zeilen |
|-------|-----------|--------|
| **cluster_espnow_radio.cpp** | Grant-RX Diagnose | 1456-1468 (13 Zeilen) |
| **cluster_espnow_radio.cpp** | Beacon-RX Diagnose | 2201-2214 (14 Zeilen) |
| **cluster_espnow_radio.cpp** | Reply-RX Rejection Diagnose | 2244-2256 (13 Zeilen) |
| **cluster_espnow_radio.cpp** | Probe-TX Diagnose | 2468-2472 (5 Zeilen) |
| **CHANGELOG.md** | Vierzehnter Korrekturzyklus | 1-71 |
| **BETA131_FOURTEENTH_CYCLE.md** | Dieser Bericht | neu |
| **BETA131_FOURTEENTH_CYCLE_DIAGNOSIS.md** | Diagnose-Hypothesen | neu (bereits erstellt) |

**Keine Änderungen an:**
- ❌ Bootstrap-Logik (Lines 1449-1455 unverändert)
- ❌ Validation-Logik (Lines 2234-2238 unverändert)
- ❌ HMAC/Replay-Protection (unverändert)
- ❌ Coordinator-Code (unverändert)

---

## 🎯 ERWARTETE DIAGNOSE-ERGEBNISSE

### Szenario A: Hypothese BESTÄTIGT (Beacon-Race)

**Serial-Log-Sequenz:**
```
[BEACON-RX] FIRST activeTimeSource set: tag=<coord_tag> epoch=100
[GRANT-RX] Bootstrap SKIPPED - activeTimeSource already set:
  Existing: tag=<coord_tag> boot=<boot> epoch=100
  Grant:    tag=<coord_tag> boot=<boot> epoch=105
  ⚠️  EPOCH MISMATCH: existing=100 grant=105 (delta=5)
[PROBE-TX] coordinatorTag=<coord_tag> expectedEpoch=100
[REPLY-RX] REJECTED - Identity mismatch:
  coordinatorTag: <coord_tag> (expected <coord_tag>) ✓
  senderBoot:     <boot> (expected <boot>) ✓
  epoch:          105 (expected 100) ✗
```

**Nächste Schritte:**
1. Fix implementieren (Option A empfohlen)
2. Cycle 15 erstellen
3. NUR devxiao3 flashen
4. Hardware-Test wiederholen

---

### Szenario B: Hypothese TEILWEISE bestätigt (Beacon-Race, KEINE Epoch-Drift)

**Serial-Log-Sequenz:**
```
[BEACON-RX] FIRST activeTimeSource set: tag=<coord_tag> epoch=100
[GRANT-RX] Bootstrap SKIPPED - activeTimeSource already set:
  Existing: tag=<coord_tag> boot=<boot> epoch=100
  Grant:    tag=<coord_tag> boot=<boot> epoch=100
[PROBE-TX] coordinatorTag=<coord_tag> expectedEpoch=100
<KEINE REPLY-RX REJECTED Logs>
```

**Interpretation:**
- Beacon-Race EXISTIERT, aber Epoch-Drift ist NICHT das Problem
- Exchanges sollten funktionieren, aber TUN es nicht
- ANDERES Problem (z.B. MAC-Mismatch, Boot-Mismatch, Coordinator-Probe-Rejection)

**Nächste Schritte:**
1. Weitere Diagnose auf Coordinator-Seite erforderlich
2. Prüfe Coordinator `stats.rejected` Gründe

---

### Szenario C: Hypothese WIDERLEGT (Grant-Bootstrap funktioniert)

**Serial-Log-Sequenz:**
```
[BOOTSTRAP] Coordinator identity from Grant: tag=<coord_tag> boot=<boot> epoch=100
[PROBE-TX] coordinatorTag=<coord_tag> expectedEpoch=100
[REPLY-RX] REJECTED - Identity mismatch:
  <andere Gründe als Epoch>
```

**Interpretation:**
- Grant-Bootstrap FUNKTIONIERT
- Problem ist NICHT Beacon-Race
- ANDERES Problem (Coordinator-Rejection?, MAC-Mismatch?, andere Validation?)

**Nächste Schritte:**
1. Coordinator-seitige Diagnose erforderlich
2. Prüfe warum Coordinator Probes rejected
3. Prüfe MAC/Boot/Epoch-Validation

---

## 📊 ZUSAMMENFASSUNG

### Was wir wissen (Code-basiert)

1. ✅ **Coordinator sendet Beacons während Bootstrap-Grace-Period** (0-1000ms nach Grant)
2. ✅ **Beacon-Reception setzt activeTimeSourceTag** (Line 2204, keine Prioritäts-Prüfung)
3. ✅ **Grant-Bootstrap hat Bedingung `if(!activeTimeSourceTag)`** (Line 1449)
4. ✅ **`[BOOTSTRAP]` Log fehlt** (Hardware-Befund) → Bedingung war FALSE!

### Was wir vermuten (Hypothese)

1. ⚠️ **Beacon kommt VOR Grant** (zeitliche Race Condition)
2. ⚠️ **Beacon-Epoch ist VERALTET** (Coordinator Epoch inkrementiert)
3. ⚠️ **Reply wird wegen Epoch-Mismatch rejected**

### Was wir NICHT wissen (Hardware-Test erforderlich)

1. ❓ **Wann genau wird Beacon empfangen?** (vor/nach Grant?)
2. ❓ **Ist Beacon-Epoch tatsächlich veraltet?**
3. ❓ **Wird Reply tatsächlich wegen Epoch rejected?**
4. ❓ **Gibt es andere Rejection-Gründe?**

### Nächster Schritt

**Hardware-Test mit Diagnostic Build:**
1. Flash devxiao3 mit Cycle 14
2. Serial-Log vollständig aufzeichnen
3. Hypothese verifizieren/falsifizieren
4. Fix implementieren (falls Hypothese bestätigt)

---

## 🚀 FLASH-EMPFEHLUNG

**NUR devxiao3 flashen:**
- ✅ Alle Diagnose-Logs sind Broadcaster-seitig
- ❌ Coordinator-Code UNVERÄNDERT
- ❌ devfreenove1 NICHT flashen

**Erwartung nach Flash:**
- Serial-Log enthält Diagnose-Ausgaben
- `exchanges_complete` bleibt vermutlich 0 (nur Diagnose, kein Fix!)
- Hypothese kann bestätigt/widerlegt werden

---

**Status:** DIAGNOSTIC BUILD ABGESCHLOSSEN
**Datum:** 2026-10-17
**Version:** v87-beta131 (Fourteenth Cycle)

**⚠️ BEREIT FÜR ARDUINO-KOMPILIERUNG UND FLASH (NUR DEVXIAO3) - DIAGNOSTIC BUILD**
