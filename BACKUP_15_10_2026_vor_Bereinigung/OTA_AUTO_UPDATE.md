# SensorForge / exapan C1 – automatische Node-Firmwareupdates

Stand: 12.10.2026. Dokumentiert den Quellstand `SensorForge_beta105_OTA_integrationsstand_2026-10-12` plus **optionale Testfreigabe** `SensorForge_beta105_node_autoupdate_testfreigabe_delta.zip`. Die Funktion ist **nicht** durch einen vollständigen ESP32-Build, einen End-to-End-OTA-Test oder Rollback-Tests auf Hardware freigegeben.

## Umfang und Grenzen

- Das Produkt heißt künftig exapan C1; der Projekt-/Firmware-Name bleibt SensorForge.
- Standard-Manifest: `https://firmware.exapan.at/c1/manifest.json`. Die URL ist geplant; ein funktionierender Server an dieser Adresse ist nicht nachgewiesen.
- Die Funktion ist **ab Werk deaktiviert** und wird pro Gerät mit dem lokalen Firmwareformular konfiguriert. Einstellung und URL liegen im NVS-Namensraum `c1-update` (`enabled`, `manifest`). Ein vorhandener gespeicherter Wert wird beim Firmwareupdate nicht zurückgesetzt.
- HTTPS und validierte Zertifikate sind Pflicht. Der aktuelle Stand unterstützt **keine lokale unverschlüsselte HTTP-Quelle** und **noch keinen vom Coordinator verbreiteten Update-Link**. DNS-Namen können über die WLAN-Netzanbindung aufgelöst werden, soweit DNS vorhanden ist.
- Aktuell werden für die automatische Release-Auswahl nur vergleichbare `vNN-betaNN`-Versionen des Kanals `beta` berücksichtigt. Stable-Releases werden zwar formal erkannt, aber noch nicht für automatische Aktualisierung ausgewählt.
- Das aktuelle System besitzt **keine clusterweite Update-Warteschlange**: Wenn mehrere Nodes die Automatik aktivieren, gibt es noch keine garantierte Serialisierung ihrer Downloads. Für Tests deshalb ausschließlich **einen Node** aktivieren.

## Voraussetzungen für den automatischen Ablauf

1. Firmware mit sämtlichen zuvor integrierten OTA-Dateien und optionaler Testfreigabe auf den Node installieren.
2. Vollständiger Build für das jeweilige Board und passendes OTA-Partitionslayout nötig; **hier noch nicht nachgewiesen**.
3. Funktion in der Firmware-Verwaltung des Nodes ausdrücklich aktivieren und URL speichern; Default ist AUS.
4. Der Node muss Drone Mode, WLAN-Verbindung und aktive Cluster-Runtime haben; der Node darf nicht selbst Coordinator sein.
5. Keine laufende Aufnahme, kein anstehender Cluster-Capture und keine kritische SD-/Sync-/Hardware-Messoperation. OTA belegt eine exklusive Reservierung und blockiert dafür vorgesehene konkurrierende Auftragsannahmen. Die vollständige Race-Freiheit aller Dienste ist **noch nicht hardwarevalidiert**.
6. Für unbeaufsichtigte Testausführung muss in `firmware_auto_scheduler.h` `SENSORFORGE_UNATTENDED_OTA_VERIFIED=1` einkompiliert sein. Im zusammengeführten Basisstand ist der Wert `0`; das separate Testfreigabe-Delta setzt ihn auf `1`. **Dieses Makro ist eine technische Freigabe, keine bestandene Verifikation.**

## Ablauf und belegbare Status

- Der Scheduler wartet frühestens 5 Minuten nach dem Boot; nach einem Versuch erfolgt die nächste Prüfung frühestens 24 Stunden später (gezählt nach `millis()`, nicht kalendergebunden). Während die lokale Automatik AUS ist, wird kein automatischer Versuch durchgeführt.
- Das Manifest wird per HTTPS geladen, mit begrenzter Größe und Schema-Prüfung. Die Release-Auswahl liefert einen **unverifizierten Kandidaten**; Metadaten sind keine Installationsfreigabe.
- Das `.sfw`-Paket wird über HTTPS heruntergeladen. Paketgröße und SHA-256 werden mit den Manifestdaten verglichen; zusätzlich erfolgt die eigenständige `.sfw`-Signatur- und Board-Prüfung.
- Die verifizierte Firmware wird in eine inaktive OTA-Partition geschrieben. Ein Fingerabdruck der Partition wird für die Aktivierung erneut überprüft; die Boot-Partitionswahl bedeutet noch keinen erfolgreichen Boot.
- Bei erfolgreicher Boot-Auswahl ruft der automatische Runner `ESP.restart()` auf. Die neue Firmware wird nach mindestens 60 Sekunden auf der erwarteten Partition geprüft; eine eventuell ausstehende ESP-IDF-Rollback-Bestätigung wird dann versucht. Das bedeutet **nicht** „Kamera, Sensoren, SD und Cluster vollständig getestet“.
- Fehlschläge dürfen nicht als erfolgreicher Download, Installation oder Neustart angezeigt werden. Beobachtbare Logs beginnen unter anderem mit `[OTA-AUTO]` und `[OTA]`.
- Es gibt bisher **keine vollständige persistente, vom Coordinator abrufbare Update-Historie**, keine Cluster-Rollout-Bestätigung und keinen sicheren zentralen Download-Stau-Regler.

