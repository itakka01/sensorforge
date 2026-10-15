# SensorForge beta115 – Integrationsstand (NICHT FLASHEN)

Datum: 2026-10-15

## Bereits in Firmwarequellen integriert
- `config.cpp`, `config.h`, `sensorforge.ino`: `time_broadcaster` als Betriebsmodus und gesonderte Loop-/Startup-Behandlung; normale Kamera-/Radar-/Recording-Pfade werden übersprungen.
- `cluster.cpp`: Broadcaster von der Cluster-Coordinator-Kandidatur ausgeschlossen.
- `cluster_espnow_radio.h/.cpp`: passiver Beacon-Empfang im Broadcaster-Modus; keine normalen Node-Probes/Reports in diesem Zustand.
- `webconfig.cpp`: statische, sehr kleine Startseite im Broadcaster-Modus mit Konfigurationslink; Konfigurationsformular akzeptiert `time_broadcaster`.
- `webconfig_streamer.cpp`: sichtbare Modusauswahl, ausdrücklich als Diagnose-Standby gekennzeichnet.
- `cluster_time_broadcaster_mode.h`: Sendefreigabe standardmäßig gesperrt.

## Noch offen – keine Firmwarefreigabe
- Eigene HMAC-authentifizierte Beacon-Identität und Replay-Prüfung.
- Coordinator-Berechtigung, Grant/ACK/Lease im echten Transport.
- Beacon-Versand durch dedizierten Sender, bestätigte Übergabe und sicherer Fallback.
- Vollständiger Arduino-ESP32-Build und Hardwaretests.

Der Modus ist derzeit **kein einsatzfähiger Zeit-Broadcaster**. Nur Entwicklungsstand; nichts daraus flashen. Produktive Capture-Zeitquelle nicht verändert. Die Firmware-Versionskennung bleibt bis zur vollständigen beta115-Freigabe unverändert.

## 2026-10-15 – weiterer konkreter Integrationsschritt
- `cluster_espnow_radio.cpp`: Authentifizierte Coordinator-Beacons besitzen nun einen separaten Replay-Zustand (`coordinatorBeaconReplay`) neben den bisherigen Steuerpaketen. Beide Fenster werden gemeinsam bei `resetTiming()` zurückgesetzt.
- `test_beacon_replay_isolation.cpp`: Nativer C++-Test für unabhängige Sequenzfenster, Zurückweisung von Duplikaten und Ablehnung eines neuen Epochenwerts ohne expliziten Reset; erfolgreich mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic`.
- Wichtig: Diese Trennung aktiviert **keinen** fremden Zeit-Broadcaster. Die bestehende Coordinator-ID-/Epochenprüfung bleibt unverändert. HMAC-gesicherte Übernahme, Senderbetrieb und Fallback im echten Transport sind weiterhin ausstehend.
- Vollständiger Arduino-ESP32-Build nicht durchgeführt. Entwicklungsstand **NICHT FLASHEN**.

## 2026-10-15 – echte Firmware-HMAC-Brücke
- `cluster.cpp`/`cluster.h`: Neue Signier- und Verifikationsadapter für das Zeitquellen-Übergabeprotokoll. Der bestehende geschützte ESP-NOW-Zeitschlüssel wird mit einem eindeutigen Handover-Domainpräfix verwendet, ohne Schlüsselmaterial herauszugeben. Temporäre MAC-/Nachrichtenpuffer werden gelöscht.
- Der HMAC allein berechtigt **keine** Übernahme; Coordinator-Rolle, Transaktion, Lease, Replay und aktive Quelle müssen weiterhin im Empfangspfad geprüft werden.
- Noch **nicht** mit dem RX-/TX-Funktransport verdrahtet. Keine Freigabe, kein Firmware-Release. Vollständiger Arduino-Build offen.

## 2026-10-15 – ESP-NOW receive-only wire integration
- `cluster_espnow_radio.cpp`: RX-Callback erlaubt jetzt zusätzlich 80-Byte-Handover-Pakete (bisher ausschließlich 76 Byte); bestehende 76-Byte-Pakete behalten ihre Behandlung.
- Die bestehende RX-Queue bekommt eine Längenangabe und vier weitere Byte pro Eintrag. Die HMAC-Verifikation erfolgt im Service-Task, NICHT im WLAN-Callback.
- Gültige Übergabenachrichten werden bewusst verworfen, bis authentisierte MAC-zu-Cluster-Identitätsbindung, Autorisierung, Lease, Replay und ACK im echten Pfad komplett implementiert sind. Kein Senderwechsel freigegeben.
- Nativer Wire-Codec-Test (80/76 Byte, gültiges Paket, manipuliertes Paket): bestanden. Vollständiger Arduino-Build und Hardwaretest: nicht durchgeführt. NICHT FLASHEN.

## 2026-10-15 – Korrektur einer regressiven 76/80-Byte-Paketprüfung
- **Bestätigter Integrationsfehler:** Der 80-Byte-Empfangspuffer wurde mit `sizeof(item.bytes)` an den 76-Byte-Standarddecoder übergeben; dieser verlangt strikt `len == 76`. Dadurch wären sämtliche normalen ESP-NOW-Pakete abgewiesen worden.
- **Korrigiert** in `cluster_espnow_radio.cpp`: `EspNowTimePackets::decode(item.bytes,item.length,...)`. Das tatsächliche Paketlängenfeld wird bereits im RX-Callback gesetzt.
- Neuer nativer Regressionstest `test_espnow_dual_packet_lengths.cpp` prüft gültige 76- und 80-Byte-Nachrichten, Ablehnung falscher Längen und HMAC-manipulierter Pakete. Erfolgreich mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic`.
- Sicherheitsgrenze bleibt: Empfangene Handover-Pakete werden weiterhin **nicht** zum Freigeben des Broadcasters verwendet. Keine Produktivfreigabe, kein Arduino-Build, **NICHT FLASHEN**.

