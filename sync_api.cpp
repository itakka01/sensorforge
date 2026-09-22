#include "sync_api.h"

#include "board_config.h"
#include "branding.h"
#include "config.h"
#include "logger.h"
#include "radar.h"
#include "recorder.h"
#include "rtc.h"
#include "storage_guard.h"
#include "thermal.h"
#include "webplayer.h"
#include "recording_storage.h"
#include "image_motion.h"

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <Preferences.h>
#include <algorithm>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>

// High-level state / operational motion decision from the main firmware.
extern bool recording;
extern bool sdReady;
extern bool webConfigStarted;
extern bool cameraInitialized;
extern bool motionDetected();
extern bool recordingSafetyCooldownActive();
extern uint32_t recordingSafetyCooldownRemainingSeconds();
extern String firmwareBuildTimestamp();

// Continuous-shooter runtime telemetry/actions exposed by the main firmware.
// These are read-only except for the explicit flush action below; configuration
// mutation remains in the established config/WebConfig path for API 1.0.
extern bool continuousShooterFlushNow();
extern uint32_t continuousShooterBufferedFrameCount();
extern uint32_t continuousShooterBufferUsedBytes();
extern uint32_t continuousShooterBufferCapacityBytes();
extern uint32_t continuousShooterAcceptedFrameCount();
extern uint32_t continuousShooterRejectedDarkCount();
extern uint32_t continuousShooterRejectedSimilarCount();
extern uint64_t continuousShooterAcceptedJpegByteCount();
extern bool continuousShooterLastBrightnessValid();
extern float continuousShooterLastBrightnessMean();
extern uint8_t continuousShooterLastBrightnessPeak();
extern uint32_t continuousShooterLastBrightnessAgeMs();

namespace {

static WebServer *syncServer = nullptr;

// API protocol revision. The build suffix is generated automatically from the
// firmware compile timestamp, so every newly compiled API identifies itself
// with a fresh, chronologically increasing build version.
static const uint16_t SYNC_API_VERSION_MAJOR = 1;
static const uint16_t SYNC_API_VERSION_MINOR = 12;

// Stable integration contract intended for Home Assistant and other local
// automation clients. The transport/protocol version above may grow additively,
// while this profile changes only when the documented integration schema does.
static const uint16_t INTEGRATION_API_VERSION_MAJOR = 1;
static const uint16_t INTEGRATION_API_VERSION_MINOR = 0;

static const size_t SYNC_TRANSFER_BUFFER_PREFERRED = 32U * 1024U;
static const size_t SYNC_TRANSFER_BUFFER_MINIMUM = 4U * 1024U;
static const size_t SYNC_SOCKET_WRITE_CHUNK = 32U * 1024U;
static const uint32_t SYNC_SOCKET_STALL_TIMEOUT_MS = 5000UL;

// API-exclusive mode is RAM-only and leased. A vanished client can never
// leave recording disabled permanently.
static const uint32_t SYNC_EXCLUSIVE_LEASE_MS = 60000UL;
static bool syncExclusiveActiveState = false;
static uint32_t syncExclusiveLastActivityMs = 0;

static const uint64_t SYNC_TEST_DEFAULT_BYTES = 1ULL * 1024ULL * 1024ULL;
static const uint64_t SYNC_TEST_MAX_BYTES = 64ULL * 1024ULL * 1024ULL;

#if defined(STORAGE_SPI)
static const uint32_t SYNC_SPI_TEST_ALLOWED_MHZ[] = {4U, 8U, 12U, 16U, 20U, 24U, 32U, 40U};
#endif

struct ApiFileEntry {
    String name;
    String fullPath;
    uint64_t size;
};

static WebServer &server()
{
    return *syncServer;
}

static uint8_t compileMonthNumber(const char *dateText)
{
    static const char *months =
        "JanFebMarAprMayJunJulAugSepOctNovDec";

    for (uint8_t month = 0; month < 12; ++month) {
        const char *candidate = months + (month * 3U);
        if (dateText[0] == candidate[0] &&
            dateText[1] == candidate[1] &&
            dateText[2] == candidate[2]) {
            return month + 1U;
        }
    }

    return 0;
}

static const String &syncApiVersionText()
{
    static String version;

    if (!version.length()) {
        const char *dateText = __DATE__;
        const char *timeText = __TIME__;

        uint8_t month = compileMonthNumber(dateText);
        int day = atoi(dateText + 4);
        int year = atoi(dateText + 7);

        char buffer[48];
        snprintf(
            buffer,
            sizeof(buffer),
            "%u.%u.%04d%02u%02d.%c%c%c%c%c%c",
            (unsigned)SYNC_API_VERSION_MAJOR,
            (unsigned)SYNC_API_VERSION_MINOR,
            year,
            (unsigned)month,
            day,
            timeText[0], timeText[1],
            timeText[3], timeText[4],
            timeText[6], timeText[7]
        );

        version = buffer;
    }

    return version;
}

static String syncClientVersionText()
{
    if (server().hasHeader("X-SensorForge-Client-Version")) {
        String value = server().header("X-SensorForge-Client-Version");
        value.trim();
        if (value.length())
            return value;
    }

    return String("unknown");
}

static void sendApiVersionHeader()
{
    server().sendHeader(
        "X-SensorForge-API-Version",
        syncApiVersionText()
    );
    server().sendHeader(
        "X-SensorForge-Device-ID",
        cfg_hostname
    );
    server().sendHeader(
        "X-SensorForge-Integration-API-Version",
        String(INTEGRATION_API_VERSION_MAJOR) + "." +
        String(INTEGRATION_API_VERSION_MINOR)
    );
}

#if defined(STORAGE_SPI)
struct SpiClockSwitchResult {
    bool targetMounted;
    bool fallbackMounted;
    bool rebootRequired;
};

// Defined with the SPI diagnostic helpers below. Forward declaration is needed
// because exclusive lease handling also uses the same proven remount primitive.
static bool remountSpiStorageAtHz(uint32_t frequencyHz);

static SpiClockSwitchResult switchSpiStorageClock(
    uint32_t targetHz,
    uint32_t fallbackHz,
    bool tryFallback,
    const char *reason
)
{
    SpiClockSwitchResult result = {false, false, false};

    bool previousStorageLock = g_storageLocked;
    bool previousRecordingBlock = g_recordingStartBlocked;

    // No SD File handle may survive a remount. Raise the existing cooperative
    // gates first, stop the player, and close the persistent logger handle.
    g_storageLocked = true;
    g_recordingStartBlocked = true;
    webPlayerStop();
    logClose();

    result.targetMounted = remountSpiStorageAtHz(targetHz);

    if (!result.targetMounted && tryFallback) {
        result.fallbackMounted = remountSpiStorageAtHz(fallbackHz);
    }

    if (result.targetMounted || result.fallbackMounted) {
        // Reopen the logger on the fresh mount before restoring the old gates,
        // matching the established SD-maintenance ordering in WebConfig.
        logInit();
        g_recordingStartBlocked = previousRecordingBlock;
        g_storageLocked = previousStorageLock;
    } else {
        // Fail safe. The card could not be remounted at either requested clock.
        // Leave all new SD/recording work blocked; reboot performs normal init.
        g_recordingStartBlocked = true;
        g_storageLocked = true;
        result.rebootRequired = true;
    }

    Serial.printf(
        "Sync API SD SPI switch | reason=%s | target=%lu MHz | target_ok=%s | fallback=%lu MHz | fallback_ok=%s | reboot_required=%s\n",
        reason ? reason : "unknown",
        (unsigned long)(targetHz / 1000000UL),
        result.targetMounted ? "yes" : "no",
        (unsigned long)(fallbackHz / 1000000UL),
        result.fallbackMounted ? "yes" : "no",
        result.rebootRequired ? "yes" : "no"
    );

    return result;
}
#endif

static void serviceExclusiveLease()
{
    if (!syncExclusiveActiveState)
        return;

    if (millis() - syncExclusiveLastActivityMs < SYNC_EXCLUSIVE_LEASE_MS)
        return;

#if defined(STORAGE_SPI)
    // Keep the established lease/storage coordination even when NORMAL and MAX
    // use the same clock. If the clocks differ, expiry also needs a safe remount.
    if (g_storageLocked || recording || recorderIsOpen())
        return;

#if SD_SPI_NORMAL_FREQUENCY_HZ != SD_SPI_MAX_FREQUENCY_HZ
    SpiClockSwitchResult result = switchSpiStorageClock(
        SD_SPI_NORMAL_FREQUENCY_HZ,
        SD_SPI_NORMAL_FREQUENCY_HZ,
        false,
        "exclusive_lease_expired"
    );

    if (!result.targetMounted) {
        // Fail safe: switchSpiStorageClock already left storage/recording gated.
        // Keep the exclusive state true so motion remains suppressed as well.
        syncExclusiveLastActivityMs = millis();
        Serial.println(
            "Sync API exclusive expiry: NORMAL SD remount failed; reboot required"
        );
        return;
    }
#endif
#endif

    syncExclusiveActiveState = false;
    syncExclusiveLastActivityMs = 0;

#if defined(STORAGE_SPI)
    Serial.printf(
        "Sync API exclusive lease expired | SD SPI=%lu MHz\n",
        (unsigned long)(SD_SPI_NORMAL_FREQUENCY_HZ / 1000000UL)
    );
#else
    Serial.println("Sync API exclusive lease expired");
#endif
}

static void touchExclusiveLease()
{
    serviceExclusiveLease();
    if (syncExclusiveActiveState)
        syncExclusiveLastActivityMs = millis();
}

static uint32_t exclusiveRemainingSeconds()
{
    serviceExclusiveLease();
    if (!syncExclusiveActiveState)
        return 0;

    uint32_t elapsed = millis() - syncExclusiveLastActivityMs;
    if (elapsed >= SYNC_EXCLUSIVE_LEASE_MS)
        return 0;

    return (SYNC_EXCLUSIVE_LEASE_MS - elapsed + 999UL) / 1000UL;
}

static void serviceLongOperation()
{
    esp_task_wdt_reset();
    yield();
}

static String uint64Text(uint64_t value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
    return String(buffer);
}

static String jsonEscape(const String &value)
{
    String escaped;
    escaped.reserve(value.length() + 8);

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if ((uint8_t)c < 0x20U) {
                    char u[7];
                    snprintf(u, sizeof(u), "\\u%04x", (unsigned)((uint8_t)c));
                    escaped += u;
                } else {
                    escaped += c;
                }
                break;
        }
    }

    return escaped;
}

