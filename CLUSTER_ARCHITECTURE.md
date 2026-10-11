## Audit-Paket 5p (2026-10-11) – SPI-Remount-Admission
- Der Sync-API-SPI-Remount (exklusiver Clock-Wechsel und Diagnose) reserviert jetzt die gemeinsame atomare `SdAdmissionController::SpiRemount`-Lease; Cluster-Wipe, Secure Erase, Formatierung und Benchmark können während der Lease keine eigene Wartungs-Lease erhalten.
- Exklusivmodus-Aktivierung/-Deaktivierung und Lease-Ablauf prüfen auch die aktive SD-Admission. Diagnose reserviert vor dem ersten Dateizugriff und hält die Lease bis nach dem Restore. Die bestehenden kooperativen SD-/Recording-Sperren bleiben erhalten.
- Restgefahr: Bereits laufende Recorder-/SD-I/O nutzt noch keine gemeinsame Lease; nicht vollständig atomare globale Sperrflags. ESP32-Compile und Hardwarevalidierung ausstehend.

## SD-Admission-Prototyp (Audit 5l, nicht produktiv)
`sd_admission_controller.h` definiert eine separat testbare, atomare Einzelbesitzer-Reservierung. Der Cluster-Wipe nutzt diesen Controller **noch nicht**; seine bisherigen Flags bleiben unverändert. Die Reservierung allein ist keine I/O-Barriere. Vor produktiver Migration müssen jeder Schreibpfad sowie Task-/Dateihandle-Lebenszyklen auf den gemeinsamen Controller umgestellt und geprüft werden. Die Ticket-Generation ist auf 24 Bit begrenzt; ein Generation-Wrap ist vor Produktion zu bewerten.

## Audit-Paket 5j – Sync-API-SD-Eintrittspunkte (2026-10-11)

- **P1:** Die Sync-API blockiert jetzt `handleTestSdSpi`, `handleConfigPost`, `handleTransportControl` und `handleConfigStoragePost` auch bei `g_recordingStartBlocked`. So kann eine allein durch eine Cluster-Wipe-Reservierung aktive Recording-Startsperre diese SD-/Konfigurationsaktionen abweisen, bevor der eigentliche Storage-Lock gesetzt wird.
- **Grenzen:** Die globalen Flags bleiben kooperativ und nicht atomar als gemeinsamer Besitzmechanismus. Kein Ersatz für eine zentrale SD-Arbitrierung. Keine Hardwaretests oder vollständiger ESP32-Compile; Prüfung der vier Guard-Stellen und ZIP-Integrität.

## Audit 5c: Capture-/Shooter-Interlock bei Cluster-SD-Wipe

Nach Annahme eines Cluster-Wipe sperrt `g_recordingStartBlocked` nicht nur normale Recording-Starts, sondern auch Power-Shooter-Capture, direkte JPEG-/Sparse-MKV-Ausgaben und den PSRAM-Flush. Das verhindert, dass der Shooter unmittelbar vor der eigentlichen SD-Wartung neue Dateien öffnet. Bereits gepufferte Shooter-Frames werden nicht allein wegen der Sperre verworfen. Diese Sperre ist kooperativ; ein vollständiger Nachweis für sämtliche SD-Zugriffe erfordert Integrations- und Hardwaretests.

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

## Audit-Korrekturpaket 1 (2026-10-10; Basis beta105)

- Bei einem **expliziten** `clusterNetworkStop()` wird ein akzeptierter, noch wartender lokaler Capture-Auftrag verworfen und die Kamera-Armierungsmarkierung gelöscht; nach Runtime-Neustart darf diese alte Deadline nicht feuern. Das ist eine Lifecycle-Stornierung, **keine** Verschiebung durch Zeitsynchronisation. Die theoretische Cluster-UTC und die eingefrorenen Deadlines anderer laufender Nodes bleiben unverändert.
- Der Coordinator nimmt Medienmetadaten aus signiertem `SFJCR1` nur für noch offene, nicht abgelaufene Capture-Jobs (`Sent`/`Accepted`) an. Die erste gültige Erfolgsmeldung bindet die UUID, die Dispatch-UTC (Softwarezeit, keine Belichtungsmessung) und die JPEG-Bytezahl; terminale und weitere Meldungen können diese Daten nicht verändern.
- Der Node löscht vorhandene JPEGs weiterhin ausschließlich bei Annahme eines **neuen Capture-Jobs**, nicht beim Cluster-Netzwerkstopp. Kein ACK-Retry oder Persistenz hinzugefügt.
- Tests: vollständiger ESP32-S3-Build und Zwei-Node-Tests (Stop vor Termin; Neustart; Duplikat / verspätete Resultate) noch offen.

## Wiedereinstieg / maßgeblicher Stand: v87-beta103 (2026-10-10)

**Zuerst `WIEDEREINSTIEG_SENSORFORGE.md` lesen.** Die Datei fasst Architektur, Grenzen, Versionsfolge, Geräteaufbau, Build-Prozess und konkret priorisierte Tests zusammen. Diese Architekturdatei enthält die ausführlichen historischen Designentscheidungen; `CHANGELOG.md` dokumentiert inkrementelle Änderungen.

**Normative Capture-Regeln:** Genau **eine** gemeinsame Cluster-UTC-Zeitbasis; das Sync-Verfahren ist davon getrennt. Coordinator verteilt signierte Aufträge frühzeitig; Nodes lösen selbstständig zum angegebenen UTC-Termin aus. Software-Dispatchzeit ist **nicht** bewiesener Sensor-Belichtungsbeginn. Jeder neue akzeptierte Capture-Job leert den Node-PSRAM-Medienspeicher vor der Aufnahme. Innerhalb desselben Jobs **kein Rollover**, bei Kapazitätsende wird nicht weiter aufgenommen. Jede Aufnahme bekommt ihre eigene Medien-UUID; HTTP-Medienabruf direkt beim Node, Zugriffsprüfung via bestehender Web-Authentifizierung. Coordinator hält Metadaten/Ergebnis nur flüchtig vor. Noch keine Sequenz-, Video- oder GPIO-Timing-Ausführung.

**Drone-Regeln:** Persistenter Betriebsmodus; keine selbstständige Aufnahme, Power-Shooter- oder Motion-Auslösung; automatischer SD-Recovery-Pfad im Hauptloop aus; keine gewöhnliche Sleep-Automatik; Cluster/Web/Zeitabgleich/Capture-Aufträge und thermischer Schutz aktiv. Laufender Recorder wird beim Eintritt regulär beendet; keine vollständige erzwungene Kamera-Verdrängung bei Stream/Vorschau.

## v87-beta103 – Download-Header-Korrektur

Der `/capture_media`-Endpunkt verwendet für `Content-Disposition` einen gültig quotierten Dateinamen der Form `attachment; filename="<uuid>.jpg"`. Die Authentifizierung und die Media-Store-Logik sind unverändert.

## Beta 101 – Capture-Abschlussbericht

Die Coordinator-Webseite stellt pro gestarteter ausgewählter Node-Menge eine Tabelle dar und ordnet signierte Job-Zustände aus `job_probes` anhand `node`, `job_boot`, `job_seq` zu. Neue Aufträge werden von bereits vorhandenen Capture-Jobs abgegrenzt; die Zustände werden mit der existierenden gemeinsamen Status-Abfrage periodisch aktualisiert. HTTP-Annahme ist kein Capture-Erfolg. `Succeeded` steht ausschließlich für die vom Node bestätigte erfolgreiche JPEG-Erzeugung im lokalen PSRAM, nicht für präzisen Beginn der Belichtung oder dauerhafte Speicherung. Der Bericht ist flüchtige Browser-UI; die bounded Coordinator-Job-Tabelle begrenzt die rückwirkende Zuordnung.

## v87-beta100 — Header-Anzeige

Das Header-Badge zeigt nun eine feste Kennzeichnung „BETRIEBSMODUS“ über dem dynamischen Moduswert. Der Wert wird weiterhin aus `operating_mode` in `/ui_status` aktualisiert; Modusumschaltung, Transport und Recording bleiben unverändert.

### v87-beta99 – Compile-Korrektur

Eine alleinstehende `"`-Zeile im C++-generierten Header-JavaScript aus Beta 98 wurde entfernt. Keine Änderung der Protokolle oder Zustandsautomaten.

### v87-beta98 – Lokale Betriebsmodus-Anzeige

Die Header-Pille direkt nach der Sprachauswahl liest `operating_mode` aus `/ui_status` im lokalen Browser-Polling (5 s). Die Bedeutung folgt dem aktuellen NVS-Drone-Override, dann Netzwerk-Streamer, dann Recording/Power-Shooter-Konfiguration. Sie stellt keinen neuen Cluster-Modus oder entfernten Status-Endpunkt dar. Bei Wechseln durch signierte Drone-Jobs zeigt der betroffene Node seine aktuelle Einstellung beim nächsten erfolgreichen Poll. Beim Poll-Fehler bleibt der bisherige Wert stehen; das ist keine Bestätigung frischer Daten.

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

## Beta 89 – UX der Cluster-Aktionsauswahl
Die Sammelaktion `runBulk` verlangt bei `cluster_node_restart` und `cluster_node_shutdown` weiterhin die explizite Browser-Bestätigung. Die nichtdestruktiven Aktionen `cluster_job_probe` und `cluster_node_sync_time` starten ohne redundante Bestätigung. Zielauswahl, authentifizierte POST-Endpunkte und Einzel-ACKs bleiben unverändert; insbesondere bleibt der Unterschied zwischen HTTP-Annahme und UDP-ACK bestehen.

### Beta 82: Probe-UI und getrennte Diagnose

