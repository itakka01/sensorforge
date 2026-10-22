# SensorForge Test Manager – dynamische Clustererkennung

Python 3.10+, Standardbibliothek, **nur lesender HTTP-Zugriff**.

## Start

1. `sensorforge_config.example.json` nach `sensorforge_config.json` kopieren.
2. `cluster_id` und `seed_host` eintragen (z. B. `devxiao3.local` oder die IP des Coordinators).
3. Falls die Geräte HTTP Basic Auth verlangen: `auth.username` und `auth.password` in der privaten Datei eintragen. `chmod 600 sensorforge_config.json` setzen. **Nicht** die Datei mit Zugangsdaten weitergeben.
4. `python3 sensorforge_test_manager.py --minutes 5` starten; alternativ `python3 sensorforge_test_manager.py` für das Menü.
5. Jede neue Messung erhält ihr eigenes Unterverzeichnis `sensorforge_tests/run_*` mit SQLite, HTML, JSON, Markdown und Metadaten.

## Erkennung

Die `/cluster_status`-API eines erreichbaren Seed-Geräts enthält die aktuellen `nodes`-Informationen des Clusters. Das Tool fragt diese regelmäßig ab und prüft jeden Node **direkt** auf seine eigene `integration_id` und die richtige `cluster_id`. Neue Nodes werden aufgenommen, ausgefallene bleiben sichtbar. IP- und Hostnamenwechsel werden anhand der Gerätekennung abgefangen. Die HTTP-Zugangsdaten gelten für die angefragten Geräte; diese müssen die gleichen Zugangsdaten akzeptieren.

**Warum ein Seed?** SensorForge-Firmware verwendet für die eigentliche Peersuche eigene signierte Discovery-Pakete über das lokale Netz. Ein gewöhnliches Python-HTTP-Testtool kennt dieses Funk-/Multicast-Protokoll bisher nicht. Die Cluster-ID ist eine Kennung und enthält keine IP-Adresse; das Cluster-Passwort ist keine HTTP-Anmeldung. Für echte ID+Passwort-only-Erkennung müsste das Discovery-/HMAC-Protokoll anhand des Firmwarecodes exakt implementiert und getestet werden. Das ist nicht Bestandteil dieser read-only HTTP-Version.

Der Seed muss beim **ersten Start** erreichbar sein. Anschließend kann das Tool über bereits validierte Nodes weiter entdecken, auch wenn der ursprüngliche Seed ausfällt. Werden alle Geräte unerreichbar, bleiben die zuletzt erkannten Geräte im Bericht und werden weiterhin abgefragt.

## Betriebshinweise

- **Read-only:** Keine Änderungen an Firmware, Drone Mode, Cluster oder Sensoren.
- `precision_sync=no` ist eine Fähigkeitskennzeichnung, nicht die tatsächliche Zeitquelle. Entscheidend ist `cluster_time.source`.
- Interne `last_series_spread_us` belegt keine tatsächliche GPIO-Triggergenauigkeit.
- Bei vielen Nodes kann die sequenzielle HTTP-Abfrage länger als 10 Sekunden dauern. Das wird durch die Zyklusausgabe sichtbar; eine Parallelisierung wäre ein separater Schritt.
- Kompatibilität: Konfigurationen mit `expected_hosts`, aber ohne `seed_host`, verwenden den alten statischen Modus. Alte SQLite-Berichte bleiben mit `--report` auswertbar.

## Startdialog und Clusterübersicht

Bei `python3 sensorforge_test_manager.py` (mit interaktivem Terminal) fragt der Manager
**vor dem Menü** Cluster-ID, Seed-Host, HTTP-Benutzername/-Passwort sowie Mess-,
Timeout- und Discovery-Intervall ab. Vorhandene JSON-Werte werden vorgeschlagen;
**Enter** übernimmt den Wert. Das Passwort wird nicht angezeigt. Änderungen
werden nur für diesen Programmlauf verwendet, die JSON bleibt unverändert.

