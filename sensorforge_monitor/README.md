# SensorForge Test Manager v1 (beta134)

Read-only stability diagnostics, Python 3.10+, no additional packages. Existing `sensorforge_monitor.py` included as the HTTP parser for compatibility.

## Start

1. Copy `sensorforge_config.example.json` to `sensorforge_config.json` and enter local HTTP credentials **only in your private copy**. Never publish that file.
2. Start `python sensorforge_test_manager.py` and choose a menu entry.
3. Or use `python sensorforge_test_manager.py --minutes 360`.
4. Reports are generated at the end (Ctrl+C also works). Rebuild with `python sensorforge_test_manager.py --report`.

Outputs: `sensorforge_tests/stability.sqlite3`, `stability_report.json`, `stability_report.md`, `test_metadata.json`. The SQLite DB contains full raw API payloads and connection metadata; review before sharing. It contains no configured HTTP password. Hostnames and IPs may still be present.

## Stability checks

- Each named device polled independently, so a failed seed or broken cluster discovery does not silently omit a node.
- DNS hostname is retried via freshly resolved IP when available; every attempt and error is saved.
- HTTP outages tracked per device, including unresolved outages at the end.
- Boot and coordinator-epoch transitions recorded; counter increases for replay, rejected frames, timeouts, and source changes recorded.
- Probe inactivity of at least 120 s (configurable) flagged if probe counter is present on non-coordinator nodes.
- Drift estimates and broadcaster lease snapshots retained from beta134 firmware diagnostics.

**Important:** `PASS_MONITORING_ONLY` never means absolute uptime, valid firmware behavior, time drift correction, or proven GPIO alignment. Shorter outages may occur between polls. An epoch change observed after all nodes rebooted is not proof of seamless epoch acceptance. Any failures are reported as FAIL, and missing evidence as INCOMPLETE. Controlled recovery tests and wired accuracy tests are separate future phases.

No automatic reboot, configuration update, flashing, or network changes. No outbound network requests except to hosts configured in the test configuration.

## Version 1.1 – Bedienung und Netzwerkdiagnose

Start: `python sensorforge_test_manager.py` (im Ordner mit `sensorforge_config.json`).

Menüoption 4: Stunden und zusätzliche Minuten getrennt eingeben. Für offene Laufzeit beide 0 wählen. Während des Tests mit `q` + Enter oder einmal `Ctrl+C` beenden; Berichte werden anschließend erzeugt. Im Terminal erscheinen Laufzeitbalken, DNS-Status, HTTP-Status, verwendeter Zugriffsweg, Probes, Exchanges und Replay-Zähler.

Optional `fallback_ips` in `sensorforge_config.json` mit den *tatsächlich aktuellen* IPv4-Adressen pro `.local`-Host befüllen. Falls mDNS/DNS nicht auflöst, wird die eingetragene IP direkt abgefragt. Auch IPs aus den `nodes`-Listen anderer erreichbarer Boards werden zur Laufzeit gelernt. Ein per IP erreichbares Gerät wird nur bei passender lokaler Host-Identität anerkannt. **Ohne konfigurierte oder gelernte IP ist kein unabhängiger IP-Fallback möglich.** DNS-Fehler und HTTP-Zustand werden je Abfrage in der Tabelle `network_checks` gespeichert; eine Wiederherstellung nur per IP gilt nicht als DNS-Erfolg.

Beim Testende werden Dateipfade und eine lokale `file://`-Adresse zu `stability_report.html` angezeigt. Den Bericht auf demselben Test-PC im Browser öffnen.

Hinweis: Terminal-Ausgabe ist kein GUI; der Abbruch wird über `q` + Enter oder `Ctrl+C` ausgelöst. Ein laufender HTTP-Aufruf kann bis zum eingestellten Timeout dauern. Für echte 24/72-Stunden-Tests das Terminal und den PC wach halten. Tests verursachen keine Änderungen an den Boards.

## Version 1.2 – HTML-Dashboard

`stability_report.html` enthält wieder eine eigenständige, offline nutzbare HTML-Auswertung mit Geräteübersicht, Netzwerkdiagnose, Vorfällen und SVG-Zeitreihendiagrammen (Probes, Exchanges, Replay, Drift, Spread, Beacons, Authentifizierung). Keine zusätzlichen Python-Pakete erforderlich. Die Diagramme zeigen Firmware-Schätzwerte und Zähler, keine GPIO-Genauigkeitsmessung.

Aufruf wie bisher: `python sensorforge_test_manager.py`. Vorhandene Messung neu auswerten mit Menüpunkt 5 oder `python sensorforge_test_manager.py --report`.

**Achtung:** Die alte `sensorforge_results/monitor.sqlite3` ist ein anderes Datenbankformat und wird von der neuen Berichts-Funktion nicht direkt verarbeitet. Für neue Testläufe die Dateien aus `sensorforge_tests` bereitstellen.
