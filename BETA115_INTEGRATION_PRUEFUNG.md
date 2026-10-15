# SensorForge beta115 – zusammenhängende Integrationsprüfung

Stand: 14.10.2026. **NICHT FLASHEN.** Firmwareversion bleibt unverändert.

## Verifiziert am Code
- `cluster_espnow_radio.cpp`: Beacon-Erzeugung in `ClusterEspNowRadio::service()`, `EspNowTimePackets::Type::Beacon`, Coordinator-gebundene Authentifizierung/Replay-Verarbeitung.
- `cluster.cpp`: `clusterLoop()` ruft `ClusterEspNowRadio::service(...)` auf; bestehende Capture-Schutzprüfung muss Vorrang behalten.
- Cluster-mDNS enthält bereits `mode=cfg_operating_mode`, aber das ist noch keine autorisierte Zeitquellen-Wahl.
- Ein separater Zeitbroadcaster benötigt neue autorisierte Senderidentität und Epoche im Beacon-Verifikationspfad, bevor er verwendet werden darf.

## Ausgeführter nativer Integrationstest
Kompilierung mit `g++ -std=c++17 -Wall -Wextra -Werror -pedantic` erfolgreich, anschließend Test bestanden:
- Wire-Frame Encode/Decode mit **nur künstlichem Test-MAC** (kein echter HMAC-Test)
- Replay-Duplikat verworfen
- Ohne bestätigte Übernahme bleibt Coordinator aktiv
- Mit bestätigter Übernahme wechselt Zeitquelle
- Rivalisierender Sender verdrängt aktive Zeitquelle nicht
- Bei veraltetem Broadcaster werden Coordinator-Beacons sofort wieder freigegeben; Arbiter fällt nach zusätzlicher Wartefrist auf Coordinator zurück

## Noch NICHT gelöst – Freigabesperren
1. HMAC-SHA256 mit echter Cluster-Schlüsselableitung, Senderberechtigung, Transaktion und Replay-Fenster
2. Betriebsmodus `time_broadcaster`: Bootpfad ohne Kamera-/Recording-/Sensorinitialisierung
3. Autorisierte Service-Ankündigung und bestätigter Grant/Active-Handshake
4. Neues, gegenüber dem Coordinator getrenntes Beacon-Senderidentitäts-/Epochmodell; Reset der **diagnostischen** Uhren bei Wechsel
5. End-to-end Arduino-ESP32-Build, Canary-Tests für HTTP/Heap/Capture

## Test lokal wiederholen
```
g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  cluster_time_source_arbiter.cpp cluster_time_handover_protocol.cpp \
  test_time_source_combined.cpp -o /tmp/sf_time_flow && /tmp/sf_time_flow
```

Die vorhandenen Produktivdateien werden durch diesen Stand nicht verändert.
