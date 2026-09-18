#include "config.h"
#include "board_config.h"

#include <FS.h>
#include <LittleFS.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>


// =============================================================
// DEFAULT CONFIG
// =============================================================

#ifdef BOARD_FREENOVE
String cfg_camera = "OV3660";
#elif defined(BOARD_XIAO)
String cfg_camera = "OV2640";
#else
String cfg_camera = "OV2640";
#endif

String cfg_resolution = "1024x768";

int cfg_fps         = 5;
int cfg_quality     = 12;
#ifdef BOARD_FREENOVE
int cfg_camera_xclk_mhz = 10;
#else
int cfg_camera_xclk_mhz = 20;
#endif
int cfg_camera_auto_exposure = 1;
int cfg_camera_ae_level = -1;
String cfg_camera_crop_zoom = "1.0";
int cfg_camera_crop_x = 1;
int cfg_camera_crop_y = 1;
int cfg_post_ms     = 3000;
int cfg_led_enabled = 1;

String cfg_recording_format = "avi";
int cfg_timestamp_enabled   = 1;
int cfg_recording_segment_seconds = 30;
int cfg_recording_segment_max_mb  = 20;
int cfg_recording_event_max_seconds = 60;
int cfg_recording_event_cooldown_seconds = 120;
String cfg_recording_not_before = "off";

String cfg_sleep_mode = "off";
int cfg_sleep_delay_ms = 2000;

int cfg_transport_mode = 0;
int cfg_transport_check_seconds = 120;
int cfg_transport_light_confirm_seconds = 10;
int cfg_transport_install_delay_seconds = 300;
int cfg_transport_max_duration_seconds = 86400;
int cfg_transport_black_mean_max = 25;
int cfg_transport_black_p95_max = 45;

int cfg_min_free_space_mb = 100;
String cfg_disk_full_action = "rollover";

String cfg_hostname  = "esp32board";
String cfg_timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
String cfg_wifi_on_system_start = "off";
int cfg_wifi_timeout_sec = 60;
String cfg_wifi_ssid = "";
String cfg_wifi_pass = "";

int cfg_hotspot_enabled = 1;
String cfg_hotspot_password = "sensorforge1234";
int cfg_hotspot_hidden = 0;

int cfg_web_auth_enabled = 0;
String cfg_web_username = "admin";
String cfg_web_password = "sensorforgeweb1234";

String cfg_log_file = "/log.txt";

int cfg_debug_enabled = 0;
int cfg_rotation = 0;


// =============================================================
// INTERNAL CONFIG STATE
// =============================================================

static ConfigSource activeConfigSource =
    CONFIG_SOURCE_DEFAULTS;

static bool sdAvailableState =
    false;

static bool sdPresentState =
    false;

static bool sdValidState =
    false;

static bool internalAvailableState =
    false;

static bool internalValidState =
    false;


// Maximum accepted config size.
// This prevents a corrupted file from consuming excessive heap.
static const size_t CONFIG_MAX_BYTES =
    32U * 1024U;


// =============================================================
// TEMPORARY PARSED VALUES
// =============================================================

struct ConfigValues {
    String camera;
    String resolution;

    int fps;
    int quality;
    int cameraXclkMhz;
    int cameraAutoExposure;
    int cameraAeLevel;
    String cameraCropZoom;
    int cameraCropX;
    int cameraCropY;
    int rotation;

    String recordingFormat;
    int timestampEnabled;
    int recordingSegmentSeconds;
    int recordingSegmentMaxMb;
    int recordingEventMaxSeconds;
    int recordingEventCooldownSeconds;
    String recordingNotBefore;
    int postMs;
    int ledEnabled;

    String sleepMode;
    int sleepDelayMs;

    int transportMode;
    int transportCheckSeconds;
    int transportLightConfirmSeconds;
    int transportInstallDelaySeconds;
    int transportMaxDurationSeconds;
    int transportBlackMeanMax;
    int transportBlackP95Max;

    int minFreeSpaceMb;
    String diskFullAction;

    String wifiOnSystemStart;
    int wifiTimeoutSec;
    String hostname;
    String timezone;
    String wifiSsid;
    String wifiPass;

    int hotspotEnabled;
    String hotspotPassword;
    int hotspotHidden;

    int webAuthEnabled;
    String webUsername;
    String webPassword;

    int debugEnabled;
    String logFile;
};


struct ConfigSeen {
    bool camera;
    bool resolution;
    bool fps;
    bool quality;
    bool cameraXclkMhz;
    bool cameraAutoExposure;
    bool cameraAeLevel;
    bool cameraCropZoom;
    bool cameraCropX;
    bool cameraCropY;
    bool rotation;

    bool recordingFormat;
    bool timestampEnabled;
    bool recordingSegmentSeconds;
    bool recordingSegmentMaxMb;
    bool recordingEventMaxSeconds;
    bool recordingEventCooldownSeconds;
    bool recordingNotBefore;
    bool postMs;
    bool ledEnabled;

    bool sleepMode;
    bool sleepDelayMs;

    bool transportMode;
    bool transportCheckSeconds;
    bool transportLightConfirmSeconds;
    bool transportInstallDelaySeconds;
    bool transportMaxDurationSeconds;
    bool transportBlackMeanMax;
    bool transportBlackP95Max;

    bool minFreeSpaceMb;
    bool diskFullAction;

    bool wifiOnSystemStart;
    bool wifiTimeoutSec;
    bool hostname;
    bool timezone;
    bool wifiSsid;
    bool wifiPass;

    bool hotspotEnabled;
    bool hotspotPassword;
    bool hotspotHidden;

    bool webAuthEnabled;
    bool webUsername;
    bool webPassword;

    bool debugEnabled;
    bool logFile;
};


static ConfigValues makeDefaultValues()
{
    ConfigValues values;

#ifdef BOARD_FREENOVE
    values.camera =
        "OV3660";
#elif defined(BOARD_XIAO)
    values.camera =
        "OV2640";
#else
    values.camera =
        "OV2640";
#endif

    values.resolution =
        "1024x768";

    values.fps =
        5;

    values.quality =
        12;

#ifdef BOARD_FREENOVE
    values.cameraXclkMhz =
        10;
#else
    values.cameraXclkMhz =
        20;
#endif

    values.cameraAutoExposure =
        1;

    values.cameraAeLevel =
        -1;

    values.cameraCropZoom =
        "1.0";

    values.cameraCropX =
        1;

    values.cameraCropY =
        1;

    values.rotation =
        0;

    values.recordingFormat =
        "avi";

    values.timestampEnabled =
        1;

    values.recordingSegmentSeconds =
        30;

    values.recordingSegmentMaxMb =
        20;

    values.recordingEventMaxSeconds =
        60;

    values.recordingEventCooldownSeconds =
        120;

    values.recordingNotBefore =
        "off";

    values.postMs =
        3000;

    values.ledEnabled =
        1;

    values.sleepMode =
        "off";

    values.sleepDelayMs =
        2000;

    values.transportMode =
        0;

    values.transportCheckSeconds =
        120;

    values.transportLightConfirmSeconds =
        10;

    values.transportInstallDelaySeconds =
        300;

    values.transportMaxDurationSeconds =
        86400;

    values.transportBlackMeanMax =
        25;

    values.transportBlackP95Max =
        45;

    values.minFreeSpaceMb =
        100;

    values.diskFullAction =
        "rollover";

    values.wifiOnSystemStart =
        "off";

    values.wifiTimeoutSec =
        60;

    values.hostname =
        "esp32board";

    values.timezone =
        "CET-1CEST,M3.5.0,M10.5.0/3";

    values.wifiSsid =
        "";

    values.wifiPass =
        "";

    values.hotspotEnabled =
        1;

    values.hotspotPassword =
        "sensorforge1234";

    values.hotspotHidden =
        0;

    values.webAuthEnabled =
        0;

    values.webUsername =
        "admin";

    values.webPassword =
        "sensorforgeweb1234";

    values.debugEnabled =
        0;

    values.logFile =
        "/log.txt";

    return values;
}


static void clearSeen(
    ConfigSeen &seen
)
{
    memset(
        &seen,
        0,
        sizeof(seen)
    );
}


// =============================================================
// VALIDATION HELPERS
// =============================================================

static bool parseIntegerStrict(
    const String &text,
    long &value
)
{
    if (!text.length())
        return false;

    size_t start =
        0;

    if (
        text[0] == '-' ||
        text[0] == '+'
    ) {
        if (text.length() == 1)
            return false;

        start =
            1;
    }

    for (
        size_t i = start;
        i < text.length();
        ++i
    ) {
        if (!isDigit(text[i]))
            return false;
    }

    value =
        text.toInt();

    return true;
}


static bool isSupportedResolution(
    const String &value
)
{
    return
        value == "160x120" ||
        value == "320x240" ||
        value == "640x480" ||
        value == "800x600" ||
        value == "1024x768" ||
        value == "1280x1024" ||
        value == "1600x1200" ||
        value == "2048x1536";
}


