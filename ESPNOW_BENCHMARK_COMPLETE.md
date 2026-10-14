# SensorForge v87-beta105 – integrierter ESP-NOW Benchmark (Quellcode)

Basis: vom Benutzer geliefertes vollständiges Projekt-ZIP `sensorforge_2026-10-13_22-48-18.zip`, einschließlich TX-Zeitstempel-Korrektur.

## Inhalt

Die Weboberfläche **Cluster koordinieren → Verbindungsdiagnose und Einzeltests** bietet neben dem unveränderten, bisherigen Job/ACK-Verbindungstest einen eigenen **ESP-NOW-Benchmark**.

- Ziel: einzelner aktuell sichtbarer Node oder alle bis zu drei aktuell aktiven Nodes, sequenziell.
- Drei wählbare Sendeabstände: 50 ms (Referenz), 10 ms, 5 ms (Lasttest); keine 60-µs-Überlastversuche.
- Drei Phasen pro Node: (1) 100 Coordinator→Node-Frames mit authentifizierten Node-ACKs; (2) 100 Node→Coordinator-Frames mit Coordinator-ACKs; (3) 100 reguläre Vier-Zeitstempel-Probe/Reply/Final-Exchanges.
- Alle neuen Funkrahmen nutzen dieselben HMAC-SHA256-Prüfung, Epoch/Coordinator-Tag, Boot-Zuordnung und Replay-Kontrolle wie der bestehende Zeitdienst. Der zentrale Paket-Codec wurde um vier Nachrichtentypen erweitert.
- Ein exklusiver Benchmark verhindert reguläre Sync-Slots während der Testlaufzeit. Start erst nach 3,5 s Ruhezeit für ablaufende 3-s-Slot-Grants. Pro Testphase erfolgt ein authentifiziertes Steuer-ACK; bei dessen Ausbleiben wird die Phase wiederholt angefordert beziehungsweise der Test beendet.
- Capture-Vorrang: `pauseForCapture()` bricht den Test ab und versucht, die Nodes durch signierte Stop-Steuerframes anzuhalten. Zusätzlich laufen Node-Phasen selbstständig zeitbegrenzt aus. Nach Abschluss wird die normale Slot-Vergabe automatisch wieder aufgenommen.
- Die reguläre Sync-Qualitätsgrenze 250 µs, Drift-Schätzung, gespeicherte Kalibrierung und UTC-Auswahl wurden nicht geändert. Vier-Zeitstempel-Benchmarkprobes werden nicht in die produktive Offset-/Drift-Schätzung übernommen.
- Es entstehen **nur während eines explizit gestarteten Benchmarks** zusätzliche ESP-NOW-Frames.

## URLs

Coordinator:

- `GET /cluster_espnow_benchmark_status` – JSON mit Testphase, Run-ID, Zielboot-IDs, Downlink-ACKs, Uplink-RX und Downlink-ACK-RTT (min/max/mean).
- `GET /cluster_espnow_benchmark_csv` – 100 Zeilen pro Richtung und Node mit sequenzbasierter Empfangsstatistik; erst nach Ende eines Tests.
- `POST /cluster_espnow_benchmark_start?interval_ms=50&boot_id=` – startet den Test. `boot_id` leer: alle aktuellen Nodes; sonst 8-stellige hexadezimale Boot-ID.
- `POST /cluster_espnow_benchmark_abort` – expliziter Abbruch.

Jeder Node:

- `GET /cluster_espnow_benchmark_node_transport` – CSV mit jeweils 100 Downlink-Empfangsmarkierungen, Uplink-ACK-Markierungen und Uplink-ACK-RTT.
- `GET /cluster_espnow_benchmark_exchanges` – CSV mit den bis zu 100 vollständigen Vier-Zeitstempel-Exchanges aus der letzten Benchmarkphase (t1, t2, t3, t4, RTT, geschätzter Offset). Fehlende Exchanges erscheinen nicht als fiktive gültige Messungen.

Beim CSV-Export erfolgt HTTP-Streaming, kein Aufbau einer großen `String`-Nutzlast; die lokale 100er-Rohdatenhaltung befindet sich im RAM und wird beim nächsten Start überschrieben.

## Bedienung: einmal flashen, mehrere Profile testen

