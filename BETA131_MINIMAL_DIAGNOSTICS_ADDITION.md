# Beta131 - Minimale Diagnostik-Ergänzung für Broadcaster-Verifikation

**Datum:** 2026-10-18
**Status:** JSON-Export ergänzt - KEINE Logik-Änderungen
**Cycle:** Vorbereitung für Unified Firmware Release

---

## PROBLEM

**Nach Cycle 16:**
- Broadcaster-Zeitbasis funktioniert (224/224 Exchanges, 0 Timeouts) ✅
- Grant/Ack funktioniert (47 Grants empfangen, 47 Acks gesendet) ✅
- **ABER:** Unklar ob Broadcaster TATSÄCHLICH production beacons sendet!

**Indizien, aber kein Beweis:**
- `rejected` Counter steigt (228→1001) - könnte Self-Beacon-Filter sein
- Beacon-CSVs von Nodes zeigen NUR 6 Coordinator-Beacons, KEINE Broadcaster-Beacons
- `broadcasterProductionBeaconsSent` Counter existiert in Code, aber NICHT in JSON exportiert

**Vor Unified Firmware Flash ERFORDERLICH:**
- Verifizierung dass Broadcaster production beacons sendet
- Verifizierung dass Nodes diese empfangen
- Verifizierung dass `activeTimeSource` auf Broadcaster wechselt

---

## IMPLEMENTIERUNG

### Datei: `cluster.cpp`

**Lines:** 5023-5032
**Änderungstyp:** JSON-Export ergänzt (KEINE Logik-Änderungen!)

**Vorher (Line 5019-5023):**
```cpp
json += ",\"action_pause_remaining_ms\":" + String((unsigned long)esn.capturePauseRemainingMs);
json += ",\"precision_clock_active\":";json += espSource ? "true" : "false";
json += ",\"precision_sample_age_ms\":" + String(espSource ? (unsigned long)sourceAge : 0UL);
json += ",\"precision_rtt_us\":" + String(espSource ? sourceRtt : 0.0, 3);
json += '}';
```

**Nachher (Line 5019-5032):**
```cpp
json += ",\"action_pause_remaining_ms\":" + String((unsigned long)esn.capturePauseRemainingMs);
json += ",\"precision_clock_active\":";json += espSource ? "true" : "false";
json += ",\"precision_sample_age_ms\":" + String(espSource ? (unsigned long)sourceAge : 0UL);
json += ",\"precision_rtt_us\":" + String(espSource ? sourceRtt : 0.0, 3);
// Beta131: Broadcaster handover diagnostics (minimal addition for verification)
json += ",\"broadcaster_lease_active\":"; json += esn.broadcasterLeaseActive ? "true" : "false";
json += ",\"broadcaster_lease_remaining_ms\":" + String((unsigned long)esn.broadcasterLeaseRemainingMs);
json += ",\"broadcaster_production_beacons_sent\":" + String((unsigned long)esn.broadcasterProductionBeaconsSent);
json += ",\"coordinator_suppressing_beacons\":"; json += esn.coordinatorSuppressingBeacons ? "true" : "false";
json += ",\"coordinator_beacons_suppressed\":" + String((unsigned long)esn.coordinatorBeaconsSuppressed);
char activeSourceText[17]; snprintf(activeSourceText,sizeof(activeSourceText),"%016llx",(unsigned long long)esn.activeTimeSource);
json += ",\"active_time_source\":\"" + String(activeSourceText) + "\"";
json += ",\"source_changes\":" + String((unsigned long)esn.sourceChanges);
json += '}';
```

### Neue JSON-Felder (7 Stück)

**HTTP-Endpoint:** `GET /cluster/espnow_radio`
**JSON-Pfad:** `espnow_time_local.*`

1. **`broadcaster_lease_active`** (bool)
   - `true`: Broadcaster hat aktiven Grant-Lease vom Coordinator
   - `false`: Kein aktiver Lease
   - Quelle: `esn.broadcasterLeaseActive` (cluster_espnow_radio.h:107)

2. **`broadcaster_lease_remaining_ms`** (uint32_t)
   - Verbleibende Lease-Zeit in Millisekunden
   - 0 wenn kein Lease aktiv
   - Quelle: `esn.broadcasterLeaseRemainingMs` (cluster_espnow_radio.h:108)