- `ClusterJobs::Kind::Probe` ist Enum-Wert **5**, `Restart` ist **1**. Die Probe-Tabelle muss auf `kind===5` filtern; `job_probes` ist bewusst ein gemischter, auf 16 RAM-Einträge begrenzter Snapshot.
- `job_probe_diagnostics.jobs_started`: monotoner uint32-Laufzeitzähler erfolgreicher Probe-Enqueues, reset beim Stop der Cluster-Runtime; nicht gleich Anzahl sichtbarer Einträge und nicht gleich Zahl bestätigter ACKs.
- Ein Browser-Abbruch nach 2,5 Sekunden bedeutet **keinen** bestätigten UDP-/Node-Ausfall. Letztes erfolgreiches JSON bleibt als UI-Anzeige stehen und der Browser-Fehlerzähler läuft separat.
- Hardware-Referenz Beta 81: Restart ACK nach 41 ms und nachfolgende geänderte Boot-ID; kein Beweis für auftragsspezifische Ausführung ohne persistierten Job-Beleg.
- Vor zentralem Shutdown weiterhin mindestens Wiederholungstest mit Probe, Einzel-Restart, Recording-Sperre und Boot-Return auf zwei Geräten erforderlich.


### Beta 81: ACK-/Timeout-Konsistenz
Ein authentifiziertes und einem laufenden Restart-Job zugeordnetes ACK beendet die reine Zustellungswartezeit. Der Zustand `Accepted` darf nach der 15-s-Sendefrist nicht verfallen. Ein nie bestätigter `Sent`-Restart bleibt hingegen `TimedOut`, auch falls unabhängig ein Boot-Wechsel beobachtet wird. HTTP-Latenz ist nur browserseitige Abrufdauer, keine Cluster-Netzlatenz.

### Beta 78: Coordinator-Webstatus / Diagnosegrenze

Die Seite `/cluster_coordinate` bündelt ihre vier Status-Widgets über **eine** laufende `/cluster_status`-Abfrage (Single-Flight, Browser-Abbruch 2500 ms). Eine kurze Wiederverwendung des JSON reduziert Burstlast; HTTP-Fehler werden angezeigt. Das ist eine browserseitige Begrenzung, keine Garantie gegen serverseitig blockierende Netzwerkoperationen. Der Probe-POST wird per `fetch` mit 5000-ms-Abbruchgrenze statt Full-Page-POST ausgeführt. Ein Client-Timeout sagt nichts über eine mögliche serverseitige Ausführung; niemals automatisch destruktive Aufträge erneut senden. Der Cluster-Job selbst behält seine 15000-ms-Deadline und Beta-77-Wiederholungen ausschließlich für Probe.

**Offen:** tatsächliche Ursache der Hänger mittels Browser-Netzwerkansicht/serieller Logs ermitteln; RAM-/HTTP-Heaplast und ACK-Rate auf zwei XIAOs prüfen. Firmware-Pool, Shutdown und SD-Massenlöschung bleiben gesperrt.


## Beta 76: Neustartnachweis / Diagnose
- `nodes[].uptime_seconds` wird erstmals auch in der Coordinator-Tabelle dargestellt; `nodes[].boot_id` ist die vorhandene signierte SFC1 Boot-Nonce (8 Hexzeichen), **kein** persistenter Hardware-Bootcounter.
- Pro ausgehendem `SFJR1`-Restart merkt der Coordinator `(jobId, nodeId, alteBootNonce)` maximal 16-mal im RAM. Bei akzeptierter authentifizierter neuer Presence wird eine veränderte Boot-Nonce gemeldet als `job_probes[].boot_change_observed`.
- Der Job-Zustandsautomat bleibt unverändert: `TimedOut` bleibt `TimedOut`, selbst wenn später eine andere Boot-Nonce erkannt wurde. Dadurch kein falsches Erfolgsergebnis und keine versehentliche Wiederholung.
- Limitation: Zufälliger/anderweitiger Neustart kann ebenfalls Boot-ID wechseln; ohne beim Boot verlässlich persistierte Job-ID ist keine kausale Zuordnung möglich. Coordinator-Neustart und Cluster-Runtime-Reset verlieren RAM-Daten. 32-bit `millis()`-Uptime läuft nach ca. 49,7 Tagen über.
- Nicht verändert: Signing, WLAN-/Sleep-Ownership, Recording-Gates, SD, Firmware und Systemzeit. Es wurde keine ACK-Neuübertragung eingeführt: echte Transport-Reliability ist noch nicht nachgewiesen.

## Beta 72 – Authenticated job transport staging

`clusterSendJobProbe(nodeId,error)` sends an explicitly invoked, non-destructive SFJP1 probe to a live authenticated peer over UDP 39428. Each packet uses existing cluster HMAC, current coordinator identity/boot/epoch, target identity/boot and a monotonic per-boot sequence. The remote node verifies the currently elected coordinator, source IP, peer boot and epoch and sends SFJA1 ACK. The coordinator verifies target and current job state before transitioning Sent -> Accepted -> Running -> Succeeded. Duplicate/late ACKs are ignored; incomplete jobs time out in RAM after 15 seconds. This stage intentionally exposes no WebConfig action and never executes reboot, shutdown, storage delete or OTA. It is a transport foundation, not completed remote administration.

# SensorForge Cluster Architecture

**Authoritative cluster foundation:** `v87-beta50`  
**Date:** 2026-10-08  
**Status:** foundation frozen for now; next functional stage is time synchronization + scheduled multi-camera capture.

This document is the detailed re-entry reference for the SensorForge local device-cluster foundation. It complements `00_PROJEKTBESCHREIBUNG_WIEDEREINSTIEG.txt`. The source code remains authoritative if documentation and code ever disagree.

## 1. Scope and freeze point

The current cluster layer provides discovery, authenticated peer membership, liveness, automatic Coordinator selection, passive cluster discovery for configuration, encrypted remembered credentials, pre-save credential validation for visible Clusters, and generic resource announcements. Beta 50 keeps the Beta-38+ wire model and validates a join password locally against recent HMAC-signed `SFC1` Presence traffic before persistent membership is changed.

The following are deliberately **not implemented yet**:

- clock correction / distributed time synchronization
- dedicated Time Master / laser synchronization
- remote snapshot / recording commands
- production sessions / synchronized takes
- automatic download of announced resources
- cluster-wide password rotation transaction
- quorum/consensus protocol for hard network partitions

The general cluster infrastructure should therefore remain unchanged until a concrete time-sync / scheduled-capture implementation is designed against the real hardware path.

**Hardware qualification status:** the code has been structurally/syntax checked during development, but a complete multi-device cluster regression on real hardware is still required. Do not treat the current foundation as field-qualified solely because it compiles or because the architecture is documented.

## 2. Non-negotiable ownership and safety rules

These rules are architectural invariants and must not be weakened by future work:

1. `cluster_enabled=0` is the default.
2. Cluster code must never call WiFi start/connect/AP ownership APIs in order to make itself available.
3. Cluster activity is strictly subordinate to the existing SensorForge WiFi lifecycle.
4. Cluster code must never prolong, reopen, or keep WiFi alive.
5. When WiFi is not available, active cluster networking is not available.
6. Ordered WiFi shutdown stops the cluster first; the cluster sends only a best-effort signed leave notification before its sockets are closed.
7. Recording, streaming, sleep/wake, storage, OTA, WebConfig, recovery and network ownership remain with their existing modules.
8. `webconfig.cpp` remains orchestration/navigation only where technically reasonable; cluster UI stays in `webconfig_cluster.cpp/.h`.
9. Existing functionality must not be removed or redesigned as incidental cleanup when extending the cluster.
10. New cluster changes must be based on the complete current source tree; never reconstruct code from memory or old delta ZIPs.

The only intentional exception to “cluster disabled means no cluster runtime” is the page-scoped **passive discovery scanner** described below. It exists only while `/cluster` is actively being polled and never transmits SensorForge cluster packets.

## 3. Identity model

Do not mix the following identities:

### 3.1 Node identity — `integration_id`

- stable random 128-bit ID stored in NVS
- shared by Integration API and cluster
- implemented centrally through `device_identity.cpp/.h`
- not the hostname
- not the MAC address
- not the license hardware ID

The `integration_id` is the persistent logical identity of one SensorForge node inside the cluster.

### 3.2 Cluster identity — `cluster_id`

Format:

`cl-` + 32 lowercase hexadecimal characters

The Cluster ID is public and stable. It identifies the logical cluster independently from its human-readable name and independently from the current password.

New Cluster IDs are random 128-bit values. A Beta-37 configuration that has no `cluster_id` derives a deterministic legacy Cluster ID from the existing Cluster name so jointly upgraded devices converge on the same Beta-38 identity.

### 3.3 Cluster name — `cluster_name`

Human-readable display name. It is not a cryptographic identity and may later be renamed without changing the Cluster ID.

### 3.4 Credential generation — `cluster_credential_epoch`

Public integer generation, starting at 1. It identifies which password generation is currently expected without publishing a password verifier.

A future successful cluster-wide password rotation must keep the same `cluster_id`, change the password, and advance this epoch.

### 3.5 Cluster secret — `cluster_password`

- shared secret, currently 8..63 characters when membership is enabled
- never advertised in mDNS, discovery, status JSON or logs
- stored in config through the existing hardware-bound SFSEC1 secret mechanism
- remembered known-cluster passwords are also stored as SFSEC1 ciphertext in NVS

### 3.6 Authentication trust domain

Beta 38 derives the cluster HMAC key from:

`SensorForgeClusterKeyV2 | cluster_id | password`

The public runtime tag is derived separately from the same stable Cluster ID + password pair. The display name is intentionally excluded from authentication identity.

Consequence: Beta-37 and Beta-38 devices must not be mixed in one active cluster because Beta 38 changed the trust-domain derivation. Devices upgraded together from Beta 37 derive the same deterministic legacy Cluster ID and converge again after the joint upgrade.

## 4. Persistent configuration

Current configuration fields:

