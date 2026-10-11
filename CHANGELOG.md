## Audit-Paket 5p (2026-10-11) – SPI-Remount-Admission
- Der Sync-API-SPI-Remount (exklusiver Clock-Wechsel und Diagnose) reserviert jetzt die gemeinsame atomare `SdAdmissionController::SpiRemount`-Lease; Cluster-Wipe, Secure Erase, Formatierung und Benchmark können während der Lease keine eigene Wartungs-Lease erhalten.
- Exklusivmodus-Aktivierung/-Deaktivierung und Lease-Ablauf prüfen auch die aktive SD-Admission. Diagnose reserviert vor dem ersten Dateizugriff und hält die Lease bis nach dem Restore. Die bestehenden kooperativen SD-/Recording-Sperren bleiben erhalten.
- Restgefahr: Bereits laufende Recorder-/SD-I/O nutzt noch keine gemeinsame Lease; nicht vollständig atomare globale Sperrflags. ESP32-Compile und Hardwarevalidierung ausstehend.

## Audit-Paket 5l (2026-10-11) – SD-Admission-Prototyp
- Isolierter atomarer Besitzer-/Lease-Controller und Host-Konkurrenztest hinzugefügt. **Noch nicht an produktive SD-Pfade angeschlossen**; deshalb keine Änderung am Laufzeitverhalten und noch keine Lösung für verteilte Flags.
- Offene Punkte: vollständiges Besitzerinventar, atomare Migration aller Gate-Schreiber, laufende SD-I/O drainen, Recovery und ESP32-/Hardwaretest.

## Audit-Paket 5j – Sync-API-SD-Eintrittspunkte (2026-10-11)

- **P1:** Die Sync-API blockiert jetzt `handleTestSdSpi`, `handleConfigPost`, `handleTransportControl` und `handleConfigStoragePost` auch bei `g_recordingStartBlocked`. So kann eine allein durch eine Cluster-Wipe-Reservierung aktive Recording-Startsperre diese SD-/Konfigurationsaktionen abweisen, bevor der eigentliche Storage-Lock gesetzt wird.
- **Grenzen:** Die globalen Flags bleiben kooperativ und nicht atomar als gemeinsamer Besitzmechanismus. Kein Ersatz für eine zentrale SD-Arbitrierung. Keine Hardwaretests oder vollständiger ESP32-Compile; Prüfung der vier Guard-Stellen und ZIP-Integrität.

## Audit-Paket 5c – Power-Shooter/Cluster-SD-Wipe (2026-10-11)

- **Hoch / behoben:** Nach Annahme eines Cluster-SD-Wipe ist `g_recordingStartBlocked` gesetzt, jedoch prüften vier Power-Shooter-Pfade ausschließlich `g_storageLocked`. Die Kameraaufnahme sowie JPEG-, Sparse-MKV- und PSRAM-Flush-Schreibpfade prüfen nun zusätzlich die Recording-Startsperre. Dadurch werden neue Shooter-SD-Zugriffe während des reservierten Wipe-Fensters verweigert; gepufferte Aufnahmen bleiben zur späteren Flush-Wiederholung im PSRAM.
- Statische Quelltext-/Paketprüfung, **kein vollständiger ESP32-Compile**, keine Hardwaretests. Weitere mögliche SD-Clients und Race Conditions bleiben Gegenstand der Abschlussprüfung.

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

## v87-beta105 – Audit-Korrekturpaket 1 (2026-10-10; Delta, Versionskennung unverändert)

- Cluster-Lifecycle: `clusterNetworkStop()` verwirft einen noch ausstehenden lokalen Capture-Auftrag, löst dessen Kamera-Armierung und setzt Capture-Replay-Marker zurück. Dadurch wird nach explizitem Cluster-Neustart kein alter Auftrag mehr ausgeführt oder als weiterhin pending angezeigt. Bereits erzeugte PSRAM-Medien werden **nicht** gelöscht; deren bisherige Lebensdauer bis zum nächsten akzeptierten Job bleibt erhalten.
- Coordinator-Capture-Result: `SFJCR1` mit erfolgreichem Ergebnis darf Medienfelder nur für `Sent`/`Accepted` vor Deadline und höchstens einmal setzen. Doppelte oder verspätete signierte Ergebnisse können terminale Medienmetadaten nicht mehr überschreiben. Einheitlicher `millis()`-Zeitpunkt für Deadline-Prüfung und Statusübergänge.
- Keine Protokoll-, Signatur-, Cluster-UTC-, Aufnahmepfad-, SD- oder Betriebsmodusänderung. **Kein vollständiger ESP32-Compile oder Hardwaretest**; statische Kontrolle und separater C++-Test des Job-Bookkeepings.
- Offen: best-effort UDP-Abschlussmeldungen können verloren gehen; Lifecycle-Abbruch besitzt keine garantierte Remote-Bestätigung.

## v87-beta105 – Unverrückbare monotone Capture-Deadline (2026-10-10)

- Beim Empfang eines authentifizierten UTC-Capture-Jobs berechnet jeder Node **einmalig** aus seiner aktuell geschätzten Clusterzeit eine lokale monotone Deadline (`esp_timer_get_time()`). Diese wird während des Jobs **nicht mehr neu berechnet**. NTP-/SNTP-Sprünge, neue Cluster-Sync-Samples und Coordinator-Zeitkorrekturen verschieben den anstehenden Shot nicht.
- Kamera-Vorbereitung bei höchstens 2 Sekunden Restzeit und Fälligkeit werden nur mit der eingefrorenen Deadline geprüft. Die UTC-Software-Dispatch-Zeit in JPEG-Metadaten wird aus derselben eingefrorenen Zeitabbildung berechnet; sie wird ausdrücklich **nicht** als tatsächlicher Belichtungsbeginn ausgewiesen.
- Relative und absolute Termine jetzt maximal 30 Minuten voraus (zuvor 120 Sekunden). Coordinator-Job-Timeout 30 Minuten + 30 Sekunden; `clusterCaptureSeconds` 0–59 und Minuten 0–30. Standard weiterhin 10 Sekunden. Browser-Countdown aus Beta 104 bleibt auf Coordinator-Zeit basiert; er ist nur Anzeige, nicht der Hardware-Trigger.
- Die eingefrorene Zeitbasis gilt **pro angenommenem Auftrag**; die allgemeine Clusterzeitsynchronisation und die ESP32-Systemuhr laufen unabhängig weiter. Beim nächsten Auftrag werden neue Zeitkorrekturen wieder berücksichtigt. Kein Versuch, externe NTP-Dienste global zu stoppen.
- Grenzen: Unterschiedliche Uhrfehler zum Annahmezeitpunkt bleiben erhalten; spätere Verbesserungen der Synchronisation korrigieren den bereits angenommenen Shot absichtlich nicht. Kameratreiber-Latenz und tatsächlicher Belichtungsbeginn weiterhin nicht gemessen. Bereits vorbereitete Kamera kann aus anderen Gründen scheitern. Keine durchgängige Arduino-ESP32-Kompilierung oder Hardwareverifikation.

## v87-beta104 – Clusterzeit für Capture-Countdown (2026-10-10)

- Der Coordinator-Webclient liest für Zeitplanung die gültige `cluster_time.utc_us` aus `/cluster_status` statt `Date.now()` vom Computer. Gilt für relative Zeiten und die Prüfung fixer UTC-Zeiten. Bei fehlender/ungültiger Clusterzeit wird kein Auftrag verschickt.
- Der Client verankert diese UTC-Zeit anhand der Mitte der HTTP-Laufzeit an `performance.now()`; der Countdown läuft monoton ohne Abhängigkeit von späteren PC-Uhränderungen. HTTP-Transport und ungewisse Abfragephase begrenzen die Anzeigepräzision; dies ist **nur eine Näherung der Coordinator-Clusterzeit**, kein gemessener Auslöse- oder Belichtungszeitpunkt.
- Der Countdown startet bei der ersten erfolgreichen HTTP-Auftragsannahme; Grün markiert weiter nur den Soll-Termin. Der signierte Medienabschlussbericht ist davon unabhängig.
- Kein Wechsel der Capture-Protokollfelder; Node-Firmware und Kamera werden nicht geändert. Auf Coordinator beta104 installieren; Node kann zunächst beta103 bleiben.
- 5 eingebettete JS-Blöcke mit Node.js `--check` geprüft, ZIP-Integrität geprüft. Kein vollständiger Arduino/ESP32-Compile und kein Hardwaretest.

## v87-beta103 – Dokumentationsübergabe (2026-10-10, nur Dokumentation)

- `WIEDEREINSTIEG_SENSORFORGE.md` neu: verbindlicher Arbeitsstand, Systemaufbau, Build-/Delta-Anleitung, Capture-/Media-Ablauf, bekannte Grenzen, Testmatrix und Prioritäten.
- `CLUSTER_ARCHITECTURE.md` um eine kompakte Einstiegskarte ergänzt.
- Firmware, Weboberfläche, Protokolle und Versionskennung **unverändert**: weiterhin **v87-beta103**, keine beta104.
- Dokumentation beruht auf Quelltext der ausgelieferten Deltas sowie bisher berichteten Hardwaretests; vollständiger Build und End-to-End-Capture-/Download-Test sind **nicht** nachgewiesen.

## v87-beta103 – Compilefix HTTP-Fotodownload

- Korrigiert die fehlerhafte C++-Anführungszeichenfolge für `Content-Disposition` in `/capture_media` (`webconfig.cpp`, bisher Zeile 18906).
- Keine Änderung der Medien-UUIDs, Aufträge oder Ringbuffer-Logik.
- ZIP- und statische Stringprüfung; vollständiger ESP32-Build steht aus.

## v87-beta101 — Capture-Fertigstellungsbericht

- Coordinator erhält unter UTC-Countdown einen Live-Bericht für die Nodes des aktuellen Capture-Durchgangs: Gesamtfortschritt und Einzelstatus.
- Authentifizierte Capture-Job-Statuswerte (`kind=6`, `state`, `result`) aus dem existierenden `/cluster_status`-Endpunkt; keine neuen UDP- oder Kameraoperationen.
- Trennung zwischen HTTP-Annahme, Node-ACK und signierter JPEG-Erfolgsmeldung; Fehler, Zeitüberschreitungen und ungeklärte HTTP-Übertragung werden explizit angezeigt.
- Der Browser verfolgt Job-ID (Coordinator-Boot + Sequenz), um andere Capture-Durchgänge nicht als Erfolg dieses Durchgangs zu zählen. Bericht bleibt auf Browser-Sitzung begrenzt; Coordinator-Job-RAM hat maximal 16 Einträge.
- Einschränkungen: Der bestehende Status enthält noch keine JPEG-Größe pro Job und keinen nachgewiesenen Belichtungszeitpunkt; auch ein erfolgreiches JPEG kann später aus dem Ringpuffer verdrängt werden. Kein ESP32-Compile ausgeführt.

