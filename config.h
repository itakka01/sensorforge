#pragma once

#include <Arduino.h>
#include <time.h>

// Camera
extern String cfg_camera;
extern String cfg_resolution;
extern int cfg_fps;
extern int cfg_quality;
extern int cfg_camera_xclk_mhz;        // 10, 16 or 20 MHz; applied at camera init
extern int cfg_camera_auto_exposure;   // 0/1
extern int cfg_camera_ae_level;        // -2..2

// OV3660 sensor crop. Zoom is one of "1.0", "1.5", "2.0".
// X/Y use a 3x3 position grid: 0=left/top, 1=center, 2=right/bottom.
// At 1.0x the position values are retained but do not change the image.
extern String cfg_camera_crop_zoom;
extern int cfg_camera_crop_x;
extern int cfg_camera_crop_y;

extern int cfg_rotation;

// Recording
extern int cfg_post_ms;
extern int cfg_led_enabled;
extern String cfg_recording_format;   // "avi" or "mkv"
extern int cfg_timestamp_enabled;     // 0/1
extern int cfg_recording_encryption;  // 0/1, encrypt newly created video/snapshot media on SD
extern int cfg_periodic_snapshot_minutes; // 0 = off, fixed periodic JPEG schedule in minutes
extern int cfg_recording_segment_seconds; // 0 = unlimited
extern int cfg_recording_segment_max_mb;  // 0 = unlimited
// Maximum duration of one complete motion event across segment rotations.
// 0 = unlimited. When reached, recording stops and the safety cooldown begins.
extern int cfg_recording_event_max_seconds;
extern int cfg_recording_event_cooldown_seconds;
// Earliest local date/time at which a NEW automatic recording may start.
// "off" = no time gate; otherwise strict local ISO format YYYY-MM-DDTHH:MM:SS.
extern String cfg_recording_not_before;

// Motion recording decision. Existing radar/OT2 wake remains authoritative;
// image_verify adds a camera confirmation step before a NEW recording starts.
extern String cfg_motion_recording_decision; // "direct" or "image_verify"
extern int cfg_image_motion_enabled;
extern int cfg_image_motion_sensitivity;
extern int cfg_image_motion_min_area_pct;
extern int cfg_image_motion_confirm_frames;
extern int cfg_image_motion_release_frames;
extern int cfg_image_motion_background_learning;
extern int cfg_image_motion_global_mean_delta;
extern int cfg_image_motion_global_change_pct;
extern String cfg_image_motion_roi_mask; // 20x15 compact bit mask, 76 hex chars

// Sleep / power management
extern String cfg_sleep_mode;         // "off", "light_sleep", "deep_sleep"
extern int cfg_sleep_delay_ms;        // 0..60000 ms idle delay before sleep
extern int cfg_bootloop_protection;   // 0/1, persistent unstable-cold-boot protection

// Transport mode.
// transport_mode is an operational persistent flag set from WebConfig.
// While active, normal radar/PIR/WiFi startup is bypassed. The camera is
// sampled periodically; once the cover is reliably removed an installation
// delay runs before normal operation is enabled.
extern int cfg_transport_mode;                    // 0/1
extern int cfg_transport_check_seconds;           // 10..3600
extern int cfg_transport_light_confirm_seconds;   // 0..120
extern int cfg_transport_install_delay_seconds;   // 0..86400
extern int cfg_transport_max_duration_seconds;      // 3600..604800, hard fallback to normal mode
extern int cfg_transport_black_mean_max;          // 0..255
extern int cfg_transport_black_p95_max;           // 0..255

// Storage safety
extern int cfg_min_free_space_mb;     // default: 100 MB
extern String cfg_disk_full_action;   // "rollover" or "stop"

// WiFi / NTP / local time
extern String cfg_hostname;
extern String cfg_timezone;             // POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3
extern String cfg_wifi_on_system_start; // "off", "on", "on_missing_time"
extern int cfg_wifi_timeout_sec;       // 0 = auto-off disabled, default 60 s
extern String cfg_wifi_ssid;
extern String cfg_wifi_pass;

// Hotspot / access point
extern int cfg_hotspot_enabled;       // 0/1, automatic AP start at boot
extern String cfg_hotspot_password;  // 8..63 chars
extern int cfg_hotspot_hidden;        // 0/1

// Web interface authentication
extern int cfg_web_auth_enabled;      // 0/1
extern int cfg_web_recording_auto_pause; // 0/1, pause recording automation when WebConfig UI is opened
extern String cfg_web_language;       // "de" or "en"; optional in older configs
extern String cfg_web_username;
extern String cfg_web_password;       // 8..63 chars

// Logging
extern String cfg_log_file;
extern int cfg_debug_enabled;


// =============================================================
// CONFIG SOURCE / SHADOW STATUS
// =============================================================

enum ConfigSource {
    CONFIG_SOURCE_DEFAULTS = 0,
    CONFIG_SOURCE_SD,
    CONFIG_SOURCE_INTERNAL
};


// Load configuration.
//
// Priority:
//   1. valid /config.txt on SD
//   2. valid /config.txt in internal LittleFS
//   3. firmware defaults
//
// If the SD config is valid, its exact contents are copied to
// the internal LittleFS shadow.
void loadConfig(bool sdAvailable);


// Active source selected during loadConfig().
ConfigSource configGetSource();
const char *configSourceName();


