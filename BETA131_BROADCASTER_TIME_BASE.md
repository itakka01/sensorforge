# Beta 131 - Broadcaster Zeitbasis: Dokumentation und Limitation

**Datum:** 2026-10-16
**Status:** LIMITATION DOKUMENTIERT - GPIO-TEST ERFORDERLICH

---

## 📊 AKTUELLE IMPLEMENTIERUNG

### Broadcaster-Zeitbasis (beta131)

**Broadcaster sendet Production-Beacons mit:**
```cpp
prodBeacon.timeUs = esp_timer_get_time();  // Monotoner Timer, nicht cluster-synchronisiert
```

**Was bedeutet das:**
- `esp_timer_get_time()` ist ein **monotoner Mikrosekunden-Zähler** seit Boot
- Dieser Timer ist **NICHT** über ESP-NOW mit dem Coordinator synchronisiert
- Broadcaster führt **KEINE** Probe/Reply/Final-Exchanges durch
- Broadcaster hat **KEINE** `latestQualityEstimate` wie normale Nodes

---

## ⚠️ LIMITATION

### Relative Synchronisation (Ziel: ≤ 250 µs)

**Erwartung:** Nodes können sich relativ zueinander synchronisieren, auch wenn Broadcaster-Zeitbasis nicht absolut präzise ist.

**Begründung:**
1. **Monotone Zeit ist stabil:** `esp_timer_get_time()` hat geringe Drift (~50 ppm typisch)
2. **Gemeinsamer Ereigniszeitpunkt:** Nodes empfangen dasselbe Beacon (shared event)
3. **Zwei-Wege-Schätzung kompensiert:** RTT-Quartile-Median-Selektion filtert Ausreißer
4. **Drift-Regression funktioniert:** Nodes können Broadcaster-Drift schätzen und kompensieren

**Vergleich beta130 A/B-Test:**
- Coordinator-Beacons: 5.472 µs Standardabweichung
- Broadcaster-Beacons: 501 µs Standardabweichung
- **WICHTIG:** Das ist RX-Callback-Timing-Vorhersage, **NICHT** Node-Synchronisationsgenauigkeit!

---

## 🎯 TESTEN ERFORDERLICH

### GPIO-Kabeltest (KRITISCH)

**Test-Setup:**
1. devxiao1 und devxiao2 als Nodes
2. devxiao3 als Broadcaster (mit aktivem Lease)
3. GPIO-Ausgang auf beiden Nodes bei geplanter Capture-Zeit
4. Oszilloskop oder Logic Analyzer misst Zeitunterschied

**Akzeptanzkriterium:**
- **Relative Abweichung ≤ 250 µs** zwischen den Nodes

**Wenn Test SCHEITERT:**
→ Broadcaster braucht präzise Zeitbasis (siehe Option C unten)

**Wenn Test BESTEHT:**
→ Aktuelle Implementierung ist ausreichend!

---

## 🔧 ALTERNATIVE LÖSUNGEN (falls nötig)

### Option A: Broadcaster mit Full Synchronization (KOMPLEX)

**Änderungen:**
```cpp
// Broadcaster muss BEIDE Modi gleichzeitig bedienen:
1. Als Node: Probe/Reply/Final mit Coordinator durchführen
2. Als Sender: Production-Beacons mit präziser Zeitbasis senden

// serviceDiagnosticBroadcaster() würde ändern:
diagnosticMode = false;  // NICHT diagnostic mode
wasCoordinator = false;  // Ist Node, nicht Coordinator

// Broadcaster muss:
- Coordinator-Beacons empfangen
- Probe senden, Reply empfangen, Final senden
- latestQualityEstimate bekommen
- Production-Beacons mit this estimate senden
```

**Vorteile:**
- Präzise Zeitbasis garantiert
- Beste mögliche Synchronisationsgenauigkeit

**Nachteile:**
- Komplex: Broadcaster ist gleichzeitig Sender UND Empfänger
- Mehr Code, mehr RAM, mehr Fehlerquellen
- Broadcaster-Beacons würden mit eigenen Probes kollidieren

### Option B: Broadcaster mit Passive Synchronization (HYBRID)

