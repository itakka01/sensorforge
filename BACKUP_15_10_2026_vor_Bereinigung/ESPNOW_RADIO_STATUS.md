# ESP-NOW Funktransport (Diagnosephase)

- 10-Sekunden-HMAC-Broadcasts vom Coordinator nur bei aktiviertem Cluster, Drone Mode und verbundener Wi-Fi-STA.
- Kein Kanalwechsel; bei Kanal-/Coordinatorwechsel wird der Transport zurückgesetzt.
- Funk-Callback legt ausschließlich unverifizierte Rohpakete in eine begrenzte Queue. Authentifizierung und Replay-Check erfolgen im normalen Cluster-Loop.
- Eine Funkbestätigung belegt weder Empfang durch die Anwendung noch korrekte Zeitreferenz.
- **Noch keine Synchronisation:** Zeitstempel sind rein diagnostisch; keine Übernahme in Cluster-Uhr, keine Präzisionsmessserien, keine Driftaktualisierung und keine Coordinator-UI.
- Das Modul sendet nur während Drone Mode. Zusätzlicher Funkverkehr kann zeitkritische Jobs stören; Messungen sind nicht präzisionsvalidiert.
- Firmware-Build und Hardwaretests für beide Boards erforderlich.
