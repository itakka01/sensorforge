# SensorForge – Wiedereinstieg und technischer Übergabestand

**Stand:** 2026-10-10 · **Firmware:** v87-beta105 · **Basis:** beta104 + beta105-Delta; beta103-Handover historisch

## 1. Zweck und wichtigste Entscheidung

SensorForge wird als verteilter Multi-Kamera-Cluster auf ESP32-S3 weiterentwickelt. Ein Coordinator verteilt signierte Einzelaufträge an auswählbare Nodes. Kernziel: synchronisierte Einzelbilder, später Sequenzen, Video und generisch UTC-terminierte Aktionen (z. B. GPIO). Die Synchronisation hat **eine einzige Cluster-UTC-Zeitbasis**; aktuelle WLAN-Synchronisation und künftig präzisere IR-Synchronisation sind nur unterschiedliche Quellen zur Verbesserung **derselben** Zeitbasis. Capture-Befehle werden **im Voraus** verschickt, nicht zum Soll-Zeitpunkt per Netz ausgelöst. Ziel ist der reale **Belichtungsbeginn**; derzeit ist jedoch nur der Software-Auslösezeitpunkt messbar. Niemals beide gleichsetzen.

## 2. Geräte und reale Beobachtungen

- `devxiao2` – Coordinator, zuletzt unter `http://192.168.0.206/`, **absichtlich ohne SD-Karte**; optionaler SD-loser Betrieb seit beta88.
- `devxiao1` – Node, zuletzt unter `http://192.168.0.3/`, **SD vorhanden**.
- Adressen sind historische Testwerte, keine festen Geräteidentitäten. Für Aufträge Node-ID/Boot-ID/Job-Sequenz verwenden.
- Vergangene Messung: 15/15 signierte Verbindungs-Probes erfolgreich, 0 Retry, HTTP-Fehler 0 (damaliger beta88-Stand); damit **kein** Nachweis aller späteren Änderungen.
- Zentraler SD-Wipe wurde einmal als erfolgreich gemeldet; korrekte frische Kapazitätsmessung und Wipe während einer aktiven Aufnahme wurden danach nicht abschließend nachgewiesen.
- Compilerfehler beta98 (isoliertes `"` in webconfig.cpp), beta102 (falsches `Content-Disposition`-Quoting) sind in beta99 bzw. beta103 behoben worden. Die `SD.h`-Mehrfachbibliothek-Ausgabe der Arduino-IDE ist lediglich informativ.

## 3. Wo liegt was?

- `CHANGELOG.md` – versionsbezogene Historie und Fehlerkorrekturen.
- `CLUSTER_ARCHITECTURE.md` – Protokolle, Rollen und Entscheidungen (lange technische Historie).
- `WIEDEREINSTIEG_SENSORFORGE.md` – **diese** schnelle und verbindliche Übergabe.
- `cluster.cpp`, `cluster_jobs.*` – Cluster-Runtime, signierte Befehle, Jobzustände, Statusmeldungen.
- `cluster_capture.h/.cpp` – zeitgesteuerter Capture-/PSRAM-Medienpfad (ab beta93, erweitert beta102).
- `webconfig_cluster.cpp` – Coordinator-Aktionen, Capture-Countdown, Fertigstellungsbericht und Medienlinks.
- `webconfig.cpp` – Node-Weboberfläche, Betriebsmodus-Banner, JPEG-Download `/capture_media`.
- `sensorforge_version.h` – Versionskennung; bleibt bei diesem reinen Dokumentationsnachtrag unverändert.

## 4. Funktionsstand und Grenzen

| Bereich | Stand beta103 | Was noch nachzuweisen/zu bauen ist |
|---|---|---|
| Coordinator / Nodes | Discovery, Rollen, signierte Unicast-Aufträge, ACK-/Jobstatus vorhanden | Langzeit-/Lasttest, Neustart-/Ausfallverhalten |
| Sammelaktionen | Probe, Sync, Reboot, Shutdown, Drone-Umschaltung, SD-Wipe in Node-Auswahl | Einzelergebnis unter allen Fehlerfällen überprüfen |
| Betriebsmodus | Aus, Recording, Power Shooter, Kombination, Streamer, Drone; Status-Banner mit 5-s-Polling | UI/Moduswechsel am Gerät vollständig verifizieren |
| Drone | passiver Bereitschaftsmodus, ohne autonome Aufnahmen/Motion/Powershooter, Sleep-Sperre | Kamera-Owner/Priorität gegenüber Stream/Vorschau |
| Zeit-Capture | Signierter UTC-Einzelfotojob, Standard „in 10 s“, optional fester UTC-Termin | Browser-Zeitbasis statt Coordinator-Zeit für relative Planung; tatsächlicher Belichtungsbeginn |
| Countdown | rot pulsierend bis Solltermin, längerer grüner Sollzeitpunkt-Impuls | Grün ist **keine** Node-Erfolgsmeldung |
| Fertigstellungsbericht | Auftragsbezogene Node-Zustände; UUID, Software-Zeitstempel, Größe, Link nach erfolgreichem Abschluss | Begrenzter RAM-Jobverlauf (16), Browser-Sitzung flüchtig, Fehlerfälle testen |
| Medien | Bis zu drei JPEG-RAM-Slots derzeit, jede Aufnahme eigene UUID, Abruf per Node-HTTP | Speicherkapazität dynamisieren, Download-/Authentifizierungs-End-to-End-Test |
| Sequenz/Video/GPIO | Architekturabsicht, keine fertige Ausführung | Scheduler/Executor ergänzen |
| Firmware-Verteilung | Nicht implementiert | Auf später verschoben |

