## Audit-Paket 5p (2026-10-11) – SPI-Remount-Admission
- Der Sync-API-SPI-Remount (exklusiver Clock-Wechsel und Diagnose) reserviert jetzt die gemeinsame atomare `SdAdmissionController::SpiRemount`-Lease; Cluster-Wipe, Secure Erase, Formatierung und Benchmark können während der Lease keine eigene Wartungs-Lease erhalten.
- Exklusivmodus-Aktivierung/-Deaktivierung und Lease-Ablauf prüfen auch die aktive SD-Admission. Diagnose reserviert vor dem ersten Dateizugriff und hält die Lease bis nach dem Restore. Die bestehenden kooperativen SD-/Recording-Sperren bleiben erhalten.
- Restgefahr: Bereits laufende Recorder-/SD-I/O nutzt noch keine gemeinsame Lease; nicht vollständig atomare globale Sperrflags. ESP32-Compile und Hardwarevalidierung ausstehend.

## Audit-Paket 5l – vorbereiteter SD-Besitzmechanismus (2026-10-11)
- Neu: `sd_admission_controller.h` und `tests/sd_admission_controller_test.cpp` als isolierter Host-Prototyp, ohne Abhängigkeit zu Arduino.
- Bewusst **keine produktive Integration**: Die zentralen Gates `g_storageLocked` und `g_recordingStartBlocked` sind weiterhin ungeschützt und benötigen vollständige Migration; die neue Reservierung verhindert keine bereits laufende SD-I/O.
- Host-Kompilierung und Tests sind separat nachgewiesen; vollständiger ESP32-Compile und Hardwaretests stehen aus.

## Audit-Paket 5j – Sync-API-SD-Eintrittspunkte (2026-10-11)

- **P1:** Die Sync-API blockiert jetzt `handleTestSdSpi`, `handleConfigPost`, `handleTransportControl` und `handleConfigStoragePost` auch bei `g_recordingStartBlocked`. So kann eine allein durch eine Cluster-Wipe-Reservierung aktive Recording-Startsperre diese SD-/Konfigurationsaktionen abweisen, bevor der eigentliche Storage-Lock gesetzt wird.
- **Grenzen:** Die globalen Flags bleiben kooperativ und nicht atomar als gemeinsamer Besitzmechanismus. Kein Ersatz für eine zentrale SD-Arbitrierung. Keine Hardwaretests oder vollständiger ESP32-Compile; Prüfung der vier Guard-Stellen und ZIP-Integrität.

## Audit-Paket 5c – Power Shooter und SD-Wipe-Reservierung (2026-10-11)

- **Behobener P1-Konflikt:** Vier Power-Shooter-Einstiegspfade beachten ab jetzt zusätzlich `g_recordingStartBlocked`: Capture, JPEG-Schreiben, Sparse-MKV-Schreiben und PSRAM-Flush. Ein bereits angenommener Cluster-Wipe blockiert so auch neue Shooter-Medienzugriffe, bevor `g_storageLocked` für den eigentlichen Wipe gesetzt ist.
- Weiter offen: vollständige SD-Client-Inventur, Power-Shooter-Zustands-/Sleep-Regression, ESP32-S3-Compile, Tests mit paralleler Last und Hardware. **Keine stabile Freigabe**.

## Audit-Paket 5a – Destruktor-Lifecycle / Recording (2026-10-11)

- **Hoch:** `RecordingWriteBufferedFile` kann beim SD-Benchmark als lokales Objekt existieren. Nach einem zeitlich begrenzten `closeChecked()`-Fehler darf der Stack-Destruktor nicht zurückkehren, solange der Drain-Task noch auf `this` zugreift. Der Destruktor wartet jetzt in diesem Sonderfall ohne Timeout auf die Fertigmeldung, beendet den Worker und versucht das Schließen erneut. Dies verhindert den Use-after-free-Pfad beim Verlassen des Scopes.
- **Bewusster Trade-off:** Ein auf Dauer blockierter SD-Treiber kann den betroffenen Task dadurch auf Dauer blockieren. Das ist kein allgemeiner Hang-Fix. Für Produktionsfreigabe bleiben Watchdog-/Recovery-Konzept und Hardware-Störtests erforderlich; keine unkontrollierte Task-Terminierung während `file_.write()`.
- Keine Änderungen an Recordingformat, Power Shooter, Drone, Zeitbasis oder Clusterprotokoll. Statische Prüfung; kein vollständiger ESP32-Build oder Hardwaretest.