## v87-beta100 — Betriebsmodus-Anzeige zweizeilig

- Der Header zeigt klein „BETRIEBSMODUS“ und darunter den aktuellen Modus (z. B. „Recording“). Damit wird der Betriebsmodus nicht mit einer aktiven Aufnahme verwechselt.
- Unveränderter /ui_status-Poll (5 Sekunden), keine neuen Endpunkte oder SD-/Kamera-Zugriffe.
- Kein vollständiger ESP32-Build durchgeführt.

## v87-beta99 – Compilerfix Betriebsmodus-Banner

- Überflüssiges einzelnes `"` in `webconfig.cpp` direkt nach dem JavaScript-Moduswechsel-Block entfernt (vorher Zeile 1677).
- Keine Änderung der Laufzeitlogik; Banner und 5-Sekunden-Statusaktualisierung aus Beta 98 bleiben unverändert.
- Statische Prüfung der betroffenen C++-String-Verkettung durchgeführt; vollständiger ESP32-Compile nicht verfügbar.

## v87-beta98 – Live-Betriebsmodus im Node-Banner

- Auf allen Node-Seiten wird der aktuelle lokale Betriebsmodus rechts neben der Sprachauswahl angezeigt.
- Die Anzeige übernimmt alle 5 Sekunden den Node-Status über den bereits vorhandenen `/ui_status`-Endpunkt. Auch ein Coordinator-Wechsel in/aus Drone Mode wird ohne Seitenreload sichtbar.
- Die Statusantwort enthält `operating_mode`: `off`, `recording`, `powershooter`, `combined`, `streamer`, `drone` (lokal bestimmter Zustand; keine gesonderte Netzwerkabfrage).
- Reiner Anzeigestatus, keine zusätzlichen Kamera-, SD- oder Cluster-Befehle.
- Status-Polling im globalen Header von 20 auf 5 Sekunden erhöht; auf sehr stark belasteten Geräten beobachten.
- Prüfstand: statische Code-/JavaScript-Kontrolle, kein vollständiger Firmware-Compile oder Hardwaretest.

## v87-beta97 – Einheitlicher Betriebsmodus Drone / Bereitschaft

- Konfiguration → Betriebsmodus enthält nun Drone / Bereitschaft als exklusive Auswahl.
- Die bisherige separate Drone-Checkbox auf der Cluster-Seite wurde entfernt.
- Lokaler Betriebsmodus und signierte Coordinator-Drone-Befehle verwenden denselben bestehenden NVS-Zustand (`droneModeSet`).
- Auswahl von Drone erhält die bisherigen Recording-/Shooter-Präferenzen; Auswahl eines normalen Modus deaktiviert Drone.
- Falls beim Wechsel der Kamera-Owner von Streamer auf Normal wechselt, gilt weiterhin der bestehende Reboot-Pfad.
- Die Cluster-Seite bietet weiterhin Drone EIN/AUS für ausgewählte Nodes.
- Hardware-Compile und Laufzeittest noch ausstehend.

## v87-beta94 – Capture-Eingabe und UTC-Countdown (2026-10-10)
- Standard: Foto in 10 Sekunden, frei einstellbare ganzzahlige Sekunden und Minuten; optionaler expliziter UTC-Termin hat Vorrang. Bestehende Capture-Zielgrenze 5–120 s bleibt unverändert.
- Eine einheitliche Hilfsfunktion berechnet den absoluten UTC-Zielzeitpunkt, unabhängig von der Eingabe. Spätere Action-Typen (GPIO, Sequenz, Video) sollen denselben UTC-Planungspfad nutzen; noch nicht implementiert.
- Countdown auf Coordinator im Browser: pulsierender roter Punkt bis zur Soll-Zeit; grünes Pulsieren für ca. 2,8 s danach. Anzeige beweist weder Paket-ACK noch tatsächliche Sensorbelichtung.
- Browser-Countdown nutzt monotone Performance-Zeit, um lokale Uhrsprünge während des Countdowns zu vermeiden. Planung selbst basiert auf Browser-Wanduhr; Abweichung zur Cluster-UTC muss für präzise Capture-Termine später berücksichtigt bzw. serverseitig berechnet werden.
- Keine Änderung am Capture-UDP-Protokoll, der Node-Zeitbasis oder am Kamera-Owner. Arduino-Compile und Hardwaretest offen.

## v87-beta92 — SD health refresh and consolidated coordinator controls (2026-10-09)

- On completion of the remote full SD wipe (success or failure), invalidate the node's cached SD usage and immediately emit a signed cluster heartbeat/health sample. If filesystem usage cannot be read, report unknown rather than stale pre-wipe usage. The ordinary at-most-once-per-minute filesystem usage policy remains in place for normal operation.
- Remove redundant standalone reboot and shutdown sections from `/cluster_coordinate`. These functions remain accessible via node selection and per-node signed jobs, with the latest job/result in the main table.
- Remove the redundant global manual time-sync action; preserve the detailed per-node time measurement display as a collapsible read-only diagnostic. Targeted sync stays in the node selection toolbar.
- Update the preflight description to match the already enabled full wipe command.
- Limitations: health emission depends on active cluster/WiFi; coordinator will not display new SD numbers until the health packet arrives. Successful wipe means maintenance returned success, not physical secure erasure. ESP32 compile and hardware verification pending.

## v87-beta90 – 2026-10-09 – Cluster-SD-Löschung: Sicherheitsvorprüfung (noch kein Löschbefehl)

- Coordinator: neuer Button „SD-Löschung prüfen (ohne Löschen)“ für ausgewählte Nodes.
- Prüft signierte, bestehende Cluster-Statusdaten zu SD-Kapazität und Recording, ohne Schreibzugriffe.
- Knoten ohne SD, mit aktiver Aufnahme oder ohne gültige SD-Daten werden als nicht löschbereit angezeigt.
- Kein SD-Wipe, kein Lösch-Executor, kein Remote-Löschauftrag; bisherige Sicherheitsmechanismen unverändert.
- TODO: isolierter Medien-Executor (Datumordner, Sidecars, offene Dateien, Storage-Sperren, Wiederanlauf) sowie signierte, eindeutige Job-ACKs vor Freischaltung.
- Tests: JS-Syntax und ZIP-Integrität; ESP32-Compile/Hardwaretest ausstehend.

## v87-beta89 (2026-10-09) – Bedienung der Cluster-Sammelaktionen
- Browser-Confirm für harmlose Probeaufträge und gezielte Zeitsynchronisation entfernt. Ein Klick auf den entsprechenden Button löst nach vorhandener Node-Auswahl direkt die individuellen signierten Anfragen aus.
- Bestätigungsdialog für Remote-Neustart und Remote-Shutdown weiterhin verpflichtend, einschließlich Shutdown-Warnhinweis.
- Server-Endpunkte, CSRF-/Authentifizierung, UDP-Protokoll, Zeitmessung, SD-/Recording-/Sleep-Verhalten und Aktionsresultate unverändert.
- JS-Syntax-/Archivprüfung durchgeführt; ESP32-Compile und Hardwaretest ausstehend.

## v87-beta86 (2026-10-09) – HTTP-Sammelaktionen stabilisieren
- Coordinator: während einer Sammelaktion werden neue Hintergrund-Statusabfragen pausiert, bereits laufende Abfragen dürfen enden. Danach sofortiger Statusrefresh. Keine Änderungen an UDP/Cluster-Wire.
- Einzelner HTTP-POST pro Node weiterhin ohne automatische Wiederholung. Browser-Grenze pro POST von 5 auf 12 Sekunden erhöht, um fehlerhafte Client-Abbrüche bei kurzzeitig belastetem ESP32 seltener zu machen. Ein abgebrochener POST hat unklaren Ausführungsstatus; niemals blind wiederholen.
- HTTP-Fehlercodes und knappe Serverfehlermeldungen werden je Node angezeigt; 409 z.B. Ablehnung, statt generischem 'unbestätigt'. Ein 303/opaquer Redirect zählt nur als HTTP-Annahme, **nicht** als signiertes ACK.
- Status-GET-Timeout von 2,5 auf 8 Sekunden erhöht. Letzte gültige Daten bleiben sichtbar. Hintergrund-Polling nutzt weiterhin die gemeinsame Statusabfrage. Kein Hinweis auf Node-Ausfall aus reinem Browser-Timeout.
- Die Meldungen über Coordinator-Reset/Absturz sind **nicht** als Ursache geklärt. Keine Änderung an Recording, SD-Recovery, WiFi, Sleep, Cluster Election, Auth, Restart oder Shutdown.
- Abschluss: JS-Parse, ZIP-Integrität. ESP32-Compile und Hardwareprüfung ausstehend.

## v87-beta85 (2026-10-09) – Coordinator-Tabelle / negative Job-ACKs
- Node-Namen in der Coordinator-Tabelle verlinken direkt auf die WebConfig des jeweiligen Geräts; Ziel-IP wird vor Bildung des Links als IPv4 geprüft.
- Firmwarestand pro Node (`release`, bereits aus dem authentifizierten Cluster-Presence-Protokoll), letzter bekannter Job-Status sowie ACK-Latenz erscheinen in der Haupttabelle.
- Redundanter Bereich „Geräteverwaltung und Firmware“ entfernt. Bestehende Node-WebConfig-Seiten bleiben über den Gerätenamen erreichbar.
- Neues HMAC-signiertes `SFJX1`-Ablehnungs-ACK für authentifizierte Restart-/Shutdown-Aufträge, wenn der lokale geschützte Planer ablehnt. Die Ablehnung wird nur nach vollständiger Identitäts-, Boot-, Epoch-, Lease- und Replay-Prüfung gesendet. Coordinator korreliert Node, IP, Boot, Epoch, Job-ID und Job-Art und setzt `Failed` (Code 2: Aufnahme oder Sicherheitsbedingung). Kein Retry.
- Broadcast-Sync-Status klar als Broadcast-Status ausgewiesen. Ein gezielter Unicast-Sync wird nicht mehr irreführend in die globale Broadcast-Zählung eingerechnet. Eine individuelle Unicast-Ausführungsbestätigung ist noch nicht implementiert.
- Keine Änderungen an Recording-, SD-, Sleep- oder Election-Lifecycle. Kein Firmware-Pool / keine SD-Massenlöschung.
- JavaScript-Syntaxprüfung (7 Blöcke) und ZIP-Integrität: bestanden. ESP32-Compile / Zwei-Node-Hardwaretest: offen.

## v87-beta84 – Koordinator-Mehrfachauswahl (2026-10-09)

