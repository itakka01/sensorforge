# SensorForge beta115 – Handshake-Paketcodec, Entwicklungsstand 14.10.2026

**Nicht flashen.** Keine Firmware-Dateien ersetzt, keine Versionskennung erhoeht.

## Neu
- Eigenes, 80 Byte grosses, versionsgebundenes Handover-Frame `SFTH` mit getrenntem Pakettyp (Offer/Grant/Active/Revoke).
- Quell-/Boot-/Clusterkennung, Epoche, monotone Sequenz, Transaktion und begrenzte Lease-Dauer.
- HMAC-SHA256-Signatur- und Verifikationscallbacks, die bei Integration mit der vorhandenen Cluster-Schluesselableitung verbunden werden muessen.
- Receiver-seitiger Replay-Zustand; nach Boot-/Epochenwechsel nur nach extern authentifizierter Neuinitialisierung zuruecksetzen.
- Native Tests fuer Roundtrip, manipuliertes Paket, falsche Laenge, Lease-Grenzen und Replay.

## Test
`g++ -std=c++17 -Wall -Wextra -Werror -pedantic cluster_time_handover_protocol.cpp test_cluster_time_handover_protocol.cpp -o /tmp/sf_handover_test && /tmp/sf_handover_test`

Die Testdatei verwendet **nur einen kuenstlichen MAC-Pruefwert**. Das ist *keine* kryptographische Implementierung und kein HMAC-Nachweis.

## Noch zwingend erforderlich
1. Separater, persistenter Board-Betriebsmodus und sichere Kamera-/Recording-Deaktivierung.
2. Authentifizierte Cluster-Service-Ankuendigung mit festem `time_broadcaster`-Dienst.
3. HMAC an bestehenden Sicherheitsschluessel koppeln; Autorisierung des Senders nach Rolle und Cluster; pro authentifizierter Senderidentitaet einen Replay-Zustand verwalten.
4. Zwei-Phasen-Uebergabe, explizite Coordinator-Bestaetigung, gemeinsamer Aktivierungszeitpunkt und sicherer Timeout/Fallback.
5. Beacons mit eindeutiger Zeitquellenidentitaet und eigener Epoche; Clock-Reset bei Wechsel; alte Coordinator-basierte Beacons nicht stillschweigend akzeptieren.
6. Vollstaendiger Arduino-Build mit vorgesehenem Boardpaket, HTTP/Heap-Regression und Hardware-Canary.

Der Codec allein loest **keine** Coordinator-Uebergabe aus und darf nicht in bestehende ESP-NOW-RX-Routinen gehaengt werden, bevor diese Validierungen implementiert sind.
