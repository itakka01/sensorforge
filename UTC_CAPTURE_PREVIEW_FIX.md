# UTC-Capture-Vorschau: Status-Aktualisierung

## Ursache
`sfCaptureClock` wurde bisher ausschließlich beim Klick auf „Aufnahme planen“ aus der Coordinator-Antwort initialisiert. Die regelmäßige Cluster-Statusabfrage aktualisierte die Zeitreferenz nicht. Deshalb zeigte die relative UTC-Vorschau selbst bei verfügbarer `cluster_time.utc_us` dauerhaft „UTC-Zeit nicht verfügbar“.

## Korrektur
Der bestehende Status-Callback `sfCaptureReportUpdate(d)` aktualisiert die monotone Browser-Ankerzeit mittels `sfCaptureClockFromStatus(d, performance.now())` und die Vorschau. Bei ungültiger Clusterzeit wird der Anker verworfen. Es werden keine zusätzlichen HTTP-Requests gestartet und kein Fallback auf `Date.now()` als autoritative UTC eingeführt. Der Aufnahme-Submit fordert wie bisher vor dem Auftrag einen gültigen Coordinator-Zeitstatus an und validiert das 5-s-bis-30-min-Fenster.

## Umfang
Nur `webconfig_cluster.cpp`; Coordinator neu flashen. ESP-NOW, Captureserver, Nodes und deren Firmware unverändert.

## Test
Quelltextprüfung und JavaScript-Syntaxprüfung; kein vollständiger ESP32-Build und kein Hardwaretest. Im Browser prüfen: gültige Clusterzeit => „Geplant ... UTC“; ungültige Clusterzeit => „UTC-Zeit nicht verfügbar“; Capture-Submit wird weiterhin bei ungültiger Uhr gesperrt.