static bool authenticate()
{
    if (server().authenticate(cfg_web_username.c_str(), cfg_web_password.c_str()))
        return true;

    // Even an authentication challenge identifies the API build.
    sendApiVersionHeader();
    server().requestAuthentication();
    return false;
}

static void sendJsonError(int statusCode, const char *code, const char *message)
{
    String json =
        "{\"ok\":false,\"error\":\"" + jsonEscape(String(code)) +
        "\",\"message\":\"" + jsonEscape(String(message)) + "\"}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(statusCode, "application/json; charset=utf-8", json);
}

static void sendBusy(const char *reason)
{
    server().sendHeader("Retry-After", "2");
    sendJsonError(409, "busy", reason);
}

static bool isDateFolderName(const String &name)
{
    if (name.length() != 8)
        return false;

    for (size_t i = 0; i < 8; ++i) {
        if (!isDigit(name[i]))
            return false;
    }

    return true;
}

static bool dayDescending(const String &a, const String &b)
{
    return a.compareTo(b) > 0;
}

static bool fileAscending(const ApiFileEntry &a, const ApiFileEntry &b)
{
    return a.name.compareTo(b.name) < 0;
}

static bool isFinalRecordingName(const String &name)
{
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".avi") ||
           lower.endsWith(".mkv") ||
           lower.endsWith(".srt") ||
           lower.endsWith(".jpg") ||
           lower.endsWith(".jpeg");
}

static const char *contentTypeForPath(const String &path)
{
    String lower = path;
    lower.toLowerCase();

    if (lower.endsWith(".mkv"))
        return "video/x-matroska";
    if (lower.endsWith(".srt"))
        return "application/x-subrip";
    if (lower.endsWith(".jpg") || lower.endsWith(".jpeg"))
        return "image/jpeg";
    return "video/x-msvideo";
}

static bool validRecordingPath(const String &path)
{
    if (!path.startsWith("/") ||
        path.indexOf("..") >= 0 ||
        path.indexOf('\\') >= 0 ||
        path.indexOf('\r') >= 0 ||
        path.indexOf('\n') >= 0 ||
        !isFinalRecordingName(path)) {
        return false;
    }

    int firstSlash = path.indexOf('/', 1);
    if (firstSlash <= 1)
        return false;

    // Exactly /<day>/<filename>. No arbitrary SD paths are exposed.
    if (path.indexOf('/', firstSlash + 1) >= 0)
        return false;

    String day = path.substring(1, firstSlash);
    if (!isDateFolderName(day) && day != "fallback")
        return false;

    String filename = path.substring(firstSlash + 1);
    return filename.length() > 4 && isFinalRecordingName(filename);
}

static bool motionNeedsRecording()
{
    // During a synchronous HTTP transfer the normal loop cannot service the
    // LD2410S parser. Keep it alive here, then ask the exact existing firmware
    // motion decision whether a recording should start.
    radarLoop();

    if (recorderIsOpen())
        return true;

    return motionDetected();
}

static bool storageReadReady()
{
    if (g_storageLocked) {
        sendBusy("storage_locked");
        return false;
    }

    if (recorderIsOpen()) {
        sendBusy("recording_active");
        return false;
    }

    // API Exclusive deliberately owns the recording-start decision. Physical
    // motion may still exist, but it must not abort or block a high-speed sync.
    if (!syncExclusiveActiveState && motionNeedsRecording()) {
        sendBusy("recording_pending");
        return false;
    }

    return true;
}

static const char *boardApiName()
{
#if defined(BOARD_XIAO)
    return "xiao_esp32s3_sense";
#elif defined(BOARD_FREENOVE)
    return "freenove_fnk0085";
#else
    return "esp32s3_unknown";
#endif
}

static const char *networkModeApiName()
{
    wifi_mode_t mode = WiFi.getMode();

    switch (mode) {
        case WIFI_MODE_STA:
            return "sta";
        case WIFI_MODE_AP:
            return "ap";
        case WIFI_MODE_APSTA:
            return "ap_sta";
        case WIFI_MODE_NULL:
        default:
            return "off";
    }
}

static String activeIpAddress()
{
    wifi_mode_t mode = WiFi.getMode();

    if (
        (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) &&
        WiFi.status() == WL_CONNECTED
    ) {
        return WiFi.localIP().toString();
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
        return WiFi.softAPIP().toString();

    return String("0.0.0.0");
}

// Home Assistant needs a stable, non-secret identity that does not depend on
// the user-editable hostname. Never expose the license hardware ID or MAC here.
// A random 128-bit public integration ID is generated once and stored in NVS.
static const String &integrationId()
{
    static String id;
    if (id.length())
        return id;

    uint8_t bytes[16] = {};
    bool persistent = false;

    Preferences prefs;
    if (prefs.begin("sfapi", false)) {
        size_t read = prefs.getBytes("iid", bytes, sizeof(bytes));
        if (read != sizeof(bytes)) {
            esp_fill_random(bytes, sizeof(bytes));
            persistent =
                prefs.putBytes("iid", bytes, sizeof(bytes)) == sizeof(bytes);
        } else {
            persistent = true;
        }
        prefs.end();
    }

    // NVS should normally be available. If it is not, still return a valid
    // per-boot identifier rather than leaking a hardware-derived identifier.
    if (!persistent) {
        esp_fill_random(bytes, sizeof(bytes));
    }

    static const char HEX_DIGITS[] = "0123456789abcdef";
    char text[3 + 32 + 1];
    text[0] = 's';
    text[1] = 'f';
    text[2] = '-';
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        text[3 + i * 2] = HEX_DIGITS[(bytes[i] >> 4) & 0x0F];
        text[4 + i * 2] = HEX_DIGITS[bytes[i] & 0x0F];
    }
    text[35] = '\0';

    id = text;
    return id;
}

