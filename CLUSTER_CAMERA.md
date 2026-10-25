# Kameras im Cluster (Phase 1 Anzeige 2026-10-23; Phase 2: Bildabstimmung/Übersicht beta155, Profil je Sensortyp beta156)

Phase 1 der zentralen Kamerasteuerung ist **reine Anzeige**. Jede Node meldet Sensortyp,
Fähigkeiten und die tatsächlich wirksamen Kameraeinstellungen. Der Coordinator zeigt sie pro
Node an. Es wird keine Kamera verändert, kein Profil verteilt und keine lokale
Konfiguration geschrieben.

## 1. Datenfluss

1. **Node:** `cameraFillClusterReport()` (in `sensorforge.ino`) liest nur RAM.
   - Liest den Treiberstatus `sensor->status`, die Sensor-ID, die vorhandenen Treiberfunktionen
     und den aktuell programmierten Crop.
   - Ist die Kamera aus, nimmt es die Werte aus der lokalen Konfiguration, mit denen die Kamera
     starten würde (`from_sensor:false`).
   - Kein I2C-Zugriff, kein Setter-Aufruf.
2. **Senden:** `serviceCameraReport()` in `cluster.cpp`, aufgerufen in `clusterLoop()`:
   - prüft einmal pro Sekunde,
   - sendet bei Änderung (höchstens alle 2 s) und sonst alle 30 s,
   - sendet **nie** bei scharfgeschalteter oder in ≤ 5 s fälliger Aufnahme.
3. **Paket:** HMAC-authentifiziertes UDP-Seitenpaket über den bestehenden Cluster-Kanal, kein ESP-NOW:
   ```
   SFK1|tag|integration_id|boot|presence_seq|report_seq|state|caps|img|exp|wb|geo|src
   ```
   - Sieben kommagetrennte Gruppen (`cluster_camera_report.h`); ein leeres Element bedeutet unbekannt.
   - Der Empfänger bindet das Paket an Boot-ID und Presence-Sequenz des Peers (wie `SFH1`/`SFG1`)
     und nimmt pro Boot nur steigende `report_seq` an.
   - Ältere Firmware ignoriert das Paket. Solche Nodes erscheinen als „Keine Kameradaten“.
4. **Coordinator:** `cluster_camera.cpp` hält den letzten Report pro Node. Die Tabelle mit
   24 Einträgen liegt lazy im PSRAM.
   - `GET /cluster_cameras`: nur Admin, nur am Coordinator.
   - Bereich **„Kameras im Cluster“** auf `/cluster_coordinate`, aktualisiert sich alle 10 s,
     solange er geöffnet ist.

## 2. Gemeldete Werte

| Gruppe | Werte |
|---|---|
| Zustand | Kamera an / Standby / aus, Drone Mode, Sensor-PID (Modell), XCLK, Herkunft (Treiber/Konfiguration) |
| Fähigkeiten | max. Auflösung (`esp_camera_sensor_get_info`), Bitmaske der vorhandenen Treiberfunktionen |
| Bild | Auflösung, JPEG-Qualität, Helligkeit, Kontrast, Sättigung, Schärfe, Rauschfilter |
| Belichtung / Verstärkung | Belichtungsautomatik, AEC2, Belichtungswert (Zeilen), AE-Korrektur, Verstärkungsautomatik, Verstärkung, Verstärkungsobergrenze |
| Weißabgleich | Weißabgleich-Automatik, AWB-Gain, WB-Modus (Auto / Sonne / Wolken / Büro / Glühlampe) |
| Geometrie | konfigurierte Drehung, wirksame Spiegelung H/V, Crop (Zoom 1,0/1,5/2,0, Position) |
| Quelle | 2 Bit je Parametergruppe: `local` / `cluster` / `node`; in Phase 1 immer `local` |

**Zur Aussagekraft:**
- Der Treiberstatus enthält die zuletzt gesetzten Werte, keine Rücklesung aus den Registern.
- „Fähigkeit vorhanden“ heißt nur, dass die Treiberfunktion existiert. Bekannte
  Platzhalter-Funktionen sind ausgenommen: Schärfe und Rauschfilter beim OV2640.
- Crop ist der SensorForge-Sensor-Crop und nur beim OV3660 vorhanden.

## 3. Vorbereitung für Phase 2