1. **Vollständige Quelldateien aus dem Delta-ZIP** in dein oben genanntes aktuelles Projekt übernehmen. Nicht in einen älteren Stand integrieren.
2. Zunächst die Firmware für **Freenove und XIAO** lokal mit der richtigen `board_config.h`-Auswahl kompilieren. **Erst bei erfolgreichen Builds** alle vier Geräte flashen.
3. Coordinator und drei Nodes einschalten, Drone Mode aktivieren. Mindestens 30 s warten, damit alle aktiven Nodes in den Funk-Peer-Tabellen erfasst sind.
4. Auf dem Coordinator `http://192.168.0.122/cluster_coordinate` öffnen → „Verbindungsdiagnose und Einzeltests“ → ESP-NOW-Benchmark.
5. Zuerst **einzelnen Node**, Profil 50 ms, starten. Status bis „beendet“ abwarten; dann JSON, Coordinator-CSV und beide CSVs des Nodes speichern.
6. Nacheinander dieselben Einzeltests für die anderen Nodes durchführen, anschließend **alle Nodes**, 50 ms, dann **alle Nodes**, 10 ms und 5 ms.
7. Für Browserlastvergleich einen 50-ms-Gesamtlauf mit nur Coordinator-Tab, einen weiteren mit vier offenen SensorForge-Tabs durchführen. Je Lauf alle CSV/JSON einzeln abspeichern, da der nächste Test die RAM-Ergebnisse überschreibt.
8. Bei Capture-Anforderungen Benchmark nicht gleichzeitig starten; ein laufender Benchmark wird bei Schutzpause abgebrochen. Die Tests verändern keine Dateien auf den SD-Karten.

## Interpretation

- Coordinator→Node `down_ack=100` bedeutet, dass der Coordinator 100 gültige ACKs erhalten hat; ein verlorener ACK kann als Verlust erscheinen, selbst wenn die ursprüngliche Sendung ankam.
- Node→Coordinator `up_received=100` bedeutet, dass alle 100 sequenzierten Frames am Coordinator authentifiziert ankamen. Separat zeigt die Node-CSV, wie viele Rück-ACKs den Node erreichten.
- `ack_rtt_us` ist **Software-Dispatch bis authentifizierter ACK-Empfang**, nicht ein PHY-Zeitstempel und keine einseitige Funklaufzeit.
- `offset_us` setzt für die Näherung implizit symmetrische Hin- und Rückwegverzögerungen voraus. Die Offset-Streuung sagt allein nichts über die tatsächliche Kamera-Belichtungsabweichung aus.
- Der Benchmark stoppt die normalen Präzisionsslots zeitweise: Die aktive Uhr kann während langer Testläufe daher veralten. Zur Beurteilung des *produktiven* Sync danach neue laufende Präzisionsserien abwarten.
- Die eigentliche physische Synchronität muss anschließend über gemeinsame GPIO-Messung und Kamerabelichtungsbeginn verifiziert werden.

## Tests und Grenzen

Durchgeführt:

- Host-C++-Syntaxprüfung von `cluster_espnow_radio.cpp` mit ESP-IDF/Arduino **Mock-Headern** (`g++ -std=gnu++17 -fsyntax-only`), erfolgreich. Damit werden C++-Syntax- und lokale Typisierungsfehler erkannt, **keine Hardware-API-Kompatibilität garantiert**.
- Kompilierung des echten `cluster_espnow_packets.cpp` unter Host-g++, erfolgreich.
- Roundtrip/Manipulations-Test aller vier neuen HMAC-gerahmten Nachrichtentypen, erfolgreich (mit Test-MAC, nicht produktivem HMAC-Schlüssel).
- JavaScript-Syntaxprüfung des eingebauten Verbindungstest-Skripts mit Node.js, erfolgreich.
- Delta-ZIP gegen das Benutzer-Projekt verglichen.

**Nicht durchgeführt:** vollständiger Arduino-ESP32-Build beider Hardwareprofile und Hardware-/Funk-/Capture-Integrationstest. Die erforderliche Arduino-ESP32-Toolchain ist in dieser Umgebung nicht vorhanden. Dieser Quellstand ist eine integrierte Implementierung, **kein hardwarevalidierter oder produktionsfreigegebener Firmware-Build**.
