# ESP-NOW Zeitpakete: Cluster-HMAC-Anbindung

- `clusterEspNowTimeSign` und `clusterEspNowTimeVerify` können als Callbacks für `EspNowTimePackets::encode/decode` übergeben werden.
- Ableitung eines separaten HMAC-SHA256-Schlüssels aus dem **laufenden Cluster-Schlüssel** mit festem Kontext `SensorForge/ESP-NOW/time/HMAC/v1`.
- Nur bei aktivem Cluster und importiertem Cluster-Schlüssel verwendbar; MAC-Vergleich mit konstanter Laufzeit.
- Kein WLAN-/ESP-NOW-Sende- oder Empfangsbetrieb, kein automatischer Sync, keine Uhränderung in diesem Patch.
- Radio-Callbacks müssen die Pakete in eine begrenzte Queue ablegen; HMAC und Zustandspflege erfolgen außerhalb der Wi-Fi-Task.
- Vor tatsächlicher Integration: Peer-/Coordinator-ID, Epoch und Boot-ID zusätzlich an den authentifizierten Clusterzustand binden; bei Schlüssel-/Coordinatorwechsel Replay-Zustand zurücksetzen; aus Broadcasts allein keine Präzisionssynchronisation behaupten.
- Patch baut auf `SensorForge_beta105_espnow_board_drift_delta.zip` und `SensorForge_beta105_espnow_authenticated_packets_delta.zip` auf.
- Vollständiger ESP32-Build und Funk-/Hardwaretest stehen aus.