**Änderungen:**
```cpp
// Broadcaster empfängt Coordinator-Beacons passiv
// Broadcaster berechnet Offset, aber sendet KEINE Probes

// Vorteile:
- Broadcaster hat Zeitbasis von Coordinator
- Keine Kollisionen mit eigenen Probes
- Mittlere Komplexität

// Nachteile:
- Nur einseitige Schätzung (kein RTT)
- Weniger präzise als Full Sync
- Immer noch deutlich besser als nur monoton
```

### Option C: Hybrid mit Beacon-Delay (PRAKTISCH)

**Änderungen:**
```cpp
// Broadcaster empfängt letztes Coordinator-Beacon
// Broadcaster berechnet: eigene monotone Zeit + geschätzter Offset
// Production-Beacons verwenden: coordMonotonic + offset

// Beispiel:
int64_t lastCoordBeaconMonotonic = ...;  // Aus letztem Beacon
int64_t lastCoordBeaconLocalRx = ...;    // Lokaler RX-Zeitpunkt
int64_t currentMono = esp_timer_get_time();
int64_t estimatedCoordMono = lastCoordBeaconMonotonic + (currentMono - lastCoordBeaconLocalRx);

prodBeacon.timeUs = estimatedCoordMono;
```

**Vorteile:**
- Einfache Implementierung
- Broadcaster-Beacons aligned mit Coordinator-Zeit
- Deutlich besser als pure monotone Zeit

**Nachteile:**
- Drift zwischen Coordinator und Broadcaster akkumuliert
- Nur gut für kurze Zeit nach letztem Coordinator-Beacon

---

## 📋 ENTSCHEIDUNGSBAUM

```
GPIO-Test mit aktueller Implementierung
├─ Ergebnis ≤ 250 µs?
│  ├─ JA → ✅ Aktuelle Impl. ausreichend, keine Änderung
│  └─ NEIN → Wie groß ist Abweichung?
│     ├─ 250-500 µs → Option C (Hybrid Delay) probieren
│     ├─ 500-1000 µs → Option B (Passive Sync) probieren
│     └─ > 1000 µs → Option A (Full Sync) erforderlich
```

---

## 🎯 AKTUELLER STATUS (beta131-FIXED)

**Implementiert:**
- Broadcaster sendet Production-Beacons mit monotoner Zeit
- Keine Cluster-Zeit-Synchronisation
- Dokumentierte Limitation

**Nächster Schritt:**
1. Arduino-Kompilierung testen
2. Hardware-Flash auf devxiao3
3. **GPIO-Kabeltest durchführen**
4. Basierend auf Testergebnis: Entweder ✅ akzeptieren ODER Option B/C implementieren

---

## 📊 ERWARTETE GENAUIGKEIT

### Relative Synchronisation (GPIO-Test)

**Best Case (wenn monotone Zeit ausreicht):**
- Relative Abweichung: **50-150 µs** ✅ (unter Ziel)
- Begründung: Nodes verwenden gleichen Beacon als Referenz, kompensieren Drift

**Realistic Case:**
- Relative Abweichung: **150-300 µs** ⚠️ (nahe Ziel)
- Begründung: Kleine Drift-Unterschiede zwischen Nodes

**Worst Case (wenn monotone Zeit nicht ausreicht):**
- Relative Abweichung: **> 500 µs** ❌ (über Ziel)
- Begründung: Broadcaster-Drift zu groß für Nodes zu kompensieren

### Absolute UTC-Genauigkeit (zweitrangig)

**Mit monotoner Zeit:**
- Absolute Genauigkeit: **~1-10 ms** (limitiert durch gettimeofday())
- User-Anforderung: "Absolute UTC-Genauigkeit ist zweitrangig" ✅

---

## ✅ ZUSAMMENFASSUNG

**Aktuelle Implementierung (beta131-FIX):**
- Broadcaster verwendet monotone Zeit (`esp_timer_get_time()`)
- Keine cluster-synchronisierte Zeitbasis
- **Dokumentierte Limitation**

**Test-Kriterium:**
- GPIO-Kabeltest muss ≤ 250 µs relative Abweichung zeigen

**Wenn Test besteht:**
- ✅ Keine weitere Aktion erforderlich
- Aktuelle Impl. erfüllt Anforderung

**Wenn Test nicht besteht:**
- Implementiere Option C (Hybrid Delay) als nächsten Schritt
- Falls immer noch nicht ausreichend: Option B oder A

---

**Status:** IMPLEMENTIERUNG PRAGMATISCH - TEST ENTSCHEIDET