- Checkbox pro entferntem Node direkt in der Ressourcenübersicht. Alle auswählen, Auswahl aufheben, Auswahl bleibt bei Status-Polling erhalten; beim Verschwinden eines Nodes wird sie entfernt.
- Sammelbuttons für Probe, Restart, Shutdown und gezielten Zeitsync. Aktionen senden pro Ziel einen **separaten** Administrator-POST und erzeugen individuelle signierte Einzelbefehle; kein Broadcast und kein Auto-Retry für Restart/Shutdown. HTTP-Lauf begrenzt auf 5 s je Ziel; Rückgabe ist **kein Ausführungsbeweis**.
- Gezielt synchronisieren verwendet das bestehende signierte SFTS1-Format als Unicast an den ausgewählten Node; die bisherige Funktion 'Zeit jetzt synchronisieren' bleibt als Broadcast verfügbar.
- Coordinator selbst nicht auswählbar. Shutdown-Warnung ausdrücklich vor Start; alle ausgewählten Ziele werden nacheinander angefordert, nicht transaktional. Bei einem Teilfehler laufen weitere Ziele weiter.
- **Nicht enthalten:** negative ACKs für wegen Recording abgelehnte Befehle und sicherer Nachweis des Deep-Sleep. Bis zur nächsten Protokollstufe ist 'keine Bestätigung' mehrdeutig. Kein SD-Löschen oder Firmware-Pool.
- Prüfungen: JavaScript-Syntax, statische Prüfung, ZIP-Integrität; ESP32-Compile/Hardwaretest offen.

## v87-beta83 – Geschützter Remote-Shutdown und Probe-Latenz (2026-10-09)

- Eigene signierte UDP-Nachrichten `SFJS1` / `SFJSA1` für **einzelnen** Remote-Shutdown mit Ziel-Node, Ziel-Boot-ID, Coordinator-ID/-Boot, Epoch und Sequenz. Kein Broadcast, keine automatische Wiederholung.
- Empfänger prüft HMAC (bestehender Authentikator), IP, Rolle, Boot, Coordinator-Lease, Epoch, Replay-Sequenz und verwendet ausschließlich `webConfigScheduleShutdown(5000, error)` mit Recording-Sperre und Aufnahme-Neustartsperre. ACK bedeutet nur *Shutdown geplant*, nicht *ausgeführt*.
- Coordinator zeigt Shutdown-Jobs getrennt und erfordert Browser-Bestätigung mit Hinweis: **Deep Sleep ohne Wake-Quellen, RESET/Stromversorgung nötig**.
- ACK-Latenz (`ack_latency_ms`) wird nun auch für bestätigte Probeaufträge angezeigt. Keine Änderungen an Zeitsynchronisation oder bestehendem Neustarttransport.
- Grenzen: keine auftragsspezifische persistente Shutdown-Bestätigung; ein verlorenes ACK wird nicht erneut gesendet. Der Coordinator bleibt eingeschaltet. Kein Massen-Shutdown.
- Prüfungen: statische Source- und JS-Syntaxprüfung, ZIP-Prüfung. Vollständiger ESP32-Compile und Hardwaretest ausstehend.

## v87-beta82 – Probe-Ergebnisliste und HTTP-Diagnose (2026-10-09)

- **Behobene UI-Vermischung:** Die Probe-Ergebnisliste enthielt bisher sämtliche `job_probes`, darunter Neustartaufträge (`kind=1`), weshalb nach einem Restart fälschlich `Angenommen` unter den Verbindungstests erschien. Die Anzeige filtert jetzt strikt auf Probeaufträge (`kind=5`).
- **Laufzeit-Gesamtzähler:** `job_probe_diagnostics.jobs_started` zählt erfolgreich angelegte Probeaufträge seit Cluster-Runtime-Start und ist unabhängig von den maximal 16 Einträgen des gemeinsamen RAM-Jobverlaufs. Gültige ACKs werden separat gezählt. Der Zähler wird bei Cluster-Runtime-Neustart zurückgesetzt.
- **HTTP-Hinweise:** Ein einzelner 2,5-s-Abbruch gilt nicht mehr als Beweis für Cluster-Ausfall. Letzte bekannte Job- und Node-Daten bleiben sichtbar; Status-Fehler und anschließende Erholung werden verständlicher kommuniziert. Kein erhöhter Timeout und keine zusätzliche HTTP-Last.
- **Referenztest Beta 81:** Einmaliger Einzel-Neustart devxiao1: gültiges ACK in 41 ms, anschließender Boot-ID-Wechsel, erfolgreiche Cluster-Rückkehr. Der Boot-Wechsel allein ist kein kausaler Nachweis.
- Keine Änderungen an signiertem UDP-Wire, Heartbeat, Election, Recording, SD, Sleep, Restart-Ausführung oder Firmwareupdate.
- Tests: JS-Syntax, ZIP-Integrität und Source-Prüfungen; vollständiger ESP32-Compile und Hardwaretests noch offen.

## v87-beta81 – Neustart-ACK-Zustand und Web-Diagnose (2026-10-09)

- **Behobener Fehler:** Ein per HMAC akzeptiertes Neustart-ACK setzte den Job auf `Accepted`, doch `ClusterJobs::expire()` überschrieb ihn nach 15 s mit `TimedOut`. Das erzeugte die widersprüchlichen Anzeigen „ACK akzeptiert“ und „Keine ACK-Bestätigung“. `Restart/Accepted` bleibt jetzt unverändert; das Transport-Timeout gilt weiter für unbestätigte `Sent`-Aufträge.
- Der signierte ACK-Pfad prüft bereits Coordinator-Boot, Epoch, Ziel-Node, Ziel-Boot, Job-Sequenz sowie ausstehende Job-Art und -Zustand. Die UI zeigt jetzt Boot-/Sequenzanteil der Job-ID und, bei bestätigtem Neustartauftrag, die Dauer von Anlage bis ACK. Keine ungenaue 64-bit-JSON-Nummer.
- Auf der Coordinator-Seite erscheinen Browser-Messwerte für erfolgreiche `/cluster_status`-Abrufe, Fehlschläge und die letzte HTTP-Dauer. Das ist **keine** UDP-RTT. Bestehende 2,5-s-Browsergrenze bleibt zunächst als Diagnosegrenze erhalten.
- Bei Probe-Zeitüberschreitung wird im Ergebnisfeld „Timeout“ statt des irreführenden Codes 0 angezeigt.
- Ein Boot-Wechsel bleibt ein separater Nachweis eines Neustarts, kein kausaler Beweis für genau diesen Fernauftrag. Kein nachträgliches Umschreiben von fehlgeschlagenen Jobs und **kein automatisches Wiederholen von Neustarts**.
- Keine Änderungen an Election, Presence, Heartbeats, Recording, Storage, Sleep, Shutdown oder Firmwareupdate.
- Host-Test für persistierenden `Restart/Accepted`-Zustand durchgeführt; Browser-JS-Syntax und ZIP geprüft. ESP32-Compile und Zwei-Node-Hardwaretest ausstehend.

## v87-beta79 – Coordinator-Statusanzeige repariert (2026-10-09)

- Fix: Vier Coordinator-Widgets behandelten das bereits dekodierte Ergebnis von `sfCoordinateStatus()` fälschlich als `Response` und riefen `.json()` erneut auf. Das ließ Node-Auswahl, Probe-Status, Neustart und Zeitstatus leer erscheinen.
- Alle vier Aufrufer verwenden jetzt das gemeinsame JSON-Objekt direkt.
- Keine Änderung an UDP-Transport, Cluster-Leases, Recording, SD oder Fernaktionen.
- Browser-JavaScript-Syntax/Archiv geprüft; ESP32-Compile und Hardwaretest ausstehend.

## v87-beta78 – Coordinator-Webstatus begrenzt (2026-10-09)

- **Befund:** Die Coordinator-Ansicht startete vier unabhängige Abfragen desselben `/cluster_status`-Endpoints (drei davon alle drei Sekunden). Ohne Request-Deadline konnten HTTP-Abfragen bei einem langsamen ESP32 überlappen und eine scheinbar endlos laufende Probe anzeigen. Das ist eine plausible Ursache; ein Hardware-Nachweis steht aus.
- **Korrektur:** Alle Coordinator-Widgets teilen sich nun eine einzige laufende Statusabfrage (Single-Flight) mit 2,5 Sekunden Browser-Abbruchgrenze und kurzer Cachefrist. Die Cluster-Status-API und das Cluster-Protokoll bleiben unverändert.
- **Probeformular:** Absenden ohne Seitennavigation, maximal 5 Sekunden Browserwartezeit auf die HTTP-Antwort; unbestätigter HTTP-Abbruch wird **nicht** als sicher fehlgeschlagener UDP-Job interpretiert. Job-Ergebnisse bleiben vom 15-Sekunden-Timeout des Cluster-Job-Moduls abhängig.
- **Diagnose:** Anzeige der Probe-Retransmissions. Keine automatische Wiederholung destruktiver Jobs, keine Änderungen an Election, Recording, Storage, Wifi, Sleep.
- **Tests:** JavaScript-Syntax und ZIP-Integrität geprüft. ESP32-Compile und Zwei-Node-Hardwaretest ausstehend.
- **Prüfplan:** Beide Nodes aktualisieren; Coordinator-Seite laden; 10 Probeaufträge nacheinander; kein Browser-Hängen, jedes Job-Ergebnis binnen 15 Sekunden plus Poll-Verzögerung; Browserstatus bei HTTP-Abbruch; danach optional Remote-Restart separat testen.

## v87-beta77 — 2026-10-09 — Probe-Transport robuster machen
- Nur harmlose SFJP1-Probeaufträge: max. zwei erneute Sendungen nach 3 bzw. 6 Sekunden; **dieselbe Job-ID**, dieselbe Coordinator-Epoch und dieselbe Ziel-Boot-ID.
- Die ursprüngliche 15-Sekunden-Frist wird nicht verlängert; Sendungen enden bei ACK, geänderter Node-Boot-ID, Lease-Verlust oder Epoch-Wechsel.
- Empfänger akzeptiert Duplikate derselben Probe-Sequenz und antwortet erneut; HMAC, Peer-IP, Boot-, Epoch- und Lease-Prüfungen bleiben aktiv.
- Neuer Diagnosewert `job_probe_diagnostics.retransmissions`; bisherige Zähler und Neustartnachweis bleiben erhalten.
- **Keine** Neustart-/Shutdown-/SD-/OTA-Retries; keine Änderung an Recording, Storage, Election oder Zeitsynchronisation.
- Noch offen: vollständiger ESP32-Compile, Hardwaretest (beide XIAOs), Ursachenanalyse anhand beider Empfangsdiagnosen. Erfolgreiche Probeübertragung belegt keine allgemeine Netzzuverlässigkeit.


