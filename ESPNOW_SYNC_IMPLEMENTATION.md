# ESP-NOW-Zeitsynchronisation – Integrationsstand 1

Dieses Delta enthält **noch keinen ESP-NOW-Zeittransport**. Es liefert das Board-Flag und die persistente Driftkalibrierung als überprüfbare Grundlage.

## Bereits umgesetzt
- `BOARD_HAS_ESP_NOW=1` in beiden ESP32-S3-Boardprofilen.
- NVS-Speicherung von maximal vier Driftschätzungen je stabiler Coordinator-ID.
- Datensatzvalidierung (Version, Prüfsumme, physikalisch plausible ppm, Unsicherheit, mindestens 8 Stichproben, gültige UTC).
- Schreibbegrenzung auf höchstens einmal je 30 Minuten im laufenden Boot (Ausnahme: explizites `force`).
- Kein gespeicherter Offset; keine Änderung der System- oder Cluster-Uhr.

## Noch umzusetzen
- ESP-NOW-Initialisierung auf STA-Kanal ohne Kanalwechsel, Lifecycle bei WLAN-Reconnect.
- Separater HMAC-Schlüssel, abgesicherte Broadcasts (10 s), Replay-Schutz.
- Authentifizierte Unicast-Zweiwegmessserien je Node (60 s, 16 Stichproben) und Jitterfilter.
- Aus Zeitstempeln ermittelter Drift/Unsicherheitswert; nur diese qualitätsgeprüften Daten an `clusterEspNowDriftStore()` geben.
- Coordinator-UI, Diagnose und GPIO-Vergleich; zunächst keine Verwendung für Capture-Termine.
- ESP32-Build und Hardwaretests.

Ein NVS-Datensatz ist nur eine historische Startschätzung. Er bestätigt keine Synchronisierung. Die angegebene Mikrosekunden-Auflösung impliziert keine Mikrosekunden-Genauigkeit.
