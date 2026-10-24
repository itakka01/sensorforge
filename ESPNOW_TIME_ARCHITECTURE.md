# SensorForge / exapan C1 – ESP-NOW-Zeitsynchronisation

**Stand:** 2026-10-24, `v87-beta147` (Zeitsignal-Verfahren, Abschnitt 0). Die Abschnitte 4–6 und 12 beschreiben in Teilen das bis beta146 verwendete Verfahren; wo sie Abschnitt 0 widersprechen, gilt Abschnitt 0.

**Validierungsstatus:** Der integrierte Code ist **noch nicht** durch einen vollständigen Arduino-ESP32-Build beider Boards und reale Funk-/GPIO-Tests bestätigt. Aussagen unten über Programmverhalten beschreiben den beabsichtigten und implementierten Codepfad, **keinen nachgewiesenen Genauigkeitswert**.

## 0. Verfahren ab v87-beta147: Zeitsignal + seltene Laufzeit-Einmessung

**Warum:** Bis beta146 bekam jeder Node seine Präzisionszeit nur aus eigenen Hin-und-zurück-Serien (Probe/Reply/Final), immer ein Node nach dem anderen (~3,5 s je Node), gültig 120 s. Das reicht lückenlos für ~24 Nodes; bei 100 Nodes wären die Nodes nur ~32 % der Zeit präzise. Ziel: 100 Nodes, Cluster in wenigen Minuten (praktisch: Sekunden) synchron.

**Messung vorab (beta146, 9 Nodes, 128 Zeitsignale im 100-ms-Takt):** Zwei Nodes empfangen dasselbe Signal typisch 4–40 µs auseinander (Median aller Paare 17,5 µs). 5–15 % der Signale kommen an einem einzelnen Node 100–300 µs zu spät, nie zu früh. Zwischen Zeitstempel und tatsächlichem Senden am Coordinator lagen gelegentlich bis 2,2 ms, dann für alle Nodes gleich. Quarzabweichungen 4–36 ppm.

**Ablauf**

1. **Zeitsignal (Coordinator, `sendBeacon()`):** jede Sekunde ein signiertes `Beacon` (Typ 1) an alle. Es trägt selbst keine Zeit, nur den Cluster-Mittelwert der Funklaufzeit (`auxUs`, ns; unbekannt = INT64_MIN). Zeitstempel unmittelbar **vor** `esp_now_send()`, also nach der HMAC-Berechnung.
2. **Folgepaket (`serviceFollowUp()`):** sobald der TX-Done-Callback des Beacons da ist (höchstens 30 ms Wartezeit) ein `FollowUp` (Typ 12): `nonce` = Sequenz des Beacons, `timeUs` = Zeitstempel (Coordinator-Monotonzeit), `auxUs` = Coordinator-UTC zu diesem Zeitstempel, `preSendDelayUs` = Sendeweg Zeitstempel → TX-Done (µs, `UINT32_MAX` = unbekannt).
3. **Node-Modell (`cluster_beacon_sync.*`, reine Logik):** je Signal ein Messpunkt `y = Zeitstempel − Empfangszeit`. Alle Störungen sind Verspätungen, daher sind die **größten** `y` die sauberen. Gleitendes Fenster der letzten 64 Punkte (≤ 120 s); Gerade (Versatz + Gangabweichung) durch die obere Hälfte, dreimal nachgeschärft („obere Hüllkurve“). Verworfen werden Punkte, deren Sendeweg mehr als 150 µs über dem kürzesten der letzten 32 lag (Coordinator hat spät gesendet), und Punkte mehr als 150 µs über der Hüllkurve (physikalisch unmöglich; 5 in Folge = Modell neu aufbauen). Gültig ab 8 Punkten über ≥ 4 s, Streuung (RMS der oberen Hälfte) ≤ 150 µs, Gangabweichung ≤ 500 ppm.
4. **Funklaufzeit:** Aus Zeitsignalen allein ist die konstante Laufzeit (Luftzeit ~1 ms + Empfangsweg) nicht bestimmbar. Sie wird mit einer Hin-und-zurück-Serie eingemessen: `Laufzeit = Zwei-Wege-Versatz − Hüllkurven-Versatz` (zum Mittelzeitpunkt der Serie). Der Node nutzt den Median seiner letzten 5 Einmessungen (eigen, höchstens 60 min alt), sonst den Cluster-Mittelwert aus dem Beacon (Median der eigenen Laufzeiten aller Nodes, Berichte ≤ 60 s, Einmessung ≤ 60 min). Plausibel: 0–10 ms.
5. **Coordinator-Zeit am Node:** `Coordinator = Node + Hüllkurven-Versatz(Node) + Laufzeit`; UTC über das UTC/Monoton-Paar des neuesten Folgepakets (`precisionUtcNow()`).
6. **Gültigkeit (`precision_state`):** Modell gültig, neuester Messpunkt ≤ 15 s alt (Überbrückung, z. B. Capture-Pause), Laufzeit (eigen oder Cluster) bekannt, UTC vom Coordinator vorhanden. Die 120-s-Gültigkeit einer Serie entfällt.
7. **Einmess-Plan:** Slot-Planung aus 8.1 unverändert, aber fällig erst nach **10 min** (`kSlotRefreshAgeMs`), „abgelaufen“ (Diagnose) nach 30 min. Ein Node meldet sich erst zur Einmessung (`NodeSyncReport`), wenn sein Zeitsignal-Modell mindestens 32 Signale hat (`kMinCalibrationSamples`); eine früher fertige Serie wird bis 120 s lang aufgehoben. Grund: Auf der Hardware lag die erste Einmessung mit nur 8 Signalen bis 150 µs daneben, spätere streuten nur ±30 µs. Erfolg im Sinne des Plans = eigene Laufzeit vorhanden. Bei 100 Nodes (~3,5 s je Einmessung) dauert eine Runde ~6 min < 10 min.
8. **Berichte:** `BeaconSyncReport` (Typ 13) alle 10 s: Modell gültig, Präzision gültig, Zustand, Laufzeitquelle, Punkte, Alter, Streuung, Gangabweichung, eigene Laufzeit + Alter, Streuung der letzten Serie. `NodeSyncReport` (Einmessung) alle 10 s und sofort nach jeder Serie, Serienstatistik alle 30 s.

**Paketformat V3:** 56 Byte Nutzdaten (+8 Byte `auxUs`) + 32 Byte HMAC = 88 Byte, `kVersion` 3. Ältere Firmware wird nicht verstanden (alle Boards neu flashen).