## Audit-Korrekturpaket 4b – Recording-Task-Lifecycle (2026-10-11)

- `recording_write_buffer.cpp/.h`: Die Drain-Task-Beendigung wird jetzt über eine binäre Fertigmeldung synchronisiert. Der Worker meldet das Ende **nach** seinen letzten Storage-/Mutex-Zugriffen, parkt sich; anschließend löscht der schließende Task den Worker explizit mittels `vTaskDelete(handle)` und gibt erst danach Speicher und Semaphore frei. `taskHandle_` wird nicht mehr vorzeitig aus dem Worker gelöscht.
- Timeout bleibt ein harter Fehler: kein Löschen des Workers und keine Freigabe seiner Ressourcen während möglicher SD-Operationen. Kein neuer Capture-Trigger, keine Änderung an UTC, Drone Mode oder Recordingformat.
- **Restgefahr:** Wenn ein SD-Treiber dauerhaft blockiert und der besitzende C++-Objektträger trotz fehlgeschlagenem `closeChecked()` zerstört wird, braucht es eine höhere Lifecycle-Sperre. Kein vollständiger ESP32-Build oder Hardwaretest; statische Prüfung und ZIP-Integritätsprüfung. SD-Wipe-/Power-Shooter-/PSRAM-Lasttests sind noch nicht abgeschlossen.


## Audit-Korrekturpaket 3 – Capture-Scheduler / Kamerapriorität (2026-10-10)

- `clusterLoop()` prüft bereits angenommene UTC-Capture-Jobs jetzt **vor** dem Empfang und der kryptografischen Verarbeitung von UDP-Paketen, Peer-Ablaufprüfungen, Heartbeats und Zeitabgleich. Die **bereits eingefrorene monotone Deadline** wird dabei nicht neu berechnet.
- Bei bereits vorgewärmter/armierter Kamera werden in den letzten **50 ms** vor der Deadline nichtkritische Cluster-Aufgaben bis nach dem Auslösen verschoben. Der ESP32-Loop bleibt kooperativ (`delay(1)` in `sensorforge.ino`), kein Busy-Spin und kein zusätzlicher Task oder ungeschützter Kamerazugriff.
- Der ehemalige zweite Scheduler-Aufruf am Ende von `clusterLoop()` entfällt, sodass ein Job pro Loop-Durchlauf nur an einer Stelle geprüft wird. Neu empfangene Jobs werden ab dem nächsten Durchlauf behandelt. Aufnahme-/SD-/Recorder-/Power-Shooter-Code, Signaturprotokolle und PSRAM-Jobregeln bleiben unverändert. Dreifacher `esp_timer.h`-Include bereinigt.
- **Grenze:** Ein Aufruf von `esp_camera_fb_get()` misst *nicht* den realen Sensor-Belichtungsbeginn. Auch hoher Scheduling-Vorrang bietet keine harte Echtzeitgarantie bei Interrupts, WiFi-Tasks, Kamera-Treiberwartezeiten oder anderen Threads. Die Kamera wird erst in den letzten 2 Sekunden vorbereitet; echtes Kamera-Owner-/Ressourcen-Reservieren und passende Task-Prioritäten sind **noch offen**.
- **Verifikation:** Quelltext- und strukturelle Assertions; kein vollständiger Arduino-/ESP32-Compile, keine Messung auf beiden Geräten. Hardwaretests: 10/30-Minuten-Termine, UDP-Last unmittelbar vor Auslösung, parallel aktive Vorschau/Recorder/Streamer, Auslöseoffset und Result-Retry-Verhalten.

## Audit-Korrekturpaket 2 auf Basis v87-beta105 (2026-10-10)

