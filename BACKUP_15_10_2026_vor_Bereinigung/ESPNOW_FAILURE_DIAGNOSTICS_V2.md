# ESP-NOW Failure diagnostics v2 — SensorForge beta105

Basis: sensorforge_2026-10-13_02-06-16.zip plus espnow_failure_telemetry_delta.

## Änderungen

- Node meldet auch vor dem ersten gültigen Sync HMAC-authentifizierte Zähler und Serienfehler.
- Zusätzliche Diagnoseframes (`NodeDriftReport`, `nonce`-Flags `0x400` und `0x800`) liefern zuletzt verworfene Serien-RTT/Streuung, akzeptierte und ausgewählte Samples sowie Minimal-RTT- und Quartilsmedian-Offset. Diese Frames sind **keine gültige Uhrkalibrierung**.
- Coordinator ordnet die Diagnose über die Node-Boot-ID zu und zeigt Details pro Node an.
- Die ESP-NOW-Auswahl der lokalen Node-Uhr wird im normalen Driftbericht als Bit 1 gemeldet. `false` bedeutet lediglich: ESP-NOW ist nicht als präzise UTC-Quelle gewählt; Wi-Fi oder ungültige UTC sind dann noch zu unterscheiden.
- Nach wiederholter Serienablehnung Wartezeit 60/120/180 Sekunden (ab drei aufeinanderfolgenden Fehlschlägen 180 Sekunden). Bei akzeptierter Serie wieder 60 Sekunden.
- Browser erhält die geöffneten `<details>` je Node-Boot-ID über Statusaktualisierungen hinweg.

## Bewusste Grenzen

- Kein veränderter Offset-Estimator und **keine gelockerten Qualitätsgrenzen** (`spread <= 250 µs`, `median RTT <= 30 ms`, mindestens 8 akzeptierte Samples).
- ESP-NOW-Sync-Freigabe, Capture-Abbruch und bestehende Zeitreferenz bleiben unverändert.
- Der Coordinator zeigt weiterhin eine eigenständige UTC-Warnung, die hier **nicht** behoben ist.
- Vollständiger ESP32-Build und Funk-/Capture-/Stream-Hardwaretest stehen aus.
- Firmware auf Coordinator **und** XIAOs aktualisieren, da die zusätzliche Diagnose über neue Flags im bestehenden HMAC-Rahmen läuft.
