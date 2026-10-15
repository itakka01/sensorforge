# SensorForge / exapan C1 – ESP-NOW-Zeitsynchronisation

**Stand:** 2026-10-13; Architektur-/Betriebsdokument zum Entwicklungsstand `v87-beta105` mit den ESP-NOW-Deltas bis `espnow_adaptive_rtt_regression_delta`.

**Validierungsstatus:** Der integrierte Code ist **noch nicht** durch einen vollständigen Arduino-ESP32-Build beider Boards und reale Funk-/GPIO-Tests bestätigt. Aussagen unten über Programmverhalten beschreiben den beabsichtigten und implementierten Codepfad, **keinen nachgewiesenen Genauigkeitswert**.

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
| `cluster_espnow_regression.*` | Gewichtete lineare Regression aus mehreren angenommenen Messserien für Drift in ppm |
| `cluster_espnow_drift.*` | Coordinator-bezogene NVS-Kalibrierung und Validierung |
| `webconfig_cluster.cpp` | Lokale Funkdiagnose; **noch keine authentifizierten Node-Einzelberichte im Coordinator** |

`ESPNOW_*_STATUS.md` und `ESPNOW_*_INTEGRATION.md` dokumentieren historische Zwischenstände. Sie sind nicht als aktuelle Funktionsübersicht zu lesen; dieses Dokument beschreibt die zusammengesetzte Zielarchitektur.

## 3. Start, Voraussetzungen, Funkkanal

ESP-NOW arbeitet auf der bereits verbundenen WLAN-STA und **erzwingt keinen Kanalwechsel**. Coordinator und Nodes benötigen kompatiblen Kanal, gültige Cluster-Mitgliedschaft/Schlüssel, aktiven Cluster und Drone Mode. Bei Reconnect, Kanal-, Coordinator-, Boot-ID- oder Epoch-Wechsel müssen alte Funk-/Zeitreferenzen verworfen und neu aufgebaut werden. Zwei-WLAN-Protokolle teilen denselben Funkchip; die Firmware kann *nicht* sicherstellen, dass fremde APs, andere Geräte oder der WLAN-Treiber im Messfenster keinerlei Verkehr erzeugen. Vermeidbare eigene Last kann reduziert, nicht garantiert ausgeschlossen werden.

## 4. Nachrichten und Sicherheitsmodell

- **Broadcast (ca. alle 10 s):** Coordinator-ID, Epoch/Boot-ID, Sequenznummer, authentifizierte UTC-/Monoton-Paarung. Der Broadcast ist grobe Referenz und keine alleinige Präzisionsmessung.
- **Probe/Reply/Final (ca. alle 60 s pro Node; adaptiv 16/32/64 Austausche):** vier *monotone* Zeitstempel `t1` Node vor Sendung, `t2` Coordinator beim Empfang, `t3` Coordinator vor Antwort, `t4` Node beim Empfang. Die Zeiten der ESP-NOW-API bzw. Callback-Ausführung sind nicht gleich physikalischen HF-Sende-/Empfangszeiten.
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
| Keine priorisierte Aktion / kein Schutzfenster | Sendet etwa alle 5 s eine Freigabe für max. 12 s | Darf während der gültigen Freigabe neue Probes starten |
| Coordinator-Aktion wird gestartet | Setzt lokale Schutzphase und sendet signierten Widerruf | Bricht Serie **nach Empfang** des gültigen Widerrufs ab |
| Capture wird geplant/angenommen | Lokaler Capture-Schutz und Widerruf; Funkaktivität im Schutzfenster pausiert | Eigener Capture-Schutz stoppt aktive Probes, sobald der Capture-Zustand lokal bekannt ist |
| Widerruf geht verloren | Kein unmittelbarer Stop auf dem entfernten Node garantiert | Die Freigabe verfällt spätestens nach 12 s seit der letzten gültigen Freigabe |
| Schutzfenster endet | Coordinator kann wieder Freigaben versenden | Misst erst wieder mit neuer gültiger Freigabe |

**Aktuelle Implementierungsgrenzen:** Die lokale Pause bei Listenaktionen ist noch zeitbasiert (typisch 10 s), nicht an den nachgewiesenen Abschluss des Jobs gekoppelt. Die Funkfreigabe ist daher noch **keine umfassende, clusterweit atomare Jobsperre**. Eine sofortige Stilllegung auf allen entfernten Nodes kann ein unzuverlässiger Funkbroadcast nicht gewährleisten. Insbesondere dürfen wir aus einem gesendeten Widerruf keinen bestätigten Empfang ableiten.

Die Funk-RX-Callbacks bleiben kurz; der ESP-NOW-Empfänger kann weiterhin Pakete entgegennehmen, aber ein Node startet ohne gültige Freigabe keine neue Präzisionsserie. Die gespeicherte Drift wird während einer Pause nicht neu gemessen. `syncPermission` und `syncPermissionRemainingMs` in den lokalen Diagnostics zeigen die Node-Freigabe, nicht den Erfolg der Zeitkalibrierung.

**Zeitverhalten:** Zeitbroadcasts ungefähr alle 10 s, Freigaben ungefähr alle 5 s, maximal 12 s Gültigkeit; Probe-Serien adaptiv mit 16, 32 oder 64 Austauschen, mit ungefähr 45 ms Mindestabstand bei erfolgreichem Ablauf, zuzüglich tatsächlicher Funklatenzen/Timeouts. Eine Serie kann deshalb länger dauern. Diese Intervalle sind Parameter und keine Echtzeitgarantien.

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

## 12. Aktueller Präzisionsschätzer v2: RTT-Auswahl, adaptive Serien, Regression

Diese Regeln ergänzen die in Abschnitt 4 beschriebenen Vier-Zeitstempel-Messungen. Das separate Dokument [`ESPNOW_ESTIMATOR_V2.md`](ESPNOW_ESTIMATOR_V2.md) enthält Detailbegründungen und Testanforderungen.

### 12.1 Drei Offset-Schätzer

Für gültige Probe-Austausche werden **drei voneinander unterscheidbare Diagnosewerte** bestimmt: (1) Offset der einzelnen Probe mit kleinster RTT, (2) Median aller gültigen Offsets, (3) Median des unteren RTT-Quartils mit mindestens drei Messwerten. **Nur Wert (3)** fließt in die aktuelle Uhrenkalibrierung ein. Die RTT-Quartil-Spannweite (`spreadUs`) wird separat als Qualitätshinweis geprüft. Die kleinste RTT verringert üblicherweise Queue-Anteile, garantiert aber keine symmetrischen Hin-/Rücklaufzeiten und daher keine tatsächliche µs-Genauigkeit.

### 12.2 Adaptive Messdauer

Eine Messserie startet mit **16 vollständigen Austauschen**. Genügt sie den Qualitätsbedingungen (mindestens acht gültige Probes, `spreadUs` maximal 250 µs, Median-RTT maximal 30 ms), endet sie unmittelbar. Sonst kann sie auf **32** und höchstens **64** vollständige Austausche erweitert werden. Der Probe-Mindestabstand liegt bei ungefähr **45 ms**. Bei ausbleibendem Fortschritt begrenzt ein **12-s-Limit** die Serie; Funkantworten und Timeouts können die Dauer deutlich beeinflussen. Capture-Priorität und Wegfall der authentifizierten Sync-Freigabe brechen die Serie unabhängig vom erreichten Sample-Ziel ab. Ein Abbruch erzeugt keinen neuen gültigen Offset.

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