struct RecordingNotBeforeParts {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};


static bool recordingNotBeforeDigits(
    const String &text,
    size_t offset,
    size_t count,
    int &value
)
{
    if (offset + count > text.length())
        return false;

    int result = 0;

    for (size_t i = 0; i < count; ++i) {
        char c = text[offset + i];

        if (c < '0' || c > '9')
            return false;

        result =
            result * 10 +
            (c - '0');
    }

    value = result;
    return true;
}


static bool recordingNotBeforeLeapYear(int year)
{
    return
        (year % 4 == 0 && year % 100 != 0) ||
        (year % 400 == 0);
}


static int recordingNotBeforeDaysInMonth(
    int year,
    int month
)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
        return 0;

    if (month == 2 && recordingNotBeforeLeapYear(year))
        return 29;

    return days[month - 1];
}


static bool parseRecordingNotBeforeParts(
    const String &text,
    RecordingNotBeforeParts &parts
)
{
    // Strict, unambiguous local ISO representation.
    // Example: 2026-09-10T05:30:00
    if (text.length() != 19)
        return false;

    if (
        text[4] != '-' ||
        text[7] != '-' ||
        text[10] != 'T' ||
        text[13] != ':' ||
        text[16] != ':'
    ) {
        return false;
    }

    if (
        !recordingNotBeforeDigits(text, 0, 4, parts.year) ||
        !recordingNotBeforeDigits(text, 5, 2, parts.month) ||
        !recordingNotBeforeDigits(text, 8, 2, parts.day) ||
        !recordingNotBeforeDigits(text, 11, 2, parts.hour) ||
        !recordingNotBeforeDigits(text, 14, 2, parts.minute) ||
        !recordingNotBeforeDigits(text, 17, 2, parts.second)
    ) {
        return false;
    }

    if (parts.year < 2021 || parts.year > 2099)
        return false;

    if (parts.month < 1 || parts.month > 12)
        return false;

    int maxDay =
        recordingNotBeforeDaysInMonth(
            parts.year,
            parts.month
        );

    if (parts.day < 1 || parts.day > maxDay)
        return false;

    if (parts.hour < 0 || parts.hour > 23)
        return false;

    if (parts.minute < 0 || parts.minute > 59)
        return false;

    if (parts.second < 0 || parts.second > 59)
        return false;

    return true;
}


static bool markOnce(
    bool &flag,
    const String &key,
    String &error
)
{
    if (flag) {
        error =
            "duplicate key: " +
            key;

        return false;
    }

    flag =
        true;

    return true;
}


static bool allRequiredKeysSeen(
    const ConfigSeen &seen,
    String &error
)
{
    struct RequiredKey {
        const char *name;
        bool present;
    };

    const RequiredKey required[] = {
        {"camera", seen.camera},
        {"resolution", seen.resolution},
        {"fps", seen.fps},
        {"quality", seen.quality},
        // Upgrade-added camera fields are optional. Missing values use
        // makeDefaultValues(), so older SD configs remain OTA-compatible.
        {"rotation", seen.rotation},
        {"recording_format", seen.recordingFormat},
        {"timestamp_enabled", seen.timestampEnabled},
        {"post_record_ms", seen.postMs},
        {"led_enabled", seen.ledEnabled},
        {"min_free_space_mb", seen.minFreeSpaceMb},
        {"disk_full_action", seen.diskFullAction},
        {"wifi_on_system_start", seen.wifiOnSystemStart},
        {"hostname", seen.hostname},
        // timezone was added after the original config schema; if absent,
        // makeDefaultValues() supplies the firmware default.
        {"wifi_ssid", seen.wifiSsid},
        {"wifi_pass", seen.wifiPass},
        {"debug_enabled", seen.debugEnabled},
        {"log_file", seen.logFile},
    };

    for (
        const RequiredKey &item :
        required
    ) {

        if (!item.present) {

            error =
                "missing key: " +
                String(item.name);

            return false;
        }
    }

    return true;
}


static bool validateValues(
    ConfigValues &values,
    String &error
)
{
    values.camera.trim();
    values.resolution.trim();

    values.cameraCropZoom.trim();

    values.recordingFormat.trim();
    values.recordingFormat.toLowerCase();

    values.recordingNotBefore.trim();

    String recordingNotBeforeLower =
        values.recordingNotBefore;

    recordingNotBeforeLower.toLowerCase();

    if (recordingNotBeforeLower == "off") {
        values.recordingNotBefore = "off";
    }

    values.diskFullAction.trim();
    values.diskFullAction.toLowerCase();

    values.sleepMode.trim();
    values.sleepMode.toLowerCase();

    values.wifiOnSystemStart.trim();
    values.wifiOnSystemStart.toLowerCase();

    // Backward compatibility for config files written by older firmware.
    // The active runtime value is always normalized to the new vocabulary.
    if (values.wifiOnSystemStart == "never") {
        values.wifiOnSystemStart = "off";
    } else if (values.wifiOnSystemStart == "always") {
        values.wifiOnSystemStart = "on";
    }

    values.hostname.trim();
    values.timezone.trim();
    values.wifiSsid.trim();

    values.webUsername.trim();

    values.logFile.trim();


    if (!values.camera.length()) {
        error =
            "camera is empty";

        return false;
    }


    if (!isSupportedResolution(
            values.resolution
        )) {

        error =
            "invalid resolution";

        return false;
    }


    if (
        values.fps < 1 ||
        values.fps > 30
    ) {

        error =
            "fps out of range";

        return false;
    }


    if (
        values.quality < 0 ||
        values.quality > 63
    ) {

        error =
            "quality out of range";

        return false;
    }


    if (
        values.cameraXclkMhz != 10 &&
        values.cameraXclkMhz != 16 &&
        values.cameraXclkMhz != 20
    ) {

        error =
            "camera_xclk_mhz must be 10, 16 or 20";

        return false;
    }


    if (
        values.cameraAutoExposure != 0 &&
        values.cameraAutoExposure != 1
    ) {

        error =
            "camera_auto_exposure must be 0 or 1";

        return false;
    }


    if (
        values.cameraAeLevel < -2 ||
        values.cameraAeLevel > 2
    ) {

        error =
            "camera_ae_level out of range (-2..2)";

        return false;
    }


    if (
        values.cameraCropZoom != "1.0" &&
        values.cameraCropZoom != "1.5" &&
        values.cameraCropZoom != "2.0"
    ) {

        error =
            "camera_crop_zoom must be 1.0, 1.5 or 2.0";

        return false;
    }


    if (
        values.cameraCropX < 0 ||
        values.cameraCropX > 2 ||
        values.cameraCropY < 0 ||
        values.cameraCropY > 2
    ) {

        error =
            "camera_crop_x/camera_crop_y out of range (0..2)";

        return false;
    }


    if (
        values.rotation != 0 &&
        values.rotation != 180
    ) {

        error =
            "rotation must be 0 or 180";

        return false;
    }


    if (
        values.recordingFormat != "avi" &&
        values.recordingFormat != "mkv"
    ) {

        error =
            "recording_format must be avi or mkv";

        return false;
    }


    if (
        values.timestampEnabled != 0 &&
        values.timestampEnabled != 1
    ) {

        error =
            "timestamp_enabled must be 0 or 1";

        return false;
    }


    if (values.recordingNotBefore != "off") {
        RecordingNotBeforeParts armParts;

        if (!parseRecordingNotBeforeParts(
                values.recordingNotBefore,
                armParts
            )) {
            error =
                "recording_not_before must be off or YYYY-MM-DDTHH:MM:SS";

            return false;
        }
    }


    if (
        values.recordingSegmentSeconds < 0 ||
        values.recordingSegmentSeconds > 86400
    ) {

        error =
            "recording_segment_seconds out of range (0..86400)";

        return false;
    }


    if (
        values.recordingSegmentMaxMb < 0 ||
        values.recordingSegmentMaxMb > 4095
    ) {

        error =
            "recording_segment_max_mb out of range (0..4095)";

        return false;
    }


    if (
        values.recordingEventMaxSeconds < 0 ||
        values.recordingEventMaxSeconds > 86400
    ) {

        error =
            "recording_event_max_seconds out of range (0..86400)";

        return false;
    }


    if (
        values.recordingEventCooldownSeconds < 0 ||
        values.recordingEventCooldownSeconds > 86400
    ) {

        error =
            "recording_event_cooldown_seconds out of range (0..86400)";

        return false;
    }


    if (values.postMs < 0) {

        error =
            "post_record_ms must be >= 0";

        return false;
    }


    if (
        values.ledEnabled != 0 &&
        values.ledEnabled != 1
    ) {

        error =
            "led_enabled must be 0 or 1";

        return false;
    }


    if (
        values.sleepMode != "off" &&
        values.sleepMode != "light_sleep" &&
        values.sleepMode != "deep_sleep"
    ) {

        error =
            "sleep_mode must be off, light_sleep or deep_sleep";

        return false;
    }


    if (
        values.sleepDelayMs < 0 ||
        values.sleepDelayMs > 60000
    ) {

        error =
            "sleep_delay_ms out of range (0..60000)";

        return false;
    }


    if (
        values.transportMode != 0 &&
        values.transportMode != 1
    ) {

        error =
            "transport_mode must be 0 or 1";

        return false;
    }


    if (
        values.transportCheckSeconds < 10 ||
        values.transportCheckSeconds > 3600
    ) {

        error =
            "transport_check_seconds out of range (10..3600)";

        return false;
    }


    if (
        values.transportLightConfirmSeconds < 0 ||
        values.transportLightConfirmSeconds > 120
    ) {

        error =
            "transport_light_confirm_seconds out of range (0..120)";

        return false;
    }


    if (
        values.transportInstallDelaySeconds < 0 ||
        values.transportInstallDelaySeconds > 86400
    ) {

        error =
            "transport_install_delay_seconds out of range (0..86400)";

        return false;
    }


    if (
        values.transportMaxDurationSeconds < 3600 ||
        values.transportMaxDurationSeconds > 604800
    ) {

        error =
            "transport_max_duration_seconds out of range (3600..604800)";

        return false;
    }


    if (
        values.transportBlackMeanMax < 0 ||
        values.transportBlackMeanMax > 255 ||
        values.transportBlackP95Max < 0 ||
        values.transportBlackP95Max > 255 ||
        values.transportBlackP95Max < values.transportBlackMeanMax
    ) {

        error =
            "transport black thresholds invalid (0..255, p95 >= mean)";

        return false;
    }


    if (
        values.minFreeSpaceMb < 0 ||
        values.minFreeSpaceMb >
            1024 * 1024
    ) {

        error =
            "min_free_space_mb out of range";

        return false;
    }


    if (
        values.diskFullAction != "rollover" &&
        values.diskFullAction != "stop"
    ) {

        error =
            "disk_full_action must be rollover or stop";

        return false;
    }


    if (
        values.wifiOnSystemStart != "off" &&
        values.wifiOnSystemStart != "on" &&
        values.wifiOnSystemStart != "on_missing_time"
    ) {

        error =
            "wifi_on_system_start must be off, on or on_missing_time";

        return false;
    }


    if (
        values.wifiTimeoutSec < 0 ||
        values.wifiTimeoutSec > 86400
    ) {

        error =
            "wifi_timeout_sec out of range (0..86400)";

        return false;
    }


    if (
        values.hotspotEnabled != 0 &&
        values.hotspotEnabled != 1
    ) {

        error =
            "hotspot_enabled must be 0 or 1";

        return false;
    }


    if (
        values.hotspotHidden != 0 &&
        values.hotspotHidden != 1
    ) {

        error =
            "hotspot_hidden must be 0 or 1";

        return false;
    }


    if (
        values.hotspotPassword.length() < 8 ||
        values.hotspotPassword.length() > 63
    ) {

        error =
            "hotspot_password length must be 8..63";

        return false;
    }


    if (
        values.webAuthEnabled != 0 &&
        values.webAuthEnabled != 1
    ) {

        error =
            "web_auth_enabled must be 0 or 1";

        return false;
    }


    if (
        !values.webUsername.length() ||
        values.webUsername.length() > 32 ||
        values.webUsername.indexOf(':') >= 0
    ) {

        error =
            "web_username must be 1..32 chars and must not contain ':'";

        return false;
    }


    if (
        values.webPassword.length() < 8 ||
        values.webPassword.length() > 63
    ) {

        error =
            "web_password length must be 8..63";

        return false;
    }


    if (
        !values.hostname.length() ||
        values.hostname.length() > 63
    ) {

        error =
            "invalid hostname";

        return false;
    }


    if (
        !values.timezone.length() ||
        values.timezone.length() > 127
    ) {

        error =
            "timezone must be a non-empty POSIX TZ string (max 127 chars)";

        return false;
    }


    for (
        size_t i = 0;
        i < values.timezone.length();
        ++i
    ) {

        unsigned char c =
            (unsigned char)values.timezone[i];

        if (
            c <= 0x20 ||
            c == 0x7F
        ) {

            error =
                "timezone contains whitespace/control characters";

            return false;
        }
    }


    if (
        !values.logFile.length() ||
        !values.logFile.startsWith("/") ||
        values.logFile.indexOf("..") >= 0
    ) {

        error =
            "invalid log_file";

        return false;
    }


    if (
        values.debugEnabled != 0 &&
        values.debugEnabled != 1
    ) {

        error =
            "debug_enabled must be 0 or 1";

        return false;
    }


    return true;
}