- `cluster_enabled=0|1` — default `0`
- `cluster_id=cl-<32 lowercase hex>`
- `cluster_name=<display name>`
- `cluster_credential_epoch=<integer >= 1>` — default `1`
- `cluster_password=<shared secret>`
- `cluster_coordinator_policy=auto|preferred|node` — default `auto`

Cluster settings are saved through the normal config system. A successful Cluster save schedules a controlled reboot so the new membership/credentials/policy enter runtime through a fresh `clusterNetworkStart()`. For a currently visible existing Cluster, Beta 50 performs credential validation before this persistent save and reboot are allowed.

## 5. Network topology and ports

### 5.1 Multicast channel

- group: `239.255.83.70`
- UDP port: `39427`
- maximum accepted cluster packet length: 511 bytes

This channel carries public discovery and authenticated best-effort cluster metadata/events.

### 5.2 Coordinator direct channel

- UDP unicast port: `39428`

This channel is separate from multicast and is used for authenticated Node → Coordinator liveness heartbeats and Coordinator ACKs.

### 5.3 mDNS/DNS-SD

SensorForge retains one central mDNS lifecycle. Cluster code does not call a second `MDNS.begin()`.

When active, Cluster adds:

`_sfcluster._udp`

Current mDNS metadata version is `4`. TXT metadata includes node/cluster identifiers and feature information, but membership must never be inferred from unauthenticated mDNS metadata alone.

Current feature advertisement includes:

`lease,leave,resource,coordinator,passive-discovery`

## 6. Runtime lifecycle

### Cluster disabled

Normal runtime has no persistent cluster socket, heartbeat, mDNS cluster service, HMAC work or resource traffic.

### Cluster enabled + WiFi unavailable

Cluster remains inactive. It must not try to make WiFi available.

### Cluster enabled + existing WiFi lifecycle becomes available

The existing WiFi owner starts networking first. Only after that may `clusterNetworkStart()` initialize:

- derived runtime HMAC key
- multicast socket on 39427
- direct Coordinator socket on 39428
- volatile peer/resource state
- boot nonce / sequences
- Coordinator election state

### WiFi shutdown

`clusterNetworkStop()` runs before radio teardown. If the active runtime can still transmit, it sends a signed best-effort `SFL1` leave packet. Failure to send leave never blocks shutdown.

Unexpected power loss/crash/funk loss is handled by lease expiry rather than by relying on leave.

## 7. Passive cluster discovery while membership is disabled

This is a configuration convenience only.

When `/cluster` is actively open, its status polling calls the discovery touch path. If WiFi is already up but active Cluster membership is disabled, SensorForge may temporarily bind only the multicast receive side.

Properties:

- scanner lease: 12 seconds, continuously refreshed while the page polls
- scanner does not start or extend WiFi
- scanner does not open the Coordinator socket
- scanner never joins/authenticates to a cluster
- scanner never sends SensorForge cluster packets
- scanner stops automatically after page polling ceases
- discovered public nodes expire after about 35 seconds
- up to 32 public discovery nodes are tracked temporarily

The UI can therefore offer a drop-down of visible Cluster IDs/names without making the device a cluster member.

While this page-scoped scanner is active, Beta 50 also retains a very small RAM-only cache of recent signed `SFC1` Presence datagrams from visible senders. These cached packets are not treated as authenticated membership. They exist only so a candidate password can be checked locally before a join is persisted. They expire with the discovery window and are cleared when Cluster networking stops.

## 8. Public discovery versus authenticated membership

A critical distinction:

### `SFD1` — public discovery

Unauthenticated and intentionally public. Current payload concept:

- Cluster ID
- credential epoch
- source `integration_id`
- Cluster display name
- Coordinator policy
- whether the announcing node currently believes it is Coordinator
- firmware release

It contains no password and does not grant membership.

A malicious LAN participant could advertise a fake `SFD1` entry. Therefore `SFD1` is only a user-interface/discovery hint. Election, membership, liveness and future control must use authenticated data. Beta 50 additionally refuses to persist a join to a currently visible Cluster until the candidate password validates an actual signed `SFC1` Presence packet from a sender that advertised that Cluster ID/credential epoch.

### Authenticated traffic

Authenticated packet families include an HMAC-SHA256 based on the active cluster trust domain. A node with the same Cluster ID but a different password is not an authenticated peer and does not participate in the same election.

## 9. Packet families in Beta 38

These packet names are useful during future debugging/re-entry:

- `SFD1` — public passive-discovery announcement, not authenticated
- `SFC1` — authenticated basic presence/status heartbeat
- `SFM1` — authenticated lease + approximate WiFi remaining metadata
- `SFL1` — authenticated best-effort leave
- `SFR1` — authenticated generic resource announcement
- `SFCO1` — authenticated coordination/election metadata
- `SFNH1` — authenticated Node → Coordinator direct heartbeat
- `SFNA1` — authenticated Coordinator → Node ACK

Packet formats are internal protocol details and must not be silently reinterpreted. Add/version fields or introduce a new packet revision if a future feature requires incompatible semantics.

The main loop processes a bounded amount of cluster traffic per pass: at most four multicast packets plus four direct Coordinator packets. This is deliberate protection against cluster traffic monopolizing the main loop.

## 10. Presence and liveness

### Multicast presence

Normal active members emit presence every 10 seconds.

`SFC1` carries basic node runtime state, including:

- Node ID
- boot nonce + sequence
- uptime
- UTC epoch if local time is valid
- operating mode
- recording/streaming/transport flags
- firmware release
- hostname

`SFM1` follows with:

- 60-second lease
- approximate `wifi_remaining_sec`

### Lease

Default authenticated peer lease: **60 seconds**.

A single lost multicast packet therefore has no effect. Several packets can be missed without causing immediate removal/election changes.

### `wifi_remaining_sec`

Status hint only. `-1` means unknown/unbounded. It may be derived from SensorForge's existing WiFi timeout/schedule logic.

No cluster component may use this value to keep WiFi alive.

### Leave

An ordered WiFi shutdown sends `SFL1` so peers can remove the departing node immediately. Loss of the leave packet is harmless because the 60-second lease remains the authoritative fallback.

## 11. Coordinator policy and automatic election

The user does not force a permanent master. Configuration only defines candidacy policy:

- `auto` — eligible; default
- `preferred` — eligible and preferred over `auto`
- `node` — never eligible to become Coordinator

Election is deterministic:

1. `preferred` candidates outrank `auto`
2. `node` candidates are ignored
3. when candidates have the same policy, stable `integration_id` ordering breaks the tie
4. only currently leased/authenticated peers are candidates

The current role is runtime state, not a persistent “I am master” flag.

A newly started first `auto`/`preferred` node can therefore bootstrap a cluster by itself and become Coordinator. If every online member is `node`, the cluster intentionally has no Coordinator.

## 12. Coordinator epoch

Whenever a local node newly becomes Coordinator it creates a runtime Coordinator epoch/generation.

Future critical commands must be associated with at least:

- Coordinator `integration_id`
- Coordinator epoch
- future production/session ID

This prevents stale commands from a previous Coordinator generation from being treated as current merely because the same physical node later returns.

The current election mechanism is deliberately lighter than a full quorum consensus protocol. A hard WLAN network partition can theoretically produce separate independently operating authenticated subclusters. Critical production-session semantics must address this explicitly when they are implemented.

## 13. Direct Node → Coordinator liveness

Multicast is best-effort, so Beta 37+ adds a second liveness channel.

Every non-Coordinator with a valid selected Coordinator sends an authenticated unicast `SFNH1` heartbeat approximately every 10 seconds to UDP 39428.

The heartbeat includes the intended Coordinator ID and epoch. The Coordinator accepts it only if it is currently that Coordinator and the epoch matches.

The Coordinator replies with authenticated `SFNA1` ACK containing the heartbeat sequence. Replayed/older heartbeat or ACK sequences are ignored.

Direct heartbeat receipt refreshes peer liveness at the Coordinator. Therefore a missed multicast presence packet alone does not make the Coordinator lose the node.

Current direct Coordinator lease is also 60 seconds.

## 14. Known Cluster profiles

Up to six successfully used clusters can be remembered board-locally.

Stored public metadata includes:

- Cluster ID
- Cluster name
- credential epoch
- whether a password is present

The password itself is stored only as board-bound SFSEC1 ciphertext in NVS and is never exposed to the browser/status JSON.

If a previously known Cluster is selected again and the visible/expected credential epoch still matches, firmware may reuse the remembered password internally without the user retyping it. When that Cluster is currently visible, the reused password is subject to the same Beta-50 signed-Presence validation before the join is saved.

If the epoch differs, the cached password is considered stale and automatic reuse is blocked. The user must provide the new password once; a later successful join can refresh the known-cluster cache.

Known profiles are convenience state only. Active cluster membership remains defined by the normal configuration.

## 15. Security boundary of the shared-password protocol

The current Cluster authentication model is a local-LAN shared-secret design, not a PAKE or certificate-based protocol. Authenticated packets contain enough public message material for a listener who captures Cluster traffic to test password guesses offline by deriving candidate keys and comparing HMACs. The mDNS runtime tag is also derived deterministically from Cluster ID + password and must be treated as public metadata, not as a secret.

Therefore:

- use a high-entropy Cluster password/secret, not a weak human password
- do not expose the Cluster to an untrusted/shared network and assume the password is protected merely because it is never transmitted in plaintext
- before a commercial security claim is made, re-evaluate whether onboarding should generate a random Cluster secret (for example via QR/copy workflow) or whether a stronger authenticated key-exchange/PAKE design is required
- do not reuse WiFi, WebConfig, license or other product passwords as the Cluster secret

This limitation does not allow a passive listener to join without guessing the secret, but it defines the resistance of the current Beta-38 trust model and must remain visible in future security reviews.

## 16. Password-change / credential rules

Cluster ID and password are independent.

### Current Beta-38 safety behavior

