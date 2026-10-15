# ESP-NOW last rejected series diagnostics

Basis: sensorforge_2026-10-13_17-00-58.zip. Kein Git-Patch; vier vollständige Ersatzdateien.

## Ergänzung

* Node merkt letzten verworfenen Serienabschluss (Grund, Zeitpunkt, RTT, Spread sowie zwei Offset-Schätzer sofern verfügbar) unabhängig davon, ob anschließend eine Messung erfolgreich war.
* Übertragung per HMAC-authentifiziertem vorhandenen ESP-NOW-Rahmenformat mit eigenem Diagnosemarker 0x2000; gegebenenfalls zusätzlicher bereits existierender Offset-Metrikrahmen 0x800. Alle 15 Sekunden im bestehenden Statistik-Report-Takt; nur wenn seit Radio-Start eine Serie verworfen wurde.
* Coordinator bindet Diagnose an Node-Boot-ID und zeigt Ursache, Alter und Messdaten zusätzlich zur vorhandenen Serienstatistik, auch bei aktiver Präzisionsuhr.
* Bei Timeout / ungültiger Schätzung zeigt UI keine erfundenen Streuungswerte.
* Werte bleiben nur im RAM bis zum Neustart. Das Alter in der Funkdiagnose ist auf 4095 Sekunden begrenzt.

## Bewusst unverändert

Vier-Zeitstempel-Algorithmus, adaptive Auswahl, 250-µs-Grenzwert, Driftmodell, exklusive Slots, Sync-Freigabe, Capture-Priorität, Zeitquellenwahl. Kein SD/NVS-Schreibzugriff.

## Geänderte Dateien

- cluster_espnow_radio.cpp
- cluster_espnow_radio.h
- cluster.cpp
- webconfig_cluster.cpp

Alle Coordinator- und Node-Firmwares gemeinsam aktualisieren, da ein ergänzender Protokollmarker verwendet wird.

## Prüfumfang

Static-Diff-Prüfung; Node.js --check für die statische Haupt-Cluster-Webskriptsektion bestanden. Zwei dynamisch zusammengesetzte Skriptfragmente sind nicht isoliert syntaktisch prüfbar. ZIP-Integrität geprüft. Kein ESP32-Vollbuild oder Hardwaretest.