## 2026-10-15 – Coordinator-Grant-Replay-Fix
- `cluster_time_handover_authority.h/.cpp`: pro Coordinator-Boot-ID monotone Sequenzgrenze für Grant/Revoke. Gleiche oder ältere Grants werden abgewiesen und können eine Lease auch nach Widerruf nicht verlängern. Neue Coordinator-Boot-ID setzt das Fenster zurück.
- `test_handover_authority_replay.cpp`: prüft Replay von Grant, Replay nach Widerruf, veraltetes Revoke und Coordinator-Neustart. Native C++17-Kompilierung mit `-Wall -Wextra -Werror -pedantic` bestanden.
- Achtung: Die Sequenzgrenze muss beim späteren Anbinden an den Funktransport mit der tatsächlichen Coordinator-Sequenzvergabe vereinbar sein. Authentisierte MAC-zu-Actor-Auflösung, Übergabe-TX, Zeitbeacon-Freigabe und Hardware-Build noch offen. NICHT FLASHEN.

## 2026-10-15 – authentifizierte Akteur-Identität im Übergabeprotokoll (nur Entwicklung)
- Wire-Protokoll **Version 2**: `actorId`, `actorBoot`, `actorEpoch` ergänzt; Body 68 Byte, HMAC 32 Byte, **100 Byte gesamt**. Alte 80-Byte-Entwicklungspakete werden nicht als gültige v2-Pakete akzeptiert.
- `Authority`: Grant und Revoke verlangen Coordinator-Identität sowohl im signierten Nachrichtentext als auch in der vom Aufrufer vertrauenswürdig gelieferten Senderidentität; Active verlangt entsprechend die Broadcaster-Identität. Target-`source*` bleibt getrennt.
- Empfangspuffer passt sich `kWireBytes` an; Queue-Length auf `uint16_t` erweitert.
- Native Tests `test_handover_authority_replay.cpp` und `test_espnow_dual_packet_lengths.cpp` mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` bestanden.
- **Sicherheitsgrenze:** `actor*` im HMAC-Paket ist keine eigenständige Senderauthentisierung. Eine verifizierte MAC↔Cluster-Geräteidentität und Coordinator-Rolle müssen aus vertrauenswürdigem Clusterzustand geliefert werden. Empfangspfad verwirft Handover-Nachrichten weiterhin (fail-closed). Nicht flashbereit, Arduino-Gesamtbuild ungeprüft.

## 2026-10-15 – fail-closed MAC-/Identitätsbindung (Entwicklungsstand)
- `cluster_time_sender_binding.h`: feste, kleine Tabelle für maximal zwei Zeitrollen (Coordinator/Broadcaster); unbekannte MACs, nicht bestätigte Einschreibungen, doppelte IDs und MACs werden abgewiesen. Beim Rollen-/Bootwechsel ist ein expliziter Reset nötig.
- `test_handover_sender_binding.cpp`: nativer Test mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` erfolgreich.
- **Noch nicht angeschlossen**: Die vorhandenen Cluster-Presence-Datensätze enthalten keine sicher nachgewiesene Bindung zwischen ESP-NOW-Funk-MAC und Geräteidentität. Ein `authenticatedEnrollment=true` darf daher NICHT aus einem empfangenen Handover-Paket, nur aus einer validierten Cluster-Ankündigung oder aus einer MAC-Adresse abgeleitet werden. Erforderlich bleibt ein separat authentisierter Enrollment-/Challenge-Ablauf mit Coordinator-Autorisierung.
- Kein Senderwechsel freigegeben, Arduino-Gesamtbuild ungeprüft, NICHT FLASHEN.

