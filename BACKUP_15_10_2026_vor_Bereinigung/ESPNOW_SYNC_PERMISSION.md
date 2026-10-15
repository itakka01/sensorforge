# ESP-NOW Sync-Freigabe (Delta)

## Verhalten
- Die bisherigen `pauseForCapture()`-Aufrufe aus Coordinator-Listenaktionen und Capture-Planung entziehen sofort die ESP-NOW-Messfreigabe am Coordinator und senden eine authentifizierte `SyncControl`-Nachricht mit `timeUs=0` (Widerruf).
- Neue kurze `SyncControl`-Freigaben (`timeUs=12000`, Millisekunden) werden nur außerhalb einer lokalen Schutzphase alle 5 Sekunden übertragen.
- Nodes starten bzw. setzen Probe/Reply/Final-Messserien ausschließlich mit gültiger Freigabe fort. Beim verifizierten Widerruf werden begonnene Messserien verworfen; ohne weitere Freigabe erlischt die Berechtigung spätestens nach Ablauf der empfangenen Lease.
- ESP-NOW bleibt empfangsbereit. Die bereits kalibrierte Zeitbasis/Drift bleibt zur Aufnahmeterminierung erhalten (Holdover).
- Wi-Fi-Clusterzeit bleibt Fallback. Aktuelle Freigabe kann über `diagnostics().syncPermission` und `.syncPermissionRemainingMs` ausgelesen werden.

## Grenzen und Tests
- Das Stoppsignal wird über unzuverlässigen Funk übertragen. Bei Verlust greift erst der Lease-Ablauf (max. 12 s seit letzter Freigabe). Ein *sofortiger* Abbruch bei allen entfernten Nodes kann ohne empfangene Steuerinformation nicht garantiert werden.
- Listeneingänge rufen bisher eine lokale Schutzphase von 10 s auf. Für längere Aktionen ist eine spätere Verbindung mit deren realen Zustandsende statt einem festen Zeitfenster nötig.
- `SyncControl` ist eine Protokollerweiterung. Coordinator und Nodes müssen denselben neuen Code verwenden.
- Auf Hardware müssen die genaue Zustellung der Widerrufe, das Ausbleiben von Probes während Jobs, der Ablauf nach Funkverlust und die Qualität der Zeitkalibrierung geprüft werden.
- Implementierung ist kein Nachweis für µs-Präzision oder ein vollständiger ESP32-Build.