### PSRAM-Medienregel (ausdrücklich verbindlich)

Sobald ein Node **einen neuen Capture-Job akzeptiert**, wird der bisherige PSRAM-Bildbestand **gelöscht**. Danach besitzt dieser Job den Speicher exklusiv: **kein Überschreiben/Rollover** während dieses Jobs. Beim Erreichen der verfügbaren Slot- oder Bytegrenze unterbleiben alle übrigen Bilder dieses Jobs; bereits aufgenommene bleiben erhalten. Nodes dürfen unterschiedliche Speicherkapazitäten besitzen. Der **aktuelle Executor ist aber nur Einzelfoto**, keine Mehrbild-Sequenz. Ein neues akzeptiertes Kommando oder Neustart macht bisherige Links ungültig. Das Löschen vor neuem Job ist **destruktiv** und muss für Wiederholungs-/Fehlerfälle überprüft werden.

### Medienidentität, Zeit und Abruf

- **Capture-Job-ID** identifiziert den gemeinsamen Aufnahmeauftrag; **Media-UUID** identifiziert **jedes** einzelne JPEG (später auch Video). Nicht vermischen.
- Metadaten: Zielnode, Auftragsbezug, geplanter UTC-Zeitpunkt, gemessener **Software-Dispatchzeitpunkt**, ggf. Zeitunsicherheit, JPEG-Größe, Aufnahmestatus und Abrufort.
- Node-URL-Schema: `http://<node-ip>/capture_media?uuid=<media-uuid>`. Die UUID ist **keine Autorisierung**; bestehende Web-Authentifizierung muss gültig sein. Netz-/IP-Änderungen sowie PSRAM-Verlust machen Links ungültig.
- Der Coordinator empfängt authentifizierte Status-/Metadaten, **nicht das JPEG automatisch**. Späterer Organisator/Abrufdienst kann den Download planen. Bisher keine dauerhafte Persistenz des Berichts.
- **Erfolg des Capture-Auftrags** bedeutet erfolgreiches JPEG im Node-RAM laut bestätigter Node-Meldung, **nicht** nachgewiesene Synchronizität des CMOS-Sensors, SD-Persistenz oder erfolgreiche Übertragung.

## 5. Präzisions- und Scheduler-Architektur

- Jede Aktion erhält unabhängig vom Typ einen Sollzeitpunkt in **UTC**. Die Berechnung „in X Sekunden/Minuten“ setzt derzeit an der Browser-Uhr an (beta94); **Verbesserung:** Coordinator-Clusterzeit als Autorität für die Ableitung des Termins.
- Alle Nodes erhalten denselben Sollzeitpunkt **vorher** per signiertem Job und lösen dann autonom anhand ihrer jeweils möglichst gut synchronisierten **einzigen Clusterzeit** aus.
- Für Einzelfoto: `PHOTO`. Später `PHOTO_SEQUENCE`, `VIDEO_START` und `GPIO_SET` als unterschiedliche Executor-Typen auf **derselben** Scheduler-/UTC-Abstraktion.
- Hohe Software-Priorität ersetzt keine sensorinterne Belichtungssteuerung: `esp_camera_fb_get()`-Timing, Frame-Buffering, Sensor-VSYNC und Belichtungsstart sind zu vermessen. Kein Mikrosekunden-Genauigkeitsversprechen.
- Vorhandene Kamera-Konkurrenz durch Recorder, Streamer, Vorschau bleibt als offene Prioritätsintegration; niemals parallele ungeschützte Kameratreiber-Zugriffe einbauen.

### IR-Sync-Dokumente des Projekts