## v87-beta76 — 2026-10-09 — Boot-Wechsel-Diagnose
- Coordinator zeigt pro Node vorhandene Uptime und authentifizierte Boot-ID aus SFC1 Presence.
- Ausgehende Einzel-Neustartaufträge speichern die ursprüngliche Boot-ID im begrenzten RAM.
- Wenn danach eine signierte Presence desselben Nodes mit anderer Boot-ID empfangen wird, erscheint ein separater Boot-Wechsel-Nachweis. ACK-Status/Timeout wird dabei bewusst **nicht** umgedeutet.
- Weder ein ACK noch eine geänderte Boot-ID beweist kausal die Ausführung exakt dieses Jobs. Kein Auto-Retry, keine neuen Remote-Aktionen.
- Offene Punkte: Hardwaretest mit beiden XIAOs; gelegentliche Probe-Timeouts diagnostizieren; später persistierte Job-ID/Boot-Ursache für eindeutigen Neustartnachweis und robusteres ACK-Protokoll.

## v87-beta74 — 2026-10-09 — Diagnose des Job-Transport-Timeouts

- SFJP1/SFJA1-Parser um getrennte RAM-Zähler für Empfang, ACK-Versand, Ablehnung und den letzten Ablehnungsgrund erweitert.
- Coordinator-Oberfläche zeigt lokale Transportdiagnose neben Probe-Ergebnissen; Ziel-Node-Diagnose über `/cluster_status` verfügbar.
- Keine Lockerung von Authentifizierung, Boot-/Epoch-/IP-Prüfung oder Replay-Schutz. Keine Retries und keine Remote-Ausführung.
- Zeitmessalgorithmus bewusst unverändert: einzelne RTT von 183 ms ist für belastbare Optimierung unzureichend. Nach Lokalisierung des Transportfehlers gezielter Messreihen-Test.
- Prüfung: statische Quellprüfung/ZIP-Integrität. ESP32-Compile und Hardwaretest offen.

## v87-beta73 — 2026-10-09 — Coordinator-Probeoberfläche

- Auf `/cluster_coordinate` lassen sich authentifizierte, harmlose SFJP1-Probeaufträge pro entferntem Node auslösen.
- `POST /cluster_job_probe` prüft die lokale Coordinator-Rolle und verwendet ausschließlich die vorhandene `clusterSendJobProbe()`-Validierung (authentifizierter Peer, Boot-/Epoch-Bindung, 15-s-Timeout).
- Ergebnisse werden über das bestehende `/cluster_status`-JSON (`job_probes`) alle 3 Sekunden angezeigt: gesendet, bestätigt, fehlgeschlagen, Zeitüberschreitung.
- Die globale bestehende Administrator-/CSRF-Middleware gilt auch für die neue POST-Route. Keine neue Web-Auth-Ausnahme.
- **Keine Remote-Ausführung:** Reboot, Shutdown, SD-Löschen und Firmwareupdates bleiben ausdrücklich deaktiviert.
- Überprüft: Delta-Dateien, ZIP-Integrität und statische Einbindung. Vollständiger ESP32-Compile und Hardwaretest offen.
- Nächster Hardwaretest: beide Nodes starten, Probe auf dem Coordinator auslösen, individuelle Bestätigung und Timeout bei ausgeschaltetem Ziel testen.

## v87-beta72 — 2026-10-09 — Authenticated job transport probe

- Adds signed SFJP1/SFJA1 unicast probe/ACK over existing coordinator UDP socket.
- Receiver validates coordinator ID, boot, epoch, target boot, authenticated peer IP and replay sequence.
- Coordinator validates reply and matches it to the exact job/node before marking success.
- Only `Probe` jobs are wired; no restart/shutdown/delete/update execution or UI action is enabled.
- Probe results are exposed in read-only cluster status as `job_probes`; expired probes time out.
- No retransmission in this stage. A sent packet is NOT considered completed without a matching ACK.
- Real hardware compile/integration testing remains outstanding.

## v87-beta71 (2026-10-09) – Cluster-Job-Grundlage, sichere Vorstufe
- Neues separates `cluster_jobs.cpp/.h`: begrenzte RAM-Tabelle mit maximal 16 Job-Einträgen, Node-ID, Job-ID, Coordinator-Epoch, Zeitlimit, Ergebniscode und strengem Zustandsautomaten.
- Doppelte Job-IDs, falsche Epochen, unzulässige Zustandsübergänge und verspätete Quittierungen werden abgewiesen. Monotone Millisekunden-Timeouts berücksichtigen `millis()`-Überlauf.
- `clusterNetworkStop()` verwirft die flüchtige Job-Tabelle. Kein persistenter Auftrag wird nach Reboot versehentlich fortgesetzt.
- **Bewusste Sicherheitsgrenze:** Diese Beta verschickt keinerlei Jobs und führt keinerlei Remote-Aktionen aus. Keine neuen Web-Routen, keine SD-Löschung, kein Power-Befehl, kein Firmwaredownload und keine Änderungen an bestehenden Cluster-Wire-Formaten.
- Hostseitiger C++11-Test: Zustandsübergänge, ID-Duplikate, Epoch-Prüfung, Timeout inklusive Wraparound, Reset und Node-ID-Validierung bestanden. Vollständiger ESP32-Compile sowie Hardwaretests stehen aus.
- Nächste Stufe: authentifizierter Job-Transport mit Coordinator-Bindung, unverwechselbarem Command-Nonce, Node-seitiger Deduplizierung und ausdrücklich bestätigtem Abschluss pro Gerät; erst danach Reboot/Shutdown.

## v87-beta70 (2026-10-09) – sichere Verwaltungslinks (Zwischenstufe)
- Coordinator listet Links zu bereits existierenden, lokal geschützten Verwaltungsseiten jedes sichtbaren Geräts: SD-Wartung, Neustart, Herunterfahren und signiertes Firmwareupdate.
- Keine Remote-Ausführung, keine Massenlöschung, kein Firmware-Pool; diese bleiben gesperrt, bis bestätigte Jobs und Board-Mapping implementiert sind.
- Redundanten Sync-Hinweis entfernt; Live-Status bleibt erhalten.
- Keine Änderung an Cluster-Protokoll, Recording, Storage, Safety oder Firmware-Signaturprüfung.

## v87-beta69 (2026-10-09) – Coordinator: read-only Node Health

- Signiertes SFH1-Zusatzpaket im bestehenden 10-s-Heartbeat: SD MiB, CPU-Temperatur, freier Heap.
- SD-Werte werden maximal einmal pro Minute und nicht während aktiver Recorder-Dateioperationen gelesen.
- Cluster-Steuerseite zeigt alle Nodes mit SD-Verbrauch/Freiplatz, Temperatur, Recording-Status und Heap.
- Zentrale destruktive und Power-Aktionen bewusst noch gesperrt: pro Node quittierte, ownership-sichere Ausführung fehlt.
- Keine Änderungen an Election, Time Sync, Recording oder bestehendem Wire-Format.

## v87-beta68 (2026-10-09) – Cluster-Zeitstatus pro Node

- Authentifizierte, kurze Unicast-Statusmeldung nach erfolgreicher WiFi-Zeitmessung; RAM-only am Coordinator.
- Coordinator-Steuerungsseite mit Live-Tabelle für letzte Messung, gemeldete Zeit, geschätzte Uhrabweichung, RTT und Auftragsstatus.
- Keine zusätzlichen periodischen Pakete; kein Eingriff in Systemuhr, Recording, Cluster-Election oder Heartbeat.

## v87-beta67 (2026-10-09) – Cluster overview navigation for all remote devices

- Elected coordinator appears first in the cluster overview and detailed peer table.
- Remote coordinator links directly to `/cluster_coordinate`; remote nodes link to `/` (start page).
- Local device deliberately has no navigation link. Invalid/offline addresses do not generate links.
- Per-device WebConfig authentication remains unchanged; no credentials or tokens are forwarded.
- Presentation-only update; cluster protocol, runtime, recording, and WiFi unchanged.

## v87-beta66 (2026-10-09) – Coordinator navigation in cluster overview

- Coordinator row links directly to `/cluster_coordinate` on the elected coordinator, including from another node.
- Link is shown only for a currently online coordinator with a valid IPv4 address; local coordinator uses a relative URL.
- Existing target-device authentication stays in force; no secrets or login tokens are forwarded.
- Display-only change: cluster election, time sync, WiFi and recording remain unchanged.

## v87-beta65 (2026-10-09) – Coordinator time sync command

- Coordinator-only WebConfig menu “Cluster koordinieren” with a manual time-sync button.
- Signed multicast SFTS1 trigger bound to current coordinator identity, boot, epoch and monotonic command sequence.
- Receiver verifies active coordinator, known boot, matching IP/epoch and rejects replayed commands.
- Nodes request time over existing authenticated SFTQ1/SFTR1 exchange immediately on trigger; 15-minute normal interval unchanged.
- Does not modify RTC, system clock, recording ownership or WiFi lifecycle.
- Best-effort command only: no per-node completion acknowledgement yet.

# v87-beta64 (2026-10-09) — Cluster time interval and quality display

- WiFi time polling every 15 minutes once a valid sample exists; 30-second retry before the first usable sample; immediate request after a coordinator/epoch change.
- Cluster UTC holdover valid for up to 30 minutes only while the authority identity and epoch remain the same. No RTC changes.
- Status estimates local system-clock offset in milliseconds and a rough RTT/2 plus nominal 50-ppm drift growth indicator. This is NOT a guaranteed uncertainty bound or microsecond/frame accuracy claim.
- Coordinator-initiated time-sync command and independent IR precision source are not implemented in this beta.

# v87-beta63 (2026-10-09) — WiFi cluster time foundation

- Optional cluster-runtime sidecar: authenticated four-timestamp request/response over existing UDP 39428 every 30 s per non-coordinator; no broadcast payloads or additional sockets.
- Coordinator answers only known authenticated cluster peers, only when its UTC clock is plausible, within elected coordinator epoch.
- Node rejects stale/wrong-epoch/replayed responses and RTT >250 ms; status JSON includes validity, sample age and observed RTT.
- Independent monotonic cluster UTC accessor; no system/RTC stepping, recording pause, licensing time changes or IR claims.
- Experimental: coordinator source is its own RTC/system clock; automatic best-source selection, PLL/slew and hardware trigger alignment are not yet implemented.

## v87-beta62 (2026-10-09)
- Cluster-Teilnahme in verständlicher Sprache und zugehörige Konfigurationsfelder bei Nichtteilnahme ausgeblendet.
- Cluster-Auswahl und Passwort zusammengeführt, technische Erklärungen in aufklappbare Hilfetexte verlagert.
- Cluster-Übersicht bleibt unabhängig von der Teilnahme sichtbar; keine Änderung an Cluster-Runtime oder Speicherpfaden.

## v87-beta61 (2026-10-09)
- Cluster-Weboberfläche: Titel enthält den lokalen Hostnamen für Copy/Paste.
- Kompakte Standard-Übersicht und aufklappbare technische Tabelle mit ACK-/Election-Diagnose.
- Ressourcen und Discovery-/Protokollinformationen sind aufklappbar; keine Runtime-/Protokolländerung.

# SensorForge Changelog

Current development worktree: **v87-beta60**  
Last stable official release: **v86**  
Core branding version: **7.1.0**

