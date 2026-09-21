#include "language.h"
#include "config.h"

namespace {

struct UiTextEntry {
    const char *de;
    const char *en;
};

static const UiTextEntry UI_TEXTS[] = {
    {"Übersicht", "Overview"}, // UI_NAV_OVERVIEW
    {"Aufnahmen", "Recordings"}, // UI_NAV_RECORDINGS
    {"Aufnahmen verwalten", "Manage recordings"}, // UI_NAV_MANAGE_RECORDINGS
    {"Kamera", "Camera"}, // UI_NAV_CAMERA
    {"Live Preview", "Live preview"}, // UI_NAV_LIVE_PREVIEW
    {"Sensor", "Sensor"}, // UI_NAV_SENSOR
    {"Radar Konfiguration", "Radar configuration"}, // UI_NAV_RADAR_CONFIG
    {"Bewegung simulieren", "Simulate motion"}, // UI_NAV_SIMULATE_MOTION
    {"System", "System"}, // UI_NAV_SYSTEM
    {"SD Status", "SD status"}, // UI_NAV_SD_STATUS
    {"SD Recovery", "SD recovery"}, // UI_NAV_SD_RECOVERY
    {"Log Viewer", "Log viewer"}, // UI_NAV_LOG_VIEWER
    {"System Info", "System info"}, // UI_NAV_SYSTEM_INFO
    {"Board Info", "Board info"}, // UI_NAV_BOARD_INFO
    {"PSRAM Test", "PSRAM test"}, // UI_NAV_PSRAM_TEST
    {"SD Benchmark", "SD benchmark"}, // UI_NAV_SD_BENCHMARK
    {"Firmware Update", "Firmware update"}, // UI_NAV_FIRMWARE_UPDATE
    {"Transportsicherung", "Transport protection"}, // UI_NAV_TRANSPORT
    {"Neustart", "Reboot"}, // UI_NAV_REBOOT
    {"Herunterfahren", "Shutdown"}, // UI_NAV_SHUTDOWN
    {"Konfiguration", "Configuration"}, // UI_NAV_CONFIGURATION
    {"AUFNAHME: AN", "RECORDING: ON"}, // UI_RECORDING_SWITCH_ON
    {"AUFNAHME: AUS", "RECORDING: OFF"}, // UI_RECORDING_SWITCH_OFF
    {"AUFNAHME: ...", "RECORDING: ..."}, // UI_RECORDING_SWITCH_PENDING
    {"Aktuelle Systemzeit des Moduls", "Current module system time"}, // UI_MODULE_TIME_TITLE
    {"Zeit", "Time"}, // UI_TIME
    {"Automatische Pause beim Öffnen des Webinterfaces ist aktiv", "Automatic pause when opening the web interface is enabled"}, // UI_AUTO_PAUSE_ENABLED
    {"Automatische Pause beim Öffnen des Webinterfaces ist deaktiviert", "Automatic pause when opening the web interface is disabled"}, // UI_AUTO_PAUSE_DISABLED
    {"Aufnahmeautomatik konnte nicht geändert werden", "Recording automation could not be changed"}, // UI_RECORDING_CHANGE_FAILED
    {"Temperaturschutz", "Thermal protection"}, // UI_THERMAL_PROTECTION
    {"Warnung ab", "Warning from"}, // UI_WARNING_FROM
    {"Notprogramm ab", "Emergency from"}, // UI_EMERGENCY_FROM
    {"Wiederanlauf unter", "Recovery below"}, // UI_RECOVERY_BELOW
    {"RTC/Gehäuseindikator", "RTC/enclosure indicator"}, // UI_RTC_ENCLOSURE_INDICATOR
    {"Übersicht", "Overview"}, // UI_DASH_OVERVIEW
    {"AUFNAHME LÄUFT", "RECORDING ACTIVE"}, // UI_STATUS_RECORDING_RUNNING
    {"AUFNAHME PAUSIERT", "RECORDING PAUSED"}, // UI_STATUS_RECORDING_PAUSED
    {"SICHERHEITSPAUSE", "SAFETY PAUSE"}, // UI_STATUS_SAFETY_PAUSE
    {"NICHT SCHARF", "NOT ARMED"}, // UI_STATUS_NOT_ARMED
    {"Bereit", "Ready"}, // UI_STATUS_READY
    {"Sicherheitspause", "Safety pause"}, // UI_VALUE_SAFETY_PAUSE
    {"Nicht scharf", "Not armed"}, // UI_VALUE_NOT_ARMED
    {"Aktiv", "Active"}, // UI_STATUS_ACTIVE
    {"Pausiert", "Paused"}, // UI_STATUS_PAUSED
    {"ZEITSPERRE", "TIME LOCK"}, // UI_STATUS_TIME_LOCK
    {"KAMERA SCHARF", "CAMERA ARMED"}, // UI_ARM_CAMERA_ARMED
    {"Keine zeitliche Aufnahmesperre", "No time-based recording lock"}, // UI_ARM_NO_TIME_LOCK
    {"Aufnahmeautomatik ist freigegeben", "Recording automation is enabled"}, // UI_ARM_AUTOMATION_RELEASED
    {"KAMERA WIRD SCHARF", "CAMERA ARMING"}, // UI_ARM_CAMERA_ARMING
    {"am", "at"}, // UI_ARM_ON_PREFIX
    {"in", "in"}, // UI_ARM_IN_PREFIX
    {"Freigabezeit", "Release time"}, // UI_ARM_RELEASE_TIME
    {"KAMERA NICHT SCHARF", "CAMERA NOT ARMED"}, // UI_ARM_CAMERA_NOT_ARMED
    {"Geplant", "Scheduled"}, // UI_ARM_SCHEDULED
    {"Systemzeit/RTC ist ungültig - Aufnahme bleibt gesperrt", "System time/RTC is invalid - recording remains locked"}, // UI_ARM_CLOCK_INVALID
    {"recording_not_before ist ungültig", "recording_not_before is invalid"}, // UI_ARM_CONFIG_INVALID
    {"Konfiguration prüfen", "Check configuration"}, // UI_ARM_CHECK_CONFIG
    {"AUFNAHME-SICHERHEITSPAUSE", "RECORDING SAFETY PAUSE"}, // UI_SAFETY_TITLE
    {"Maximale Dauer eines Aufnahmeereignisses wurde erreicht.", "Maximum recording-event duration has been reached."}, // UI_SAFETY_MAX_DURATION
    {"Neue Aufnahmen wieder möglich in", "New recordings available again in"}, // UI_SAFETY_NEW_IN
    {"Aufnahme", "Recording"}, // UI_CARD_RECORDING
    {"Format", "Format"}, // UI_CARD_FORMAT
    {"Bewegung", "Motion"}, // UI_CARD_MOTION
    {"Erkannt", "Detected"}, // UI_MOTION_DETECTED
    {"Keine", "None"}, // UI_MOTION_NONE
    {"SD-Karte", "SD card"}, // UI_CARD_SD_CARD
    {"frei", "free"}, // UI_FREE
    {"belegt von", "used of"}, // UI_USED_OF
    {"Netzwerk", "Network"}, // UI_CARD_NETWORK
    {"Hotspot", "Hotspot"}, // UI_HOTSPOT
    {"Hostname", "Hostname"}, // UI_HOSTNAME
    {"Konfiguration", "Configuration"}, // UI_CARD_CONFIGURATION
    {"interner Shadow", "internal shadow"}, // UI_INTERNAL_SHADOW
    {"gültig", "valid"}, // UI_VALID
    {"ungültig", "invalid"}, // UI_INVALID
    {"SD-Karte", "SD card"}, // UI_CONFIG_SOURCE_SD
    {"Interner LittleFS-Fallback", "Internal LittleFS fallback"}, // UI_CONFIG_SOURCE_INTERNAL
    {"Firmware-Standardwerte", "Firmware defaults"}, // UI_CONFIG_SOURCE_DEFAULTS
    {"nicht verfügbar", "unavailable"}, // UI_SD_UNAVAILABLE
    {"fehlt", "missing"}, // UI_SD_MISSING
    {"gültig", "valid"}, // UI_SD_VALID
    {"ungültig", "invalid"}, // UI_SD_INVALID
    {"Plattform", "Platform"}, // UI_CARD_PLATFORM
    {"Node", "Node"}, // UI_NODE
    {"Rolle", "Role"}, // UI_ROLE
    {"Build", "Build"}, // UI_BUILD
    {"Firmware", "Firmware"}, // UI_CARD_FIRMWARE
    {"Installiert", "Installed"}, // UI_INSTALLED
    {"Quelle", "Source"}, // UI_SOURCE
    {"Echtzeituhr", "Real-time clock"}, // UI_CARD_RTC
    {"RTC OK", "RTC OK"}, // UI_RTC_OK
    {"Zeit prüfen", "Check time"}, // UI_CHECK_TIME
    {"Nicht erkannt", "Not detected"}, // UI_NOT_DETECTED
    {"Optional", "Optional"}, // UI_OPTIONAL
    {"System läuft ohne externe RTC weiter.", "System continues without an external RTC."}, // UI_RTC_ABSENT_NOTE
    {"Temperatur", "Temperature"}, // UI_CARD_TEMPERATURE
    {"ESP32 Chiptemperatur", "ESP32 chip temperature"}, // UI_ESP32_CHIP_TEMPERATURE
    {"Schutzstatus", "Protection status"}, // UI_PROTECTION_STATUS
    {"nach", "after"}, // UI_AFTER
    {"Messungen", "samples"}, // UI_SAMPLES
    {"Die RTC-Temperatur ist nur ein Gehäuseindikator und keine direkte LiPo-Zelltemperatur.", "The RTC temperature is only an enclosure indicator and not a direct LiPo cell temperature."}, // UI_RTC_TEMP_NOTE
    {"Abkühlpause", "Cooldown"}, // UI_COOLDOWN
    {"min", "min"}, // UI_MIN_SHORT
    {"Im Notprogramm wird eine laufende Aufnahme sauber beendet, WLAN/Kamera werden abgeschaltet und das Gerät geht in timer-gesteuerten Deep Sleep. Sicherheitsgrenzen sind fest in der Firmware hinterlegt.", "During a thermal emergency, an active recording is finalized cleanly, Wi-Fi/camera are shut down and the device enters timer-controlled deep sleep. Safety limits are fixed in firmware."}, // UI_THERMAL_EMERGENCY_ACTION
    {"Aufnahmeautomatik", "Recording automation"}, // UI_RECORDING_AUTOMATION
    {"Bewegungssensoren bleiben für die Diagnose sichtbar, lösen aber keine Aufnahme aus. Eine laufende Aufnahme wird sauber beendet.", "Motion sensors remain visible for diagnostics but do not trigger recordings. An active recording is finalized cleanly."}, // UI_REC_NOTE_PAUSED
    {"Die maximale Dauer eines Aufnahmeereignisses wurde erreicht. Bis zum Ende der Sicherheitspause werden keine neuen Aufnahmen gestartet.", "The maximum duration of a recording event has been reached. No new recordings are started until the safety pause ends."}, // UI_REC_NOTE_SAFETY
    {"Für Wartung oder Konfigurationsänderungen kann die automatische Bewegungsauslösung vorübergehend pausiert werden.", "Automatic motion-triggered recording can be paused temporarily for maintenance or configuration changes."}, // UI_REC_NOTE_ARMED
    {"Die Aufnahmeautomatik ist durch recording_not_before zeitlich gesperrt. Bewegung wird erkannt, startet aber vor der Freigabezeit keine Aufnahme.", "Recording automation is time-locked by recording_not_before. Motion is detected but does not start a recording before the release time."}, // UI_REC_NOTE_TIMELOCK
    {"Sicherheitsfunktion:", "Safety function:"}, // UI_SAFETY_FUNCTION
    {"Die Pause wird automatisch aufgehoben, wenn etwa 35 Sekunden keine sichtbare SensorForge-Webseite mehr aktiv ist.", "The pause is released automatically when no visible SensorForge web page has been active for about 35 seconds."}, // UI_PAUSE_AUTO_RELEASE
    {"Die manuelle Pause kann zusätzlich verwendet werden.", "The manual pause can also be used."}, // UI_MANUAL_PAUSE_AVAILABLE
    {"Aufnahme wieder aktivieren", "Enable recording again"}, // UI_RESUME_RECORDING
    {"Aufnahmeautomatik pausieren", "Pause recording automation"}, // UI_PAUSE_RECORDING
    {"Aktiviere ...", "Enabling ..."}, // UI_ACTIVATING
    {"Pausiere ...", "Pausing ..."}, // UI_PAUSING
    {"Radar", "Radar"}, // UI_RADAR
    {"Bewegungssensor", "Motion sensor"}, // UI_MOTION_SENSOR_TYPE
    {"LD2410S Radar", "LD2410S radar"}, // UI_MOTION_SENSOR_RADAR
    {"PIR / digitaler Eingang", "PIR / digital input"}, // UI_MOTION_SENSOR_PIR
    {"SR602 PIR", "SR602 PIR"}, // UI_MOTION_SENSOR_SR602_PIR
    {"Für den PIR-Betrieb am Freenove-Board ist ausschließlich der SR602 getestet und freigegeben. Andere PIR-Module, einschließlich AM312, sind nicht validiert.", "For PIR operation on the Freenove board, only the SR602 has been tested and approved. Other PIR modules, including the AM312, are not validated."}, // UI_MOTION_PIR_FREENOVE_NOTE
    {"Eingang", "Input"}, // UI_PRESENCE_INPUT
    {"PIR GPIO", "PIR GPIO"}, // UI_MOTION_PIR_GPIO
    {"OT2 GPIO", "OT2 GPIO"}, // UI_MOTION_OT2_GPIO
    {"Radar intern", "Internal radar"}, // UI_MOTION_RADAR_INTERNAL
    {"Gate", "Gate"}, // UI_MOTION_GATE
    {"Energie", "Energy"}, // UI_MOTION_ENERGY
    {"nicht verfügbar, OT2-Fallback", "unavailable, OT2 fallback"}, // UI_MOTION_RADAR_FALLBACK
    {"Letzter Trigger", "Last trigger"}, // UI_MOTION_LAST_TRIGGER
    {"noch kein Trigger", "no trigger yet"}, // UI_MOTION_NO_TRIGGER_YET
    {"Trigger seit Seitenaufruf", "Triggers since page opened"}, // UI_MOTION_TRIGGERS_SINCE_OPEN
    {"vor", ""}, // UI_MOTION_AGO
    {"", " ago"}, // UI_MOTION_AGO_SUFFIX
    {"Radar-Konfiguration nicht verfügbar", "Radar configuration unavailable"}, // UI_RADAR_CONFIG_UNAVAILABLE
    {"Beim Systemstart wurde nach vier Versuchen kein LD2410S erkannt. SensorForge verwendet den digitalen PIR/Presence-Eingang.", "No LD2410S was detected after four attempts during startup. SensorForge is using the digital PIR/presence input."}, // UI_RADAR_CONFIG_PIR_NOTE
    {"Testet die Aufnahmeauslösung ohne reale Radar-/PIR-Bewegung.", "Tests recording triggering without real radar/PIR motion."}, // UI_SIM_DESCRIPTION
    {"Dauer", "Duration"}, // UI_DURATION
    {"Sekunden", "seconds"}, // UI_SECONDS
    {"Simulation starten", "Start simulation"}, // UI_START_SIMULATION
    {"Simulation aktiv: noch ca.", "Simulation active: approx."}, // UI_SIM_ACTIVE_REMAINING
    {"Tag", "day"}, // UI_DAY_SINGULAR
    {"Tage", "days"}, // UI_DAY_PLURAL
    {"Stunden", "hours"}, // UI_HOURS
    {"Minuten", "minutes"}, // UI_MINUTES
    {"Sekunden", "seconds"}, // UI_SECONDS_UNIT
    {"Konfiguration gespeichert", "Configuration saved"}, // UI_NOTICE_CONFIG_SAVED_TITLE
    {"Interner Shadow und SD-Karte wurden aktualisiert. Sleep-Einstellungen sind sofort aktiv; andere geänderte Einstellungen werden nach einem Neustart vollständig übernommen.", "Internal shadow and SD card were updated. Sleep settings are active immediately; other changed settings take full effect after a reboot."}, // UI_NOTICE_CONFIG_BOTH_BODY
    {"Der interne Config-Shadow wurde aktualisiert. Die SD-Karte wurde nicht beschrieben. Sleep-Einstellungen sind sofort aktiv; andere geänderte Einstellungen werden nach einem Neustart vollständig übernommen.", "The internal config shadow was updated. The SD card was not written. Sleep settings are active immediately; other changed settings take full effect after a reboot."}, // UI_NOTICE_CONFIG_INTERNAL_BODY
    {"SD Wipe abgeschlossen", "SD wipe completed"}, // UI_NOTICE_SD_WIPE_DONE_TITLE
    {"Der SD-Inhalt wurde gelöscht. Die vorherige Config-Policy wurde beibehalten: eine zuvor vorhandene SD-config.txt wurde wiederhergestellt; Internal-only bleibt ohne SD-config.txt.", "SD contents were deleted. The previous config policy was preserved: an existing SD config.txt was restored; internal-only remains without an SD config.txt."}, // UI_NOTICE_SD_WIPE_DONE_BODY
    {"SD Wipe nicht vollständig", "SD wipe incomplete"}, // UI_NOTICE_SD_WIPE_FAILED_TITLE
    {"Mindestens ein SD-Löschvorgang ist fehlgeschlagen. Die vorherige Config-Policy wurde soweit möglich beibehalten. Bitte SD-Status prüfen.", "At least one SD deletion failed. The previous config policy was preserved where possible. Check SD status."}, // UI_NOTICE_SD_WIPE_FAILED_BODY
    {"SD Format abgeschlossen", "SD format completed"}, // UI_NOTICE_SD_FORMAT_DONE_TITLE
    {"Das FAT-Dateisystem wurde neu erzeugt. Die vorherige Config-Policy wurde beibehalten.", "The FAT filesystem was recreated. The previous config policy was preserved."}, // UI_NOTICE_SD_FORMAT_DONE_BODY
    {"SD Format fehlgeschlagen", "SD format failed"}, // UI_NOTICE_SD_FORMAT_FAILED_TITLE
    {"Die Formatierung konnte nicht sauber abgeschlossen werden. Die vorherige Config-Policy wurde soweit möglich beibehalten. Bitte SD-Status prüfen.", "Formatting could not be completed cleanly. The previous config policy was preserved where possible. Check SD status."}, // UI_NOTICE_SD_FORMAT_FAILED_BODY
    {"SD Format nicht unterstützt", "SD format not supported"}, // UI_NOTICE_SD_FORMAT_UNSUPPORTED_TITLE
    {"Der aktive Storage-Backend/Core stellt die für SensorForge benötigten RAW-/Remount-Funktionen nicht bereit. Es wurden keine Daten verändert.", "The active storage backend/core does not provide the RAW/remount functions required by SensorForge. No data was changed."}, // UI_NOTICE_SD_FORMAT_UNSUPPORTED_BODY
    {"Secure Erase abgeschlossen", "Secure erase completed"}, // UI_NOTICE_SD_SECURE_DONE_TITLE
    {"Der adressierbare freie Datenbereich wurde mit Nullen überschrieben und FAT neu erzeugt. Die vorherige Config-Policy wurde beibehalten.", "The addressable free data area was overwritten with zeros and FAT was recreated. The previous config policy was preserved."}, // UI_NOTICE_SD_SECURE_DONE_BODY
    {"Secure Erase nicht vollständig", "Secure erase incomplete"}, // UI_NOTICE_SD_SECURE_FAILED_TITLE
    {"Überschreiben oder Neuformatierung konnte nicht vollständig abgeschlossen werden. Die vorherige Config-Policy wurde soweit möglich beibehalten. Bitte SD-Status prüfen.", "Overwrite or reformatting could not be completed. The previous config policy was preserved where possible. Check SD status."}, // UI_NOTICE_SD_SECURE_FAILED_BODY
    {"Secure Erase nicht unterstützt", "Secure erase not supported"}, // UI_NOTICE_SD_SECURE_UNSUPPORTED_TITLE
    {"Diese Board-/Storage-Kombination unterstützt den vorgesehenen sicheren Ablauf nicht. Es wurden keine Daten verändert.", "This board/storage combination does not support the intended secure procedure. No data was changed."}, // UI_NOTICE_SD_SECURE_UNSUPPORTED_BODY
    {"SD-Wartung nicht gestartet", "SD maintenance not started"}, // UI_NOTICE_SD_RECORDING_ACTIVE_TITLE
    {"Eine Aufnahme läuft gerade. Wipe, Format und Secure Erase werden während einer laufenden Aufnahme grundsätzlich nicht gestartet. Bitte nach Ende der Aufnahme erneut ausführen.", "A recording is active. Wipe, format and secure erase are never started during an active recording. Try again after the recording ends."}, // UI_NOTICE_SD_RECORDING_ACTIVE_BODY
    {"SD-Karte ist momentan gesperrt", "SD card is currently locked"}, // UI_NOTICE_SD_STORAGE_LOCKED_TITLE
    {"Eine andere Systemoperation hat den globalen Storage-Lock gesetzt. Die SD-Wartung wurde nicht gestartet und es wurden keine Daten verändert.", "Another system operation holds the global storage lock. SD maintenance was not started and no data was changed."}, // UI_NOTICE_SD_STORAGE_LOCKED_BODY
    {"SD-Wartung abgebrochen", "SD maintenance aborted"}, // UI_NOTICE_SD_RESTORE_SOURCE_FAILED_TITLE
    {"Der interne Flash-Shadow /config.txt fehlt oder ist ungültig. Aus Sicherheitsgründen wurde die SD-Karte nicht verändert.", "The internal flash shadow /config.txt is missing or invalid. The SD card was not changed for safety reasons."}, // UI_NOTICE_SD_RESTORE_SOURCE_FAILED_BODY
    {"Konfiguration konnte nicht auf SD wiederhergestellt werden", "Configuration could not be restored to SD"}, // UI_NOTICE_SD_RESTORE_FAILED_TITLE
    {"Die SD-Wartung wurde ausgeführt, aber das Rückkopieren von /config.txt aus dem internen Flash ist fehlgeschlagen. Die laufende RAM-Konfiguration bleibt aktiv; vor einem Neustart bitte SD-Status prüfen.", "SD maintenance completed, but copying /config.txt back from internal flash failed. The current RAM configuration remains active; check SD status before rebooting."}, // UI_NOTICE_SD_RESTORE_FAILED_BODY
    {"OK", "OK"}, // UI_THERMAL_STATE_OK
    {"WARNUNG", "WARNING"}, // UI_THERMAL_STATE_WARNING
    {"NOTPROGRAMM", "EMERGENCY"}, // UI_THERMAL_STATE_EMERGENCY
    {"Lizenz", "License"}, // UI_NAV_LICENSE
    {"Lizenzierung", "Licensing"}, // UI_LICENSE_TITLE
    {"Boardgebundene SensorForge-Lizenz. Die Hardware-ID ist fest aus der Factory-eFuse-Identität dieses Geräts abgeleitet.", "Board-bound SensorForge license. The hardware ID is derived permanently from this device's factory eFuse identity."}, // UI_LICENSE_SUBTITLE
    {"Lizenzstatus", "License status"}, // UI_LICENSE_STATUS
    {"Hardware-ID", "Hardware ID"}, // UI_LICENSE_HARDWARE_ID
    {"Edition", "Edition"}, // UI_LICENSE_EDITION
    {"Lizenz-ID", "License ID"}, // UI_LICENSE_LICENSE_ID
    {"Ausgestellt", "Issued"}, // UI_LICENSE_ISSUED
    {"Gültigkeit", "Validity"}, // UI_LICENSE_VALIDITY
    {"Unbegrenzt", "Perpetual"}, // UI_LICENSE_PERMANENT
    {"Lizenzierte Funktionen", "Licensed features"}, // UI_LICENSE_FEATURES
    {"Keine", "None"}, // UI_LICENSE_NONE
    {"Lizenz aktivieren", "Activate license"}, // UI_LICENSE_ACTIVATION_TITLE
    {"Kopiere die Hardware-ID in den SensorForge-Lizenzgenerator und füge den erzeugten SF1-Aktivierungscode hier ein. Signatur und Boardbindung werden vor dem Speichern geprüft.", "Copy the hardware ID into the SensorForge license generator and paste the generated SF1 activation code here. The signature and board binding are verified before it is stored."}, // UI_LICENSE_ACTIVATION_HELP
    {"Aktivierungscode", "Activation code"}, // UI_LICENSE_ACTIVATION_CODE
    {"Lizenz aktivieren", "Activate license"}, // UI_LICENSE_ACTIVATE_BUTTON
    {"Hardware-ID kopieren", "Copy hardware ID"}, // UI_LICENSE_HARDWARE_COPY
    {"Installierte Lizenz entfernen", "Remove installed license"}, // UI_LICENSE_REMOVE_BUTTON
    {"Installierte Lizenz wirklich entfernen? Das Gerät wechselt danach in den unlizenzierten Zustand.", "Really remove the installed license? The device will then return to the unlicensed state."}, // UI_LICENSE_REMOVE_CONFIRM
    {"Lizenz wurde installiert und ist sofort aktiv.", "License was installed and is active immediately."}, // UI_LICENSE_NOTICE_INSTALLED
    {"Lizenz wurde entfernt.", "License was removed."}, // UI_LICENSE_NOTICE_REMOVED
    {"Lizenz konnte nicht aktiviert werden", "License could not be activated"}, // UI_LICENSE_ACTIVATION_FAILED
    {"Lizenz konnte nicht entfernt werden", "License could not be removed"}, // UI_LICENSE_REMOVE_FAILED
    {"Gültig", "Valid"}, // UI_LICENSE_STATUS_VALID
    {"Nicht lizenziert", "Unlicensed"}, // UI_LICENSE_STATUS_MISSING
    {"Ungültiges Lizenzformat", "Invalid license format"}, // UI_LICENSE_STATUS_INVALID_FORMAT
    {"Ungültige Signatur", "Invalid signature"}, // UI_LICENSE_STATUS_INVALID_SIGNATURE
    {"Lizenz gehört zu einem anderen Gerät", "License belongs to a different device"}, // UI_LICENSE_STATUS_WRONG_DEVICE
    {"Lizenz gehört zu einem anderen Produkt", "License belongs to a different product"}, // UI_LICENSE_STATUS_WRONG_PRODUCT
    {"Lizenzversion wird nicht unterstützt", "License version is not supported"}, // UI_LICENSE_STATUS_UNSUPPORTED_VERSION
    {"Lizenz ist abgelaufen", "License has expired"}, // UI_LICENSE_STATUS_EXPIRED
    {"Systemzeit für Lizenzprüfung nicht verfügbar", "System time unavailable for license validation"}, // UI_LICENSE_STATUS_TIME_UNAVAILABLE
    {"Interner Lizenzspeicherfehler", "Internal license storage error"}, // UI_LICENSE_STATUS_STORAGE_ERROR
    {"Aufnahme", "Recording"}, // UI_LICENSE_FEATURE_RECORDING
    {"Radar", "Radar"}, // UI_LICENSE_FEATURE_RADAR
    {"Sync API", "Sync API"}, // UI_LICENSE_FEATURE_SYNC_API
    {"Erweiterte Analysen", "Advanced analytics"}, // UI_LICENSE_FEATURE_ANALYTICS
    {"SD-Verschlüsselung (Video + Log)", "SD encryption (video + log)"}, // UI_RECORDING_ENCRYPTION
    {"0 - aus (Videos und Logs unverschlüsselt)", "0 - off (videos and logs unencrypted)"}, // UI_RECORDING_ENCRYPTION_DISABLED
    {"1 - an (Videos und Logs verschlüsselt)", "1 - on (videos and logs encrypted)"}, // UI_RECORDING_ENCRYPTION_ENABLED
    {"Bei aktivierter SD-Verschlüsselung werden neue AVI/MKV-Aufnahmen und die System-Logdatei boardgebunden verschlüsselt. WebPlayer, Downloads und Log Viewer entschlüsseln transparent. Bestehende Klartext-Logs werden einmalig nach SFLOG1 migriert; bestehende Video-Dateien bleiben unverändert lesbar. Bei der ersten Aktivierung wird einmalig ein zufälliger Hardware-Schlüssel dauerhaft in einem freien ESP32-S3-eFuse-Keyblock eingerichtet.", "When SD encryption is enabled, new AVI/MKV recordings and the system log are encrypted with a board-bound key. WebPlayer, downloads, and the Log Viewer decrypt transparently. Existing plaintext logs are migrated once to SFLOG1; existing video files remain readable unchanged. On first activation, a random hardware key is initialized once in an available ESP32-S3 eFuse key block."}, // UI_RECORDING_ENCRYPTION_HELP
    {"Legacy-Hinweis: Alte Entwicklungs-Testaufnahmen bleiben lesbar, werden aber nicht mehr für neue Aufnahmen verwendet.", "Legacy note: Old development test recordings remain readable, but the development key is no longer used for new recordings."}, // UI_RECORDING_ENCRYPTION_DEV_WARNING
    {"Hardware-Schlüssel bereit", "Hardware key ready"}, // UI_RECORDING_ENCRYPTION_KEY_READY
    {"Hardware-Schlüssel noch nicht initialisiert; er wird beim ersten Aktivieren einmalig automatisch eingerichtet.", "Hardware key not initialized yet; it will be provisioned automatically once when encryption is first enabled."}, // UI_RECORDING_ENCRYPTION_KEY_UNINITIALIZED
    {"Hardware-Schlüssel konnte nicht eingerichtet werden", "Hardware key provisioning failed"}, // UI_RECORDING_ENCRYPTION_PROVISION_FAILED
    {"Verschlüsselung kann nicht erstmals aktiviert werden, solange eine Aufnahme läuft.", "Encryption cannot be enabled for the first time while a recording is active."}, // UI_RECORDING_ENCRYPTION_RECORDING_ACTIVE
    {"Sprache", "Language"}, // UI_LANGUAGE
    {"Sprache konnte nicht gespeichert werden", "Language could not be saved"}, // UI_LANGUAGE_SAVE_FAILED
    {"Gespeicherte WLAN-, Hotspot- und Web-Passwörter sind in dieser Datei gerätegebunden als SFSEC1 verschlüsselt und nicht im Klartext enthalten.", "Stored Wi-Fi, hotspot and web passwords are device-bound and encrypted as SFSEC1 in this file; they are not stored in plaintext."}, // UI_CONFIG_SECRETS_DOWNLOAD_NOTE
    {"Bildbewegung", "Image motion"}, // UI_NAV_IMAGE_MOTION
    {"Bewegungserkennung im Bild", "Motion detection in the image"}, // UI_IMAGE_MOTION_TITLE
    {"Hier stellst du die Bildbewegung ein und siehst live, ob sie anschlägt. Welche Quelle eine Aufnahme startet, wird separat unter Konfiguration > Aufnahme gewählt.", "Tune image motion here and see live whether it triggers. Which source starts a recording is selected separately under Configuration > Recording."}, // UI_IMAGE_MOTION_SUBTITLE
    {"Bildanalyse verwenden", "Use image analysis"}, // UI_IMAGE_MOTION_ACTIVE
    {"Wann soll eine Aufnahme starten?", "When should a recording start?"}, // UI_IMAGE_MOTION_DECISION
    {"Direkt – Sensor startet Aufnahme sofort", "Direct – sensor starts recording immediately"}, // UI_IMAGE_MOTION_DIRECT
    {"Mit Bildbestätigung – Sensor + erkannte Bildbewegung", "With image verification – sensor + detected image motion"}, // UI_IMAGE_MOTION_VERIFY
    {"Empfindlichkeit (1–10)", "Sensitivity (1–10)"}, // UI_IMAGE_MOTION_SENSITIVITY
    {"Mindestgröße der Bewegung (%)", "Minimum motion size (%)"}, // UI_IMAGE_MOTION_MIN_AREA
    {"Bestätigung über mehrere Bilder", "Confirmation across multiple images"}, // UI_IMAGE_MOTION_CONFIRM
    {"Bewegungsende nach ruhigen Bildern", "End motion after quiet images"}, // UI_IMAGE_MOTION_RELEASE
    {"Anpassung an langsame Änderungen", "Adaptation to slow changes"}, // UI_IMAGE_MOTION_BG_LEARNING
    {"Helligkeitssprung-Schwelle", "Brightness-jump threshold"}, // UI_IMAGE_MOTION_GLOBAL_MEAN
    {"Großflächige Änderung ab (%)", "Large-area change from (%)"}, // UI_IMAGE_MOTION_GLOBAL_CHANGE
    {"Überwachungsbereich im Bild", "Monitored area in the image"}, // UI_IMAGE_MOTION_ROI
    {"Felder per Klick oder Ziehen ein- bzw. ausschalten.", "Click or drag over the image to enable or exclude grid cells."}, // UI_IMAGE_MOTION_ROI_HELP
    {"Gesamtes Bild überwachen", "Monitor entire image"}, // UI_IMAGE_MOTION_SELECT_ALL
    {"Gesamtes Bild ignorieren", "Ignore entire image"}, // UI_IMAGE_MOTION_CLEAR
    {"Invertieren", "Invert"}, // UI_IMAGE_MOTION_INVERT
    {"Einstellungen speichern", "Save settings"}, // UI_IMAGE_MOTION_SAVE
    {"Bildanalyse jetzt testen", "Test image analysis now"}, // UI_IMAGE_MOTION_TEST
    {"Referenzbild neu lernen", "Relearn reference image"}, // UI_IMAGE_MOTION_RESET_BG
    {"Testergebnis", "Test result"}, // UI_IMAGE_MOTION_DIAGNOSTICS
    {"Solange diese Seite geöffnet ist, sind neue automatische Aufnahmen gesperrt. Die Kameravorschau wird laufend analysiert, startet dabei aber keine Aufnahme.", "While this page is open, new automatic recordings are blocked. The camera preview is analyzed continuously but does not start a recording."}, // UI_IMAGE_MOTION_TEST_NOTE
    {"Bildbewegungs-Einstellungen gespeichert.", "Image-motion settings saved."}, // UI_IMAGE_MOTION_SAVED
    {"Bildbewegungs-Einstellungen konnten nicht gespeichert werden", "Image-motion settings could not be saved"}, // UI_IMAGE_MOTION_SAVE_FAILED
    {"Bildanalyse-Test fehlgeschlagen", "Image-analysis test failed"}, // UI_IMAGE_MOTION_TEST_FAILED
    {"0 – Deaktiviert", "0 – Disabled"}, // UI_IMAGE_MOTION_ENABLED_OFF
    {"1 – Aktiviert", "1 – Enabled"}, // UI_IMAGE_MOTION_ENABLED_ON
    {"Schaltet die Bildbewegung ein oder aus. Standard: deaktiviert (0). Die Art der Aufnahmeauslösung wird separat unter Konfiguration > Aufnahme gewählt.", "Turns image motion on or off. Default: disabled (0). The recording trigger mode is selected separately under Configuration > Recording."}, // UI_IMAGE_MOTION_ACTIVE_HELP
    {"Standard: Direkt. 'Direkt' entspricht dem bisherigen Verhalten: Radar/OT2 kann sofort eine Aufnahme starten. 'Mit Bildbestätigung' startet erst, wenn der Sensor auslöst und die Bildanalyse Bewegung bestätigt. Dafür muss die Bildanalyse aktiviert sein.", "Default: Direct. Direct matches the previous behavior: radar/OT2 can start recording immediately. With image verification, recording starts only when the sensor triggers and image analysis confirms motion. Image analysis must be enabled for this."}, // UI_IMAGE_MOTION_DECISION_HELP
    {"1 = unempfindlich, 10 = maximale Empfindlichkeit. Ein höherer Wert reagiert auf kleinere Bildänderungen, kann aber eher Fehlalarme auslösen. Standard: 5.", "1 = low sensitivity, 10 = maximum sensitivity. A higher value reacts to smaller image changes but may cause more false triggers. Default: 5."}, // UI_IMAGE_MOTION_SENSITIVITY_HELP
    {"Wie groß die zusammenhängende veränderte Fläche mindestens sein muss. Kleine Werte erkennen kleinere Objekte, größere Werte ignorieren kleine lokale Änderungen. Bezogen wird der Wert nur auf die nicht rot markierten Überwachungsbereiche. Standard: 6 %.", "Minimum size of one connected changed area. Small values detect smaller objects; larger values ignore small local changes. The percentage refers only to monitored areas that are not shaded red. Default: 6%."}, // UI_IMAGE_MOTION_MIN_AREA_HELP
    {"Wie viele aufeinanderfolgende Analysebilder Bewegung zeigen müssen, bevor sie bestätigt wird. Weniger Bilder reagieren schneller, mehr Bilder sind robuster gegen einzelne Störungen. Standard: 2.", "How many consecutive analysis images must show motion before it is confirmed. Fewer images react faster; more images are more robust against single disturbances. Default: 2."}, // UI_IMAGE_MOTION_CONFIRM_HELP
    {"Wie viele ruhige Analysebilder nötig sind, bis eine bereits bestätigte Bildbewegung wieder als beendet gilt. Höhere Werte halten den Bewegungszustand länger. Standard: 2.", "How many quiet analysis images are required before already confirmed image motion is considered finished. Higher values keep the motion state active longer. Default: 2."}, // UI_IMAGE_MOTION_RELEASE_HELP
    {"Bestimmt, wie schnell sich das gelernte Referenzbild an langsame, dauerhafte Änderungen anpasst, z. B. wandernde Schatten. Höher = schnellere Anpassung, niedriger = stabilere Referenz. Bereich 1–64, Standard: 4.", "Controls how quickly the learned reference image adapts to slow, lasting changes such as moving shadows. Higher = faster adaptation, lower = more stable reference. Range 1–64, default: 4."}, // UI_IMAGE_MOTION_BG_LEARNING_HELP
    {"Schützt vor Fehlalarmen durch plötzliches Ein-/Ausschalten von Licht oder starke Helligkeitswechsel. Wird die mittlere Helligkeit stärker verändert als dieser Wert, behandelt die Analyse das Ereignis als Beleuchtungsänderung statt als Bewegung. Niedriger = stärkere Unterdrückung. Standard: 24.", "Protects against false triggers caused by lights switching on/off or strong brightness changes. If the average brightness changes by more than this value, the event is treated as a lighting change rather than motion. Lower = stronger suppression. Default: 24."}, // UI_IMAGE_MOTION_GLOBAL_MEAN_HELP
    {"Wenn gleichzeitig mindestens dieser Anteil des überwachten Bildbereichs stark verändert ist, wird die Änderung als großflächig bewertet – typisch für Lichtwechsel statt eines einzelnen Objekts. Niedriger = großflächige Änderungen werden früher ausgefiltert. Standard: 70 %.", "If at least this share of the monitored image area changes strongly at the same time, the change is treated as large-area – typical of lighting changes rather than one object. Lower = large-area changes are filtered earlier. Default: 70%."}, // UI_IMAGE_MOTION_GLOBAL_CHANGE_HELP
    {"Ohne rote Markierung = aktiv überwacht", "No red overlay = actively monitored"}, // UI_IMAGE_MOTION_ROI_ACTIVE_LEGEND
    {"Rötlich markiert = von der Bildanalyse ignoriert", "Red shaded = ignored by image analysis"}, // UI_IMAGE_MOTION_ROI_EXCLUDED_LEGEND
    {"Standardwerte einsetzen", "Insert default values"}, // UI_IMAGE_MOTION_RESET_DEFAULTS
    {"Setzt alle Felder und den Überwachungsbereich im Formular auf die SensorForge-Standardwerte zurück. Es wird noch nichts gespeichert; erst 'Einstellungen speichern' übernimmt die Werte dauerhaft.", "Resets all fields and the monitored area in the form to the SensorForge defaults. Nothing is saved yet; only Save settings stores the values permanently."}, // UI_IMAGE_MOTION_RESET_DEFAULTS_HELP
    {"Standardwerte wurden in das Formular eingesetzt. Zum Übernehmen bitte Einstellungen speichern.", "Default values were inserted into the form. Save settings to apply them."}, // UI_IMAGE_MOTION_RESET_DEFAULTS_DONE
    {"Analysiert die aktuelle Kameraszene mit den gespeicherten Einstellungen. Es wird keine Aufnahme gestartet. Das Ergebnis wird darunter in verständlicher Form angezeigt. Nach Änderungen an den Feldern zuerst speichern.", "Analyzes the current camera scene using the saved settings. No recording is started. The result is shown below in an understandable form. Save first after changing any fields."}, // UI_IMAGE_MOTION_TEST_HELP
    {"Vergisst das bisher gelernte Referenzbild der ruhigen Szene. Sinnvoll, wenn die Kamera versetzt wurde oder sich die Umgebung dauerhaft verändert hat. Beim nächsten Test oder Sensortrigger wird die aktuelle Szene wieder als Referenz gelernt. Die Einstellungen selbst bleiben unverändert.", "Forgets the previously learned reference image of the quiet scene. Useful if the camera was moved or the environment changed permanently. On the next test or sensor trigger, the current scene is learned again as the reference. Settings themselves remain unchanged."}, // UI_IMAGE_MOTION_RESET_BG_HELP
    {"Das bisherige Referenzbild wurde verworfen. Beim nächsten Test oder Sensortrigger wird die aktuelle Szene neu gelernt.", "The previous reference image was discarded. The current scene will be learned again on the next test or sensor trigger."}, // UI_IMAGE_MOTION_RESET_BG_DONE
    {"Hier erscheint nach einem Test zuerst eine einfache Aussage wie 'Bewegung erkannt' oder 'keine ausreichende Bewegung'. Technische Messwerte sind darunter optional aufklappbar.", "After a test, this first shows a simple result such as 'motion detected' or 'no sufficient motion'. Technical measurements can optionally be expanded below."}, // UI_IMAGE_MOTION_DIAGNOSTICS_HELP
    {"Technische Diagnose anzeigen", "Show technical diagnostics"}, // UI_IMAGE_MOTION_TECH_DETAILS
    {"Info", "Info"}, // UI_IMAGE_MOTION_INFO
    {"Schließen", "Close"}, // UI_IMAGE_MOTION_INFO_CLOSE
    {"Ergebnis", "Result"}, // UI_IMAGE_MOTION_RESULT_TITLE
    {"Bewegung erkannt – die Bildanalyse würde diesen Trigger bestätigen.", "Motion detected – image analysis would confirm this trigger."}, // UI_IMAGE_MOTION_RESULT_MOTION
    {"Keine ausreichende Bewegung erkannt.", "No sufficient motion detected."}, // UI_IMAGE_MOTION_RESULT_NONE
    {"Bildanalyse steht momentan nicht zur Verfügung.", "Image analysis is currently unavailable."}, // UI_IMAGE_MOTION_RESULT_DISABLED
    {"Das Referenzbild wurde neu gelernt. Führe den Test erneut aus, während sich etwas im überwachten Bereich bewegt.", "The reference image was relearned. Run the test again while something moves in the monitored area."}, // UI_IMAGE_MOTION_RESULT_LEARNING
    {"Eine Bildänderung wurde erkannt, aber noch nicht oft genug hintereinander bestätigt.", "An image change was detected but has not yet been confirmed in enough consecutive images."}, // UI_IMAGE_MOTION_RESULT_CONFIRMING
    {"Großflächiger Helligkeitswechsel erkannt. Er wurde bewusst nicht als Bewegung gewertet.", "Large-area brightness change detected. It was deliberately not treated as motion."}, // UI_IMAGE_MOTION_RESULT_GLOBAL_LIGHT
    {"Es ist kein Bildbereich zur Überwachung aktiviert. Markiere mindestens einen Bereich als aktiv und speichere die Einstellung.", "No image area is enabled for monitoring. Enable at least one area and save the setting."}, // UI_IMAGE_MOTION_RESULT_NO_ROI
    {"Die Bildanalyse konnte nicht korrekt ausgewertet werden. Siehe technische Diagnose.", "Image analysis could not be evaluated correctly. See technical diagnostics."}, // UI_IMAGE_MOTION_RESULT_ERROR
    {"Analysezeit", "Analysis time"}, // UI_IMAGE_MOTION_RESULT_TIME
    {"Größte zusammenhängende Änderung", "Largest connected change"}, // UI_IMAGE_MOTION_RESULT_AREA
    {"Eingestellte Mindestgröße", "Configured minimum size"}, // UI_IMAGE_MOTION_RESULT_LIMIT
    {"Der Test verwendet die zuletzt gespeicherten Werte.", "The test uses the most recently saved values."}, // UI_IMAGE_MOTION_TEST_USES_SAVED
    {"Aktueller Sensorstatus", "Current sensor status"}, // UI_MOTION_CURRENT_SENSOR_STATUS
    {"Radar-Auswertung", "Radar evaluation"}, // UI_MOTION_RADAR_EVALUATION
    {"Keine Bewegung", "No motion"}, // UI_MOTION_RADAR_EVALUATION_NONE
    {"Bewegung erkannt", "Motion detected"}, // UI_MOTION_RADAR_EVALUATION_ACTIVE
    {"Letzter Radar-Trigger", "Last radar trigger"}, // UI_MOTION_LAST_RADAR_TRIGGER
    {"Aufnahmeauslösung", "Recording trigger mode"}, // UI_RECORDING_TRIGGER_MODE
    {"Sensor direkt – Radar/PIR startet sofort", "Direct sensor – radar/PIR starts immediately"}, // UI_RECORDING_TRIGGER_DIRECT
    {"Sensor + Bildbestätigung – Bildbewegung muss bestätigen", "Sensor + image verification – image motion must confirm"}, // UI_RECORDING_TRIGGER_VERIFY
    {"Nur Bildbewegung – Sensor dient im Sleep nur zum Aufwecken", "Image motion only – sensor is wake-only during sleep"}, // UI_RECORDING_TRIGGER_IMAGE_ONLY
    {"Legt fest, wodurch eine automatische Aufnahme startet. 'Sensor direkt' ist der bisherige schnelle Pfad. 'Sensor + Bildbestätigung' benötigt zuerst Radar/PIR und anschließend bestätigte Bildbewegung. 'Nur Bildbewegung' lässt die Kamera entscheiden; im Light Sleep bleibt Radar/PIR lediglich die Hardware-Wakequelle.", "Defines what starts an automatic recording. Direct sensor is the existing low-latency path. Sensor + image verification requires radar/PIR first and then confirmed image motion. Image motion only lets the camera decide; in light sleep radar/PIR remains only the hardware wake source."}, // UI_RECORDING_TRIGGER_HELP
    {"Bildbewegung ist ausgeschaltet. Bildbasierte Aufnahmemodi sind damit momentan nicht aktiv; SensorForge verwendet bis zur Aktivierung den direkten Sensorpfad.", "Image motion is disabled. Image-based recording modes are currently inactive; SensorForge uses the direct sensor path until image motion is enabled."}, // UI_RECORDING_TRIGGER_IMAGE_DISABLED_WARNING
    {"Bildbewegung einstellen", "Configure image motion"}, // UI_RECORDING_TRIGGER_IMAGE_SETTINGS
    {"Live-Erkennung", "Live detection"}, // UI_IMAGE_MOTION_LIVE_TITLE
    {"Bildbewegung", "Image motion"}, // UI_IMAGE_MOTION_LIVE_STATUS
    {"Bestätigung", "Confirmation"}, // UI_IMAGE_MOTION_LIVE_CONFIRMATION
    {"Bild insgesamt verändert", "Total image changed"}, // UI_IMAGE_MOTION_LIVE_CHANGED_AREA
    {"Aktuelle Bewegung", "Current motion"}, // UI_IMAGE_MOTION_LIVE_CURRENT_MOTION
    {"Abweichung vom Hintergrund", "Background difference"}, // UI_IMAGE_MOTION_LIVE_BACKGROUND_DIFFERENCE
    {"gesamt", "total"}, // UI_IMAGE_MOTION_LIVE_TOTAL_SHORT
    {"zusammenhängend", "connected"}, // UI_IMAGE_MOTION_LIVE_CONNECTED_SHORT
    {"Letzte Erkennung", "Last detection"}, // UI_IMAGE_MOTION_LIVE_LAST_DETECTION
    {"BEWEGUNG", "MOTION"}, // UI_IMAGE_MOTION_LIVE_DETECTED
    {"Keine Bewegung", "No motion"}, // UI_IMAGE_MOTION_LIVE_NONE
    {"Referenz wird gelernt", "Learning reference"}, // UI_IMAGE_MOTION_LIVE_LEARNING
    {"Helligkeitswechsel ignoriert", "Brightness change ignored"}, // UI_IMAGE_MOTION_LIVE_GLOBAL_LIGHT
    {"Fehler", "Error"}, // UI_IMAGE_MOTION_LIVE_ERROR
    {"Bestätigt", "Confirmed"}, // UI_IMAGE_MOTION_LIVE_CONFIRMED
    {"noch keine", "none yet"}, // UI_IMAGE_MOTION_LIVE_NEVER
    {"warte auf Daten", "waiting for data"}, // UI_IMAGE_MOTION_LIVE_WAITING
    {"Nicht aktiv", "Inactive"}, // UI_IMAGE_MOTION_LIVE_INACTIVE
    {"Diagnose herunterladen", "Download diagnostics"}, // UI_IMAGE_MOTION_DIAG_DOWNLOAD
    {"RAM-Puffer; keine SD-/Hauptlog-Einträge", "RAM buffer; no SD/main-log writes"}, // UI_IMAGE_MOTION_DIAG_BUFFER
};

static_assert(
    sizeof(UI_TEXTS) / sizeof(UI_TEXTS[0]) == UI_TEXT_COUNT,
    "UI translation table does not match UiTextId"
);

} // namespace

const char *uiLanguageCode()
{
    return cfg_web_language == "en" ? "en" : "de";
}

bool uiLanguageSupported(const String &code)
{
    String normalized = code;
    normalized.trim();
    normalized.toLowerCase();

    return
        normalized == "de" ||
        normalized == "en";
}

const char *tr(UiTextId id)
{
    if (id >= UI_TEXT_COUNT)
        return "";

    const UiTextEntry &entry = UI_TEXTS[(uint16_t)id];

    return
        cfg_web_language == "en"
        ? entry.en
        : entry.de;
}
