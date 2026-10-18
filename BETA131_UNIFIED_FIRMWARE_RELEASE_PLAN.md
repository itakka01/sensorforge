# Beta131 - Unified Firmware Release Plan

**Datum:** 2026-10-18
**Ziel:** Gemeinsame stabile Firmware-Ausgangsbasis für alle vier ESP32-S3-Geräte

---

## AUSGANGSZUSTAND

### Aktuelle Hardware-Konfiguration

| Gerät | Hardware | Firmware | Rolle | Status |
|---|---|---|---|---|
| devfreenove1 | Freenove FNK0085 ESP32-S3 | v87-beta131 Cycle 16 | Coordinator | ✅ Funktioniert |
| devxiao3 | XIAO ESP32-S3 Sense | v87-beta131 Cycle 15 | Time-Broadcaster | ✅ Funktioniert |
| devxiao2 | XIAO ESP32-S3 | v87-beta130 | Node | ❓ Ungetestet |
| devxiao1 | XIAO ESP32-S3 | v87-beta122 | Node | ❓ Ungetestet |

### Verifizierte Funktionen (devfreenove1 + devxiao3)

**✅ DEFINITIV FUNKTIONIERT:**
- Probe/Reply/Final-Exchanges: 224/224, 0 timeouts
- Grant/Ack-Handover: 285/285, 0 timeouts
- Precision-Synchronisation: spread 26.5 μs, RTT 5274-5315 μs (41 μs spread)
- Drift-Schätzung: -21.649 ppm ± 0.1147 ppm
- Coordinator-Beacons: Empfangen auf Nodes (6 Beacons in CSV nachgewiesen)

**❌ NICHT VERIFIZIERT:**
- **Broadcaster production beacons:** KEINE in Node-Beacon-CSVs nachgewiesen!
- Broadcaster-Beacon-Empfang auf Nodes
- Senderexklusivität während Handover
- GPIO-Capture-Genauigkeit ≤250 μs

**⚠️ OFFENE FRAGEN:**
- Sendet devxiao3 tatsächlich production beacons?
- Empfangen devxiao1/devxiao2 Broadcaster-Beacons?
- Funktioniert Protokoll zwischen beta122/beta130/beta131?

---

## KRITISCHER BEFUND: BROADCASTER PRODUCTION BEACONS NICHT NACHGEWIESEN

### 1. Beacon-CSV-Analyse

**Was `/cluster_espnow_beacons_csv` aufzeichnet:**
- cluster_espnow_radio.cpp:38: `kBeaconObservations=12` (Ringpuffer, nur 12 Beacons!)
- cluster_espnow_radio.cpp:2190-2196: Speichert ALLE empfangenen Beacons
- **AUSNAHME:** cluster_espnow_radio.cpp:2176-2179: Self-Beacon-Filter verwirft EIGENE Beacons

**Code-Analyse:**
```cpp
// Line 2176-2179: Self-Beacon-Filter
if(f.senderBoot == localBootNonce) {
    ++stats.rejected;
    continue;  // Ignore own beacon (self-reception) ← NICHT gespeichert!
}

// Line 2190-2196: Beacon-Observation gespeichert
BeaconObservation &obs=beaconObservations[beaconObsWrite];
obs.sequence=f.sequence; obs.epoch=f.epoch;
obs.coordinatorBoot=f.senderBoot;
obs.coordinatorUtcUs=f.timeUs; obs.nodeRxCallbackUs=item.rxUs;
beaconObsWrite=(beaconObsWrite+1)%kBeaconObservations;  // Ringpuffer rotiert!
```

**Warum nur Coordinator-Beacons im CSV?**

**Option A:** Broadcaster sendet KEINE production beacons
- Bedingungen nicht erfüllt (wahrscheinlich!)
- `broadcasterLeaseActive=false` oder `broadcasterTimeBaseValid=false`
- Oder `precisionUtcNow()` schlägt fehl

**Option B:** Broadcaster sendet, aber Nodes empfangen nicht
- RF-Probleme
- Beta122/Beta130 Nodes inkompatibel

