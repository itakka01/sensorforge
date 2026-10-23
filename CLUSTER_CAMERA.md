# Kamera-Transparenz im Cluster (Phase 1, Stand 2026-10-23, nach v87-beta139)

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