**Entfernt (beta147):** Zeit-Broadcaster mit Grant/Ack/Lease (`cluster_time_grant_protocol.h`, `cluster_time_shadow_handover.h`, `cluster_time_source_policy.h`, 80-Byte-SFDB-Diagnosebeacons `cluster_time_diagnostic_beacon.*`), Beacon-Intervalltest, passive Beacon-Vorhersage/Shared-Events, Drift-Regression über Serien (`cluster_espnow_regression.*`) und NVS-Driftspeicher (`cluster_espnow_drift.*`; die Gangabweichung kommt jetzt laufend aus dem Zeitsignal). Die Betriebsart „Zeit-Broadcaster“ bleibt als Standby-Betriebsart erhalten, verhält sich funkseitig aber wie ein normaler Node.

**Simulation** (`test_beacon_sync.cpp.test`, Störungen wie gemessen, Zwei-Wege-Fehler 25 µs Versatz + 12 µs Rauschen je Einmessung, 1 % Verlust):

| Szenario | Node-zu-Node p50 / p95 / max | alle gültig nach |
|---|---|---|
| 9 Nodes, alle eingemessen | 7 / 19 / 34 µs | 14 s |
| 100 Nodes, alle eingemessen | 9 / 28 / 69 µs | 14 s |
| 100 Nodes, Hälfte nur mit Cluster-Mittel | 12 / 33 / 68 µs | 14 s |
| 9 Nodes, 12 s ohne Zeitsignal | 8 / 22 / 39 µs | 13 s |
| 9 Nodes, Coordinator-Quarz +30 ppm | 7 / 20 / 35 µs | 13 s |

Das sind Software-Zeitstempel in einem Modell; die Abnahme braucht den GPIO-Messaufbau (Abschnitt 13).

**Erster Hardwarelauf (beta147, 9 Nodes, 13 dBm):** alle Nodes `valid`, Streuung des Zeitsignals 8–26 µs, keine unplausibel frühen Signale, je Node 1–2 % Signale wegen spätem Senden verworfen. Eingemessene Laufzeiten nach 2–3 Einmessungen 2041–2127 µs (erste Einmessung mit 8 Signalen bis 2240 µs, behoben durch die 32-Signal-Regel); Cluster-Mittel 2129 µs.

**Diagnose:** `/cluster_status` → `espnow_time_local` (`precision_state`, `beacon_*`, `delay_source`, `own_delay_*`, `cluster_delay_*`, `beacon_tx_path_*`), je Node in `espnow_node_reports` (`precision_valid`, `precision_state`, `beacon_jitter_us`, `drift_ppm`, `own_delay_us`, `delay_source`, `calibrated`, …). Rohdaten der letzten 128 Zeitsignale je Gerät: `GET /cluster_espnow_beacon_trace_csv` (`sequence` ist auf allen Geräten gleich; `est_coord_us` = geschätzte Coordinator-Zeit beim Empfang vor dem Einrechnen des Punkts).

## 1. Ziel und Grenzen

Ziel ist eine gemeinsame, möglichst ruhige **Cluster-UTC-Zeitbasis** für zeitgesteuerte Aufnahmen von Freenove-Coordinator und XIAO-Nodes. Die Systemuhr (`gettimeofday`, SNTP/RTC) wird nicht fortlaufend verstellt. Die bisherige Wi-Fi-Clusterzeit bleibt als Fallback erhalten. ESP-NOW-Zeitstempel sind Software-Zeitstempel; Mikrosekundenauflösung ist **keine nachgewiesene Mikrosekundengenauigkeit**. Auch eine präzise Cluster-Uhr beweist keine gleiche Kamera-Belichtungszeit.

## 2. Module und Zuständigkeiten

| Modul | Zuständigkeit |
|---|---|
| `board_config.h` | `BOARD_HAS_ESP_NOW` pro unterstütztem ESP32-S3-Board |
| `cluster_espnow_packets.*` | Kompaktes Zeitpaketformat, Kodierung/Validierung, Replay-Daten |
| `cluster.cpp`, `cluster.h` | Clusteridentität, Epoch/Boot-ID, HMAC-Key-Ableitung, Anbindung an `clusterTimeNowUs()` |
| `cluster_espnow_radio.*` | ESP-NOW-Initialisierung, Kanalbindung, begrenzte Empfangsqueue, Beacon/Probe/Reply/Final, SyncControl-Freigaben/Widerruf, Qualitätszustand |
| `cluster_espnow_twoway.*` | Vier-Zeitstempel-Offset, RTT, RTT-Quartilsaggregation und Vergleichsschätzer |
| `cluster_beacon_sync.*` | Zeitsignal-Modell (obere Hüllkurve, Versatz + Gangabweichung) und Laufzeit-Einmessung, ab beta147 (Abschnitt 0) |
| `webconfig_cluster.cpp` | Lokale Funkdiagnose; **noch keine authentifizierten Node-Einzelberichte im Coordinator** |

`ESPNOW_*_STATUS.md` und `ESPNOW_*_INTEGRATION.md` dokumentieren historische Zwischenstände. Sie sind nicht als aktuelle Funktionsübersicht zu lesen; dieses Dokument beschreibt die zusammengesetzte Zielarchitektur.

## 3. Start, Voraussetzungen, Funkkanal

ESP-NOW arbeitet auf der bereits verbundenen WLAN-STA und **erzwingt keinen Kanalwechsel**. Coordinator und Nodes benötigen kompatiblen Kanal, gültige Cluster-Mitgliedschaft/Schlüssel, aktiven Cluster und Drone Mode. Bei Reconnect, Kanal-, Coordinator-, Boot-ID- oder Epoch-Wechsel müssen alte Funk-/Zeitreferenzen verworfen und neu aufgebaut werden. Zwei-WLAN-Protokolle teilen denselben Funkchip; die Firmware kann *nicht* sicherstellen, dass fremde APs, andere Geräte oder der WLAN-Treiber im Messfenster keinerlei Verkehr erzeugen. Vermeidbare eigene Last kann reduziert, nicht garantiert ausgeschlossen werden.

## 4. Nachrichten und Sicherheitsmodell

