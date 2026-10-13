# SensorForge beta105 – Clusteraktionen und CSRF (Delta)

## Befund
Browser-POSTs wurden vom CSRF-Bootstrap nur mit einem `_csrf`-Parameter in der URL versehen. Der Coordinator sendet Cluster-Befehle als `multipart/form-data`. Die zentrale WebServer-CSRF-Prüfung meldete HTTP 403 mit `Invalid or missing CSRF token`.

## Korrektur
`web_csrf.cpp`: Die Browser-`fetch`-Hilfe ergänzt bei POST und `FormData` den vorhandenen CSRF-Token zusätzlich direkt als `_csrf`-Feld im Formularinhalt. Die URL-Absicherung und serverseitige Prüfung bleiben bestehen. Kein globaler Bypass; die tatsächlichen Coordinator->Node-Clusterjobs werden weiterhin nach ihrem bestehenden Authentifizierungsmodell ausgeführt.

## Test
Nach Flashen des Coordinators, Seite neu laden und Drone Mode EIN testen. Erwartung: kein HTTP 403 mehr; HTTP-Annahme allein bestätigt keine Node-Ausführung. Auch `Verbindung testen` und andere Listenaktionen prüfen.

## Prüfstatus
Quellcodeänderung und ZIP geprüft. Vollständiger ESP32-Build/Hardwaretest offen.