static void handleDevice()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    sensor_t *cameraSensor = esp_camera_sensor_get();
    uint16_t detectedPid = cameraSensor ? cameraSensor->id.PID : 0U;

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"api_protocol_major\":" + String(SYNC_API_VERSION_MAJOR) +
        ",\"api_protocol_minor\":" + String(SYNC_API_VERSION_MINOR) +
        ",\"integration_api_version\":\"" +
            String(INTEGRATION_API_VERSION_MAJOR) + "." +
            String(INTEGRATION_API_VERSION_MINOR) +
        "\",\"integration_id\":\"" + jsonEscape(integrationId()) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"app\":\"" + jsonEscape(String(Branding::APP_NAME)) +
        "\",\"platform\":\"" + jsonEscape(String(Branding::PLATFORM)) +
        "\",\"core_version\":\"" + jsonEscape(String(Branding::CORE_VERSION)) +
        "\",\"firmware_build\":\"" + jsonEscape(firmwareBuildTimestamp()) +
        "\",\"board\":\"" + String(boardApiName()) +
        "\",\"camera_config\":\"" + jsonEscape(cfg_camera) +
        "\",\"camera_initialized\":" + String(cameraInitialized ? "true" : "false") +
        ",\"camera_pid\":" + String((unsigned)detectedPid) +
        ",\"config_source\":\"" + jsonEscape(String(configSourceName())) +
        "\",\"network_mode\":\"" + String(networkModeApiName()) +
        "\",\"capabilities\":{" +
            "\"camera_snapshot\":true," +
            "\"motion_state\":true," +
            "\"image_motion_state\":true," +
            "\"motion_recording\":true," +
            "\"continuous_shooter\":true," +
            "\"shooter_flush\":true," +
            "\"storage_status\":true," +
            "\"sensor_status\":true," +
            "\"media_browser\":true," +
            "\"rtsp\":false," +
            "\"mqtt\":false" +
        "}}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleIntegrationState()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    bool recorderOpen = recorderIsOpen();
    bool cooldown = recordingSafetyCooldownActive();
    bool imageMotionActive = imageMotionMotionActive();

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"integration_api_version\":\"" +
            String(INTEGRATION_API_VERSION_MAJOR) + "." +
            String(INTEGRATION_API_VERSION_MINOR) +
        "\",\"integration_id\":\"" + jsonEscape(integrationId()) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"uptime_ms\":" + String((unsigned long)millis()) +
        ",\"network_mode\":\"" + String(networkModeApiName()) +
        "\",\"ip\":\"" + jsonEscape(activeIpAddress()) +
        "\",\"web_config_active\":" + String(webConfigStarted ? "true" : "false") +
        ",\"camera_initialized\":" + String(cameraInitialized ? "true" : "false") +
        ",\"storage_ready\":" + String(sdReady ? "true" : "false") +
        ",\"storage_locked\":" + String(g_storageLocked ? "true" : "false") +
        ",\"recording\":" + String(recording ? "true" : "false") +
        ",\"recorder_open\":" + String(recorderOpen ? "true" : "false") +
        ",\"recording_start_blocked\":" + String(g_recordingStartBlocked ? "true" : "false") +
        ",\"motion_recording_enabled\":" + String(cfg_motion_recording_enabled ? "true" : "false") +
        ",\"motion_recording_decision\":\"" + jsonEscape(cfg_motion_recording_decision) +
        "\",\"automation_motion_detected\":" + String(motionDetected() ? "true" : "false") +
        ",\"image_motion_active\":" + String(imageMotionActive ? "true" : "false") +
        ",\"recording_cooldown_active\":" + String(cooldown ? "true" : "false") +
        ",\"recording_cooldown_remaining_seconds\":" +
            String(recordingSafetyCooldownRemainingSeconds()) +
        ",\"shooter_enabled\":" + String(cfg_shooter_enabled ? "true" : "false") +
        ",\"exclusive_active\":" + String(syncExclusiveActiveState ? "true" : "false") +
        ",\"sleep_mode\":\"" + jsonEscape(cfg_sleep_mode) +
        "\",\"sleep_delay_ms\":" + String(cfg_sleep_delay_ms) +
        "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleShooterStatus()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    uint32_t usedBytes = continuousShooterBufferUsedBytes();
    uint32_t capacityBytes = continuousShooterBufferCapacityBytes();
    uint32_t accepted = continuousShooterAcceptedFrameCount();
    uint64_t acceptedBytes = continuousShooterAcceptedJpegByteCount();
    bool brightnessValid = continuousShooterLastBrightnessValid();

    uint32_t averageJpegBytes =
        accepted > 0
        ? (uint32_t)(acceptedBytes / (uint64_t)accepted)
        : 0U;

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"integration_id\":\"" + jsonEscape(integrationId()) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"enabled\":" + String(cfg_shooter_enabled ? "true" : "false") +
        ",\"storage_format\":\"" + jsonEscape(cfg_shooter_storage_format) +
        "\",\"interval_ms\":" + String(cfg_shooter_interval_ms) +
        ",\"dark_mean_min\":" + String(cfg_shooter_dark_mean_min) +
        ",\"min_change_pct\":" + String(cfg_shooter_min_change_pct, 1) +
        ",\"force_save_seconds\":" + String(cfg_shooter_force_save_seconds) +
        ",\"flush_seconds\":" + String(cfg_shooter_flush_seconds) +
        ",\"buffered_frames\":" + String(continuousShooterBufferedFrameCount()) +
        ",\"buffer_used_bytes\":" + String(usedBytes) +
        ",\"buffer_capacity_bytes\":" + String(capacityBytes) +
        ",\"buffer_fill_pct\":" + String(
            capacityBytes > 0
            ? ((float)usedBytes * 100.0f / (float)capacityBytes)
            : 0.0f,
            1
        ) +
        ",\"accepted_frames\":" + String(accepted) +
        ",\"accepted_jpeg_bytes\":" + uint64Text(acceptedBytes) +
        ",\"average_jpeg_bytes\":" + String(averageJpegBytes) +
        ",\"rejected_dark\":" + String(continuousShooterRejectedDarkCount()) +
        ",\"rejected_similar\":" + String(continuousShooterRejectedSimilarCount()) +
        ",\"brightness_valid\":" + String(brightnessValid ? "true" : "false") +
        ",\"brightness_mean\":" + String(
            brightnessValid ? continuousShooterLastBrightnessMean() : 0.0f,
            1
        ) +
        ",\"brightness_peak\":" + String(
            brightnessValid ? (unsigned)continuousShooterLastBrightnessPeak() : 0U
        ) +
        ",\"brightness_age_ms\":" + String(
            brightnessValid ? continuousShooterLastBrightnessAgeMs() : 0U
        ) +
        "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleShooterFlush()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (recording || recorderIsOpen()) {
        sendBusy("recording_active");
        return;
    }

    if (!sdReady) {
        sendJsonError(503, "storage_unavailable", "storage_is_not_ready");
        return;
    }

    if (g_storageLocked) {
        sendBusy("storage_locked");
        return;
    }

    if (syncExclusiveActiveState) {
        sendBusy("exclusive_active");
        return;
    }

    if (!continuousShooterFlushNow()) {
        sendBusy("shooter_flush_deferred");
        return;
    }

    String json =
        "{\"ok\":true,\"flushed\":true,\"buffered_frames\":" +
        String(continuousShooterBufferedFrameCount()) +
        ",\"buffer_used_bytes\":" +
        String(continuousShooterBufferUsedBytes()) +
        "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleStatus()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    bool recorderOpen = recorderIsOpen();
    bool exclusiveActive = syncExclusiveActiveState;
    String ip = WiFi.status() == WL_CONNECTED
        ? WiFi.localIP().toString()
        : WiFi.softAPIP().toString();

    String clientVersion = syncClientVersionText();
    String apiVersion = syncApiVersionText();

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(apiVersion) +
        "\",\"api_protocol_major\":" + String(SYNC_API_VERSION_MAJOR) +
        ",\"api_protocol_minor\":" + String(SYNC_API_VERSION_MINOR) +
        ",\"client_version\":\"" + jsonEscape(clientVersion) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"app\":\"" +
        jsonEscape(String(Branding::APP_NAME)) +
        "\",\"platform\":\"" + jsonEscape(String(Branding::PLATFORM)) +
        "\",\"core_version\":\"" + jsonEscape(String(Branding::CORE_VERSION)) +
        "\",\"recording\":" + String(recording ? "true" : "false") +
        ",\"recorder_open\":" + String(recorderOpen ? "true" : "false") +
        ",\"storage_locked\":" + String(g_storageLocked ? "true" : "false") +
        ",\"sync_available\":" +
        String((!recorderOpen && !g_storageLocked) ? "true" : "false") +
        ",\"exclusive_active\":" + String(exclusiveActive ? "true" : "false") +
        ",\"exclusive_remaining_seconds\":" + String(exclusiveRemainingSeconds());