## 2026-10-15 – Fail-closed Senderprüfung im realen RX-Pfad
- `cluster_espnow_radio.cpp` verwendet jetzt die vorhandene `SenderBinding`-Tabelle für eingehende, gültig HMAC-geprüfte Handover-v2-Nachrichten.
- Zusätzlich müssen `actorId`, `actorBoot` und `actorEpoch` mit dem vertrauenswürdig registrierten Funkabsender übereinstimmen. Unbekannte/abweichende Absender erhöhen `stats.rejected`.
- `resetTiming()` löscht auch alle Senderbindungen; bei Epochen-/Rollenwechsel ist eine erneute authentifizierte Einschreibung nötig.
- Sicherheitssperre: Weil noch keine authentifizierte Enrollment-Strecke integriert ist, bleibt die Tabelle leer und sämtliche Übergabepakete werden abgewiesen. Es erfolgt keine Zeitquellenumschaltung.
- Der native `test_handover_sender_binding.cpp` wurde mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` kompiliert und erfolgreich ausgeführt. Ein Arduino-Gesamtbuild steht aus.
- **NICHT FLASHEN:** Betrieb mit dediziertem Zeit-Broadcaster ist weiterhin unvollständig. Nächste Voraussetzung: bestätigter Enrollment-/Challenge-Ablauf im echten Cluster-Transport und anschließend Grant/ACK/Lease-Anbindung.

## 2026-10-15 – Registrierungsgrenze nachgeprüft
- Neuer nativer Regressionstest `test_handover_fail_closed_integration.cpp` bestätigt: ohne explizit verifizierte Einschreibung bleibt die Senderauflösung gesperrt; bei `reset()` erlischt die Bindung.
- Kompiliert und ausgeführt mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` (PASS).
- Wichtige Einschränkung: `authenticatedEnrollment=true` wird im Test bewusst simuliert; die echte authentifizierte Einschreibung im Clustertransport ist weiterhin **nicht implementiert**. Der Test belegt keine erfolgreiche Funkübergabe.
- Firmware nicht freigeben / nicht flashen.

## 2026-10-15 – Enrollment-DoS-Korrektur
- `cluster_time_enrollment_guard.h`: Ein zweiter oder ungültiger `begin()`-Aufruf darf eine bereits laufende, gültige Challenge nicht mehr durch `cancel()` löschen. Solange eine Anmeldung aussteht, wird eine konkurrierende Anmeldung abgelehnt.
- `test_time_enrollment_guard.cpp`: Regression für ungültigen und konkurrierenden Start; Original-Challenge bleibt abschließbar.
- Test: `g++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined` erfolgreich. Keine echte Registrierung im Clustertransport, kein Arduino-Gesamtbuild. NICHT FLASHEN.

## 2026-10-15 – Enrollment-Timeout-Recovery (Entwicklungsstand)
- `cluster_time_enrollment_guard.h`: Nach Ablauf der maximal 30 Sekunden gültigen Challenge wird der Guard bei der nächsten `begin()`-Anfrage freigegeben. Nicht abgelaufene Challenges bleiben vor konkurrierenden Registrierungen geschützt.
- `test_time_enrollment_guard.cpp`: Prüft Erneuerung nach Ablauf inklusive `millis()`-Überlauf, Zurückweisung der alten Challenge und Bestätigung der neuen.
- Nativer Test mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined` bestanden.
- Kein echter Enrollment-Transport und kein Arduino-Gesamtbuild. **NICHT FLASHEN.**