A local password replacement for the same Cluster ID is blocked while authenticated remote peers are currently attached. This prevents one user action on one device from silently splitting a healthy cluster.

An isolated/recovery device with no authenticated peers may be locally re-credentialed. If the same Cluster ID is currently visible and the replacement password successfully validates its signed Presence traffic, the remotely advertised credential epoch is retained. Only a truly local/offline replacement without a visible authenticated proof advances the local credential epoch as needed.

### Future proper cluster-wide rotation

Not implemented yet. The intended model is Coordinator-only and transactional:

1. PREPARE a new secret under the current authenticated trust domain
2. active members ACK receipt/readiness
3. Coordinator COMMITs the new credential generation
4. all participating nodes move to the new password + incremented credential epoch

Offline nodes that miss the rotation will later see the newer public credential epoch, refuse automatic use of their stale cached password, and require re-credentialing.

### Same Cluster ID, different passwords

Such nodes are separate authentication partitions. They do not elect each other and do not automatically select a password winner. Public discovery may reveal that the same Cluster ID is present with incompatible credential generations/configurations, but only explicit recovery/rotation can resolve it.

## 17. Cluster status UI

`/cluster` is implemented in `webconfig_cluster.cpp/.h`. `webconfig.cpp` only wires navigation/routes.

Each node can show the complete currently authenticated visible cluster, including the local node.

Current status includes, where available:

- integration ID
- hostname / IP
- operating mode
- firmware release
- uptime
- recording / streaming / transport state
- local time validity / epoch
- lease and remaining lease
- approximate WiFi remaining time
- configured Coordinator policy
- actual runtime role
- selected Coordinator + epoch
- direct-heartbeat / ACK age

The page is status/configuration only; it does not currently issue remote control commands to other nodes.

## 18. Generic resource announcements

`clusterAnnounceResource()` provides a generic authenticated metadata broadcast foundation.

Current `SFR1` metadata contains:

- owner `integration_id`
- resource ID
- resource type
- UTC timestamp in **microseconds** if known
- size
- TTL
- compact source-relative locator

Limits:

- up to 24 volatile resources in the local table
- resource TTL 1..3600 seconds
- resource ID up to 48 characters
- type up to 24 characters
- locator up to 160 characters

Important:

- JPEG/video/file bytes are never broadcast through the cluster multicast channel
- no camera/recording producer currently calls this automatically
- no automatic peer download is implemented yet
- future pull must reuse an existing, reviewed SensorForge storage/API read path and respect ownership/concurrency rules

A resource announcement is a notification/hint, not the durable source of truth. Because multicast can be missed, future important resource workflows should combine announcements with a recoverable manifest/catalog or sequence-based catch-up mechanism, followed by reliable unicast transfer.

## 19. Performance design constraints

The cluster is intentionally low-rate and bounded:

- Cluster disabled: no permanent cluster traffic/runtime
- Presence cadence: 10 seconds
- direct Node → Coordinator heartbeat: 10 seconds
- small control/status packets only
- packet size capped at 511 bytes
- at most 16 authenticated peers
- at most 24 volatile resource entries
- at most four multicast + four direct packets processed per loop pass
- large resources never multicast

These choices are intended to make the feature negligible compared with MJPEG/RTSP/video traffic. Nevertheless, real hardware regression is still required before calling the performance impact proven negligible.

Recommended regression comparison:

- Cluster OFF vs ON
- recording-only
- RTSP/HTTP-MJPEG streaming
- audio streaming where applicable
- multiple cluster peers
- FPS / frame drops / reconnects
- CPU / heap / PSRAM
- temperature
- WiFi stability
- sleep/wake behavior

## 20. Failure behavior to preserve

### Single multicast packet lost

No immediate effect. Lease/direct heartbeat continues.

### Several multicast packets lost, direct Coordinator heartbeat still works

Coordinator can retain node liveness through the unicast path.

### Ordered WiFi shutdown

Best-effort leave + normal cluster stop. Cluster never blocks shutdown.

### Crash/power loss/radio loss

No leave is expected. Peer disappears after lease expiry.

### Coordinator disappears

After authenticated peer state/lease expiry causes recomputation, eligible nodes deterministically select another Coordinator.

### All eligible Coordinator nodes disappear

Only `node` members remain; no Coordinator exists until an eligible node returns.

### Same Cluster ID with wrong password

No authenticated membership; separate authentication partitions.

### Stored known-cluster password is stale

Credential-epoch mismatch blocks automatic password reuse; user supplies new credential.

## 21. Planned first real application: time synchronization + scheduled capture

This is the next intended cluster feature, but it is not implemented yet.

### 20.1 Keep Coordinator and Time Master conceptually separate

The Coordinator is the organizational/control leader.

The future Time Master is the node providing the best timing reference. In many installations they may be the same device, especially if that device has laser synchronization hardware, but the protocol should not assume they must always be identical.

Future capabilities may include, for example:

- software/NTP-quality time
- WiFi TSF/FTM-derived timing
- laser synchronization transmitter/receiver
- external hardware pulse/PPS/clock input

Do not turn the current `auto/preferred/node` Coordinator policy into a permanent forced Time-Master switch.

### 20.2 Separate timestamp representation from real accuracy

The current resource protocol stores `timestampUs` (UTC microseconds). Do **not** silently reinterpret this existing field as nanoseconds.

When the time-sync layer is implemented, introduce an additive/versioned representation capable of carrying, for example:

- 64-bit time in nanoseconds
- time source/domain
- estimated offset
- measured drift
- uncertainty / quality in nanoseconds
- age of last synchronization

This keeps the protocol open to sub-microsecond hardware methods later without pretending the current system already has that precision.

### 20.3 Scheduled execution, not packet-arrival execution

Critical multi-camera actions should not mean “take a picture when this WiFi packet arrives”.

Intended pattern:

1. Coordinator establishes a production/session context.
2. Nodes have an already synchronized local cluster clock with known uncertainty.
3. Coordinator sends a command sufficiently early with an absolute target execution time.
4. Each Node ACKs that the command is authenticated, current and scheduled.
5. Each Node executes from its own local synchronized timer at the target time.
6. Each Node returns an execution report with actual local execution timestamp and timing quality.

This makes WLAN packet latency/jitter much less important than the quality of the synchronized local clocks.

### 20.4 Production session context

Future critical control should bind commands to at least:

- explicit production/session ID
- Coordinator ID
- Coordinator epoch
- command ID/sequence
- target execute time
- authentication

Stale commands from a previous Coordinator generation/session must be rejected.

During an active take, do not silently perform an automatic Coordinator failover and continue as though nothing happened. A lost Coordinator should initially be treated as a failed/incomplete take; re-election can prepare the next session. Any more advanced behavior must be designed and tested explicitly.

### 20.5 Broadcast versus reliable control

Use multicast/broadcast for discovery and non-critical event/resource notification.

Use authenticated acknowledged unicast for critical commands and readiness/ACK flows. Important commands such as scheduled snapshot, start/stop recording, time-sync state changes or credential changes must not depend on a single best-effort multicast packet.

## 22. Recommended re-entry sequence when time-sync work resumes

1. Start from the complete then-current source tree, not this document or an old Cluster delta ZIP.
2. Re-read `cluster.cpp/.h`, `webconfig_cluster.cpp/.h`, `cluster_profiles.cpp/.h`, WiFi lifecycle ownership, camera capture ownership and the relevant timer APIs.
3. First hardware-test the existing Beta-38 foundation with at least two, preferably three devices:
   - discovery
   - known profile reuse
   - Auto/Preferred/Node election
   - direct heartbeat/ACK
   - LEAVE
   - lease expiry
   - WiFi shutdown behavior
   - Cluster OFF behavior
4. Measure Cluster OFF vs ON performance under real recording/streaming load.
5. Define the actual physical synchronization mechanism and measurable target accuracy.
6. Implement a read-only timing diagnostics stage first: offset/drift/uncertainty measurement without controlling the camera.
7. Only after timing quality is measured, design the scheduled camera-trigger hook against the real existing camera ownership/capture code.
8. Add production-session + authenticated scheduled command + ACK/execution report.
9. Add resource announcements/pull only on top of the real existing storage/API path.
10. Keep all new functionality additive and modular; do not grow `webconfig.cpp` into a feature implementation file.

## 23. Files relevant to future Cluster work

Primary current modules:

- `cluster.cpp` / `cluster.h` — runtime, packets, peer/resource tables, liveness, election
- `cluster_profiles.cpp` / `cluster_profiles.h` — encrypted known-cluster cache
- `webconfig_cluster.cpp` / `webconfig_cluster.h` — Cluster WebConfig/status UI
- `device_identity.cpp` / `device_identity.h` — shared stable node identity
- `config.cpp` / `config.h` — persistent Cluster config
- `config_secrets.cpp` / `config_secrets.h` — active Cluster secret + profile password protection
- `sensorforge.ino` — authoritative WiFi/mDNS lifecycle hooks; Cluster must remain subordinate

Related future timing/capture work must additionally inspect the real camera, recorder, timer and WiFi lifecycle source present at that future revision before changing anything.

## 24. Development / delivery rules for Cluster work

The normal SensorForge rules apply without exception:

- never guess code
- work from the complete latest source tree
- inspect all real callers/owners before changing a lifecycle path
- safety first
- preserve existing functionality unless the requested change explicitly replaces it
- prefer small additive modules
- keep `webconfig.cpp` slim
- secrets never appear in UI/status/logs/release artifacts
- code changes are delivered only as one delta ZIP containing exactly changed/new files under their original project names/paths
- do not add unrelated cleanup, patches, renamed copies or test-plan files to the code ZIP


## Beta 48 activation and first-use clarification

