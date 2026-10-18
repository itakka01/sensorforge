# SensorForge v87-beta131 – Test Suite

## Offline Regression Tests

Diese Tests sind **NICHT** Teil des Arduino-Builds. Sie werden separat mit `g++` kompiliert.

### Test-Dateien

1. **test_fixed_series.cpp.test** (Cycle 21)
   - Tests für feste 16-Sample-Serien
   - Drift-Akkumulation-Verifikation
   - nodeReferenceUs-Konsistenz
   - Forward-Propagation-Kompatibilität

2. **test_lease_state_machine.cpp.test** (Cycles 8-20)
   - Broadcaster-Lease-State-Machine
   - Coordinator-Suppression-Timing
   - Source-Switching-Validierung
   - Capture-Invalidierung

### Tests ausführen

```bash
# Test 1: Fixed Series (Cycle 21)
mv test_fixed_series.cpp.test test_fixed_series.cpp
g++ -o test_fixed_series test_fixed_series.cpp cluster_espnow_twoway.cpp -std=c++17 -I.
./test_fixed_series
mv test_fixed_series.cpp test_fixed_series.cpp.test

# Test 2: Lease State Machine
mv test_lease_state_machine.cpp.test test_lease_state_machine.cpp
g++ -o test_lease_state_machine test_lease_state_machine.cpp -std=c++17
./test_lease_state_machine
mv test_lease_state_machine.cpp test_lease_state_machine.cpp.test
```

### Warum `.test` Endung?

Die Arduino IDE kompiliert **ALLE** `.cpp` Dateien im Projekt-Verzeichnis. Beide Test-Dateien enthalten eine `main()` Funktion, was zu einem Linker-Fehler führt:

```
multiple definition of `main'
```

Durch Umbenennung zu `.cpp.test` werden sie vom Arduino-Build ausgeschlossen.

### Erwartete Ergebnisse

**test_fixed_series:**
```
Test Results: 6/6 PASSED
```

**test_lease_state_machine:**
```
Test Results: 13 PASSED, 0 FAILED
```

### Bei Test-Fehlern

Falls ein Test fehlschlägt:
1. Prüfe, ob `cluster_espnow_twoway.cpp` unverändert ist
2. Prüfe, ob `cluster_espnow_radio.cpp` die Cycle 21 Änderungen enthält
3. Vergleiche Test-Output mit erwarteten Werten in Dokumentation

### Integration mit Arduino-Build

Die Tests sind **OFFLINE** und simulieren die ESP-NOW-Logik ohne Hardware. Sie können:
- ✓ Mathematische Korrektheit verifizieren
- ✓ State-Machine-Logik testen
- ✓ Timing-Verhalten simulieren
- ✗ **NICHT** testen: Echte ESP-NOW-Kommunikation, WiFi-Delays, Hardware-Interrupts

Für Hardware-Tests: Firmware flashen und Serial-Monitor überwachen.
