# ESP-NOW: TX-Callback-Latenz (Diagnose)

## Zweck

Misst auf dem Coordinator die Zeit vom Aufrufkontext `esp_now_send()` (Zeitstempel unmittelbar vor dem Aufruf) bis zum ESP-NOW-Sendecallback. Der Wert ist **kein** MAC-/PHY-TX-Zeitstempel und kein Beweis einer bestimmten Synchronisationsgenauigkeit. Callback-Erfolg meldet nicht, dass ein Node eine Uhrkorrektur akzeptiert hat.

## Ausgabe

`TX-Sendecallback zuletzt/max` in µs, Anzahl zugeordneter Callbacks, fehlgeschlagene TX-Callbacks, `Zuordnung verloren` und `Queue voll`. `RX-Queue` und `Reply→Final` bleiben erhalten. Werte sind kumulative Maxima seit dem ESP-NOW-Start; bei Null Samples keine Latenzinterpretation.

Die Callback-Zuordnung verwendet Ziel-MAC plus ältesten noch ausstehenden Send; das ESP-NOW-Sendecallback liefert keine eigene Sequenznummer. Bei überlappenden Sends an denselben Peer kann die Zuordnung unscharf werden. Daten werden ohne Wartezeit im kurzen Callback verarbeitet. Fehlende Callbacks werden nach 2 s aus dem begrenzten Zuordnungspuffer verdrängt; `Zuordnung verloren` kennzeichnet nicht zuordenbare Rückmeldungen. Sende-Aufruf- und TX-Callback-Dauer darf nicht direkt als Offsetfehler eingesetzt werden.

## Vergleichstest (1 Coordinator + 1 Node)

1. Den Stream **normal aktiviert** lassen, Node im Drone Mode, 2–3 Sync-Zyklen abwarten. Coordinator-Werte `RX-Queue zuletzt/max`, `Reply→Final`, `TX-Sendecallback zuletzt/max`, TX-Fehler/Zähler und Node-Klartextdiagnose speichern.
2. Stream abschalten, sofern das ohne Kamera-Neustart möglich ist. Nach Möglichkeit ESP-NOW neu starten, um Maxima zurückzusetzen, sonst auf neue `zuletzt`-Werte und steigende Zähler achten. Gleiche Messdauer und Bedingungen.
3. Streuung, RTT, Anzahl angenommener Serien und Callback-Verteilung vergleichen. Ein bloßes Maximum aus einem alten Testlauf ist kein valider Vergleich.
4. Keine Qualitätsgrenzen verändern. Falls sich Jitter verbessert, Sendestempelpfad und WLAN-Last genauer untersuchen. Endgültige Node-zu-Node-Genauigkeit mit gemeinsamem GPIO-Logikanalysator testen.

## Kompatibilität und Grenzen

ESP-IDF 5.5+ nutzt die neue TX-Callback-Signatur; ältere IDF-Versionen die MAC-Adresse direkt. Der vollständige Arduino-Board-Build und Hardwarevalidierung stehen aus. Für diesen Coordinator-Diagnosepatch müssen Nodes nicht neu geflasht werden. Die Meldung `UTC-Zeit nicht verfügbar` bleibt unabhängig davon offen.
