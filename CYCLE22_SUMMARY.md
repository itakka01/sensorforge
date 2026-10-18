# Cycle 22 – Zusammenfassung: Reversion und Machbarkeitsnachweis

## Status: ABGESCHLOSSEN

**Datum:** 2026-10-18
**Firmware:** v87-beta131 (unverändert)
**Cycle 21:** Bleibt bestehen (feste 16-Sample-Serien)

---

## Teil 1: Reversion der Cycle 22-Änderung ✓

### Was wurde revertiert?

**Cycle 22 Versuch (FEHLGESCHLAGEN):**
- Reply TX Timestamp (t3) NACH HMAC erfassen
- Ziel: 50% Spread-Reduktion
- **Problem:** Führte zu +366 µs systematischem Bias

**Mathematischer Beweis des Problems:**
```
Offset-Bias = (d1_probe - d3_reply) / 2

OLD (symmetrisch, beide vor HMAC):
  d1 ≈ 732 µs, d3 ≈ 732 µs
  → Bias ≈ 0 µs (Mittelwert)
  → Spread: ~62 µs (reale Variation)

Cycle 22 (asymmetrisch, Reply nach HMAC):
  d1 ≈ 732 µs, d3 ≈ 0 µs
  → Bias = +366 µs (SYSTEMATISCH!)
  → Erfüllt NICHT ≤250 µs GPIO-Genauigkeitsanforderung
```

### Code-Status nach Reversion

**cluster_espnow_radio.cpp Lines 762-782:**
```cpp
// Beta131-NOTE (Twenty-second cycle REVERTED): Capturing t3 after HMAC would reduce
// spread by ~50%, but introduces systematic +366µs bias in offset estimation.
// For ≤250µs GPIO accuracy requirement, symmetric pre-HMAC capture is better.
int64_t stampUs=0;
if(txStampOut) {
    stampUs=esp_timer_get_time();  // BEIDE vor HMAC (wie ursprünglich)
    *txStampOut=stampUs;
    if(!replyStamp)f.timeUs=stampUs;
}
```

### Verifikation ✓

**Alle Cycle 21 Tests bestanden:** 6/6 PASSED
- Test A: 16 samples, constant offset → spread=10 µs ✓
- Test B: 16 samples, -22 ppm drift → spread=6 µs ✓
- Test C: 64 samples drift verification → spread=23 µs ✓
- Test D: Excessive spread rejection → 300 µs correctly rejected ✓
- Test E: nodeReferenceUs consistency → 5.18 µs difference ✓
- Test F: Forward-propagation drift → 0.00 ppm error ✓

---

## Teil 2: Machbarkeitsnachweis Software-Korrektur ✓

### Konzept: Gemessene Pre-Send-Korrektur

**Statt Timestamps zu verschieben, messen und übertragen wir die Verzögerung:**

1. **Node sendet Probe:**
   - Erfasse `t1_early` VOR HMAC (Authentifizierung)
   - Messe `d1 = Zeit(nach esp_now_send) - t1_early`
   - Sende BEIDE authentifiziert: `(t1_early, d1)`

2. **Coordinator korrigiert:**
   - Empfängt `(t1_early, d1)`
   - Berechnet `t1_corrected = t1_early + d1`
   - Nutzt korrigierten Wert für Offset-Berechnung

3. **Analog für Reply:**
   - Coordinator sendet `(t3_early, d3)` im Final-Paket
   - Node korrigiert: `t3_corrected = t3_early + d3`

**Verbleibender Bias:** Nur Treiber-Asymmetrie (geschätzt ±25 µs)

### Simulations-Tests (test_software_correction.cpp.test)

**Ergebnisse: 19/19 PASSED**

**Test L – Realistisches Szenario:**

| Ansatz | Mittelwert Bias | Spread | Stddev | Erfüllt Anforderungen? |
|--------|----------------|--------|--------|------------------------|
| **OLD** | -2.50 µs | 62.50 µs | 15.91 µs | ✓ Beide <250 µs |
| **Cycle 22** | **+368.75 µs** ❌ | 40.00 µs | 14.52 µs | ❌ Bias >250 µs |
| **CORRECTED** | **-2.81 µs** ✓ | **15.00 µs** ✓ | **4.99 µs** ✓ | ✓ Beide <250 µs |

**Verbesserung CORRECTED vs OLD:**
- Bias: Gleich (~-3 µs, vernachlässigbar)
- Spread: **-76%** (62.5 → 15.0 µs)
- Stddev: **-69%** (15.91 → 4.99 µs)
- Robustheit: **Signifikant besser**

**Verbesserung CORRECTED vs Cycle 22:**
- Bias: **-99%** (368.75 → -2.81 µs)
- Spread: **-62%** (40.0 → 15.0 µs)
- **Erfüllt BEIDE Anforderungen** (<250 µs Spread UND GPIO-Genauigkeit)

### Weitere Test-Ergebnisse

**Test K – Worst-Case Treiber-Asymmetrie:**
- Maximale Treiber-Differenz: ±50 µs
- Verbleibender Bias: -50 µs
- **Immer noch weit unter 250 µs Anforderung** ✓

**Test I – Pre-Send-Variation eliminiert:**
- Pre-Send variiert stark (600-825 µs)
- Spread NACH Korrektur: **0.00 µs**
- **Komplette Elimination der HMAC-Timing-Variation** ✓

### Implementierungsanforderungen