- **Zeitsignal (ab beta147 jede Sekunde, Beacon + Folgepaket):** siehe Abschnitt 0. Bis beta146: Beacon alle 10 s nur als UTC-Anker.
- **Probe/Reply/Final (eine Serie je Mess-Slot des Nodes; fest 16 Austausche je Serie (die frühere adaptive Erweiterung auf 32/64 wurde in beta131 entfernt)):** vier *monotone* Zeitstempel `t1` Node vor Sendung, `t2` Coordinator beim Empfang, `t3` Coordinator vor Antwort, `t4` Node beim Empfang. Die Zeiten der ESP-NOW-API bzw. Callback-Ausführung sind nicht gleich physikalischen HF-Sende-/Empfangszeiten.
- **Authentizität:** HMAC-SHA256 mit separat aus dem laufenden Cluster-Schlüssel abgeleitetem Schlüssel und Replay-/Sequenzprüfung; kein zusätzlicher langlebiger Klartext-Schlüssel. Broadcasts sind **nicht verschlüsselt**. Offenlegung der Zeit ist unkritischer als das Einschleusen einer falschen Zeit; HMAC schützt gegen unautorisierte Änderung, nicht gegen Jamming, Verzögerung oder asymmetrische Funklaufzeiten.
- **Callback-Regel:** Ungeprüfte Eingangsdaten zunächst in eine begrenzte Queue; HMAC/Parsing/Auswertung außerhalb des WLAN-Callbacks. Queue-Überlauf ist als Verlust zu zählen.

Für t1/t2/t3/t4 gilt (Coordinator-Monotonzeit minus Node-Monotonzeit):

`offset_us = ((t2 - t1) + (t3 - t4)) / 2`

`rtt_us = (t4 - t1) - (t3 - t2)`

Diese Schätzung setzt näherungsweise symmetrische Funk-/Stack-Latenzen voraus. Median/RTT-Filter begrenzen Ausreißer, beseitigen aber keinen systematischen Richtungsbias.

## 5. UTC-Anker, Zeitmodell und Fallback

Die Probe-Serie ermittelt zunächst nur die Differenz der **monotonen** Geräteuhren. Für UTC muss der Coordinator zusätzlich eine zu seiner monotonen Uhr gehörende UTC-Angabe liefern. Im aktuellen Paket nutzt ein authentifizierter Beacon `timeUs` als UTC in µs und `nonce` für die niederwertigen 32 Bit des Coordinator-ESP-Timers. Der Node rekonstruiert die monotonen 64 Bit anhand frischer zugehöriger Messungen und verwirft unplausible Paare. Dieses Beacon-Paar ist softwaregestempelt und damit nicht frei von Sendewartezeit.

Das laufende Modell entspricht näherungsweise `coordinator_time = a * local_monotonic_time + b`, mit `a` aus relativem Drift und `b` aus aktueller Präzisionsmessung plus UTC-Anker. Nicht aus gespeichertem NVS-Drift allein UTC ableiten.

**ESP-NOW wird nur bevorzugt**, wenn u. a. gilt:

| Prüfung | Aktueller Grenzwert |
|---|---|
| Akzeptierte Einzelmessungen je Serie | mindestens 8 |
| Offset-Spanne der Serie | höchstens 250 µs |
| Median-RTT | höchstens 30 ms |
| Alter der Präzisionsmessung | höchstens 120 s |
| Alter des UTC-Beacons | höchstens 30 s |
| Identität/Epoch | aktuelle, authentifizierte Coordinator-Referenz |

Bei unzureichender Qualität bleibt/greift die bisherige Wi-Fi-Clusterzeit, sofern gültig; anderenfalls muss die Zeit als nicht verfügbar gelten. `clusterTimeNowUs()` ist der für geplante Aufnahmen relevante Eintrittspunkt. `cluster_time.source` kann `espnow_precision`, `wifi_coordinator` oder `local_coordinator` melden. Quellenwechsel sind im Hardwaretest auf mögliche Zeitstufen zu untersuchen. **`uncertainty_estimate_us=0` im ESP-NOW-Quellbetrieb ist derzeit kein gemessener Fehlerbereich oder eine Null-Unsicherheit!**

## 6. Drift pro Referenz-Coordinator und NVS

- Drift ist **relativ** zwischen Node und einer bestimmten Coordinator-Hardware. Darum nach stabiler Coordinator-ID indizieren, nicht bloß nach IP oder wechselnder Boot-ID.
- Bis zu vier Referenzprofile. NVS enthält Drift (ppm), Schätzqualität, Stichprobenzahl, Kalibrierzeit, Schema und Integritätsdaten; **keinen** dauerhaft gültigen UTC-Offset.
- Erst mehrere zeitlich getrennte, valide Präzisionsserien erlauben einen frischen Drift. Unglaubwürdige Sprünge (im Integrationsmodul z. B. >100 ppm) zurückweisen bzw. nicht unmittelbar übernehmen.
- Reguläre NVS-Speicherung höchstens etwa alle 30 Minuten und nur bei plausibler System-UTC sowie ausreichender Stichprobe. Das reduziert Flash-Verschleiß. Ohne gültige lokale UTC kann die Speicherung ausfallen; der RAM-Drift kann trotzdem weitergeführt werden.
- Nach Reboot ist der persistierte Wert ausschließlich **Prior/Startschätzung** (`StoredDriftOnly`), kein gültiger Offset oder Sync-Nachweis. Erst neue authentifizierte Präzisionsmessungen können den Status verbessern.
- Bei anderem Coordinator ein anderes Profil laden oder von null lernen. Eine neue Boot-ID desselben Coordinators verlangt neue Zeitanker/Sequenzen, nicht zwingend ein anderes Driftprofil.
- Gleichartige Raumtemperaturen erleichtern Stabilität, ersetzen aber keine Messungen; auch der Coordinator-Oszillator driftet.

## 7. Diagnose: nachgewiesener Status statt Etiketten

`espnow_precision` bedeutet nur **gewählte Zeitquelle**, nicht bewiesene Präzisionsgenauigkeit. `espnow_time_local` und Funkzähler (gesendet/empfangen, HMAC abgelehnt, Replay, Queue-Verluste, Probe-Timeouts) sind lokal zu prüfen. **Im Coordinator erscheinen Node-Offset/Drift noch nicht zentral**, da signierte Node-Ergebnisberichte fehlen; dessen lokale Diagnose darf nicht als Node-Messung ausgegeben werden.

## 8. Priorisierung von Aktionen und Sync-Freigabe (aktueller Stand)

