# SensorForge – ESP-NOW-Zeitsynchronisation

**Stand:** 2026-10-24, `v87-beta149`. Einziges Verfahren seit beta147: Zeitsignal des Coordinators + seltene Laufzeit-Einmessung. Alle älteren Verfahren (Zeit-Broadcaster, Drift-Regression über Serien, NVS-Driftspeicher, manueller Zeitabgleich, SFHT1-Hardwaremessung) sind aus dem Code entfernt; ihre Beschreibungen stehen nur noch in der Git-Historie und im `CHANGELOG.md`.

**Validierungsstatus:** Hardwarelauf mit 9 Nodes (beta147): alle Nodes durchgehend präzise, Software-Streuung 8–26 µs. Ein unabhängiger GPIO-Nachweis (Abschnitt 8) steht noch aus. Alle Werte sind Software-Zeitstempel, keine gemessene Auslösegenauigkeit; eine präzise Cluster-Uhr beweist keine gleiche Kamera-Belichtung.

## 1. Verfahren

**Warum so:** Mit reinen Hin-und-zurück-Serien (bis beta146) misst immer nur ein Node (~3,5 s), das reicht lückenlos für ~24 Nodes. Ein Zeitsignal an alle kostet unabhängig von der Node-Zahl gleich viel Funkzeit.

**Messung vorab (beta146, 9 Nodes):** Zwei Nodes empfangen dasselbe Signal typisch 4–40 µs auseinander (Median 17,5 µs). 5–15 % der Signale kommen an einem einzelnen Node 100–300 µs zu spät, nie zu früh. Zwischen Zeitstempel und Senden am Coordinator lagen gelegentlich bis 2,2 ms, dann für alle Nodes gleich. Quarzabweichungen 4–36 ppm.

1. **Zeitsignal (Coordinator, `sendBeacon()`):** jede Sekunde ein signiertes `Beacon` (Typ 1) an alle. Es trägt selbst keine Zeit, nur den Cluster-Mittelwert der Funklaufzeit (`auxUs`, ns; unbekannt = INT64_MIN). Zeitstempel unmittelbar **vor** `esp_now_send()`, also nach der HMAC-Berechnung.
2. **Folgepaket (`serviceFollowUp()`):** sobald der TX-Done-Callback des Beacons da ist (höchstens 30 ms) ein `FollowUp` (Typ 12): `nonce` = Sequenz des Beacons, `timeUs` = Zeitstempel (Coordinator-Monotonzeit), `auxUs` = Coordinator-UTC zu diesem Zeitstempel, `preSendDelayUs` = Sendeweg Zeitstempel → TX-Done (µs, `UINT32_MAX` = unbekannt).
3. **Node-Modell (`cluster_beacon_sync.*`, reine Logik):** je Signal ein Messpunkt `y = Zeitstempel − Empfangszeit`. Alle Störungen sind Verspätungen, daher sind die **größten** `y` die sauberen. Gleitendes Fenster der letzten 64 Punkte (≤ 120 s); Gerade (Versatz + Gangabweichung) durch die obere Hälfte („obere Hüllkurve“). Verworfen werden Punkte, deren Sendeweg mehr als 150 µs über dem unteren Viertel der letzten 32 lag (Coordinator hat spät gesendet; bis beta148 Bezug = kürzester Sendeweg, ein einzelner ungewöhnlich schneller Wert ließ dann ~30 s lang fast alle Signale verwerfen und alle Nodes gleichzeitig „Zeitsignal ausgeblieben“ melden), und Punkte mehr als 150 µs über der Hüllkurve (physikalisch unmöglich; 5 in Folge = Modell neu aufbauen). Gültig ab 8 Punkten über ≥ 4 s, Streuung (RMS der oberen Hälfte) ≤ 150 µs, Gangabweichung ≤ 500 ppm.
4. **Funklaufzeit:** Aus Zeitsignalen allein ist die konstante Laufzeit (Luftzeit ~1 ms + Empfangsweg) nicht bestimmbar. Sie wird mit einer Hin-und-zurück-Serie eingemessen (Abschnitt 2): `Laufzeit = Zwei-Wege-Versatz − Hüllkurven-Versatz` (zum Mittelzeitpunkt der Serie). Der Node nutzt den Median seiner letzten 5 Einmessungen (eigen, höchstens 60 min alt), sonst den Cluster-Mittelwert aus dem Beacon (Median der eigenen Laufzeiten aller Nodes, Berichte ≤ 60 s, Einmessung ≤ 60 min). Plausibel: 0–10 ms.
5. **Coordinator-Zeit am Node:** `Coordinator = Node + Hüllkurven-Versatz(Node) + Laufzeit`; UTC über das UTC/Monoton-Paar des neuesten Folgepakets (`precisionUtcNow()`).
6. **Gültigkeit (`precision_state`):** Modell gültig, neuester Messpunkt ≤ 15 s alt (Überbrückung, z. B. Capture-Pause), Laufzeit (eigen oder Cluster) bekannt, UTC vom Coordinator vorhanden.
7. **Fallback:** Ist die Präzisionszeit nicht gültig, liefert `clusterTimeNowUs()` die Wi-Fi-Clusterzeit (`SFTQ1`/`SFTR1`, Millisekundenbereich), sonst gilt die Zeit als nicht verfügbar. `cluster_time.source`: `espnow_precision`, `wifi_coordinator` oder `local_coordinator`. Die Systemuhr wird nie verstellt.