**Option C:** Broadcaster sendet, Nodes empfangen, aber CSV rotiert zu schnell
- Ringpuffer nur 12 Beacons
- Bei 10 Hz Coordinator-Beacons: Ringpuffer voll in 1.2 Sekunden
- Broadcaster-Beacons könnten überschrieben werden

### 2. Warum Broadcaster vermutlich KEINE Production Beacons sendet

**Bedingung 1:** `broadcasterLeaseActive` - sollte true sein (Grant/Ack funktioniert)

**Bedingung 2:** `broadcasterTimeBaseValid` (cluster_espnow_radio.cpp:2316-2317):
```cpp
broadcasterTimeBaseValid = latestQualityEstimate.valid && lastPrecisionMs &&
                          (millis() - lastPrecisionMs) <= kPrecisionLifetimeMs;
```

**Hardware zeigt:**
- `precision_recently_accepted=true` ✅
- `last_series_estimate_age_ms=13490` < 120000 ✅
- Sollte `broadcasterTimeBaseValid=true` sein!

**Bedingung 3:** `precisionUtcNow()` erfolgreich (cluster_espnow_radio.cpp:2350):
```cpp
if(precisionUtcNow(epoch, id, clusterUtcUs, clusterAgeMs, clusterRttUs)) {
    // Production beacon senden
    ++broadcasterProductionBeaconsSent;
} else {
    // Time base LOST
    broadcasterLeaseActive=false;
    broadcasterTimeBaseValid=false;
}
```

**Mögliche Ursache:** `precisionUtcNow()` schlägt fehl!

**Warum könnte `precisionUtcNow()` fehlschlagen?** (cluster_espnow_radio.cpp:1240-1260)
```cpp
bool precisionUtcNow(uint32_t epoch, const char *id, int64_t &utc, uint32_t &age, double &rtt) {
    if(!latestQualityEstimate.valid) return false;
    // ...
    const double ageS = static_cast<double>(nowMs - lastPrecisionMs) * 0.001;
    if (ageS < 0.0 || ageS > 120.0) return false;  // ← Age-Check!
    // ...
    return true;
}
```

**Mögliche Ursachen für Failure:**
1. `latestQualityEstimate.valid=false` (aber Hardware zeigt `precision_recently_accepted=true`!)
2. Age > 120 Sekunden (aber Hardware zeigt 13.5 Sekunden!)
3. Epoch/ID-Mismatch (möglich!)

**KRITISCH:** Ohne `broadcasterProductionBeaconsSent` im JSON können wir nicht feststellen, ob Beacons gesendet werden!

### 3. Fehlende Metriken im JSON-Output

**Broadcaster-Metriken existieren im Code, aber NICHT im JSON:**
- `broadcasterProductionBeaconsSent` (cluster_espnow_radio.h:113, cluster_espnow_radio.cpp:2377)
- In Diagnostics-Struktur vorhanden
- **ABER:** cluster.cpp enthält KEINE JSON-Ausgabe dafür!

**Coordinator-Metriken existieren, sind im JSON:**
- `coordinator_suppressing_beacons` ✅
- `coordinator_beacons_suppressed` ✅

**Node-Metriken für aktive Zeitquelle:**
- `active_time_source` (Tag der aktuellen Zeitquelle) ✅
- Aber NICHT: "Letzer Broadcaster-Beacon empfangen"

---

## SOURCE-STAND-KOMPATIBILITÄT

### Board-Unterschiede (board_config.h)

**FREENOVE (Line 44-163):**
- `BOARD_DISPLAY_NAME "Freenove FNK0085 ESP32-S3 WROOM"`
- `BOARD_HAS_ESP_NOW 1` ✅
- SD_MMC (nicht SPI)
- Pins: LED GPIO2, SD_MMC GPIO38/39/40, Radar GPIO41/42, RTC GPIO1/47
- Kein integriertes Mikrofon

**XIAO (Line 168-224):**
- `BOARD_DISPLAY_NAME "Seeed XIAO ESP32S3 Sense"`
- `BOARD_HAS_ESP_NOW 1` ✅
- SD_SPI
- Pins: D1/GPIO2 für Diagnostik, anderes Pin-Layout
- Integriertes PDM-Mikrofon