#if defined(STORAGE_SPI)
    json +=
        ",\"sd_spi_normal_mhz\":" +
        String((unsigned long)(SD_SPI_NORMAL_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_max_mhz\":" +
        String((unsigned long)(SD_SPI_MAX_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_active_mhz\":" +
        String((unsigned long)(
            (exclusiveActive
                ? SD_SPI_MAX_FREQUENCY_HZ
                : SD_SPI_NORMAL_FREQUENCY_HZ) / 1000000UL
        ));
#endif

    json +=
        ",\"ip\":\"" + jsonEscape(ip) + "\"}";

    Serial.printf(
        "Sync API status | client=%s | host=%s\n",
        clientVersion.c_str(),
        apiVersion.c_str()
    );

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleStorageStatus()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t reserveBytes = storageReserveBytes();

    if (!g_storageLocked) {
        totalBytes = STORAGE.totalBytes();
        usedBytes = STORAGE.usedBytes();
        if (usedBytes < totalBytes)
            freeBytes = totalBytes - usedBytes;
    }

#if defined(STORAGE_SPI)
    const char *storageBackend = "spi";
#elif defined(STORAGE_SDMMC)
    const char *storageBackend = "sd_mmc";
#else
    const char *storageBackend = "unknown";
#endif

    bool reserveOk =
        !g_storageLocked &&
        (reserveBytes == 0 || freeBytes >= reserveBytes);

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"backend\":\"" + String(storageBackend) +
        "\",\"storage_locked\":" + String(g_storageLocked ? "true" : "false") +
        ",\"recording\":" + String(recording ? "true" : "false") +
        ",\"recorder_open\":" + String(recorderIsOpen() ? "true" : "false") +
        ",\"total_bytes\":" + uint64Text(totalBytes) +
        ",\"used_bytes\":" + uint64Text(usedBytes) +
        ",\"free_bytes\":" + uint64Text(freeBytes) +
        ",\"reserve_bytes\":" + uint64Text(reserveBytes) +
        ",\"reserve_ok\":" + String(reserveOk ? "true" : "false");

#if defined(STORAGE_SPI)
    json +=
        ",\"sd_spi_normal_mhz\":" +
        String((unsigned long)(SD_SPI_NORMAL_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_max_mhz\":" +
        String((unsigned long)(SD_SPI_MAX_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_active_mhz\":" +
        String((unsigned long)(
            (syncExclusiveActiveState
                ? SD_SPI_MAX_FREQUENCY_HZ
                : SD_SPI_NORMAL_FREQUENCY_HZ) / 1000000UL
        ));
#endif

    json += "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleSensorsStatus()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    float cpuTempC = thermalCpuTemperatureC();
    bool cpuTempValid = isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid = thermalRtcTemperatureC(rtcTempC);

    bool rtcPresent = rtcDetected();
    String rtcTime = rtcPresent ? rtcTimeText() : String("unavailable");

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"device_id\":\"" + jsonEscape(cfg_hostname) +
        "\",\"presence\":" + String(digitalRead(PRESENCE_PIN) == HIGH ? "true" : "false") +
        ",\"radar_report_recent\":" + String(radarReportIsRecent() ? "true" : "false") +
        ",\"radar_gate_energy_recent\":" + String(radarGateEnergyIsRecent() ? "true" : "false") +
        ",\"radar_target_state\":" + String(radarLastTargetState()) +
        ",\"radar_distance_cm\":" + String(radarLastTargetDistanceCm()) +
        ",\"radar_motion_active\":" + String(radarMotionActive() ? "true" : "false") +
        ",\"radar_motion_remaining_ms\":" + String(radarMotionRemainingMs()) +
        ",\"radar_last_gate\":" + String(radarLastMotionGate()) +
        ",\"radar_last_energy_db\":" + String(radarLastMotionEnergyDb(), 1) +
        ",\"radar_energy_db\":[";

    for (uint8_t gate = 0; gate < 16; ++gate) {
        if (gate > 0)
            json += ',';
        json += String(radarGateEnergyDb(gate), 1);
    }

    json +=
        "],\"rtc_detected\":" + String(rtcPresent ? "true" : "false") +
        ",\"rtc_clock_valid\":" + String(rtcClockValid() ? "true" : "false") +
        ",\"rtc_oscillator_stopped\":" + String(rtcOscillatorStopped() ? "true" : "false") +
        ",\"rtc_eeprom_detected\":" + String(rtcEepromDetected() ? "true" : "false") +
        ",\"rtc_restored_system_time\":" + String(rtcRestoredSystemTime() ? "true" : "false") +
        ",\"rtc_type\":\"" + jsonEscape(String(rtcTypeName())) +
        "\",\"rtc_address\":" + String((unsigned)rtcI2cAddress()) +
        ",\"rtc_time\":\"" + jsonEscape(rtcTime) +
        "\",\"cpu_temp_valid\":" + String(cpuTempValid ? "true" : "false") +
        ",\"cpu_temp_c\":" + String(cpuTempValid ? cpuTempC : 0.0f, 1) +
        ",\"rtc_temp_valid\":" + String(rtcTempValid ? "true" : "false") +
        ",\"rtc_temp_c\":" + String(rtcTempValid ? rtcTempC : 0.0f, 1) +
        ",\"thermal_state\":\"" + jsonEscape(String(thermalStateName())) +
        "\",\"thermal_source\":\"" + jsonEscape(String(thermalSourceName())) +
        "\"}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleCameraSnapshot()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (recording || recorderIsOpen()) {
        sendBusy("recording_active");
        return;
    }

    if (!esp_camera_sensor_get()) {
        sendJsonError(503, "camera_unavailable", "camera_is_not_initialized");
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        sendJsonError(500, "camera_capture_failed", "cannot_capture_jpeg_frame");
        return;
    }

    size_t frameLength = fb->len;

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().setContentLength(frameLength);
    server().send(200, "image/jpeg", "");

    WiFiClient client = server().client();
    size_t sent = client.write(fb->buf, frameLength);
    esp_camera_fb_return(fb);

    if (sent != frameLength)
        client.stop();
}

static void handleDays()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (!storageReadReady())
        return;

    std::vector<String> days;
    bool fallbackExists = false;
    bool recordingPending = false;

    File root = STORAGE.open("/", FILE_READ);
    if (!root || !root.isDirectory()) {
        if (root)
            root.close();
        sendJsonError(500, "storage_open_failed", "cannot_open_storage_root");
        return;
    }

    uint16_t scanned = 0;
    File file = root.openNextFile();

    while (file) {
        if (file.isDirectory()) {
            String name = String(file.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0)
                name = name.substring(slash + 1);

            if (isDateFolderName(name))
                days.push_back(name);
            else if (name == "fallback")
                fallbackExists = true;
        }

        file.close();
        scanned++;

        if ((scanned & 0x0FU) == 0) {
            serviceLongOperation();
            if (!syncExclusiveActiveState && motionNeedsRecording()) {
                recordingPending = true;
                break;
            }
        }

        file = root.openNextFile();
    }

    if (file)
        file.close();
    root.close();

    if (recordingPending) {
        sendBusy("recording_pending");
        return;
    }

    std::sort(days.begin(), days.end(), dayDescending);
    if (fallbackExists)
        days.push_back("fallback");

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().setContentLength(CONTENT_LENGTH_UNKNOWN);
    server().send(200, "application/json; charset=utf-8", "");
    server().sendContent("{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) + "\",\"days\":[");

    for (size_t i = 0; i < days.size(); ++i) {
        if (i > 0)
            server().sendContent(",");
        server().sendContent("\"" + jsonEscape(days[i]) + "\"");
    }

    server().sendContent("]}");
}

static void handleFiles()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (!storageReadReady())
        return;

    String day = server().arg("day");
    if (!isDateFolderName(day) && day != "fallback") {
        sendJsonError(400, "invalid_day", "day_must_be_YYYYMMDD_or_fallback");
        return;
    }

    String folderPath = "/" + day;
    File root = STORAGE.open(folderPath.c_str(), FILE_READ);
    if (!root || !root.isDirectory()) {
        if (root)
            root.close();
        sendJsonError(404, "day_not_found", "recording_folder_not_found");
        return;
    }

    std::vector<ApiFileEntry> entries;
    bool recordingPending = false;
    uint16_t scanned = 0;
    File file = root.openNextFile();

    while (file) {
        if (!file.isDirectory()) {
            String name = String(file.name());
            int slash = name.lastIndexOf('/');
            if (slash >= 0)
                name = name.substring(slash + 1);

            if (isFinalRecordingName(name)) {
                ApiFileEntry entry;
                entry.name = name;
                entry.fullPath = folderPath + "/" + name;

                uint64_t logicalSize = 0;

                if (!recordingStorageLogicalSize(
                        entry.fullPath,
                        logicalSize
                    )) {
                    logicalSize =
                        (uint64_t)file.size();
                }

                entry.size = logicalSize;
                entries.push_back(entry);
            }
        }

        file.close();
        scanned++;

        if ((scanned & 0x0FU) == 0) {
            serviceLongOperation();
            if (!syncExclusiveActiveState && motionNeedsRecording()) {
                recordingPending = true;
                break;
            }
        }

        file = root.openNextFile();
    }

    if (file)
        file.close();
    root.close();

    if (recordingPending) {
        sendBusy("recording_pending");
        return;
    }

    std::sort(entries.begin(), entries.end(), fileAscending);

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().setContentLength(CONTENT_LENGTH_UNKNOWN);
    server().send(200, "application/json; charset=utf-8", "");
    server().sendContent(
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"day\":\"" + jsonEscape(day) + "\",\"files\":["
    );

    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0)
            server().sendContent(",");

        String lower = entries[i].name;
        lower.toLowerCase();
        const char *type = lower.endsWith(".mkv") ? "mkv" :
                           (lower.endsWith(".srt") ? "srt" :
                           ((lower.endsWith(".jpg") || lower.endsWith(".jpeg")) ? "jpg" : "avi"));

        server().sendContent(
            "{\"name\":\"" + jsonEscape(entries[i].name) +
            "\",\"path\":\"" + jsonEscape(entries[i].fullPath) +
            "\",\"size\":" + uint64Text(entries[i].size) +
            ",\"type\":\"" + String(type) + "\"}"
        );
    }

    server().sendContent("]}");
}

static bool parseUnsigned64(const String &text, uint64_t &value)
{
    if (!text.length())
        return false;

    uint64_t result = 0;
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        if (c < '0' || c > '9')
            return false;

        uint8_t digit = (uint8_t)(c - '0');
        if (result > (UINT64_MAX - digit) / 10ULL)
            return false;

        result = result * 10ULL + digit;
    }

    value = result;
    return true;
}

static bool parseRangeHeader(
    const String &rangeHeader,
    uint64_t fileSize,
    uint64_t &rangeStart,
    uint64_t &rangeEnd,
    bool &partial
)
{
    partial = false;
    rangeStart = 0;
    rangeEnd = fileSize > 0 ? fileSize - 1ULL : 0;

    if (!rangeHeader.length())
        return true;
    if (!rangeHeader.startsWith("bytes="))
        return false;

    String spec = rangeHeader.substring(6);
    spec.trim();
    if (!spec.length() || spec.indexOf(',') >= 0)
        return false;

    int dash = spec.indexOf('-');
    if (dash < 0)
        return false;

    String startText = spec.substring(0, dash);
    String endText = spec.substring(dash + 1);
    startText.trim();
    endText.trim();

    if (!startText.length()) {
        uint64_t suffixLength = 0;
        if (!parseUnsigned64(endText, suffixLength) || suffixLength == 0 || fileSize == 0)
            return false;
        if (suffixLength > fileSize)
            suffixLength = fileSize;

        rangeStart = fileSize - suffixLength;
        rangeEnd = fileSize - 1ULL;
        partial = true;
        return true;
    }

    if (!parseUnsigned64(startText, rangeStart) || fileSize == 0 || rangeStart >= fileSize)
        return false;

    if (endText.length()) {
        if (!parseUnsigned64(endText, rangeEnd) || rangeEnd < rangeStart)
            return false;
        if (rangeEnd >= fileSize)
            rangeEnd = fileSize - 1ULL;
    } else {
        rangeEnd = fileSize - 1ULL;
    }

    partial = true;
    return true;
}

static uint8_t *allocateTransferBuffer(size_t &bufferSize)
{
    bufferSize = SYNC_TRANSFER_BUFFER_PREFERRED;

    while (bufferSize >= SYNC_TRANSFER_BUFFER_MINIMUM) {
        uint8_t *buffer = (uint8_t *)malloc(bufferSize);
        if (buffer)
            return buffer;
        bufferSize /= 2U;
    }

    bufferSize = 0;
    return nullptr;
}

// Double-buffered SD -> TCP pipeline used for exclusive high-speed transfers.
//
// The old transfer path was strictly sequential: read one SD block, wait until
// that complete block had been accepted by TCP, then read the next SD block.
// This makes SD-read time and TCP-write time add together. During API Exclusive
// mode recording is already blocked, so a small reader task can safely prefetch
// the next SD block while the request task is sending the current block.
//
// The public HTTP protocol does not change: Range/Resume and Content-Length are
// identical. If RAM/task allocation fails we simply keep the proven sequential
// path below.
struct SyncPipelineBuffer {
    uint8_t *data;
    size_t length;
};

struct SyncPipelineContext {
    RecordingStorageFile *file;
    SyncPipelineBuffer buffers[2];
    size_t bufferSize;
    uint64_t remainingToRead;
    QueueHandle_t freeQueue;
    QueueHandle_t readyQueue;
    SemaphoreHandle_t doneSemaphore;
    volatile bool stopRequested;
    volatile bool readError;
};

static bool allocateTransferBufferPair(
    uint8_t *&bufferA,
    uint8_t *&bufferB,
    size_t &bufferSize
)
{
    bufferA = nullptr;
    bufferB = nullptr;
    bufferSize = SYNC_TRANSFER_BUFFER_PREFERRED;

    while (bufferSize >= SYNC_TRANSFER_BUFFER_MINIMUM) {
        bufferA = (uint8_t *)malloc(bufferSize);
        if (bufferA)
            bufferB = (uint8_t *)malloc(bufferSize);

        if (bufferA && bufferB)
            return true;

        if (bufferA)
            free(bufferA);
        if (bufferB)
            free(bufferB);

        bufferA = nullptr;
        bufferB = nullptr;
        bufferSize /= 2U;
    }

    bufferSize = 0;
    return false;
}

static void syncPipelineReaderTask(void *parameter)
{
    SyncPipelineContext *context =
        static_cast<SyncPipelineContext *>(parameter);

    while (!context->stopRequested && context->remainingToRead > 0) {
        uint8_t index = 0;

        while (!context->stopRequested) {
            if (xQueueReceive(
                    context->freeQueue,
                    &index,
                    pdMS_TO_TICKS(25)
                ) == pdTRUE) {
                break;
            }
        }

        if (context->stopRequested)
            break;

        size_t wanted =
            context->remainingToRead < (uint64_t)context->bufferSize
            ? (size_t)context->remainingToRead
            : context->bufferSize;

        size_t got =
            context->file->read(
                context->buffers[index].data,
                wanted
            );

        if (got == 0) {
            context->readError = true;
            break;
        }

        context->buffers[index].length = got;
        context->remainingToRead -= (uint64_t)got;

        bool queued = false;
        while (!context->stopRequested) {
            if (xQueueSend(
                    context->readyQueue,
                    &index,
                    pdMS_TO_TICKS(25)
                ) == pdTRUE) {
                queued = true;
                break;
            }
        }

        if (!queued)
            break;
    }

    xSemaphoreGive(context->doneSemaphore);
    vTaskDelete(nullptr);
}

static void waitForPipelineReader(SyncPipelineContext &context)
{
    // file.read() is expected to be bounded. Keep servicing the watchdog while
    // waiting so cancellation after a client disconnect cannot trip loopTask.
    while (xSemaphoreTake(
               context.doneSemaphore,
               pdMS_TO_TICKS(50)
           ) != pdTRUE) {
        serviceLongOperation();
    }
}

static bool clientWriteAll(
    WiFiClient &client,
    const uint8_t *data,
    size_t length,
    bool allowRecordingPreemption,
    bool &recordingPreempted
)
{
    size_t offset = 0;
    uint32_t stalledSince = millis();

    while (offset < length) {
        if (!client.connected())
            return false;

        if (allowRecordingPreemption && motionNeedsRecording()) {
            recordingPreempted = true;
            return false;
        }

        size_t remaining = length - offset;
        size_t writeSize = remaining < SYNC_SOCKET_WRITE_CHUNK
            ? remaining
            : SYNC_SOCKET_WRITE_CHUNK;

        size_t written = client.write(data + offset, writeSize);
        if (written > 0) {
            offset += written;
            stalledSince = millis();
        } else {
            if (millis() - stalledSince > SYNC_SOCKET_STALL_TIMEOUT_MS)
                return false;
            delay(1);
        }

        touchExclusiveLease();
        serviceLongOperation();
    }

    return true;
}

static void handleFile()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (!storageReadReady())
        return;

    String path = server().arg("path");
    if (!validRecordingPath(path)) {
        sendJsonError(400, "invalid_path", "only_finalized_recording_files_are_available");
        return;
    }

    // Release an old player session before allocating the encrypted-read
    // buffers and starting a potentially long API transfer.
    webPlayerStop();

    RecordingStorageFile file;

    if (!file.openRead(path) || file.isDirectory()) {
        file.close();
        sendJsonError(404, "file_not_found", "recording_file_not_found_or_unreadable");
        return;
    }

    uint64_t fileSize = (uint64_t)file.size();
    if (fileSize == 0) {
        file.close();
        sendJsonError(409, "empty_file", "recording_file_is_empty");
        return;
    }

    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    bool partial = false;

    if (!parseRangeHeader(server().header("Range"), fileSize, rangeStart, rangeEnd, partial)) {
        file.close();
        server().sendHeader("Accept-Ranges", "bytes");
        server().sendHeader("Content-Range", "bytes */" + uint64Text(fileSize));
        sendJsonError(416, "range_not_satisfiable", "invalid_or_unsupported_range");
        return;
    }

    uint64_t sendLength = rangeEnd - rangeStart + 1ULL;
    if (!file.seek((uint32_t)rangeStart)) {
        file.close();
        sendJsonError(500, "seek_failed", "cannot_seek_recording_file");
        return;
    }

    // Prefer a two-buffer pipeline only while API Exclusive is active. This is
    // the high-speed mode: recording starts are already suppressed and the SD
    // card is running at the board-defined high-speed clock. Normal API reads
    // retain the simpler sequential path and its original motion-preemption
    // behavior.
    size_t bufferSize = 0;
    uint8_t *buffer = nullptr;
    uint8_t *pipelineBufferA = nullptr;
    uint8_t *pipelineBufferB = nullptr;
    bool pipelineAllocated = false;

    if (syncExclusiveActiveState) {
        pipelineAllocated = allocateTransferBufferPair(
            pipelineBufferA,
            pipelineBufferB,
            bufferSize
        );
    }

    if (!pipelineAllocated) {
        buffer = allocateTransferBuffer(bufferSize);
        if (!buffer) {
            file.close();
            sendJsonError(503, "buffer_unavailable", "cannot_allocate_transfer_buffer");
            return;
        }
    }

    String filename = path.substring(path.lastIndexOf('/') + 1);
    filename.replace("\"", "_");
    filename.replace("\r", "_");
    filename.replace("\n", "_");

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().sendHeader("Accept-Ranges", "bytes");
    server().sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    server().sendHeader("X-Content-Type-Options", "nosniff");
    server().sendHeader("X-SensorForge-File-Size", uint64Text(fileSize));

    if (partial) {
        server().sendHeader(
            "Content-Range",
            "bytes " + uint64Text(rangeStart) + "-" + uint64Text(rangeEnd) + "/" + uint64Text(fileSize)
        );
    }

    server().setContentLength((size_t)sendLength);
    server().send(partial ? 206 : 200, contentTypeForPath(path), "");

    WiFiClient client = server().client();
    uint64_t remaining = sendLength;
    uint64_t sentBytes = 0;
    uint32_t startedMs = millis();
    bool recordingPreempted = false;
    bool ok = true;
    const bool allowRecordingPreemption = !syncExclusiveActiveState;

    bool pipelineUsed = false;
    int pipelineReaderCore = -1;

    if (pipelineAllocated) {
        SyncPipelineContext context = {};
        context.file = &file;
        context.buffers[0].data = pipelineBufferA;
        context.buffers[1].data = pipelineBufferB;
        context.bufferSize = bufferSize;
        context.remainingToRead = sendLength;
        context.stopRequested = false;
        context.readError = false;
        context.freeQueue = xQueueCreate(2, sizeof(uint8_t));
        context.readyQueue = xQueueCreate(2, sizeof(uint8_t));
        context.doneSemaphore = xSemaphoreCreateBinary();

        bool pipelineReady =
            context.freeQueue &&
            context.readyQueue &&
            context.doneSemaphore;

        TaskHandle_t readerTask = nullptr;

        if (pipelineReady) {
            uint8_t first = 0;
            uint8_t second = 1;
            pipelineReady =
                xQueueSend(context.freeQueue, &first, 0) == pdTRUE &&
                xQueueSend(context.freeQueue, &second, 0) == pdTRUE;
        }

        if (pipelineReady) {
#if CONFIG_FREERTOS_UNICORE
            const BaseType_t readerCore = 0;
#else
            // Measured faster on the XIAO ESP32-S3 than pinning the reader to
            // core 1: keep SD prefetch on core 0 for the high-speed sync path.
            const BaseType_t readerCore = 0;
#endif

            BaseType_t taskResult = xTaskCreatePinnedToCore(
                syncPipelineReaderTask,
                "SyncSDRead",
                4096,
                &context,
                1,
                &readerTask,
                readerCore
            );
            pipelineReady = taskResult == pdPASS;
            if (pipelineReady)
                pipelineReaderCore = (int)readerCore;
        }

        if (pipelineReady) {
            pipelineUsed = true;

            while (remaining > 0 && client.connected()) {
                if (allowRecordingPreemption && motionNeedsRecording()) {
                    recordingPreempted = true;
                    ok = false;
                    break;
                }

                uint8_t index = 0;
                bool haveBuffer = false;

                while (client.connected() && !context.stopRequested) {
                    if (xQueueReceive(
                            context.readyQueue,
                            &index,
                            pdMS_TO_TICKS(25)
                        ) == pdTRUE) {
                        haveBuffer = true;
                        break;
                    }

                    if (context.readError)
                        break;

                    if (allowRecordingPreemption && motionNeedsRecording()) {
                        recordingPreempted = true;
                        break;
                    }

                    touchExclusiveLease();
                    serviceLongOperation();
                }

                if (!haveBuffer) {
                    ok = false;
                    break;
                }

                size_t got = context.buffers[index].length;
                if (got == 0 || got > remaining) {
                    ok = false;
                    break;
                }

                if (!clientWriteAll(
                        client,
                        context.buffers[index].data,
                        got,
                        allowRecordingPreemption,
                        recordingPreempted
                    )) {
                    ok = false;
                    break;
                }

                remaining -= (uint64_t)got;
                sentBytes += (uint64_t)got;

                if (remaining > 0) {
                    if (xQueueSend(
                            context.freeQueue,
                            &index,
                            pdMS_TO_TICKS(100)
                        ) != pdTRUE) {
                        ok = false;
                        break;
                    }
                }

                touchExclusiveLease();
                serviceLongOperation();
            }

            context.stopRequested = true;
            waitForPipelineReader(context);

            if (context.readError && remaining > 0)
                ok = false;
        }

        if (!pipelineReady) {
            // Queue/task setup failed. No reader owns the File, so fall back to
            // the original sequential algorithm using one of the allocated
            // pipeline buffers instead of failing the request.
            buffer = pipelineBufferA;
            pipelineBufferA = nullptr;

            if (pipelineBufferB) {
                free(pipelineBufferB);
                pipelineBufferB = nullptr;
            }
        }

        if (context.freeQueue)
            vQueueDelete(context.freeQueue);
        if (context.readyQueue)
            vQueueDelete(context.readyQueue);
        if (context.doneSemaphore)
            vSemaphoreDelete(context.doneSemaphore);
    }

    if (!pipelineUsed) {
        while (remaining > 0 && client.connected()) {
            if (allowRecordingPreemption && motionNeedsRecording()) {
                recordingPreempted = true;
                ok = false;
                break;
            }

            size_t wanted = remaining < (uint64_t)bufferSize
                ? (size_t)remaining
                : bufferSize;

            size_t got = file.read(buffer, wanted);
            if (got == 0) {
                ok = false;
                break;
            }

            if (!clientWriteAll(
                    client,
                    buffer,
                    got,
                    allowRecordingPreemption,
                    recordingPreempted
                )) {
                ok = false;
                break;
            }

            remaining -= (uint64_t)got;
            sentBytes += (uint64_t)got;
            serviceLongOperation();
        }
    }

    file.close();

    if (buffer)
        free(buffer);
    if (pipelineBufferA)
        free(pipelineBufferA);
    if (pipelineBufferB)
        free(pipelineBufferB);

    uint32_t elapsedMs = millis() - startedMs;

    if (ok && remaining == 0) {
        float mbPerSecond = 0.0f;
        if (elapsedMs > 0) {
            mbPerSecond =
                ((float)sentBytes / (1024.0f * 1024.0f)) /
                ((float)elapsedMs / 1000.0f);
        }

        Serial.printf(
            "Sync API: %s | bytes=%llu | offset=%llu | %lu ms | %.2f MB/s | mode=%s | buffer=%u | reader_core=%d\n",
            path.c_str(),
            (unsigned long long)sentBytes,
            (unsigned long long)rangeStart,
            (unsigned long)elapsedMs,
            mbPerSecond,
            pipelineUsed ? "double-buffer" : "sequential",
            (unsigned)bufferSize,
            pipelineReaderCore
        );
    } else {
        Serial.printf(
            "Sync API interrupted: %s | sent=%llu/%llu | offset=%llu | reason=%s\n",
            path.c_str(),
            (unsigned long long)sentBytes,
            (unsigned long long)sendLength,
            (unsigned long long)rangeStart,
            recordingPreempted ? "recording_priority" : "connection_or_read_error"
        );

        // Force the synchronous request to end now. A Range-capable client keeps
        // its .part file and resumes from the bytes already persisted locally.
        client.stop();
    }
}