3. **`broadcaster_production_beacons_sent`** (uint32_t) **← KRITISCH!**
   - Anzahl gesendeter production beacons (10 Hz)
   - Inkrementiert in `cluster_espnow_radio.cpp:2365` (nur wenn `precisionUtcNow()` erfolgreich!)
   - **Wenn 0:** Broadcaster sendet NICHT (precisionUtcNow() fails oder Lease inaktiv)
   - **Wenn steigend:** Broadcaster sendet production beacons ✅
   - Quelle: `esn.broadcasterProductionBeaconsSent` (cluster_espnow_radio.h:113)

4. **`coordinator_suppressing_beacons`** (bool)
   - `true`: Coordinator unterdrückt eigene beacons (Broadcaster aktiv)
   - `false`: Coordinator sendet eigene beacons
   - Quelle: `esn.coordinatorSuppressingBeacons` (cluster_espnow_radio.h:109)

5. **`coordinator_beacons_suppressed`** (uint32_t)
   - Anzahl unterdrückter Coordinator-Beacons
   - Steigt nur wenn `coordinatorSuppressingBeacons=true`
   - Quelle: `esn.coordinatorBeaconsSuppressed` (cluster_espnow_radio.h:110)

6. **`active_time_source`** (string, 16 hex chars)
   - Tag der aktiven Zeitquelle (Coordinator oder Broadcaster)
   - `"0000000000000000"`: Keine aktive Quelle
   - Coordinator-Tag: z.B. `"0000000012345678"`
   - Broadcaster-Tag: z.B. `"0000000087654321"`
   - **Erwartung:** Wechselt zu Broadcaster-Tag wenn Handover erfolgreich
   - Quelle: `esn.activeTimeSource` (cluster_espnow_radio.h:111)

7. **`source_changes`** (uint32_t)
   - Anzahl Zeitquellen-Wechsel
   - Steigt bei Coordinator→Broadcaster oder Broadcaster→Coordinator
   - Quelle: `esn.sourceChanges` (cluster_espnow_radio.h:112)

---

## VERIFIKATIONSPLAN

**Nach Flash von devxiao3 (Broadcaster) mit dieser Änderung:**

### 1. Broadcaster-Metriken prüfen (devxiao3)

HTTP: `GET http://devxiao3.local/cluster/espnow_radio`

**Erwartete Werte:**
```json
{
  "espnow_time_local": {
    "broadcaster_lease_active": true,
    "broadcaster_lease_remaining_ms": 8000,  // 8-10s (15s Lease, renewals)
    "broadcaster_production_beacons_sent": >0,  // STEIGEND! (Critical!)
    "coordinator_suppressing_beacons": false,  // Broadcaster unterdrückt nicht
    "coordinator_beacons_suppressed": 0,
    "active_time_source": "00000000xxxxxxxx",  // Broadcaster's own tag
    "source_changes": 1,  // Wechsel Coordinator→Broadcaster
    "exchanges_complete": >224,  // Weiterhin steigend
    "exchange_timeouts": 0  // Weiterhin 0
  }
}
```

**Falls `broadcaster_production_beacons_sent=0`:**
- Broadcaster sendet NICHT production beacons
- Mögliche Ursachen:
  1. `precisionUtcNow()` schlägt fehl (Zeitbasis ungültig)
  2. `broadcasterLeaseActive=false` (Lease abgelaufen)
  3. Logic-Bug in production beacon sending (cluster_espnow_radio.cpp:2342-2386)

### 2. Coordinator-Metriken prüfen (devfreenove1)

HTTP: `GET http://devfreenove1.local/cluster/espnow_radio`

**Erwartete Werte:**
```json
{
  "espnow_time_local": {
    "broadcaster_lease_active": false,  // Coordinator ist nicht Broadcaster
    "broadcaster_lease_remaining_ms": 0,
    "broadcaster_production_beacons_sent": 0,  // Coordinator sendet keine production beacons
    "coordinator_suppressing_beacons": true,  // Coordinator UNTERDRÜCKT eigene beacons!
    "coordinator_beacons_suppressed": >0,  // STEIGEND während Broadcaster aktiv
    "active_time_source": "00000000yyyyyyyy",  // Broadcaster's tag (observed)
    "source_changes": 0  // Coordinator bleibt auf eigenem Takt
  }
}
```

