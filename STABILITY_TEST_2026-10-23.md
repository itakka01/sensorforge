# 6-Stunden-Stabilitätstest vom 23.10.2026

Ergebnisprotokoll, damit spätere Arbeiten ohne erneute Analyse anknüpfen können.
Nur Dokumentation; Firmware und Tests wurden für dieses Protokoll nicht geändert.

## 1. Testaufbau

| Punkt | Wert |
|---|---|
| Lauf | `sensorforge_monitor/sensorforge_tests/run_20261022_233925_803800/` (lokal, per `.gitignore` nicht im Git) |
| Zeitraum | 2026-10-22 23:39:26 UTC bis 2026-10-23 05:39:26 UTC (01:39–07:39 MESZ), 360 min |
| Abfrageintervall | 10 s, 2160 HTTP-Zyklen pro Gerät |
| Monitor | `sensorforge_monitor/sensorforge_test_manager.py`, Seed-Host `devxiao3.local` |
| Ergebnis laut Monitor | `PASS_MONITORING_ONLY` (reine Beobachtung, keine aktiven Trigger/Captures) |
| Firmware-Stand | Commit `2fafb99` („relativ_stabil_vor_6h_test“): v87-beta139 mit Recovery-Slot (`cluster_slot_recovery.h`) und Heap-Optimierung (`psram_alloc.h`, ca. 67 KB statisch nach PSRAM) |
| Modus | Drone Mode mit ESP-NOW-Präzisionssync |

Geräte (Name@Kurz-ID aus dem Monitor):

| Gerät | Board | Rolle |
|---|---|---|
| `devxiao3@c7ff8e30` | XIAO ESP32-S3 Sense | **Coordinator** (Zeitquelle `local_coordinator`, muss Coordinator bleiben) |
| `devxiao1@6f2b156a` | XIAO ESP32-S3 Sense | Node |
| `devxiao2@13718c33` | XIAO ESP32-S3 Sense | Node |
| `c001@b7dbf27e` | diymore ESP32-S3 CAM (N16R8, OV5640), `BOARD_DIYMORE_S3CAM` | Node |
| `devfreenove1@3d336835` | Freenove FNK0085 | Node / passives GPIO-Messgerät |

## 2. Ergebnisse

### Erreichbarkeit und Stabilität
- Alle 5 Geräte: **2160/2160 HTTP-Zyklen (100 %)**, je 1 Boot (kein Neustart), je 1 Epoche (kein Epochenwechsel), Replay-Zähler 0.

### ESP-NOW-Präzisionszeit pro Node

| Node | Anteil Präzisionszeit | Serien | Bewertung |
|---|---|---|---|
| devxiao1 | durchgehend | keine verworfenen | gut |
| devxiao2 | durchgehend | keine verworfenen | gut |
| c001 (diymore) | durchgehend | 283 akzeptiert, 1 verworfen | gut; interne Streuung nahezu auf XIAO-Niveau |
| devfreenove1 | **ca. 64 %** | **235 verworfen**, 57 WiFi-Fallback-Phasen | **wichtigste offene Synchronisationsbaustelle** |

Freenove-Endstand laut Monitor: probes=6182, exchanges=6174, drift_ppm≈21,06; Zeitquellen
wechselnd `espnow_precision` / `wifi_coordinator`. Die 2164 Monitor-„Incidents“ sind fast
ausschließlich `counter_increase` des Zählers `rejected` (Ablehnungen auf Austauschebene,
ca. 10–30 je 10-s-Intervall), nicht 2164 Einzelstörungen.

Damit ist der diymore-Port (Freigabe `BOARD_SUPPORTS_PRECISION_SYNC=1`) im Dauerbetrieb bestätigt.

### Recovery-Scheduler (Coordinator)
- 97 Recovery-Slots vergeben: **40 erfolgreich, 57 fehlgeschlagen**.
- Die Funktion arbeitet real (Slots werden vergeben und retten einen Teil der Serien), ihre
  Wirksamkeit ist aber begrenzt.
- Auffällig: 57 fehlgeschlagene Recovery-Slots = 57 Freenove-Fallback-Phasen. Das legt nahe, dass
  die Fehlschläge überwiegend Freenove betreffen, ist aber **nicht belegt** (siehe Grenzen).

### Interner Heap
- Alle Boards über 6 h stabil, **ca. 82–87 KiB frei**, kein erkennbarer Abwärtstrend.
- Vergleich: c001 hatte vor der Heap-Optimierung nur 20–23 KB frei (größter Block 14 KB,
  Minimum 1 KB). Der statisch nachgewiesene Gewinn von ca. 67 KB ist damit im Betrieb angekommen.

### RX-Queue-Verzögerungen
- In den historischen Diagnosewerten auffällige RX-Queue-Verzögerungen, insbesondere beim
  **Coordinator devxiao3** und bei **Freenove**. Ursache nicht untersucht.

## 3. Grenzen der Aussagen

- **Interne Streuung ist keine gemessene GPIO-Genauigkeit.** Sie beschreibt die Konsistenz des
  Zeitschätzers (RTT/Offset-Serien), nicht den realen Zeitversatz an einem GPIO-Pin oder den
  Belichtungsbeginn einer Kamera. `esp_camera_fb_get()` ≠ Belichtungsbeginn.
- **Recovery-Zähler sind nicht eindeutig einzelnen Nodes zugeordnet.** Die Zähler
  (`slot_*` im Cluster-Status) sind Coordinator-Summen; die Zuordnung zu Freenove ist eine
  Vermutung.
- Reiner Beobachtungstest: keine Captures, keine Trigger, kein WebConfig-/Streaming-Lastprofil,
  keine Recording-Last. Heap-Werte gelten für diesen Betriebszustand.
- Ein einzelner Lauf in einer Nacht (eine Funkumgebung); keine Aussage über Langzeit > 6 h.
- Anteile/Serienzahlen stammen aus der Auswertung des Monitorlaufs (`stability_report.*`,
  `stability.sqlite3`); diese Rohdaten liegen nur lokal.

## 4. Offene Aufgaben (Priorität)

1. **Freenove-Präzisionsprobleme gezielt untersuchen** – warum 235 verworfene Serien und
   57 Fallbacks, während c001 (Freenove-Pin-Klon, anderes Modul) stabil ist. Ansatzpunkte:
   Ablehnungsgründe pro Serie, Antenne/Funkpegel, RX-Queue-Latenz, SD_MMC-/Kamera-Last.
2. **Physische GPIO-/Kamera-Triggergenauigkeit messen** – z. B. GPIO-Testimpuls aller Nodes
   gegen ein gemeinsames Messgerät (Oszilloskop/Logikanalysator); Freenove als passives
   Messgerät nur, wenn es selbst nicht Teil des Messobjekts ist.
3. **Recovery-Wirkung bei Bedarf genauer analysieren** – Zähler pro Node erfassen, um
   Erfolgsquote je Gerät zu belegen.
4. **RX-Queue-Verzögerungen untersuchen** – vor allem Coordinator und Freenove.

Randbedingungen für Folgearbeiten (unverändert): devxiao3 bleibt Coordinator; keine Änderungen
an Präzisionslogik, Recovery-Steuerung, Präzisionsgrenzen oder Protokoll ohne ausdrückliche
Freigabe; der Nutzer kompiliert und flasht selbst über die Arduino IDE.