This changelog is intentionally concise. Detailed intermediate experiments and
superseded implementation notes belong in Git history and the project re-entry
document, not in an ever-growing release file.



## v87-beta60 — 2026-10-09

- Process bounded queued Cluster multicast and direct UDP packets before expiring peer leases.
- Refresh expiration/election time after packet processing. Accepted authenticated ACKs already renew coordinator liveness; preserve that behavior and prevent pending ACKs from losing a race against lease expiry.
- Keep the actual 60-second failure lease, existing HMAC, replay/epoch checks, election order, and WiFi/recording ownership unchanged.
- Hardware and larger-cluster validation remain required; this is not a stable release yet.

## v87-beta59 — 2026-10-09

- Fix a loop-start versus packet-reception clock mismatch in `clusterLoop()`:
  election now uses a fresh `millis()` sample after processing UDP packets.
- Clamp a peer's slightly future-dated `lastSeenMs` to age zero while choosing
  the Coordinator, instead of allowing unsigned subtraction to wrap and
  temporarily disqualify an otherwise live peer.
- Preserve authenticated peer leases, policy order, Coordinator epoch semantics,
  ACK validation, packet formats, WiFi ownership and recording behavior.
- Hardware validation, especially failover and recovery, is still required.

## v87-beta57 — 2026-10-09

- Guard the current coordinator's ACK-authenticated identity and epoch against transient incomplete self-announcements while the same boot is live and the ACK is recent (maximum three heartbeat intervals).
- Explicit nonzero coordinator epoch announcements, coordinator reboots and lease expiry remain authoritative; HMAC validation and the election order are unchanged.
- Add a RAM-only diagnostic counter for guarded incomplete coordinator announcements to help distinguish the suspected cause from other epoch changes.
- Hardware regression and compile verification are still required; this change is not yet a qualified cluster stability release.

## v87-beta50 — 2026-10-08

- Added pre-save credential validation when joining a currently discovered Cluster. The passive `/cluster` scanner now retains a small RAM-only cache of recent HMAC-signed `SFC1` Presence packets while the page is open.
- A candidate Cluster password is checked locally against that signed traffic using the existing Beta-38 trust-domain derivation (`cluster_id + password`). The password and derived key are never transmitted. No new wire packet or protocol revision is introduced.
- If the visible Cluster rejects the candidate password, WebConfig returns `Cluster-Passwort ist falsch`, does not modify `config.txt`, and does not schedule a reboot. If public discovery is visible but a signed proof has not yet arrived, save is also blocked and the user is asked to wait a few seconds and retry.
- Recovery from a previously saved wrong password for the same visible Cluster ID keeps the remotely advertised credential epoch after successful proof instead of incorrectly advancing the local epoch and creating another authentication partition.
- Manual creation of a genuinely new/offline Cluster remains possible because no remote proof exists by definition. Existing active membership with unchanged credentials is not revalidated on every settings save.
- Cluster heartbeat, discovery, Coordinator election, WiFi ownership, recording, storage and config schema are otherwise unchanged.


## v87-beta49 — 2026-10-08

- Compile-only fix for the Beta 48 Cluster restart page. `webconfig.cpp` now includes `cluster.h`, which declares `clusterEffectiveId()`.
- No runtime, Cluster protocol, WiFi, storage, recording, or configuration behavior changed.

## v87-beta48 — 2026-10-08

- Fixed first-use Known-Cluster profile lookup on new boards: the NVS namespace does not exist until the first profile is written, and opening it read-only was previously reported as `cluster profile storage unavailable`. Explicit join/save lookup now opens the profile namespace read-write so an empty namespace is initialized safely; a truly unavailable NVS backend still remains an error.
- Cluster settings now follow the same boot-owned apply model as WiFi settings. After a successful persistent cluster save, WebConfig shows a dedicated restart notice and schedules a controlled reboot. This guarantees the newly saved membership/ID/password/policy is applied by a fresh `clusterNetworkStart()` instead of leaving `restart_required` with an inactive/old runtime.
- Passive discovery behavior itself is unchanged: only active cluster members emit public `SFD1` discovery announcements every heartbeat, and a disabled device listens only while `/cluster` is actively open. The UI now states that only active Beta-38-or-newer members are discoverable.
- No change to cluster authentication, election ordering, heartbeat/lease timing, resource announcements, WiFi ownership or recording/streaming paths.

## v87-beta47 — 2026-10-08

- Fixed a real no-SD boot crash observed after Beta 46: after the five expected SD mount retries, the ESP32-S3 entered config load/recovery and hit `Stack canary watchpoint triggered (loopTask)`, causing a reboot loop before WiFi could start.
- The SD failure itself is not treated as fatal. The crash source was excessive nested stack use in config parsing/default/recovery: `ConfigValues` has grown with Camera/Audio/WiFi/Cluster/etc. and several paths created large local instances or return-value temporaries on Arduino's loopTask stack.
- Reworked the config working-object lifetime without changing schema/semantics: defaults are filled into an existing object, validation/parsing/apply/factory-reset temporary `ConfigValues` objects are allocated with bounded temporary heap storage, and the boot load path no longer keeps large `ConfigValues` locals on the loopTask stack.
- This also makes Beta-45 orphaned `/config.tmp` recovery safer, because its validation no longer adds another large `ConfigValues` frame underneath the boot/config call chain.
- Config priority remains SD > internal LittleFS > firmware defaults. Missing SD remains a supported condition; WiFi, Cluster, recording policy and config contents are otherwise unchanged.

## v87-beta46 — 2026-10-08

- WiFi settings are now applied through a controlled reboot immediately after a successful, verified save. Previously the runtime config was updated while the old WiFi interface kept running; a later inactivity timeout could shut that old interface down without ever starting the newly configured STA/fallback path.
- After saving, WebConfig redirects to a dedicated restart/reconnect notice showing the new device name, selected network mode and fallback policy before the old connection disappears.
- The existing STA profile connection and hotspot fallback logic is unchanged; it is now deterministically re-entered by the reboot.
- No change to recording, cluster, storage, config persistence format or WiFi timeout semantics.

## v87-beta45 — 2026-10-08

- Fixed a concrete atomic-config recovery bug exposed by a fresh board without SD: if the very first internal save was interrupted after a complete `/config.tmp` had been written but before `/config.txt` existed, boot recovery previously deleted that temporary file and the device fell back to firmware defaults.
- `recoverAtomicFiles()` now reads and validates an orphaned `/config.tmp`; when valid and no older `/config.txt`/`/config.bak` exists, it reconstructs `/config.txt` from that candidate and only removes the temp file after the final path is verified. Invalid/incomplete temp files are still discarded.
- `atomicWriteText()` no longer treats a successful `rename()` alone as a committed save. The final `/config.txt` is reopened, compared byte-for-byte with the intended text and validated before the backup is discarded or success is returned.
- On a first-ever save, if the metadata rename cannot produce a verified final file, the same already-validated text is written directly to `/config.txt` and verified. Existing-config updates retain backup/rollback semantics.
- This change is inside the shared config persistence layer; WiFi semantics, SD priority, Cluster, recording and streaming behavior are unchanged.


## v87-beta44 — 2026-10-08

- WiFi WebConfig display-only fix: the shown `Hotspot-Name` now updates immediately while the user edits `Gerätename` / `hostname` at the top of the same page.
- The preview uses browser `textContent`; it does not save, restart, open or reconfigure WiFi. The actual hotspot name is still persisted only through the existing WiFi Save path and becomes active according to the existing network/reboot lifecycle.
- No config format, WiFi runtime, fallback, Cluster, recording, storage or persistence behavior was changed.


## v87-beta43 — 2026-10-08

- Hardened the internal LittleFS configuration store after a real no-SD first-run test showed that a saved internal config could disappear or become unavailable after reboot and WebConfig then fell back to firmware defaults.
- Removed destructive unconditional mount-failure recovery. SensorForge now retries a normal LittleFS mount and automatically formats only when the exact internal `spiffs` data partition is demonstrably blank/erased (fresh first-run case).
- If LittleFS mount fails while flash data is already present, SensorForge preserves that data and does **not** format the filesystem. Runtime falls back safely, allowing the existing WiFi recovery behavior to keep the device reachable where possible.
- Added exact internal-config diagnostics: `valid`, `missing`, `invalid`, `unavailable`, plus a detail string such as validation/decryption failure or mount-failure preservation. `/config` now shows this reason instead of the ambiguous `missing / invalid` text.
- Internal config writes still use the existing validated atomic write + SFSEC1 protection path. SD priority, WiFi configuration semantics, Cluster, recording and storage behavior are otherwise unchanged.
- This is a persistence hardening/diagnostic release; the reported fresh-board issue must be re-tested on real hardware before claiming the underlying LittleFS failure mode is fully closed.


## v87-beta42 — 2026-10-08

- Hardened WiFi provisioning after the Beta-40/41 first-run path. The dedicated WiFi save now rejects `hotspot_enabled=0` (infrastructure/STA mode) when no SSID profile is configured, instead of persisting a state that can never connect.
- Added a narrow boot-time recovery for historical/partially provisioned configs that already contain STA mode with zero configured SSIDs: SensorForge starts the normal local hotspot as a recovery path even if the stored fallback flag is `0`.
- This recovery does **not** override the explicit `Offline bleiben` choice when at least one actual infrastructure WiFi profile exists. If configured profiles exist but are unreachable and fallback is disabled, the established offline behavior remains authoritative.
- Successful STA connection, normal hotspot mode, WiFi timeout, schedule, Cluster networking and all recording/storage paths are unchanged.

## v87-beta41 — 2026-10-08

- Changed only the factory/default value of `hotspot_fallback_enabled` from `0` to `1`.
- When infrastructure WiFi is selected and no configured network is reachable, a factory/default configuration now falls back to the device hotspot instead of remaining offline.
- Existing persisted configs remain authoritative: a device that already stores `hotspot_fallback_enabled=0` stays on `0` until the user changes it.
- The normal hotspot/STA lifecycle, fallback implementation, WiFi timeout and Beta-40 first-run persistence path are otherwise unchanged.

## v87-beta40 — 2026-10-08

- Fixed first-run WiFi persistence on a freshly flashed board with no SD card and no existing internal `/config.txt`.
- `configSaveWifiSettings()` now uses the existing canonical `configBuildFactoryDefaultText()` output as the complete initial baseline only when neither SD nor LittleFS contains a persistent config, then patches the requested WiFi keys and passes the result through the unchanged validation, SFSEC1 secret-protection and `configSaveText()` path.
- The first successful save creates only the internal LittleFS `/config.txt`; it does not create an SD config. Existing SD > internal > defaults priority and later SD synchronization rules remain unchanged.
- Existing boards that already have a persistent config follow the exact previous patch path; no camera, recording, Cluster, WiFi runtime or SD behavior was otherwise changed.

## v87-beta39 — 2026-10-08

