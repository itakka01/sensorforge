# ESP-NOW: lokale Niedrig-RTT-Diagnose (risikoarm)

Basis: aktuell zusammengeführter Quellstand `sensorforge_2026-10-13_17-00-58.zip` mit letzter verworfener Serien-Diagnose. Vollständige Ersatzdateien, kein Git-Patch.

Die 16 (oder 8, wenn 32 Samples) nach RTT ausgewählten Exchanges erhalten zusätzliche Beobachtungswerte: Minimum und Maximum der RTT sowie 10./90. Perzentil der Offsetwerte, jeweils relativ zum ausgewählten Offset-Median. Die Kennzahl `spread` (volle Offset-Spannweite) und der Qualitätsgrenzwert 250 us bleiben **unverändert**. Ein auffälliges volles Range bei enger innerer Perzentilspanne deutet auf wenige Randwerte hin, beweist aber keine tatsächliche Uhrengenauigkeit.

Die Werte werden nur über die lokale `/cluster_status`-JSON-API am jeweiligen XIAO veröffentlicht, unter `espnow_time_local`. Keine neuen Funkpakete, keine Erweiterung des ESP-NOW-Protokolls, keine zusätzliche NVS/SD-Last. Die JSON-Felder heißen `last_series_estimate_available`, `last_series_estimate_age_ms`, `last_series_selected`, `last_series_spread_us`, `last_series_rtt_min_us`, `last_series_rtt_max_us`, `last_series_offset_p10_relative_us`, `last_series_offset_p90_relative_us`.

Die Werte geben die **letzte abgeschlossene Schätzung** an, auch wenn die Qualitätsprüfung sie verworfen hat; nach einem Deadline-Abbruch ohne Schätzung bleiben die vorherigen Diagnosedaten erhalten; ihr Alter bezieht sich auf die letzte tatsächliche Schätzung. Diese Einschränkung nicht mit der letzten gültigen Zeit verwechseln.

Installiere die fünf geänderten Dateien in den zuletzt zusammengeführten Stand. Nur die XIAOs brauchen für diese lokale Diagnose neue Firmware. Coordinator-Code muss nicht aktualisiert werden, falls du keine Builds angleichen willst. Bei einem Test alle drei Node-JSON-Diagnosen erfassen, besonders bei schlechter Erfolgsquote. Das aktuelle Slot-System nicht verändern.

Prüfungen: isolierte C++-Syntaxprüfung des Estimators, überprüfte Änderungsanker und ZIP-CRC. **Kein vollständiger ESP32-Build, kein Hardwaretest.**
