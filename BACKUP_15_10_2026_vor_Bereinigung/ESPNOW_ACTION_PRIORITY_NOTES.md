# ESP-NOW Hintergrundsync – Aktionen und Diagnose

Delta auf Basis des ESP-NOW-Capture-Prioritäts-Patches.

- Bei gültiger Coordinator-Listenaktion pausiert lokaler ESP-NOW-Funk für 10 s; eine angefangene Messserie wird verworfen, eine bereits gültige Kalibrierung bleibt für Holdover erhalten.
- Mehrere Aktionen verlängern das Fenster. Der Node erhält **noch kein separates signiertes Pausensignal** nur aufgrund der Coordinator-Anfrage. Für eine garantierte clusterweite Pause muss ein authentifizierter Pause/Resume-Transport ergänzt werden.
- Probeabstand: 120 ms -> 45 ms. 16 erfolgreiche Exchanges erfordern dadurch mindestens ca. 0,72 s zuzüglich Funk-/Service-Latenz; Timeouts bleiben möglich.
- Lokale Diagnose ergänzt Alter der letzten qualitätsgeprüften Messserie und verbleibende Funkpause; der Coordinator selbst ist keine Offset-Messstation.
- Coordinator zeigt je Node ausdrücklich **nicht gemeldet**, solange Node-Messberichte nicht authentifiziert zum Coordinator übertragen werden. Keine erfundenen Synczeiten.
- Keine Änderung am ESP-NOW-Qualitätsfilter oder an der bestehenden Wi-Fi-Zeitbasis.
- Vollständiger Arduino/ESP32-Build und Funk-/Capture-Praxistest weiterhin erforderlich.