**Protokollerweiterung:**
```cpp
// cluster_espnow_packets.h
struct Frame {
    // ... existing fields ...
    int64_t timeUs;           // t1 oder t3 (early capture)
    uint32_t nonce;
    uint32_t preSendDelayUs;  // NEU: gemessene Verzögerung
};
```

**Änderungen erforderlich:**
1. **cluster_espnow_packets.h:** Frame um `preSendDelayUs` erweitern (+4 bytes)
2. **cluster_espnow_packets.cpp:** encode/decode anpassen
3. **cluster_espnow_radio.cpp:**
   - Sende-Seite: Schreibe gemessenen Delay in Frame
   - Empfangs-Seite: Wende Korrektur an
4. **Versionierung:** kVersion = 2, Kompatibilitätslogik

**Deployment:**
- **Alle 4 Geräte gleichzeitig** (devfreenove1, devxiao3, devxiao1, devxiao2)
- **Grund:** Protokoll V1↔V2 inkompatibel
- **Alternative:** Fallback-Logik (komplexer)

**Aufwand:** 2-3 Tage

### Verbleibende technische Grenzen

**Was Software-Korrektur KANN:**
- ✓ Eliminiert Pre-Send-Variation (HMAC-Timing) komplett
- ✓ Reduziert Spread um 76% (62.5 → 15.0 µs)
- ✓ Erhält Bias nahe 0 µs (~-3 µs)
- ✓ Erfüllt beide Anforderungen (<250 µs Spread UND GPIO-Genauigkeit)

**Was Software-Korrektur NICHT KANN:**
- ✗ Misst Treiber-Queue-Verzögerung nicht (esp_now_send → RF)
- ✗ Erreicht exakte RF-Timestamp-Genauigkeit nicht (Hardware erforderlich)
- ✗ Garantiert perfekt 0 µs Bias nicht (Treiber-Asymmetrie ±25 µs verbleibt)

**Aber:** Für ≤250 µs Anforderung ist Software-Korrektur **ausreichend**.

---

## Finale Entscheidung

### ✓ IMPLEMENTIERBARE SOFTWARE-KORREKTUR MIT BEGRÜNDETEM NUTZEN

**Begründung:**

1. **Machbarkeit bewiesen:**
   - 19/19 Simulations-Tests bestanden
   - Klare Implementierung (Messung existiert bereits)
   - Protokollerweiterung unkompliziert (+4 bytes)

2. **Signifikanter Nutzen:**
   - 76% Spread-Reduktion (62.5 → 15.0 µs)
   - 69% Stddev-Reduktion (robustere Schätzung)
   - Beide Anforderungen erfüllt (<250 µs)

3. **Geringer Aufwand:**
   - 2-3 Tage Implementierung
   - Niedriges Risiko (isolierte Änderung)
   - Vollständige Test-Abdeckung

4. **Keine Sicherheitseinbußen:**
   - Volle HMAC-Authentifizierung erhalten
   - Replay-Schutz unverändert
   - Epoch/Boot/Tag-Validierung unverändert

5. **Verbleibende Grenzen akzeptabel:**
   - Treiber-Asymmetrie: ±25 µs (geschätzt)
   - Weit unter 250 µs Anforderung
   - Hardware-Timestamps wären besser, aber nicht erforderlich

---

## Dokumentation

**Hauptdokument:** `CYCLE22_REVERSION_AND_FEASIBILITY.md`
- Teil 1: Reversion und Grund
- Teil 2: Machbarkeitsprüfung (6 Prüfungen)
- Simulations-Ergebnisse
- Implementierungsplan
- Finale Entscheidung

**Test-Datei:** `test_software_correction.cpp.test`
- 19 Tests für OLD, Cycle 22, CORRECTED
- Vergleichstests
- Worst-Case-Szenarien
- Realistische Szenarien

**CHANGELOG:** Aktualisiert mit Cycle 22 Reversion + Feasibility Study

---

## Nächste Schritte (auf Anfrage)

**Falls Implementierung gewünscht:**

### Phase 1: Protokollerweiterung (1-2 Tage)
- Erweitere Frame-Struktur (+4 bytes)
- Implementiere Versionierung (V1→V2)
- Sende-/Empfangs-Logik anpassen

### Phase 2: Tests (1 Tag)
- Unit-Tests
- Integration-Tests (HMAC-Verifikation)
- Regressions-Tests (50 bestehende Tests)

### Phase 3: Deployment (koordiniert)
- Build für alle 4 Geräte
- Gleichzeitiges Flash (oder Fallback-Logik)
- Hardware-Verifikation

**Erwartete Hardware-Ergebnisse:**
- `last_series_spread_us`: ~15-30 µs (statt 325 µs)
- `offsetUs` median: ~±25 µs (statt ±3 µs, akzeptabel)
- `broadcaster_lease_active`: true (stabil)
- `broadcaster_precision_failed`: <5 (nur echte RF-Störungen)

---

## Zusammenfassung

**Cycle 22 Reversion:** ✓ Abgeschlossen, Tests bestanden
**Machbarkeitsnachweis:** ✓ Erbracht, Implementierung empfohlen
**Technische Grenze:** Treiber-Verzögerung ±25 µs (akzeptabel für ≤250 µs Anforderung)
**Implementierung:** Bereit, auf Anfrage (2-3 Tage Aufwand)

**Die gemessene Software-Timestamp-Korrektur ist die empfohlene Lösung zur Erfüllung beider Anforderungen (<250 µs Spread UND <250 µs GPIO-Genauigkeit) ohne Hardware-Timestamps.**