User-Referenzen: `00_SYNC_SENDER_BESCHREIBUNG.txt`, `00_SYNC_CAMS_BESCHREIBUNG(2).txt`. Angedacht sind 10-µs-IR-Pulse bei 10 Hz und Empfänger BPW34/MCP6022/LM393P. **Es wird keine zweite Capture-Zeitbasis eingeführt.** Die spätere IR-Messung verbessert die bereits verwendete gemeinsame Uhr. Der spätere Kurz-Wiedereinstieg nennt für Sender/Empfänger GPIO4; die ausführliche ältere Senderzeichnung an einzelnen Stellen GPIO2: **vor Hardwareanschluss Pinbelegung gegen reale Boardkonfiguration prüfen** (GPIO4 kann anderweitig belegt sein). Diese IR-Hardware ist noch nicht als fertig oder getestet anzunehmen.

## 6. Installations-/Build-Prozedur

1. **Vollständigen Basis-Source und sämtliche nachfolgenden Deltas in Versionsfolge** herstellen. Ein Delta-ZIP allein ist **kein vollständiger Sketch**. Bestehendes Projekt mit beta103 als Ausgangspunkt verwenden; keine Quellstände von unterschiedlichen Betas mischen.
2. Dieses Paket enthält **nur** die drei Dokumentationsdateien, keine Änderung an Firmware/Web/Protokoll/Version.
3. Vor Flashen **Arduino-ESP32 / XIAO ESP32-S3** mit den real verwendeten Einstellungen kompilieren; die vorliegenden Syntax-/ZIP-Checks sind kein Ersatz für den C++-Build.
4. Für Protokoll-/Nodeänderungen Coordinator und Node kompatibel aktualisieren. Für rein Coordinator-UI-Änderungen kann ein einseitiges Update genügen (nur nach expliziter Versions-/Protokollprüfung).
5. Nach Flashen: Versionskennung, Rolle, Boot-ID, SD-Modus, Heap, Cluster-Sichtbarkeit prüfen; dann Auftrags- und Capture-Test.

## 7. Konkreter nächster Testplan (zuerst)

1. **Build:** beta103 vollständig für ESP32-S3 kompilieren; reale Compilerdiagnosen sammeln.
2. **Regression:** Beide Geräte starten; devxiao2 ohne SD bleibt verfügbar und löst keine fortlaufenden SD-Recovery-Schleifen aus. Devxiao1 mit SD wird erkannt.
3. **Drone:** Auf devxiao1 via Betriebsmodus setzen, lokales Status-Banner und Coordinator-Zustand prüfen; keine autonome Aufnahme durch Motion/Power Shooter, kein unbeabsichtigter Sleep; zurückschalten verifiziert Normalmodus.
4. **Capture Single:** Node devxiao1 auswählen; Auftrag „in 10 Sekunden“ (genügend Vorlauf) senden; ACK, Countdown, roten/grünen UI-Impuls, signierte JPEG-Abschlussmeldung, UUID und Software-Zeitstempel kontrollieren.
5. **Download:** Link am Node öffnen **mit gültiger Web-Authentifizierung**, `image/jpeg`/Bildinhalt/Dateiname prüfen, währenddessen Stabilität/Heap beobachten.
6. **No rollover:** Zweiten Job ausführen; altes UUID-Objekt darf nicht mehr als vorheriges JPEG abrufbar sein. **Nicht** als Test einer noch nicht implementierten Bildsequenz interpretieren.
7. **Fehlerfälle:** Node offline, zu wenig PSRAM, Kamera durch Stream/Recorder belegt, Capture zu spät, unzureichend synchronisierte Uhr; UI darf nie „HTTP angenommen“ mit „JPEG fertig“ gleichsetzen.
8. **Abschluss:** Tatsächlichen Hardwaretest samt Logs dokumentieren; erst danach Präzisions-/Prioritäts- und Sequenzfunktionen erweitern.

## 8. Offene Risiken / technische Schulden (Priorität)

**P0 – Vor weiterer komplexer Funktionalität:** vollständiger Compile beider Builds, grundlegender JPEG-Download-/UUID-/No-Rollover-Hardwaretest; SD-Zustand nach Wipe bei devxiao1 überprüfen.

**P1 – Für sichere synchronisierte Aufnahmen:** Kamerabesitz/Kamera-Präemption, Timing-/Versatzmessung einschließlich Sensor-VSYNC, zuverlässige Coordinator-Zeit statt Browser-Uhr für relative Jobtermine, Abbruch/Timeout und Task-Priorisierung.