**Entwurfsziel:** ESP-NOW bleibt empfangsbereit. Nur *aktive* Präzisionsmessserien (Probe/Reply/Final) werden ausgesetzt, wenn Clusteraktionen oder Captures Vorrang haben. Eine bereits gültige Zeitkalibrierung wird nicht gelöscht: Sie wird für die Terminierung anhand des vorhandenen Driftmodells weitergeführt (Holdover), soweit ihre Gültigkeitsregeln erfüllt bleiben.

**Wer steuert?** Der Coordinator sendet `SyncControl` als HMAC-geschütztes ESP-NOW-Paket (Typ 5). In diesem Paket interpretiert das Protokoll `timeUs` ausdrücklich als **Freigabedauer in Millisekunden**, nicht als UTC-Zeitstempel. `timeUs=0` bedeutet Widerruf.

| Ereignis | Coordinator | Node |
|---|---|---|
| Keine priorisierte Aktion / kein Schutzfenster | Sendet dem aktuellen Slot-Inhaber jede Sekunde eine Freigabe für 3 s (Abschnitt 8.1) | Darf während der gültigen Freigabe neue Probes starten |
| Coordinator-Aktion wird gestartet | Setzt lokale Schutzphase und sendet signierten Widerruf | Bricht Serie **nach Empfang** des gültigen Widerrufs ab |
| Capture wird geplant/angenommen | Lokaler Capture-Schutz und Widerruf; Funkaktivität im Schutzfenster pausiert | Eigener Capture-Schutz stoppt aktive Probes, sobald der Capture-Zustand lokal bekannt ist |
| Widerruf geht verloren | Kein unmittelbarer Stop auf dem entfernten Node garantiert | Die Freigabe verfällt spätestens 3 s nach der letzten gültigen Freigabe |
| Schutzfenster endet | Coordinator kann wieder Freigaben versenden | Misst erst wieder mit neuer gültiger Freigabe |

**Aktuelle Implementierungsgrenzen:** Die lokale Pause bei Listenaktionen ist noch zeitbasiert (typisch 10 s), nicht an den nachgewiesenen Abschluss des Jobs gekoppelt. Die Funkfreigabe ist daher noch **keine umfassende, clusterweit atomare Jobsperre**. Eine sofortige Stilllegung auf allen entfernten Nodes kann ein unzuverlässiger Funkbroadcast nicht gewährleisten. Insbesondere dürfen wir aus einem gesendeten Widerruf keinen bestätigten Empfang ableiten.

Die Funk-RX-Callbacks bleiben kurz; der ESP-NOW-Empfänger kann weiterhin Pakete entgegennehmen, aber ein Node startet ohne gültige Freigabe keine neue Präzisionsserie. Die gespeicherte Drift wird während einer Pause nicht neu gemessen. `syncPermission` und `syncPermissionRemainingMs` in den lokalen Diagnostics zeigen die Node-Freigabe, nicht den Erfolg der Zeitkalibrierung.

**Zeitverhalten:** Zeitbroadcasts ungefähr alle 10 s, Freigaben jede Sekunde an den Slot-Inhaber, 3 s Gültigkeit; Probe-Serien mit fest 16 Austausche je Serie (die frühere adaptive Erweiterung auf 32/64 wurde in beta131 entfernt), mit ungefähr 45 ms Mindestabstand bei erfolgreichem Ablauf, zuzüglich tatsächlicher Funklatenzen/Timeouts. Eine Serie kann deshalb länger dauern. Diese Intervalle sind Parameter und keine Echtzeitgarantien.

### 8.1 Slot-Planung der Präzisionsserien (Coordinator, ab v87-beta141)

**Ab beta147** sind die Serien nur noch Laufzeit-Einmessungen (Abschnitt 0): fällig nach 10 min statt 45 s, „abgelaufen“ nach 30 min statt 120 s, Ergebnis-Timeout 12 s (Berichte alle 10 s und sofort nach der Serie), ein Node probt nach Erfolg frühestens nach 30 s wieder. Die Tabellen unten zeigen das Verhalten des alten Verfahrens (120-s-Gültigkeit).

Der Coordinator beantwortet Probes nur vom jeweiligen **Slot-Inhaber**; immer nur ein Node misst. Die Planung liegt in `cluster_slot_scheduler.h` (reine Logik, Host-Test `test_slot_scheduler.cpp.test`), angebunden in `serviceSlots()` von `cluster_espnow_radio.cpp`. Node-Firmware, Paketformate, Messverfahren (16 Austausche, Qualitätsgrenzen, Schätzer) und die 120-s-Gültigkeit sind unverändert; alte Nodes arbeiten mit einem neuen Coordinator zusammen.

**Problem bis beta140:** feste Reihum-Vergabe, jeder aktive Node 14 s Slot + 3,5 s Pause, also ein Zyklus von N × 17,5 s (9 Nodes: 157,5 s). Eine Serie braucht aber nur rund 2 s (Start etwa 1 s nach der ersten Freigabe, 16 Austausche ≈ 0,8 s). Ab 7 Nodes war der Zyklus länger als die Gültigkeit von 120 s; Nodes fielen trotz erfolgreicher Serien auf `wifi_coordinator` zurück. Zusätzlich fassten Slot-Tabelle und Wiedergabeschutz nur 12 Nodes, und ESP-NOW-Partner (max. 20 in ESP-IDF) wurden nie freigegeben.

**Ablauf eines Slots**

1. Start: Freigabe sofort, danach jede Sekunde (Lease 3 s, unverändert).
2. Ende, sobald zuerst eintritt:
   - **Serie vorbei** – der Inhaber hat geprobt und seit 1,5 s keine Probe mehr gesendet (`kSlotIdleEndMs` > Austausch-Timeout 1,2 s + 45 ms; ein verlorener Austausch beendet den Slot nicht). Keine Pause danach: Der nächste Node probt ohnehin erst ~1 s nach seiner Freigabe, und ein fertiger Node probt frühestens nach 12 s wieder (Retry) bzw. 45 s (Erfolg) – weit jenseits seiner 3-s-Lease. Ein verlorener Widerruf kann also keine zwei gleichzeitig messenden Nodes erzeugen (per `static_assert` abgesichert).
   - **Keine Aktivität** – 3 s ohne Probe (Freigaben verloren, Node ausgelastet): Pause 3,5 s.
   - **Obergrenze** 14 s (unverändert): Pause 3,5 s.
   - **Inhaber verschwunden** (30 s ohne Bericht) oder neuer Boot: Pause 3,5 s.
3. Capture-Pause, Benchmark und Beacon-Experiment brechen den Slot ohne Bewertung ab (wie bisher mit 3,5 s Pause).