**Kompatibilität:**
- ✅ Beide haben ESP-NOW
- ✅ Beide nutzen gleiche ESP-NOW-Paketstruktur
- ✅ Unterschiedliche Binaries erforderlich (BOARD_FREENOVE vs BOARD_XIAO)
- ✅ Gleicher Source-Code, unterschiedliche Compile-Flags

### Build-Prozess

**Aktuell (board_config.h:27-28):**
```cpp
#define BOARD_FREENOVE
//#define BOARD_XIAO
```

**Für XIAO-Builds:**
```cpp
//#define BOARD_FREENOVE
#define BOARD_XIAO
```

**Erforderliche Builds:**
1. v87-beta131-freenove (für devfreenove1)
2. v87-beta131-xiao (für devxiao1, devxiao2, devxiao3)

### ESP-NOW-Protokoll-Kompatibilität

**Paketstruktur (cluster_espnow_packets.h):**
```cpp
constexpr size_t kPayloadSize = 44;
constexpr size_t kMacSize = 32;
constexpr size_t kPacketSize = 76;  // 44 + 32
constexpr uint8_t kVersion = 1;

enum class Type : uint8_t {
    Beacon=1, Probe=2, Reply=3, Final=4, SyncControl=5,
    NodeSyncReport=6, NodeDriftReport=7,
    BenchControl=8, BenchDown=9, BenchUp=10, BenchAck=11
};
```

**Keine Änderungen zwischen beta122/beta130/beta131 erkennbar!**

**HMAC-Keys:** Gemeinsame Cluster-Keys (config.txt), sollten kompatibel sein.

**Erwartung:** Beta122/Beta130 Nodes sollten beta131 Broadcaster-Beacons empfangen können!

---

## CYCLE 16 FIXES - SICHERHEITSANALYSE

### Fix 1: Coordinator Probe-Handling (Line 1995)

**Änderung:**
```cpp
// VORHER (Cycle 11):
if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe) {
    continue;  // Skip ALL Probes
}

// NACHHER (Cycle 16):
if(coordinatorGrantedLeaseActive && f.type==EspNowTimePackets::Type::Probe && f.coordinatorTag != tag) {
    continue;  // Skip only Probes NOT addressed to coordinator
}
```

**Sicherheit:**
- ✅ coordinatorTag-Validation erhalten (Line 2007-2010)
- ✅ Nur Probes AN Coordinator werden beantwortet
- ✅ Probes AN Broadcaster werden übersprungen
- ✅ Verhindert doppelte Replies (Coordinator + Broadcaster)

**Risiko für Nodes:**
- ❌ KEINE zusätzlichen Coordinator-Replies an Nodes
- Begründung: Nodes senden Probes an `activeTimeSourceTag`
- Wenn Broadcaster aktiv: Nodes senden an `broadcasterOwnTag`, NICHT `coordinatorTag`
- Coordinator antwortet nur auf Probes an `coordinatorTag`

**Bewertung:** ✅ SICHER

### Fix 2: Broadcaster Slot-Exception (Line 2015-2025)

**Änderung:**
```cpp
const bool isObservedBroadcaster = (observedBroadcasterAt != 0 &&
                                    memcmp(item.mac, observedBroadcasterMac, 6) == 0 &&
                                    f.senderBoot == observedBroadcasterBoot);

if(!slotIsOwner(...) && !bench... && !isObservedBroadcaster) continue;
```

**Wie wird observedBroadcaster gesetzt?** (Line 1693-1696)
```cpp
// In serviceDiagnosticReceive() - NACH HMAC-Verification + Replay-Check!
memcpy(observedBroadcasterMac, item.mac, 6);
observedBroadcasterTag = msg.senderTag;
observedBroadcasterBoot = msg.senderBoot;
observedBroadcasterAt = nowMs;
```

**Invalidierung:**
- ✅ Bei `stop()` (Line 1154-1155): auf 0 gesetzt
- ✅ Bei Diagnostic-Beacon mit neuer Boot-ID: aktualisiert (Line 1693-1696)
- ❌ KEIN Timeout! `observedBroadcasterAt` läuft nicht ab!

**⚠️ POTENTIELLES SICHERHEITSPROBLEM:**

