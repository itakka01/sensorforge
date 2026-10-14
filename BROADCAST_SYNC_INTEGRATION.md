# SensorForge Broadcast-Zeitreferenz – Integrationsstufe 1 (2026-10-14)

Basis: `sensorforge_2026-10-14_11-35-03.zip` (vom Nutzer als verbindlich benannt).

## Zweck und Grenzen

Eigenständige **Messstufe**, keine produktive Synchronisierung. Kein ESPNowMeshClock-/GPL-Code übernommen. Ausschließlich vorhandenes SensorForge-ESP-NOW-Protokoll mit bestehenden HMAC-Verifizierungsfunktionen erweitert (Typ `BroadcastSample=12`). Der Coordinator sendet bei eingeschaltetem Versuch etwa alle 100 ms eine signierte Probe mit monotoner `esp_timer_get_time()`-Zeit **vor** dem `esp_now_send()`-Aufruf. Keine Hardware-TX-Zeitmarke. Nodes erfassen `esp_timer_get_time()` am Eintritt des ESP-NOW-Callbacks. Zusätzlich wird `esp_wifi_get_tsf_time(WIFI_IF_STA)` erst beim Verarbeiten in `service()` gelesen: dieser TSF-Wert ist **kein RX-Zeitstempel** und nur Zusatzdiagnostik.

## Aktivierung und CSV

Die Funktion ist standardmäßig **aus** und nur vom aktiven Coordinator aktivierbar. Unter der bestehenden Cluster-Webseite im Diagnosebereich: `ESP-NOW Broadcast-Experiment`, Start/Stopp. HTTP: `POST /cluster_espnow_broadcast_control?enabled=1` bzw. `enabled=0`. Der bestehende Authentifizierungs- und CSRF-Middleware-Gate wird nicht umgangen. Der Status lässt sich am JSON `{"enabled":true|false}` sehen. Lokale CSV-Datei jeder Node: `GET /cluster_espnow_broadcast_csv` (bestehender Web-Zugriffsschutz). Bis zu 256 Messungen im Ringpuffer; älteste Werte werden überschrieben. Keine SD-Schreibzugriffe. Zur einfachen manuellen Messung 20–25 s laufen lassen und CSV je Node herunterladen. Bezeichnungen sind bewusst explizit (TX-Call / RX-Callback / TSF-Service).

**Wichtig:** Ein ausgeschalteter Coordinator sendet keine zusätzlichen BroadcastSamples. Nodes empfangen aber stets die authentifizierten Frames, ohne Capture-Quelle zu ändern. Wiederanlauf/Wechsel von Cluster/ESP-NOW-Radio setzt Ringdaten zurück. Coordinator-Stop und Capture-Pause verhindern weitere Sendungen; im laufenden Benchmark ist der Versuch deaktiviert. Die bisherigen Probe-/Reply-/Final-/Beacon-/Benchmark-Protokolle und die produktive `precisionUtcNow`-Auswahl bleiben erhalten. Kein neuer Node-spezifischer Probe-Dialog.

## Was die Daten nicht beweisen

Coordinator-TX-Aufruf und Node-RX-Callback enthalten variable Softwarelatenz. Eine lineare Regression von `coordinator_send_call_us` gegen `node_rx_callback_us` kann Drift und beobachtete Streuung darstellen, aber eine konstante relative Verzögerung zwischen Nodes nicht unabhängig identifizieren. Für eine belastbare 250-µs-Aussage ist ein gemeinsames extern validiertes optisches/elektrisches Ereignis erforderlich; echte Bildbelichtungszeit ist gesondert zu erfassen. TSF-Synchronisationsverhalten hängt unter anderem von WLAN-Infrastruktur und Treiber ab. Etwa 100 Nodes: reine Broadcast-TX-Frequenz wächst nicht mit Node-Anzahl; Empfangs- und Netzlast sowie Sicherheits- und Verlustverhalten müssen trotzdem getestet werden.

## Offene Arbeit für Integrationsstufe 2

- Getrennte, numerisch stabile affine Clock-Modelle je Node mit robuster Ausreißerbehandlung, kontrollierter Gültigkeitsdauer und Holdover entwickeln (nicht hier als produktiv ausgegeben).
- Daten offline gruppiert nach `epoch`, `coordinator_boot` und Sequenz zusammenführen; Drift-/Residualplots und unabhängige Testereignisse vergleichen.
- Auf realen Boards mit Arduino IDE / Arduino-ESP32 **3.3.12** voll kompilieren und auf allen Geräten testen. Vollständige Kompilierung ist **NICHT geprüft**.
- Erst bei belegter Genauigkeit und Stabilität alte Vier-Zeitstempel-Algorithmen und überholte Diagnosefelder gezielt entfernen. Capture bleibt bis dahin auf dem bisherigen Pfad.

## Lizenz

Keine GPL-Bibliothek integriert. Die gewählte Ergänzung ist als projektspezifischer SensorForge-Code implementiert. Abhängigkeiten und die übrige kommerzielle Lizenzlage des Gesamtprojekts sind dadurch noch nicht vollständig auditiert.
