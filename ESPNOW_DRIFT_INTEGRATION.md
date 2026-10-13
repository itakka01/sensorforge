# ESP-NOW: Driftmodell aus echten Zwei-Wege-Messserien

- Authentifizierte Probe/Reply/Final-Messungen bilden maximal 16er-Messserien.
- Nur valide Schätzungen mit mindestens acht akzeptierten Samples und Offset-Spanne <= 250 us gehen in die Driftberechnung ein.
- Erst zwei zeitlich getrennte Serien ergeben einen frischen Drift-Schätzwert. Abweichende Sprünge > 100 ppm gegenüber einem frischen Modell werden verworfen.
- NVS speichert frühestens ab zwei gültigen Driftpaaren, nur bei plausibler UTC-Systemzeit und mit bestehender 30-Minuten-Schreibsperre.
- Gespeicherter Drift ist nur ein Prior für dieselbe stabile Coordinator-ID. Nach einem Boot bleibt driftValid=false bis echte neue Messungen vorliegen.
- `diagnostics()` stellt Drift, Unsicherheit, Probenzahl und Zähler erfolgreicher Speicherung bereit; Anzeige und Nachregelung der Cluster-Uhr sind noch nicht angebunden.
- Wi-Fi-Sync und Systemuhr bleiben unverändert.

Hinweis: Software-Zeitstempel haben keine nachgewiesene Mikrosekunden-Messgenauigkeit. Volle ESP32-Builds und RF-Messungen sind noch erforderlich.