Anschließend wird der Seed über `/cluster_status` gelesen und die erwartete
Cluster-ID geprüft. Eine tabellarische Übersicht zeigt alle angekündigten Geräte
mit Rolle, Online-Kennzeichen und IP. Diese Übersicht ist eine **Ankündigung**;
im anschließenden Test prüft der Manager jeden Node separat auf Cluster-ID und
`integration_id`. Ist der Seed nicht erreichbar, kann man die Eingaben korrigieren,
das Menü trotzdem öffnen oder beenden.

`--minutes 5` sowie `--no-prompt` unterdrücken den Startdialog (nützlich für
automatisierte Läufe). `--report` erstellt Berichte ohne Netzwerk-Abfrage.
Cluster-Passwort und HTTP-Passwort sind verschieden: Das Tool verwendet noch
HTTP-Seed-Discovery und nimmt nicht selbst am signierten Multicast-Verfahren teil.

## Live-Diagnoseansicht (Update)

Der dynamische Clustertest zeigt nun ein Terminal-Dashboard mit Laufzeitbalken,
Restzeit, aggregierter Cluster-Gesundheit, ESP-NOW-Zeitquellen, Serien-Zuwächsen
und Ereignissen. Standardmäßig werden **höchstens 12 Nodes gleichzeitig**
angezeigt. Bei 100+ Nodes bleiben Erfassung, SQLite und Berichte vollständig;
die Anzeige ist nur eine paginierte Ansicht. Die Startübersicht zeigt ebenfalls
maximal 15 Geräte, damit das Menü sichtbar bleibt.

**Steuerung während des Tests (jeweils Taste + Enter):**
- `n` nächste Seite, `b` vorherige Seite
- `f` nur Warnungen/Ausfälle, `a` alle Nodes
- `s` sofort einen Zwischenbericht (`stability_report.html`/`.json`/`.md`) erzeugen
- `p` Messung pausieren/fortsetzen; die Pause wird nicht auf die Testdauer angerechnet
- `q` sauber beenden und Endbericht erstellen; alternativ einmal `Ctrl+C`

Alle Tastenkürzel setzen eine interaktive Konsole voraus. Beim Umleiten der
Ausgabe in eine Datei schreibt das Tool nur eine kompakte Zeile je Zyklus.
Der Bildschirm ist eine Diagnoseübersicht; Serien-Zähler sind **Deltas seit
dem letzten erfolgreichen Sample**, keine kumulative Statistik. Ein fehlgeschlagener
HTTP-Abruf bleibt sichtbar und führt nicht zu einer gefälschten Nullmessung.
Neue Nodes werden weiterhin anhand ihrer `integration_id` identifiziert.

Für größere Cluster erfolgen HTTP-Geräteabfragen parallel (maximal 16 Worker,
anpassbar mit `max_http_workers` in der JSON-Konfiguration). Ein Polling-Zyklus,
der länger als das Zielintervall dauert, erscheint unter **Überläufe**; bei
100+ Geräten sind Zykluszeiten daher vom Netzwerk, den HTTP-Timeouts und der
Rechnerleistung abhängig. `dashboard_page_size` (Standard 12) konfiguriert die
maximale Zeilenzahl der Ansicht. Es sind keine zusätzlichen Python-Pakete nötig.

Die neue Oberfläche gilt für den dynamischen Modus mit `seed_host` und
`cluster_id`. Der bisherige statische Kompatibilitätsmodus bleibt unverändert.
Der Live-Dashboard-Test `python3 test_live_dashboard.py` deckt 125 Geräte und
die Navigation/Filterung ab. Die Laufzeit wurde noch nicht in einem realen
100-Node-Cluster belastungsgetestet.

## Tastatursteuerung – Korrektur