- Paket 1 ist enthalten; Delta 2 wird **auf den Originalstand plus Paket 1** angewendet, nicht auf eine veraltete beta105-Datei. Firmware-Version unverändert.
- Node sendet **nur terminale** Capture-Ergebnisse (`SFJCR1`, Status 2/3) maximal 10-mal, anfänglich sofort, danach in 2-s-Abständen, sofern keine signierte `SFJCA1`-Bestätigung eintrifft. Retries führen **nie** `SFJC1` oder einen Kameratrigger erneut aus.
- Coordinator sendet eine signierte Empfangsbestätigung für einen erfolgreichen oder fehlgeschlagenen Capture-Job nur dann, wenn das gespeicherte terminale Ergebnis zu Job-/Medienmetadaten bzw. Fehlercode passt. Gleiche spätere Meldungen erzeugen nur ein erneutes Receipt, keine Metadatenänderung.
- Receipt-Verifikation am Node: HMAC-Transport plus aktuelle Coordinator-ID/IP, Boot, Epoch, Job-Sequenz und Status. RAM-only, Stop/Epoch-Wechsel invalidieren Retries. Zusatzdiagnostik unter `capture_diagnostics`: `result_retry_pending`, `result_retry_attempts`, `result_retransmissions`, `result_receipts`, `result_retry_exhausted`.
- **Grenzen:** Bei 10 verlorenen Meldungen/ACKs bleibt der Auftrag ggf. `TimedOut`; kein Exactly-once-Netzversprechen. Bei einem später neu akzeptierten Capture-Job wird der alte Retry verworfen (ein RAM-Retry-Slot pro Node, passend zur PSRAM-Job-Lebensdauer). Ablehnung bereits bei der Annahme wird weiterhin nur einmal gesendet. Rollenwechsel/Neustart können Ergebnisse verlieren. `SFJCA1` verlangt die aktualisierte Coordinator-Firmware; gemischte Versionen funktionieren für Captures weiter, aber bestätigen Retries nicht.
- **Prüfstatus:** statische Code- und Protokollprüfung; kein kompletter Arduino-ESP32-Build und keine reale Verlustsimulation auf zwei Boards. Vor Nutzung: OTA beider Boards, Test mit 0/1/mehreren absichtlich verlorenen Ergebnis-/Receipt-Paketen, Epoch-/Node-Neustart und 30-Minuten-Deadline-Grenze.

## Audit-Korrekturpaket 1 auf Basis v87-beta105 (2026-10-10)

- **Delta, Versionskennung weiterhin beta105:** `cluster.cpp`, `cluster_jobs.cpp` und die drei Pflichtdokumente. Nicht als vollständigen Sketch installieren.
- Expliziter Cluster-Runtime-Stopp storniert einen ausstehenden Capture-Auftrag und entfernt dessen Armierung. Ein Cluster-Neustart kann dadurch keinen alten lokalen Fototermin ausführen. Bereits erzeugte PSRAM-Bilder bleiben bis zur nächsten **akzeptierten** Capture-Aufgabe abrufbar.
- Signierte Capture-Abschlussmeldung: Nur ein offener, noch nicht abgelaufener Job darf Medienfelder übernehmen; abgeschlossenes Ergebnis ist unveränderlich. Replay- und Signaturprüfung sowie 30-Minuten-Deadline-Vertrag bleiben erhalten.
- **Nicht nachgewiesen:** kompletter Arduino/ESP32-S3-Build, reale Cluster-Neustarttests, Kamerahardware und verlorene UDP-Meldungen. Nächstes Paket: zuverlässige Completion-Kommunikation und Fehler-/Timeoutpfade.

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

### Audit-Paket 4 – Zwischenstand
- Recording-Write-Behind: Startup-Race zwischen `xTaskCreatePinnedToCore()` und dem Setzen von `taskHandle_` durch einen Start-Handshake behoben (`recording_write_buffer.cpp`).
- Nicht als geprüft markieren: Task-Ende-Freigabe, SD-Wipe unter aktiver Recorder-Last, Power-Shooter-Abläufe und PSRAM-Limits auf Hardware. Weiterer Audit und ESP32-Compile erforderlich.
- Verbindliche Basis: Original v87-beta105 **plus kumulative Pakete 1–3 und Paket 4**; vorhandene Funktionen bleiben erhalten.


