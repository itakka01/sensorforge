#include "config.h"
#include "board_config.h"

#include <FS.h>
#include <LittleFS.h>
#include <time.h>


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
int cfg_camera_auto_exposure = 1;
int cfg_camera_ae_level = -1;
int cfg_post_ms     = 3000;
int cfg_led_enabled = 1;

String cfg_recording_format = "avi";
int cfg_timestamp_enabled   = 1;
int cfg_recording_segment_seconds = 30;
int cfg_recording_segment_max_mb  = 20;

String cfg_sleep_mode = "off";
int cfg_sleep_delay_ms = 2000;

int cfg_min_free_space_mb = 100;
String cfg_disk_full_action = "rollover";

String cfg_hostname  = "esp32board";
String cfg_timezone = "CET-1CEST,M3.5.0,M10.5.0/3";
String cfg_wifi_on_system_start = "off";
int cfg_wifi_timeout_sec = 60;
String cfg_wifi_ssid = "";
String cfg_wifi_pass = "";

int cfg_hotspot_enabled = 1;
String cfg_hotspot_password = "itakka1234";
int cfg_hotspot_hidden = 0;

int cfg_web_auth_enabled = 0;
String cfg_web_username = "admin";
String cfg_web_password = "itakkaweb1234";

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
    int cameraAutoExposure;
    int cameraAeLevel;
    int rotation;

    String recordingFormat;
    int timestampEnabled;
    int recordingSegmentSeconds;
    int recordingSegmentMaxMb;
    int postMs;
    int ledEnabled;

    String sleepMode;
    int sleepDelayMs;

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
    bool cameraAutoExposure;
    bool cameraAeLevel;
    bool rotation;

    bool recordingFormat;
    bool timestampEnabled;
    bool recordingSegmentSeconds;
    bool recordingSegmentMaxMb;
    bool postMs;
    bool ledEnabled;

    bool sleepMode;
    bool sleepDelayMs;

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

    values.cameraAutoExposure =
        1;

    values.cameraAeLevel =
        -1;

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

    values.postMs =
        3000;

    values.ledEnabled =
        1;

    values.sleepMode =
        "off";

    values.sleepDelayMs =
        2000;

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
        "itakka1234";

    values.hotspotHidden =
        0;

    values.webAuthEnabled =
        0;

    values.webUsername =
        "admin";

    values.webPassword =
        "itakkaweb1234";

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

    values.recordingFormat.trim();
    values.recordingFormat.toLowerCase();

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

    cfg_camera_auto_exposure =
        values.cameraAutoExposure;

    cfg_camera_ae_level =
        values.cameraAeLevel;

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

    cfg_post_ms =
        values.postMs;

    cfg_led_enabled =
        values.ledEnabled;

    cfg_sleep_mode =
        values.sleepMode;

    cfg_sleep_delay_ms =
        values.sleepDelayMs;

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


    if (!writeToSd) {

        return
            CONFIG_SAVE_INTERNAL_ONLY;
    }


    if (!sdAvailableState) {

        error =
            "internal config saved, but SD is unavailable";

        return
            CONFIG_SAVE_SD_FAILED;
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