## Manifestformat (Schema 1)

Das JSON muss ein Objekt mit `schema: 1` und `releases` (maximal 32 Einträge) sein; maximale Abrufgröße 8 KiB. Ein gültiger Release-Eintrag benötigt `board`, `channel`, `version`, `url`, `size`, `sha256`.

```json
{
  "schema": 1,
  "releases": [
    {
      "board": "xiao",
      "channel": "beta",
      "version": "v87-beta106",
      "url": "https://firmware.exapan.at/c1/beta/xiao/v87-beta106.sfw",
      "size": 1234567,
      "sha256": "0000000000000000000000000000000000000000000000000000000000000000"
    }
  ]
}
```

**Das ist ausschließlich ein Syntaxbeispiel.** URL, Größe und Prüfsumme sind Platzhalter; dieses Beispiel ist **kein** realer Release und kann keinen legitimen Updatevorgang begründen. Gültige Board-IDs sind `xiao` und `freenove`; Kanäle `beta` und `stable`. Die Manifest-URL und Paket-URLs müssen HTTPS verwenden. Die `.sfw`-Signatur liefert die unabhängige Paket-Authentifizierung; das Manifest wird nicht als signierte Installationsfreigabe betrachtet.

## Test- und Freigabeprotokoll (noch offen)

- [ ] Vollständigen ESP32-Arduino-Build für XIAO und Freenove durchführen.
- [ ] OTA-Partitionierung und verfügbaren Platz beider Boardvarianten prüfen.
- [ ] TLS inklusive DNS, Zertifikatskette und Fehlerfällen auf dem XIAO prüfen.
- [ ] Signiertes Testpaket für das **richtige Board** verwenden; falsch signierte und falsche Boardpakete müssen abgelehnt werden.
- [ ] Tests für unterbrochenen Download, beschädigte Hashes, Verbindungsverlust und Stromausfall durchführen.
- [ ] Aufnahmeplanung, Recording, SD-Wartung, Drone Mode und Hardware-Messung während OTA auf konkurrierende Zugriffe testen.
- [ ] Boot-Auswahl, Neustart, tatsächliche neue Version, 60-Sekunden-Bestätigung und Rollback praktisch verifizieren.
- [ ] Persistenten Status / Diagnose und später die clusterweite Warteschlange implementieren.

**Testreihenfolge:** Zuerst Build und manuelles signiertes OTA prüfen. Danach einen entbehrlichen XIAO-Node in Drone Mode mit aktivierter Automatik testen. Nicht gleichzeitig mehrere Nodes aktualisieren und nicht auf dem einzigen betriebsnotwendigen Coordinator testen.

## Code-Orientierung

- `webconfig_firmware_auto.cpp/.h`: Einstellungen, NVS, manueller Manifest-Check (`/firmware_auto_settings`, `/firmware_auto_check`).
- `firmware_manifest_probe.cpp/.h`: HTTPS-Metadatenabruf, Board-/Versionsfilter, Kandidatenauswahl.
- `firmware_package_fetch.cpp/.h`: Signierter Download und inaktives OTA-Staging.
- `firmware_update_admission.cpp/.h`: Kooperative exklusive Update-Zulassung.
- `firmware_ota_activation.cpp/.h`: Partitionsfingerabdruck, Boot-Auswahl, 60-Sekunden-Prüfung.
- `firmware_auto_runner.cpp/.h`: Vorbereitung, Boot-Auswahl, kontrollierter Neustart.
- `firmware_auto_scheduler.cpp/.h`: Zeitfenster und Testfreigabe für unbeaufsichtigte Node-Updates.
- `cluster.cpp`: ausgewählte Sperren für konkurrierende Cluster-Kommandos.
