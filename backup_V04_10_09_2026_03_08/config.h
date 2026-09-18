#pragma once

#include <Arduino.h>

// Camera
extern String cfg_camera;
extern String cfg_resolution;
extern int cfg_fps;
extern int cfg_quality;
extern int cfg_camera_auto_exposure;   // 0/1
extern int cfg_camera_ae_level;        // -2..2
extern int cfg_rotation;

// Recording
extern int cfg_post_ms;
extern int cfg_led_enabled;
extern String cfg_recording_format;   // "avi" or "mkv"
extern int cfg_timestamp_enabled;     // 0/1
extern int cfg_recording_segment_seconds; // 0 = unlimited
extern int cfg_recording_segment_max_mb;  // 0 = unlimited

// Sleep / power management
extern String cfg_sleep_mode;         // "off", "light_sleep", "deep_sleep"
extern int cfg_sleep_delay_ms;        // 0..60000 ms idle delay before sleep

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


// Validate a complete config.txt without changing the active
// runtime configuration.
bool configValidateText(
    const String &text,
    String &error
);


// Save a complete config.txt.
//
// The internal LittleFS shadow is ALWAYS updated.
// The SD card is updated only when writeToSd == true.
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


// Existing helpers
String makeFolder();
String makeFilename();
bool timeIsValid();
