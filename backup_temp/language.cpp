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
    {"Log Viewer", "Log viewer"}, // UI_NAV_LOG_VIEWER
    {"System Info", "System info"}, // UI_NAV_SYSTEM_INFO
    {"Board Info", "Board info"}, // UI_NAV_BOARD_INFO
    {"PSRAM Test", "PSRAM test"}, // UI_NAV_PSRAM_TEST
    {"SD Benchmark", "SD benchmark"}, // UI_NAV_SD_BENCHMARK
    {"Firmware Update", "Firmware update"}, // UI_NAV_FIRMWARE_UPDATE
    {"Transportsicherung", "Transport protection"}, // UI_NAV_TRANSPORT
    {"Neustart", "Reboot"}, // UI_NAV_REBOOT
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
    {"Video-Verschlüsselung auf SD", "Video encryption on SD"}, // UI_RECORDING_ENCRYPTION
    {"0 - aus (neue Videos unverschlüsselt)", "0 - off (new videos unencrypted)"}, // UI_RECORDING_ENCRYPTION_DISABLED
    {"1 - an (neue Videos verschlüsselt)", "1 - on (new videos encrypted)"}, // UI_RECORDING_ENCRYPTION_ENABLED
    {"Die Einstellung gilt nur für neu erzeugte AVI/MKV-Aufnahmen. Bereits vorhandene verschlüsselte und unverschlüsselte Videos bleiben unabhängig davon lesbar; WebPlayer und Downloads entschlüsseln automatisch. Bei der ersten Aktivierung wird einmalig ein zufälliger Hardware-Schlüssel dauerhaft in einem freien ESP32-S3-eFuse-Keyblock eingerichtet.", "This setting applies only to newly created AVI/MKV recordings. Existing encrypted and unencrypted videos remain readable independently of it; WebPlayer and downloads decrypt automatically. On first activation, a random hardware key is initialized once in an available ESP32-S3 eFuse key block."}, // UI_RECORDING_ENCRYPTION_HELP
    {"Legacy-Hinweis: Alte Entwicklungs-Testaufnahmen bleiben lesbar, werden aber nicht mehr für neue Aufnahmen verwendet.", "Legacy note: Old development test recordings remain readable, but the development key is no longer used for new recordings."}, // UI_RECORDING_ENCRYPTION_DEV_WARNING
    {"Hardware-Schlüssel bereit", "Hardware key ready"}, // UI_RECORDING_ENCRYPTION_KEY_READY
    {"Hardware-Schlüssel noch nicht initialisiert; er wird beim ersten Aktivieren einmalig automatisch eingerichtet.", "Hardware key not initialized yet; it will be provisioned automatically once when encryption is first enabled."}, // UI_RECORDING_ENCRYPTION_KEY_UNINITIALIZED
    {"Hardware-Schlüssel konnte nicht eingerichtet werden", "Hardware key provisioning failed"}, // UI_RECORDING_ENCRYPTION_PROVISION_FAILED
    {"Verschlüsselung kann nicht erstmals aktiviert werden, solange eine Aufnahme läuft.", "Encryption cannot be enabled for the first time while a recording is active."}, // UI_RECORDING_ENCRYPTION_RECORDING_ACTIVE
    {"Sprache", "Language"}, // UI_LANGUAGE
    {"Sprache konnte nicht gespeichert werden", "Language could not be saved"}, // UI_LANGUAGE_SAVE_FAILED
    {"Gespeicherte WLAN-, Hotspot- und Web-Passwörter sind in dieser Datei gerätegebunden als SFSEC1 verschlüsselt und nicht im Klartext enthalten.", "Stored Wi-Fi, hotspot and web passwords are device-bound and encrypted as SFSEC1 in this file; they are not stored in plaintext."}, // UI_CONFIG_SECRETS_DOWNLOAD_NOTE
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