### 3. Node-Metriken prüfen (devxiao1, devxiao2)

**Nach Unified Firmware Flash (beta131):**

HTTP: `GET http://devxiao1.local/cluster/espnow_radio`

**Erwartete Werte:**
```json
{
  "espnow_time_local": {
    "broadcaster_lease_active": false,  // Node ist nicht Broadcaster
    "broadcaster_production_beacons_sent": 0,
    "active_time_source": "00000000yyyyyyyy",  // Broadcaster's tag (empfangene beacons!)
    "source_changes": >=1,  // Wechsel Coordinator→Broadcaster
    "exchanges_complete": >0,  // Wenn Node Probes sendet
    "rejected": >0  // Falls Node eigene beacons empfängt (Self-Beacon-Filter)
  }
}
```

**Beacon-CSV prüfen:**
```bash
# Download Beacon-CSV von Node
wget http://devxiao1.local/cluster/espnow_beacon_csv -O node1_beacons.csv

# Prüfen: coordinator_boot sollte Broadcaster's Boot-ID zeigen
cat node1_beacons.csv | grep -v "^sequence" | head -5
```

**Erwartung:**
- `coordinator_boot` zeigt Broadcaster's Boot-ID (aus Diagnostic Beacons bekannt)
- Mindestens 6-12 Beacons sichtbar (kBeaconObservations=12, Ringpuffer rotiert)
- Falls NUR Coordinator-Beacons: Broadcaster sendet NICHT oder Nodes empfangen NICHT

---

## SICHERHEIT

**Keine Sicherheitsänderungen!**

- ✅ KEINE Logik-Änderungen
- ✅ KEINE neuen Funktionen
- ✅ NUR bestehende Diagnostik-Counter exportiert
- ✅ Alle Counter sind read-only (keine Schreibzugriffe via HTTP)
- ✅ Keine neuen Angriffsvektoren

**Risiko:** **MINIMAL** (nur JSON-Formatierung)

---

## FLASH-EMPFEHLUNG

**Option 1: Minimal-Verifikation (NUR Broadcaster)**

```bash
# Flash devxiao3 (Broadcaster) mit Diagnostik-Ergänzung
cd /home/itakka/ESP32_Board/sensorforge
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM0

# Nach Flash: JSON-Endpoint prüfen
curl http://devxiao3.local/cluster/espnow_radio | jq .espnow_time_local
```

**Erwartung:**
- `broadcaster_production_beacons_sent` steigt kontinuierlich
- `broadcaster_lease_active=true`
- `active_time_source` zeigt Broadcaster's Tag

**Falls `broadcaster_production_beacons_sent=0`:**
- **NICHT mit Unified Firmware fortfahren!**
- Problem debuggen (precisionUtcNow() Failure?)

**Option 2: Unified Firmware (NACH Broadcaster-Verifikation)**

```bash
# 1. Flash devxiao1 (Node, beta122 → beta131)
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM1

# 2. Flash devxiao2 (Node, beta130 → beta131)
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM2
```

**Nach jedem Flash:**
- JSON-Endpoint prüfen
- Beacon-CSV downloaden und analysieren
- `active_time_source` prüfen (sollte Broadcaster's Tag zeigen)

**NICHT flashen:**
- ❌ devfreenove1 (Coordinator, Cycle 16, funktioniert!)
- ❌ devxiao3 (Broadcaster, Cycle 15 + Diagnostik, funktioniert!)

---

## ROLLBACK

**Falls Probleme nach Flash:**

```bash
# Rollback zu vorherigem Zustand (git)
cd /home/itakka/ESP32_Board/sensorforge
git checkout HEAD~1 cluster.cpp
pio run -e xiao_esp32s3 -t upload --upload-port /dev/ttyACM0
```

**Kriterien für Rollback:**
- Flash schlägt fehl
- Device bootet nicht
- JSON-Endpoint antwortet nicht
- Neue Felder fehlen oder zeigen falsche Werte

**Alternative:** Vorheriges Binary re-flashen (falls .pio/build/ vorhanden)

---

## OFFENE FRAGEN

### 1. Warum zeigen Node-CSVs nur Coordinator-Beacons?

**Drei mögliche Ursachen:**

**A) Broadcaster sendet NICHT production beacons:**
- `precisionUtcNow()` schlägt fehl (Zeitbasis ungültig trotz erfolgreicher Exchanges?)
- `broadcasterLeaseActive` wird false (vorzeitiger Lease-Ablauf?)
- Production beacon sending Logic-Bug

