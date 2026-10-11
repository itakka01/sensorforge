# Audit-Paket 5s – Recorder/SD-Lifecycle: Übergabeprotokoll

**Befund (P1, bestätigt):** `recorderStart()` hält die `RecorderStart`-Lease nur während Dateiöffnung. `recorderAddFrame()` und `recorderEnd()` besitzen keine Admission-Lease. Die gemeinsame SD-I/O-Koordination ist damit unvollständig.

**Wichtige Architekturabhängigkeit:** `webSdQueueClusterWipe()` reserviert eine zukünftige Wartung über `ClusterWipe`; `webSdMaintenanceLoop()` ruft *danach* `stopRecording()` auf. Eine exklusive `RecorderStart`-Lease über die komplette Aufnahme würde solche Wipes bereits bei der Reservierung zurückweisen, während Recording aktiv ist. Eine naive Verlängerung der bestehenden Lease ist daher **nicht** kompatibel mit dem dokumentierten Cluster-Verhalten.

**Erforderliches Übergabeprotokoll (noch nicht implementiert):**
1. Aufnahme-I/O besitzt eine Laufzeit-Registrierung (aktive Schreiber, inklusive asynchroner Drain-Tasks), getrennt von zukünftigen Wartungsreservierungen.
2. Eine angenommene Cluster-Wipe-Reservierung sperrt atomar **neue** Schreibstarts, darf aber laufende Aufnahme finalisieren lassen.
3. Vor Destruktion/Remount wird `stopRecording()` ausgeführt und auf das bestätigte Ende sämtlicher SD-I/O, Drain-Tasks und Datei-Handles gewartet.
4. Erst nach nachgewiesener Quiescence erhält Wartung exklusiven SD-Zugriff. Bei Timeout: **kein** Wipe/Remount; definierten Fehlerstatus melden und Reservierung kontrolliert freigeben.
5. Lease-Tickets dürfen nie von fremden Eigentümern freigegeben werden; Fehler- und Abbruchpfade müssen abgedeckt sein.

**Tests für die spätere Implementierung:** Aufnahme offen → Cluster-Wipe vorab annehmen → neue Aufnahme verweigern → alte Aufnahme finalisieren → Drain vollständig beenden → Wipe starten; ebenso Drain-Timeout, Fehler beim Finalisieren, doppelter Wipe, Secure Erase/Benchmark während laufender Aufnahme, Reboot/Stop während Pending-Wipe.

**Keine Änderung des produktiven Codes in 5s:** Für die vollständige Laufzeitkoordination muss erst die Quiescence-API über `recorder.cpp`, `recording_write_buffer.cpp` und `web_sd_maintenance.cpp` abgestimmt werden. Eine isolierte Lease-Änderung wäre regressionsgefährlich.

**Validierung:** Rein statischer Pfadabgleich. Weder ESP32-Build noch Hardwaretest.
