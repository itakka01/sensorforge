# Gruppen- und Montageverwaltung (Stand 2026-10-23, nach v87-beta139)

Einfache, persistente Gruppen- und Montageverwaltung auf dem **unveränderten flachen Cluster**.
Es gibt keine Untercluster, Funkgruppen oder Coordinator-Hierarchien. Gruppen sind reine
Anzeige- und Auswahlinformation. Sie beeinflussen weder Coordinator-Wahl noch
ESP-NOW-Zeitsync, Recovery-Slots, Capture-Timing oder Jobs.

## 1. Node: Montagedaten

Bearbeitung in der WebConfig des Geräts unter **Cluster → „Montage und Gruppe (optional)“**.
Die Felder gehören zum Formular der Cluster-Einstellungen und werden mit dem gemeinsamen
Knopf **„Speichern“** gespeichert (`POST /cluster_save`); einen eigenen Speicherknopf gibt es nicht.
Gespeichert wird in `config.txt` (SD bzw. LittleFS, gleicher Speicherweg wie die übrigen
Cluster-Einstellungen).

- Nur Montagedaten geändert: sofort wirksam, **kein Neustart**.
- Cluster-Einstellungen geändert (mit oder ohne Montagedaten): Neustart wie bisher.
- Ungültige Montagewerte lehnen das ganze Speichern ab, bevor etwas geschrieben wird.

| Config-Schlüssel | Bedeutung | Format / Bereich | Standard |
|---|---|---|---|
| `group_id` | Gruppe, z. B. `G03` | 0–15 Zeichen `A-Z 0-9 _ . -`, erstes Zeichen Buchstabe/Ziffer, wird in Großbuchstaben umgewandelt | leer |
| `mount_position` | Montageposition, z. B. `P02` | wie `group_id` | leer |
| `height_mm` | Montagehöhe | leer oder 0–100000 (ganze mm) | leer = unbekannt |
| `orientation_horizontal_deg` | horizontale Abweichung zur Gruppenausrichtung | −180…180, 0,1° | 0 |
| `orientation_vertical_deg` | vertikale Neigung | −90…90, 0,1° | 0 |

- Alle Felder sind optional. Die Gerätebezeichnung bleibt Hostname bzw. Integration-ID.
- **Kompatibilität:** Ältere `config.txt` ohne diese Schlüssel laden unverändert.
  Ungültige Werte in einer von Hand bearbeiteten Datei machen die Konfiguration **nicht**
  ungültig. Sie werden ignoriert (Standardwert) und auf der Konsole gemeldet.
  Beim Speichern über die WebConfig wird streng geprüft.
- Speichern der allgemeinen Einstellungen erhält die Schlüssel (dort werden unbekannte bzw.
  fremde Schlüssel bewahrt). Ältere Firmware ignoriert sie.

## 2. Transport zum Coordinator

Pro 10-s-Heartbeat sendet jedes Gerät nach `SFC1`, `SFM1` und `SFH1` zusätzlich ein kleines
**HMAC-authentifiziertes UDP-Paket** über den bestehenden Multicast-Kanal:

```
SFG1|tag|integration_id|boot|presence_sequence|group|position|height_mm|h_deci|v_deci
```

- Leere IDs werden als `-` gesendet, unbekannte Höhe als `-1`; Winkel in Zehntelgrad.
- Der Empfänger akzeptiert das Paket nur für einen bekannten Peer mit gleicher Boot-ID und
  gleicher Presence-Sequenz (wie `SFH1`) und prüft alle Felder streng.
- Die Werte bleiben im Peer-Eintrag erhalten, solange die Boot-ID gleich bleibt. Nach einem
  Neustart gelten sie erst wieder ab dem nächsten `SFG1`.
- Ältere Firmware verwirft das unbekannte Magic stillschweigend. Ein solcher Node erscheint
  mit `mount_known:false` und wird als „unbekannt (ältere Firmware)“ angezeigt; der
  Clusterbetrieb ist davon nicht betroffen.
- **ESP-NOW wird nicht benutzt.** Präzisionssync, Slots und Recovery-Scheduling sind unverändert.

`/cluster_status` enthält je Gerät: `mount_known`, `group_id`, `mount_position`,
`height_mm` (oder `null`), `orientation_horizontal_deg`, `orientation_vertical_deg`.

## 3. Coordinator: Gruppenverwaltung

Seite **Cluster koordinieren** (`/cluster_coordinate`), aufklappbarer Bereich **„Gruppenverwaltung“**.

- Gruppen werden automatisch aus den `group_id` der online gemeldeten Geräte erkannt. Das
  schließt den Coordinator selbst ein.
- Pro Gruppe editierbar: `orientation_deg` (0–359,9°) und `distance_mm` (0–1000000, Abstand
  zum Objekt). Beide Werte sind optional.
- Speicherung **nur lokal am Coordinator in NVS** (Namespace `sfgroups`, ein Blob mit
  max. 32 Gruppen und Versions- und Magic-Prüfung). Ein beschädigter Blob ergibt eine leere
  Tabelle und blockiert nichts.
- NVS wird **nur** bei ausdrücklichem Speichern oder Löschen in der WebConfig geschrieben,
  nie aus der Cluster-Schleife.

Zustände:

| Zustand | Bedeutung |
|---|---|
| Neu | Nodes melden die Gruppe, es sind noch keine Werte gespeichert |
| Bestätigt | gespeichert und für die aktuelle Nutzung bestätigt |
| Vorschlag | gespeicherte Werte nach einer Abwesenheit der Gruppe von mindestens 10 min; mit „Bestätigen“ übernehmen oder anpassen |
| Inaktiv | gespeichert, derzeit kein Node online; Werte bleiben erhalten, Löschen nur in diesem Zustand |

Regel für „Vorschlag“: Eine Gruppe verliert ihre Bestätigung, wenn der Coordinator sie
mindestens 10 min lang ohne Node beobachtet hat (auch: nach dem Coordinator-Start 10 min nicht
gesehen). Kurze Ausfälle wie ein Node-Neustart behalten die Bestätigung. Diese Information
liegt im RAM und wird beim nächsten Speichern in der Gruppenverwaltung persistiert.

Routen (nur Admin; nur am aktiven Coordinator, sonst 403):

| Route | Zweck |
|---|---|
| `GET /cluster_groups` | Gruppen-JSON |
| `POST /cluster_group_save` | Felder `group_id`, `orientation_deg`, `distance_mm` |
| `POST /cluster_group_delete` | Feld `group_id`; nur ohne Nodes online |

## 4. Filter und Auswahl

Über der Gerätetabelle der Coordinator-Seite steht eine kombinierbare Filterleiste (UND-Verknüpfung):

- Gruppe (alle / ohne Gruppe / konkrete Gruppe)
- Position (Liste, z. B. `P01,P02`)
- Höhe min/max
- unter „Ausrichtung“: horizontale und vertikale Winkel min/max

Geräte ohne den gefilterten Wert, etwa mit unbekannter Höhe oder älterer Firmware, fallen bei
diesem Kriterium heraus.

- Nicht passende Zeilen werden ausgeblendet **und aus der Auswahl entfernt**. Sammelaktionen
  wirken daher nur auf sichtbare Nodes.
- „Treffer auswählen“ ersetzt die Auswahl durch alle sichtbaren, auswählbaren Nodes.
- „Alle sichtbaren Nodes auswählen“ respektiert den Filter.
- Die Spalte „Montage“ zeigt Gruppe, Position, Höhe und Winkel.

## 5. Dateien

- `cluster_group_logic.h` (neu): reine Logik ohne Arduino-Abhängigkeit. Enthält
  Normalisierung, Parser, Paketfelder, Gruppentabelle und Aktivitäts- bzw. Bestätigungsregeln.
- `cluster_groups.cpp/.h` (neu): NVS-Persistenz, RAM-Aktivität, JSON, Speichern und Löschen.
  Die Tabellen liegen lazy im PSRAM über `psram_alloc.h`; interner RAM wird nicht zusätzlich
  statisch belegt.
- `webconfig_cluster_groups.cpp/.h` (neu): Montageformular, Gruppenbereich, Filterleiste, Routen.
- `cluster.cpp/.h`: `SFG1` senden und empfangen, Peer-Felder, Status-JSON,
  `clusterMountSnapshot()`.
- `config.cpp/.h`: fünf Schlüssel und `configSaveClusterMount()`.
- `webconfig_cluster.cpp`: Einbindung (Formular, Bereich, Filter, Spalte „Montage“, Auswahl-Button).
- `test_cluster_groups.cpp.test` (neu): Host-Test.

## 6. Grenzen und offene Punkte

- **Gruppendaten liegen am Coordinator-Gerät.** Übernimmt ein anderes Gerät die
  Coordinator-Rolle, sieht es dessen eigene, meist leere Tabelle. devxiao3 bleibt Coordinator.
  Es gibt keinen Export und keine Replikation.
- **Abwesenheit bei ausgeschaltetem Coordinator** ist nicht beobachtbar. Kehrt eine Gruppe
  innerhalb von 10 min nach dem Coordinator-Start zurück, bleibt sie bestätigt, auch wenn sie
  zwischenzeitlich umgebaut wurde.
- RAM-Invalidierungen („Vorschlag“) werden erst beim nächsten Speichern in der
  Gruppenverwaltung persistiert. Das ist bewusst so, damit es keine automatischen
  Flash-Schreibvorgänge gibt.
- Max. 32 gespeicherte Gruppen; die Peer-Tabelle bleibt bei 16. Für „sehr viele Nodes“
  begrenzt die bestehende Cluster-Peer-Tabelle, nicht die Gruppenverwaltung.
- Die Gruppen-Ausrichtung, der Abstand und die Node-Winkel werden **nur gespeichert und
  angezeigt**. Es gibt keine Geometrieberechnung und keine Prüfung gegen echte Messungen.
- Der Mount-Status ist nicht in `/api/v1` enthalten, nur in `/cluster_status` und `/cluster_groups`.
- Hardware-Test steht aus: Die Firmware ist nur kompiliert und die Logik host-getestet.
- **Flash-Reserve:** Die Erweiterung kostet ca. 32 KB Flash. Freenove belegt jetzt
  3.286.127 von 3.342.336 B (98,3 %, ca. 55 KB frei), XIAO 3.266.371 B, diymore 3.274.803 B.
  Statischer interner RAM: +816 B je Board (Peer-Felder).
