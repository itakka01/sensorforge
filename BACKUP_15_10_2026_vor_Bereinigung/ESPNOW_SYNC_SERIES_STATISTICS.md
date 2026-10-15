# ESP-NOW Sync-Serienstatistik – Integration in Benutzerstand 2026-10-13 14:43:54

Quelle: sensorforge_2026-10-13_14-43-54.zip. Die Dateien in diesem ZIP sind vollständige Ersatzdateien, kein Git-Patch.

## Geänderte Dateien
- cluster_espnow_radio.cpp: zusätzliche Zähler für abgeschlossene, akzeptierte und abgewiesene Messserien; letztes Serienergebnis; authentifiziertes Zusatz-Diagnosepaket ca. alle 15 Sekunden.
- cluster_espnow_radio.h: additive Felder im lokalen Node-Berichtsdatentyp.
- cluster.cpp: additive JSON-Felder in espnow_node_reports.
- webconfig_cluster.cpp: Anzeige der erfolgreichen/verworfen Serien, Erfolgsquote und Alter/Ergebnis des letzten abgeschlossenen Versuchs sowohl bei gültigem als auch bei bislang fehlendem Sync.

## Scope und Betriebslogik
- Kein Eingriff in Slot-Auswahl, Slot-Laufzeiten, Four-Timestamp-Formel, Driftberechnung, Captures oder 250-µs-Grenze.
- Statistik-Zähler sind ab Start der ESP-NOW-Radio-Runtime und werden bei deren Stop/Neustart zurückgesetzt; NICHT persistent und nicht zwingend ab Geräte-Boot, falls der Funkstack später neugestartet wurde.
- Die letzte Serienentscheidung wird bei vollständig bewerteter Serie bzw. Serien-Zeitlimit aktualisiert. Ein unterbrochener oder widerrufener Slot wird nicht als verworfene Qualitätsserie gezählt.
- 1 zusätzliches authentifiziertes ESP-NOW-Paket je Node pro ~15 s. Dieses ist auf die bestehende HMAC/Replay-Verarbeitung angewiesen. Coordinator und Nodes müssen denselben Stand verwenden.
- Paketmarkierung 0x1000 im nonce eines NodeSyncReport. Erfolgs- und Fehlerzähler je 32 Bit im timeUs-Feld; Status im nonce, Alter auf ganze Sekunden gerundet (maximal 4095 s). Bestehende Erfolgs-/Fehlermeldungen bleiben kompatibel und werden nicht umgedeutet.
- Noch kein Ereignisringpuffer, Verfügbarkeitsanteil oder Persistenz; bewusst kleiner Diagnoseausbau.

## Installation
1. Laufenden Projektstand extern sichern.
2. Vier Quelldateien aus diesem ZIP in den SensorForge-Projektordner kopieren und ersetzen.
3. Freenove-Coordinator und alle drei XIAOs mit dem jeweils korrekten Boardprofil kompilieren und flashen.
4. Im Coordinator unter Cluster koordinieren die neuen Statistikzeilen pro Node prüfen. Nach Neustart beginnen alle Zähler wieder bei null.

## Validierung
- Ausgangs-ZIP und Änderungen programmgesteuert verglichen: nur die vier aufgeführten Quelldateien geändert.
- JavaScript der Coordinator-Seite mit Node.js `--check` syntaktisch geprüft.
- ZIP-Integrität geprüft.
- Vollständiger Arduino/ESP32-Build und Hardwaretest nicht durchgeführt.