**Scenario: Broadcaster-Neustart**
1. Broadcaster sendet Diagnostic Beacons (Boot-ID: old)
2. Coordinator: `observedBroadcasterBoot = old`
3. Broadcaster neustartet (Boot-ID: new)
4. Broadcaster sendet Probes (Boot-ID: new)
5. Coordinator: `f.senderBoot (new) != observedBroadcasterBoot (old)` → `isObservedBroadcaster=false`
6. Coordinator: `!slotIsOwner` → Probe verworfen!
7. **DEADLOCK:** Broadcaster kann keine Zeitbasis aufbauen!

**ABER:** Broadcaster sendet ZUERST Diagnostic Beacons (kontinuierlich 10 Hz)
- observedBroadcaster wird SOFORT aktualisiert (Line 1693-1696)
- Sehr kurzes Zeitfenster für Problem

**Mitigation:**
- Grant/Ack-System läuft zuerst
- Diagnostic Beacons vor Probes
- observedBroadcaster wird kontinuierlich aktualisiert

**Bewertung:** ⚠️ THEORETISCHES RISIKO, aber in Praxis unwahrscheinlich

**Empfehlung:** Monitoring bei Hardware-Tests. Falls Problem auftritt: Timeout für `observedBroadcasterAt` hinzufügen.

---

## ESP-NOW-NETZ-VEREINHEITLICHUNG - RISIKEN

### Upgrade devxiao1 (beta122 → beta131)

**Mögliche Risiken:**
1. ❌ Paketstruktur-Inkompatibilität → UNWAHRSCHEINLICH (gleiche Struktur)
2. ❌ HMAC-Key-Änderung → NEIN (Cluster-Keys unverändert)
3. ❌ Neue Validierungen blockieren alte Coordinator → NEIN (Cycle 16 erlaubt Broadcaster-Probes)
4. ❌ Slot-System-Änderungen → NEIN (Slot-System unverändert)

**Erwartung:** ✅ Sollte funktionieren

### Upgrade devxiao2 (beta130 → beta131)

**Kleinerer Versionssprung, weniger Risiko.**

**Erwartung:** ✅ Sollte funktionieren

### Upgrade devxiao3 (Cycle 15 → Cycle 16)

**NICHT ERFORDERLICH!** Cycle 16 Fixes sind auf COORDINATOR-Seite.

**Empfehlung:** ❌ NICHT flashen (bereits funktionierend)

### Upgrade devfreenove1 (Cycle 16 → Cycle 16)

**NICHT ERFORDERLICH!** Bereits Cycle 16.

**Empfehlung:** ❌ NICHT flashen (bereits funktionierend)

---

## DIAGNOSTIK-BEDARF

### Fehlende Metriken

**Broadcaster (devxiao3):**
- ❌ `broadcasterProductionBeaconsSent` (existiert in Code, fehlt in JSON!)
- ❌ `broadcasterLeaseActive` (existiert in Code, im JSON als `broadcaster_lease_active`)
- ❌ `broadcasterTimeBaseValid` (existiert in Code, FEHLT in JSON!)
- ❌ `precisionUtcNow()` success/failure (FEHLT!)

**Nodes (devxiao1, devxiao2):**
- ❌ Letzter Broadcaster-Beacon empfangen (Timestamp, Tag)
- ❌ Anzahl Broadcaster-Beacons empfangen

**Coordinator (devfreenove1):**
- ✅ `coordinator_suppressing_beacons` (vorhanden)
- ✅ `coordinator_beacons_suppressed` (vorhanden)

### Minimale Diagnostik-Ergänzung

**ERFORDERLICH für Broadcaster-Verifikation:**

**cluster.cpp JSON-Ausgabe ergänzen:**
```cpp
// In clusterStatusJson(), nach Line 5047:
json += ",\"broadcaster_time_base_valid\":";
json += esn.broadcasterTimeBaseValid ? "true" : "false";  // Aus Diagnostics
json += ",\"broadcaster_production_beacons_sent\":";
json += String((unsigned long)esn.broadcasterProductionBeaconsSent);
```