**Simulation** (`test_beacon_sync.cpp.test`, Störungen wie gemessen, Zwei-Wege-Fehler 25 µs Versatz + 12 µs Rauschen je Einmessung, 1 % Verlust):

| Szenario | Node-zu-Node p50 / p95 / max | alle gültig nach |
|---|---|---|
| 9 Nodes, alle eingemessen | 6 / 19 / 39 µs | 35 s |
| 100 Nodes, alle eingemessen | 11 / 32 / 84 µs | 35 s |
| 100 Nodes, Hälfte nur mit Cluster-Mittel | 10 / 30 / 71 µs | 35 s |
| 9 Nodes, 12 s ohne Zeitsignal | 7 / 22 / 56 µs | 34 s |
| 9 Nodes, Coordinator-Quarz +30 ppm | 7 / 21 / 38 µs | 35 s |

**Hardware (beta147, 9 Nodes, 13 dBm):** alle Nodes `valid`, Streuung des Zeitsignals 8–26 µs, je Node 1–2 % Signale wegen späten Sendens verworfen. Eigene Laufzeiten nach 3–4 Einmessungen 2052–2098 µs (vor der 32-Signal-Regel bis 2240 µs). 22 Minuten ohne Ausfall.

## 2. Laufzeit-Einmessung (Hin-und-zurück-Serie)

- **Austausch:** `Probe` (Node, `t1`) → `Reply` (Coordinator, `t2` Empfang, `t3` Antwort) → `Final` (liefert `t3`). `t4` = Empfang des Reply am Node, nie des Final. `offset = ((t2 − t1) + (t3 − t4)) / 2`, `rtt = (t4 − t1) − (t3 − t2)`. Die vor dem Senden genommenen Zeitstempel werden um die gemessene Dauer bis zur Rückkehr von `esp_now_send()` korrigiert (`lastProbePreSendUs`, Reply-Wert im Final).
- **Serie:** fest 16 Austausche, ~45 ms Abstand, Abbruch nach 12 s. Schätzer `cluster_espnow_twoway.*`: Median des unteren RTT-Quartils (mindestens 3 Werte). Angenommen bei ≥ 8 gültigen Austauschen, Spannweite ≤ 250 µs, Median-RTT ≤ 30 ms; sonst verworfen (Wiederholung nach 12–24 s, zufällig gestreut).
- **Voraussetzung:** Der Node meldet sich erst zur Einmessung (`NodeSyncReport`), wenn sein Zeitsignal-Modell mindestens 32 Signale hat (`kMinCalibrationSamples`); eine früher fertige Serie wird bis 120 s aufgehoben. Mit nur 8 Signalen lag die erste Einmessung bis 150 µs daneben.
- **Plausibilität (beta152):** Eine Einmessung, die mehr als 400 µs von der Cluster-Laufzeit abweicht, wird verworfen (`delayPlausible`, Zähler `calibrations_discarded`) und neu gemessen. Anlass: Antennen 5 cm auseinander, erste Serie von c001/c005 um +1,3 ms verschoben bei kleiner Streuung. Ohne Cluster-Laufzeit gilt nur der Bereich 0–10 ms.
- **Zeitplan:** fällig nach 10 min (`kSlotRefreshAgeMs`), „abgelaufen“ (nur Diagnose) nach 30 min. Bei 100 Nodes dauert eine Runde ~6 min.