### Audit-Paket 5b – SD-Wartungsreservierung (2026-10-11)
- `web_sd_maintenance.cpp`: Ein angenommener Cluster-Wipe reserviert den SD-Wartungspfad nun auch gegenüber lokalem Wipe/Format, Secure-Erase und SD-Benchmark. Zuvor sperrte `g_recordingStartBlocked` lediglich neue Aufnahmen, aber nicht die lokalen Wartungsstarts.
- Die Cluster-Ausführung räumt `g_clusterWipe.active` vor dem eigenen `performSdMaintenance(SD_MAINT_WIPE)` ab; dadurch blockiert die zusätzliche Admission-Sperre nicht den reservierten Auftrag.
- Statische Prüfung durchgeführt; ein vollständiger ESP32-Build und Hardwaretests sind weiterhin ausständig. Allgemeine Parallelzugriffe, Recovery-Operationen, Power-Shooter-Zustände und PSRAM-Lastverhalten bleiben Prüfgegenstand.


### Audit-Paket 5d – aktueller Stabilisierungsstand (2026-10-11)
- Ausgangsbasis unverändert v87-beta105 + kumuliertes Paket 5c.
- `web_sd_maintenance.cpp`: SD-Benchmark bei gesetzter Wartungssperre **vor** SD-Metadatenzugriff mit HTTP 409 abweisen; Busy-Status berücksichtigt auch `g_storageLocked`.
- Noch offen: vollständiger ESP32-S3-Compile, SD-Parallellast/Power-Shooter-Zustandswechsel, PSRAM-Grenzwerte und echte Hardwaretests. Keine Stable-Freigabe.

### Stabilisierung Audit 5e (2026-10-11)
- `sensorforge.ino`: interner Sparse-MKV-Pufferflush hat jetzt selbst einen SD-/Recording-Startsperren-Guard. Bei Ablehnung bleiben gepufferte Bilder im RAM.
- **Offen:** vollständiger ESP32-Compile; Task-/PSRAM-Stresstests, konkurrierende SD-Leser/Schreiber, Power-Shooter-Moduswechsel, Drone-/API-Sicherheitsprüfung. Keine Stable-Freigabe.

### Fortsetzung Audit Paket 5f (2026-10-11)
- **Korrigiert (P1):** Cluster-Wipe-Reservierung wird während Recorder-Finalisierung und Ausführung nicht mehr vorzeitig gelöscht. Lokale Wartung wird dadurch nicht zwischen Reservierung und `g_storageLocked` zugelassen.
- **Nicht erledigt:** vollständige Analyse paralleler SD-Nutzer und Blockade-Recovery für SD-Drain-Task; danach Power Shooter/PSRAM sowie Cluster/Drone/API prüfen.
- **Validierung:** statische Quelltext- und Archivprüfung; weder vollständiger ESP32-S3-Compile noch Hardwaretest.


### Audit-Paket 5g (2026-10-11)
- `webconfig.cpp`: Schutz gegen pauschales Löschen der SD-Sperre durch den Mikrofon-Test bei zwischenzeitlich gesetzter Cluster-Wipe-Reservierung.
- Offen (P1): atomare Koordination sämtlicher SD-Zugriffe; blockierte Drain-I/O; Power Shooter und PSRAM unter Last.
- Noch nicht durchgeführt: vollständiger ESP32-Compile, Integrations- und Hardwaretests.

### Audit-Paket 5h (2026-10-11)
- `web_sd_maintenance.cpp`: Cluster-Wipe wird nicht mehr angenommen, während `g_recordingStartBlocked` bereits durch andere Abläufe gesetzt ist.
- Weiter offen (P1): atomare SD-Ressourcenkoordination über alle Tasks, begrenzte Recovery bei dauerhaft blockierter SD-I/O. Als nächste Prüfgegenstände Power Shooter/PSRAM sowie API/Drone Mode.
- Auslieferung als kumulatives Delta auf Original v87-beta105; noch keine ESP32-Kompilation/Hardwaretests.