**P2 – Ausbau:** Sequenzexecutor mit variabler PSRAM-Kapazität und eindeutiger UUID pro Frame; ein Ergebnis **pro Medium** im Bericht, eventuelle mehrteilige Signalmeldung statt nur letzter Job-Metadaten; später Video-/GPIO-Terminierung, Medienabruf-/Transfer-Orchestrierung, Berichtspersistenz, Firmware-Pool.

**Wichtig:** Der derzeitige Jobstatus kann nur begrenzt viele Einträge halten, die Fotodaten sind flüchtig. Es gibt **keine bestätigte End-to-End-Hardware-Abnahme von beta103**; keine stillschweigende Produktivfreigabe.

## 9. Für den nächsten Chat

Neuen Chat mit diesem Paket beginnen; möglichst den **vollen Sketch auf beta103** plus die beiden Original-IR-Sync-Beschreibungen beilegen, damit nicht nur die Historie, sondern der reale Quellcode vorliegt. Als Kurzprompt:

> Wir arbeiten an SensorForge v87-beta105 weiter. Bitte zuerst `WIEDEREINSTIEG_SENSORFORGE.md`, `CLUSTER_ARCHITECTURE.md` und `CHANGELOG.md` lesen. Zwei XIAO ESP32-S3: devxiao2 Coordinator ohne SD, devxiao1 Node mit SD. Signierter Cluster, passiver Drone Mode, UTC-Einzelfotojob, JPEG im PSRAM mit UUID und direktem Node-Link. Ein neuer akzeptierter Capture-Job leert den Bildspeicher, innerhalb eines Jobs kein Rollover. Bevor neue Funktionen ergänzt werden, bestehende Hardwaretests und Risiken beachten. Firmwareänderungen bitte nur als Delta-ZIP geänderter Dateien mit Dokumentationsnachtrag.


### Update beta104: Anzeige der Capture-Zeit
- Der Coordinator-Webclient liest für Zeitplanung die gültige `cluster_time.utc_us` aus `/cluster_status` statt `Date.now()` vom Computer. Gilt für relative Zeiten und die Prüfung fixer UTC-Zeiten. Bei fehlender/ungültiger Clusterzeit wird kein Auftrag verschickt.
- Der Client verankert diese UTC-Zeit anhand der Mitte der HTTP-Laufzeit an `performance.now()`; der Countdown läuft monoton ohne Abhängigkeit von späteren PC-Uhränderungen. HTTP-Transport und ungewisse Abfragephase begrenzen die Anzeigepräzision; dies ist **nur eine Näherung der Coordinator-Clusterzeit**, kein gemessener Auslöse- oder Belichtungszeitpunkt.
- Der Countdown startet bei der ersten erfolgreichen HTTP-Auftragsannahme; Grün markiert weiter nur den Soll-Termin. Der signierte Medienabschlussbericht ist davon unabhängig.
- Kein Wechsel der Capture-Protokollfelder; Node-Firmware und Kamera werden nicht geändert. Auf Coordinator beta104 installieren; Node kann zunächst beta103 bleiben.
- 5 eingebettete JS-Blöcke mit Node.js `--check` geprüft, ZIP-Integrität geprüft. Kein vollständiger Arduino/ESP32-Compile und kein Hardwaretest.


## Nachtrag v87-beta105 – Unveränderliche Capture-Zeiten (2026-10-10)

**Aktueller Stand: v87-beta105** (Delta auf beta104). Annahme eines UTC-Capture-Auftrags: Zeit einmalig aus lokaler Cluster-UTC nach monotonic ESP-Timer umrechnen; danach für diesen Job **keine** Uhr-/Sync-Nachregelung. NTP/SNTP und Cluster-Sync bleiben global unabhängig aktiv, verschieben aber den schon geplanten Shot nicht. Zeitstempel aus eingefrorener Abbildung, kein Belichtungsbeginn. Standard 10 Sekunden; gültiger Vorausbereich 5 Sekunden bis 30 Minuten im Browser, 2 Sekunden bis 30 Minuten Coordinator, 1,5 Sekunden bis 30 Minuten Node (Netzwerklaufzeit berücksichtigt). Job-Timeout 30 Minuten 30 Sekunden. Für einen künftigen gemeinsamen Capture von GPIO/Video/Sequenzen monotone Deadline wiederverwenden.

**Empfohlener Test:** Beide Geräte aktualisieren; 10-Sekunden- und 3-Minuten-Capture testen; während Wartephase kontrolliert NTP-/Cluster-Zeitänderungen beobachten und überprüfen, dass die geplante monotone Triggerzeit nicht springt. Report/JPEG/UUID prüfen. Aufnahme-Zeitgleichheit und Shutter-Genauigkeit sind damit noch nicht physisch nachgewiesen. Vollständiger ESP32-Build offen.