A configured Cluster is not considered discoverable merely because values exist in persistent configuration. Public `SFD1` discovery is emitted only while `clusterNetworkStart()` has successfully activated the Cluster runtime and WiFi is already open under the normal SensorForge lifecycle. Therefore a freshly saved membership/ID/password/policy must be applied before other devices can discover it. Starting with Beta 48, `/cluster_save` schedules a controlled reboot after the persistent commit and shows a dedicated restart page; after reboot the normal WiFi startup owns the subsequent Cluster runtime start.

Known-Cluster credentials are cached in NVS namespace `sfclprof`. On a brand-new board that namespace legitimately does not exist yet. A read-only NVS open cannot distinguish this normal first-use state from a storage error. Beta 48 therefore creates/opens that namespace during an explicit join/save password lookup; an empty cache then means simply “no stored password”, while a real NVS open failure is still surfaced as storage unavailable.

Passive scanner rules are unchanged: while `/cluster` is open, a disabled local node may join the multicast receive group and listen for public `SFD1` announcements, but it does not transmit Cluster packets and never starts or extends WiFi. Only active Beta-38-or-newer members publish `SFD1`; older Beta-37 nodes are not visible to this passive dropdown even though they may implement earlier authenticated Cluster traffic.


## Beta 71: zentrale Job-Verwaltung – Wiedereinstieg / Sicherheitsgrenze

Basis: kompletter Source-Tree v87-beta70 einschließlich Beta-69-Compile-Fix; Delta v87-beta71. **Beta 71 ist eine Infrastrukturvorstufe, kein ausführbarer Remote-Command-Release.**

### Implementiert

- `cluster_jobs.h/.cpp` enthält eine feste, dynamisch nicht wachsende RAM-Tabelle von höchstens 16 Einträgen. Es gibt keine NVS- oder SD-Persistenz.
- Job-Felder: `jobId` (64 Bit), `coordinatorEpoch` (32 Bit), Ziel-Node-ID, Jobart, Status, Zeitlimit, Zeitpunkt der letzten Änderung und numerischer Resultatcode.
- Erlaubter Ablauf: `Queued -> Sent -> Accepted -> Running -> Succeeded`; aus nichtterminalen Zuständen darf ein Job in `Failed`, `TimedOut` oder `Cancelled` wechseln. Terminale Jobs sind endgültig.
- Die Job-ID ist während der gesamten Cluster-Runtime eindeutig und bleibt auch nach Erfolg reserviert. Keine Wiederausführung durch erneutes Einreihen derselben ID.
- `transition()` verlangt die korrekte Coordinator-Epoch. Ein Timeout ist maximal 1 h; Vergleich der monotonen 32-Bit-Zeit ist überlaufverträglich im erlaubten Fenster.
- `clusterNetworkStop()` ruft `ClusterJobs::reset()` auf; damit kann eine neue Runtime keine flüchtigen Altjobs übernehmen.

### Absichtlich NICHT implementiert

- Kein signierter, gegen Replay geschützter Job-Wire-Transport; kein Aufruf dieser Warteschlange von HTTP oder UDP.
- Keine Ausführung oder Umgehung von Recording-, SD-, Shutdown- oder OTA-Ownership.
- Kein Firmware-Pool, keine Downloads, kein Update-Scheduler.
- Keine Freigabe für destruktive Aktionen. `enqueue()` ist eine interne Datenstruktur-API, **keine Berechtigung**, eine Aktion auszuführen.

### Vor den nächsten Freigaben verbindlich

1. Authentifizierte, versionierte Wire-Nachrichten mit konkreter Cluster-ID, Coordinator-ID, Coordinator-Boot-ID und Coordinator-Epoch binden. Alte Aufträge nach Failover strikt ablehnen.
2. Empfangende Nodes benötigen eigene Deduplizierung und persistente/neu bewertete Idempotenz für destruktive Aktionen; coordinatorseitige RAM-IDs allein reichen bei Paketverlust und Reboot NICHT aus.
3. Quittungen unterscheiden *angenommen*, *ausgeführt* und *auf neuem Boot wieder erreichbar*; ein ACK vor Reboot gilt nicht als Erfolg.
4. Pro Node denselben Auftragslebenszyklus vollständig dokumentieren und seine Sicherheits-Owner-Funktionen (Storage-Guard, Recorder, OTA, WiFi/Sleep) vor dem Aufruf prüfen.
5. Für Firmware-Upgrades Board-/Partition-/Version-Metadaten gegen das signierte Paket prüfen; Download-Slots strikt sequenziell vergeben; Coordinator zuletzt; ausfalltoleranter Fortschritt und Recovery.
6. Negative Tests: doppelte Pakete, Replay, ungültige HMAC, falsche Epoch, Coordinator-Ausfall, leere/defekte SD, aktive Aufnahme, Stromverlust während OTA, Zeitüberschreitung und Node-Restart.

### Verifikation

Host-C++11-Zustandsautomat mit `-Wall -Wextra -Werror` kompiliert und getestet. Der vollständige Arduino-ESP32-3.3.12-Build und reale Hardwaretests sind noch offen. Keine Änderungen an dem hardwarebestätigten Cluster-Election-/ACK-/Zeit-/WiFi-Protokoll.


## v87-beta73: Coordinator-Probeoberfläche

`/cluster_coordinate` bietet je entferntem Node einen harmlosen Probeauftrag. Der POST-Endpunkt `/cluster_job_probe` wird von der zentralen Administrator-/CSRF-Prüfung geschützt und ruft ausschließlich `clusterSendJobProbe()` auf. Die Resultate kommen aus der flüchtigen `job_probes`-Tabelle von `/cluster_status`; ein ACK bestätigt lediglich die Verarbeitung eines signierten Probeauftrags, nicht die Ausführung einer Wartungsaktion. Keine Änderungen an Presence/Election/Recording/Storage/OTA. Reboot, Shutdown, Löschen und Firmware-Update sind weiterhin nicht freigeschaltet.

## Beta 74 – Job-Transportdiagnose

`job_probe_diagnostics` unter `/cluster_status` zeigt lokal seit Runtime-Start `received`, `ack_sent`, `rejected`, `last_reject`, `ack_received`, `ack_rejected`, `last_ack_reject`. Alle Zähler RAM-only und ohne Secrets. Eine HMAC-ungültige Nachricht gelangt nicht in diese Parser-Zähler. Coordinator-Timeout bei `received=0` am Ziel: Weg bis zur authentifizierten Job-Verarbeitung prüfen. Bei `received>0` / `rejected>0`: angezeigten Grund prüfen. Bei `ack_sent>0` und `ack_received=0` am Coordinator: Rückweg/Empfang untersuchen. Keine Remote-Aktionen implementiert.

### v87-beta75 – Remote restart safety boundary

- New unicast SFJR1 and reply SFJRA1 reuse the existing HMAC packet envelope. Target selection is per-node, never multicast.
- The receiver checks coordinator identity, live authenticated peer and source IP, coordinator epoch, coordinator boot and its own boot before scheduling the restart.
- Restart job replay counter is RAM-only, keyed to coordinator boot+epoch, and is not reset by repeated packets. Target boot nonce changes after a reboot so an old command cannot target a subsequent boot instance.
- Acknowledgment means scheduling via the existing WebConfig reboot path, not proof of a completed reboot. No retries occur automatically, and unsuccessful outcomes need operator review.
- Recording-active denial remains authoritative. Shutdown and all destructive filesystem/firmware jobs are deliberately not exposed.
- Testing before use: both boards compile, probe still succeeds, individual restart while idle, denial while recording, confirm reboot/rejoin, replay/lost ACK, coordinator failover during command. Do not interpret timeouts as safe to resend automatically.

### Beta 77: begrenzte, idempotente Probe-Wiederholung
- Coordinator hält maximal 16 flüchtige Retry-Einträge parallel zu den vorhandenen ClusterJobs.
- Nur `Kind::Probe` darf retransmittiert werden: nach 3 und 6 Sekunden, insgesamt höchstens drei Sendeversuche mit unveränderter Sequenz/Job-ID.
- Ein laufender Retry wird bei Abschluss, Timeout, Epoch-/Rollenwechsel, Lease-Verlust oder geänderter Ziel-Boot-ID nicht erneut versendet.
- `SFJP1` verarbeitet wiederholte **identische** Sequenzen erneut und sendet eine neue `SFJA1`-Antwort. Sicherheitsvalidierung unverändert. `SFJR1` (Reboot) bleibt strikt einmalig.
- Diagnose: `/cluster_status` liefert `job_probe_diagnostics.retransmissions` als Anzahl zusätzlicher Sendeversuche (nicht bestätigter Empfang).
- Grenzen: UDP-Zustellung nicht garantiert; ACK-Verlust und fehlender Auftrag lassen sich ohne Diagnosen des Zielgeräts nicht zuverlässig unterscheiden.


### Beta79 Status-Frontend-Korrektur (2026-10-09)

Der Coordinator-Status-Cache liefert ein bereits geparstes JSON-Objekt (kein `Response`). Vier Consumer in der Coordinator-Seite müssen `.then(render...)` direkt verwenden. Beta78 verursachte andernfalls JavaScript-Ausnahmen (`r.json is not a function`) und irreführende UI-Ausfälle. Beta79 korrigiert ausschließlich diese Consumer. Hardware-Verifizierung noch ausstehend.

### Beta 80: Fernneustart diagnostizieren
- `job_restart_diagnostics` im lokalen `/cluster_status`: `sent`, `received`, `accepted`, `rejected`, `last_reject`, `ack_received`, `ack_accepted`, `ack_rejected`, `last_ack_reject`.
- Auf Coordinator: ACK-Zähler zeigen Eingang und Ablehnung; auf Ziel-Node: Empfang, Ausführungsplanung, Sendefehler/Ablehnung. Zähler sind RAM-lokal und resetten beim Boot.
- Negative Restart-ACKs werden nicht neu eingeführt; verweigerte Ausführung bleibt ohne ACK und kann mit lokalen Zählern untersucht werden.
- Auf devxiao1 vor Test `Cluster` öffnen, nach dem Test lokale Neustart-Diagnose ablesen. Bestätigter Job-ACK bedeutet weiterhin nur geplante Ausführung.
- SD-Bootlogs bei absichtlich leerem devxiao2-Kartenslot sind für Cluster-Jobfehler nicht beweiskräftig.

