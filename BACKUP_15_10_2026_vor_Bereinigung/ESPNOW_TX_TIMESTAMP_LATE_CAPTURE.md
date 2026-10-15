# SensorForge v87-beta105 – ESP-NOW Software-TX-Zeitstempel später erfassen

## Grundlage

Vollprojekt `sensorforge_2026-10-13_17-00-58.zip`, anschließend die bereits verwendeten Deltas
`espnow_last_reject_diagnostics_integrated_delta`, `espnow_selected_quartile_diagnostics_delta`
und `espnow_tx_presend_diagnostics_delta` in dieser Reihenfolge.

## Datei zum Ersetzen

- `cluster_espnow_radio.cpp` – vollständige Ersatzdatei, kein Diff

## Änderung

- Die TX-Callback-Buchhaltung und die Sequenznummer werden vor der Zeitstempelaufnahme vorbereitet.
- Auf dem Node wird `t1` unmittelbar vor der HMAC-signierten Probe-Kodierung erfasst.
- Auf dem Coordinator wird `t3` unmittelbar vor der HMAC-signierten Reply-Kodierung erfasst.
- Im Reply bleibt `timeUs=t2`; erst das folgende Final trägt das aufgezeichnete `t3`.
- Die Felder `probe_tx_pre_send_*` / `reply_tx_pre_send_*` bleiben erhalten und messen nun den verkürzten Vorlauf.
- Keine Änderung an Paketlayout, Authentifizierung, Empfangszeitstempeln, Slotvergabe, Driftmodell,
  Capture-Prioritäten oder der 250-µs-Grenze.

## Grenzen

`t1` und `t3` bleiben Software-Zeitstempel. Die HMAC-Kodierung MUSS für die authentifizierte
Probe nach `t1` erfolgen. Wartezeiten im Wi-Fi-Treiber und vor dem RF-Senden werden nicht erfasst.
Der Effekt auf Erfolgsquote und reale Kamera-Synchronität ist noch NICHT hardwareverifiziert.

## Test

Alle vier Boards mit identischem Projektstand bauen und flashen. Danach 15–30 Minuten drei Nodes
im Drone Mode ohne offene Webseiten laufen lassen. Anschließend die kumulativen Erfolgs-/Fehlerzähler,
die aktiven Zeitquellen und aus `/cluster_status` aller vier Geräte die neuen `*_tx_pre_send_*`-
Messwerte vergleichen. Boot-IDs unverändert lassen. Für tatsächliche Auslösesynchronität gemeinsame
GPIO-/Logic-Analyzer-Messung vornehmen.

## Prüfstatus

Paketintegrität und Quelltext-Invarianten geprüft. Kein vollständiger ESP32-Compile oder Hardwaretest
in dieser Umgebung.