- Fixed an Arduino-ESP32 3.3.12 compile error in `cluster.cpp`: Arduino defines `HEX` as a macro in `Print.h`, which collided with the local hexadecimal lookup-table name used by `textToHex()`.
- Renamed only that local lookup table to `HEX_DIGITS`; Cluster protocol, configuration, networking, credentials, election and runtime behavior are unchanged from Beta 38.

## v87-beta38 — 2026-10-08

- Added `CLUSTER_ARCHITECTURE.md` as the detailed Beta-38 Cluster freeze/re-entry reference. It records current ownership rules, identities, ports, packet families, election/liveness/credential behavior, known limitations, hardware-test status and the intended future Time-Master/scheduled-capture continuation without changing firmware behavior.
- Added page-scoped passive Cluster discovery. While `/cluster` is actively open, a disabled device may temporarily join the Cluster multicast receive group, but it sends no SensorForge Cluster packet, does not join any Cluster and never starts/extends WiFi. The listener expires automatically after browser polling stops.
- Added public stable `cluster_id` (`cl-` + 128-bit hex) and `cluster_credential_epoch`. Existing Beta-37 configs without an ID receive a deterministic legacy ID derived from the existing Cluster name and persist it on the next Cluster save.
- Active Cluster members emit a tiny unauthenticated `SFD1` discovery announcement every normal 10 s heartbeat containing only Cluster ID/name, credential epoch, node ID, policy and release. It contains no password and grants no membership.
- `/cluster` now builds a dropdown from discovered Clusters plus previously known Clusters. Cluster ID/name can still be entered manually and a new ID is generated automatically when omitted for a new Cluster.
- Added a board-local known-Cluster cache (up to six entries) in NVS. Passwords are encrypted with the existing hardware-bound SFSEC1 mechanism and are never returned to the browser/status JSON. Successfully authenticated membership refreshes the matching cached credential; a freshly created local Cluster is cached immediately.
- If discovery reports a credential epoch different from the saved profile, the saved password is treated as stale and is not reused automatically. The user must enter the new password.
- A local password edit for the same Cluster ID is rejected while authenticated remote peers are currently attached. This prevents accidentally splitting a healthy multi-node Cluster. Isolated recovery/rejoin remains possible and advances the credential epoch when necessary.
- Defined the future password-rotation rule: Cluster-wide credential rotation is a Coordinator transaction (prepare/ACK/commit) and is not partially implemented in Beta 38. Offline nodes that miss a future rotation will see a newer credential epoch and must be re-credentialed.
- Same Cluster ID with incompatible credentials never participates in the same election. The public discovery view can expose the shared ID while authenticated membership remains partitioned, avoiding an automatic or ambiguous password winner.
- Existing signed `SFC1`/lease/resource/coordinator packet layouts remain structurally compatible, but Beta 38 intentionally changes the HMAC/tag trust domain to `cluster_id + password` (`SensorForgeClusterKeyV2`). All members of one Cluster must therefore be updated to Beta 38 together; mixed Beta-37/Beta-38 Cluster membership is not supported. Upgraded Beta-37 configs derive the same deterministic legacy `cluster_id` from their existing Cluster name, so Beta-38 nodes converge after the joint update. mDNS metadata advances to protocol version 4 and adds public Cluster ID/credential-epoch fields.

## v87-beta37 — 2026-10-08

- Added automatic Cluster Coordinator selection without changing WiFi ownership or any recording/streaming lifecycle.
- Added `cluster_coordinator_policy=auto|preferred|node`; default is `auto`, so older configs remain valid without migration. `node` can never become Coordinator; `preferred` outranks `auto`; ties are deterministic by stable `integration_id`.
- The selected Coordinator receives a runtime epoch/generation. Nodes track Coordinator ID + epoch as the future authorization context for coordinated commands; Beta 37 does not yet execute remote commands.
- Added a dedicated authenticated unicast coordination socket on UDP 39428. Every non-Coordinator sends a signed heartbeat every 10 s to the current Coordinator and receives a signed ACK.
- Direct Node→Coordinator heartbeats refresh the Coordinator's peer liveness, so the central view remains reliable even if individual multicast presence packets are missed. Broadcast/multicast remains best-effort discovery/event transport.
- Coordinator election changes only when authenticated peer state changes through LEAVE/lease expiry or a higher-ranked candidate appears; a single missed broadcast does not trigger failover.
- Expanded `/cluster` to show configured policy, actual runtime role, selected Coordinator/epoch and direct heartbeat/ACK age per device.
- mDNS `_sfcluster._udp` remains on the existing central responder; protocol metadata is now version 3 and advertises the separate coordination port.
- Cluster remains disabled by default and still never starts, prolongs or keeps WiFi alive. Time Master/laser synchronization, production sessions and remote snapshot/recording commands remain future stages.

## v87-beta36 — 2026-10-08

- Extended the optional local cluster without changing SensorForge WiFi ownership.
- Presence remains every 10 s; peer liveness now uses a 60 s lease rather than a fixed 35 s timeout.
- Added a small authenticated metadata packet carrying `lease_sec` and an approximate `wifi_remaining_sec`.
- Added a signed best-effort `LEAVE` packet before orderly WiFi shutdown so peers can remove the node immediately; unexpected power/radio loss still falls back to lease expiry.
- Cluster code still never starts, prolongs or keeps WiFi alive. With `cluster_enabled=0`, no cluster socket, mDNS service, heartbeats, HMAC work or resource traffic is started.
- Expanded `/cluster` into a whole-cluster overview showing local/remote nodes, release, operating state, lease remainder, approximate WiFi lifetime and last-seen age.
- Added a generic HMAC-authenticated resource-announcement foundation (`SFR1`) with source node, resource ID/type, UTC microsecond timestamp, size, TTL and compact locator metadata.
- Resource payload bytes are deliberately not multicast. Automatic peer download is not yet wired to storage/API; that will be a separate safety-reviewed step based on the existing concrete file-serving path.
- Existing Beta-34/35 `SFC1` presence packets remain wire-compatible; the new lease metadata is additive.

## v87-beta35 — 2026-10-08

- Fixed a WiFi schedule UI edge case that could submit duplicate start times.
- Newly added schedule rows now choose the next unused 15-minute start slot instead of always defaulting to `12:00`.
- Duplicate start times are detected locally in WebConfig and blocked with a clear browser validation message before the request reaches the config parser.
- The persistent schedule format, runtime scheduler and server-side validation remain unchanged.

## v87-beta34 — 2026-10-08

- Added an optional local SensorForge cluster foundation over the existing WiFi lifecycle; disabled by default for backward compatibility.
- Centralized the existing stable random `integration_id` in `device_identity.cpp/.h` so API and cluster use exactly the same NVS identity.
- Added `_sfcluster._udp` DNS-SD advertisement to the existing central mDNS responder without creating a second mDNS lifecycle.
- Added bounded authenticated UDP multicast presence/status (10 s heartbeat, 35 s peer timeout, max. 16 volatile peers) using HMAC-SHA256.
- Cluster password is persisted through the existing SFSEC1 secret-protection path and is never advertised or returned by status JSON.
- Added dedicated `cluster.cpp/.h` and `webconfig_cluster.cpp/.h`; `webconfig.cpp` only receives navigation/registration hooks.
- Added `/cluster` read-only peer status UI and persisted cluster membership settings. Changes take effect on the next normal WiFi/device restart; no forced reboot is triggered by the page.
- Time validity/epoch is status-only groundwork. Beta 34 does not implement clock correction, remote commands or synchronized multi-camera recording.
- Strengthened the documented development rules: work only from current source, never guess code, preserve existing functionality, safety first, delta ZIPs only, and keep new WebConfig functionality modular.

## v87-beta33 — 2026-10-08

- Extracted the complete Transport Security WebConfig page and its handlers from
  `webconfig.cpp` into `webconfig_transport.cpp/.h`.
- Kept URLs, config keys, measurement flow, recording-pause handling, activation,
  cancellation and reboot semantics unchanged.
- Structural cleanup only; no intended product behavior change.

## v87-beta32 — 2026-10-08

- Simplified Transport Security time controls.
- Maximum transport duration is edited as minutes / hours / days and remains
  stored internally in seconds.
- Transport check interval is edited as minutes / hours; default is now 10 min.
- Confirmation seconds field uses the same compact width as other time controls.
- Removed redundant human-readable total-time echoes from the form.
- Expanded maximum check interval to 24 h and allowed transport duration from
  1 minute up to the existing 7-day maximum.

## v87-beta31 — 2026-10-08

- Gave Transport Security the same floating Save control pattern as other
  WebConfig settings.
- Changed WiFi timeout editing from seconds to minutes while keeping
  `wifi_timeout_sec` as the canonical internal/config representation.
- Kept compatibility with old seconds-based POSTs.
- Prevented unusually bright black-reference measurements from being copied into
  the suggested Transport Security threshold.

## v87-beta30 — 2026-10-07

- Added the SF2 board-bound activation format with a signed issuer key ID.
- Added a license public-key ring with issuer capabilities.
- Firmware now validates both ECDSA signature and issuer authorization, so a
  server key restricted to EVALUATION cannot issue FULL/SERVICE licenses.
- Added tooling for future separated TRIAL, FULL authority and RECOVERY keypairs.
- Production model: online server holds only the restricted trial signer; FULL /
  SERVICE authority and RECOVERY keys remain offline or later non-exportable in
  HSM/KMS.
- Firmware-signing and license-signing trust domains remain completely separate.
- Legacy SF1 remains accepted only for the current beta transition; production
  keyring generation disables it by default.

## v87-beta29 — 2026-10-06

- Split automatic trial media budgets into independent limits:
  - 20 h accumulated recording time
  - 100 h accumulated active streaming time
- Trial also retains the 14-day active/trusted-calendar limit.

## v87-beta28 — 2026-10-06

- Made TRIAL the automatic factory/default product mode; no activation is needed
  to evaluate the complete product.
- After trial exhaustion, devices fall back to DEMO.
- DEMO policy:
  - 10 recording starts per day
  - 1 h recording per day
  - 1 h active streaming per day
  - radar configuration/calibration remains available
  - local management/diagnostic API remains available
- Valid FULL/SERVICE licenses always override trial/demo accounting.
- Licensing limits remain centralized in `license_policy.cpp`.

## v87-beta27 — 2026-10-06

- Added accumulated active streaming usage accounting.
- Streaming time counts wall-clock time while at least one real RTSP or
  HTTP-MJPEG client is active; simultaneous viewers do not multiply usage.
- Added coarse low-wear persistence and live `STRM` UI telemetry.

## v87-beta26 — 2026-10-06

- Added accumulated system-uptime display and total filming-time display.
- Added product-mode badge to the main WebConfig screen.
- Kept uptime accounting deliberately approximate and low-wear across
  deep-sleep/reboot cycles.
- Added migration from the Beta-25 counter format.

## v87-beta25 — 2026-10-06