## Beta 83 – Einzelner Remote-Shutdown (Testbetrieb)

- `SFJS1` und `SFJSA1` verwenden das bestehende HMAC-signierte Cluster-Unicast und dieselben Rollen-/Lease-/Epoch-/Boot-/Replay-Kriterien wie der Remote-Restart.
- Ein Node quittiert erst nach erfolgreicher Planung über `webConfigScheduleShutdown(5000)`. Dies schützt offene Aufnahmen und sperrt neue Starts bis zum Power-Down.
- Timeout 15 Sekunden für fehlendes ACK; **nie automatische Retries** für Shutdown. Gültiges ACK bleibt als `Accepted` erhalten.
- Die Board-Abschaltung ist der vorhandene manuelle Deep-Sleep ohne Wake-Quellen. Nach dem Abschalten ist keine automatische Rückkehr zu erwarten; RESET oder Power-Cycle erforderlich.
- Coordinator-UI erlaubt ausschließlich Einzelaktionen nach Bestätigung, zeigt Job-ID/ACK-Zeit und erklärt, dass ACK weder Ausführung noch Stromverlust beweist.
- Probe-ACK-Zeit erscheint separat als `ack_latency_ms` für erfolgreiche Probe-Jobs.
- Testplan: Probe-Aufträge; Shutdown nur eines ersetzbaren Nodes ohne laufende Aufnahme; ACK prüfen; offline nach Lease; Power-Cycle; Join prüfen. Gegenprobe bei laufender Aufnahme (muss gesperrt werden). ESP32-Compile und Hardwaretest offen.

## Beta84 – Node-Auswahl und Serienausführung

Die Coordinator-Node-Tabelle hält im Browser eine Auswahl nur für entfernte Nodes. Select all / deselect all verändern keine Firmware-Konfiguration. Die Auswahl wird beim Statusrefresh beibehalten, nicht mehr sichtbare Geräte werden entfernt. Reboot/Shutdown/Probe werden über die vorhandenen Administrator-POSTs individuell nacheinander abgesetzt; damit bleiben signierte Job-IDs, Boot/Epoch/Recording-Schranken und das 16-Slot-RAM-Limit gültig. Keine automatische Wiederholung, keine Rollback-Semantik, kein garantiertes Ausführen nach HTTP-202/303. Für große Cluster kann die Job-Tabelle frühere Einträge verdrängen: Auswertung pro Ziel, vor allem bei Shutdown, bleibt Pflicht.

Gezielter Zeit-Sync verwendet identisches authentifiziertes SFTS1-Paket und sendet Unicast pro Ziel (keine Änderungen am Empfänger). Die alte manuelle Broadcast-Schaltfläche ist weiterhin vorhanden. Negative ACKs/Recording-Ablehnungsgründe sind als separate Protokollerweiterung offen; keinen verlorenen UDP-Befehl als Aufnahme-Sperre interpretieren.


### Beta 85: signierte Ablehnung und zentrale Tabellenansicht
- `SFJX1|tag|coordinator|coord_boot|epoch|node|node_boot|seq|kind|reason` ist ein signiertes Unicast-Refusal. Es wird **ausschließlich** gesendet, wenn ein vollständig authentifizierter und als neu erkannter Restart-/Shutdown-Befehl die lokale Recording-/Safety-Schedule-Prüfung nicht besteht.
- Auf dem Coordinator gilt Refusal nur für einen noch `Sent`-Job von exakt demselben Node/Boot/Coordinator/Epoch/Kind und Quell-IP. Das Refusal ist endgültig; keine Retries und kein automatischer Zustandswechsel von `Accepted` nach `Failed`.
- Reason Code `2` bedeutet *Recording oder sonstige lokale Sicherheitsbedingung*; keine Behauptung einer genaueren Ursache als durch die bestehenden Schedule-APIs nachgewiesen.
- Gerätetabelle liest `release` aus dem bestehenden signierten Presence-Peer-Status. IP-Adressen sind vor Links auf die einzelne Node-WebConfig streng als IPv4 geprüft, keine frei kontrollierbaren Hosts im Link.
- `manual_sync_sequence` ist der **Broadcast**-Sync-Zähler. Die gezielten Unicast-Sync-Aufträge besitzen in Beta 85 noch keinen eigenen aggregierten Ausführungsnachweis. Die Oberfläche darf diese Befehle nicht als bestätigten Broadcast darstellen.
- Offene Verifikation: Arduino ESP32-S3 Kompilation, Hardwaretest einer bewusst blockierten Restart-/Shutdown-Annahme (z.B. laufendes Recording), 2-Node-Job-Korrelation, Mehrfachauswahl, Display auf Browsern mit wechselnder Node-Liste.

### Beta 86 – HTTP-Fehlerbilder und Neustart-/Absturzdiagnose
- Auf dem Coordinator im Browser ist `/cluster_status` ein GET, pro ausgewähltem Node ein separater POST zu `/cluster_job_probe`, `/cluster_node_restart`, `/cluster_node_shutdown` oder `/cluster_node_sync_time`. Die Probe und der manuelle Sync benutzen unterschiedliche Transportpfade.
- Die Sammelaktion pausiert neue Status-Polls, damit der ESP32-WebServer den POST unter geringerem Browser-Konkurrenzdruck bearbeiten kann. Pro POST werden HTTP-Status bzw. Timeout je Node angezeigt. Achtung: bei HTTP-Timeout kann der Befehl bereits am Gerät angekommen sein. **Nie automatisch Restart/Shutdown erneut senden.**
- POST 12-s-Browserlimit; Status-GET 8-s-Browserlimit. Diese Grenzen sind reine UI-Wartezeiten, keine UDP-ACK-Zeiten.
- Die Beta-85-Beobachtung '42 HTTP-Fehler, 0 Probe-Jobs, unicast Sync 0 von 1 HTTP-Annahmen' weist auf HTTP-Transport-/Server-Belastung; sie beweist weder einen defekten UDP-Transport noch einen Firmware-Absturz. Resetursache nur aus neuem Serial-Bootlog inkl. ESP-Resetgrund und ggf. Watchdog-/Backtrace zu klären.
- SD-Mount-Fehler an devxiao2 sind bei absichtlich leerem Slot erwartbar.

### Beta 87: selected-node time sync and pending SD administration
- `clusterRequestNodeTimeSync` sends the existing signed unicast `SFTS1`, and, after successful send, records the command sequence and request timestamp in the authenticated peer entry. These fields survive presence refresh only for the same peer boot.
- The existing signed `SFTR1` report carries the received command sequence. `cluster_status` exposes `selected_sync_seq` and `selected_sync_age_ms`; the UI displays `Gezielter Sync bestätigt` only when `time_report_valid` and the signed report sequence equals the requested sequence.
- This is per-node report correlation, NOT proof of precise time synchronization. RTT/2 remains only a heuristic and reports are not a precision bound. No system clock or recorder behavior changed.
- Selected requests and broadcast requests use the same monotonic coordinator sequence counter. The UI keeps broadcast and selected statuses distinguishable.
- SD deletion safety gate: current maintenance WIPE is NOT a recordings-only deletion: it traverses the entire card and rebuilds config. Do not wire that path to remote selected-node bulk actions. Implement a dedicated incremental recordings-only cleanup with explicit media path allowlist, open-recording and active-storage checks, crash-safe progress, durable deduplication, HMAC job validation, and independently acknowledged per-node results before enabling remote SD deletion.


### Beta 88 – SD-unabhängiger Coordinator
Ein Node darf im Modus „SD optional“ ohne periodisches Blockieren durch Mount-Recovery laufen. Die Einstellung liegt in NVS, damit sie schon vor dem Config-/SD-Boot-Abschnitt gelesen wird; bei jedem Boot erfolgt genau ein SD-Mount-Versuch. Bei Erfolg wird sie persistent zurückgesetzt. Die Netz-/Cluster-Semantik wurde nicht geändert.
**Wiedereinstieg/Test:** SD-Wartung von devxiao2 öffnen, Option aktivieren, neu starten; Log auf nur einen Boot-Mount und keine Recovery pro Minute prüfen. Danach SD einsetzen und neu starten: Mount muss erfolgreich sein, Schalter automatisch auf AUS. Ohne SD keine Recording-Funktion.


### Beta 90: SD-Bereitschaftsprüfung für ausgewählte Nodes

Der Coordinator bewertet die bereits signiert übermittelten SD-Gesundheitsinformationen und den Recording-Status nur lesend. Die Vorprüfung besitzt **keinerlei Lösch- oder Formatierungswirkung** und kann keine vollständige Dateisystemprüfung ersetzen. Sie ist keine Freigabe für einen späteren Löschauftrag.

Vor produktiver SD-Löschung muss ein dedizierter Node-lokaler Executor alle Aufnahmedatei- und Sidecar-Typen zuverlässig klassifizieren, offene Player-/Recorder-/Storage-Handles ausschließen, neue Aufnahmen atomar sperren, Löschvorgänge in kurzen, watchdog-freundlichen Etappen bearbeiten und terminale Ergebnisse signiert zurückmelden. Der Coordinator benötigt Nonce/Epoch/Boot/Job-ID und Schutz vor Replays/Mehrfachausführung sowie explizite Bestätigung für irreversible Aktionen. SD-Konfiguration und Systemdaten dürfen nie über die zentrale Medienbereinigung entfernt werden.