## 3. Slot-Planung (Coordinator, `cluster_slot_scheduler.h`)

Der Coordinator beantwortet Probes nur vom jeweiligen **Slot-Inhaber**; immer nur ein Node misst. Reine Logik mit Host-Test `test_slot_scheduler.cpp.test`, angebunden in `serviceSlots()`.

1. Start: Freigabe (`SyncControl`, Typ 5, `timeUs` = Lease in ms) sofort, danach jede Sekunde, Lease 3 s.
2. Ende, sobald zuerst eintritt: **Serie vorbei** (seit 1,5 s keine Probe, keine Pause danach), **keine Aktivität** (3 s ohne Probe), **Obergrenze** 14 s oder **Inhaber verschwunden** (30 s ohne Bericht, neuer Boot); in den letzten drei Fällen 3,5 s Pause. Capture-Pause und Benchmark brechen den Slot ohne Bewertung ab.
3. Nächster Inhaber: nur fällige Nodes, ohne eigene Laufzeit zuerst, dann ältestes Einmessalter, bei Gleichstand wer am längsten wartet. Niemand fällig = kein Slot.
4. Ergebnis über `NodeSyncReport` (alle 10 s und sofort nach der Serie; ohne Bericht nach 12 s gilt der Slot als fehlgeschlagen). Erster Fehlschlag: sofort wieder wählbar; danach Wartezeit 5, 10, 20, 40, höchstens 60 s.

## 4. Berichte an den Coordinator

| Paket | Takt | Inhalt |
|---|---|---|
| `BeaconSyncReport` (Typ 13) | 10 s | Modell gültig, Präzision gültig, Zustand, Laufzeitquelle, Punkte, Alter, Streuung, Gangabweichung, eigene Laufzeit + Alter |
| `NodeSyncReport` (Typ 6) | 10 s + nach jeder Serie | `nonce` 0x100 = eingemessen (Bits 16–31 Alter in s), 0x200 = noch nicht eingemessen |
| `NodeSyncReport` mit 0x1000 | 30 s | Serien angenommen / verworfen (`timeUs`) |

Seit beta154 gehen alle Berichte als Unicast an den Coordinator (MAC aus dessen Zeitsignal; vorher Broadcast ohne Bestätigung). Dessen Funkschicht bestätigt jeden Bericht, unbestätigte wiederholt der Sender selbst. Meldet die Sendebestätigung beim `BeaconSyncReport` trotzdem einen Fehlschlag, sendet der Node ihn nach 300 ms erneut, höchstens 2× je regulärem Bericht. Solange der Coordinator unbekannt ist, wird weiter per Broadcast gesendet. Zähler in `espnow_time_local`: `reports_acked`, `reports_unacked`, `report_retries`.

Der Coordinator führt daraus je Node-Boot Langzeitwerte: Überwachungsdauer, präzise Zeit, Ausfälle (Node meldet präzise → nicht präzise) mit Grund und Alter des letzten, fehlende Berichte (> 30 s ohne Bericht, getrennt gezählt; diese Zeit gilt als nicht überwacht), größte Streuung im präzisen Zustand. Die Einmess-Runde schätzt der Coordinator als aktive Nodes × Slotzeit je angenommener Serie (abgelehnte Serien eingerechnet).

## 5. Nachrichten und Sicherheit

- **Paketformat V3:** 56 Byte Nutzdaten + 32 Byte HMAC-SHA256 = 88 Byte, `kVersion` 3. Ältere Firmware wird nicht verstanden (alle Boards gleich flashen).
- **HMAC** mit einem aus dem Cluster-Schlüssel abgeleiteten ESP-NOW-Schlüssel, Replay-Schutz je Absender-MAC über (Boot-ID, Epoch, Sequenz). Broadcasts sind nicht verschlüsselt; HMAC schützt gegen eingeschleuste Zeit, nicht gegen Störsender oder Verzögerung.
- **Callback-Regel:** Empfang nur in eine begrenzte Queue (Zeitstempel im Callback), HMAC und Auswertung in der Cluster-Schleife.
- **Funkkanal:** ESP-NOW nutzt den Kanal der bestehenden WLAN-Verbindung. Kanal-, Epoch-, Coordinator- oder Boot-Wechsel starten den Funkdienst neu und verwerfen das Modell.
- **Typen:** 1 Beacon, 2 Probe, 3 Reply, 4 Final, 5 SyncControl, 6 NodeSyncReport, 8–11 Benchmark, 12 FollowUp, 13 BeaconSyncReport.