static void sendExclusiveStatus()
{
    serviceExclusiveLease();

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"exclusive_active\":" +
        String(syncExclusiveActiveState ? "true" : "false") +
        ",\"lease_seconds\":" + String(SYNC_EXCLUSIVE_LEASE_MS / 1000UL) +
        ",\"remaining_seconds\":" + String(exclusiveRemainingSeconds());

#if defined(STORAGE_SPI)
    json +=
        ",\"sd_spi_normal_mhz\":" +
        String((unsigned long)(SD_SPI_NORMAL_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_max_mhz\":" +
        String((unsigned long)(SD_SPI_MAX_FREQUENCY_HZ / 1000000UL)) +
        ",\"sd_spi_active_mhz\":" +
        String((unsigned long)(
            (syncExclusiveActiveState
                ? SD_SPI_MAX_FREQUENCY_HZ
                : SD_SPI_NORMAL_FREQUENCY_HZ) / 1000000UL
        ));
#endif

    json += "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleExclusiveGet()
{
    if (!authenticate())
        return;

    touchExclusiveLease();
    sendExclusiveStatus();
}

static void handleExclusivePost()
{
    if (!authenticate())
        return;

    serviceExclusiveLease();

    String enableText = server().arg("enable");
    if (enableText != "0" && enableText != "1") {
        sendJsonError(400, "invalid_enable", "enable_must_be_0_or_1");
        return;
    }

    if (enableText == "0") {
        if (!syncExclusiveActiveState) {
            sendExclusiveStatus();
            return;
        }

#if defined(STORAGE_SPI)
        if (g_storageLocked) {
            sendBusy("storage_locked");
            return;
        }

        if (recording || recorderIsOpen()) {
            sendBusy("recording_active");
            return;
        }

#if SD_SPI_NORMAL_FREQUENCY_HZ != SD_SPI_MAX_FREQUENCY_HZ
        SpiClockSwitchResult result = switchSpiStorageClock(
            SD_SPI_NORMAL_FREQUENCY_HZ,
            SD_SPI_NORMAL_FREQUENCY_HZ,
            false,
            "exclusive_disabled"
        );

        if (!result.targetMounted) {
            sendJsonError(
                500,
                "sd_normal_remount_failed",
                "cannot_restore_normal_sd_clock_reboot_required"
            );
            return;
        }
#endif
#endif

        syncExclusiveActiveState = false;
        syncExclusiveLastActivityMs = 0;

#if defined(STORAGE_SPI)
        Serial.printf(
            "Sync API exclusive disabled | SD SPI=%lu MHz\n",
            (unsigned long)(SD_SPI_NORMAL_FREQUENCY_HZ / 1000000UL)
        );
#else
        Serial.println("Sync API exclusive disabled");
#endif

        sendExclusiveStatus();
        return;
    }

    // Never cut an existing recording in half. The client can retry after the
    // active recording has completed. Storage maintenance keeps priority too.
    if (recording || recorderIsOpen()) {
        sendBusy("recording_active");
        return;
    }

    if (g_storageLocked) {
        sendBusy("storage_locked");
        return;
    }

#if defined(STORAGE_SPI)
#if SD_SPI_NORMAL_FREQUENCY_HZ != SD_SPI_MAX_FREQUENCY_HZ
    SpiClockSwitchResult result = switchSpiStorageClock(
        SD_SPI_MAX_FREQUENCY_HZ,
        SD_SPI_NORMAL_FREQUENCY_HZ,
        true,
        "exclusive_enabled"
    );

    if (!result.targetMounted) {
        sendJsonError(
            500,
            "sd_highspeed_remount_failed",
            result.rebootRequired
                ? "cannot_mount_highspeed_or_restore_normal_reboot_required"
                : "cannot_mount_highspeed_sd_clock_normal_clock_restored"
        );
        return;
    }
#endif
#endif

    syncExclusiveActiveState = true;
    syncExclusiveLastActivityMs = millis();

#if defined(STORAGE_SPI)
    Serial.printf(
        "Sync API exclusive enabled | lease=60 s | SD SPI=%lu MHz\n",
        (unsigned long)(SD_SPI_MAX_FREQUENCY_HZ / 1000000UL)
    );
#else
    Serial.println("Sync API exclusive enabled | lease=60 s");
#endif

    sendExclusiveStatus();
}

static bool parseTestByteCount(uint64_t &byteCount)
{
    byteCount = SYNC_TEST_DEFAULT_BYTES;

    if (!server().hasArg("bytes"))
        return true;

    uint64_t requested = 0;
    if (!parseUnsigned64(server().arg("bytes"), requested) || requested == 0)
        return false;

    if (requested > SYNC_TEST_MAX_BYTES)
        requested = SYNC_TEST_MAX_BYTES;

    byteCount = requested;
    return true;
}

#if defined(STORAGE_SPI)
static bool spiTestFrequencyAllowed(uint32_t mhz)
{
    for (size_t i = 0; i < sizeof(SYNC_SPI_TEST_ALLOWED_MHZ) / sizeof(SYNC_SPI_TEST_ALLOWED_MHZ[0]); ++i) {
        if (SYNC_SPI_TEST_ALLOWED_MHZ[i] == mhz)
            return true;
    }
    return false;
}

static bool remountSpiStorageAtHz(uint32_t frequencyHz)
{
    STORAGE.end();
    SPI.end();
    delay(30);

    SPI.begin(
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    if (!SD.begin(
            SD_CS_PIN,
            SPI,
            frequencyHz,
            "/sd",
            5,
            false
        )) {
        return false;
    }

    if (STORAGE.cardType() == CARD_NONE) {
        STORAGE.end();
        return false;
    }

    return true;
}
#endif

static void handleTestSdSpi()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

#if !defined(STORAGE_SPI)
    sendJsonError(501, "unsupported", "sd_spi_frequency_test_requires_spi_storage");
    return;
#else
    // A remount invalidates every open SD File handle. Require an explicit
    // exclusive lease and additionally take the existing storage/recording
    // gates while the card is being remounted.
    if (!syncExclusiveActiveState) {
        sendJsonError(409, "exclusive_required", "enable_api_exclusive_before_sd_spi_test");
        return;
    }

    if (g_storageLocked) {
        sendBusy("storage_locked");
        return;
    }

    if (recording || recorderIsOpen()) {
        sendBusy("recording_active");
        return;
    }

    String mhzText = server().arg("mhz");
    uint64_t parsedMhz = 0;
    if (!parseUnsigned64(mhzText, parsedMhz) || parsedMhz > UINT32_MAX ||
        !spiTestFrequencyAllowed((uint32_t)parsedMhz)) {
        sendJsonError(400, "invalid_mhz", "allowed_mhz_are_4_8_12_16_20_24_32_40");
        return;
    }

    String path = server().arg("path");
    if (!validRecordingPath(path)) {
        sendJsonError(400, "invalid_path", "path_must_reference_a_finalized_recording");
        return;
    }

    uint64_t requestedBytes = 0;
    if (!parseTestByteCount(requestedBytes)) {
        sendJsonError(400, "invalid_bytes", "bytes_must_be_a_positive_integer");
        return;
    }

    // Verify the source file before touching the mount.
    File verifyFile = STORAGE.open(path.c_str(), FILE_READ);
    if (!verifyFile || verifyFile.isDirectory()) {
        if (verifyFile)
            verifyFile.close();
        sendJsonError(404, "file_not_found", "recording_file_not_found");
        return;
    }
    uint64_t sourceFileSize = (uint64_t)verifyFile.size();
    verifyFile.close();

    if (sourceFileSize == 0) {
        sendJsonError(409, "empty_file", "recording_file_is_empty");
        return;
    }

    const uint32_t testMhz = (uint32_t)parsedMhz;
    const uint32_t testHz = testMhz * 1000000UL;
    // The SPI diagnostic runs only under API-exclusive mode. After each
    // temporary test clock, return to the board-approved production high-speed
    // clock. Releasing/expiring Exclusive later restores NORMAL.
    const uint32_t restoreHz = SD_SPI_MAX_FREQUENCY_HZ;
    const uint32_t restoreMhz = restoreHz / 1000000UL;

    bool previousStorageLock = g_storageLocked;
    bool previousRecordingBlock = g_recordingStartBlocked;

    g_storageLocked = true;
    g_recordingStartBlocked = true;
    webPlayerStop();

    // The normal logger deliberately keeps an SD File open. Close it before
    // unmounting, exactly like the existing SD maintenance/remount path.
    logClose();

    bool testMountOk = remountSpiStorageAtHz(testHz);
    uint64_t readBytes = 0;
    uint32_t elapsedMs = 0;
    size_t bufferSize = 0;
    bool readOk = false;

    if (testMountOk) {
        File file = STORAGE.open(path.c_str(), FILE_READ);
        if (file && !file.isDirectory()) {
            uint64_t mountedFileSize = (uint64_t)file.size();
            uint64_t targetBytes = requestedBytes < mountedFileSize
                ? requestedBytes
                : mountedFileSize;

            uint8_t *buffer = allocateTransferBuffer(bufferSize);
            if (buffer && targetBytes > 0) {
                uint32_t startedMs = millis();

                while (readBytes < targetBytes) {
                    size_t wanted = (targetBytes - readBytes) < (uint64_t)bufferSize
                        ? (size_t)(targetBytes - readBytes)
                        : bufferSize;

                    size_t got = file.read(buffer, wanted);
                    if (got == 0)
                        break;

                    readBytes += (uint64_t)got;
                    touchExclusiveLease();
                    serviceLongOperation();
                }

                elapsedMs = millis() - startedMs;
                readOk = (readBytes == targetBytes);
                free(buffer);
            }
            file.close();
        }
    }

    // Always return to the firmware's configured/original SPI clock. The test
    // is intentionally non-persistent and must not silently alter field state.
    bool restoreOk = remountSpiStorageAtHz(restoreHz);

    if (restoreOk) {
        logInit();
        g_recordingStartBlocked = previousRecordingBlock;
        g_storageLocked = previousStorageLock;
    } else {
        // Fail safe: without a valid remount, keep storage and recording gated.
        // A reboot will execute the normal SD initialization path again.
        g_recordingStartBlocked = true;
        g_storageLocked = true;
    }

    float mbPerSecond = 0.0f;
    if (readBytes > 0 && elapsedMs > 0) {
        mbPerSecond =
            ((float)readBytes / (1024.0f * 1024.0f)) /
            ((float)elapsedMs / 1000.0f);
    }

    Serial.printf(
        "Sync API test_sd_spi | client=%s | host=%s | test=%lu MHz | restore=%lu MHz | mount=%s | restore_ok=%s | bytes=%llu | %lu ms | %.3f MB/s\n",
        syncClientVersionText().c_str(),
        syncApiVersionText().c_str(),
        (unsigned long)testMhz,
        (unsigned long)restoreMhz,
        testMountOk ? "ok" : "failed",
        restoreOk ? "yes" : "no",
        (unsigned long long)readBytes,
        (unsigned long)elapsedMs,
        mbPerSecond
    );

    String json =
        "{\"ok\":" + String((testMountOk && readOk && restoreOk) ? "true" : "false") +
        ",\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"test\":\"sd_spi\",\"path\":\"" + jsonEscape(path) +
        "\",\"requested_mhz\":" + String(testMhz) +
        ",\"restore_mhz\":" + String(restoreMhz) +
        ",\"mount_ok\":" + String(testMountOk ? "true" : "false") +
        ",\"read_ok\":" + String(readOk ? "true" : "false") +
        ",\"restore_ok\":" + String(restoreOk ? "true" : "false") +
        ",\"reboot_required\":" + String(restoreOk ? "false" : "true") +
        ",\"bytes_read\":" + uint64Text(readBytes) +
        ",\"elapsed_ms\":" + String(elapsedMs) +
        ",\"buffer_bytes\":" + String((unsigned)bufferSize) +
        ",\"mb_per_s\":" + String(mbPerSecond, 3) + "}";

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    // A benchmark failure is still a valid test result. Return JSON so clients
    // can continue the sweep and can see whether a reboot is required.
    server().send(
        200,
        "application/json; charset=utf-8",
        json
    );
#endif
}

static void handleTestSdRead()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (!storageReadReady())
        return;

    String path = server().arg("path");
    if (!validRecordingPath(path)) {
        sendJsonError(400, "invalid_path", "path_must_reference_a_finalized_recording");
        return;
    }

    uint64_t requestedBytes = 0;
    if (!parseTestByteCount(requestedBytes)) {
        sendJsonError(400, "invalid_bytes", "bytes_must_be_a_positive_integer");
        return;
    }

    File file = STORAGE.open(path.c_str(), FILE_READ);
    if (!file || file.isDirectory()) {
        if (file)
            file.close();
        sendJsonError(404, "file_not_found", "recording_file_not_found");
        return;
    }

    uint64_t fileSize = (uint64_t)file.size();
    uint64_t targetBytes = requestedBytes < fileSize ? requestedBytes : fileSize;

    if (targetBytes == 0) {
        file.close();
        sendJsonError(409, "empty_file", "recording_file_is_empty");
        return;
    }

    size_t bufferSize = 0;
    uint8_t *buffer = allocateTransferBuffer(bufferSize);
    if (!buffer) {
        file.close();
        sendJsonError(503, "buffer_unavailable", "cannot_allocate_test_buffer");
        return;
    }

    uint64_t readBytes = 0;
    uint32_t startedMs = millis();
    bool recordingPreempted = false;

    while (readBytes < targetBytes) {
        if (!syncExclusiveActiveState && motionNeedsRecording()) {
            recordingPreempted = true;
            break;
        }

        size_t wanted = (targetBytes - readBytes) < (uint64_t)bufferSize
            ? (size_t)(targetBytes - readBytes)
            : bufferSize;

        size_t got = file.read(buffer, wanted);
        if (got == 0)
            break;

        readBytes += (uint64_t)got;
        touchExclusiveLease();
        serviceLongOperation();
    }

    uint32_t elapsedMs = millis() - startedMs;

    file.close();
    free(buffer);

    if (recordingPreempted) {
        sendBusy("recording_pending");
        return;
    }

    if (readBytes != targetBytes) {
        sendJsonError(500, "sd_read_incomplete", "sd_read_test_did_not_reach_requested_length");
        return;
    }

    float mbPerSecond = 0.0f;
    if (elapsedMs > 0) {
        mbPerSecond =
            ((float)readBytes / (1024.0f * 1024.0f)) /
            ((float)elapsedMs / 1000.0f);
    }

    String json =
        "{\"ok\":true,\"api_version\":\"" + jsonEscape(syncApiVersionText()) +
        "\",\"test\":\"sd_read\",\"path\":\"" + jsonEscape(path) +
        "\",\"file_size\":" + uint64Text(fileSize) +
        ",\"bytes_read\":" + uint64Text(readBytes) +
        ",\"elapsed_ms\":" + String(elapsedMs) +
        ",\"buffer_bytes\":" + String((unsigned)bufferSize) +
        ",\"mb_per_s\":" + String(mbPerSecond, 3) + "}";

    Serial.printf(
        "Sync API test_sd_read | client=%s | host=%s | %s | bytes=%llu | %lu ms | %.3f MB/s\n",
        syncClientVersionText().c_str(),
        syncApiVersionText().c_str(),
        path.c_str(),
        (unsigned long long)readBytes,
        (unsigned long)elapsedMs,
        mbPerSecond
    );

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().send(200, "application/json; charset=utf-8", json);
}

static void handleTestWifi()
{
    if (!authenticate())
        return;

    touchExclusiveLease();

    if (recorderIsOpen()) {
        sendBusy("recording_active");
        return;
    }

    if (!syncExclusiveActiveState && motionNeedsRecording()) {
        sendBusy("recording_pending");
        return;
    }

    uint64_t testBytes = 0;
    if (!parseTestByteCount(testBytes)) {
        sendJsonError(400, "invalid_bytes", "bytes_must_be_a_positive_integer");
        return;
    }

    size_t bufferSize = 0;
    uint8_t *buffer = allocateTransferBuffer(bufferSize);
    if (!buffer) {
        sendJsonError(503, "buffer_unavailable", "cannot_allocate_test_buffer");
        return;
    }

    for (size_t i = 0; i < bufferSize; ++i)
        buffer[i] = (uint8_t)(i & 0xFFU);

    sendApiVersionHeader();
    server().sendHeader("Cache-Control", "no-store");
    server().sendHeader("X-SensorForge-Test", "wifi");
    server().sendHeader("X-SensorForge-Test-Bytes", uint64Text(testBytes));
    server().setContentLength((size_t)testBytes);
    server().send(200, "application/octet-stream", "");

    WiFiClient client = server().client();
    uint64_t remaining = testBytes;
    uint64_t sentBytes = 0;
    uint32_t startedMs = millis();
    bool recordingPreempted = false;
    bool ok = true;
    const bool allowRecordingPreemption = !syncExclusiveActiveState;

    while (remaining > 0 && client.connected()) {
        if (allowRecordingPreemption && motionNeedsRecording()) {
            recordingPreempted = true;
            ok = false;
            break;
        }

        size_t wanted = remaining < (uint64_t)bufferSize
            ? (size_t)remaining
            : bufferSize;

        if (!clientWriteAll(
                client,
                buffer,
                wanted,
                allowRecordingPreemption,
                recordingPreempted
            )) {
            ok = false;
            break;
        }

        remaining -= (uint64_t)wanted;
        sentBytes += (uint64_t)wanted;
        touchExclusiveLease();
        serviceLongOperation();
    }

    uint32_t elapsedMs = millis() - startedMs;
    free(buffer);

    float mbPerSecond = 0.0f;
    if (elapsedMs > 0) {
        mbPerSecond =
            ((float)sentBytes / (1024.0f * 1024.0f)) /
            ((float)elapsedMs / 1000.0f);
    }

    if (ok && remaining == 0) {
        Serial.printf(
            "Sync API test_wifi | client=%s | host=%s | bytes=%llu | %lu ms | %.3f MB/s\n",
            syncClientVersionText().c_str(),
            syncApiVersionText().c_str(),
            (unsigned long long)sentBytes,
            (unsigned long)elapsedMs,
            mbPerSecond
        );
    } else {
        Serial.printf(
            "Sync API test_wifi interrupted | client=%s | host=%s | sent=%llu/%llu | reason=%s\n",
            syncClientVersionText().c_str(),
            syncApiVersionText().c_str(),
            (unsigned long long)sentBytes,
            (unsigned long long)testBytes,
            recordingPreempted ? "recording_priority" : "connection_error"
        );
        client.stop();
    }
}

} // namespace


bool syncApiExclusiveActive()
{
    serviceExclusiveLease();
    return syncExclusiveActiveState;
}


void syncApiRegisterRoutes(WebServer &webServer)
{
    syncServer = &webServer;

    // WebServer retains only explicitly collected custom request headers.
    // Range is needed for standard HTTP resume support.
    const char *headerKeys[] = {
        "Range",
        "X-SensorForge-Client-Version"
    };
    webServer.collectHeaders(headerKeys, sizeof(headerKeys) / sizeof(headerKeys[0]));

    // Stable Integration API 1.0 profile. Existing Sync API endpoints remain
    // unchanged and continue to coexist with these additive automation routes.
    webServer.on("/api/v1/device", HTTP_GET, handleDevice);
    webServer.on("/api/v1/state", HTTP_GET, handleIntegrationState);
    webServer.on("/api/v1/shooter", HTTP_GET, handleShooterStatus);
    webServer.on("/api/v1/shooter/flush", HTTP_POST, handleShooterFlush);

    webServer.on("/api/v1/status", HTTP_GET, handleStatus);
    webServer.on("/api/v1/storage", HTTP_GET, handleStorageStatus);
    webServer.on("/api/v1/sensors", HTTP_GET, handleSensorsStatus);
    webServer.on("/api/v1/camera/snapshot", HTTP_GET, handleCameraSnapshot);
    webServer.on("/api/v1/days", HTTP_GET, handleDays);
    webServer.on("/api/v1/files", HTTP_GET, handleFiles);
    webServer.on("/api/v1/file", HTTP_GET, handleFile);

    webServer.on("/api/v1/exclusive", HTTP_GET, handleExclusiveGet);
    webServer.on("/api/v1/exclusive", HTTP_POST, handleExclusivePost);

    webServer.on("/api/v1/test_sd_read", HTTP_GET, handleTestSdRead);
    webServer.on("/api/v1/test_sd_spi", HTTP_GET, handleTestSdSpi);
    webServer.on("/api/v1/test_wifi", HTTP_GET, handleTestWifi);
}