### Paket 5i: Sync-API-SD-SPI-Remount gegen Cluster-Wipe-Reservierung abgesichert. Offen: atomare SD-Ressourcenkoordination, blockierende I/O, Power Shooter/PSRAM, Vollbuild und Hardwaretests.

### Paket 5m (2026-10-11): SD-Besitzmechanismus – erste Anbindung
- Cluster-Wipe nutzt jetzt zusätzlich ein atomisches Lease-Ticket; Freigabe erfolgt erst nach dem Wipe. Die Legacy-Sperren bleiben bestehen.
- Offene P1-Aufgaben: alle SD-Clients migrieren, laufende I/O und blockierte Drain-Tasks absichern; Power-Shooter/PSRAM prüfen.
- Kein vollständiger ESP32-Compile oder Hardwaretest; **keine stabile Freigabe**.


### Stabilisierung Paket 5n (2026-10-11)
Secure Erase und Cluster-Wipe besitzen jetzt konkurrierende atomare Leases; der Secure-Erase-Job gibt sein Ticket bei Abschluss und im Start-Rollback frei. Paket 5n setzt Paket 5m voraus. Offen: alle übrigen SD-Clients migrieren, Race-/Recovery-Tests, Power Shooter, PSRAM, ESP32-Compile und Hardwaretests.

## Paket 5o – Zwischenstand
Lokale Formatierung/Wipe und SD-Benchmark wurden in den Lease-Controller eingebunden. Im Konfliktfall werden Operationen abgelehnt; Scoped-Leases verhindern Leaks auch bei vorzeitigem Rücksprung. Weiter offen: SPI-Remount, übrige SD-Nutzer, Recovery und vollständiger ESP32-Build plus Hardwaretests.

### Audit-Paket 5q – Recorder-Start und SD-Lease (2026-10-11)
- `recorder.cpp`: `recorderStart()` verweigert neue Aufnahmen auch bei aktiver atomarer SD-Wartungslease (`webSdAdmissionController().busy()`). Vorher wurden nur die kooperativen Flags geprüft; die Reihenfolge Lease-vor-Flag ließ ein kurzes Zulassungsfenster.
- Diese Ergänzung ist **Defense-in-Depth**, kein vollständiger Ausschluss laufender Recorder-I/O: der Start prüft die Lease bislang nur einmal vor den Dateisystemzugriffen. Eine atomare Recorder-Besitzerlease bzw. Drain-/Finalize-Quiescence bleibt P1.
- Keine Änderungen an bestehendem Aufnahmeformat, Capture-Timing oder Power Shooter. Nur statische Strukturprüfung; ESP32-Compile und Hardwaretests offen.

### Audit-Paket 5r – atomarer Recorder-Dateistart (2026-10-11)
- `recorderStart()` reserviert jetzt vor jeglichem Dateisystemzugriff eine kurzlebige `RecorderStart`-Lease und hält sie bis zum Ende des Startpfads, auch bei Fehlern. Dadurch kann keine bereits migrierte Wartung/Remount-Operation zwischen Admission-Prüfung und Dateiöffnung reservieren.
- Die bisherigen Flags werden nach Erwerb der Lease erneut geprüft. Bereits laufende Aufnahmen und ihre Schreib-/Finalize-Pfade werden **noch nicht** durch dieselbe Lease geschützt. Die Änderung ist bewusst auf den Start begrenzt; ein atomarer Schutz der gesamten I/O-Lebensdauer und die Migration weiterer SD-Nutzer bleiben P1.
- Host-C++-Test des Admission-Controllers; vollständiger ESP32-Compile und Hardwaretests offen.

### Stand Audit 5s (2026-10-11)
- P1 weiter offen: gemeinsamer atomarer Übergang vom aktiven Recorder zu exklusiver SD-Wartung einschließlich Drain-Task-Quiescence. Die Start-Lease allein reicht nicht. Eine langfristige exklusive Recorder-Lease wäre mit vorab akzeptierten Cluster-Wipes unvereinbar. Ausarbeitung: `SD_RECORDER_HANDOFF_AUDIT.md`.
- Paket 5s enthält ausschließlich Dokumentation, keine Laufzeitänderung. Firmware-Compile/HW-Tests weiter offen.
