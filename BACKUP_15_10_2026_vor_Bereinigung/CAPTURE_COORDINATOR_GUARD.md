# Coordinator-Capture-Schutz – Firmware-Delta

## Betriebsregeln

- Coordinator: ESP-NOW-Zeitserver wird bei aktiver Coordinator-Rolle im Cluster gestartet, unabhängig vom lokalen Drone Mode. Bei fehlendem WLAN-STA, ungültiger Epoch/Identität oder fehlendem Board-Support bleibt er aus.
- XIAO: aktive Präzisions-Messserien weiterhin nur im Drone Mode und mit authentifizierter Sync-Freigabe.
- Die Webseite setzt **keinen** Drone Mode.
- Ein vom Coordinator disponierter Shot reserviert lokal ein Schutzfenster bis fünf Sekunden nach dem geplanten Zeitpunkt. Neue Dispositionen verlängern dieses Fenster. Ein Node reserviert ab Annahme ebenfalls bis zum lokalen Capture.
- Solange ein lokales Capture geplant ist, wird der ESP-NOW-Service-Loop nicht ausgeführt. Der bestehende `pauseForCapture()` widerruft zusätzlich die Sync-Freigabe über signierte Funksteuerung und erhält die letzte Kalibrierung. Unzuverlässige Funkübertragung garantiert keinen sofortigen Remote-Widerruf.
- OTA-Admisson, SD-Wipe/Format/Secure-Erase und SD-Benchmark prüfen jetzt `clusterCaptureProtectionActive()` statt nur das letzte 2s-Armed-Flag. **Aufnahmen haben Vorrang vor neuen destruktiven SD-Wartungsarbeiten.**

## Grenzen

- Schutz verhindert den Beginn der genannten Operationen auf dem betroffenen Gerät; laufende SD-/Wi-Fi-/Treiberoperationen können nicht präemptiv abgebrochen werden. Bereits gestartete Wartung muss über deren eigenes Admission/Busy-System das Einplanen von Captures verhindern; eine vollständig atomare Cluster-Transaktion ist noch nicht bewiesen.
- Die Coordinator-Schutzfrist läuft nach dem geplanten Shot +5s ab, auch wenn die Node-Antwort ausfällt. Für ein künftiges End-to-End-Jobprotokoll sollte statt des Timers der bestätigte terminale Jobzustand maßgeblich sein.
- Bestehende lokale SD-Wartungsendpunkte außerhalb der vier hier geschützten Einstiegspunkte müssen getrennt auditiert werden.
- Vollständiger Arduino-ESP32-Build und Hardware-Test sind offen. Die Freenove-Firmware muss `BOARD_HAS_ESP_NOW=1` besitzen.

## Testplan

1. Coordinator ohne Drone Mode hochfahren: `ESP-NOW: aktiv` und gültigen WLAN-Kanal prüfen. XIAO im Drone Mode: Freigaben/Probe-Zähler prüfen.
2. Aufnahme mit langer Vorlaufzeit planen, direkt danach lokale SD-Wipe-/Format-/Secure-Erase-/Benchmark-Aktionen sowie OTA testen: sie müssen vor Arbeitsbeginn verweigert werden.
3. Capture abwarten und danach überprüfen, ob SD/OTA wieder zulässig und Sync-Broadcasts wieder aktiv sind.
4. Während einer laufenden SD-Aktion einen Shot disponieren: Das System muss eine Konfliktlage erkennen; keine Genauigkeitsgarantie, bis der gegengerichtete Capture-Admission-Pfad auditiert wurde.
5. Wi-Fi-Verlust, Role-Change und ESP-NOW-Kanalwechsel prüfen.
