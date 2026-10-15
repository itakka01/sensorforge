# ESP-NOW Fehlerserien-Diagnose – beta105

## Fehlerursache in Ausgangsstand

`sendNodeReports()` beendete sich vor dem ersten akzeptierten Präzisions-Sync sofort (`if(!lastPrecisionMs)return`). So konnte der Coordinator gerade bei fehlgeschlagenen Serien nichts anzeigen.

## Änderung

Nodes übertragen alle etwa fünf Sekunden auch vor dem ersten Erfolg einen HMAC-authentifizierten Diagnoseframe vom bestehenden Typ `NodeSyncReport` mit Bit `0x200` im `nonce`. Dieser Frame ist **kein Synchronisationserfolg**: Die beiden 32-Bit-Hälften von `timeUs` tragen dann `probesSent` und `exchangesComplete`, `nonce` Bits 16–23 den Fehlercode und Bits 24–31 die gesättigte Zahl gescheiterter Serien. Im Erfolgsfall bleiben die bestehenden Frames erhalten. Der Coordinator verknüpft die Berichtsdaten mit der aktuellen Boot-ID; die UI stellt Zähler und Ablehnungsgrund aufklappbar dar.

Fehlercodes: 0=noch kein Ergebnis, 1=invalid/zu wenig Samples, 2=Streuung >250µs, 3=Median-RTT >30ms, 4=12s Serienzeitlimit. Pakete bleiben HMAC-/Replay-geprüft. Die bisherigen Genauigkeitsfilter wurden NICHT gelockert.

## Einschränkungen

Nur der letzte Fehlercode wird übertragen, nicht alle per-Sample-Latenzen oder Sende-Callbacks. `exchangesComplete` zählt vollständige Proben, nicht erfolgreich akzeptierte Messserien. Ein Send-Aufruf bestätigt keine Zustellung. Ein fehlender Bericht kann weiterhin auf Funkzustellung oder Boot-ID-Zuordnung zurückgehen. Nächster Schritt je nach angezeigtem Fehler: gezielte Prüfung von Reply/Final, Reihenfolge, Freigabefenster und Zeitbasis. Die leere separate Wi-Fi-Zeittabelle und `UTC-Zeit nicht verfügbar` sind damit nicht behoben.

Firmware auf Coordinator UND ALLEN Nodes aktualisieren. Kein vollständiger Arduino-ESP32-Build oder Hardwaretest ausgeführt.
