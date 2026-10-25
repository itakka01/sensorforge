# SensorForge – Host-Tests

Die Tests sind **nicht** Teil des Arduino-Builds. Die Endung `.cpp.test` verhindert, dass die Arduino IDE sie mitkompiliert (jeder Test hat ein eigenes `main()`). Kompiliert wird mit `g++ -x c++` direkt aus dem Sketch-Ordner, ohne Umbenennen.

| Test | Prüft | Zusätzliche Quellen |
|---|---|---|
| `test_beacon_sync.cpp.test` | Zeitsignal-Modell und Simulation 9/100 Nodes | `cluster_beacon_sync.cpp` |
| `test_fixed_series.cpp.test` | Zwei-Wege-Schätzer (16er-Serie) der Laufzeit-Einmessung | `cluster_espnow_twoway.cpp` |
| `test_slot_scheduler.cpp.test` | Slot-Planung der Einmessung, Simulation bis 140 Nodes | – |
| `test_v2_protocol.cpp.test` | ESP-NOW-Paketcodec, HMAC-Rahmen, Replay | `cluster_espnow_packets.cpp` |
| `test_v2_espnow_size.cpp.test` | Paketgröße 88 Byte | `cluster_espnow_packets.cpp` |
| `test_cluster_capture_pulse.cpp.test` | Testimpuls-Option in SFJC1, Ereignisbildung des GPIO-Messgeräts | – |
| `test_cluster_jobs.cpp.test` | Job-Tabelle (100 Nodes) | `-Itest_stubs` |
| `test_cluster_groups.cpp.test` | Gruppen-/Montagedaten | – |
| `test_cluster_camera_report.cpp.test` | Kamera-Bericht SFK1 | – |
| `test_psram_alloc.cpp.test` | PSRAM-Zuteilung ohne internen Rückfall | `-Itest_stubs` |
| `test_wifi_power_policy.cpp.test` | WLAN-Stromsparmodus je Rolle | – |
| `test_day_archive_tools.cpp.test` | EXAPAN-Konverter im Tages-ZIP | – |
| `test_camera_tuning.cpp.test` | Textformat der Bildabstimmung (`camera_tuning`) | – |
| `test_camera_profile.cpp.test` | Textformat des Cluster-Kameraprofils | – |
| `test_jpeg_mini_encoder.cpp.test` | Kleinbild-JPEG-Encoder; schreibt JPEGs zur Prüfung mit Pillow | `jpeg_mini_encoder.cpp` |

Beispiel:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -I. -x c++ test_beacon_sync.cpp.test -x none cluster_beacon_sync.cpp -o /tmp/test_beacon_sync && /tmp/test_beacon_sync
```

Bei Tests mit zusätzlichen Quellen muss vor den `.cpp`-Dateien `-x none` stehen, sonst behandelt `g++` sie ebenfalls als Testquelle mit Endung `.test`.
