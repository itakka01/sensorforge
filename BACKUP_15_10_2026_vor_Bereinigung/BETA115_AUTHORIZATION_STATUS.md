# SensorForge beta115 – Autorisierung der Zeitquellen-Übergabe

Stand 14.10.2026 – **Entwicklungsbaustein, NICHT FLASHEN**.

Neu: `cluster_time_handover_authority.h/.cpp` bindet die Handover-Transaktion an die im authentifizierten Cluster-Transport ermittelte Coordinator-Identität, Sender-Boot-ID, Epoche, Cluster-Tag, Transaktionsnummer und Freigabezeit.

Ein Grant darf ausschließlich vom aktuell bekannten Coordinator kommen. Die Aktivierung benötigt die zugehörige Transaktion und die tatsächliche Broadcaster-Identität. Coordinator-Epochenwechsel und Fristablauf verwerfen die Freigabe. Eine Rücknahme kann nur der Coordinator auslösen. Der Code ist ohne Heap-Allokation.

Native Prüfung: `g++ -std=c++17 -Wall -Wextra -Werror -pedantic cluster_time_handover_authority.cpp test_time_handover_authority.cpp -o /tmp/sf115_auth && /tmp/sf115_auth` erfolgreich. Getestet: falscher Absender, falsche Transaktion, Fristablauf, Coordinator-Wechsel, Timer-Überlauf, unberechtigtes Revoke.

Sicherheitsgrenze: Diese Prüfungen sind nur wirksam, wenn der Aufrufer die `Actor`-Identität aus einem tatsächlich authentifizierten Transportpfad übergibt. Eine Sender-ID aus dem Handover-Payload ist **keine** Authentizitätsquelle. Replay-Schutz und HMAC müssen vor diesem Modul geprüft werden.

**Noch nicht integriert:** Board-Betriebsmodus und Boot-Bypass für Kamera, Aufnahme und Sensorik; Cluster-Service-Advertising; Transportidentitätsbindung; Wire-Pakettyp; Zeitbeacon-Sender/Empfängerrolle; Arbiter-Anschluss; End-to-End-Failover; Arduino-Gesamtkompilierung. Keine Änderung an produktiver Firmware, Version oder CHANGELOG.