- Added the first persistent commercial-usage accounting layer.
- Tracks total recording count, recorded seconds, interrupted recordings and last
  trusted time anchor.
- Uses redundant CRC-protected NVS slots for robustness.
- Avoids writes at recording start and in the frame path; segment rotation is not
  counted as a new recording event.
- Initial release was measurement-only and intentionally fail-open.

## v87-beta24 — 2026-10-03

- Moved the user-facing VPN/WireGuard status into WiFi settings.
- Removed the standalone WireGuard configuration/navigation page from the active
  product UI.
- Kept internal config/secret/API scaffolding for future continuation.
- Parked the runtime backend: current XIAO/Freenove builds link no legacy
  WireGuard-ESP32 dependency.
- Camera, RTSP, ONVIF, audio, recording, storage, WiFi runtime and firmware-update
  paths remained unchanged. Host API remains 1.22.

## v87 milestone summary — 2026-10-02 to 2026-10-03

The earlier v87 betas established the major foundations that remain in the
current source tree:

- **Beta 1–2:** feature-freeze checkpoint and additive `/api/v1` expansion to
  host protocol 1.20+ / Integration profile 1.1.
- **Beta 3–4:** signed `.sfw` firmware update format and consolidated firmware
  signing/key workflow.
- **Beta 5–7:** recovery-hotspot consistency and centralized browser CSRF
  protection.
- **Beta 8:** documented local/trusted-network management security model.
- **Beta 9–18:** ONVIF discovery/authentication/interoperability work, culminating
  in field-qualified Discovery + Device/Media + Snapshot + RTSP/JPEG and later
  read-only Imaging/network extensions. No official ONVIF certification claim.
- **Beta 19–24:** WireGuard architecture experiment and cleanup; runtime backend
  ultimately parked because the legacy Arduino backend is incompatible with the
  current ESP-NETIF/Arduino-ESP32 reference stack.

Superseded per-beta debugging details are intentionally no longer duplicated here;
Git history retains them.

## v86 — 2026-10-01 — OFFICIAL

- Last stable official release before the v87 beta line.
- Consolidated role-aware WebConfig access, streaming-user accounts and media
  permissions.
- Added authentication for RTSP and direct HTTP-MJPEG when web access protection
  is enabled.
- Includes Home Assistant/network fixes and the previously stabilized camera,
  recording, storage and integration functionality.

## v85 — 2026-10-01 — OFFICIAL

- Image-Motion WebConfig/live-analysis milestone.
- Streamer frame-analysis groundwork.

## v84 — 2026-09-30 — OFFICIAL

- Network-streamer milestone.
- Multi-WiFi and camera/preview workflow improvements.

## Historical releases

Older pre-v84 release details are intentionally omitted from this concise file.
Use the Git history/tags and retained specialist project documents when a specific
legacy regression must be reconstructed.

## v87-beta75 (2026-10-09) – Single-Node Remote Restart (Testbetrieb)
- Additive signed SFJR1 / SFJRA1 Unicast job pair for one explicitly selected remote Node.
- Coordinator role, boot nonce, coordinator epoch, current peer lease, IP and target boot nonce required.
- Node consumes accepted sequence once; duplicate/replayed commands are rejected.
- Node uses existing webConfigScheduleReboot(3500), refusing active recordings; no direct ESP.restart in cluster code.
- Signed ACK means **reboot scheduled**, not successful startup; Coordinator does not auto-retry.
- New per-node confirmation button and RAM job status on /cluster_coordinate. Bulk restart, shutdown, deletion and firmware pool remain disabled.
- No change to cluster heartbeat, election, SD recording, config or license protocols.
- Scope: code prepared, full Arduino build and hardware test still required.

## v87-beta80 — Neustart-Auftragsdiagnose (2026-10-09)
- SD-Slot devxiao2 laut Hardwaretest leer. Die SD-Mount-Fehler sind daher erwartete Meldungen bei nicht vorhandener Karte und kein nachgewiesener Clusterfehler.
- Remote-Restart auf devxiao1 war zuletzt nicht ausgeführt (Boot-ID unverändert, Uptime gestiegen); Probe-Transport 10/10 einmal erfolgreich, danach ein Timeout.
- cluster.cpp: getrennte RAM-Zähler und begründete Diagnosen für empfangene/verworfene Neustartbefehle sowie empfangene/akzeptierte/verworfene Neustart-ACKs.
- Serielle Logzeilen nur für Restart-Ereignisse mit Annahme/Ablehnungsgrund; keine Secrets oder Auth-Payloads.
- Cluster-Seite jedes Nodes und Coordinator-Seite zeigen lokale Restart-Diagnosen. Die Werte der Nodes werden nicht automatisch zentral gesammelt: Zielgerät separat öffnen.
- Kein Retry destruktiver Jobs, keine Änderung an Reboot-/Recording-Schutz, Cluster-Election, SD-Recovery oder WiFi-Lifecycle.
- Host-Prüfung: JS-Parsing und Delta-Archiv. ESP32-Compile/Hardware-Verifikation ausstehend.

## v87-beta87 — targeted cluster sync correlation (2026-10-09)
- Authenticated selected-node time-sync requests remember their exact sequence per peer, scoped to the current node boot and coordinator runtime.
- Signed node measurement reports are matched to that sequence in `/cluster_status`; the coordinator table now distinguishes selected sync confirmed/pending from broadcast sync.
- No system-clock adjustment, recording timestamp, election, WLAN, or SD maintenance behavior changes.
- **SD mass-delete NOT enabled:** existing SD maintenance WIPE recursively removes all files, including the SD config, then restores it; it is not a recordings-only operation. A distinct recordings-only cooperative executor, storage gate, exact media allowlist and per-node signed job/ACK are prerequisites.
- Verification: JavaScript parse and ZIP integrity; ESP32 compile and hardware tests pending.


### v87-beta88 – Optional-SD-Betrieb (2026-10-09)
- Neuer Schalter auf SD-Wartung: „Betrieb ohne SD-Karte erwartet“. Persistenz in gerätelokalem NVS (`sfsdpolicy/optional`), bewusst vor Laden der SD-/LittleFS-Konfiguration verfügbar.
- Bei aktivem Schalter: einmaliger Mount-Versuch bei jedem Boot, kein periodischer Runtime-Recovery-Versuch, keine Storage-Fault-WLAN-Abschaltung und kein Storage-Fault-Tiefschlaf allein wegen fehlender SD.
- Bei erfolgreichem Boot-Mount: NVS-Schalter automatisch löschen und normale SD-Recovery wiederherstellen. Wenn die NVS-Speicherung fehlschlägt, wird dies gemeldet; der Mount wird nicht verworfen.
- Bestehendes Verhalten im Standardmodus bleibt unverändert; Recording-/Storage-Recovery bei tatsächlichen Medienfehlern darf nicht umgangen werden.
- Test offen: ESP32-Compile, Boot ohne SD, Boot mit nachträglich eingesetzter SD, Aufnahmeschutz, Reset/Power-Cycle.


## v87-beta91 – 2026-10-09: Cluster-SD-Wipe (Hardwaretest ausstehend)
- Der Coordinator kann signierte vollständige SD-Wipes pro markiertem Remote-Node starten. Keine Broadcasts, keine automatischen Retries.
- Der Node nimmt den Auftrag nur vom authentifizierten aktuellen Coordinator an; Boot/Epoch/Replay-Prüfungen bleiben aktiv.
- Sofortiger Recording-Start-Stopp, Abbruch des laufenden Recording-Events über den bestehenden stopRecording()-Finalize-Pfad, danach performSdMaintenance(SD_MAINT_WIPE): SD-Dateien löschen, Config aus interner Sicherung wiederherstellen, Logger neu öffnen.
- Wipe findet im regulären Firmware-Loop statt, **nicht** im UDP-Paket-Handler. Per signiertem Endresultat wird Erfolg oder Fehler zurückgemeldet.
- Einschränkungen: Bei Peer-Lease-Verlust während eines langen Wipes kann das Ergebnis verworfen werden; keine persistenten Aufträge, kein automatischer Retry. Fehlende SD/Besetzt meldet Fehler 100.
- **Nicht getestet:** kompletter ESP32-Build und Zwei-Node-Hardwaretests. Erst mit Testkarten ohne wichtige Daten testen.

## v87-beta93 – UTC scheduled single JPEG (experimental)
- Coordinator can schedule one photo on selected nodes with a shared UTC timestamp.
- Signed, node-specific command; replay protection, accepted/result statuses and bounded 3-JPEG PSRAM ring.
- Dispatch timestamp recorded as **software dispatch**, not sensor shutter/exposure timestamp.
- Camera capture refuses if recording/streamer/API camera owner active; no unsafe parallel camera access. Camera preparation starts approximately 2 s before target, temporarily deferring main-loop work.
- No frame-download endpoint yet; no sequences or film start yet.
- Main-loop cooperative scheduler can be late under blocking activity. Not production sync precision.
- Build and hardware validation still required.

## v87-beta95 — Drone Mode (experimental)
- Persistent independent readiness flag in internal NVS, without overwriting recording/shooter configuration.
- Blocks the normal configured sleep gate while enabled. Thermal emergency and storage-fault safety still apply; certain specialist sleep paths may require follow-up tests.
- Signed, replay-protected unicast coordinator Drone ON/OFF jobs; one acknowledgment records the applied value. No retry.
- Local Drone Mode toggle on cluster coordination page and bulk ON/OFF controls for selected nodes.
- Drone Mode is **not** yet exclusive camera ownership/preemption, and does not guarantee sensor readiness, timing, or interrupt other camera owners.
- No full Arduino ESP32 compile or hardware tests performed.

## v87-beta96 — Passive Drone Mode

- Drone Mode now bypasses autonomous motion/radar/presence diagnostics, Power Shooter, automatic recording starts and idle SD-recovery in the firmware main loop.
- On transition to Drone ON, an ongoing recording is finalized once through the normal `stopRecording()` mechanism; its SD writes are a necessary one-time cleanup, not background activity. No automatic recording restarts while Drone is active.
- Cluster command processing, scheduled UTC capture, web service, WireGuard service and thermal monitoring remain active. Scheduled capture retains the existing camera-owner safety guards. Normal recording/shooter configuration remains unchanged in NVS.
- Local and coordinator Drone help text corrected. No change to signed Drone wire protocol.
- Limitation: explicit manual web maintenance, preview/streaming requests and setup-time SD initialization can still access storage or camera; Drone Mode suppresses the **main-loop autonomous paths**, not every possible explicit service endpoint. No full Arduino/ESP32 build verified.


