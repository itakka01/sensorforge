# ESP-NOW-Zeitbasis: Integration und Teststand

Dieses Delta ergänzt die bestehenden ESP-NOW-Probe/Reply- und Driftmodule und verwendet deren **frische** Schätzung in `clusterTimeNowUs()`.

## Funkzeit und UTC

Die vier Zeitstempel der Zwei-Wege-Messung sind **monoton**, nicht UTC. Ein authentifizierter Beacon überträgt deshalb jetzt zusätzlich eine UTC/Monoton-Paarung: `timeUs` enthält UTC in Mikrosekunden, `nonce` die niederwertigen 32 Bit der monotonic ESP-Timer-Uhr des Coordinators. Die Rundungsauflösung des Beacon-Paars ist softwarebedingt; der tatsächliche WLAN-Sendezeitpunkt ist nicht hardwaregestempelt. Der Node rekonstruiert den 64-Bit-Zeitanker nur, wenn eine frische und plausible Präzisionsmessung vorliegt. Sonst wird der Anchor verworfen.

Für ein gültiges ESP-NOW-Zeitmodell werden vorausgesetzt:

- aktiver Drone Mode, authentifizierte Cluster- und Coordinator-/Epoch-Zuordnung;
- mindestens 8 akzeptierte Messungen aus einer Serie, maximale Offset-Streuung 250 µs und Median-RTT maximal 30 ms;
- Präzisionsmessung höchstens 120 Sekunden alt;
- authentifizierter UTC-Zeitbeacon höchstens 30 Sekunden alt;
- für die Drift nur plausible und frische Werte, gespeicherter NVS-Drift allein genügt nicht.

Erst dann liefert `clusterTimeNowUs()` ESP-NOW-UTC und die JSON-Quelle lautet `espnow_precision`. Andernfalls erfolgt der bisherige Wi-Fi-Fallback, falls dessen Zeitwert noch gültig ist. Der Coordinator verwendet weiter seine eigene Systemzeit. Die Systemzeit auf Nodes wird nicht verstellt.

Die UTC-Broadcast-Nutzlast unterscheidet sich semantisch vom vorigen Diagnosestand: **alle ESP-NOW-Geräte des Clusters müssen mit dieser Änderung aktualisiert werden**, bevor man die ESP-NOW-Präzision beurteilt. Authentifizierte unicast Probe/Reply/Final-Nachrichten bleiben monotone Zeitstempel.

## Validierung auf echter Hardware ist erforderlich

Kein erfolgreicher ESP32-Firmware-Build und keine Funk-/Hardwarezeitmessung wurden in dieser Umgebung durchgeführt. Mikrosekundenauflösung ist keine Aussage über Mikrosekundengenauigkeit. Vor präzisionskritischen Fotojobs bitte mit GPIO-Zeitmessung Abweichung, Drift und Ausreißer prüfen. Besonders Kanalwechsel, Coordinatorwechsel, Zeitbasiswechsel, Stromausfall und geplante Captures verifizieren. Die vorhandenen Nodes veröffentlichen weiterhin keine zentralen ESP-NOW-Offsets; dazu ist ein separater signierter Node-Bericht erforderlich.

Der Wert `uncertainty_estimate_us` ist beim ESP-NOW-Quellbetrieb auf `0` gesetzt und **kein Unsicherheitsmaß**; erst ein empirisch validiertes Qualitätsmodell darf eine echte Unsicherheitsangabe liefern. Die ESP-NOW-Quelle und ihre Qualität sind unter `cluster_time.source` und `espnow_time_local` erkennbar.
