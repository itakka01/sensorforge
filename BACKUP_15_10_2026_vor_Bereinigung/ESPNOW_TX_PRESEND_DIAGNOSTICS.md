# ESP-NOW TX-Software-Vorlaufdiagnose (v87-beta105)

**Basis:** letzter integrierter Stand (17:00:58 ZIP), plus die bereits gelieferte `selected_quartile_diagnostics`-Erweiterung. Dieses Delta baut **auf diesem Stand** auf. Vollständige Ersatzdateien; kein Git-Patch.

**Neue lokale `/cluster_status`-Felder** innerhalb `espnow_time_local`:
- `probe_tx_pre_send_last_us`, `probe_tx_pre_send_max_us`, `probe_tx_pre_send_samples`, `probe_tx_pre_send_mean_us`: Node, von t1 bis unmittelbar vor esp_now_send(Probe)
- `reply_tx_pre_send_last_us`, `reply_tx_pre_send_max_us`, `reply_tx_pre_send_samples`, `reply_tx_pre_send_mean_us`: Coordinator, von t3 bis unmittelbar vor esp_now_send(Reply)

Die Messung schließt Protokollkodierung/HMAC, TX-Pending-Registrierung und lokale Vorbereitung ein; sie **enthält nicht** die nicht sichtbare Funk-/Treiber-Wartezeit nach dem Aufruf. `esp_now_send()`-Rückgabestatus kann fehlgeschlagen sein; die Aufrufvorlaufzeit wird trotzdem gezählt. Die angezeigten Werte werden bei Neustart der lokalen ESP-NOW-Radioinstanz zurückgesetzt. Der Mittelwert ist ein Laufzeitmittel aller seit dem letzten Reset betrachteten Probe-/Reply-Sendeversuche, **nicht** ein Mittelwert einer einzelnen abgelehnten Serie. Ein Last/Max-Vergleich allein beweist deshalb keine Kausalität.

**Unverändert:** Zeitstempel t1/t2/t3/t4, HMAC/Funkprotokoll, Slot-Vergabe, Sync-Qualitätsgrenze 250 us, Offset-Estimator, Driftberechnung, Capture-Priorität. Kein zusätzliches ESP-NOW-Paket; keine Persistenz.

**Flash:** Für Probe-Diagnose Nodes; für Reply-Diagnose **auch Coordinator**. Daher empfohlen alle vier Boards zu aktualisieren. Der Coordinator benötigt die neuen Felder nur über seine eigene `/cluster_status`-API, nicht in der normalen Web-Clusteranzeige.

**Auswertung:** Nach ca. 15 min alle drei Node-JSON und Coordinator-JSON sammeln. Vergleichen, ob die lokalen Vorlauf-Maxima und Mittelwerte von Probe und Reply im Bereich der Offset-Streuung liegen. Falls beide stabil klein sind, ist der Treiber-/Funkpfad als Kandidat wichtiger, aber nicht bewiesen.

**Prüfungen:** statischer Quellen-/ZIP-Vergleich; kein vollständiger ESP32-Build oder Hardwaretest.
