# Beta131 - Vierzehnter Korrekturzyklus - DIAGNOSE

**Datum:** 2026-10-17
**Status:** ROOT CAUSE HYPOTHESE - DIAGNOSE ERFORDERLICH

---

## 🔴 HARDWARE-BEFUND (Nach Cycle 13 Flash)

**devxiao3 (Broadcaster, Cycle 13):**
- Uptime: 46s, Boot-ID: `ee3e663a`
- `probes_sent=9`, `exchanges_complete=0`, `exchange_timeouts=8`
- `authenticated=27`, `rejected=49` ← SEHR HOCH!
- `espnow_grant_diagnostic.received=15`, `authenticated=15`, `acks_sent=15`
- **`[BOOTSTRAP]` Meldung FEHLT im Log!**

**devfreenove1 (Coordinator, Cycle 11):**
- Epoch: `4274446014`, Boot-ID: `ee72ef6a`
- Grant/Ack funktioniert
- Beacons während Lease unterdrückt

---

## 🔬 HYPOTHESE: Beacon-Grant Race Condition

### Problem: Grant-Bootstrap wird übersprungen

**Cycle 13 Bootstrap-Logik (cluster_espnow_radio.cpp:1448):**
```cpp
if(!activeTimeSourceTag) {
    activeTimeSourceTag = m.coordinatorTag;  // Grant-Bootstrap
    // ... "[BOOTSTRAP]" Log
}
```

**Fehlende Log-Meldung bedeutet:** `activeTimeSourceTag != 0` bei Grant-Empfang!

### Was setzt activeTimeSourceTag VOR Grant?

**Möglichkeit 1: Coordinator Beacon während Bootstrap-Grace-Period**

**Grant Send (Line 1637):**
```cpp
coordinatorSuppressionStartMs=now+kSuppressionDelayMs;  // +1000ms
```

**Beacon TX Check (Line 2364):**
```cpp
if(coordinatorGrantedLeaseActive && coordinatorSuppressionStartMs && now >= coordinatorSuppressionStartMs) {
    return;  // Suppress beacons
}
// ELSE: Send beacon ← Beacons AKTIV bis T=1000ms!
```

**Timing-Sequenz:**
```
T=0:     Coordinator sendet Grant
T=0-1000: Coordinator sendet WEITERHIN Beacons (Bootstrap-Grace-Period)
T=50:    devxiao3 empfängt Beacon
         → activeTimeSourceTag = coordinator_tag (Line 2188)
T=100:   devxiao3 empfängt Grant
         → if(!activeTimeSourceTag) → FALSE! (bereits gesetzt)
         → Grant-Bootstrap ÜBERSPRUNGEN
         → KEIN "[BOOTSTRAP]" Log
```

**Beacon Reception setzt activeTimeSourceTag (Line 2188):**
```cpp
activeTimeSourceTag = f.coordinatorTag;
activeTimeSourceBoot = f.senderBoot;
activeTimeSourceEpoch = f.epoch;  ← Beacon-Epoch!
```

---

### Hypothese 2: Epoch-Mismatch zwischen Beacon und Reply

**Wenn Beacon VOR Grant empfangen:**

```
1. devxiao3 empfängt Beacon (Epoch = X)
   → activeTimeSourceEpoch = X

2. devxiao3 sendet Probe (Line 2433):
   → pending.expectedEpoch = activeTimeSourceEpoch = X

3. Coordinator Epoch inkrementiert (z.B. X → Y)

4. Coordinator sendet Reply (Line 1998):
   → r.epoch = epoch = Y (aktuelle Epoch)

5. devxiao3 empfängt Reply (Line 2211):
   → epochMatches = (Y == X) → FALSE
   → Reply REJECTED!
   → stats.rejected++
```

**Result:** `exchanges_complete=0`, `rejected=49`

---

## ⚠️ WARUM IST DAS EIN PROBLEM?

**Cycle 10 Design-Absicht:**
- Coordinator sendet Beacons während Bootstrap-Grace-Period (1000ms)
- Broadcaster baut Zeitbasis auf
- Dann: Coordinator suppressed, Broadcaster aktiviert

**ABER:**
- Beacons setzen `activeTimeSourceTag/Boot/Epoch`
- Grant-Bootstrap wird übersprungen (`if(!activeTimeSourceTag)` ist falsch)
- **Wenn Beacon VERALTET ist (alte Epoch), schlagen Exchanges fehl!**

---

## 🔍 DIAGNOSE ERFORDERLICH

**Zu prüfen:**

1. **Wird Beacon VOR Grant empfangen?**
   - Beacon RX Timestamp vs. Grant RX Timestamp

2. **Ist Beacon-Epoch VERALTET?**
   - Beacon-Epoch vs. Grant-Epoch
   - Beacon-Epoch vs. Reply-Epoch

3. **Wird Grant-Bootstrap tatsächlich übersprungen?**
   - `activeTimeSourceTag`-Wert bei Grant-Empfang

4. **Warum werden Probes rejected?**
   - Coordinator-Tag Match?
   - Epoch Match?
   - Boot Match?

---

## 🛠️ DIAGNOSE-IMPLEMENTIERUNG

Folgende Diagnose-Counter/Logs sind erforderlich:

**Bei Grant-Empfang (cluster_espnow_radio.cpp:~1448):**
```cpp
Serial.printf("[GRANT-RX] activeTimeSourceTag=%llx (before check)\n", activeTimeSourceTag);
if(!activeTimeSourceTag) {
    // Bootstrap
    Serial.printf("[BOOTSTRAP] From Grant: tag=%llx epoch=%u\n", ...);
} else {
    Serial.printf("[GRANT-RX] Bootstrap SKIPPED: activeTimeSource already set\n");
    Serial.printf("  Existing: tag=%llx boot=%llx epoch=%u\n",
        activeTimeSourceTag, activeTimeSourceBoot, activeTimeSourceEpoch);
    Serial.printf("  Grant:    tag=%llx boot=%llx epoch=%u\n",
        m.coordinatorTag, m.coordinatorBoot, m.coordinatorEpoch);
}
```

**Bei Beacon-Empfang (cluster_espnow_radio.cpp:~2188):**
```cpp
if(activeTimeSourceTag && activeTimeSourceTag != f.coordinatorTag) {
    Serial.printf("[BEACON-RX] Source switch: %llx -> %llx\n",
        activeTimeSourceTag, f.coordinatorTag);
}
Serial.printf("[BEACON-RX] Set activeTimeSource: tag=%llx epoch=%u\n",
    f.coordinatorTag, f.epoch);
```

**Bei Probe TX (cluster_espnow_radio.cpp:~2422):**
```cpp
Serial.printf("[PROBE-TX] coordinatorTag=%llx expectedEpoch=%u\n",
    p.coordinatorTag, pending.expectedEpoch);
```

**Bei Reply RX Rejection (cluster_espnow_radio.cpp:~2218):**
```cpp
} else {
    Serial.printf("[REPLY-RX] REJECTED: tag=%llx/%llx boot=%llx/%llx epoch=%u/%u\n",
        f.coordinatorTag, pending.expectedTag,
        f.senderBoot, pending.expectedBoot,
        f.epoch, pending.expectedEpoch);
    ++stats.rejected;
}
```

---

## 📋 NÄCHSTE SCHRITTE

**Diagnose-Build flashen:**
1. Diagnose-Logs hinzufügen (wie oben)
2. NUR devxiao3 flashen
3. Hardware-Test mit Serial-Log-Aufzeichnung
4. Analyze: Beacon vor/nach Grant? Epoch-Mismatch?

**Falls Hypothese bestätigt:**
- Fix implementieren (siehe Optionen unten)

---

## 🔧 FIX-OPTIONEN (noch nicht implementiert)

### Option A: Grant-Bootstrap hat Priorität (CONDITIONAL UPDATE)

**Problem:** Beacon überschreibt Grant-Identity

**Fix:** Grant-Bootstrap auch wenn `activeTimeSourceTag != 0`, ABER nur wenn Grant NEUER:

```cpp
// Bei Grant-Empfang:
const bool grantIsNewer = !activeTimeSourceTag ||
    (m.coordinatorEpoch > activeTimeSourceEpoch) ||
    (m.coordinatorEpoch == activeTimeSourceEpoch && m.sequence > grantLastCoordinatorSequence);

if(grantIsNewer) {
    activeTimeSourceTag = m.coordinatorTag;
    activeTimeSourceBoot = m.coordinatorBoot;
    activeTimeSourceEpoch = m.coordinatorEpoch;
    memcpy(activeTimeSourceMac, item.mac, 6);
    Serial.printf("[GRANT-BOOTSTRAP] ...\n");
}
```

**Sicherheit:** ✅ Grant ist HMAC-authentifiziert, Epoch-Vergleich verhindert Downgrade

---

### Option B: Beacon-Bootstrap während Lease deaktivieren

**Problem:** Beacons überschreiben während aktiver Lease

**Fix:** Ignoriere Beacons für activeTimeSource-Update wenn Lease aktiv:

```cpp
// Bei Beacon-Empfang:
if(!broadcasterLeaseActive) {
    // Nur außerhalb Lease: Beacon-Bootstrap erlaubt
    activeTimeSourceTag = f.coordinatorTag;
    activeTimeSourceBoot = f.senderBoot;
    activeTimeSourceEpoch = f.epoch;
}
```

**Problem:** Verhindert legitime Source-Switches (z.B. Coordinator-Reboot)

---

### Option C: Beacon-Epoch-Validation

**Problem:** Veraltete Beacons setzen alte Epoch

**Fix:** Akzeptiere nur Beacons mit NEUER/GLEICHER Epoch:

```cpp
// Bei Beacon-Empfang:
const bool beaconIsNewer = !activeTimeSourceTag ||
    (f.epoch >= activeTimeSourceEpoch);

if(beaconIsNewer) {
    activeTimeSourceTag = f.coordinatorTag;
    activeTimeSourceBoot = f.senderBoot;
    activeTimeSourceEpoch = f.epoch;
}
```

**Problem:** Epochs können wraparound, Vergleich nicht trivial

---

## 🎯 ENTSCHEIDUNG

**ZUERST: Diagnose durchführen, Hypothese beweisen**

Keine spekulative Reparatur. Implementiere Diagnose-Logs, flashe devxiao3, sammle Daten.

**Erst danach:** Fix implementieren basierend auf nachgewiesener Root Cause.

---

**Status:** DIAGNOSE-PHASE
**Nächster Schritt:** Diagnose-Logs implementieren