## 6. Vorrang von Aufnahmen und Aktionen

Capture und Coordinator-Listenaktionen rufen `pauseForCapture()`: Freigaben werden widerrufen, laufende Serien verworfen, Zeitsignale pausieren. Das Zeitsignal-Modell bleibt erhalten und überbrückt bis 15 s. Ein verlorener Widerruf wird durch die 3-s-Lease begrenzt. Die Pause bei Listenaktionen ist zeitbasiert (typisch 10 s), nicht an den bestätigten Jobabschluss gekoppelt.

## 7. Diagnose

- **Coordinator-Seite** `/cluster_coordinate`: Spalte „Zeit“ in der Geräteliste und Abschnitt „Zeitsynchronisation“ (Daten `GET /cluster_time_nodes`, Langzeitwerte als CSV `GET /cluster_time_csv`). Laden nur per „Aktualisieren“ bzw. optional alle 30 s; keine zusätzliche Funklast.
- **Je Gerät** `/cluster_status` → `espnow_time_local`: Zähler, `precision_state`, `beacon_*`, `delay_source`, `own_delay_*`, `cluster_delay_*`, Serien angenommen/verworfen, am Coordinator `slot_*`.
- **Rohdaten** der letzten 128 Zeitsignale je Gerät: `GET /cluster_espnow_beacon_trace_csv` (`sequence` ist auf allen Geräten gleich).

## 8. Abnahme mit GPIO (offen)

Freenove als passives Messgerät (Seite `/cluster`, „GPIO-Messgerät (passiv)“, Eingänge GPIO14/21/41), Quellen D1/GPIO2 der XIAO-Nodes, gemeinsame Masse. Auf der Coordinator-Seite die verdrahteten Nodes wählen, „GPIO-Testimpuls (D1)“ setzen und Aufnahmen planen; der Impuls liegt direkt um `esp_camera_fb_get()`. Über mindestens 30–60 Minuten Verteilung, Extremwerte und Ausreißer dokumentieren. Erst danach ist eine Aussage zur tatsächlichen Auslösegenauigkeit belastbar.

## 9. Grenzen (ab beta144 für 100 Geräte ausgelegt, Tabellen im PSRAM)

| Grenze | Wert | Verhalten darüber |
|---|---|---|
| Cluster-Mitgliederliste (`CLUSTER_MAX_PEERS`) | 100 fremde Geräte | weitere werden abgewiesen, rote Warnung auf `/cluster` und `/cluster_coordinate` |
| Slot-Tabelle, Wiedergabeschutz, Node-Berichte (`kNodeReportCapacity`) | 128 | Slot: kein Slot, `slot_table_full_recent`; Berichte: der am längsten nicht gehörte wird ersetzt |
| ESP-NOW-Partner für Antworten | 18 + Broadcast | am längsten unbenutzter wird ersetzt |
| Präzisionszeit | unabhängig von der Node-Zahl | Einmess-Runde je Node ~3,5 s; gelbe Warnung, wenn eine Runde länger als 10 min dauert, rot ab 30 min (ab ~500 Nodes) |

## 10. Boardvarianten

`BOARD_SUPPORTS_PRECISION_SYNC` (`board_config.h`) setzt nur Bit 1 im `capabilities`-Feld von SFCO1 und erscheint als `precision_sync` in `/cluster_status`. Es steuert keine Laufzeitentscheidung.

| Board | `BOARD_SUPPORTS_PRECISION_SYNC` | Physische Triggergenauigkeit |
|---|---|---|
| XIAO ESP32S3 Sense | 1 | nicht unabhängig vermessen |
| diymore ESP32-S3 CAM (OV5640) | 1 | nicht vermessen |
| Freenove FNK0085 | 1 (seit beta149; vorher 0, mit dem alten Verfahren nur ~64 % präzise) | nicht vermessen (dient selbst als passives GPIO-Messgerät) |

Präzisionszeit läuft auf Nodes nur im Drone Mode; dort sind Streamer und Aufnahme gestoppt. Web-Abfragen und Streams anderer Geräte auf demselben Kanal können den Empfang verzögern; das Zeitsignal-Modell verwirft verspätete Signale, die Einmessung verspätete Austausche.