**Wer ist der nächste Inhaber?** Nur Nodes, die *fällig* sind: keine Präzision bekannt oder Präzisionsalter ≥ 45 s (gleicher Takt wie die Nachmessung eines Nodes nach Erfolg). Unter den fälligen der dringendste: ohne Präzision zuerst, dann das älteste geschätzte Alter (Alter aus dem letzten `NodeSyncReport` + seither vergangene Zeit), bei Gleichstand wer am längsten auf einen Slot wartet. Ist niemand fällig, wird kein Slot vergeben (weniger Funklast als die alte Dauerrotation).

**Ergebnis und Wiederholung.** Nach einem Slot wird der Node erst wieder gewählt, wenn ein Bericht das Ergebnis zeigt (Berichte alle 5 s; ohne Bericht nach 8 s gilt der Slot als fehlgeschlagen). Erfolg = berichtetes Alter ≤ Zeit seit Slot-Beginn. Erster Fehlschlag: sofort wieder wählbar (eine einzelne verworfene Serie ist häufig). Ab dem zweiten Fehlschlag in Folge Wartezeit 5 s, dann 10, 20, 40, höchstens 60 s – ein dauerhaft scheiternder Node kostet so rund einen Slot pro Minute und hungert die anderen nicht aus. Der frühere „Recovery-Slot“ (`cluster_slot_recovery.h`) ist in dieser Planung aufgegangen und entfällt.

**Kapazität und Annahmen** (Simulation `test_slot_scheduler.cpp.test`, 2 h je Szenario, 1 % Paketverlust, 3 % verworfene Serien; Node-Verhalten wie in der Firmware):

| Nodes | Slot Ø | Präzision gültig (schlechtester Node) | Rückfälle auf WLAN-Zeit | ältestes Alter |
|---|---|---|---|---|
| 4 | 3,6 s | 100 % | 0 | 55 s |
| 9 | 3,6 s | 100 % | 0 | 74 s |
| 16 | 3,5 s | 100 % | 0 | 78 s |
| 24 | 3,6 s | 100 % | 0 | 111 s |
| 28 | 3,5 s | 99,8 % | vereinzelt | 132 s |
| 32 | 3,6 s | 98,4 % | ja (gesättigt, gleichmäßig verteilt) | 140 s |
| 48 | 3,6 s | 67 % | ja (gesättigt, gleichmäßig verteilt) | 224 s |
| 100 (Auslegung ab beta144) | 3,6 s | 32 % | ja (gesättigt, gleichmäßig verteilt) | 396 s |
| 140 (über Tabellengröße 128) | 3,6 s | 128 Nodes eingeplant, 12 gar nicht | ja | 507 s |
| 9 bei 5 % Verlust, 15 % verworfen | 5,0 s | 100 % | 0 | 104 s |
| 16 bei 5 % Verlust, 15 % verworfen | 4,9 s | 96,9 % | ja | 188 s |

Faustregel: dauerhaft lückenlos, solange `Nodes × Slot-Dauer` deutlich unter 120 s bleibt (bei ~3,6 s je Slot bis etwa 24 Nodes). Darüber ist die Funkzeit ausgeschöpft: Alle Nodes werden weiter gleichmäßig nachgemessen (Fairness bleibt, kein Node fällt dauerhaft heraus), aber einzelne laufen zeitweise über 120 s. Ein Slot ist zu etwa 1 s durch den Node-Start nach der Freigabe bestimmt (Node-Firmware, bewusst unverändert) – ein künftiger Node-seitiger Sofortstart wäre der nächste Hebel (~25 % kürzere Slots).

**Harte Grenzen** (ab v87-beta144 für 100 Geräte ausgelegt; alle Tabellen im PSRAM)

| Grenze | Wert | Verhalten darüber |
|---|---|---|
| Cluster-Mitgliederliste (`CLUSTER_MAX_PEERS`, cluster.cpp) | 100 fremde Geräte | **Auslegungsgrenze**: weitere Geräte werden abgewiesen (aktive Mitglieder werden nie verdrängt), `peer_rejected`-Liste und rote Warnung auf `/cluster` und `/cluster_coordinate` |
| Slot-Tabelle, Wiedergabeschutz, Node-Berichte (`kNodeReportCapacity`) | 128 (100 + neu gestartete/verschwundene) | Slot: kein Slot, `slot_table_full_recent`; Wiedergabeschutz: Pakete weiterer MACs verworfen; Berichte: der am längsten nicht gehörte wird ersetzt |
| Jobs (`ClusterJobs::kMaxJobs`) | 256 | zwei Foto-Aufträge an 100 Nodes passen; volle Tabelle aktiver Jobs lehnt neue ab („Job-Liste voll“) |
| Ressourcen / Discovery / Kamera-Übersicht | 256 / 128 / 128 | älteste bzw. abgelaufene Einträge werden ersetzt |
| ESP-NOW-Partner für Antworten | 18 + Broadcast | am längsten unbenutzter Partner wird ersetzt (`unicastPeerEvictions` intern) |
| **Präzisionszeit (Funkzeit)** | ab beta147 unabhängig von der Node-Zahl (Zeitsignal an alle) | Einmess-Runde je Node ~3,5 s; gelbe Warnung, wenn eine Runde länger als 10 min dauert, rot ab 30 min (ab ~500 Nodes). Bis beta146: lückenlos bis ~24 Nodes |

Speicher: Die Tabellen belegen rund 250 KB PSRAM (von 8 MB) und haben ~28 KB statischen internen RAM freigegeben. `/cluster_status` wird in 4-KB-Teilen gesendet (100 Nodes ≈ 200 KB), Kamera- und Gruppenliste ebenso; zwischen den Teilen bedient der Coordinator den ESP-NOW-Funk (`clusterServiceTimeCritical()`), damit Zeitmessungen nicht ausfallen.

**Diagnose** (`/cluster_status` → `espnow_time_local`, nur Coordinator):