## Beta 91 – Signierter Cluster-SD-Wipe
- `SFJW1` ist ein auf Ziel-Boot-ID, Coordinator-Boot-ID und Epoch gebundener HMAC-signierter Unicast, kein Broadcast.
- `webSdQueueClusterWipe()` reserviert nur den Auftrag und blockiert neue Aufnahmen. `webSdMaintenanceLoop()` ruft `stopRecording()` auf und nutzt den existierenden kompletten Wipe mit internem Config-Backup/Restore.
- `SFJWR1` berichtet signiert ausschließlich nach Abschluss an den Coordinator; er verwechselt HTTP-Annahme nicht mit der tatsächlichen Dateilöschung.
- Kein Retransmit. Ein nicht bestätigter Wipe muss vor jeglichem erneuten Versuch lokal geprüft werden. Der Auftrag ist RAM-only; verlorene Abschlussberichte werden nicht nachgeliefert.
- SD-Wipe löscht gewöhnliche Dateien, **keine** garantiert forensische physische Medienvernichtung.
- Bei Wipe-Dauer über 120 s, Netzwerkverlust oder Änderung der Peer-Lease kann der Coordinator ein nicht eindeutig bestätigtes Ergebnis zeigen, selbst wenn die SD bereits gelöscht wurde.

## Beta 93 preliminary capture contract
`SFJC1` authenticated command: cluster tag, coordinator ID/boot/epoch, target ID/boot, sequence, UTC microseconds. One shared UTC timestamp must be submitted by Coordinator for the full selection. Local cluster time estimate is the single timebase, whatever its discipline method.
`SFJCR1` signed status: accepted(1), JPEG stored(2), refused/failed(3), error, bytes. JPEG stays in a volatile 3-slot PSRAM ring. Slots are overwritten cyclically and do not survive power loss. Transfer is intentionally not yet implemented.
This is a first functional lab prototype: it schedules the driver call from the main loop. **No exposure-start measurement and no guaranteed jitter.** Streaming, ongoing recording and Sync API exclusive use are rejected rather than preempted. A later camera-owner integration and precise scheduler is necessary before multiview production capture.

Beta93 main-loop prearm: initialize/wake camera approximately 2 s before target, then prioritize capture in cooperative firmware loop. Timing remains unverified; frame delivery may not equal exposure start.

## v87-beta95: Drone readiness
`drone_mode.h/.cpp` owns one NVS boolean (`sf_drone/enabled`) independent of `operating_mode` and shooter configuration. `tryEnterConfiguredSleep()` returns early when Drone Mode is active. The explicit thermal shutdown and SD-fault protection code remains separate.
Coordinator sends signed `SFJD1` targeted unicast including coordinator boot/epoch, node boot, sequence, desired value. Node verifies active authenticated coordinator, lease, target identity and anti-replay sequence, persists the bit, and returns signed `SFJDA1` with actual value and error code. Coordinator validates the reply against peer boot/IP and pending job before recording terminal state. No automatic resend.
Capture still uses a single UTC timebase. Drone Mode does not itself reserve exclusive camera ownership or guarantee exposure timing; sensor prewarm and safe streamer/recorder preemption are future work.

## v87-beta96: Passive Drone standby semantics

The persistent Drone flag is tested directly after `clusterLoop()` and the armed-capture early return in `sensorforge.ino::loop()`. If active, an ongoing recording is closed through `stopRecording()` once; the loop then services thermal safety, WebConfig and nonblocking WireGuard before returning. This avoids normal autonomous radar/PIR/motion, Power Shooter, background SD retry and recording start decision paths. The signed scheduled UTC capture remains in clusterLoop, and normal modes resume automatically when the flag is cleared. Drone is a policy overlay, not a new timebase or camera owner.

Important limits: shutdown/reboot/web maintenance and explicitly requested operations still work; WebConfig may itself carry out explicit SD operations. Boot-time SD setup is not altered. A camera that is held by an explicit streaming/preview request is not forcibly preempted; scheduled shots can still be rejected by camera-owner checks. Thermal emergencies have priority over readiness.


### v87-beta97: einheitliche Drone-Moduswahl

Drone ist nun im regulären Konfigurationsformular unter Betriebsmodus auswählbar. `droneModeSet()` ist die persistente gemeinsame Zustandsquelle für lokalen UI-Speicherpfad und authentifizierten Coordinator-Auftrag. Die früher separate lokale Cluster-Checkbox wird nicht mehr gerendert (der alte HTTP-Endpunkt bleibt für Kompatibilität bestehen). Autonome Aufnahme- und Sensorroutinen werden wie in beta96 durch die bestehende Drone-Gate-Logik pausiert. Recording-/Shooter-Präferenzen werden bei Auswahl von Drone nicht überschrieben. Der Wechsel zum normalen Betriebsmodus deaktiviert den Drone-Zustand. Ein Wechsel von Streamer zu normalem Kamera-Owner erfolgt weiterhin über den bestehenden verzögerten Reboot.


### Beta 102: RAM-Medienauslieferung
Capture-Jobs bleiben an die einzige UTC-Zeitbasis gekoppelt. Der Node leert bei akzeptiertem Auftrag den vorhandenen PSRAM-Ring; der Ring überschreibt innerhalb des Auftrags niemals Bilder. Ein späterer Sequenzexecutor muss vor jedem Bild die freie Slot- und Bytekapazität prüfen und bei Erschöpfung die restlichen Bilder auslassen. HTTP JPEG-GET wird auf demselben Node bedient, durch den bestehenden Web-Authentifizierungsmechanismus geschützt; die UUID ist keine Berechtigung. Der Coordinator speichert nur UUID/Software-Dispatch/Bytezahl in seiner begrenzten RAM-Job-Tabelle, zusammen mit dem zuletzt bekannten IP-Link. Die Linkgültigkeit endet mit dem nächsten akzeptierten Capture-Auftrag oder einem Node-Neustart.


### Capture-Uhr auf Coordinator-Webseite (beta104)
- Der Coordinator-Webclient liest für Zeitplanung die gültige `cluster_time.utc_us` aus `/cluster_status` statt `Date.now()` vom Computer. Gilt für relative Zeiten und die Prüfung fixer UTC-Zeiten. Bei fehlender/ungültiger Clusterzeit wird kein Auftrag verschickt.
- Der Client verankert diese UTC-Zeit anhand der Mitte der HTTP-Laufzeit an `performance.now()`; der Countdown läuft monoton ohne Abhängigkeit von späteren PC-Uhränderungen. HTTP-Transport und ungewisse Abfragephase begrenzen die Anzeigepräzision; dies ist **nur eine Näherung der Coordinator-Clusterzeit**, kein gemessener Auslöse- oder Belichtungszeitpunkt.
- Der Countdown startet bei der ersten erfolgreichen HTTP-Auftragsannahme; Grün markiert weiter nur den Soll-Termin. Der signierte Medienabschlussbericht ist davon unabhängig.
- Kein Wechsel der Capture-Protokollfelder; Node-Firmware und Kamera werden nicht geändert. Auf Coordinator beta104 installieren; Node kann zunächst beta103 bleiben.
- 5 eingebettete JS-Blöcke mit Node.js `--check` geprüft, ZIP-Integrität geprüft. Kein vollständiger Arduino/ESP32-Compile und kein Hardwaretest.


## Beta 105: Capture-Zeitanker nach Auftragseingang eingefroren

**Verbindliche Regel:** Ein gültiger und akzeptierter signierter Capture-Auftrag (`SFJC1`) besitzt einen absoluten Soll-Termin in UTC-Mikrosekunden. Der Node liest dazu genau einmal seine aktuelle Cluster-UTC-Schätzung sowie `esp_timer_get_time()` und bildet `deadlineMonoUs = receivedMonoUs + (targetUtcUs - receivedClusterUtcUs)`. Danach verwendet die lokale Terminsteuerung bis zum Shot **ausschließlich** `deadlineMonoUs`. Weder ein neuer NTP-Wert noch eine geänderte Clusterzeit darf eine bereits angenommene Deadline nachführen. Gültige Folgeaufträge verwenden wieder die zu ihrem jeweiligen Annahmezeitpunkt beste Schätzung.

Die Kamera wird höchstens zwei Sekunden vor der monotonic Deadline vorbereitet. Beim Software-Dispatch berechnet sie den berichteten UTC-Zeitstempel aus der **eingefrorenen** Abbildung `anchorUtcUs + (esp_timer_get_time() - anchorMonoUs)`; Abweichung zur geplanten UTC wird ebenso aus dieser Skala berechnet. Dieser Zeitstempel ist **kein Sensorshutter-/VSYNC-Nachweis**. Die gemeinsame Auslösung hängt von der Synchronisationsqualität beim jeweiligen Auftragseingang, Drift des monotonen Timers und Kamera-Latenz ab. Synchronisationsverbesserungen innerhalb eines bereits laufenden Jobs dürfen dessen Deadline nicht verändern. Eine fallende Netzwerkverbindung nach Annahme soll den lokalen Timer nicht verschieben; die Ergebniszustellung kann jedoch fehlschlagen.

Geltungsbereich: Capture-Job (PHOTO), nicht globale Sperre von NTP oder der ESP32-Systemuhr. Bei Node-Neustart gehen RAM-Deadline und PSRAM-Medien verloren. Die aktuelle Kamera-Pipeline synchronisiert den **Software-Dispatch**, nicht nachweislich den Belichtungsbeginn. Maximal 30 Minuten Planungsvorlauf (zuvor 120 Sekunden); Job-Timeout 30 Minuten plus 30 Sekunden, keine automatischen Wiederholungen. Weitere Aktionen (Sequenz, Video, GPIO) müssen später denselben unveränderlichen Deadline-Vertrag übernehmen.


## Auditpaket 2 (v87-beta105): terminale Capture-Ergebnissicherung