**Begründung:**
- KEINE Änderung der Zeitlogik
- NUR JSON-Ausgabe
- Minimal-invasiv
- Ermöglicht Broadcaster-Verifikation

---

## TESTS

### Offline-Tests

**Ausgeführt:** `test_lease_state_machine.cpp`
```
==============================================
Test Results: 13 PASSED, 0 FAILED
==============================================
```

**Abdeckung:**
- ✅ Lease-State-Machine-Logik
- ✅ Timing-Invarianten (Suppression, Activation, Gap)
- ✅ Bootstrap-Timing (550ms gap)
- ✅ Startup Guard (6000ms)
- ✅ Epoch-Validation (Probe/Reply/Final)

**NICHT abgedeckt:**
- ❌ ESP-NOW RF-Kommunikation
- ❌ Tatsächliche Beacon-Übertragung
- ❌ Paketempfang auf Nodes
- ❌ Broadcaster production beacons
- ❌ Senderexklusivität in Hardware

**Bewertung:** Offline-Tests bestätigen STATE-MACHINE-LOGIK, nicht RF-FUNKTIONALITÄT!

---

## RELEASE-KANDIDAT

**Name:** v87-beta131 (gemeinsamer Integrationsstand nach Cycle 16)

**Source-Stand:** Aktueller Hauptzweig (Cycle 16 auf Coordinator, Cycle 15 auf Broadcaster)

**Erforderliche Builds:**
1. **v87-beta131-freenove** (BOARD_FREENOVE)
   - Für: devfreenove1
2. **v87-beta131-xiao** (BOARD_XIAO)
   - Für: devxiao1, devxiao2, devxiao3

---

## FLASH-REIHENFOLGE UND PRÜFUNG

### Reihenfolge-Analyse

**Option A: Nodes zuerst**
1. devxiao2 (beta130 → beta131) - kleinster Sprung
2. devxiao1 (beta122 → beta131)
3. devxiao3 (Cycle 15 → NICHT flashen!)
4. devfreenove1 (Cycle 16 → NICHT flashen!)

**Option B: Conservative**
1. devxiao1 (beta122 → beta131) - größter Sprung, höchstes Risiko zuerst
2. devxiao2 (beta130 → beta131)
3. devxiao3, devfreenove1 NICHT flashen

**Empfehlung:** **Option B** (Conservative)

**Begründung:**
- devxiao1 hat älteste Firmware → größtes Upgrade → höchstes Risiko
- Wenn devxiao1 scheitert, stoppen wir
- devfreenove1 + devxiao3 funktionieren bereits → NICHT anfassen!

### Flash-Plan devxiao1

**Vorbereitung:**
1. Backup aktueller Config via HTTP GET `/export_config`
2. Serial-Verbindung für Logs
3. Zweiter Rechner für HTTP-Monitoring

**Flash:**
```bash
# Board-Definition auf XIAO ändern
sed -i 's/#define BOARD_FREENOVE/#\/\/define BOARD_FREENOVE/' board_config.h
sed -i 's/\/\/#define BOARD_XIAO/#define BOARD_XIAO/' board_config.h

# Build und Flash
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM<N>
```

**Unmittelbar nach Neustart prüfen:**
```bash
# Warte 30 Sekunden für Boot
sleep 30

# Status abrufen
curl http://devxiao1/cluster_status | jq '{
  runtime: .runtime_seconds,
  espnow_radio: .cluster_time.espnow_radio | {
    active,
    probes_sent,
    exchanges_complete,
    exchange_timeouts,
    rejected,
    precision_recently_accepted
  },
  cluster_time: .cluster_time | {
    source,
    valid
  }
}'
```

**Erfolgskriterien (nach 5 Minuten):**
- ✅ `espnow_radio.active=true`
- ✅ `espnow_radio.probes_sent > 0`
- ✅ `espnow_radio.exchanges_complete > 0`
- ✅ `espnow_radio.exchange_timeouts < 20%`
- ✅ `espnow_radio.precision_recently_accepted=true`
- ✅ `cluster_time.valid=true`

**Rollback-Kriterien:**
- ❌ `exchanges_complete=0` nach 5 Minuten
- ❌ `exchange_timeouts > 50%`
- ❌ `rejected` steigt dramatisch (> 1000)
- ❌ Keine Beacons empfangen (CSV leer)