| Feld | Bedeutung |
|---|---|
| `slot_active_peers`, `slot_capacity`, `slot_table_full_recent` | aktive Nodes, Tabellengröße, Tabelle zuletzt voll |
| `slot_due_peers` | jetzt fällige Nodes (Warteschlange) |
| `slot_expired_peers` | aktive Nodes ohne gültige Präzision (≥ 120 s oder nie) |
| `slot_oldest_age_ms` | ältestes geschätztes Präzisionsalter |
| `slot_mean_ms`, `slot_last_ms`, `slot_period_mean_ms` | Slot-Dauer Ø/zuletzt, Abstand Slot-Beginn zu Slot-Beginn |
| `slot_cycle_estimate_ms` | aktive Nodes × `slot_period_mean_ms` (Zeit, bis jeder einmal dran war; soll deutlich < 120 000 bleiben) |
| `slot_ended_series_over` / `_no_activity` / `_hard_cap` / `_owner_lost` | Gründe des Slot-Endes |
| `slot_succeeded`, `slot_failed` | Slots mit / ohne akzeptierte Serie |
| `slot_recovery_requested` | = `slot_failed` (Slots, die eine Wiederholung brauchen) |
| `slot_recovery_granted` / `_succeeded` / `_failed` | Wiederholungs-Slots nach Fehlschlag und ihr Ergebnis |
| `slot_recovery_deferred` | Nodes, die gerade in der Fehlschlag-Wartezeit sind |
| `slot_owner_recovery` | laufender Slot ist eine Wiederholung |
| `slot_rotations` | beendete Slots |

**Auf Hardware zu prüfen:** reale Slot-Dauer (Erwartung 3–4 s), `slot_ended_no_activity` und `slot_ended_hard_cap` nahe 0, `slot_cycle_estimate_ms` und `slot_expired_peers` im Langzeittest, keine Verschlechterung der Serienqualität (Streuung, verworfene Serien) durch die dichtere Folge von Messungen verschiedener Nodes.

## 9. Diagnose und Fehlersuche bei einer Messserie

Die folgenden Anzeigen und Zähler **dürfen nicht gleichgesetzt werden**:

| Beobachtung | Aussage | Nicht daraus ableitbar |
|---|---|---|
| `authenticated` steigt | MAC-verifizierte Nachrichten angekommen | Gültiger Uhrenoffset |
| `syncPermission=true` | Node darf aktuell Probes starten | Präzisionsserie abgeschlossen |
| `probesSent` steigt | Node versucht Messungen | Coordinator-Antwort angekommen |
| `probesAnswered` steigt | Coordinator hat Antwortpfad ausgeführt | Node hat `Final` verarbeitet |
| `exchangeTimeouts` steigt | Messversuche wurden nicht vollständig abgeschlossen | Konkrete Ursache ohne weiteren Funktrace |
| `precisionRecentlyAccepted=true` | Kürzlich eine Serie nach Qualitätsregeln akzeptiert | Garantierte Genauigkeit in µs |
| `cluster_time.source=espnow_precision` | Zeitdienst wählt ESP-NOW-Modell | Physisch gemessener Auslöseversatz |
| `cluster_time.source=wifi_coordinator` | Wi-Fi-Fallback aktiv | ESP-NOW grundsätzlich defekt |

**Typische Diagnosefolge:** Board-Support → Drone Mode/WLAN-Kanal → Clusteridentität und HMAC → gültige Sync-Freigabe → vollständige Probes/Replies/Finals → Annahme der Serie → UTC-Anker → Wahl der Zeitquelle. Bei Coordinator-Wechsel, Epoch-Wechsel oder WLAN-Reconnect sind alte Referenzen zu verwerfen und neu zu messen.

Die zentrale Coordinator-Tabelle erhält noch keine vollständigen authentifizierten ESP-NOW-Ergebnisberichte sämtlicher Nodes. Daher müssen Offset/RTT/Drift derzeit jeweils aus den lokalen Node-Diagnosen gelesen werden. Fehlende Werte sind als *nicht verfügbar* und nicht als null oder erfolgreich darzustellen.

## 10. Architekturentscheidungen und bewusst offene Risiken

1. **HMAC statt Broadcast-Verschlüsselung:** Authentizität und Replay-Abwehr sind wichtiger als Geheimhaltung der Uhrzeit; ESP-NOWs eingebaute Broadcast-Verschlüsselung steht nicht wie bei Unicast zur Verfügung.
2. **Monotone Zeit statt laufender Systemuhrkorrektur:** Kein Sprung von `settimeofday()` während einer geplanten Aufnahme; die UTC-Zuordnung erfolgt getrennt.
3. **Drift persistent, Offset nicht persistent:** Ein Reboot entwertet den Offset, nicht notwendigerweise den langfristigen Drift derselben Coordinator-Hardware.
4. **Letzte Kalibrierung während Pause erhalten:** Keine neuen Probes, aber vorhandene Kalibrierung läuft nur innerhalb der festgelegten Qualitäts-/Altersschranken weiter. Anschließend muss Wi-Fi-Fallback oder „keine Zeit“ gelten.
5. **Kein garantierter Funk-Exklusivbetrieb:** Andere WLAN-Teilnehmer und WLAN-Treiberaktivität bleiben mögliche Quellen für Jitter.
6. **Kein Sicherheitsversprechen aus Lease allein:** Lease-Ablauf begrenzt das Risiko eines verlorenen Stoppsignals, garantiert aber keine sofortige Reaktion.
7. **Noch kein Hardware-Präzisionsnachweis:** Die Grenzen dienen der Qualitätsfilterung, nicht als gemessene µs-Fehlerschranken.

## 11. Erweiterter Abnahmetest speziell für Aktionspriorität

- Während einer laufenden Probe-Serie einen Shot planen: Messserie am betroffenen Node abbrechen; Capture darf nicht warten.
- Nach bestätigtem Capture-Abschluss wieder Freigaben und neue Messserien beobachten.
- Neustart, Shutdown, SD-Aktion und Drone-Mode-Umschaltung aus der Coordinator-Liste starten; den lokalen Widerruf und die Node-Reaktion beobachten.
- Testweise `SyncControl`-Widerruf verlieren lassen: Keine fortlaufenden neuen Probes nach Lease-Ablauf; erlaubte Verzögerung bis zu 12 s dokumentieren.
- Eine Clusteraktion deutlich länger als 10 s ausführen: Prüfen, ob die derzeitige zeitbasierte Freigabe **zu früh** zurückkehrt; dieses Risiko ist noch zu beseitigen.
- Synchronisation während und nach WLAN-Kanalwechsel, Coordinator-Reboot, Node-Reboot und HMAC-/Epoch-Änderungen testen.
- Für zeitkritische Aufnahmen unabhängig über GPIO den tatsächlichen Node-versus-Coordinator-Versatz messen.

## 12. Aktueller Präzisionsschätzer v2: RTT-Auswahl, feste Serienlänge, Regression

