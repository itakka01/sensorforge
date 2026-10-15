# SensorForge konsolidierter Quellstand v87-beta121

Dies ist ein rekonstruierter und bereinigter **Quellstand**, kein auf ESP32 vollstaendig kompilierter oder freigegebener Release.

## Zusammenstellung
Grundlage: sensorforge_2026-10-14_11-35-03.zip; danach beta114, beta115 (freigegebener Diagnose-Standby, OHNE experimentelle Handover-Dateien), beta118 (Coordinator-Modus-Erkennung), beta119 (Diagnose-Beacons plus Web-Endpunkte) und beta121 (Low-RAM-Statistik). Die beta116/117-Oberflaechenfehler sind durch beta118 ersetzt, der speicherintensive beta120-Ringspeicher wurde durch beta121 ersetzt.

## Hardwareseitig bereits beobachtet
- devfreenove1: beta118, Coordinator und produktive Zeitquelle
- devxiao1: beta110, unveraenderter Referenz-Node
- devxiao2: beta121, empfaengt Diagnose-Beacons und liefert Low-RAM-Statistik
- devxiao3: beta119, Zeit-Broadcaster mit Diagnose-Beacons (nicht die produktive Zeitquelle)
- devxiao2 beta120 verursachte einen haengenden Webserver und wurde zurueckgesetzt.

## Was NICHT integriert/freigegeben ist
- Authentifiziertes Enrollment/Handover und Lease-/Failover-Steuerung
- Automatisches Stummschalten des Coordinator-Zeitsenders
- Umschaltung der produktiven Capture-Uhr oder 250-us-Genauigkeitsgarantie

## Bauhinweise
Dies ist eine Quellkopie zum Archivieren und als einheitliche Basis fuer die naechsten Aenderungen. Der existierende Arduino-Projektordner darf nicht blind ueberkopiert werden. Eine vollstaendige Arduino-Kompilierung ist hier nicht verfuegbar; die bestehenden gemischten Geraeteversionen NICHT allein aufgrund dieses Archivs aendern.

## Bereinigung
Die experimentellen cluster_time_handover_*, cluster_time_enrollment_*, cluster_time_sender_binding_* und test_*.cpp aus vorherigen Arbeitsarchiven werden NICHT beigelegt. Benutzte produktive Diagnosedateien cluster_time_diagnostic_beacon.h/.cpp sind enthalten.