- `SFJCR1` bleibt unverändert und signiert. Node wiederholt ausschließlich terminale Resultate (Erfolg/Fehler) für denselben Job nach 2 s bis maximal 10 Sendungen. Es wird keine `SFJC1`-Capture-Anweisung wiederholt, keine neue Deadline gerechnet und niemals eine zweite Kameraauslösung gestartet.
- Neuer signierter Receipt `SFJCA1|tag|coordinator_id|coordinator_boot_hex|epoch|node_boot_hex|seq|status`, als bestehender HMAC-Wire-Frame übertragen. Der Coordinator quittiert nur, wenn das terminal gespeicherte Jobresultat zum Status und zu sämtlichen Erfolg-Medienfeldern bzw. zum Fehlercode passt; Duplikate werden ohne Statusmutation quittiert. Der Node authentifiziert Coordinator, Peer-Lease, IP, Boot, Epoch, lokale Job-Sequenz und Status.
- RAM-only: ein unbestätigter Abschluss pro Node, maximal 10 Sendungen. Jobannahme, Lifecycle-Stopp und Coordinator-/Epoch-Wechsel invalidieren diese volatile Historie. Keine Persistenz, keine garantierte Zustellung bei längerem Ausfall. Zeitgrenze des Jobs unverändert 30 Minuten + 30 Sekunden.
- Annahme-Ablehnungen (`status=3` ohne angenommene Capture) bleiben best-effort, da keine mehrfachen abgelehnten Aufträge den RAM-Medienabschluss eines früheren Jobs verdrängen sollen. Für garantierte Nachverfolgung ist künftig ein kleiner mehrteiliger Result-Cache nötig.
- Diagnosefelder in `capture_diagnostics`: `result_retry_pending`, `result_retry_attempts`, `result_retransmissions`, `result_receipts`, `result_retry_exhausted`. Hardware/Compiler noch nicht vollständig verifiziert.

### Audit-Paket 4 – Recording/PSRAM Task-Lifecycle
Der separate Recording-SD-Drain-Task wartet vor der Nutzung des Besitzerobjekts auf ein explizites Startup-Signal. Der erzeugende Task speichert zuvor das Task-Handle und gibt den Drain-Task anschließend frei. Die Cluster-Zeitbasis, Capture-PSRAM-Jobregeln und Capture-Aufträge bleiben unverändert. Die endgültige Task-Löschung und die SD-Wartung unter Last sind weiterhin auf Hardware zu überprüfen.


### Audit-Paket 5b – SD-Wartungsreservierung (2026-10-11)
- `web_sd_maintenance.cpp`: Ein angenommener Cluster-Wipe reserviert den SD-Wartungspfad nun auch gegenüber lokalem Wipe/Format, Secure-Erase und SD-Benchmark. Zuvor sperrte `g_recordingStartBlocked` lediglich neue Aufnahmen, aber nicht die lokalen Wartungsstarts.
- Die Cluster-Ausführung räumt `g_clusterWipe.active` vor dem eigenen `performSdMaintenance(SD_MAINT_WIPE)` ab; dadurch blockiert die zusätzliche Admission-Sperre nicht den reservierten Auftrag.
- Statische Prüfung durchgeführt; ein vollständiger ESP32-Build und Hardwaretests sind weiterhin ausständig. Allgemeine Parallelzugriffe, Recovery-Operationen, Power-Shooter-Zustände und PSRAM-Lastverhalten bleiben Prüfgegenstand.


### Audit-Paket 5d – Wartungs-Busy-Zustand
Während einer reservierten Cluster-Wipe-Ausführung wird `g_clusterWipe.active` vor dem eigentlichen Wipe zurückgesetzt. `g_storageLocked` hält während der destruktiven Operation die SD-Sperre; `webSdMaintenanceBusy()` berücksichtigt nun beide Zustände. SD-Benchmark verweigert frühzeitige Zugriffe auf SD-Metadaten bei aktiver Wartung. Dies ist keine vollständige Hardware-Nebenläufigkeitsgarantie.

### Audit 5e – Cluster-Wipe und Power-Shooter-Flush
Auch der interne Sparse-MKV-Flush prüft nun `g_storageLocked || g_recordingStartBlocked` vor der Speicherplatz-/SD-Vorbereitung. Das verhindert einen bislang ungeschützten Eintrittspfad während reservierter Cluster-Wipes. Keine Änderung der Cluster-UTC, Auftragsdeadlines oder Signaturprotokolle. Statisch geprüft, Hardwaretest offen.

### Audit 5f: Reservierung des Cluster-SD-Wipe
Der angenommene Cluster-Wipe behält `g_clusterWipe.active` bis nach Recorder-Stopp und `performSdMaintenance` bei. Ausschließlich der interne Wipe-Aufruf kann die Reservierung durch `admittedClusterWipe=true` passieren; normale lokale Wartung bleibt blockiert. Die zusätzliche `g_storageLocked`-Sperre wird während der eigentlichen Wartung gesetzt. Keine Änderung am signierten Cluster-Protokoll. Der Zustand wurde statisch untersucht, nicht auf Hardware validiert.


### Stabilisierung Paket 5g – SD-Sperre während Mikrofon-Test
Der lokale Mikrofon-Diagnosetest verweigert Starts während einer reservierten Cluster-Wipe-Operation. Beim Testende bleibt die Storage-Sperre gesetzt, falls die Cluster-Reservierung inzwischen aktiv geworden ist. Die verteilten `volatile bool`-Sperren sind weiterhin kein atomarer Reservierungsmechanismus und benötigen eine gesonderte Nebenläufigkeitsprüfung.

### Stabilisierung 5h – Wipe-Admission
Die Node-Annahme eines Cluster-SD-Wipes erfordert neben freier SD und fehlender Wartungsreservierung nun auch eine offene `g_recordingStartBlocked`-Sperre. Während einer bestehenden Recording-Startsperre wird kein neuer Cluster-Wipe angenommen. Dies ist ein Admission-Guard und ersetzt **keinen** gemeinsamen atomaren SD-Ressourcen-Lock.


### Paket 5i: Der Sync-API-SD-SPI-Taktwechsel respektiert neben g_storageLocked auch g_recordingStartBlocked bei Exclusive-Lease-Ende und Umschaltung. Verteilte Flag-Pruefungen bleiben kooperativ und sind kein atomarer Lock.

### Paket 5m: Cluster-Wipe-Lease (teilweise Migration)
Der Cluster-Wipe reserviert beim Annehmen zusätzlich ein atomisches, ownergebundenes Ticket (`SdAdmissionController::ClusterWipe`), das nach Recorder-Stopp und Wipe wieder freigegeben wird. Bestehende Legacy-Gates bleiben aktiv. Diese Teilmigration schützt **noch nicht** vor anderen SD-Nutzern, die den Controller nicht verwenden; die SD-Gesamtmigration und Hardwarevalidierung bleiben erforderlich.


### SD-Admission – Paket 5n
Neben dem Cluster-Wipe nutzt nun der Secure-Erase-Job die gleiche atomare Admission-Lane. Die Lease wird vor destruktiven Schritten nach erfolgreicher Vorbereitung erworben und nach finaler Bereinigung freigegeben. Legacy-Flags bleiben für nicht migrierte Clients in Kraft; die Admission-Lane ist noch **kein globaler SD-Dateizugriffsmutex**.

## SD-Admission Migration Paket 5o
Lokaler SD-Wipe/Format und der SD-Benchmark reservieren die gemeinsame Admission-Lane mit bereichsgebundenen Tickets. Cluster-Wipe verwendet seine bereits bei Auftragsannahme erteilte Lease. Die bisherigen kooperativen Sperrflags bleiben aktiv. Ein solcher Admission-Mechanismus ersetzt noch keine vollständige Synchronisation aller bestehenden Datei- und SD-Bus-Nutzer.

### Audit-Paket 5q – Recorder-Start und SD-Lease (2026-10-11)
- `recorder.cpp`: `recorderStart()` verweigert neue Aufnahmen auch bei aktiver atomarer SD-Wartungslease (`webSdAdmissionController().busy()`). Vorher wurden nur die kooperativen Flags geprüft; die Reihenfolge Lease-vor-Flag ließ ein kurzes Zulassungsfenster.
- Diese Ergänzung ist **Defense-in-Depth**, kein vollständiger Ausschluss laufender Recorder-I/O: der Start prüft die Lease bislang nur einmal vor den Dateisystemzugriffen. Eine atomare Recorder-Besitzerlease bzw. Drain-/Finalize-Quiescence bleibt P1.
- Keine Änderungen an bestehendem Aufnahmeformat, Capture-Timing oder Power Shooter. Nur statische Strukturprüfung; ESP32-Compile und Hardwaretests offen.

### Audit-Paket 5r – atomarer Recorder-Dateistart (2026-10-11)
- `recorderStart()` reserviert jetzt vor jeglichem Dateisystemzugriff eine kurzlebige `RecorderStart`-Lease und hält sie bis zum Ende des Startpfads, auch bei Fehlern. Dadurch kann keine bereits migrierte Wartung/Remount-Operation zwischen Admission-Prüfung und Dateiöffnung reservieren.
- Die bisherigen Flags werden nach Erwerb der Lease erneut geprüft. Bereits laufende Aufnahmen und ihre Schreib-/Finalize-Pfade werden **noch nicht** durch dieselbe Lease geschützt. Die Änderung ist bewusst auf den Start begrenzt; ein atomarer Schutz der gesamten I/O-Lebensdauer und die Migration weiterer SD-Nutzer bleiben P1.
- Host-C++-Test des Admission-Controllers; vollständiger ESP32-Compile und Hardwaretests offen.

### Audit 5s – SD-Übergabe
Eine Cluster-Wipe-Reservierung muss trotz laufender Aufnahme vorab möglich bleiben. Vor tatsächlicher SD-Wartung sind Recorder-Stopp, asynchroner Drain-Abschluss und geschlossene Dateihandles nachzuweisen. Der bisherige kurze RecorderStart-Lease genügt dafür nicht. Siehe `SD_RECORDER_HANDOFF_AUDIT.md`; noch nicht implementiert.
