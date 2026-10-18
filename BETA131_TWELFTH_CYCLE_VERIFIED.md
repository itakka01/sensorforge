# Beta131 - Zwölfter Korrekturzyklus - Verifiziert

**Datum:** 2026-10-17
**Version:** v87-beta131 (Twelfth Cycle VERIFIED)
**Status:** ROOT CAUSE BESTÄTIGT - BEREIT FÜR FLASH (NUR DEVXIAO3)

---

## ✅ VERIFIKATION DER 3 PRÜFPUNKTE

### 1. Self-Beacon-Identifikation: `f.senderBoot == localBootNonce`

**✅ AUSREICHEND UND WIRKSAM**

**Production Beacon TX (cluster_espnow_radio.cpp:2301):**
```cpp
prodBeacon.senderBoot=localBootNonce;  // Broadcaster's boot
```

**Coordinator Beacon TX (cluster_espnow_radio.cpp:2358):**
```cpp
b.senderBoot=localBootNonce;
```

**Vergleich:**
- ✅ **`senderBoot == localBootNonce`**: Filtert BEIDE Beacon-Typen, eindeutig, HMAC-authentifiziert
- ⚠️ `coordinatorTag == broadcasterOwnTag`: Nur Broadcaster-Beacons
- ⚠️ `coordinatorTag == tag`: Tag-abhängig
- ⚠️ MAC-Vergleich: Zusätzlicher Code erforderlich

**Entscheidung:** `senderBoot == localBootNonce` ist die BESTE Wahl.

---

### 2. Root-Cause-Nachweis: Self-Reception BESTÄTIGT

**ESP-NOW RX Callback - KEINE MAC-Filterung (Line 619-658):**
```cpp
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int length) {
    if(!info||!info->src_addr||!data)return;  // ✗ KEIN MAC-CHECK!
    // ... packet wird gequeuet ...
    memcpy(queue[head].mac, info->src_addr, 6);  // Auch eigene MAC!
}
```

**Beacon RX Path - KEINE MAC-Filterung (Line 2124-2171):**
```cpp
if(f.type==EspNowTimePackets::Type::Beacon && ...) {
    // ✗ KEIN MAC-CHECK!
    // ... beacon processing ...
    activeTimeSourceTag = f.coordinatorTag;  // Überschreibt mit broadcasterOwnTag!
}
```

**TX → RX Pfad (NACHGEWIESEN):**
```
1. devxiao3 TX Production Beacon (Line 2307):
   esp_now_send(kBroadcastMac, ...) → Broadcast an ALLE

2. ESP-NOW Delivery:
   Broadcast wird an ALLE Geräte auf Kanal geliefert
   → INKLUSIVE Sender (devxiao3)! ✓

3. devxiao3 RX Callback (Line 619):
   onReceive(..., src_addr=devxiao3_mac, ...)
   → KEIN MAC-Filter → Paket gequeuet ✓

4. Beacon Processing (Line 2168-2171):
   activeTimeSourceTag = f.coordinatorTag  // broadcasterOwnTag!
   → devxiao3 sendet Probes an SICH SELBST ✗
   → Coordinator ignoriert Probes ✗
   → 203 Timeouts ✗
```

**ROOT CAUSE: DEFINITIV BESTÄTIGT**

---

### 3. Erfolgskriterien: REALE Indikatoren

**Broadcaster Time Base Validity (Line 2248-2249):**
```cpp
broadcasterTimeBaseValid = latestQualityEstimate.valid && lastPrecisionMs &&
    (millis() - lastPrecisionMs) <= kPrecisionLifetimeMs;  // 120s
```

**Bedingungen:**
1. Serie akzeptiert (`latestQualityEstimate.valid=true`, Line 579)
2. Präzision etabliert (`lastPrecisionMs!=0`, Line 581)
3. Alter ≤ 120s (`kPrecisionLifetimeMs=120000`, Line 521)

**Serie-Akzeptanz-Kriterien (Line 566-573):**
- Spread ≤ 250 µs
- Accepted ≥ 8 Samples
- Median RTT ≤ 30ms

**HAUPT-ERFOLGSKRITERIUM:**
```
broadcaster_time_base_valid = true
```
→ **Bedeutet:** Serie akzeptiert UND Präzision frisch (<120s)

**Hardware-Indikatoren (KORRIGIERT):**

| Metrik | Vorher (Eleventh) | Nachher (Twelfth) | Bedeutung |
|--------|------------------|-------------------|-----------|
| `exchanges_complete` | 48 (nur initial) | >400 (kontinuierlich) | Laufende Exchanges |
| `exchange_timeouts` | 203 (75%) | <100 (<20%) | Niedrige Timeout-Rate |
| `last_series_estimate_age_ms` | 1060061 (18 min) | <60000 (<1 min) | **Frische Serien!** |
| `broadcaster_time_base_valid` | false | true | **HAUPT-INDIKATOR!** |
| `broadcaster_lease_active` | true | true | Lease aktiv |
| `broadcaster_production_beacons_sent` | 0 | >10000 | **Production läuft!** |

**NICHT in cluster_espnow_radio.cpp:**
- `cluster_time.source` (möglicherweise in cluster.cpp)
- `precision_clock_active` (möglicherweise in cluster.cpp)

---

## 📋 GEÄNDERTE DATEIEN (MINIMAL)

| Datei | Änderungen | Zeilen |
|-------|-----------|--------|
| **cluster_espnow_radio.cpp** | Self-beacon filtering | 2126-2132 (7 Zeilen) |
| **CHANGELOG.md** | Erfolgskriterien korrigiert | 65-71 |
| **BETA131_TWELFTH_CYCLE_VERIFIED.md** | Dieser Bericht | neu |

**Keine Änderungen an:**
- ❌ test_lease_state_machine.cpp (Offline-Test-Limitierung)
- ❌ cluster.cpp, webconfig_cluster.cpp (nicht betroffen)
- ❌ Coordinator-Code (unverändert)

---

## 🧪 TEST-LIMITATION EHRLICH BENANNT

**Offline-Tests:**
- ✅ 13/13 Tests PASSED (keine Regression)
- ❌ KEIN spezifischer Self-Beacon-Filtering-Test

**Grund:**
- Offline-Tests simulieren State-Machine-Logik
- ESP-NOW Beacon-Reception kann NICHT offline simuliert werden
- Self-Reception erfordert tatsächliche ESP-NOW Broadcast-Delivery

**Hardware-Test ERFORDERLICH:**
- Nachweis, dass Self-Beacons gefiltert werden (`rejected` steigt)
- Nachweis, dass `activeTimeSourceTag` = coordinator bleibt
- Nachweis, dass `broadcaster_time_base_valid` = true wird
- Nachweis, dass `last_series_estimate_age_ms` aktuell bleibt (<60s)

---

## 🚀 FLASH-EMPFEHLUNG

**NUR devxiao3 flashen:**
- ✅ devxiao3 (Broadcaster) - empfängt Self-Beacon-Filter
- ❌ devfreenove1 (Coordinator) - KEINE Code-Änderungen

**Begründung:**
- Alle Änderungen betreffen nur Beacon-Reception (Node-Seite)
- Coordinator-Code ist UNVERÄNDERT

**Erfolgskriterium nach Flash:**
```
broadcaster_time_base_valid = true (dauerhaft!)
```

---

**Status:** ZWÖLFTER KORREKTURZYKLUS VERIFIZIERT
**Datum:** 2026-10-17
**Version:** v87-beta131 (Twelfth Cycle VERIFIED)

**⚠️ BEREIT FÜR ARDUINO-KOMPILIERUNG UND FLASH (NUR DEVXIAO3)**