Die Steuerung wird auch dann gestartet, wenn `sys.stdin.isatty()` `False` meldet (z. B. in manchen IDE-/Windows-Terminals). Während der Messintervall-Wartezeit wird alle 150 ms nach Befehlen geschaut. `q`, `s`, `p`, `n`, `b`, `f`, `a` werden mit **Enter** bestätigt; `s` schreibt einen Zwischenbericht, `f` filtert Warnungen, `q` beendet sauber. Wenn die Konsole überhaupt kein stdin zur Verfügung stellt, verwende ein reguläres Windows-Terminal/PowerShell und starte `python sensorforge_test_manager.py` direkt. Alte Messdaten und private Konfiguration beibehalten.


### Erweiterte Live-Diagnose (Firmware/Temperatur/Heap/Clusterzeit)

Die Gerätetabelle trennt den Gerätenamen von der 8-stelligen Hex-Kennung (`integration_id`). `Firmware` stammt aus `nodes[].release`, die CPU-Temperatur aus `cpu_temp_deci_c` (°C), und `Heap` aus `free_heap_bytes` (KiB). Nicht gelieferte Werte erscheinen als `—`. Bei nicht erreichbaren Nodes wird kein veralteter Messwert als aktuell dargestellt.

Der Kopfbereich zeigt die vom Coordinator in `cluster_time.utc_us` gemeldete UTC-Clusterzeit **zum Zeitpunkt der letzten erfolgreichen Abfrage**. Das ist bewusst keine fortlaufend interpolierte Uhr. Fehlt die Clusterzeit oder ist sie ungültig, erscheint `—`.

Die vom Coordinator **aktuell gemeldete** Firmwareversion dient als Vergleichsreferenz (falls unbekannt: häufigste Version); abweichende Versionen werden mit `!` und (auf Farbkonsolen) gelb markiert und im `f`-Filter angezeigt. Abweichend heißt nicht automatisch *veraltet*: Bei gleichmäßig verteilten Firmwareständen entscheidet keine Versionsordnung. Bei der Terminalbreite passt sich die Anzahl der angezeigten Spalten automatisch an, die Liste bleibt immer auf maximal `dashboard_page_size` (standardmäßig 12) Zeilen begrenzt. Für alle Felder Terminal auf mindestens 136 Spalten verbreitern.

## Lange Messungen: kompakte zweite SQLite-Datei

Im dynamischen Cluster-Modus wird automatisch neben `stability.sqlite3` auch
`stability_compact.sqlite3` angelegt. Die Originaldatenbank und deren Reports
bleiben unverändert. Die kompakte Datei enthält für jeden bekannten Node eine
Statusprobe pro 60 Sekunden sowie **jede erkannte** Änderung von Zeitquelle,
HTTP-Erreichbarkeit, Präzisionsstatus, Boot/Epoch und den Zählern für akzeptierte
/ abgelehnte Serien, Timeouts und Replay. Ereignisse haben die Auflösung des
konfigurierten HTTP-Abfrageintervalls (standardmäßig 10 Sekunden).

Die Tabelle `samples` enthält u.a. Firmware, Temperatur/Heap (soweit im Status
vorhanden), Clusterzeit und eine gezielte JSON-Auswahl an ESP-NOW-Messwerten.
Die Tabelle `events` enthält Veränderungen einschließlich der kumulativen
Vorher-/Nachher-Zähler. Die Datei ist direkt mit SQLite analysierbar und kann
für die externe Analyse hochgeladen werden. Rohdaten und detaillierte HTTP-
Fehler bleiben weiterhin ausschließlich in der vollständigen Datenbank.

Optionale Einstellungen in `sensorforge_config.json`:

```json
"compact_enabled": true,
"compact_sample_seconds": 60
```

**Wichtig:** Ein 6-Stunden-Test mit fünf Geräten ist typischerweise handlich;
bei 100+ Nodes wachsen auch kompakte Dateien linear. Die tatsächliche Größe
hängt von Ereignishäufigkeit und Konfiguration ab. Der kompakte Recorder wird
im dynamischen Testpfad benutzt; der statische Legacy-Test ist unverändert.
