# SensorForge beta115 – HMAC- und Rollen-Audit

Stand: 14.10.2026. **Nicht flashen. Keine Änderung an der Firmware-Version.**

## Konkrete Quellcodebefunde

- `cluster.cpp` verwendet PSA Crypto HMAC-SHA256 (`psa_mac_compute`) mit einem Cluster-Laufzeitschlüssel.
- ESP-NOW-Zeitpakete nutzen bereits eine abgeleitete Domänentrennung `SensorForge/ESP-NOW/time/HMAC/v1`.
- Das neue Übergabeprotokoll besitzt bislang nur `Sign`/`Verify`-Callbacks und wird noch nicht vom Produktivpfad aufgerufen.
- Ein gemeinsamer Cluster-HMAC-Schlüssel authentifiziert die Mitgliedschaft, **nicht** die Coordinator-Rolle des Senders: ein anderer Clusterteilnehmer kann ebenfalls ein MAC mit einem gemeinsam bekannten Schlüssel erzeugen.

## Durchgeführter nativer Test

OpenSSL-HMAC-SHA256 mit 32-Byte-Testschlüssel über den tatsächlichen Übergabe-Codec; gültiges Paket akzeptiert, manipulierte Transaktion, falscher Schlüssel, fehlerhafte Paketlänge und doppelter Replay-Eintrag abgelehnt. `g++ -std=c++17 -Wall -Wextra -Werror -pedantic ... -lcrypto` erfolgreich.

## Noch zwingend vor Firmware-Integration

1. Rollenprüfung zusätzlich zum HMAC: Ein `Grant` muss vom tatsächlich gewählten Cluster-Coordinator stammen. Quelle und Sequenz allein genügen nicht; Identitätsbindung muss an den authentifizierten Transportpfad erfolgen.
2. Transaktionsbindung: `Active` muss zu gültigem `Grant` und identischem Kandidaten-Boot/Quelle/Epoche passen, inklusive Ablaufzeit.
3. Zeit-Beacons einer eigenständigen Quelle benötigen neue Sender-/Epoch-Verifikation; bisherige Coordinator-Beacons dürfen nicht stillschweigend als fremde Quelle gewertet werden.
4. Gerätemodus darf Kamera/Recording nur nach sauberer Konfigurationspersistenz und Neustart überspringen; Failover darf vorhandene Capture-Jobs nicht beeinflussen.
5. Vollständiger Arduino-ESP32-Build, HTTP/Heap/Cluster/Capture-Hardwaretest und ein Canary-Gerät vor Verteilung.

## Wiederholung des nativen Tests

```sh
g++ -std=c++17 -Wall -Wextra -Werror -pedantic cluster_time_handover_protocol.cpp test_time_handover_hmac.cpp -lcrypto -o /tmp/sf115_hmac && /tmp/sf115_hmac
```

**Ergebnis:** Kryptographische Paketintegrität getestet; Senderberechtigung und End-to-End-Integration ausdrücklich nicht getestet. Keine produktiven Dateien wurden geändert.
