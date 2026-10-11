# SensorForge Hardware-Zeitmessung – Integrationsstand

## Aufspielen

Dieses Delta über die Datei-Struktur des zuletzt gelieferten Projekts
`sensorforge_2026-10-11_18-25-51.zip` kopieren. Es enthält die
zusammengeführten Cluster-Änderungen des Coordinator-Session-Patches,
den XIAO-Node-Impuls sowie die Web-Anbindung. Keine Teile selektiv
mit älteren `cluster.cpp`-Dateien überschreiben.

## Voraussetzungen vor dem ersten echten Versuch

1. Freenove als Coordinator, genau drei XIAO ESP32-S3 als Nodes.
2. Bei allen vier Geräten Drone Mode; Cluster-Uhr gültig; Nodes erreichbar.
3. Verdrahtung **vor dem Einschalten prüfen**: XIAO GPIO2 (D1)
   an Coordinator GPIO14 / GPIO21 / GPIO41, jeweils in Node-Reihenfolge;
   alle GND-Leitungen verbinden. Keine Fremdspannungen, kein 5V-Signal.
4. Die drei Freenove-Pins sind regulär Magnetschalter, Präsenzsensor und Radar-RX.
   Angeschlossene Sensoren können Signale treiben und das Messergebnis verfälschen
   oder elektrische Konflikte erzeugen. Nur für einen kontrollierten Versuchsaufbau
   mit getrennten bzw. hardwareseitig sicher entkoppelten Sensorleitungen nutzen.
5. **Explizite Freigabe nach Hardwareprüfung:** nur beim Freenove-Build in
   `board_config.h` die drei `CLUSTER_HW_TIME_IN_x` von `(-1)` auf
   `(14)`, `(21)`, `(41)` ändern. Standardmäßig bleibt die Funktion gesperrt.
6. Für XIAO-Build `BOARD_XIAO`, für Coordinator-Build `BOARD_FREENOVE` verwenden.
7. Den resultierenden Sketch zunächst vollständig mit dem verwendeten Arduino-
   ESP32-S3-Core kompilieren. In dieser Umgebung war kein Arduino-CLI/PlatformIO
   installiert: Firmware-Build und Hardwareverifikation sind **nicht erfolgt**.

## Ablauf

Cluster koordinieren -> drei Nodes auswählen -> Aktion
`Hardware-Zeitmessung` -> `Ausführen` -> Sicherheitsabfrage bestätigen.
Der Coordinator reserviert drei Eingänge, versendet signierte, 5 Sekunden im
Voraus geplante Impulse und nimmt erste steigende Flanken auf. Nach dem
Messfenster werden GPIOs freigegeben. Die Statuszeile zeigt
ARM-Bestätigungen und Messergebnis; Diagramm und CSV werden aktualisiert.
Für Drift mehrere **einzelne** Messungen im Zeitabstand wiederholen.

## Grenzen der Aussage

Messwerte sind **softwarebasierte Flankenzeitpunkte** auf der monotonen
Coordinator-Uhr, nicht die Belichtungszeit. Sie umfassen Timer-Dispatch-,
GPIO- und Interruptlatenzen und dürfen ohne Referenzmessung nicht als
kalibrierte Uhrabweichungen oder sub-Millisekunden-Genauigkeit ausgegeben werden.
Bei fehlendem ARM oder fehlender Flanke bleibt der Kanal `null`.
Freenove-GPIO48 ist kein unterstützter Node-Ausgang (RGB-LED-Konflikt).

## Gelieferte neue HTTP-Schnittstellen

- POST `/cluster_hw_time_start` mit `node1`, `node2`, `node3` (Integration-IDs)
- GET `/cluster_hw_time_status` (JSON mit ARM, Messverlauf, Drift)
- GET `/cluster_hw_time_csv` (CSV, max. 256 Messungen im RAM)

Keine automatische Serienmessung, kein automatisches Einschalten belegter Pins.