Diese Regeln ergänzen die in Abschnitt 4 beschriebenen Vier-Zeitstempel-Messungen. Das separate Dokument [`ESPNOW_ESTIMATOR_V2.md`](ESPNOW_ESTIMATOR_V2.md) enthält Detailbegründungen und Testanforderungen.

### 12.1 Drei Offset-Schätzer

Für gültige Probe-Austausche werden **drei voneinander unterscheidbare Diagnosewerte** bestimmt: (1) Offset der einzelnen Probe mit kleinster RTT, (2) Median aller gültigen Offsets, (3) Median des unteren RTT-Quartils mit mindestens drei Messwerten. **Nur Wert (3)** fließt in die aktuelle Uhrenkalibrierung ein. Die RTT-Quartil-Spannweite (`spreadUs`) wird separat als Qualitätshinweis geprüft. Die kleinste RTT verringert üblicherweise Queue-Anteile, garantiert aber keine symmetrischen Hin-/Rücklaufzeiten und daher keine tatsächliche µs-Genauigkeit.

### 12.2 Messdauer (feste Serienlänge)

Eine Messserie umfasst **fest 16 vollständige Austausche** und wird danach bewertet (mindestens acht gültige Probes, `spreadUs` maximal 250 µs, Median-RTT maximal 30 ms). Die frühere adaptive Erweiterung auf **32** bzw. **64** Austausche wurde in beta131 entfernt (`cluster_espnow_radio.cpp`, `finishSample()`): Bei längeren Serien vergrößert die Drift die Spannweite (z. B. 22 ppm × 12 s ≈ 264 µs). Eine nicht bestandene Serie wird verworfen, nicht verlängert. Der Probe-Mindestabstand liegt bei ungefähr **45 ms**. Bei ausbleibendem Fortschritt begrenzt ein **12-s-Limit** die Serie; Funkantworten und Timeouts können die Dauer deutlich beeinflussen. Capture-Priorität und Wegfall der authentifizierten Sync-Freigabe brechen die Serie unabhängig vom erreichten Sample-Ziel ab. Ein Abbruch erzeugt keinen neuen gültigen Offset.

### 12.3 Drift aus Langzeitregression

Das System berücksichtigt bis zu **24 zeitlich geordnete und gültige Serien**. Der gewichtete Least-Squares-Fit regressiert Node/Coordinator-Offset (µs) auf monotone lokale Sekunden; die resultierende Steigung entspricht relativer Drift in ppm. Die Gewichtung berücksichtigt die Qualität bzw. Offset-Spannweite. Als belastbares neues Regressionsmodell wird Drift erst akzeptiert, wenn mindestens **vier Serien** über mindestens **drei Minuten** vorliegen und Steigung (maximal ±500 ppm) sowie Residuenstreuung plausibel sind. Die Drift bleibt Coordinator-spezifisch in NVS hinterlegt und ist nach Reboot lediglich ein Startwert. Offset und frische UTC-Zuordnung müssen weiterhin neu gemessen werden.

**Statusgrenze:** `Estimate` enthält die Zusatzgrößen `minRttOffsetUs`, `allMedianOffsetUs` und `selected`. Diese Werte werden aktuell **nicht automatisch** in der zentralen Coordinator-Weboberfläche angezeigt. Ein authentifizierter Node-Ergebnisbericht pro Gerät und eine zentrale Last-Sync-Tabelle stehen weiterhin aus. Ohne diese Berichte darf die Weboberfläche weder erfolgreiche Sync-Zeiten noch Offset/Drift/RTT pro Node behaupten.

### 12.4 Abnahme

Vergleiche vor und nach der Schätzer-Änderung mindestens 30 gültige Serien unter möglichst gleicher WLAN-Last. Dokumentiere RTT-Verteilung, Anzahl gültiger Austausche, Zeit bis zur ersten gültigen Serie, `spreadUs`, Stabilität der Regressionsdrift und Erfolge/Abbrüche bei parallel gestarteten Clusteraktionen. Miss die Flanken planmäßig gleichzeitig geschalteter GPIOs mit *gemeinsamer externer Zeitbasis* (Logikanalysator/Oszilloskop); die Software-Offsetwerte alleine beweisen keine Triggergenauigkeit. Vollständige ESP32-S3-Builds und echte Funk-/GPIO-Tests sind noch offen.

## 13. Reproduzierbarer Testplan / Abnahmekriterien

1. Beide Boardvarianten bauen; alle Geräte mit kompatiblem Stand flashen. Insbesondere wurde die Semantik von UTC-Beacons gegenüber früheren Diagnosedeltas geändert.
2. Einen Coordinator + einen XIAO im gleichen WLAN-Kanal und Drone Mode starten. Prüfen: Broadcast-Empfang, HMAC-Annahme, keine Replay-Fehler und vollständige Probe-Serien.
3. Nach mehreren Serien Quelle `cluster_time.source` und lokale Offset-/RTT-/Driftwerte protokollieren; Wi-Fi-Fallback bei absichtlich unterbrochenem Funk bzw. ungültigen Messungen verifizieren.
4. Coordinator neu starten, Epoch/Boot-ID ändern, WLAN-Reconnect/Kanalwechsel und Node-Reboot testen. Bis zum frischen Kalibrierungsnachweis darf keine alte Präzisionszeit als gültig gelten.
5. Wiederholt SD/Stream/OTA/Recording/Capture parallel prüfen; kein Crash, Queue-Überlauf oder unzulässiger Zeitstatus. Für zeitkritische Aufnahmen die möglichen Zeitsprünge beim Quellenwechsel gesondert erfassen.
6. GPIO-Pulse aller Nodes gegen die monotone Coordinator-Zeit **unabhängig** messen: Verteilungen/Perzentile, Extremwerte, Drift über mindestens 30–60 Minuten und Störfälle notieren. Software-RTT oder ausgegebene µs alleine reichen nicht.
7. Erst nach bestandener Hardwaremessung eine belastbare Aussage zur tatsächlichen Genauigkeit treffen; bei Bedarf Filter/Grenzwerte und Scheduling optimieren.

**Bis dahin:** experimentell, nicht als garantiert mikrosekundengenau freigegeben.

## 14. Offene technische Arbeiten