- **Gruppen:** Die Parametergruppen (`ClusterCamera::Group`) trennen bereits gemeinsame
  Bildparameter von montageabhängigen Werten.
  - Gemeinsam, clusterweit: Auflösung, Qualität, Belichtung, Verstärkung, Weißabgleich, Bild.
  - Montageabhängig, pro Node: Drehung, Spiegelung, Crop.
- **Quelle je Gruppe:** Der Report trägt für jede Gruppe ihre Quelle. Phase 2 kann damit
  „lokal“, „Cluster-Profil“ und „Node-Override“ anzeigen, ohne das Paketformat zu ändern.
- **Grundsatz für Phase 2:** Overrides gelten nur im Drone Mode und schreiben nie in die lokale
  Konfiguration. Nach Verlassen gelten wieder die lokalen Werte. Angewendet wird einmal,
  nicht pro Aufnahme.
  - Anwenden am Ende von `initCamera()`, damit jeder Kamera-Neustart das Profil behält.
  - Wiederherstellen per Kamera-Neustart mit den lokalen Werten.

## 4. Sensorbefunde (aus der Analyse)

- OV2640: max. 1600×1200; feste Belichtung (Zeilen 0–1200) und Verstärkung (0–30);
  Weißabgleich nur Auto oder 4 Voreinstellungen; keine Schärfe, kein Rauschfilter.
- OV3660 (Freenove): max. 2048×1536, zusätzlich Schärfe und Rauschfilter, Sensor-Crop.
- OV5640 (diymore): max. 2592×1944; Projekt nutzt bisher max. 2048×1536.
- **1/500 s:** möglich, aber nur angenähert, weil die Belichtung in Zeilendauern eingestellt
  wird.
  - Die Zeilendauer hängt von Sensor, XCLK und Auflösung ab; die Rasterung liegt bei etwa 2–7 %.
  - Voraussetzung: Belichtungs- und Verstärkungsautomatik aus, fester Weißabgleich.
  - Die Zeilendauer ist pro Sensor am Gerät zu messen, bevor eine angezeigte „1/500 s“
    belegt ist.
  - Rolling Shutter und frei laufende Bildphasen bleiben bestehen.

## 5. Unverändert

ESP-NOW-Präzisionssync, Slots und Recovery, `clusterCapturePhoto()`, Aufnahmeplanung und
-scharfschaltung, Kamera-Initialisierung und -Einstellungen sowie die lokale Konfiguration.
Die einzige Ergänzung in `sensorforge.ino` außerhalb der Report-Funktion: Drei RAM-Variablen
merken sich den zuletzt programmierten Crop (gesetzt in `cameraApplyCropRuntime()`,
zurückgesetzt beim Deinit). Das Kameraverhalten ändert sich dadurch nicht.

## 6. Grenzen / offen

- Es gibt keine Rücklesung aus den Sensorregistern und keine gemessene Zeilendauer. Beides ist
  für Phase 2 bzw. 3 vorgesehen.
- Fähigkeiten sind erst nach dem ersten Kamerastart seit dem Booten bekannt (vorher „–“).
- Ein Report ist nach Node-Neustart erst nach dem ersten Heartbeat wieder da (≤ 10 s).
- Ein Hardware-Test steht aus: Anzeige je Board, Änderung von Crop oder Drehung lokal und
  Wirkung in der Übersicht, Verhalten bei laufender Aufnahmeplanung.

## 7. Phase 2, Schritt 1 (beta155): Bildabstimmung und Kamera-Übersicht

Ziel: einheitliche Kameras für Gaussian Splats. Vom User festgelegt (2026-10-25):
- Referenzkamera (Herz) **je Sensortyp**; ihre Einstellungen werden als Profil mit Version an Nodes mit gleichem Sensor übertragen, wahlweise nur für die Sitzung oder dauerhaft in einem eigenen Profilspeicher (nicht `config.txt`; „Profil entfernen“ stellt die lokalen Werte wieder her). Nodes mit anderem Sensor werden markiert. → Schritt 2.
- Übersicht über Kleinbilder mit den echten Einstellungen statt Umschalten auf 320×240 (Drone Mode sperrt die Vorschau; eine andere Auflösung ändert bei fester Belichtung die Belichtungszeit).

Umgesetzt in Schritt 1:

| Teil | Datei | Inhalt |
|---|---|---|
| Abstimmung | `camera_tuning.h`, `webconfig_camera_tuning.cpp` | Schlüssel `camera_tuning` (`1,aec_value,agc,agc_gain,gainceiling,awb,wb_mode,awb_gain,brightness,contrast,saturation`), Seite `/camera_tuning`, POST `/camera_tuning_apply` (`mode=session|save|reset`) |
| Anwenden | `sensorforge.ino` | `cameraApplyTuning()` am Ende der Belichtungseinstellung in `initCamera()`; `cameraApplyTuningRuntime()` ohne Kameraneustart |
| Live-Bild | `cluster_capture.cpp` (`clusterCaptureLiveFrame`), `/camera_frame` | frisches Bild, `size=small` (≈1/8, Standard), `medium` (≥ 800 px breit) oder `full`; gleiche Sperren wie eine Cluster-Aufnahme |
| Kleinbild | `camera_thumbnail.cpp`, `jpeg_mini_encoder.cpp` | Dekodieren direkt in 1/2, 1/4 oder 1/8 (≥ 160 px breit), Neukodierung Qualität 80, 4:4:4 |
| Übersicht | `webconfig_cluster_camera.cpp` | Raster aller Kameras, nacheinander geladen, optional laufend |

Hinweise:
- Der Belichtungswert zählt in Sensorzeilen und ist nur bei gleichem Sensor und gleicher Auflösung vergleichbar.
- Bei laufender Belichtungsautomatik meldet der Treiberstatus den zuletzt gesetzten, nicht den von der Automatik gefundenen Wert.
- Der Browser lädt die Bilder direkt von den Geräten; ist dort die Anmeldung aktiv, muss sie im Browser für jedes Gerät bestehen (wie bei der Ergebnis-Galerie).

## 8. Phase 2, Schritt 2 (beta156): Kameraprofil je Sensortyp

| Teil | Datei | Inhalt |
|---|---|---|
| Profilformat | `camera_profile.h` | `P1,<version>,<pid>,<WxH>,<quality>,<aec>,<ae_level>,<tuning>`, Ergebniscodes |
| Node-Speicher | `camera_profile_store.cpp` | Sitzung (RAM) / dauerhaft (NVS `sf_camprof`) |
| Node-Anwendung | `sensorforge.ino` | `cameraProfileWanted()` (Drone Mode + gleicher Sensor), Übernahme in `initCamera()`, `cameraProfileService()` startet die Kamera bei Änderung neu, `cameraProfileApplyJob()` |
| Protokoll | `cluster.cpp` | `SFJP1` Coordinator → Node (Modus 0 Sitzung, 1 dauerhaft, 2 entfernen), `SFJPA1` Antwort; Job-Art 9 |
| Coordinator | `cluster_camera_profile.cpp` | Referenz + letztes Profil je Sensortyp (NVS `sf_camref`), Profil aus der Referenz-Meldung, Verteilung |
| Meldung | `cluster_camera_report.h` | Gruppe `src` = `sourceMask,profileVersion,profileState` |
| Oberfläche/API | `webconfig_cluster_camera.cpp`, `sync_api.cpp` | Herz, Übertragen/Entfernen, Spalte „Profil“; `/api/v1/cluster/camera_profile`, `/api/v1/cluster/cameras`, `/api/v1/camera/tuning` |

Ablauf: ♡ bei einem Gerät setzen → dessen Bildabstimmung öffnen, Werte einstellen, „Anwenden“ → am Coordinator „Übertragen“. Die Spalte „Profil“ vergleicht danach die gemeldeten Werte jedes Geräts mit dem Profil.

Grenzen:
- Die Verstärkungsobergrenze ist nur übertragbar, wenn die Referenz sie als Index 0–6 meldet (OV3660/OV5640 melden bis zum ersten Setzen über die Bildabstimmung den Rohwert); mit ausgeschalteter Verstärkungsautomatik spielt sie keine Rolle.
- Profile gelten nur im Drone Mode; außerhalb gelten die lokalen Werte (bisherige Vorgabe: Overrides nur im Cluster-Aufnahmebetrieb).
- Ein Gerät, das beim Übertragen offline ist, bekommt das Profil nicht automatisch nach; die Spalte zeigt es rot, erneut „Übertragen“ holt es nach (gleiche Version).