// =============================================================
// PARSE COMPLETE CONFIG TEXT
// =============================================================

static bool parseConfigText(
    const String &text,
    ConfigValues &values,
    String &error
)
{
    if (!text.length()) {
        error =
            "config is empty";

        return false;
    }


    if (
        text.length() >
        CONFIG_MAX_BYTES
    ) {

        error =
            "config is too large";

        return false;
    }


    values =
        makeDefaultValues();

    ConfigSeen seen;

    clearSeen(
        seen
    );


    size_t lineStart =
        0;


    while (
        lineStart <
        text.length()
    ) {

        int newlinePos =
            text.indexOf(
                '\n',
                lineStart
            );


        size_t lineEnd =
            newlinePos >= 0
            ? (size_t)newlinePos
            : text.length();


        String line =
            text.substring(
                lineStart,
                lineEnd
            );


        line.replace(
            "\r",
            ""
        );

        line.trim();


        if (
            line.length() &&
            !line.startsWith("#")
        ) {

            int separator =
                line.indexOf('=');


            if (separator <= 0) {

                error =
                    "invalid line: " +
                    line;

                return false;
            }


            String key =
                line.substring(
                    0,
                    separator
                );

            String value =
                line.substring(
                    separator + 1
                );


            key.trim();
            value.trim();


            long numericValue =
                0;


            if (key == "camera") {

                if (!markOnce(
                        seen.camera,
                        key,
                        error
                    )) {
                    return false;
                }

                values.camera =
                    value;

            } else if (
                key == "resolution"
            ) {

                if (!markOnce(
                        seen.resolution,
                        key,
                        error
                    )) {
                    return false;
                }

                values.resolution =
                    value;

            } else if (
                key == "fps"
            ) {

                if (
                    !markOnce(
                        seen.fps,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid fps";

                    return false;
                }

                values.fps =
                    (int)numericValue;

            } else if (
                key == "quality"
            ) {

                if (
                    !markOnce(
                        seen.quality,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid quality";

                    return false;
                }

                values.quality =
                    (int)numericValue;

            } else if (
                key == "camera_xclk_mhz"
            ) {

                if (
                    !markOnce(
                        seen.cameraXclkMhz,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid camera_xclk_mhz";

                    return false;
                }

                values.cameraXclkMhz =
                    (int)numericValue;

            } else if (
                key == "camera_auto_exposure"
            ) {

                if (
                    !markOnce(
                        seen.cameraAutoExposure,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid camera_auto_exposure";

                    return false;
                }

                values.cameraAutoExposure =
                    (int)numericValue;

            } else if (
                key == "camera_ae_level"
            ) {

                if (
                    !markOnce(
                        seen.cameraAeLevel,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid camera_ae_level";

                    return false;
                }

                values.cameraAeLevel =
                    (int)numericValue;

            } else if (
                key == "camera_crop_zoom"
            ) {

                if (!markOnce(
                        seen.cameraCropZoom,
                        key,
                        error
                    )) {
                    return false;
                }

                values.cameraCropZoom =
                    value;

            } else if (
                key == "camera_crop_x"
            ) {

                if (
                    !markOnce(
                        seen.cameraCropX,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid camera_crop_x";

                    return false;
                }

                values.cameraCropX =
                    (int)numericValue;

            } else if (
                key == "camera_crop_y"
            ) {

                if (
                    !markOnce(
                        seen.cameraCropY,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid camera_crop_y";

                    return false;
                }

                values.cameraCropY =
                    (int)numericValue;

            } else if (
                key == "rotation"
            ) {

                if (
                    !markOnce(
                        seen.rotation,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid rotation";

                    return false;
                }

                values.rotation =
                    (int)numericValue;

            } else if (
                key == "recording_format"
            ) {

                if (!markOnce(
                        seen.recordingFormat,
                        key,
                        error
                    )) {
                    return false;
                }

                values.recordingFormat =
                    value;

            } else if (
                key == "timestamp_enabled"
            ) {

                if (
                    !markOnce(
                        seen.timestampEnabled,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid timestamp_enabled";

                    return false;
                }

                values.timestampEnabled =
                    (int)numericValue;

            } else if (
                key == "recording_not_before"
            ) {

                if (!markOnce(
                        seen.recordingNotBefore,
                        key,
                        error
                    )) {
                    return false;
                }

                values.recordingNotBefore =
                    value;

            } else if (
                key == "recording_event_max_seconds"
            ) {

                if (
                    !markOnce(
                        seen.recordingEventMaxSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid recording_event_max_seconds";

                    return false;
                }

                values.recordingEventMaxSeconds =
                    (int)numericValue;

            } else if (
                key == "recording_event_cooldown_seconds"
            ) {

                if (
                    !markOnce(
                        seen.recordingEventCooldownSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid recording_event_cooldown_seconds";

                    return false;
                }

                values.recordingEventCooldownSeconds =
                    (int)numericValue;

            } else if (
                key == "recording_segment_seconds"
            ) {

                if (
                    !markOnce(
                        seen.recordingSegmentSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid recording_segment_seconds";

                    return false;
                }

                values.recordingSegmentSeconds =
                    (int)numericValue;

            } else if (
                key == "recording_segment_max_mb"
            ) {

                if (
                    !markOnce(
                        seen.recordingSegmentMaxMb,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid recording_segment_max_mb";

                    return false;
                }

                values.recordingSegmentMaxMb =
                    (int)numericValue;

            } else if (
                key == "post_record_ms"
            ) {

                if (
                    !markOnce(
                        seen.postMs,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid post_record_ms";

                    return false;
                }

                values.postMs =
                    (int)numericValue;

            } else if (
                key == "led_enabled"
            ) {

                if (
                    !markOnce(
                        seen.ledEnabled,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid led_enabled";

                    return false;
                }

                values.ledEnabled =
                    (int)numericValue;

            } else if (
                key == "sleep_mode"
            ) {

                if (!markOnce(
                        seen.sleepMode,
                        key,
                        error
                    )) {
                    return false;
                }

                values.sleepMode =
                    value;

            } else if (
                key == "sleep_delay_ms"
            ) {

                if (
                    !markOnce(
                        seen.sleepDelayMs,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid sleep_delay_ms";

                    return false;
                }

                values.sleepDelayMs =
                    (int)numericValue;

            } else if (
                key == "transport_mode"
            ) {

                if (
                    !markOnce(
                        seen.transportMode,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_mode";

                    return false;
                }

                values.transportMode =
                    (int)numericValue;

            } else if (
                key == "transport_check_seconds"
            ) {

                if (
                    !markOnce(
                        seen.transportCheckSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_check_seconds";

                    return false;
                }

                values.transportCheckSeconds =
                    (int)numericValue;

            } else if (
                key == "transport_light_confirm_seconds"
            ) {

                if (
                    !markOnce(
                        seen.transportLightConfirmSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_light_confirm_seconds";

                    return false;
                }

                values.transportLightConfirmSeconds =
                    (int)numericValue;

            } else if (
                key == "transport_install_delay_seconds"
            ) {

                if (
                    !markOnce(
                        seen.transportInstallDelaySeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_install_delay_seconds";

                    return false;
                }

                values.transportInstallDelaySeconds =
                    (int)numericValue;

            } else if (
                key == "transport_max_duration_seconds"
            ) {

                if (
                    !markOnce(
                        seen.transportMaxDurationSeconds,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_max_duration_seconds";

                    return false;
                }

                values.transportMaxDurationSeconds =
                    (int)numericValue;

            } else if (
                key == "transport_black_mean_max"
            ) {

                if (
                    !markOnce(
                        seen.transportBlackMeanMax,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_black_mean_max";

                    return false;
                }

                values.transportBlackMeanMax =
                    (int)numericValue;

            } else if (
                key == "transport_black_p95_max"
            ) {

                if (
                    !markOnce(
                        seen.transportBlackP95Max,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid transport_black_p95_max";

                    return false;
                }

                values.transportBlackP95Max =
                    (int)numericValue;

            } else if (
                key == "min_free_space_mb"
            ) {

                if (
                    !markOnce(
                        seen.minFreeSpaceMb,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid min_free_space_mb";

                    return false;
                }

                values.minFreeSpaceMb =
                    (int)numericValue;

            } else if (
                key == "disk_full_action"
            ) {

                if (!markOnce(
                        seen.diskFullAction,
                        key,
                        error
                    )) {
                    return false;
                }

                values.diskFullAction =
                    value;

            } else if (
                key == "wifi_on_system_start" ||
                key == "wifi_mode"
            ) {

                // "wifi_mode" is accepted only as a legacy alias.
                // If both old and new keys exist, markOnce() rejects
                // the second occurrence as an ambiguous duplicate.
                if (!markOnce(
                        seen.wifiOnSystemStart,
                        key,
                        error
                    )) {
                    return false;
                }

                values.wifiOnSystemStart =
                    value;

            } else if (
                key == "wifi_timeout_sec"
            ) {

                if (
                    !markOnce(
                        seen.wifiTimeoutSec,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid wifi_timeout_sec";

                    return false;
                }

                values.wifiTimeoutSec =
                    (int)numericValue;

            } else if (
                key == "hostname"
            ) {

                if (!markOnce(
                        seen.hostname,
                        key,
                        error
                    )) {
                    return false;
                }

                values.hostname =
                    value;

            } else if (
                key == "timezone"
            ) {

                if (!markOnce(
                        seen.timezone,
                        key,
                        error
                    )) {
                    return false;
                }

                values.timezone =
                    value;

            } else if (
                key == "wifi_ssid"
            ) {

                if (!markOnce(
                        seen.wifiSsid,
                        key,
                        error
                    )) {
                    return false;
                }

                values.wifiSsid =
                    value;

            } else if (
                key == "wifi_pass"
            ) {

                if (!markOnce(
                        seen.wifiPass,
                        key,
                        error
                    )) {
                    return false;
                }

                values.wifiPass =
                    value;

            } else if (
                key == "hotspot_enabled"
            ) {

                if (
                    !markOnce(
                        seen.hotspotEnabled,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid hotspot_enabled";

                    return false;
                }

                values.hotspotEnabled =
                    (int)numericValue;

            } else if (
                key == "hotspot_password"
            ) {

                if (!markOnce(
                        seen.hotspotPassword,
                        key,
                        error
                    )) {
                    return false;
                }

                values.hotspotPassword =
                    value;

            } else if (
                key == "hotspot_hidden"
            ) {

                if (
                    !markOnce(
                        seen.hotspotHidden,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid hotspot_hidden";

                    return false;
                }

                values.hotspotHidden =
                    (int)numericValue;

            } else if (
                key == "web_auth_enabled"
            ) {

                if (
                    !markOnce(
                        seen.webAuthEnabled,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid web_auth_enabled";

                    return false;
                }

                values.webAuthEnabled =
                    (int)numericValue;

            } else if (
                key == "web_username"
            ) {

                if (!markOnce(
                        seen.webUsername,
                        key,
                        error
                    )) {
                    return false;
                }

                values.webUsername =
                    value;

            } else if (
                key == "web_password"
            ) {

                if (!markOnce(
                        seen.webPassword,
                        key,
                        error
                    )) {
                    return false;
                }

                values.webPassword =
                    value;

            } else if (
                key == "debug_enabled"
            ) {

                if (
                    !markOnce(
                        seen.debugEnabled,
                        key,
                        error
                    ) ||
                    !parseIntegerStrict(
                        value,
                        numericValue
                    )
                ) {
                    if (!error.length())
                        error = "invalid debug_enabled";

                    return false;
                }

                values.debugEnabled =
                    (int)numericValue;

            } else if (
                key == "log_file"
            ) {

                if (!markOnce(
                        seen.logFile,
                        key,
                        error
                    )) {
                    return false;
                }

                values.logFile =
                    value;

            } else {

                // Unknown keys are ignored intentionally.
                // A future firmware may add fields while older
                // firmware can still use the known subset.
            }
        }


        if (newlinePos < 0)
            break;


        lineStart =
            (size_t)newlinePos + 1U;
    }


    if (!allRequiredKeysSeen(
            seen,
            error
        )) {
        return false;
    }


    return
        validateValues(
            values,
            error
        );
}


bool configValidateText(
    const String &text,
    String &error
)
{
    ConfigValues values;

    error =
        "";

    return
        parseConfigText(
            text,
            values,
            error
        );
}


// =============================================================
// APPLY PARSED VALUES
// =============================================================

static void applyValues(
    const ConfigValues &values
)
{
    cfg_camera =
        values.camera;

    cfg_resolution =
        values.resolution;

    cfg_fps =
        values.fps;

    cfg_quality =
        values.quality;

    cfg_camera_xclk_mhz =
        values.cameraXclkMhz;

    cfg_camera_auto_exposure =
        values.cameraAutoExposure;

    cfg_camera_ae_level =
        values.cameraAeLevel;

    cfg_camera_crop_zoom =
        values.cameraCropZoom;

    cfg_camera_crop_x =
        values.cameraCropX;

    cfg_camera_crop_y =
        values.cameraCropY;

    cfg_rotation =
        values.rotation;

    cfg_recording_format =
        values.recordingFormat;

    cfg_timestamp_enabled =
        values.timestampEnabled;

    cfg_recording_segment_seconds =
        values.recordingSegmentSeconds;

    cfg_recording_segment_max_mb =
        values.recordingSegmentMaxMb;

    cfg_recording_event_max_seconds =
        values.recordingEventMaxSeconds;

    cfg_recording_event_cooldown_seconds =
        values.recordingEventCooldownSeconds;

    cfg_recording_not_before =
        values.recordingNotBefore;

    cfg_post_ms =
        values.postMs;

    cfg_led_enabled =
        values.ledEnabled;

    cfg_sleep_mode =
        values.sleepMode;

    cfg_sleep_delay_ms =
        values.sleepDelayMs;

    cfg_transport_mode =
        values.transportMode;

    cfg_transport_check_seconds =
        values.transportCheckSeconds;

    cfg_transport_light_confirm_seconds =
        values.transportLightConfirmSeconds;

    cfg_transport_install_delay_seconds =
        values.transportInstallDelaySeconds;

    cfg_transport_max_duration_seconds =
        values.transportMaxDurationSeconds;

    cfg_transport_black_mean_max =
        values.transportBlackMeanMax;

    cfg_transport_black_p95_max =
        values.transportBlackP95Max;

    cfg_min_free_space_mb =
        values.minFreeSpaceMb;

    cfg_disk_full_action =
        values.diskFullAction;

    cfg_wifi_on_system_start =
        values.wifiOnSystemStart;

    cfg_wifi_timeout_sec =
        values.wifiTimeoutSec;

    cfg_hostname =
        values.hostname;

    cfg_timezone =
        values.timezone;

    cfg_wifi_ssid =
        values.wifiSsid;

    cfg_wifi_pass =
        values.wifiPass;

    cfg_hotspot_enabled =
        values.hotspotEnabled;

    cfg_hotspot_password =
        values.hotspotPassword;

    cfg_hotspot_hidden =
        values.hotspotHidden;

    cfg_web_auth_enabled =
        values.webAuthEnabled;

    cfg_web_username =
        values.webUsername;

    cfg_web_password =
        values.webPassword;

    cfg_debug_enabled =
        values.debugEnabled;

    cfg_log_file =
        values.logFile;
}


// =============================================================
// FILE HELPERS
// =============================================================

static bool readTextFile(
    fs::FS &filesystem,
    const char *path,
    String &text
)
{
    File file =
        filesystem.open(
            path,
            FILE_READ
        );


    if (!file)
        return false;


    size_t size =
        file.size();


    if (
        size == 0 ||
        size >
            CONFIG_MAX_BYTES
    ) {

        file.close();

        return false;
    }


    text =
        "";

    text.reserve(
        size + 1
    );


    while (file.available()) {

        char buffer[256];

        size_t got =
            file.readBytes(
                buffer,
                sizeof(buffer)
            );


        if (got == 0)
            break;


        text.concat(
            buffer,
            got
        );
    }


    file.close();


    return
        text.length() ==
        size;
}


static void recoverAtomicFiles(
    fs::FS &filesystem
)
{
    if (
        filesystem.exists(
            "/config.txt"
        )
    ) {

        if (
            filesystem.exists(
                "/config.tmp"
            )
        ) {
            filesystem.remove(
                "/config.tmp"
            );
        }

        if (
            filesystem.exists(
                "/config.bak"
            )
        ) {
            filesystem.remove(
                "/config.bak"
            );
        }

        return;
    }


    if (
        filesystem.exists(
            "/config.bak"
        )
    ) {

        filesystem.rename(
            "/config.bak",
            "/config.txt"
        );

        if (
            filesystem.exists(
                "/config.tmp"
            )
        ) {
            filesystem.remove(
                "/config.tmp"
            );
        }

        return;
    }


    if (
        filesystem.exists(
            "/config.tmp"
        )
    ) {

        filesystem.remove(
            "/config.tmp"
        );
    }
}


static bool atomicWriteText(
    fs::FS &filesystem,
    const String &text
)
{
    filesystem.remove(
        "/config.tmp"
    );


    File file =
        filesystem.open(
            "/config.tmp",
            FILE_WRITE
        );


    if (!file)
        return false;


    size_t written =
        file.print(
            text
        );


    file.flush();
    file.close();


    if (
        written !=
        text.length()
    ) {

        filesystem.remove(
            "/config.tmp"
        );

        return false;
    }


    String verifyText;


    if (
        !readTextFile(
            filesystem,
            "/config.tmp",
            verifyText
        ) ||
        verifyText != text
    ) {

        filesystem.remove(
            "/config.tmp"
        );

        return false;
    }


    String validationError;


    if (!configValidateText(
            verifyText,
            validationError
        )) {

        filesystem.remove(
            "/config.tmp"
        );

        return false;
    }


    filesystem.remove(
        "/config.bak"
    );


    bool hadOld =
        filesystem.exists(
            "/config.txt"
        );


    if (hadOld) {

        if (!filesystem.rename(
                "/config.txt",
                "/config.bak"
            )) {

            filesystem.remove(
                "/config.tmp"
            );

            return false;
        }
    }


    if (!filesystem.rename(
            "/config.tmp",
            "/config.txt"
        )) {

        if (hadOld) {
            filesystem.rename(
                "/config.bak",
                "/config.txt"
            );
        }

        filesystem.remove(
            "/config.tmp"
        );

        return false;
    }


    filesystem.remove(
        "/config.bak"
    );


    return true;
}


// =============================================================
// LITTLEFS SHADOW
// =============================================================

static bool initInternalConfigStorage()
{
    if (
        LittleFS.begin(
            false
        )
    ) {

        internalAvailableState =
            true;

        recoverAtomicFiles(
            LittleFS
        );

        return true;
    }


    Serial.println(
        "Config: LittleFS mount failed - attempting format/recovery"
    );


    if (
        !LittleFS.begin(
            true
        )
    ) {

        Serial.println(
            "Config: LittleFS unavailable"
        );

        internalAvailableState =
            false;

        return false;
    }


    internalAvailableState =
        true;

    recoverAtomicFiles(
        LittleFS
    );


    return true;
}


// =============================================================
// LOAD CONFIG
// =============================================================

void loadConfig(
    bool sdAvailable
)
{
    activeConfigSource =
        CONFIG_SOURCE_DEFAULTS;

    sdAvailableState =
        sdAvailable;

    sdPresentState =
        false;

    sdValidState =
        false;

    internalAvailableState =
        false;

    internalValidState =
        false;


    initInternalConfigStorage();


    // ---------------------------------------------------------
    // 1. SD has absolute priority when present AND valid.
    // ---------------------------------------------------------

    if (sdAvailableState) {

        recoverAtomicFiles(
            STORAGE
        );


        sdPresentState =
            STORAGE.exists(
                "/config.txt"
            );


        if (sdPresentState) {

            String sdText;


            if (readTextFile(
                    STORAGE,
                    "/config.txt",
                    sdText
                )) {

                ConfigValues values;
                String error;


                if (parseConfigText(
                        sdText,
                        values,
                        error
                    )) {

                    sdValidState =
                        true;

                    applyValues(
                        values
                    );

                    activeConfigSource =
                        CONFIG_SOURCE_SD;


                    Serial.println(
                        "Config: valid SD /config.txt loaded"
                    );


                    // Exact 1:1 shadow copy.
                    if (internalAvailableState) {

                        if (atomicWriteText(
                                LittleFS,
                                sdText
                            )) {

                            internalValidState =
                                true;

                            Serial.println(
                                "Config: internal shadow updated"
                            );

                        } else {

                            internalValidState =
                                false;

                            Serial.println(
                                "Config: WARNING - internal shadow update failed"
                            );
                        }
                    }


                    // SD wins. No need to inspect internal config
                    // for selection after successful SD load.
                    goto config_loaded;
                }


                Serial.println(
                    "Config: SD /config.txt INVALID: " +
                    error
                );

            } else {

                Serial.println(
                    "Config: SD /config.txt cannot be read"
                );
            }
        }
    }


    // ---------------------------------------------------------
    // 2. SD missing/invalid -> internal LittleFS fallback.
    // ---------------------------------------------------------

    if (
        internalAvailableState &&
        LittleFS.exists(
            "/config.txt"
        )
    ) {

        String internalText;


        if (readTextFile(
                LittleFS,
                "/config.txt",
                internalText
            )) {

            ConfigValues values;
            String error;


            if (parseConfigText(
                    internalText,
                    values,
                    error
                )) {

                internalValidState =
                    true;

                applyValues(
                    values
                );

                activeConfigSource =
                    CONFIG_SOURCE_INTERNAL;


                Serial.println(
                    "Config: using INTERNAL LittleFS fallback"
                );


                goto config_loaded;
            }


            Serial.println(
                "Config: internal shadow INVALID: " +
                error
            );
        }
    }


    // ---------------------------------------------------------
    // 3. No usable external config -> firmware defaults.
    // ---------------------------------------------------------

    applyValues(
        makeDefaultValues()
    );

    activeConfigSource =
        CONFIG_SOURCE_DEFAULTS;


    Serial.println(
        "Config: no valid SD/internal config - using firmware defaults"
    );


config_loaded:

    // If SD was valid, internalValidState was set by the shadow
    // write above. If internal was selected, it is already true.
    // When SD wins but LittleFS existed beforehand and shadow
    // write failed, we deliberately report internal invalid.


    Serial.println(
        "Config source: " +
        String(
            configSourceName()
        )
    );


    Serial.println(
        "Config SD: " +
        String(
            configSdStatusName()
        )
    );


    Serial.println(
        "Config internal shadow: " +
        String(
            internalValidState
            ? "valid"
            : (
                internalAvailableState
                ? "missing/invalid"
                : "unavailable"
            )
        )
    );


    Serial.println(
        "Config Camera: quality=" +
        String(cfg_quality) +
        " xclk_mhz=" +
        String(cfg_camera_xclk_mhz) +
        " auto_exposure=" +
        String(cfg_camera_auto_exposure) +
        " ae_level=" +
        String(cfg_camera_ae_level)
    );


    Serial.println(
        "Config Sleep: mode=" +
        cfg_sleep_mode +
        " delay_ms=" +
        String(cfg_sleep_delay_ms)
    );


    Serial.println(
        "Config Transport: mode=" +
        String(cfg_transport_mode) +
        " check_s=" +
        String(cfg_transport_check_seconds) +
        " confirm_s=" +
        String(cfg_transport_light_confirm_seconds) +
        " install_delay_s=" +
        String(cfg_transport_install_delay_seconds) +
        " max_duration_s=" +
        String(cfg_transport_max_duration_seconds) +
        " black_mean_max=" +
        String(cfg_transport_black_mean_max) +
        " black_p95_max=" +
        String(cfg_transport_black_p95_max)
    );


    Serial.println(
        "Config Arming: recording_not_before=" +
        cfg_recording_not_before
    );

    Serial.println(
        "Config Recording safety: event_max_seconds=" +
        String(cfg_recording_event_max_seconds) +
        " cooldown_seconds=" +
        String(cfg_recording_event_cooldown_seconds)
    );


    // Diagnostic output deliberately never prints the password.
    Serial.println(
        "Config Timezone: " +
        cfg_timezone
    );


    Serial.println(
        "Config WiFi: on_system_start=" +
        cfg_wifi_on_system_start
    );

    Serial.println(
        "Config WiFi: timeout_sec=" +
        String(cfg_wifi_timeout_sec)
    );

    Serial.println(
        "Config WiFi: hostname=" +
        cfg_hostname
    );

    Serial.println(
        "Config WiFi: ssid=" +
        (
            cfg_wifi_ssid.length()
            ? cfg_wifi_ssid
            : String("<empty>")
        )
    );

    Serial.println(
        "Config WiFi: password=" +
        String(
            cfg_wifi_pass.length()
            ? "set"
            : "empty"
        )
    );

    Serial.println(
        "Config Hotspot: enabled=" +
        String(cfg_hotspot_enabled) +
        " hidden=" +
        String(cfg_hotspot_hidden) +
        " password=" +
        String(
            cfg_hotspot_password.length()
            ? "set"
            : "empty"
        )
    );

    Serial.println(
        "Config WebAuth: enabled=" +
        String(cfg_web_auth_enabled) +
        " username=" +
        cfg_web_username +
        " password=" +
        String(
            cfg_web_password.length()
            ? "set"
            : "empty"
        )
    );
}


// =============================================================
// STATUS API
// =============================================================

ConfigSource configGetSource()
{
    return
        activeConfigSource;
}


const char *configSourceName()
{
    switch (activeConfigSource) {

        case CONFIG_SOURCE_SD:
            return "SD card";

        case CONFIG_SOURCE_INTERNAL:
            return "Internal LittleFS fallback";

        case CONFIG_SOURCE_DEFAULTS:
        default:
            return "Firmware defaults";
    }
}


bool configSdAvailable()
{
    return
        sdAvailableState;
}


bool configSdPresent()
{
    return
        sdPresentState;
}


bool configSdValid()
{
    return
        sdValidState;
}


bool configInternalAvailable()
{
    return
        internalAvailableState;
}


bool configInternalValid()
{
    return
        internalValidState;
}


void configRefreshSdStatus()
{
    sdPresentState =
        false;

    sdValidState =
        false;


    if (!sdAvailableState)
        return;


    sdPresentState =
        STORAGE.exists(
            "/config.txt"
        );


    if (!sdPresentState)
        return;


    String text;


    if (!readTextFile(
            STORAGE,
            "/config.txt",
            text
        )) {
        return;
    }


    String error;


    sdValidState =
        configValidateText(
            text,
            error
        );
}


const char *configSdStatusName()
{
    if (!sdAvailableState)
        return "unavailable";

    if (!sdPresentState)
        return "missing";

    if (sdValidState)
        return "valid";

    return "invalid";
}


// =============================================================
// CONFIG FILE MANAGEMENT
// =============================================================

bool configReadInternalText(
    String &text,
    String &error
)
{
    text = "";
    error = "";

    if (
        !internalAvailableState &&
        !initInternalConfigStorage()
    ) {
        error =
            "internal LittleFS unavailable";
        return false;
    }

    if (!LittleFS.exists(
            "/config.txt"
        )) {
        internalValidState =
            false;

        error =
            "internal /config.txt not found";
        return false;
    }

    if (!readTextFile(
            LittleFS,
            "/config.txt",
            text
        )) {
        internalValidState =
            false;

        error =
            "cannot read internal /config.txt";
        return false;
    }

    String validationError;

    if (!configValidateText(
            text,
            validationError
        )) {
        internalValidState =
            false;

        error =
            "internal /config.txt validation failed: " +
            validationError;
        return false;
    }

    internalValidState =
        true;

    return true;
}


bool configCopyInternalToSd(
    String &error
)
{
    error = "";

    if (!sdAvailableState) {
        error =
            "SD is unavailable";
        return false;
    }

    String text;

    if (!configReadInternalText(
            text,
            error
        )) {
        return false;
    }

    if (!atomicWriteText(
            STORAGE,
            text
        )) {
        error =
            "could not write SD /config.txt";
        return false;
    }

    sdPresentState =
        true;

    sdValidState =
        true;

    return true;
}


bool configDeleteSdCopy(
    String &error
)
{
    error = "";

    if (!sdAvailableState) {
        error =
            "SD is unavailable";
        return false;
    }

    // Never remove the SD copy unless a valid internal copy is already
    // available. This guarantees that deleting /config.txt only changes
    // the storage policy and cannot leave the device without configuration.
    String internalText;

    if (!configReadInternalText(
            internalText,
            error
        )) {
        return false;
    }

    const char *paths[] = {
        "/config.tmp",
        "/config.bak",
        "/config.txt"
    };

    for (const char *path : paths) {
        if (
            STORAGE.exists(path) &&
            !STORAGE.remove(path)
        ) {
            error =
                "could not remove SD " +
                String(path);
            return false;
        }
    }

    sdPresentState =
        false;

    sdValidState =
        false;

    // If this boot originally loaded the SD copy, the exact validated shadow
    // is now the authoritative persistent copy for subsequent WebConfig status
    // and saves. Runtime cfg_* values are unchanged.
    if (
        activeConfigSource ==
        CONFIG_SOURCE_SD
    ) {
        activeConfigSource =
            CONFIG_SOURCE_INTERNAL;
    }

    return true;
}


// =============================================================
// SAVE CONFIG
// =============================================================

ConfigSaveResult configSaveText(
    const String &text,
    bool writeToSd,
    String &error
)
{
    error =
        "";


    if (!configValidateText(
            text,
            error
        )) {

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    if (!internalAvailableState) {

        if (!initInternalConfigStorage()) {

            error =
                "internal LittleFS unavailable";

            return
                CONFIG_SAVE_INTERNAL_FAILED;
        }
    }


    // The shadow is the durable fallback and is always updated.
    if (!atomicWriteText(
            LittleFS,
            text
        )) {

        error =
            "internal config write failed";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    internalValidState =
        true;


    // Storage policy is derived exclusively from the physical presence of
    // SD /config.txt. The legacy writeToSd argument is retained for source
    // compatibility with existing callers, but it can neither create a new
    // SD config nor suppress synchronization while one already exists.
    (void)writeToSd;

    bool syncToSd =
        sdAvailableState &&
        STORAGE.exists(
            "/config.txt"
        );


    if (!syncToSd) {

        if (sdAvailableState) {
            sdPresentState =
                false;

            sdValidState =
                false;
        }

        return
            CONFIG_SAVE_INTERNAL_ONLY;
    }


    if (!atomicWriteText(
            STORAGE,
            text
        )) {

        error =
            "internal config saved, but SD config write failed";

        return
            CONFIG_SAVE_SD_FAILED;
    }


    sdPresentState =
        true;

    sdValidState =
        true;


    return
        CONFIG_SAVE_BOTH;
}



// -------------------------------------------------------------
// Persist only the camera crop settings while preserving the exact active
// config text. This avoids rebuilding unrelated fields from a Live Preview
// request and keeps future/unknown keys intact.
// -------------------------------------------------------------

static bool replaceOrAppendConfigKey(
    String &text,
    const String &key,
    const String &value
)
{
    String output;
    output.reserve(
        text.length() +
        key.length() +
        value.length() +
        8
    );

    bool replaced = false;
    size_t start = 0;

    while (start < text.length()) {

        int newlinePos =
            text.indexOf(
                '\n',
                start
            );

        size_t end =
            newlinePos < 0
            ? text.length()
            : (size_t)newlinePos;

        String line =
            text.substring(
                start,
                end
            );

        String probe =
            line;

        probe.trim();

        bool isComment =
            probe.startsWith("#") ||
            probe.startsWith(";");

        bool isTarget =
            false;

        if (!isComment) {

            int equalsPos =
                probe.indexOf('=');

            if (equalsPos >= 0) {

                String foundKey =
                    probe.substring(
                        0,
                        equalsPos
                    );

                foundKey.trim();

                isTarget =
                    foundKey == key;
            }
        }

        if (isTarget) {

            if (replaced) {
                // The active config should already have passed duplicate-key
                // validation. Refuse to manufacture an ambiguous file if that
                // invariant was broken unexpectedly.
                return false;
            }

            output +=
                key +
                "=" +
                value;

            replaced = true;

        } else {

            output +=
                line;
        }

        if (newlinePos >= 0) {
            output += '\n';
            start = end + 1U;
        } else {
            start = text.length();
        }
    }

    if (!replaced) {

        if (
            output.length() > 0 &&
            !output.endsWith("\n")
        ) {
            output += '\n';
        }

        output +=
            key +
            "=" +
            value +
            "\n";
    }

    text =
        output;

    return true;
}


ConfigSaveResult configSaveCameraCrop(
    const String &zoom,
    int positionX,
    int positionY,
    bool writeToSd,
    String &error
)
{
    error =
        "";

    String normalizedZoom =
        zoom;

    normalizedZoom.trim();

    if (
        normalizedZoom != "1.0" &&
        normalizedZoom != "1.5" &&
        normalizedZoom != "2.0"
    ) {

        error =
            "camera crop zoom must be 1.0, 1.5 or 2.0";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (
        positionX < 0 ||
        positionX > 2 ||
        positionY < 0 ||
        positionY > 2
    ) {

        error =
            "camera crop position must be in range 0..2";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    String text;
    bool sourceRead =
        false;

    // Prefer the active source. A valid SD config is normally mirrored 1:1
    // into LittleFS, but reading the active source directly also covers the
    // rare case where the shadow update failed.
    if (
        activeConfigSource == CONFIG_SOURCE_SD &&
        sdAvailableState &&
        STORAGE.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                STORAGE,
                "/config.txt",
                text
            );
    }

    if (
        !sourceRead &&
        internalAvailableState &&
        LittleFS.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                LittleFS,
                "/config.txt",
                text
            );
    }

    if (
        !sourceRead &&
        sdAvailableState &&
        STORAGE.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                STORAGE,
                "/config.txt",
                text
            );
    }

    if (!sourceRead) {

        error =
            "no persistent config.txt available for crop save";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    if (
        !replaceOrAppendConfigKey(
            text,
            "camera_crop_zoom",
            normalizedZoom
        ) ||
        !replaceOrAppendConfigKey(
            text,
            "camera_crop_x",
            String(positionX)
        ) ||
        !replaceOrAppendConfigKey(
            text,
            "camera_crop_y",
            String(positionY)
        )
    ) {

        error =
            "could not patch camera crop keys";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    String validationError;

    if (!configValidateText(
            text,
            validationError
        )) {

        error =
            "patched config invalid: " +
            validationError;

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    ConfigSaveResult result =
        configSaveText(
            text,
            writeToSd,
            error
        );


    if (
        result == CONFIG_SAVE_BOTH ||
        result == CONFIG_SAVE_INTERNAL_ONLY
    ) {

        cfg_camera_crop_zoom =
            normalizedZoom;

        cfg_camera_crop_x =
            positionX;

        cfg_camera_crop_y =
            positionY;
    }


    return result;
}




ConfigSaveResult configSaveTransportMode(
    int mode,
    bool writeToSd,
    String &error
)
{
    error =
        "";

    if (
        mode != 0 &&
        mode != 1
    ) {
        error =
            "transport mode must be 0 or 1";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    String text;
    bool sourceRead =
        false;

    // Preserve the exact active config text, including comments and any
    // future optional keys, and patch only transport_mode.
    if (
        activeConfigSource == CONFIG_SOURCE_SD &&
        sdAvailableState &&
        STORAGE.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                STORAGE,
                "/config.txt",
                text
            );
    }

    if (
        !sourceRead &&
        internalAvailableState &&
        LittleFS.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                LittleFS,
                "/config.txt",
                text
            );
    }

    if (
        !sourceRead &&
        sdAvailableState &&
        STORAGE.exists(
            "/config.txt"
        )
    ) {

        sourceRead =
            readTextFile(
                STORAGE,
                "/config.txt",
                text
            );
    }

    if (!sourceRead) {
        error =
            "no persistent config.txt available for transport mode save";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    if (!replaceOrAppendConfigKey(
            text,
            "transport_mode",
            String(mode)
        )) {

        error =
            "could not patch transport_mode";

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    String validationError;

    if (!configValidateText(
            text,
            validationError
        )) {

        error =
            "patched config invalid: " +
            validationError;

        return
            CONFIG_SAVE_INTERNAL_FAILED;
    }


    ConfigSaveResult result =
        configSaveText(
            text,
            writeToSd,
            error
        );


    if (
        result == CONFIG_SAVE_BOTH ||
        result == CONFIG_SAVE_INTERNAL_ONLY
    ) {
        cfg_transport_mode =
            mode;
    }


    return result;
}


ConfigSaveResult configSaveTransportSettings(
    int checkSeconds,
    int lightConfirmSeconds,
    int installDelaySeconds,
    int maxDurationSeconds,
    int blackMeanMax,
    int blackP95Max,
    bool writeToSd,
    String &error
)
{
    error = "";

    if (checkSeconds < 10 || checkSeconds > 3600) {
        error = "transport_check_seconds out of range (10..3600)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (lightConfirmSeconds < 0 || lightConfirmSeconds > 120) {
        error = "transport_light_confirm_seconds out of range (0..120)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (installDelaySeconds < 0 || installDelaySeconds > 86400) {
        error = "transport_install_delay_seconds out of range (0..86400)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (maxDurationSeconds < 3600 || maxDurationSeconds > 604800) {
        error = "transport_max_duration_seconds out of range (3600..604800)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (blackMeanMax < 0 || blackMeanMax > 255) {
        error = "transport_black_mean_max out of range (0..255)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (blackP95Max < 0 || blackP95Max > 255) {
        error = "transport_black_p95_max out of range (0..255)";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (blackP95Max < blackMeanMax) {
        error = "transport_black_p95_max must be >= transport_black_mean_max";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    String text;
    bool sourceRead = false;

    // Use the exact active config text so comments and future optional keys
    // survive a transport calibration/settings save unchanged.
    if (
        activeConfigSource == CONFIG_SOURCE_SD &&
        sdAvailableState &&
        STORAGE.exists("/config.txt")
    ) {
        sourceRead = readTextFile(STORAGE, "/config.txt", text);
    }

    if (
        !sourceRead &&
        internalAvailableState &&
        LittleFS.exists("/config.txt")
    ) {
        sourceRead = readTextFile(LittleFS, "/config.txt", text);
    }

    if (
        !sourceRead &&
        sdAvailableState &&
        STORAGE.exists("/config.txt")
    ) {
        sourceRead = readTextFile(STORAGE, "/config.txt", text);
    }

    if (!sourceRead) {
        error = "no persistent config.txt available for transport settings save";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    if (
        !replaceOrAppendConfigKey(text, "transport_check_seconds", String(checkSeconds)) ||
        !replaceOrAppendConfigKey(text, "transport_light_confirm_seconds", String(lightConfirmSeconds)) ||
        !replaceOrAppendConfigKey(text, "transport_install_delay_seconds", String(installDelaySeconds)) ||
        !replaceOrAppendConfigKey(text, "transport_max_duration_seconds", String(maxDurationSeconds)) ||
        !replaceOrAppendConfigKey(text, "transport_black_mean_max", String(blackMeanMax)) ||
        !replaceOrAppendConfigKey(text, "transport_black_p95_max", String(blackP95Max))
    ) {
        error = "could not patch transport settings";
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    String validationError;

    if (!configValidateText(text, validationError)) {
        error = "patched config invalid: " + validationError;
        return CONFIG_SAVE_INTERNAL_FAILED;
    }

    ConfigSaveResult result =
        configSaveText(
            text,
            writeToSd,
            error
        );

    if (
        result == CONFIG_SAVE_BOTH ||
        result == CONFIG_SAVE_INTERNAL_ONLY
    ) {
        cfg_transport_check_seconds = checkSeconds;
        cfg_transport_light_confirm_seconds = lightConfirmSeconds;
        cfg_transport_install_delay_seconds = installDelaySeconds;
        cfg_transport_max_duration_seconds = maxDurationSeconds;
        cfg_transport_black_mean_max = blackMeanMax;
        cfg_transport_black_p95_max = blackP95Max;
    }

    return result;
}


// =============================================================
// CREATE RECORDING FOLDER
// =============================================================

String makeFolder()
{
    struct tm t;
    char buf[32];

    if (getLocalTime(&t, 100)) {

        strftime(
            buf,
            sizeof(buf),
            "/%Y%m%d",
            &t
        );

        if (!STORAGE.exists(buf)) {

            if (!STORAGE.mkdir(buf)) {
                Serial.println(
                    "Config: could not create folder " +
                    String(buf)
                );
            }
        }

        return String(buf);
    }


    const char *fallback =
        "/fallback";


    if (!STORAGE.exists(fallback)) {

        if (!STORAGE.mkdir(fallback)) {
            Serial.println(
                "Config: could not create fallback folder"
            );
        }
    }


    return
        String(fallback);
}


// =============================================================
// CREATE RECORDING FILENAME
// =============================================================

String makeFilename()
{
    struct tm t;
    char buf[32];

    if (getLocalTime(&t, 100)) {

        const char *extension =
            (cfg_recording_format == "mkv")
            ? ".mkv"
            : ".avi";

        strftime(
            buf,
            sizeof(buf),
            "%H%M%S",
            &t
        );

        return
            String(buf) +
            extension;
    }


    const char *extension =
        (cfg_recording_format == "mkv")
        ? ".mkv"
        : ".avi";


    snprintf(
        buf,
        sizeof(buf),
        "fallback_%lu",
        millis()
    );


    return
        String(buf) +
        extension;
}


// =============================================================
// RECORDING NOT-BEFORE / ARMING SCHEDULE
// =============================================================

static String recordingScheduleCacheValue;
static String recordingScheduleCacheTimezone;
static bool recordingScheduleCacheInitialized = false;
static bool recordingScheduleCacheEnabled = false;
static bool recordingScheduleCacheValid = false;
static time_t recordingScheduleCacheEpoch = 0;


static void refreshRecordingScheduleCache()
{
    if (
        recordingScheduleCacheInitialized &&
        recordingScheduleCacheValue == cfg_recording_not_before &&
        recordingScheduleCacheTimezone == cfg_timezone
    ) {
        return;
    }

    recordingScheduleCacheInitialized = true;
    recordingScheduleCacheValue = cfg_recording_not_before;
    recordingScheduleCacheTimezone = cfg_timezone;
    recordingScheduleCacheEnabled = false;
    recordingScheduleCacheValid = false;
    recordingScheduleCacheEpoch = 0;

    String value = cfg_recording_not_before;
    value.trim();

    String lower = value;
    lower.toLowerCase();

    if (lower == "off") {
        recordingScheduleCacheEnabled = false;
        recordingScheduleCacheValid = true;
        return;
    }

    recordingScheduleCacheEnabled = true;

    RecordingNotBeforeParts parts;

    if (!parseRecordingNotBeforeParts(
            value,
            parts
        )) {
        return;
    }

    if (!cfg_timezone.length())
        return;

    // mktime()/localtime_r() interpret the civil date in the configured local
    // timezone. Re-applying TZ here also keeps runtime behavior correct after a
    // WebConfig save that changes the timezone without an immediate reboot.
    setenv(
        "TZ",
        cfg_timezone.c_str(),
        1
    );

    tzset();

    struct tm localTime;
    memset(
        &localTime,
        0,
        sizeof(localTime)
    );

    localTime.tm_year = parts.year - 1900;
    localTime.tm_mon = parts.month - 1;
    localTime.tm_mday = parts.day;
    localTime.tm_hour = parts.hour;
    localTime.tm_min = parts.minute;
    localTime.tm_sec = parts.second;
    localTime.tm_isdst = -1;

    time_t epoch =
        mktime(
            &localTime
        );

    if (epoch == (time_t)-1)
        return;

    // Round-trip validation rejects impossible civil times, including a local
    // time skipped by a daylight-saving transition.
    struct tm roundTrip;

    if (!localtime_r(
            &epoch,
            &roundTrip
        )) {
        return;
    }

    if (
        roundTrip.tm_year != parts.year - 1900 ||
        roundTrip.tm_mon != parts.month - 1 ||
        roundTrip.tm_mday != parts.day ||
        roundTrip.tm_hour != parts.hour ||
        roundTrip.tm_min != parts.minute ||
        roundTrip.tm_sec != parts.second
    ) {
        return;
    }

    recordingScheduleCacheEpoch = epoch;
    recordingScheduleCacheValid = true;
}


RecordingNotBeforeState configRecordingNotBeforeState(
    time_t *deadlineEpoch,
    int64_t *remainingSeconds
)
{
    if (deadlineEpoch)
        *deadlineEpoch = 0;

    if (remainingSeconds)
        *remainingSeconds = 0;

    refreshRecordingScheduleCache();

    if (!recordingScheduleCacheEnabled) {
        return
            recordingScheduleCacheValid
            ? RECORDING_NOT_BEFORE_OFF
            : RECORDING_NOT_BEFORE_INVALID;
    }

    if (!recordingScheduleCacheValid)
        return RECORDING_NOT_BEFORE_INVALID;

    if (deadlineEpoch)
        *deadlineEpoch = recordingScheduleCacheEpoch;

    time_t now =
        time(nullptr);

    // 2021-01-01 UTC, matching the existing time validity policy without the
    // 100 ms getLocalTime() wait. This path is called frequently by motion
    // detection and therefore must remain non-blocking.
    if (now < (time_t)1609459200)
        return RECORDING_NOT_BEFORE_CLOCK_INVALID;

    int64_t remaining =
        (int64_t)recordingScheduleCacheEpoch -
        (int64_t)now;

    if (remaining > 0) {
        if (remainingSeconds)
            *remainingSeconds = remaining;

        return RECORDING_NOT_BEFORE_WAITING;
    }

    return RECORDING_NOT_BEFORE_REACHED;
}


bool configRecordingAllowedNow()
{
    RecordingNotBeforeState state =
        configRecordingNotBeforeState(
            nullptr,
            nullptr
        );

    return
        state == RECORDING_NOT_BEFORE_OFF ||
        state == RECORDING_NOT_BEFORE_REACHED;
}


const char *configRecordingNotBeforeStateName(
    RecordingNotBeforeState state
)
{
    switch (state) {
        case RECORDING_NOT_BEFORE_OFF:
            return "off";

        case RECORDING_NOT_BEFORE_WAITING:
            return "waiting";

        case RECORDING_NOT_BEFORE_REACHED:
            return "reached";

        case RECORDING_NOT_BEFORE_CLOCK_INVALID:
            return "clock_invalid";

        case RECORDING_NOT_BEFORE_INVALID:
        default:
            return "invalid";
    }
}


String configRecordingNotBeforeDisplay()
{
    String value = cfg_recording_not_before;
    value.trim();

    String lower = value;
    lower.toLowerCase();

    if (lower == "off")
        return "keine Zeitsperre";

    RecordingNotBeforeParts parts;

    if (!parseRecordingNotBeforeParts(
            value,
            parts
        )) {
        return "ungueltig";
    }

    char buffer[32];

    snprintf(
        buffer,
        sizeof(buffer),
        "%02d.%02d.%04d %02d:%02d:%02d",
        parts.day,
        parts.month,
        parts.year,
        parts.hour,
        parts.minute,
        parts.second
    );

    return String(buffer);
}


// =============================================================
// CHECK IF SYSTEM TIME IS VALID
// =============================================================

bool timeIsValid()
{
    struct tm t;

    if (!getLocalTime(
            &t,
            100
        )) {
        return false;
    }

    return
        (t.tm_year + 1900) >
        2020;
}