// Status of the two physical copies.
bool configSdAvailable();
bool configSdPresent();
bool configSdValid();
bool configInternalAvailable();
bool configInternalValid();


// Re-check the current SD /config.txt without changing the
// active runtime configuration. Useful after SD wipe/card change.
void configRefreshSdStatus();


// Human-readable SD status for WebConfig.
const char *configSdStatusName();


// Read and validate the authoritative internal LittleFS /config.txt.
// The returned text is byte-for-byte the stored file contents.
bool configReadInternalText(
    String &text,
    String &error
);

// Explicitly create/replace SD /config.txt from the validated internal copy.
// This is the only operation that intentionally creates an SD config when one
// is currently absent.
bool configCopyInternalToSd(
    String &error
);

// Remove SD /config.txt and its atomic-write recovery files. Refuses to do so
// unless a valid internal LittleFS copy is available first.
bool configDeleteSdCopy(
    String &error
);


// Board-specific recording performance guard.
//
// The limit itself lives in board_config.h because it describes qualified
// hardware performance, not a user preference. The score is a deliberately
// coarse weighted pixel rate used only as an upper configuration ceiling:
//
//   width * height * fps * JPEG-quality-weight / 100
//
// A board limit of 0 means that no board-specific ceiling has been qualified.
uint32_t configRecordingPerformanceLimit();
uint16_t configRecordingQualityWeightPercent(int quality);
uint32_t configRecordingPerformanceLoad(
    const String &resolution,
    int fps,
    int quality
);
int configRecordingPerformanceMaxFps(
    const String &resolution,
    int quality
);
bool configRecordingPerformanceAllowed(
    const String &resolution,
    int fps,
    int quality,
    uint32_t &load,
    uint32_t &limit
);


// Validate a complete config.txt without changing the active
// runtime configuration.
bool configValidateText(
    const String &text,
    String &error
);


// Save a complete config.txt.
//
// The internal LittleFS shadow is ALWAYS updated. SD synchronization is
// determined solely by whether SD /config.txt already exists at save time:
// present -> update SD too; absent -> remain internal-only. The writeToSd
// argument is retained for source compatibility with existing callers but
// cannot create or suppress the SD copy. Use configCopyInternalToSd() for an
// explicit transition from internal-only to SD+internal operation.
//
// Return values:
//   CONFIG_SAVE_INTERNAL_ONLY
//   CONFIG_SAVE_BOTH
//   CONFIG_SAVE_INTERNAL_FAILED
//   CONFIG_SAVE_SD_FAILED
enum ConfigSaveResult {
    CONFIG_SAVE_INTERNAL_ONLY = 0,
    CONFIG_SAVE_BOTH,
    CONFIG_SAVE_INTERNAL_FAILED,
    CONFIG_SAVE_SD_FAILED
};

ConfigSaveResult configSaveText(
    const String &text,
    bool writeToSd,
    String &error
);

// Persist only the OV3660 crop settings while preserving the complete active
// config text (including comments and future/unknown keys). This is used by
// Live Preview so a tested crop can be saved without rebuilding every field.
ConfigSaveResult configSaveCameraCrop(
    const String &zoom,
    int positionX,
    int positionY,
    bool writeToSd,
    String &error
);

// Persist only the WebConfig UI language while preserving the complete active
// config text. The language setting is intentionally UI-only; API/config/log
// field names remain language-neutral.
ConfigSaveResult configSaveWebLanguage(
    const String &languageCode,
    bool writeToSd,
    String &error
);

// Persist the image-motion decision/settings/ROI mask while preserving the
// complete active config text and all unrelated/future keys.
ConfigSaveResult configSaveImageMotion(
    const String &recordingDecision,
    int enabled,
    int sensitivity,
    int minAreaPct,
    int confirmFrames,
    int releaseFrames,
    int backgroundLearning,
    int globalMeanDelta,
    int globalChangePct,
    const String &roiMask,
    bool writeToSd,
    String &error
);

// Persist only transport_mode while preserving the complete active config text.
// Used by the dedicated WebConfig action and by the automatic transport release.
ConfigSaveResult configSaveTransportMode(
    int mode,
    bool writeToSd,
    String &error
);

// Persist only the transport timing/black-detection settings while preserving
// the complete active config text. Used by the dedicated Transport page.
ConfigSaveResult configSaveTransportSettings(
    int checkSeconds,
    int lightConfirmSeconds,
    int installDelaySeconds,
    int maxDurationSeconds,
    int blackMeanMax,
    int blackP95Max,
    bool writeToSd,
    String &error
);


// Time-based recording arming. The configured date/time is interpreted in
// cfg_timezone. A configured deadline with an invalid system clock fails safe:
// NEW recordings remain blocked until the clock becomes valid.
enum RecordingNotBeforeState : uint8_t {
    RECORDING_NOT_BEFORE_OFF = 0,
    RECORDING_NOT_BEFORE_WAITING,
    RECORDING_NOT_BEFORE_REACHED,
    RECORDING_NOT_BEFORE_CLOCK_INVALID,
    RECORDING_NOT_BEFORE_INVALID
};

RecordingNotBeforeState configRecordingNotBeforeState(
    time_t *deadlineEpoch = nullptr,
    int64_t *remainingSeconds = nullptr
);

bool configRecordingAllowedNow();
const char *configRecordingNotBeforeStateName(RecordingNotBeforeState state);
String configRecordingNotBeforeDisplay();

// Existing helpers
String makeFolder();
String makeFilename();
bool timeIsValid();