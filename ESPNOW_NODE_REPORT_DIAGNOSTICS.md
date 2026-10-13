# Authentifizierte ESP-NOW Node-Diagnose

Stand: beta105 – Delta auf Coordinator-Capture-Guard und adaptive RTT/Regression anwenden.

Nodes senden nach der **ersten akzeptierten** Messserie etwa alle 5 Sekunden zwei signierte ESP-NOW-Berichte (Typ 6 Zeitstatus, Typ 7 Drift). Keine Meldung vor erfolgreicher Serie ist **kein Erfolg**. Berichte sind HMAC-gebunden an Cluster-Epoch/Coordinator-Tag und besitzen Replay-Zähler. Der Coordinator speichert maximal 12 volatile Datensätze, zugeordnet zur aktuellen Boot-ID der Node-Liste. Berichte anderer Boots werden nicht einer Node zugeordnet.

`NodeSyncReport`: signed 32-bit Offset in µs, uint32 RTT in µs, Samples (8 bit), akzeptiert-Flag und Alter in s (16 bit). `NodeDriftReport`: signed Drift in milli-ppm, Streuung uint16 µs, Drift-gültig Flag. Typen 6 und 7 verwenden das bestehende Paketformat und müssen auf beiden Seiten aktualisiert werden. Genauigkeitswerte sind Diagnosedaten, kein HW-Präzisionsnachweis.

Ein fehlender Bericht kann trotz steigender Probe-Antwortzähler auftreten: verfehltes Final, verworfene Qualitätsserie, keine Freigabe, Paketverlust oder abweichende Boot-ID. Die angezeigte Zeitquelle ist ausdrücklich **nicht zentral gemeldet**; die Auswahl `espnow_precision` oder Wi-Fi wird weiterhin lokal getroffen. Für eine vollständige Erklärung abgelehnter Serien braucht es in einem Folgeschritt signierte Fehlerstatusberichte.

Prüfen: Coordinator-Funk aktiv und Probes steigen; in der aufklappbaren Node-Diagnose erscheint nach gültiger Serie Alter, Offset, RTT, Samplezahl, Spread und später Drift. Nach Node-Reboot darf ein Bericht vom alten Boot nicht als aktuell erscheinen. Während Capture keine neuen Reports senden.

Kein vollständiger ESP32-Build oder Hardwaretest durchgeführt.