## v87-beta102 – Capture-Medien und definierter Ringbuffer-Start
- Bei Annahme jedes neuen Capture-Jobs wird der lokale PSRAM-JPEG-Ring geleert.
- Innerhalb eines Jobs gibt es KEIN automatisches Überschreiben: bei drei Slots bzw. PSRAM-Mangel schlägt die Aufnahme mit Kapazitätsfehler fehl; frühere Bilder bleiben erhalten.
- Jede erfolgreiche Aufnahme erhält eine RFC4122-v4-artige UUID (ESP32 Hardwarezufall), UTC-Software-Dispatch-Zeit und Größe.
- Signierte Capture-Abschlussmeldung überträgt UUID, Zeitstempel und Länge; Coordinator bindet sie an die individuelle Job-ID.
- GET /capture_media?uuid=... liefert JPEG vom betreffenden Node mit bestehender Web-Authentifizierung; kein SD-Zugriff.
- Fertigstellungsbericht zeigt UUID, Software-Zeitstempel und direkten Link.
- **Noch nicht implementiert:** Mehrbild-Sequenzen, Video, präziser Sensorshutter-Zeitstempel, persistent gespeicherte Medien.
- **Risiko:** Vorherige RAM-Fotos werden unmittelbar beim nächsten angenommenen Job gelöscht; Links sind nicht dauerhaft.
- Noch kein vollständiger ESP32-Build/Hardwaretest.

### Audit-Paket 4 (Recording-Task-Start, nach Paket 3)
- `recording_write_buffer.cpp`: Start-Handshake per FreeRTOS-Task-Notification ergänzt. Der SD-Drain-Task darf vor Veröffentlichung von `taskHandle_` nicht in `drainTask()` eintreten; ein sehr früher Task-Abbruch kann so kein veraltetes Handle zurücklassen.
- Offenes Risiko: Die Beendigungssequenz setzt `taskHandle_` vor `vTaskDelete(nullptr)` zurück. Eine vollständig abgesicherte Task-Ende-Übergabe erfordert einen gesonderten Lifecycle-Test auf ESP32/FreeRTOS. Keine weiteren Änderungen an SD-Wipe, Power Shooter oder Capture-Speicher.
- Prüfung: Quellcode-Strukturprüfung; kein vollständiger ESP32-Build, kein Hardwaretest.


### Audit-Paket 5b – SD-Wartungsreservierung (2026-10-11)
- `web_sd_maintenance.cpp`: Ein angenommener Cluster-Wipe reserviert den SD-Wartungspfad nun auch gegenüber lokalem Wipe/Format, Secure-Erase und SD-Benchmark. Zuvor sperrte `g_recordingStartBlocked` lediglich neue Aufnahmen, aber nicht die lokalen Wartungsstarts.
- Die Cluster-Ausführung räumt `g_clusterWipe.active` vor dem eigenen `performSdMaintenance(SD_MAINT_WIPE)` ab; dadurch blockiert die zusätzliche Admission-Sperre nicht den reservierten Auftrag.
- Statische Prüfung durchgeführt; ein vollständiger ESP32-Build und Hardwaretests sind weiterhin ausständig. Allgemeine Parallelzugriffe, Recovery-Operationen, Power-Shooter-Zustände und PSRAM-Lastverhalten bleiben Prüfgegenstand.


### Audit-Paket 5d – SD-Benchmark-Sperre (2026-10-11)
- `web_sd_maintenance.cpp`: SD-Benchmark wird bei aktiver SD-Wartung sofort mit HTTP 409 abgelehnt, noch bevor Kartenmetadaten abgefragt werden. Zuvor setzte der Handler nur einen Fehlertext und las dennoch `STORAGE.cardType()`, `cardSize()` usw.
- `webSdMaintenanceBusy()` meldet nun auch `g_storageLocked`, also laufende Wartungsoperationen nach Freigabe des Pending-Cluster-Wipe-Flags.
- Statische Prüfung; keine Hardwaretests oder vollständiger ESP32-Build. Weitere SD-Nebenläufigkeit, Power-Shooter-Transitions und PSRAM-Lasttests bleiben offen.

## Audit-Paket 5e – Power-Shooter-Flush-Admission (2026-10-11)
- Interner `continuousShooterWriteSparseMkvFromBuffer()` verweigert neue SD-Flushing-Vorgänge bei `g_storageLocked` oder `g_recordingStartBlocked`, statt sich ausschließlich auf seine Aufrufer zu verlassen. Gepufferte JPEG-Daten werden bei dieser Ablehnung nicht verändert.
- Nur statische Prüfung; kein ESP32-S3-Compile oder Hardwaretest. Weitere Speicher-/SD-Races und API-Prüfung bleiben offen.

### Audit Paket 5f – Cluster-SD-Wipe (2026-10-11)
- P1: Die Cluster-Wipe-Reservierung bleibt während Recorder-Finalisierung und destruktiver SD-Wartung aktiv; erst danach wird sie freigegeben.
- `performSdMaintenance()` lässt die reservierte Operation nur bei explizit autorisiertem internem Aufruf passieren; lokale Wartungsaufrufe bleiben gesperrt.
- Statische Prüfungen, kein ESP32-Build oder Hardwaretest. Restrisiken: blockierte SD-I/O und alle nicht zentral über die Wartungssperren koordinierten SD-Zugriffe.


### Audit Paket 5g – Mikrofon-Diagnose / SD-Sperre (2026-10-11)
- `webconfig.cpp`: Der Mikrofon-Test akzeptiert keine neuen Aufträge bei `g_recordingStartBlocked` und setzt am Ende `g_storageLocked` nicht mehr blind auf `false`, falls inzwischen ein Cluster-Wipe reserviert wurde.
- Statische Prüfung; keine atomare globale Storage-Lock-Koordination, kein ESP32-Build und kein Hardwaretest.

### Audit Paket 5h – Cluster-Wipe Admission (2026-10-11)
- Cluster-Wipe-Annahme verweigert jetzt auch Aufträge, wenn bereits `g_recordingStartBlocked` gesetzt ist. Damit übernimmt der Wipe keine SD-Exklusivität, während ein anderer Vorgang die Recording-Zulassung blockiert.
- P1 offen: Eine gemeinsame atomare Reservierung aller SD-Nutzer fehlt weiterhin; verteilte volatile Flags sind kein Mutex. Blockierte SD-I/O bleibt ein Fail-Closed-Restrisiko.
- Nur statische Prüfung, kein ESP32-Build und keine Hardwarevalidierung.


### Paket 5i (SD-SPI Remount): Sync-API prueft bei Exclusive-Lease-Ablauf, Aktivierung und Deaktivierung auch g_recordingStartBlocked. Dadurch kein Remount waehrend reserviertem Cluster-SD-Wipe. Nur statisch geprueft; ESP32-Build und Hardwaretest offen.

## Audit-Paket 5m – erste produktive Lease-Anbindung (2026-10-11)
- `web_sd_maintenance.cpp`: Cluster-Wipe erhält beim Queueing ein eindeutiges atomisches Ticket über `SdAdmissionController` und gibt es nach Abschluss gezielt frei. Die bestehenden kooperativen Flags bleiben für kompatibles Verhalten erhalten.
- Nur Cluster-Wipe ist migriert: andere SD-Nutzer nehmen noch keine Lease; laufende I/O wird dadurch **nicht** umfassend atomar ausgeschlossen. Keine Stabilitätsfreigabe.
- Host-Test des Controllers; Firmware-Compile und Hardwaremessung ausstehend.

## Audit-Paket 5n – Secure-Erase-Lease (2026-10-11)
- Secure-Erase-Start reserviert nach vorbereitender Konfigurationsprüfung atomar dieselbe SD-Admission-Lane wie Cluster-Wipe. Abgebrochene Starts geben ihre Lease frei; der abgeschlossene Job gibt sie erst bei der finalen Bereinigung frei.
- Bestehende Sperrflags bleiben erhalten. Andere SD-Nutzer sind noch nicht migriert; vollständiger I/O-Ausschluss und Recovery bei blockierten SD-Schreibvorgängen bleiben offen. Kein ESP32-Hardwaretest.

## Audit-Paket 5o – lokale Wartung und Benchmark (2026-10-11)
- Lokaler Wipe/Format beansprucht jetzt eine Scoped-Format-Lease; Cluster-Wipe verwendet stattdessen seine bestehende Reservierung. Fehlerpfade geben die lokale Lease automatisch frei.
- SD-Benchmark beansprucht vor Kartenabfragen eine Scoped-Benchmark-Lease und weist konkurrierende Reservierungen mit HTTP 409 ab.
- Kompatibilitätsflags bleiben bestehen. Noch keine vollständige Ausschlussgarantie für laufende Recorder-I/O; kein ESP32-Build oder Hardwaretest.

### Audit-Paket 5q – Recorder-Start und SD-Lease (2026-10-11)
- `recorder.cpp`: `recorderStart()` verweigert neue Aufnahmen auch bei aktiver atomarer SD-Wartungslease (`webSdAdmissionController().busy()`). Vorher wurden nur die kooperativen Flags geprüft; die Reihenfolge Lease-vor-Flag ließ ein kurzes Zulassungsfenster.
- Diese Ergänzung ist **Defense-in-Depth**, kein vollständiger Ausschluss laufender Recorder-I/O: der Start prüft die Lease bislang nur einmal vor den Dateisystemzugriffen. Eine atomare Recorder-Besitzerlease bzw. Drain-/Finalize-Quiescence bleibt P1.
- Keine Änderungen an bestehendem Aufnahmeformat, Capture-Timing oder Power Shooter. Nur statische Strukturprüfung; ESP32-Compile und Hardwaretests offen.

### Audit-Paket 5r – atomarer Recorder-Dateistart (2026-10-11)
- `recorderStart()` reserviert jetzt vor jeglichem Dateisystemzugriff eine kurzlebige `RecorderStart`-Lease und hält sie bis zum Ende des Startpfads, auch bei Fehlern. Dadurch kann keine bereits migrierte Wartung/Remount-Operation zwischen Admission-Prüfung und Dateiöffnung reservieren.
- Die bisherigen Flags werden nach Erwerb der Lease erneut geprüft. Bereits laufende Aufnahmen und ihre Schreib-/Finalize-Pfade werden **noch nicht** durch dieselbe Lease geschützt. Die Änderung ist bewusst auf den Start begrenzt; ein atomarer Schutz der gesamten I/O-Lebensdauer und die Migration weiterer SD-Nutzer bleiben P1.
- Host-C++-Test des Admission-Controllers; vollständiger ESP32-Compile und Hardwaretests offen.

### Audit-Paket 5s – Recorder/SD-Lifecycle (2026-10-11)
- Statisch nachgewiesen: Start-Lease schützt nicht Frame-Schreibvorgänge/Finalisierung. Eine unbedachte Ganzaufnahme-Lease würde Cluster-Wipe-Vorreservierungen blockieren. Kein produktiver Code geändert; Übergabeprotokoll in `SD_RECORDER_HANDOFF_AUDIT.md` beschrieben.
- Weiter P1: Quiescence/Drain-Abschluss vor exklusiver SD-Wartung. ESP32-Build und Hardwaretests fehlen.