- Vollständiger ESP32-S3-Build (Freenove und XIAO) und Laufzeit-/Funkvalidierung.
- Signierte, Coordinator-seitig abrufbare Node-Messberichte inklusive Messalter und Qualitätskennzahlen.
- Empirische Unsicherheitsbewertung (statt `uncertainty_estimate_us=0`) und mögliche Zeitsprünge bei Quellenwechsel prüfen.
- GPIO-basierte unabhängige Präzisionsvalidierung einschließlich Pin-Konflikten und realer Last.
- Verifizierter Umgang mit Wi-Fi-Kanalwechsel, unterschiedlichen APs und Wiederanlauf.

- Zustandsbasiertes Freigabeende anhand des *wirklich bestätigten* Abschlusses aller relevanten Coordinator-Aktionen statt einer festen lokalen 10-s-Pause.
- Authentifizierte Node-Ergebnisberichte und zentrale Last-Sync-Diagnose ergänzen.

## 15. Boardvarianten: Präzisions-Fähigkeit und diymore ESP32-S3 CAM

**Semantik von `BOARD_SUPPORTS_PRECISION_SYNC`** (`board_config.h`): Das Flag setzt nur Bit 1 im bestehenden `capabilities`-Feld von SFCO1 und erscheint als `precision_sync` in `/cluster_status` sowie als Warnhinweis in der Cluster-Ansicht. Es steuert **keine** Laufzeitentscheidung: Coordinator-Wahl, Start des ESP-NOW-Zeitdienstes (nur Coordinator bzw. Node mit Drone Mode, `cluster.cpp` `ClusterEspNowRadio::service`), Qualitätsprüfungen, Grenzwerte und der Fallback auf `wifi_coordinator` sind davon unabhängig. `precision_sync=yes` heißt „Boardtyp für den ESP-NOW-Präzisionszeitdienst vorgesehen“, **nicht** „GPIO- oder Kamera-Trigger physisch vermessen“ (siehe Abschnitt 13, Punkt 6–7).

| Board | `BOARD_SUPPORTS_PRECISION_SYNC` | Physische Triggergenauigkeit |
|---|---|---|
| XIAO ESP32S3 Sense | 1 | nicht unabhängig vermessen |
| diymore ESP32-S3 CAM (OV5640) | 1 (seit 2026-10-22) | nicht vermessen |
| Freenove FNK0085 | 0 | – (passives GPIO-Messgerät) |

**Nachweis diymore (`c001`, 2026-10-22, 5-Minuten-Lauf, Node mit Drone Mode, Coordinator devxiao3):** HTTP 30/30, `cluster_time.source=espnow_precision` bei 30/30 Abfragen, 4 Serien akzeptiert / 0 verworfen, Median der Serien-Streuung 52 µs, Maximum 114 µs, 65 Probes, 64 Exchanges, 1 Exchange-Timeout, 0 Neustarts. Referenz im selben Lauf: XIAO 1 Median 52 µs, XIAO 2 Median 38,5 µs (je 4/0 Serien). Das ist ein kurzer Lauf und beschreibt nur die interne Zeitschätzung.

**Offene Beobachtung Browser-/Kameralast (nicht bewiesen):** In einem früheren Lauf war ein Browserfenster geöffnet; das diymore zeigte deutlich schlechtere Synchronisationswerte und längere Phasen mit `wifi_coordinator`. Vor dem erfolgreichen Lauf wurden Fenster geschlossen, der Test neu gestartet **und** die Antenne geändert. Die Ursache ist deshalb nicht zuordenbar; ein bloß geöffnetes, inaktives Fenster gilt nicht als Fehlerursache.

Plausible Mechanismen aus dem Code (Hypothesen, nicht gemessen):
- WebServer (`server.handleClient()`), HTTP-MJPEG/RTSP (`streamerLoop()`) und `clusterLoop()` laufen kooperativ im selben `loopTask`. Lange HTTP-Antworten oder Stream-Frames verzögern die Abarbeitung der ESP-NOW-Empfangsqueue und das Senden von Reply/Final. Die Zeitstempel selbst werden im RX-Callback bzw. direkt vor `esp_now_send()` genommen; die Verzögerung zeigt sich in `rx_queue_delay_*_us` und `reply_final_dispatch_gap_*_us` und kann bei > 1,2 s zu `exchange_timeouts` führen.
- Auf dem messenden Node selbst ist während der Präzisionssynchronisation **kein** Kamerastream möglich: Präzisionszeit läuft nur im Drone Mode, und dort stoppt die Hauptschleife Streamer und Aufnahme (`sensorforge.ino`, Drone-Zweig), `/snapshot` antwortet mit 503. Ein Kamerastream eines *anderen* Geräts (nicht im Drone Mode) auf demselben Kanal belegt jedoch Funkzeit. Mehr Kanalbelegung verlängert/variiert die Sendeverzögerung (`probe_tx_pre_send_*_us`) und kann RTT-Asymmetrie erzeugen; die RTT-Auswahl verwirft solche Austausche, wodurch Serien länger dauern oder abgelehnt werden.
- Regelmäßiges Web-Polling (Statusseiten mit Auto-Refresh) wirkt wie kleine, häufige Loop-Blockaden; ein Fenster ohne Polling erzeugt keine Last.

Bis zur Messung werden Task-Prioritäten, WLAN-Einstellungen und Synchronisationsalgorithmus **nicht** geändert.

**Vorgeschlagener Belastungstest (reproduzierbar, Antenne unverändert):** gleiche Aufstellung, je Phase 10 Minuten, Reihenfolge A–B–A–C–A, Statusabfrage `/cluster_status` alle 10 s von einem Rechner ohne sonstige Seiten:
- A: kein Browser zum diymore.
- B: Browser mit Cluster-/Statusseite (Auto-Refresh), kein Stream.
- C: Browser mit aktivem HTTP-MJPEG-Stream eines **anderen** Geräts außerhalb des Drone Mode auf demselben WLAN-Kanal (Gerät, Auflösung und fps notieren). Ein Stream vom diymore selbst ist im Drone Mode nicht möglich und daher keine Testphase.

Je Phase erfassen: Anteil `espnow_precision`, Serien akzeptiert/verworfen, Median und Maximum der Serien-Streuung, `exchange_timeouts`, `rx_queue_delay_max_us`, `reply_final_dispatch_gap_max_us`, `probe_tx_pre_send_last_us` sowie die WLAN-RSSI (`/api/v1` bzw. WebConfig-WLAN-Seite). Alle Messwerte vollständig dokumentieren, keine Ausreißer entfernen. Die XIAOs laufen als Referenz unverändert mit. Erst wenn B oder C gegenüber beiden umgebenden A-Phasen wiederholt abweicht, gilt ein Lasteinfluss als belegt.