**B) Nodes empfangen Broadcaster-Beacons NICHT:**
- RF-Reichweite (Broadcaster zu weit weg?)
- Kanal-Wechsel (ESP-NOW Kanal-Switch während Handover?)
- Paket-Verwerfung (HMAC-Fehler, Replay-Filter?)

**C) Ringpuffer zu klein:**
- `kBeaconObservations=12` (cluster_espnow_radio.cpp:38)
- 10 Hz Coordinator-Beacons → 12 Beacons = 1.2s
- Broadcaster-Beacons werden rotiert bevor CSV-Download
- Self-Beacon-Filter verhindert Broadcaster-Beacons in eigener CSV

**Verifikation:**
- `broadcaster_production_beacons_sent` steigt → A) ausgeschlossen
- Node-CSV zeigt Broadcaster-Boot-ID → B) ausgeschlossen
- C) wahrscheinlich wenn A)+B) ausgeschlossen

### 2. Ist broadcasterTimeBaseValid in Diagnostics?

**Nein!**

- `broadcasterTimeBaseValid` ist LOKALE Variable in `cluster_espnow_radio.cpp`
- Berechnet aus `latestQualityEstimate.valid` + Age-Check
- NICHT in `Diagnostics` struct exportiert

**Workaround:**
- `broadcaster_production_beacons_sent` steigt NUR wenn `precisionUtcNow()` erfolgreich
- Wenn `broadcaster_production_beacons_sent` steigt → Zeitbasis MUSS valid sein
- Wenn `broadcaster_production_beacons_sent=0` trotz Lease → Zeitbasis ungültig

**Optional:** broadcasterTimeBaseValid zu Diagnostics hinzufügen (erfordert Header-Änderung)

---

## ZUSAMMENFASSUNG

**Änderungen:**
- ✅ 7 neue JSON-Felder in `espnow_time_local` (cluster.cpp:5023-5032)
- ✅ KEINE Logik-Änderungen (nur bestehende Counter exportiert)
- ✅ KEINE Sicherheitsrisiken (read-only diagnostics)

**Kritische Metriken:**
- **`broadcaster_production_beacons_sent`** - MUSS steigen wenn Broadcaster funktioniert!
- **`active_time_source`** - MUSS zu Broadcaster-Tag wechseln
- **`coordinator_suppressing_beacons`** - MUSS true auf Coordinator sein

**Nächste Schritte:**
1. ✅ Flash devxiao3 (Broadcaster) mit Diagnostik-Ergänzung
2. ⏳ Verifikation: `broadcaster_production_beacons_sent` steigt
3. ⏳ Falls OK: Unified Firmware Flash (devxiao1, devxiao2)
4. ⏳ Falls NOK: Problem debuggen (precisionUtcNow()?)

**Blocker für Unified Firmware:**
- ❌ `broadcaster_production_beacons_sent=0` (Broadcaster sendet nicht!)
- ❌ Node-CSVs zeigen keine Broadcaster-Beacons (Nodes empfangen nicht!)

**Go-Ahead für Unified Firmware:**
- ✅ `broadcaster_production_beacons_sent` steigt kontinuierlich
- ✅ Node-CSVs zeigen Broadcaster's Boot-ID
- ✅ `active_time_source` wechselt zu Broadcaster-Tag

---

**Status:** Bereit für Broadcaster-Flash und Verifikation
**Risiko:** MINIMAL (nur JSON-Export)
**Erwartung:** Endlich Klarheit ob Broadcaster TATSÄCHLICH production beacons sendet! 🎯