**Rollback-Prozedur:**
```bash
# Flash alte beta122-Firmware
# ODER: Flash beta131 erneut (falls Config-Problem)
# ODER: Factory-Reset via Web-UI
```

### Flash-Plan devxiao2

**Identisch zu devxiao1, aber geringeres Risiko (beta130 → beta131).**

### Flash-Plan devxiao3

**NICHT flashen!** Bereits Cycle 15, Broadcaster funktioniert.

### Flash-Plan devfreenove1

**NICHT flashen!** Bereits Cycle 16, Coordinator funktioniert.

---

## ROLLBACK-PLAN

**Für jedes Gerät:**
1. Alte Firmware-Binary aufbewahren
2. Bei Rollback: Alte Binary flashen
3. Config restaurieren (falls nötig)
4. Neustart
5. Verifikation wie oben

**Cluster-weiter Rollback:**
- Falls devxiao1 Upgrade scheitert: Stoppen, devxiao2 NICHT flashen
- Falls devxiao2 Upgrade scheitert: devxiao1 zurück auf beta122, stoppen
- Falls beide Nodes Probleme: Gesamtsystem rollback

---

## GPIO-CAPTURE-VORBEREITUNG

**Voraussetzung:** Broadcaster sendet VERIFIZIERT production beacons

**Aktuell:** NICHT verifiziert!

**Nächste Schritte:**
1. Diagnostik-Ergänzung (broadcasterProductionBeaconsSent im JSON)
2. Flash Nodes (devxiao1, devxiao2)
3. Broadcaster-Beacon-Empfang auf Nodes prüfen
4. Wenn verifiziert: GPIO-Capture-Test vorbereiten

**GPIO-Capture-Test-Plan (für später):**
- GPIO-Kabel zwischen Broadcaster-Out und Node-In
- Broadcaster sendet GPIO-Pulse synchron zu Beacons
- Node misst GPIO-Eingang relativ zu empfangenen Beacons
- Erwartung: ≤250 μs Abweichung

**Noch NICHT durchführen!** Erst Broadcaster-Beacons verifizieren.

---

## ZUSAMMENFASSUNG

**Gemeinsamer Source-Stand:**
- ✅ Aktueller beta131 geeignet für alle Geräte
- ✅ Unterschiedliche Board-Builds erforderlich (FREENOVE vs XIAO)
- ✅ ESP-NOW-Protokoll kompatibel beta122/beta130/beta131

**Cycle 16 Fixes:**
- ✅ Fix 1 (Probe-Filter): SICHER
- ⚠️ Fix 2 (Broadcaster-Exception): THEORETISCHES RISIKO bei Broadcaster-Neustart

**Broadcaster Production Beacons:**
- ❌ NICHT in Node-CSVs nachgewiesen
- ❌ Fehlende Metriken im JSON (`broadcasterProductionBeaconsSent`)
- ⚠️ Unklar ob tatsächlich gesendet

**Flash-Empfehlung:**
- ✅ devxiao1 (beta122 → beta131)
- ✅ devxiao2 (beta130 → beta131)
- ❌ devxiao3 NICHT flashen (bereits funktionierend)
- ❌ devfreenove1 NICHT flashen (bereits funktionierend)

**Reihenfolge:** devxiao1 → devxiao2 (Conservative)

**Diagnostik-Ergänzung erforderlich:** Ja (minimal, nur JSON-Ausgabe)

**GPIO-Capture-Test:** Noch NICHT möglich (erst Broadcaster-Beacons verifizieren)

---

## OFFENE FRAGEN

1. ❓ Sendet devxiao3 tatsächlich production beacons?
2. ❓ Warum sind keine Broadcaster-Beacons in Node-CSVs?
3. ❓ Schlägt `precisionUtcNow()` auf Broadcaster fehl?
4. ❓ Funktioniert beta131 Broadcaster mit beta122/beta130 Nodes?
5. ❓ Ist observedBroadcaster-Timeout erforderlich?

**Antworten erst nach Hardware-Tests möglich!**
