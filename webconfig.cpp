#include "webconfig.h"
#include "web_log_reader.h"
#include "web_sd_maintenance.h"
#include "config.h"
#include "audio_capture.h"
#include "audio_wav.h"
#include "language.h"
#include "board_config.h"

#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_err.h>
#include <vector>
#include <algorithm>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "logger.h"
#include "webplayer.h"
#include "recorder.h"
#include "storage_guard.h"
#include "radar.h"
#include "branding.h"
#include "sensorforge_version.h"
#include "rtc.h"
#include "thermal.h"
#include "sync_api.h"
#include "license.h"
#include "recording_crypto.h"
#include "recording_storage.h"
#include "image_motion.h"
#include "motion_diagnostics.h"


// High-level recording state from the main firmware loop.
// This is the same state used by the periodic STATUS line.
extern bool recording;
extern bool sdReady;

// Gracefully finalizes an active recording when the operator explicitly
// pauses the recording automation from WebConfig.
extern void stopRecording();

// Camera-backed image-motion test supplied by the main firmware. Test mode
// analyzes frames only and never starts a recording.
extern bool imageMotionWebTest(String &json, String &error);

// OV3660 crop runtime control implemented by the main camera module.
extern bool cameraApplyCropRuntime(
    const String &zoom,
    int positionX,
    int positionY,
    String &error
);

// Uses the exact grayscale transport-check camera path from the main firmware.
extern bool cameraMeasureTransportBlackReference(
    float &referenceMean,
    uint8_t &referenceP95,
    uint8_t sampleCount,
    String &error
);

// Firmware compatibility information supplied by the main firmware.
// The browser update streams directly into the inactive internal OTA partition
// and uses the same board-specific marker as the SD auto-updater.
extern const char *firmwareExpectedCompatibilityMarker();

// Recording-event safety cooldown state supplied by the main firmware loop.
extern bool recordingSafetyCooldownActive();
extern uint32_t recordingSafetyCooldownRemainingSeconds();




static WebServer server(80);
static bool webActive = false;

// HTTP handlers persist in the WebServer object after server.stop().
// Register them only once so repeated magnet WiFi toggles do not
// append duplicate handlers and slowly consume heap.
static bool webRoutesRegistered = false;

// Last real HTTP/browser activity. All requests pass through the
// WebServer middleware below. Open WebConfig pages also send a
// lightweight heartbeat every 10 seconds, including background tabs,
// so an open browser session is not mistaken for inactivity.
static uint32_t webLastActivityMs = 0;

// Radar configuration is a synchronous UART maintenance operation. Keep WiFi
// alive explicitly while it runs and for a short grace period afterwards so a
// redirect/browser reconnect cannot race the normal WebConfig inactivity timer.
static const uint32_t RADAR_WEB_MAINTENANCE_GRACE_MS = 120000UL;
static const uint32_t RADAR_CALIBRATION_MAX_IDLE_HOLD_MS = 10UL * 60UL * 1000UL;
static uint8_t radarWebMaintenanceDepth = 0;
static uint32_t radarWebMaintenanceHoldUntilMs = 0;

// Reboot is scheduled instead of executed directly inside the HTTP handler.
// This gives the browser enough time to follow the POST/Redirect/GET flow and
// render the reboot notice before the network connection disappears.
static bool rebootScheduled = false;
static uint32_t rebootAtMs = 0;

// Manual software shutdown. Unlike the normal power-management sleep path,
// this deliberately enables NO wake source. The ESP32-S3 therefore remains in
// deep sleep until external reset or power is removed and applied again.
static bool shutdownScheduled = false;
static uint32_t shutdownAtMs = 0;

static void noteWebActivity()
{
    webLastActivityMs =
        millis();
}

static void radarWebMaintenanceBegin()
{
    if (radarWebMaintenanceDepth < 0xFFU)
        radarWebMaintenanceDepth++;

    noteWebActivity();
}

static void radarWebMaintenanceEnd()
{
    if (radarWebMaintenanceDepth > 0)
        radarWebMaintenanceDepth--;

    noteWebActivity();

    if (radarWebMaintenanceDepth == 0) {
        radarWebMaintenanceHoldUntilMs =
            millis() + RADAR_WEB_MAINTENANCE_GRACE_MS;
    }
}

static bool radarWebMaintenanceGraceActive()
{
    if (radarWebMaintenanceHoldUntilMs == 0)
        return false;

    if (
        (int32_t)(
            radarWebMaintenanceHoldUntilMs - millis()
        ) > 0
    ) {
        return true;
    }

    radarWebMaintenanceHoldUntilMs = 0;
    return false;
}

class RadarWebMaintenanceGuard {
public:
    RadarWebMaintenanceGuard()
    {
        radarWebMaintenanceBegin();
    }

    ~RadarWebMaintenanceGuard()
    {
        radarWebMaintenanceEnd();
    }

    RadarWebMaintenanceGuard(const RadarWebMaintenanceGuard &) = delete;
    RadarWebMaintenanceGuard &operator=(const RadarWebMaintenanceGuard &) = delete;
};

static void serviceWebLongOperation()
{
    // WebServer handlers are synchronous in loopTask.
    // Long SD work must feed the task watchdog itself.
    esp_task_wdt_reset();
    yield();
}

// Random ID that stays constant for one board boot.
// The recordings page uses it to distinguish browser navigation
// within the same boot from an actual ESP32 reboot/reset.
static uint32_t webBootSessionId = 0;

// PIR simulation state.
// The duration is intentionally RAM-only and is not written to config.txt.
static uint32_t simulatedMotionUntilMs = 0;
static uint32_t simulationDurationSeconds = 5;

// Temporary operator-controlled recording pause. This is deliberately RAM-only:
// it must never survive a reboot and is never written to config.txt. While paused,
// the physical sensors may still be read for diagnostics, but the main firmware
// suppresses NEW motion-recording starts and the continuous shooter.
//
// The pause is a short lease rather than a sticky switch. Every visible WebConfig
// page renews (or, when auto-pause is enabled, re-establishes) the lease.
// Closing/hiding the UI therefore re-enables capture automation automatically
// after a short grace period, so the system cannot be forgotten in maintenance
// mode.
static bool recordingAutomationPaused = false;
static uint32_t recordingPauseLeaseMs = 0;
static bool recordingPauseTransportHold = false;
static const uint32_t RECORDING_PAUSE_LEASE_TIMEOUT_MS = 35000UL;
// Transport preparation is allowed a longer fallback lease because browsers may
// heavily throttle timers in a background tab. Explicit pagehide/navigation
// drops back to the normal 35 s maintenance timeout immediately.
static const uint32_t RECORDING_PAUSE_TRANSPORT_TIMEOUT_MS = 120000UL;

// Web UI session tracking for the optional automatic recording pause.
// The common /activity heartbeat keeps this timestamp fresh while a normal
// SensorForge browser page is open. A manual resume suppresses automatic
// re-pausing until that UI session has been inactive for the normal pause
// lease timeout. API traffic does not touch this state.
static uint32_t recordingUiSessionLastSeenMs = 0;
static bool recordingAutoPauseSuppressedForUiSession = false;

static void setRecordingAutomationPaused(
    bool paused,
    const char *reason
)
{
    if (recordingAutomationPaused == paused) {
        if (paused)
            recordingPauseLeaseMs = millis();
        return;
    }

    recordingAutomationPaused = paused;

    if (paused) {
        recordingPauseLeaseMs = millis();
        simulatedMotionUntilMs = 0;

        consoleWrite(
            "REC",
            "Automation paused | WebConfig"
        );

        logWrite(
            "Capture automation paused from WebConfig"
        );

        // Finish the current file cleanly. Once the flag above is set, the
        // next main-loop iteration cannot immediately start a replacement.
        if (recording) {
            stopRecording();
        } else if (recorderIsOpen()) {
            // Defensive recovery for a stale recorder handle whose high-level
            // recording flag was already cleared. Maintenance operations must
            // never remain blocked by such an orphaned open recorder.
            recorderEnd();
        }

    } else {
        recordingPauseLeaseMs = 0;
        recordingPauseTransportHold = false;
        simulatedMotionUntilMs = 0;

        String message =
            "Automation resumed";

        if (reason && reason[0]) {
            message +=
                " | " +
                String(reason);
        }

        consoleWrite(
            "REC",
            message
        );

        logWrite(
            "Recording " +
            message
        );
    }
}


static void noteRecordingUiSessionActivity()
{
    recordingUiSessionLastSeenMs = millis();
}


static void maybeAutoPauseRecordingForWebUi()
{
    uint32_t now = millis();

    bool newUiSession =
        recordingUiSessionLastSeenMs == 0 ||
        (uint32_t)(now - recordingUiSessionLastSeenMs) >=
            RECORDING_PAUSE_LEASE_TIMEOUT_MS;

    if (newUiSession)
        recordingAutoPauseSuppressedForUiSession = false;

    recordingUiSessionLastSeenMs = now;

    if (recordingAutomationPaused) {
        // Any visible WebConfig/player heartbeat renews an existing manual or
        // automatic pause. This also recovers cleanly after a long synchronous
        // HTTP operation that consumed most of the previous lease.
        recordingPauseLeaseMs = now;
        return;
    }

    if (
        cfg_web_recording_auto_pause &&
        !recordingAutoPauseSuppressedForUiSession
    ) {
        // Important: this path is also reached from /activity and from the
        // standalone player keepalive. Therefore an expired lease is
        // re-established while the browser session is visibly active instead
        // of leaving recording/shooter automation running behind the UI.
        setRecordingAutomationPaused(
            true,
            "automatic WebConfig activity"
        );
    }
}


bool webConfigRecordingPaused()
{
    return
        recordingAutomationPaused;
}


static void renewRecordingPauseLease(
    bool transportHold = false
)
{
    if (!recordingAutomationPaused)
        return;

    recordingPauseLeaseMs = millis();

    if (transportHold)
        recordingPauseTransportHold = true;
}


static void releaseRecordingPauseIfLeaseExpired()
{
    if (!recordingAutomationPaused)
        return;

    if (!webActive) {
        setRecordingAutomationPaused(
            false,
            "WebConfig stopped"
        );
        return;
    }

    uint32_t timeoutMs =
        recordingPauseTransportHold
        ? RECORDING_PAUSE_TRANSPORT_TIMEOUT_MS
        : RECORDING_PAUSE_LEASE_TIMEOUT_MS;

    if (
        recordingPauseLeaseMs != 0 &&
        (uint32_t)(
            millis() -
            recordingPauseLeaseMs
        ) >= timeoutMs
    ) {
        setRecordingAutomationPaused(
            false,
            "WebConfig inactive"
        );
    }
}

// Live camera preview gate. Snapshot requests arrive every ~200 ms while the
// preview is visible. The short timeout is a safety net for closed tabs, lost
// WiFi, browser crashes or navigation where the explicit stop request is lost.
static bool cameraPreviewActive = false;
static uint32_t cameraPreviewLastActivityMs = 0;
static const uint32_t CAMERA_PREVIEW_TIMEOUT_MS = 2500UL;

// Image-Motion diagnostics are deliberately slower than the camera preview.
// Decoding/analyzing every preview JPEG would serialize the HTTP server behind
// the relatively expensive JPEG motion analysis and make the live picture feel
// much more sluggish than the normal camera preview. Keep the picture at its
// normal cadence and update the diagnostic state at ~2 Hz instead.
static uint32_t imageMotionPreviewLastAnalysisMs = 0;
static const uint32_t IMAGE_MOTION_PREVIEW_ANALYSIS_INTERVAL_MS = 500UL;

// Live Preview may temporarily test a crop before SAVE. That temporary sensor
// state must never leak into later recordings. Leaving/losing the preview
// restores the persisted cfg_camera_crop_* values automatically.
static bool cameraPreviewCropTemporary = false;


static bool restoreSavedCameraCrop()
{
    if (!cameraPreviewCropTemporary)
        return true;

    if (recorderIsOpen())
        return false;

    String error;

    if (!cameraApplyCropRuntime(
            cfg_camera_crop_zoom,
            cfg_camera_crop_x,
            cfg_camera_crop_y,
            error
        )) {

        if (cfg_debug_enabled) {
            Serial.println(
                "WebConfig: camera crop restore failed | " +
                error
            );
        }

        // Fail safe: keep the preview gate active. An unsaved crop must never
        // leak into a later automatic recording just because a restore failed.
        return false;
    }

    cameraPreviewCropTemporary = false;
    return true;
}


bool webConfigCameraPreviewActive()
{
    if (!cameraPreviewActive)
        return false;

    if (
        (uint32_t)(
            millis() -
            cameraPreviewLastActivityMs
        ) > CAMERA_PREVIEW_TIMEOUT_MS
    ) {
        if (!restoreSavedCameraCrop()) {
            // Retry later and, more importantly, keep NEW recordings blocked
            // until the persisted framing has actually been restored.
            cameraPreviewLastActivityMs = millis();
            return true;
        }

        cameraPreviewActive = false;
        cameraPreviewLastActivityMs = 0;
        return false;
    }

    return true;
}


static void noteCameraPreviewActivity()
{
    cameraPreviewActive = true;
    cameraPreviewLastActivityMs = millis();
}


static void stopCameraPreview()
{
    if (!restoreSavedCameraCrop()) {
        // Fail safe: retain the gate if the sensor could not be restored to
        // the persisted crop. A reboot/reinit can recover, but a wrong crop
        // must not silently reach an automatic recording.
        cameraPreviewActive = true;
        cameraPreviewLastActivityMs = millis();
        return;
    }

    cameraPreviewActive = false;
    cameraPreviewLastActivityMs = 0;
}


bool webConfigMotionActive()
{
    if (simulatedMotionUntilMs == 0)
        return false;

    int32_t remaining =
        (int32_t)(simulatedMotionUntilMs - millis());

    if (remaining <= 0) {
        simulatedMotionUntilMs = 0;
        return false;
    }

    return true;
}


uint32_t webConfigMotionRemainingMs()
{
    if (!webConfigMotionActive())
        return 0;

    return
        (uint32_t)(
            simulatedMotionUntilMs - millis()
        );
}


// -------------------------------------------------------------
// HTML / URL Helpers
// -------------------------------------------------------------

static String htmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];

        switch (c) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;            break;
        }
    }

    return out;
}


static String htmlJsString(const String &value)
{
    String out;
    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];

        switch (c) {
            case '\\': out += F("\\\\"); break;
            case '\'': out += F("\\'"); break;
            case '\r': out += F("\\r"); break;
            case '\n': out += F("\\n"); break;
            case '<': out += F("\\x3C"); break;
            default: out += c; break;
        }
    }

    return out;
}


static String htmlText(
    UiTextId id
)
{
    return
        htmlEscape(
            String(tr(id))
        );
}


static bool radarConfigAvailable()
{
    // This is the result of the robust four-attempt LD2410S startup check.
    // It deliberately does not depend on current report freshness.
    return radarSensorDetected();
}


static UiTextId motionSensorTypeUiId()
{
    if (radarConfigAvailable())
        return UI_MOTION_SENSOR_RADAR;

#if defined(BOARD_FREENOVE)
    return UI_MOTION_SENSOR_SR602_PIR;
#else
    return UI_MOTION_SENSOR_PIR;
#endif
}


static bool freenovePirCompatibilityNoteApplies()
{
#if defined(BOARD_FREENOVE)
    return !radarConfigAvailable();
#else
    return false;
#endif
}


static bool rejectRadarConfigurationUnavailable()
{
    if (radarConfigAvailable())
        return false;

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    String message =
        String(tr(UI_RADAR_CONFIG_UNAVAILABLE)) +
        "\n\n" +
        String(tr(UI_RADAR_CONFIG_PIR_NOTE));

    if (freenovePirCompatibilityNoteApplies()) {
        message +=
            "\n\n" +
            String(tr(UI_MOTION_PIR_FREENOVE_NOTE));
    }

    server.send(
        409,
        "text/plain; charset=utf-8",
        message
    );

    return true;
}


static const char *configSourceUiName()
{
    switch (configGetSource()) {
        case CONFIG_SOURCE_SD:
            return tr(UI_CONFIG_SOURCE_SD);
        case CONFIG_SOURCE_INTERNAL:
            return tr(UI_CONFIG_SOURCE_INTERNAL);
        case CONFIG_SOURCE_DEFAULTS:
        default:
            return tr(UI_CONFIG_SOURCE_DEFAULTS);
    }
}


static const char *configSdStatusUiName()
{
    if (!configSdAvailable())
        return tr(UI_SD_UNAVAILABLE);

    if (!configSdPresent())
        return tr(UI_SD_MISSING);

    if (configSdValid())
        return tr(UI_SD_VALID);

    return tr(UI_SD_INVALID);
}



static const char *thermalStateUiName(
    const char *state
)
{
    if (state && strcmp(state, "WARNING") == 0)
        return tr(UI_THERMAL_STATE_WARNING);

    if (state && strcmp(state, "EMERGENCY") == 0)
        return tr(UI_THERMAL_STATE_EMERGENCY);

    return tr(UI_THERMAL_STATE_OK);
}


static String urlEncode(const String &value)
{
    static const char hex[] = "0123456789ABCDEF";

    String out;
    out.reserve(value.length() * 2);

    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {

            out += (char)c;

        } else {

            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }

    return out;
}


static String moduleClockText()
{
    time_t now =
        time(nullptr);

    if (now < (time_t)1609459200)
        return String();

    struct tm localTime;

    if (!localtime_r(
            &now,
            &localTime
        )) {
        return String();
    }

    char buffer[32];

    if (!strftime(
            buffer,
            sizeof(buffer),
            "%d.%m.%Y %H:%M:%S",
            &localTime
        )) {
        return String();
    }

    return String(buffer);
}


static String recordingArmCountdownText(
    int64_t seconds
)
{
    if (seconds < 0)
        seconds = 0;

    uint64_t value =
        (uint64_t)seconds;

    uint64_t days =
        value / 86400ULL;

    value %= 86400ULL;

    uint32_t hours =
        (uint32_t)(value / 3600ULL);

    value %= 3600ULL;

    uint32_t minutes =
        (uint32_t)(value / 60ULL);

    uint32_t secs =
        (uint32_t)(value % 60ULL);

    char timePart[96];

    snprintf(
        timePart,
        sizeof(timePart),
        "%02lu %s %02lu %s %02lu %s",
        (unsigned long)hours,
        tr(UI_HOURS),
        (unsigned long)minutes,
        tr(UI_MINUTES),
        (unsigned long)secs,
        tr(UI_SECONDS_UNIT)
    );

    if (days == 0)
        return String(timePart);

    char dayPart[32];

    snprintf(
        dayPart,
        sizeof(dayPart),
        "%llu",
        (unsigned long long)days
    );

    String result =
        String(dayPart) +
        " " +
        String(
            days == 1
            ? tr(UI_DAY_SINGULAR)
            : tr(UI_DAY_PLURAL)
        ) +
        " " +
        String(timePart);

    return result;
}



static void recordingArmFormDefaults(
    bool &scheduled,
    int &year,
    int &month,
    int &day,
    int &hour,
    int &minute
)
{
    scheduled =
        cfg_recording_not_before != "off";

    year = 2026;
    month = 1;
    day = 1;
    hour = 0;
    minute = 0;

    time_t now =
        time(nullptr);

    if (now >= (time_t)1609459200) {
        // For a new schedule, default the menus to one hour from now.
        time_t suggested =
            now + 3600;

        struct tm localTime;

        if (localtime_r(
                &suggested,
                &localTime
            )) {
            year = localTime.tm_year + 1900;
            month = localTime.tm_mon + 1;
            day = localTime.tm_mday;
            hour = localTime.tm_hour;
            minute = localTime.tm_min;
        }
    }

    if (
        scheduled &&
        cfg_recording_not_before.length() == 19
    ) {
        year = cfg_recording_not_before.substring(0, 4).toInt();
        month = cfg_recording_not_before.substring(5, 7).toInt();
        day = cfg_recording_not_before.substring(8, 10).toInt();
        hour = cfg_recording_not_before.substring(11, 13).toInt();
        minute = cfg_recording_not_before.substring(14, 16).toInt();
    }
}


static String htmlHeader()
{
    maybeAutoPauseRecordingForWebUi();

    String html =
        String("<!doctype html><html lang='") +
        uiLanguageCode() +
        "'><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>" SENSORFORGE_APP_NAME_LITERAL " &middot; "
        SENSORFORGE_PLATFORM_LITERAL " &middot; v"
        SENSORFORGE_CORE_VERSION_LITERAL "</title>"
        "<style>"
        ":root{--bg:#eef1f4;--panel:#fff;--text:#1f2933;--muted:#667085;"
        "--line:#d8dee6;--nav:#1f2937;--nav2:#344054;--accent:#2563eb;"
        "--ok:#087a00;--warn:#9a5a00;--danger:#b42318;}"
        "*{box-sizing:border-box;}"
        "body{font-family:Arial,sans-serif;margin:0;background:var(--bg);color:var(--text);}"
        "h1,h2,h3{margin-top:0;color:#27313d;}"
        "h2{font-size:1.55rem;margin-bottom:10px;}"
        "h3{font-size:1.08rem;}"
        "a{color:var(--accent);}"
        ".topbar{position:sticky;top:0;z-index:1000;background:var(--nav);"
        "box-shadow:0 2px 8px rgba(0,0,0,.18);}"
        ".navwrap{max-width:1200px;margin:0 auto;display:flex;align-items:center;"
        "gap:8px;padding:0 14px;min-height:54px;}"
        ".brand{color:#fff;text-decoration:none;font-weight:700;"
        "padding:8px 10px 8px 0;margin-right:8px;white-space:nowrap;"
        "display:flex;flex-direction:column;line-height:1.05;}"
        ".brand-main{font-size:1.02rem;letter-spacing:.08em;text-transform:uppercase;}"
        ".brand-sub{font-size:.61rem;font-weight:600;color:#aeb8c5;"
        "letter-spacing:.08em;margin-top:4px;text-transform:uppercase;}"
        ".navlinks{display:flex;align-items:center;gap:2px;flex-wrap:wrap;}"
        ".module-meta{margin-left:auto;display:flex;flex-direction:column;align-items:flex-end;"
        "justify-content:center;gap:3px;padding:4px 0 4px 10px;}"
        ".module-actions{display:flex;align-items:center;justify-content:flex-end;gap:5px;}"
        ".language-switch{margin:0;padding:0;}"
        ".language-switch select{width:auto;min-width:58px;margin:0;padding:3px 6px;border:0;"
        "border-radius:999px;background:#fff;color:#1f2933;font-size:.72rem;font-weight:800;cursor:pointer;}"
        ".module-clock{color:#f8fafc;font-size:.88rem;font-weight:700;"
        "font-variant-numeric:tabular-nums;white-space:nowrap;text-align:right;letter-spacing:.02em;}"
        ".module-clock.invalid{color:#aeb8c5;font-weight:600;}"
        ".module-clock.thermal-warning{color:#fbbf24;}"
        ".module-clock.thermal-emergency{color:#fca5a5;}"
        ".module-recording-switch{display:inline-block;padding:4px 9px;margin:0;border:0;border-radius:999px;"
        "font-size:.68rem;font-weight:800;letter-spacing:.05em;white-space:nowrap;cursor:pointer;}"
        ".module-recording-switch.on{background:#dcfce7;color:#166534;}"
        ".module-recording-switch.off{background:#f59e0b;color:#241500;}"
        ".module-recording-switch:disabled{opacity:.65;cursor:wait;}"
        ".navitem,.navdrop>summary{display:block;color:#e5e7eb;text-decoration:none;"
        "padding:10px 11px;border-radius:6px;cursor:pointer;user-select:none;"
        "font-size:.95rem;white-space:nowrap;}"
        ".navitem:hover,.navdrop>summary:hover,.navitem.active,.navdrop>summary.active{"
        "background:var(--nav2);color:#fff;}"
        ".navdrop{position:relative;}"
        ".navdrop>summary{list-style:none;}"
        ".navdrop>summary::-webkit-details-marker{display:none;}"
        ".navdrop>summary:after{content:'  ▾';font-size:.75em;}"
        ".dropdown{position:absolute;left:0;top:calc(100% + 2px);min-width:210px;"
        "background:#fff;border:1px solid var(--line);border-radius:8px;"
        "box-shadow:0 10px 28px rgba(0,0,0,.18);padding:6px;}"
        ".navdrop:not([open]) .dropdown{display:none;}"
        ".dropdown a{display:block;text-decoration:none;color:var(--text);"
        "padding:9px 10px;border-radius:6px;margin:0;}"
        ".dropdown a:hover,.dropdown a.active{background:#eef4ff;color:#174ea6;}"
        ".dropdown .nav-disabled{display:block;color:#98a2b3;padding:9px 10px;border-radius:6px;"
        "margin:0;cursor:not-allowed;background:#f8fafc;}"
        ".button.disabled{opacity:.5;cursor:not-allowed;pointer-events:none;}"
        ".dropdown .sep{height:1px;background:var(--line);margin:6px 4px;}"
        ".dropdown .danger-link{color:var(--danger);}"
        ".page{max-width:1200px;margin:20px auto;padding:0 14px 30px;}"
        "div.box{background:var(--panel);padding:22px;border-radius:10px;"
        "box-shadow:0 2px 12px rgba(16,24,40,.08);}"
        "input,select{padding:8px 10px;margin:4px 0 8px;width:280px;max-width:100%;"
        "border:1px solid #c7cfd8;border-radius:6px;background:#fff;color:var(--text);}"
        "input:focus,select:focus{outline:2px solid #b9d2ff;border-color:var(--accent);}"
        "button,.button{padding:9px 15px;margin:5px 5px 5px 0;border:0;border-radius:6px;"
        "background:#e9edf2;color:#1f2933;cursor:pointer;font-weight:600;text-decoration:none;"
        "display:inline-block;}"
        "button:hover,.button:hover{filter:brightness(.96);}"
        ".button.primary,button.primary{background:var(--accent);color:#fff;}"
        ".button.danger,button.danger{background:var(--danger);color:#fff;}"
        ".muted{color:var(--muted);}"
        ".page-title{display:flex;justify-content:space-between;gap:14px;align-items:flex-start;"
        "margin-bottom:18px;}"
        ".page-title p{margin:0;color:var(--muted);}"
        ".dashboard-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));"
        "gap:12px;margin:16px 0;}"
        ".dash-card{border:1px solid var(--line);border-radius:9px;padding:15px;background:#fafbfc;}"
        ".card-label{font-size:.82rem;color:var(--muted);text-transform:uppercase;"
        "letter-spacing:.04em;margin-bottom:6px;}"
        ".card-value{font-size:1.15rem;font-weight:700;word-break:break-word;}"
        ".card-note{font-size:.88rem;color:var(--muted);margin-top:5px;}"
        ".status-pill{display:inline-block;padding:5px 9px;border-radius:999px;"
        "font-size:.82rem;font-weight:700;background:#e8edf3;color:#344054;white-space:nowrap;}"
        ".status-pill.ok{background:#e7f6e5;color:var(--ok);}"
        ".status-pill.warn{background:#fff1d6;color:var(--warn);}"
        ".status-pill.danger{background:#fde7e5;color:var(--danger);}"
        ".progress{height:8px;background:#e5e9ef;border-radius:999px;overflow:hidden;margin-top:9px;}"
        ".progress>span{display:block;height:100%;background:var(--accent);}"
        ".quick-actions{display:flex;flex-wrap:wrap;gap:6px;margin:16px 0 6px;}"
        ".settings-section{border:1px solid var(--line);border-radius:9px;padding:16px;"
        "margin:14px 0;background:#fbfcfd;}"
        ".settings-section h3{margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid var(--line);}"
        ".recording-control{display:flex;align-items:center;justify-content:space-between;gap:18px;"
        "border:1px solid var(--line);border-left:5px solid var(--ok);border-radius:9px;"
        "padding:15px 16px;margin:16px 0;background:#fbfcfd;transition:.2s ease;}"
        ".recording-control.paused{border-color:#f0c36a;border-left-color:#d97706;background:#fff7e8;"
        "box-shadow:0 2px 10px rgba(154,90,0,.10);}"
        ".recording-control-copy{min-width:0;}"
        ".recording-control-title{font-weight:800;margin-bottom:5px;}"
        ".recording-control-note{font-size:.9rem;color:var(--muted);line-height:1.45;}"
        ".recording-control-actions{flex:0 0 auto;text-align:right;}"
        ".recording-control-actions button{min-width:220px;margin:0;}"
        ".recording-control.paused .recording-control-actions button{background:#d97706;color:#fff;}"
        ".form-actions{padding:14px 0 2px;}"
        ".form-actions button{background:var(--accent);color:#fff;min-width:120px;}"
        ".flash-notice{position:relative;margin:0 0 16px;padding:14px 16px 14px 18px;"
        "border:1px solid #b8d5bf;border-left:5px solid var(--ok);border-radius:9px;"
        "background:#eef9f0;box-shadow:0 2px 8px rgba(16,24,40,.06);"
        "transition:opacity .35s ease,transform .35s ease;}"
        ".flash-notice.hide{opacity:0;transform:translateY(-6px);}"
        ".flash-notice strong{display:block;margin-bottom:4px;color:#14532d;}"
        ".flash-notice .muted{font-size:.92rem;}"
        ".flash-notice.error{border-color:#efb7b2;border-left-color:var(--danger);background:#fff1f0;}"
        ".flash-notice.error strong{color:#8a1c14;}"
        ".modal-backdrop{position:fixed;inset:0;z-index:2000;display:flex;align-items:center;"
        "justify-content:center;padding:18px;background:rgba(15,23,32,.58);backdrop-filter:blur(2px);}"
        ".modal-backdrop[hidden]{display:none!important;}"
        ".modal-card{width:min(520px,100%);background:#fff;border:1px solid var(--line);border-radius:12px;"
        "box-shadow:0 20px 60px rgba(0,0,0,.28);padding:24px;}"
        ".modal-card h3{margin:12px 0 8px;font-size:1.25rem;}"
        ".modal-card p{margin:8px 0;color:var(--muted);line-height:1.5;}"
        ".modal-actions{display:flex;flex-wrap:wrap;gap:6px;margin-top:18px;}"
        ".modal-state{margin-top:12px;font-size:.88rem;color:var(--muted);}"
        ".radar-cal-modal-card{width:min(600px,100%);text-align:center;padding:28px;}"
        ".radar-cal-kicker{font-size:.78rem;font-weight:800;letter-spacing:.12em;text-transform:uppercase;color:var(--muted);}"
        ".radar-cal-modal-card h3{font-size:1.45rem;margin:8px 0 4px;}"
        ".radar-cal-big{font-size:4.4rem;line-height:1;font-weight:850;letter-spacing:-.04em;margin:24px 0 6px;color:var(--accent);}"
        ".radar-cal-big-label{font-size:1rem;font-weight:700;color:#263445;margin-bottom:18px;}"
        ".radar-cal-progress{height:12px;background:#e5e9ef;border-radius:999px;overflow:hidden;margin:18px 0 10px;}"
        ".radar-cal-progress>span{display:block;width:0;height:100%;background:var(--accent);transition:width .3s ease;}"
        ".radar-cal-hint{min-height:2.8em;margin:8px auto 18px;color:var(--muted);line-height:1.45;max-width:470px;}"
        ".radar-cal-meta{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin:18px 0;}"
        ".radar-cal-meta>div{border:1px solid var(--line);border-radius:9px;background:#f7f9fb;padding:12px 8px;}"
        ".radar-cal-meta b{display:block;font-size:1.15rem;margin-bottom:3px;color:#1f2937;}"
        ".radar-cal-meta span{display:block;font-size:.78rem;color:var(--muted);line-height:1.25;}"
        ".radar-cal-modal-card .modal-actions{justify-content:center;}"
        "@media(max-width:520px){.radar-cal-big{font-size:3.6rem}.radar-cal-meta{grid-template-columns:1fr}.radar-cal-modal-card{padding:22px 16px;}}"
        ".operation-card{max-width:620px;margin:26px auto;text-align:center;border:1px solid var(--line);"
        "border-radius:12px;padding:28px 22px;background:#fafbfc;}"
        ".operation-card .countdown{font-size:2.4rem;font-weight:700;margin:14px 0 4px;}"
        ".log-toolbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:14px 0 10px;}"
        ".log-filter-row{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:0 0 10px;}"
        ".log-search{width:min(420px,100%);margin:0;}"
        ".log-live-label{display:inline-flex;align-items:center;gap:6px;font-size:.88rem;color:var(--muted);}"
        ".log-live-label input{width:auto;margin:0;}"
        ".log-meta{display:flex;flex-wrap:wrap;gap:8px 16px;margin:0 0 12px;color:var(--muted);font-size:.9rem;}"
        ".log-terminal{height:min(68vh,720px);min-height:360px;overflow:auto;background:#0f1720;"
        "border:1px solid #263445;border-radius:10px;box-shadow:inset 0 1px 0 rgba(255,255,255,.03);}"
        ".log-view{margin:0;padding:16px 18px;color:#d6e2ef;background:transparent;font-family:Consolas,"
        "'Liberation Mono',Menlo,monospace;font-size:.82rem;line-height:1.55;white-space:pre-wrap;"
        "word-break:break-word;tab-size:4;}"
        ".log-status{font-size:.86rem;color:var(--muted);margin-left:auto;}"
        ".log-analysis{border:1px solid var(--line);border-radius:10px;background:#fbfcfd;"
        "padding:16px;margin:14px 0 16px;}"
        ".log-analysis-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;"
        "margin-bottom:12px;}"
        ".log-analysis-head h3{margin:0 0 4px;}"
        ".log-analysis-status{font-size:.84rem;color:var(--muted);text-align:right;}"
        ".log-analysis-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px;}"
        ".log-stat{border:1px solid var(--line);border-radius:8px;background:#fff;padding:11px 12px;min-width:0;}"
        ".log-stat-label{font-size:.72rem;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;}"
        ".log-stat-value{font-size:1.02rem;font-weight:800;margin-top:4px;word-break:break-word;}"
        ".log-stat-note{font-size:.78rem;color:var(--muted);margin-top:3px;line-height:1.35;}"
        ".log-chart-card{margin-top:12px;border:1px solid var(--line);border-radius:8px;background:#fff;padding:12px;}"
        ".log-chart-title{font-weight:800;margin-bottom:3px;}"
        ".log-chart-note{font-size:.82rem;color:var(--muted);margin-bottom:8px;line-height:1.4;}"
        ".log-chart-scroll{overflow-x:auto;overflow-y:hidden;}"
        ".log-chart-inner{min-width:900px;}"
        ".log-hour-chart{display:block;width:100%;height:auto;}"
        "@media(max-width:900px){.log-analysis-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}"
        "@media(max-width:760px){.log-terminal{height:62vh;min-height:300px;}"
        ".log-view{padding:12px;font-size:.76rem;}.log-status{width:100%;margin-left:0;}"
        ".log-analysis{padding:12px;}.log-analysis-head{flex-direction:column;}"
        ".log-analysis-status{text-align:left;}.log-analysis-grid{grid-template-columns:1fr;}}"
        "table{max-width:100%;}"
        "code{background:#f2f4f7;padding:2px 4px;border-radius:4px;}"
        "@media(max-width:900px){.dashboard-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}"
        "@media(max-width:760px){"
        ".navwrap{align-items:flex-start;flex-direction:column;padding:8px 12px;}"
        ".brand{padding:6px 2px;margin:0;}"
        ".brand-sub{font-size:.58rem;}"
        ".navlinks{width:100%;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px;}"
        ".module-meta{width:100%;margin-left:0;padding:3px 2px 5px;align-items:flex-end;}"
        ".module-actions{width:100%;justify-content:flex-end;}"
        ".module-clock{width:100%;text-align:right;font-size:.82rem;white-space:normal;line-height:1.25;}"
        ".recording-control{align-items:stretch;flex-direction:column;}"
        ".recording-control-actions{text-align:left;}"
        ".recording-control-actions button{width:100%;min-width:0;}"
        ".navitem,.navdrop>summary{width:100%;padding:9px;}"
        ".navdrop{width:100%;}"
        ".dropdown{position:static;min-width:0;margin-top:3px;box-shadow:none;border-color:#566170;}"
        ".dashboard-grid{grid-template-columns:1fr;}"
        ".page{margin-top:12px;padding-left:8px;padding-right:8px;}"
        "div.box{padding:15px;border-radius:8px;}"
        ".page-title{flex-direction:column;}"
        "}"
        "</style></head><body>";

    String returnPath =
        server.uri();

    if (
        !returnPath.startsWith("/") ||
        returnPath.startsWith("//") ||
        returnPath.indexOf('\r') >= 0 ||
        returnPath.indexOf('\n') >= 0 ||
        returnPath.length() > 192
    ) {
        returnPath = "/";
    }

    html +=
        "<nav class='topbar'><div class='navwrap'>";

    html +=
        "<a class='brand' href='/'><span class='brand-main'>"
        SENSORFORGE_APP_NAME_LITERAL
        "</span><span class='brand-sub'>" SENSORFORGE_PLATFORM_LITERAL
        " &middot; CORE v" SENSORFORGE_CORE_VERSION_LITERAL "</span></a>";

    html +=
        "<div class='navlinks'>";

    html +=
        "<a class='navitem' data-nav='home' href='/'>" +
        htmlText(UI_NAV_OVERVIEW) +
        "</a>";

    html +=
        "<details class='navdrop' id='navRecordings'><summary data-nav='recordings'>" +
        htmlText(UI_NAV_RECORDINGS) +
        "</summary><div class='dropdown'><a href='/files'>" +
        htmlText(UI_NAV_MANAGE_RECORDINGS) +
        "</a></div></details>";

    html +=
        "<details class='navdrop' id='navCamera'><summary data-nav='camera'>" +
        htmlText(UI_NAV_CAMERA) +
        "</summary><div class='dropdown'><a href='/preview'>" +
        htmlText(UI_NAV_LIVE_PREVIEW) +
        "</a></div></details>";

    html +=
        "<details class='navdrop' id='navSensor'><summary data-nav='sensor'>" +
        htmlText(UI_NAV_SENSOR) +
        "</summary><div class='dropdown'>";

    if (radarConfigAvailable()) {
        html +=
            "<a href='/radar_config'>" +
            htmlText(UI_NAV_RADAR_CONFIG) +
            "</a>";
    } else {
        String radarUnavailableTitle =
            htmlText(UI_RADAR_CONFIG_PIR_NOTE);

        if (freenovePirCompatibilityNoteApplies()) {
            radarUnavailableTitle +=
                " " +
                htmlText(UI_MOTION_PIR_FREENOVE_NOTE);
        }

        html +=
            "<span class='nav-disabled' title='" +
            radarUnavailableTitle +
            "'>" +
            htmlText(UI_NAV_RADAR_CONFIG) +
            " &middot; " +
            htmlText(motionSensorTypeUiId()) +
            "</span>";
    }

    html +=
        "<a href='/image_motion'>" +
        htmlText(UI_NAV_IMAGE_MOTION) +
        "</a><a href='/#simulation'>" +
        htmlText(UI_NAV_SIMULATE_MOTION) +
        "</a></div></details>";

    html +=
        "<details class='navdrop' id='navSystem'><summary data-nav='system'>" +
        htmlText(UI_NAV_SYSTEM) +
        "</summary><div class='dropdown'>";

    html +=
        "<a href='/sd_maintenance'>" + htmlText(UI_NAV_SD_MAINTENANCE) + "</a>"
        "<a href='/log'>" + htmlText(UI_NAV_LOG_VIEWER) + "</a>"
        "<a href='/sysinfo'>" + htmlText(UI_NAV_SYSTEM_INFO) + "</a>"
        "<a href='/board'>" + htmlText(UI_NAV_BOARD_INFO) + "</a>"
        "<a href='/psram'>" + htmlText(UI_NAV_PSRAM_TEST) + "</a>"
        "<a href='/firmware_update'>" + htmlText(UI_NAV_FIRMWARE_UPDATE) + "</a>"
        "<a href='/license'>" + htmlText(UI_NAV_LICENSE) + "</a>"
        "<a href='/transport'>" + htmlText(UI_NAV_TRANSPORT) + "</a>"
        "<div class='sep'></div><a class='danger-link' href='/reboot'>" +
        htmlText(UI_NAV_REBOOT) +
        "</a>"
        "<a class='danger-link' href='/shutdown'>" +
        htmlText(UI_NAV_SHUTDOWN) +
        "</a></div></details>";

    html +=
        "<a class='navitem' data-nav='config' href='/config'>" +
        htmlText(UI_NAV_CONFIGURATION) +
        "</a></div>";

    html +=
        "<div class='module-meta'><div class='module-actions'>";

    html +=
        "<form class='language-switch' method='POST' action='/language'>"
        "<input type='hidden' name='return' value='" +
        htmlEscape(returnPath) +
        "'>"
        "<select name='code' aria-label='" + htmlText(UI_LANGUAGE) + "' onchange='this.form.submit()'>"
        "<option value='de'" +
        String(cfg_web_language == "de" ? " selected" : "") +
        ">DE</option>"
        "<option value='en'" +
        String(cfg_web_language == "en" ? " selected" : "") +
        ">EN</option></select></form>";

    html +=
        "<button id='recordingPauseGlobal' class='module-recording-switch " +
        String(recordingAutomationPaused ? "off" : "on") +
        "' type='button' data-on='" +
        htmlText(UI_RECORDING_SWITCH_ON) +
        "' data-off='" +
        htmlText(UI_RECORDING_SWITCH_OFF) +
        "' data-auto-on='" +
        htmlText(UI_AUTO_PAUSE_ENABLED) +
        "' data-auto-off='" +
        htmlText(UI_AUTO_PAUSE_DISABLED) +
        "' data-error='" +
        htmlText(UI_RECORDING_CHANGE_FAILED) +
        "'>" +
        htmlText(
            recordingAutomationPaused
            ? UI_RECORDING_SWITCH_OFF
            : UI_RECORDING_SWITCH_ON
        ) +
        "</button></div>";

    html +=
        "<div id='moduleClock' class='module-clock invalid' data-time-label='" +
        htmlText(UI_TIME) +
        "' data-thermal-label='" +
        htmlText(UI_THERMAL_PROTECTION) +
        "' data-warning-from='" +
        htmlText(UI_WARNING_FROM) +
        "' data-emergency-from='" +
        htmlText(UI_EMERGENCY_FROM) +
        "' data-recovery-below='" +
        htmlText(UI_RECOVERY_BELOW) +
        "' data-rtc-label='" +
        htmlText(UI_RTC_ENCLOSURE_INDICATOR) +
        "' title='" +
        htmlText(UI_MODULE_TIME_TITLE) +
        "'>" +
        htmlText(UI_TIME) +
        " --.--.---- &middot; --:--:--</div></div></div></nav>";

    // Central browser-session keepalive. Every normal SensorForge page uses
    // this common header/menu, so an open web UI keeps WiFi alive even if the
    // page itself is otherwise idle. API traffic never calls htmlHeader().
    html +=
        "<script>"
        "(function(){"
        "if(window.__sensorForgeKeepalive)return;"
        "window.__sensorForgeKeepalive=true;"
        "var lastPing=0;"
        "function ping(force){"
        "var now=Date.now();"
        "if(!force&&now-lastPing<5000)return;"
        "lastPing=now;"
        "fetch('/activity?t='+now,{cache:'no-store',credentials:'same-origin',keepalive:true})"
        ".catch(function(){});"
        "}"
        "window.sensorForgeKeepalive=ping;"
        "ping(true);"
        "setInterval(function(){ping(true);},10000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)ping(true);});"
        "window.addEventListener('focus',function(){ping(true);});"
        "window.addEventListener('pageshow',function(){ping(true);});"
        "document.addEventListener('pointerdown',function(){ping(false);},{passive:true});"
        "document.addEventListener('keydown',function(){ping(false);});"
        "})();"
        "</script>";

    // Live module clock and the global recording switch use translated strings
    // supplied as data attributes above. The JSON/status payload stays stable.
    html +=
        "<script>"
        "(function(){"
        "var el=document.getElementById('moduleClock');"
        "var pauseEl=document.getElementById('recordingPauseGlobal');"
        "if(!el)return;"
        "var baseMs=0,syncMs=0,pauseActive=false,cpuText='',rtcText='',thermalState='OK';"
        "var timeLabel=el.dataset.timeLabel||'Time';"
        "function pad(v){return String(v).padStart(2,'0');}"
        "function tempSuffix(){"
        "var x='';"
        "if(cpuText)x+=' · CPU '+cpuText+' °C';"
        "if(rtcText)x+=' · RTC '+rtcText+' °C';"
        "return x;"
        "}"
        "function render(){"
        "var suffix=tempSuffix();"
        "if(!baseMs){el.textContent=timeLabel+' --.--.---- · --:--:--'+suffix;el.classList.add('invalid');return;}"
        "var d=new Date(baseMs+(Date.now()-syncMs));"
        "el.textContent=timeLabel+' '+pad(d.getUTCDate())+'.'+pad(d.getUTCMonth()+1)+'.'+d.getUTCFullYear()+"
        "' · '+pad(d.getUTCHours())+':'+pad(d.getUTCMinutes())+':'+pad(d.getUTCSeconds())+suffix;"
        "el.classList.remove('invalid');"
        "}"
        "function renewPauseLease(){"
        "if(!pauseActive||document.hidden)return;"
        "fetch('/recording_pause_keepalive?t='+Date.now(),{method:'POST',cache:'no-store',"
        "credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "function apply(s){"
        "pauseActive=!!(s&&s.recording_paused);"
        "if(pauseEl){pauseEl.textContent=pauseActive?pauseEl.dataset.off:pauseEl.dataset.on;"
        "pauseEl.classList.toggle('off',pauseActive);pauseEl.classList.toggle('on',!pauseActive);"
        "pauseEl.title=(s&&s.web_recording_auto_pause)?pauseEl.dataset.autoOn:pauseEl.dataset.autoOff;}"
        "if(pauseActive)renewPauseLease();"
        "cpuText=(s&&s.cpu_temp_valid)?Number(s.cpu_temp_c).toFixed(1):'';"
        "rtcText=(s&&s.rtc_temp_valid)?Number(s.rtc_temp_c).toFixed(1):'';"
        "thermalState=(s&&s.thermal_state)||'OK';"
        "el.classList.toggle('thermal-warning',thermalState==='WARNING');"
        "el.classList.toggle('thermal-emergency',thermalState==='EMERGENCY');"
        "if(s){el.title=el.dataset.thermalLabel+': CPU '+el.dataset.warningFrom+' '+Number(s.thermal_warning_c).toFixed(0)+"
        "' °C, '+el.dataset.emergencyFrom+' '+Number(s.thermal_emergency_c).toFixed(0)+' °C, '+el.dataset.recoveryBelow+' '+"
        "Number(s.thermal_recovery_c).toFixed(0)+' °C; '+el.dataset.rtcLabel+' '+el.dataset.warningFrom+' '+"
        "Number(s.thermal_rtc_warning_c).toFixed(0)+' °C, '+el.dataset.emergencyFrom+' '+Number(s.thermal_rtc_emergency_c).toFixed(0)+"
        "' °C, '+el.dataset.recoveryBelow+' '+Number(s.thermal_rtc_recovery_c).toFixed(0)+' °C';}"
        "if(!s||!s.clock_valid||!s.clock){baseMs=0;render();return;}"
        "var m=/^(\\d{2})\\.(\\d{2})\\.(\\d{4}) (\\d{2}):(\\d{2}):(\\d{2})$/.exec(s.clock);"
        "if(!m){baseMs=0;render();return;}"
        "baseMs=Date.UTC(+m[3],+m[2]-1,+m[1],+m[4],+m[5],+m[6]);"
        "syncMs=Date.now();render();"
        "}"
        "function sync(){fetch('/ui_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
        ".then(function(r){if(!r.ok)throw new Error();return r.json();}).then(apply).catch(function(){});}"
        "if(pauseEl)pauseEl.addEventListener('click',function(){"
        "pauseEl.disabled=true;"
        "fetch('/recording_pause',{method:'POST',cache:'no-store',credentials:'same-origin',"
        "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+(pauseActive?'resume':'pause')})"
        ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
        ".then(apply).catch(function(e){alert(pauseEl.dataset.error+': '+e.message);sync();})"
        ".finally(function(){pauseEl.disabled=false;});"
        "});"
        "sync();"
        "setInterval(render,1000);"
        "setInterval(sync,10000);"
        "setInterval(renewPauseLease,10000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)sync();});"
        "window.addEventListener('focus',sync);"
        "})();"
        "</script>";

    html +=
        "<main class='page'><div class='box'>";

    return html;
}


static String htmlFooter()
{
    return
        "<script>"
        "(function(){"
        "var p=location.pathname;"
        "var group='';"
        "if(p==='/')group='home';"
        "else if(p==='/config'||p==='/save'||p==='/audio_test_record'||p==='/audio_benchmark')group='config';"
        "else if(p.indexOf('/files')===0||p==='/file'||p==='/play')group='recordings';"
        "else if(p==='/preview'||p==='/snapshot')group='camera';else if(p==='/image_motion')group='sensor';"
        "else if(p.indexOf('/radar_')===0)group='sensor';"
        "else group='system';"
        "var active=document.querySelector('[data-nav=\"'+group+'\"]');"
        "if(active)active.classList.add('active');"
        "document.addEventListener('click',function(e){"
        "var open=document.querySelectorAll('.navdrop[open]');"
        "for(var i=0;i<open.length;i++){if(!open[i].contains(e.target))open[i].removeAttribute('open');}"
        "});"
        "})();"
        "</script>"
        "</div></main></body></html>";
}


// -------------------------------------------------------------
// Recording priority
// -------------------------------------------------------------

static bool rejectWhileRecording(
    const char *operation
)
{
    if (!recorderIsOpen())
        return false;

    server.send(
        409,
        "text/plain; charset=utf-8",
        "Recording active - " +
        String(operation) +
        " is temporarily unavailable."
    );

    return true;
}


// V22 module bridge for the external Web Log Reader. Keep these wrappers
// intentionally small so the log-reader module can reuse the common WebConfig
// shell without owning unrelated WebConfig state.
String webConfigLogReaderHtmlHeader()
{
    return htmlHeader();
}

String webConfigLogReaderHtmlFooter()
{
    return htmlFooter();
}

String webConfigLogReaderHtmlEscape(const String &value)
{
    return htmlEscape(value);
}

bool webConfigLogReaderRejectWhileRecording(const char *operation)
{
    return rejectWhileRecording(operation);
}


// Lightweight dynamic UI state. The dashboard uses the high-level
// `recording` flag (same source as the serial STATUS line), while
// `recorder_open` remains available for SD/file-operation safety.
static void handleUiStatus()
{
    String clockText =
        moduleClockText();

    bool clockValid =
        clockText.length() > 0;

    time_t armDeadlineEpoch = 0;
    int64_t armRemainingSeconds = 0;

    RecordingNotBeforeState armState =
        configRecordingNotBeforeState(
            &armDeadlineEpoch,
            &armRemainingSeconds
        );

    bool recordingArmed =
        configRecordingAllowedNow();

    bool recordingSafetyCooldown =
        recordingSafetyCooldownActive();

    uint32_t recordingSafetyRemainingSeconds =
        recordingSafetyCooldownRemainingSeconds();

    char armEpochText[32];
    char armRemainingText[32];

    snprintf(
        armEpochText,
        sizeof(armEpochText),
        "%lld",
        (long long)armDeadlineEpoch
    );

    snprintf(
        armRemainingText,
        sizeof(armRemainingText),
        "%lld",
        (long long)armRemainingSeconds
    );

    float cpuTempC =
        thermalCpuTemperatureC();

    bool cpuTempValid =
        isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    String json =
        String("{\"recording\":") +
        (recording ? "true" : "false") +
        ",\"recording_paused\":" +
        (recordingAutomationPaused ? "true" : "false") +
        ",\"web_recording_auto_pause\":" +
        (cfg_web_recording_auto_pause ? "true" : "false") +
        ",\"recorder_open\":" +
        (recorderIsOpen() ? "true" : "false") +
        ",\"clock_valid\":" +
        (clockValid ? "true" : "false") +
        ",\"clock\":\"" +
        clockText +
        "\",\"recording_armed\":" +
        (recordingArmed ? "true" : "false") +
        ",\"recording_arm_state\":\"" +
        String(configRecordingNotBeforeStateName(armState)) +
        "\",\"recording_arm_epoch\":" +
        String(armEpochText) +
        ",\"recording_arm_remaining_sec\":" +
        String(armRemainingText) +
        ",\"recording_not_before_display\":\"" +
        configRecordingNotBeforeDisplay() +
        "\",\"recording_safety_cooldown\":" +
        (recordingSafetyCooldown ? "true" : "false") +
        ",\"recording_safety_cooldown_remaining_sec\":" +
        String(recordingSafetyRemainingSeconds) +
        ",\"recording_event_max_seconds\":" +
        String(cfg_recording_event_max_seconds) +
        ",\"recording_event_cooldown_seconds\":" +
        String(cfg_recording_event_cooldown_seconds) +
        ",\"cpu_temp_valid\":" +
        (cpuTempValid ? "true" : "false") +
        ",\"cpu_temp_c\":" +
        String(cpuTempValid ? cpuTempC : 0.0f, 1) +
        ",\"rtc_temp_valid\":" +
        (rtcTempValid ? "true" : "false") +
        ",\"rtc_temp_c\":" +
        String(rtcTempValid ? rtcTempC : 0.0f, 1) +
        ",\"thermal_state\":\"" +
        String(thermalStateName()) +
        "\",\"thermal_source\":\"" +
        String(thermalSourceName()) +
        "\",\"thermal_warning_c\":" +
        String(SENSORFORGE_THERMAL_WARNING_C, 1) +
        ",\"thermal_emergency_c\":" +
        String(SENSORFORGE_THERMAL_EMERGENCY_C, 1) +
        ",\"thermal_recovery_c\":" +
        String(SENSORFORGE_THERMAL_RECOVERY_C, 1) +
        ",\"thermal_rtc_warning_c\":" +
        String(SENSORFORGE_THERMAL_RTC_WARNING_C, 1) +
        ",\"thermal_rtc_emergency_c\":" +
        String(SENSORFORGE_THERMAL_RTC_EMERGENCY_C, 1) +
        ",\"thermal_rtc_recovery_c\":" +
        String(SENSORFORGE_THERMAL_RTC_RECOVERY_C, 1) +
        "}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static void handleMotionStatus()
{
    MotionDiagnosticsSnapshot diagnostics;
    motionDiagnosticsGetSnapshot(diagnostics);

    bool radarDetected =
        radarConfigAvailable();

    bool radarTrackingAvailable =
        radarMotionTrackingAvailable();

    bool useRadarTrigger =
        radarDetected &&
        radarTrackingAvailable;

    bool physicalMotionActive =
        useRadarTrigger
        ? diagnostics.radarMotionActive
        : diagnostics.presenceActive;

    bool simulatedMotion =
        webConfigMotionActive();

    bool operationalMotionActive =
        physicalMotionActive ||
        simulatedMotion;

    uint32_t primaryTriggerCount =
        useRadarTrigger
        ? diagnostics.radarTriggerCount
        : diagnostics.presenceTriggerCount;

    bool primaryLastTriggerValid =
        useRadarTrigger
        ? diagnostics.radarLastTriggerValid
        : diagnostics.presenceLastTriggerValid;

    uint32_t primaryLastTriggerAgeMs =
        useRadarTrigger
        ? diagnostics.radarLastTriggerAgeMs
        : diagnostics.presenceLastTriggerAgeMs;

    String json =
        String("{\"sensor_type\":\"") +
        (radarDetected ? "radar" : "pir") +
        "\",\"presence_pin\":" +
        String((int)PRESENCE_PIN) +
        ",\"presence_active\":" +
        (diagnostics.presenceActive ? "true" : "false") +
        ",\"presence_trigger_count\":" +
        String(diagnostics.presenceTriggerCount) +
        ",\"presence_last_trigger_valid\":" +
        (diagnostics.presenceLastTriggerValid ? "true" : "false") +
        ",\"presence_last_trigger_age_ms\":" +
        String(diagnostics.presenceLastTriggerAgeMs) +
        ",\"radar_detected\":" +
        (radarDetected ? "true" : "false") +
        ",\"radar_tracking_available\":" +
        (radarTrackingAvailable ? "true" : "false") +
        ",\"radar_motion_active\":" +
        (diagnostics.radarMotionActive ? "true" : "false") +
        ",\"radar_trigger_count\":" +
        String(diagnostics.radarTriggerCount) +
        ",\"radar_last_trigger_valid\":" +
        (diagnostics.radarLastTriggerValid ? "true" : "false") +
        ",\"radar_last_trigger_age_ms\":" +
        String(diagnostics.radarLastTriggerAgeMs) +
        ",\"radar_last_gate\":" +
        String(radarLastMotionGate()) +
        ",\"radar_last_energy_db\":" +
        String(radarLastMotionEnergyDb(), 1) +
        ",\"primary_source\":\"" +
        (useRadarTrigger ? "radar" : "presence") +
        "\",\"primary_trigger_count\":" +
        String(primaryTriggerCount) +
        ",\"primary_last_trigger_valid\":" +
        (primaryLastTriggerValid ? "true" : "false") +
        ",\"primary_last_trigger_age_ms\":" +
        String(primaryLastTriggerAgeMs) +
        ",\"physical_motion_active\":" +
        (physicalMotionActive ? "true" : "false") +
        ",\"simulated_motion\":" +
        (simulatedMotion ? "true" : "false") +
        ",\"motion_active\":" +
        (operationalMotionActive ? "true" : "false") +
        ",\"image_motion_mode\":\"" +
        cfg_motion_recording_decision +
        "\",\"image_motion\":" +
        imageMotionDiagnosticsJson() +
        "}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static void handleRecordingPause()
{
    String action =
        server.arg("action");

    action.trim();
    action.toLowerCase();

    if (action == "pause") {
        setRecordingAutomationPaused(
            true,
            "operator"
        );

    } else if (
        action == "resume"
    ) {
        recordingAutoPauseSuppressedForUiSession = true;
        noteRecordingUiSessionActivity();

        setRecordingAutomationPaused(
            false,
            "operator"
        );

    } else {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording pause action"
        );
        return;
    }

    handleUiStatus();
}


static void handleRecordingPauseKeepalive()
{
    bool transportHold =
        server.hasArg("transport") &&
        server.arg("transport") == "1";

    // A keepalive must be able to re-establish auto-pause, not merely extend
    // an already-active lease. This is essential for the standalone player.
    maybeAutoPauseRecordingForWebUi();

    renewRecordingPauseLease(
        transportHold
    );

    server.send(
        204,
        "text/plain",
        ""
    );
}


static void handleTransportPauseRelease()
{
    // Leaving the transport preparation page must not create a sticky pause.
    // Keep the pause itself active, but return to the ordinary 35 s lease.
    recordingPauseTransportHold = false;
    renewRecordingPauseLease();

    server.send(
        204,
        "text/plain",
        ""
    );
}


static void handleLanguageChange()
{
    String language =
        server.arg("code");

    language.trim();
    language.toLowerCase();

    if (!uiLanguageSupported(language)) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid web language"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveWebLanguage(
            language,
            configSdAvailable() &&
                configSdPresent(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            String(tr(UI_LANGUAGE_SAVE_FAILED)) +
                ": " +
                error
        );
        return;
    }

    // Logs deliberately remain language-neutral/English.
    logWrite(
        "Web language changed | code=" +
        language
    );

    String target =
        server.arg("return");

    if (
        !target.startsWith("/") ||
        target.startsWith("//") ||
        target.indexOf('\r') >= 0 ||
        target.indexOf('\n') >= 0 ||
        target.length() > 192
    ) {
        target = "/";
    }

    server.sendHeader(
        "Location",
        target
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static String recordingBrowserModalHtml(
    bool visible
)
{
    String html =
        "<div id='recordingBlockedModal' class='modal-backdrop'";

    if (!visible)
        html += " hidden";

    html +=
        ">"
        "<div class='modal-card' role='dialog' aria-modal='true' "
        "aria-labelledby='recordingBlockedTitle'>"
        "<span class='status-pill danger'>AUFNAHME AKTIV</span>"
        "<h3 id='recordingBlockedTitle'>Aufnahmen momentan nicht verfügbar</h3>"
        "<p>Während einer laufenden Aufnahme wird der Verzeichnisbaum nicht gelesen. "
        "Das vermeidet zusätzliche SD-I/O und schützt die aktive Aufzeichnung.</p>"
        "<p>Die Ansicht wird automatisch wieder freigegeben, sobald der Recorder fertig ist.</p>"
        "<div class='modal-actions'>"
        "<button type='button' class='primary' onclick='location.reload()'>Erneut versuchen</button>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "</div>"
        "<div id='recordingBlockedState' class='modal-state'>Warte auf Aufnahmeende ...</div>"
        "</div></div>";

    return html;
}


// -------------------------------------------------------------
// CONFIG COPY CONSISTENCY
// -------------------------------------------------------------

enum ConfigCopyCompareResult : uint8_t {
    CONFIG_COPY_COMPARE_UNAVAILABLE = 0,
    CONFIG_COPY_COMPARE_IDENTICAL,
    CONFIG_COPY_COMPARE_DIFFERENT,
    CONFIG_COPY_COMPARE_READ_ERROR
};


static ConfigCopyCompareResult compareConfigCopies()
{
    if (
        !configSdAvailable() ||
        !configSdPresent() ||
        !configInternalAvailable()
    ) {
        return
            CONFIG_COPY_COMPARE_UNAVAILABLE;
    }


    if (
        !STORAGE.exists(
            "/config.txt"
        ) ||
        !LittleFS.exists(
            "/config.txt"
        )
    ) {
        return
            CONFIG_COPY_COMPARE_UNAVAILABLE;
    }


    File sdFile =
        STORAGE.open(
            "/config.txt",
            FILE_READ
        );

    File internalFile =
        LittleFS.open(
            "/config.txt",
            FILE_READ
        );


    if (
        !sdFile ||
        !internalFile
    ) {

        if (sdFile)
            sdFile.close();

        if (internalFile)
            internalFile.close();

        return
            CONFIG_COPY_COMPARE_READ_ERROR;
    }


    if (
        sdFile.size() !=
        internalFile.size()
    ) {

        sdFile.close();
        internalFile.close();

        return
            CONFIG_COPY_COMPARE_DIFFERENT;
    }


    uint8_t sdBuffer[256];
    uint8_t internalBuffer[256];


    while (
        sdFile.available() ||
        internalFile.available()
    ) {

        size_t sdRead =
            sdFile.read(
                sdBuffer,
                sizeof(sdBuffer)
            );

        size_t internalRead =
            internalFile.read(
                internalBuffer,
                sizeof(internalBuffer)
            );


        if (
            sdRead !=
            internalRead
        ) {

            sdFile.close();
            internalFile.close();

            return
                CONFIG_COPY_COMPARE_DIFFERENT;
        }


        if (
            sdRead > 0 &&
            memcmp(
                sdBuffer,
                internalBuffer,
                sdRead
            ) != 0
        ) {

            sdFile.close();
            internalFile.close();

            return
                CONFIG_COPY_COMPARE_DIFFERENT;
        }


        serviceWebLongOperation();
    }


    sdFile.close();
    internalFile.close();


    return
        CONFIG_COPY_COMPARE_IDENTICAL;
}


// -------------------------------------------------------------
// ROOT PAGE
// -------------------------------------------------------------

static void handleRoot()
{
    // Keep SD/config state fresh for the dashboard as well.
    configRefreshSdStatus();

    bool recordingActive =
        recording;

    bool recordingPaused =
        recordingAutomationPaused;

    time_t armDeadlineEpoch = 0;
    int64_t armRemainingSeconds = 0;

    RecordingNotBeforeState armState =
        configRecordingNotBeforeState(
            &armDeadlineEpoch,
            &armRemainingSeconds
        );

    bool recordingArmed =
        configRecordingAllowedNow();

    bool recordingSafetyCooldown =
        recordingSafetyCooldownActive();

    uint32_t recordingSafetyRemainingSeconds =
        recordingSafetyCooldownRemainingSeconds();

    String armDeadlineDisplay =
        configRecordingNotBeforeDisplay();

    bool simulatedMotion =
        webConfigMotionActive();

    bool radarDetected =
        radarConfigAvailable();

    bool radarTrackingAvailable =
        radarMotionTrackingAvailable();

    MotionDiagnosticsSnapshot motionDiagnostics;
    motionDiagnosticsGetSnapshot(motionDiagnostics);

    bool radarMotion =
        motionDiagnostics.radarMotionActive;

    bool presenceInputActive =
        motionDiagnostics.presenceActive;

    bool physicalMotionActive =
        radarDetected && radarTrackingAvailable
        ? radarMotion
        : presenceInputActive;

    bool motionActive =
        simulatedMotion ||
        physicalMotionActive;

    float cpuTempC =
        thermalCpuTemperatureC();

    bool cpuTempValid =
        isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    String thermalUiState =
        String(
            thermalStateUiName(
                thermalStateName()
            )
        );

    if (
        strcmp(
            thermalSourceName(),
            "NONE"
        ) != 0
    ) {
        thermalUiState +=
            " (" +
            String(thermalSourceName()) +
            ")";
    }

    uint64_t totalBytes =
        STORAGE.totalBytes();

    uint64_t usedBytes =
        STORAGE.usedBytes();

    uint64_t freeBytes =
        totalBytes > usedBytes
        ? totalBytes - usedBytes
        : 0;

    uint32_t usedPercent =
        totalBytes > 0
        ? (uint32_t)(
            (usedBytes * 100ULL) /
            totalBytes
        )
        : 0;

    if (usedPercent > 100)
        usedPercent = 100;

    String networkText;

    if (WiFi.status() == WL_CONNECTED) {
        networkText =
            "STA " +
            WiFi.localIP().toString();
    } else {
        networkText =
            String(tr(UI_HOTSPOT)) +
            " " +
            WiFi.softAPIP().toString();
    }

    String html =
        htmlHeader();

    String notice =
        server.arg("notice");

    bool knownNotice =
        notice == "config_saved_both" ||
        notice == "config_saved_internal" ||
        notice == "sd_wipe_done" ||
        notice == "sd_wipe_failed" ||
        notice == "sd_format_done" ||
        notice == "sd_format_failed" ||
        notice == "sd_format_unsupported" ||
        notice == "sd_secure_done" ||
        notice == "sd_secure_failed" ||
        notice == "sd_secure_unsupported" ||
        notice == "sd_recording_active" ||
        notice == "sd_storage_locked" ||
        notice == "sd_restore_source_failed" ||
        notice == "sd_restore_failed";

    if (knownNotice) {
        bool noticeIsError =
            notice == "sd_wipe_failed" ||
            notice == "sd_format_failed" ||
            notice == "sd_format_unsupported" ||
            notice == "sd_secure_failed" ||
            notice == "sd_secure_unsupported" ||
            notice == "sd_recording_active" ||
            notice == "sd_storage_locked" ||
            notice == "sd_restore_source_failed" ||
            notice == "sd_restore_failed";

        UiTextId noticeTitle =
            UI_NOTICE_CONFIG_SAVED_TITLE;

        UiTextId noticeBody =
            UI_NOTICE_CONFIG_BOTH_BODY;

        if (notice == "config_saved_internal") {
            noticeBody =
                UI_NOTICE_CONFIG_INTERNAL_BODY;
        } else if (notice == "sd_wipe_done") {
            noticeTitle = UI_NOTICE_SD_WIPE_DONE_TITLE;
            noticeBody = UI_NOTICE_SD_WIPE_DONE_BODY;
        } else if (notice == "sd_wipe_failed") {
            noticeTitle = UI_NOTICE_SD_WIPE_FAILED_TITLE;
            noticeBody = UI_NOTICE_SD_WIPE_FAILED_BODY;
        } else if (notice == "sd_format_done") {
            noticeTitle = UI_NOTICE_SD_FORMAT_DONE_TITLE;
            noticeBody = UI_NOTICE_SD_FORMAT_DONE_BODY;
        } else if (notice == "sd_format_failed") {
            noticeTitle = UI_NOTICE_SD_FORMAT_FAILED_TITLE;
            noticeBody = UI_NOTICE_SD_FORMAT_FAILED_BODY;
        } else if (notice == "sd_format_unsupported") {
            noticeTitle = UI_NOTICE_SD_FORMAT_UNSUPPORTED_TITLE;
            noticeBody = UI_NOTICE_SD_FORMAT_UNSUPPORTED_BODY;
        } else if (notice == "sd_secure_done") {
            noticeTitle = UI_NOTICE_SD_SECURE_DONE_TITLE;
            noticeBody = UI_NOTICE_SD_SECURE_DONE_BODY;
        } else if (notice == "sd_secure_failed") {
            noticeTitle = UI_NOTICE_SD_SECURE_FAILED_TITLE;
            noticeBody = UI_NOTICE_SD_SECURE_FAILED_BODY;
        } else if (notice == "sd_secure_unsupported") {
            noticeTitle = UI_NOTICE_SD_SECURE_UNSUPPORTED_TITLE;
            noticeBody = UI_NOTICE_SD_SECURE_UNSUPPORTED_BODY;
        } else if (notice == "sd_recording_active") {
            noticeTitle = UI_NOTICE_SD_RECORDING_ACTIVE_TITLE;
            noticeBody = UI_NOTICE_SD_RECORDING_ACTIVE_BODY;
        } else if (notice == "sd_storage_locked") {
            noticeTitle = UI_NOTICE_SD_STORAGE_LOCKED_TITLE;
            noticeBody = UI_NOTICE_SD_STORAGE_LOCKED_BODY;
        } else if (notice == "sd_restore_source_failed") {
            noticeTitle = UI_NOTICE_SD_RESTORE_SOURCE_FAILED_TITLE;
            noticeBody = UI_NOTICE_SD_RESTORE_SOURCE_FAILED_BODY;
        } else if (notice == "sd_restore_failed") {
            noticeTitle = UI_NOTICE_SD_RESTORE_FAILED_TITLE;
            noticeBody = UI_NOTICE_SD_RESTORE_FAILED_BODY;
        }

        html +=
            "<div id='flashNotice' class='flash-notice";

        if (noticeIsError)
            html += " error";

        html +=
            "'><strong>" +
            htmlText(noticeTitle) +
            "</strong><span class='muted'>" +
            htmlText(noticeBody) +
            "</span></div>"
            "<script>"
            "history.replaceState(null,'','/');"
            "setTimeout(function(){"
                "var n=document.getElementById('flashNotice');"
                "if(!n)return;"
                "n.classList.add('hide');"
                "setTimeout(function(){if(n.parentNode)n.parentNode.removeChild(n);},400);"
            "},6200);"
            "</script>";
    }

    html +=
        "<div class='page-title'><div><h2>" +
        htmlText(UI_DASH_OVERVIEW) +
        "</h2><p>" +
        htmlEscape(cfg_hostname) +
        " &middot; " SENSORFORGE_PLATFORM_LITERAL
        " &middot; Core v" SENSORFORGE_CORE_VERSION_LITERAL "</p></div>";

    if (recordingActive) {
        html +=
            "<span id='recordingStatusPill' class='status-pill danger'>" +
            htmlText(UI_STATUS_RECORDING_RUNNING) +
            "</span>";
    } else if (recordingPaused) {
        html +=
            "<span id='recordingStatusPill' class='status-pill warn'>" +
            htmlText(UI_STATUS_RECORDING_PAUSED) +
            "</span>";
    } else if (recordingSafetyCooldown) {
        html +=
            "<span id='recordingStatusPill' class='status-pill warn'>" +
            htmlText(UI_STATUS_SAFETY_PAUSE) +
            "</span>";
    } else if (!recordingArmed) {
        html +=
            "<span id='recordingStatusPill' class='status-pill warn'>" +
            htmlText(UI_STATUS_NOT_ARMED) +
            "</span>";
    } else {
        html +=
            "<span id='recordingStatusPill' class='status-pill ok'>" +
            htmlText(UI_STATUS_READY) +
            "</span>";
    }

    html +=
        "</div>";

    String armBannerBorder = "#087a00";
    String armBannerBackground = "#eef9f0";
    String armTitle =
        tr(UI_ARM_CAMERA_ARMED);
    String armDeadlineLine =
        tr(UI_ARM_NO_TIME_LOCK);
    String armCountdownLine =
        tr(UI_ARM_AUTOMATION_RELEASED);

    if (armState == RECORDING_NOT_BEFORE_WAITING) {
        armBannerBorder = "#d97706";
        armBannerBackground = "#fff7e8";
        armTitle =
            tr(UI_ARM_CAMERA_ARMING);
        armDeadlineLine =
            String(tr(UI_ARM_ON_PREFIX)) +
            " " +
            armDeadlineDisplay;
        armCountdownLine =
            String(tr(UI_ARM_IN_PREFIX)) +
            " " +
            recordingArmCountdownText(
                armRemainingSeconds
            );
    } else if (armState == RECORDING_NOT_BEFORE_REACHED) {
        armDeadlineLine =
            String(tr(UI_ARM_RELEASE_TIME)) +
            ": " +
            armDeadlineDisplay;
    } else if (armState == RECORDING_NOT_BEFORE_CLOCK_INVALID) {
        armBannerBorder = "#b42318";
        armBannerBackground = "#fff1f0";
        armTitle =
            tr(UI_ARM_CAMERA_NOT_ARMED);
        armDeadlineLine =
            String(tr(UI_ARM_SCHEDULED)) +
            ": " +
            armDeadlineDisplay;
        armCountdownLine =
            tr(UI_ARM_CLOCK_INVALID);
    } else if (armState == RECORDING_NOT_BEFORE_INVALID) {
        armBannerBorder = "#b42318";
        armBannerBackground = "#fff1f0";
        armTitle =
            tr(UI_ARM_CAMERA_NOT_ARMED);
        armDeadlineLine =
            tr(UI_ARM_CONFIG_INVALID);
        armCountdownLine =
            tr(UI_ARM_CHECK_CONFIG);
    }

    html +=
        "<section id='armingBanner' class='settings-section' style='text-align:center;border:2px solid " +
        armBannerBorder +
        ";background:" +
        armBannerBackground +
        ";padding:22px'>"
        "<div id='armingTitle' style='font-size:1.7rem;font-weight:900;letter-spacing:.04em'>" +
        htmlEscape(armTitle) +
        "</div><div id='armingDeadline' style='font-size:1.15rem;font-weight:700;margin-top:8px'>" +
        htmlEscape(armDeadlineLine) +
        "</div><div id='armingCountdown' style='font-size:1.45rem;font-weight:800;margin-top:8px'>" +
        htmlEscape(armCountdownLine) +
        "</div></section>";

    html +=
        "<section id='recordingSafetyBanner' class='settings-section' style='text-align:center;border:2px solid #d97706;background:#fff7e8;padding:18px" +
        String(recordingSafetyCooldown ? "" : ";display:none") +
        "'><div style='font-size:1.35rem;font-weight:900'>" +
        htmlText(UI_SAFETY_TITLE) +
        "</div><div class='muted' style='margin-top:6px'>" +
        htmlText(UI_SAFETY_MAX_DURATION) +
        "</div><div id='recordingSafetyCountdown' style='font-size:1.35rem;font-weight:800;margin-top:8px'>" +
        htmlText(UI_SAFETY_NEW_IN) +
        " " +
        htmlEscape(
            recordingArmCountdownText(
                (int64_t)recordingSafetyRemainingSeconds
            )
        ) +
        "</div></section>";

    html +=
        "<div class='dashboard-grid'>";

    UiTextId recordingValue =
        recordingActive
        ? UI_STATUS_ACTIVE
        : (
            recordingPaused
            ? UI_STATUS_PAUSED
            : (
                recordingSafetyCooldown
                ? UI_VALUE_SAFETY_PAUSE
                : (
                    recordingArmed
                    ? UI_STATUS_READY
                    : UI_VALUE_NOT_ARMED
                )
            )
        );

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_RECORDING) +
        "</div><div id='recordingCardValue' class='card-value'>" +
        htmlText(recordingValue) +
        "</div><div class='card-note'>" +
        htmlText(UI_CARD_FORMAT) +
        ": " +
        htmlEscape(cfg_recording_format) +
        " &middot; " +
        String(cfg_resolution) +
        " @ " +
        String(cfg_fps) +
        " fps</div></section>";

    String presencePillClass;

    if (!presenceInputActive) {
        presencePillClass = "ok";
    } else if (radarDetected && !radarMotion) {
        // OT2 can remain HIGH for the sensor's own hold/absence time even
        // after the fast UART gate evaluation has returned to idle. Do not
        // present that raw hardware level as an active alarm.
        presencePillClass = "warn";
    } else {
        presencePillClass = "danger";
    }

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_MOTION) +
        "</div><div id='motionCardValue' class='card-value'>" +
        htmlText(
            motionActive
            ? UI_MOTION_DETECTED
            : UI_MOTION_NONE
        ) +
        "</div><div class='card-note'>" +
        htmlText(UI_MOTION_SENSOR_TYPE) +
        ": <b>" +
        htmlText(motionSensorTypeUiId()) +
        "</b><br><b>" +
        htmlText(UI_MOTION_CURRENT_SENSOR_STATUS) +
        ":</b> <span id='motionPresenceLabel'>" +
        htmlText(
            radarDetected
            ? UI_MOTION_OT2_GPIO
            : UI_MOTION_PIR_GPIO
        ) +
        " " +
        String((int)PRESENCE_PIN) +
        "</span>: <b id='motionPresenceState' class='status-pill " +
        presencePillClass +
        "'>" +
        String(presenceInputActive ? "HIGH" : "LOW") +
        "</b>";

    html +=
        "<span id='motionRadarRow' style='" +
        String(radarDetected ? "" : "display:none") +
        "'> &middot; <b>" +
        htmlText(UI_MOTION_RADAR_EVALUATION) +
        ":</b> <span id='motionRadarState'>" +
        (
            radarTrackingAvailable
            ? htmlText(
                radarMotion
                ? UI_MOTION_RADAR_EVALUATION_ACTIVE
                : UI_MOTION_RADAR_EVALUATION_NONE
            )
            : htmlText(UI_MOTION_RADAR_FALLBACK)
        ) +
        "</span></span>";

    html +=
        "<span id='motionRadarLastRow' style='" +
        String(radarDetected ? "" : "display:none") +
        "'><br><b>" +
        htmlText(UI_MOTION_LAST_RADAR_TRIGGER) +
        ":</b> " +
        htmlText(UI_MOTION_GATE) +
        " <span id='motionRadarGate'>" +
        (radarLastMotionGate() >= 0 ? String(radarLastMotionGate()) : String("-")) +
        "</span> &middot; " +
        htmlText(UI_MOTION_ENERGY) +
        " <span id='motionRadarEnergy'>" +
        (radarLastMotionGate() >= 0 ? String(radarLastMotionEnergyDb(), 1) : String("-")) +
        "</span> dB &middot; <b id='motionRadarLastTriggerAge'>" +
        htmlText(UI_MOTION_NO_TRIGGER_YET) +
        "</b></span>";

    html +=
        "<br><b>" +
        htmlText(UI_IMAGE_MOTION_LIVE_STATUS) +
        ":</b> <span id='motionImageState' class='status-pill warn'>" +
        htmlText(UI_IMAGE_MOTION_LIVE_WAITING) +
        "</span><span id='motionImageLastRow'> &middot; <b>" +
        htmlText(UI_IMAGE_MOTION_LIVE_LAST_DETECTION) +
        ":</b> <span id='motionImageLast'>" +
        htmlText(UI_IMAGE_MOTION_LIVE_NEVER) +
        "</span></span>";

    html +=
        "<br>Simulation=<span id='motionSimulationState'>" +
        String(simulatedMotion ? "1" : "0") +
        "</span><span id='motionPirLastTriggerRow' style='" +
        String(radarDetected ? "display:none" : "") +
        "'> &middot; " +
        htmlText(UI_MOTION_LAST_TRIGGER) +
        ": <b id='motionLastTrigger'>" +
        htmlText(UI_MOTION_NO_TRIGGER_YET) +
        "</b></span> &middot; " +
        htmlText(UI_MOTION_TRIGGERS_SINCE_OPEN) +
        ": <b id='motionTriggerCount'>0</b>";

    if (freenovePirCompatibilityNoteApplies()) {
        html +=
            "<br><span class='muted' style='display:inline-block;margin-top:8px'>" +
            htmlText(UI_MOTION_PIR_FREENOVE_NOTE) +
            "</span>";
    }

    html +=
        "</div></section>";

    html +=
        "<div id='motionLiveConfig' hidden"
        " data-base-presence='" + String(motionDiagnostics.presenceTriggerCount) + "'"
        " data-base-radar='" + String(motionDiagnostics.radarTriggerCount) + "'"
        " data-detected='" + htmlText(UI_MOTION_DETECTED) + "'"
        " data-none='" + htmlText(UI_MOTION_NONE) + "'"
        " data-active='" + htmlText(UI_STATUS_ACTIVE) + "'"
        " data-radar-active='" + htmlText(UI_MOTION_RADAR_EVALUATION_ACTIVE) + "'"
        " data-radar-none='" + htmlText(UI_MOTION_RADAR_EVALUATION_NONE) + "'"
        " data-fallback='" + htmlText(UI_MOTION_RADAR_FALLBACK) + "'"
        " data-no-trigger='" + htmlText(UI_MOTION_NO_TRIGGER_YET) + "'"
        " data-ago='" + htmlText(UI_MOTION_AGO) + "'"
        " data-ago-suffix='" + htmlText(UI_MOTION_AGO_SUFFIX) + "'"
        " data-image-detected='" + htmlText(UI_IMAGE_MOTION_LIVE_DETECTED) + "'"
        " data-image-none='" + htmlText(UI_IMAGE_MOTION_LIVE_NONE) + "'"
        " data-image-learning='" + htmlText(UI_IMAGE_MOTION_LIVE_LEARNING) + "'"
        " data-image-light='" + htmlText(UI_IMAGE_MOTION_LIVE_GLOBAL_LIGHT) + "'"
        " data-image-error='" + htmlText(UI_IMAGE_MOTION_LIVE_ERROR) + "'"
        " data-image-waiting='" + htmlText(UI_IMAGE_MOTION_LIVE_WAITING) + "'"
        " data-image-inactive='" + htmlText(UI_IMAGE_MOTION_LIVE_INACTIVE) + "'"
        " data-image-never='" + htmlText(UI_IMAGE_MOTION_LIVE_NEVER) + "'"
        "></div>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_SD_CARD) +
        "</div><div class='card-value'>" +
        String(
            (unsigned long)(
                freeBytes /
                1024ULL /
                1024ULL
            )
        ) +
        " MB " +
        htmlText(UI_FREE) +
        "</div><div class='card-note'>" +
        String(usedPercent) +
        "% " +
        htmlText(UI_USED_OF) +
        " " +
        String(
            (unsigned long)(
                totalBytes /
                1024ULL /
                1024ULL
            )
        ) +
        " MB</div><div class='progress'><span style='width:" +
        String(usedPercent) +
        "%'></span></div></section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_NETWORK) +
        "</div><div class='card-value'>" +
        htmlEscape(networkText) +
        "</div><div class='card-note'>" +
        htmlText(UI_HOSTNAME) +
        ": " +
        htmlEscape(cfg_hostname) +
        ".local</div></section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_CONFIGURATION) +
        "</div><div class='card-value'>" +
        htmlEscape(String(configSourceUiName())) +
        "</div><div class='card-note'>SD: " +
        htmlEscape(String(configSdStatusUiName())) +
        " &middot; " +
        htmlText(UI_INTERNAL_SHADOW) +
        ": " +
        htmlText(
            configInternalValid()
            ? UI_VALID
            : UI_INVALID
        ) +
        "</div></section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_PLATFORM) +
        "</div><div class='card-value'>" SENSORFORGE_FABRIC_LITERAL "</div><div class='card-note'>" +
        htmlText(UI_NODE) +
        ": " +
        htmlEscape(cfg_hostname) +
        "<br>" +
        htmlText(UI_ROLE) +
        ": " SENSORFORGE_PLATFORM_LITERAL
        "<br>Core: v" SENSORFORGE_CORE_VERSION_LITERAL
        "<br>" +
        htmlText(UI_BUILD) +
        ": " +
        htmlEscape(firmwareBuildTimestamp()) +
        "</div></section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_FIRMWARE) +
        "</div><div class='card-value'>" +
        htmlEscape(firmwareBuildTimestamp()) +
        "</div><div class='card-note'>" +
        htmlText(UI_INSTALLED) +
        ": " +
        htmlEscape(firmwareInstallTimestamp()) +
        "<br>" +
        htmlText(UI_SOURCE) +
        ": " +
        htmlEscape(firmwareInstallSource()) +
        "</div></section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_RTC) +
        "</div>";

    if (rtcDetected()) {
        html +=
            "<div class='card-value'>" +
            htmlEscape(String(rtcTypeName())) +
            "</div><div class='card-note'><span class='status-pill " +
            String(rtcClockValid() ? "ok" : "warn") +
            "'>" +
            htmlText(
                rtcClockValid()
                ? UI_RTC_OK
                : UI_CHECK_TIME
            ) +
            "</span><br>" +
            htmlEscape(rtcTimeText()) +
            "</div>";
    } else {
        html +=
            "<div class='card-value'>" +
            htmlText(UI_NOT_DETECTED) +
            "</div><div class='card-note'><span class='status-pill'>" +
            htmlText(UI_OPTIONAL) +
            "</span><br>" +
            htmlText(UI_RTC_ABSENT_NOTE) +
            "</div>";
    }

    html +=
        "</section>";

    html +=
        "<section class='dash-card'><div class='card-label'>" +
        htmlText(UI_CARD_TEMPERATURE) +
        "</div><div id='cpuTempCard' class='card-value'>" +
        String(
            cpuTempValid
            ? String(cpuTempC, 1) + " °C"
            : String("--")
        ) +
        "</div><div class='card-note'>" +
        htmlText(UI_ESP32_CHIP_TEMPERATURE) +
        "<br>" +
        htmlText(UI_RTC_ENCLOSURE_INDICATOR) +
        ": <span id='rtcTempCard'>" +
        String(
            rtcTempValid
            ? String(rtcTempC, 1) + " °C"
            : String("--")
        ) +
        "</span><br>" +
        htmlText(UI_PROTECTION_STATUS) +
        ": <b><span id='thermalStateCard'>" +
        htmlEscape(thermalUiState) +
        "</span></b><br><b>CPU:</b> " +
        htmlText(UI_WARNING_FROM) +
        " " +
        String(SENSORFORGE_THERMAL_WARNING_C, 0) +
        " °C, " +
        htmlText(UI_EMERGENCY_FROM) +
        " " +
        String(SENSORFORGE_THERMAL_EMERGENCY_C, 0) +
        " °C " +
        htmlText(UI_AFTER) +
        " " +
        String((unsigned)SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES) +
        " " +
        htmlText(UI_SAMPLES) +
        ", " +
        htmlText(UI_RECOVERY_BELOW) +
        " " +
        String(SENSORFORGE_THERMAL_RECOVERY_C, 0) +
        " °C.<br><b>" +
        htmlText(UI_RTC_ENCLOSURE_INDICATOR) +
        ":</b> " +
        htmlText(UI_WARNING_FROM) +
        " " +
        String(SENSORFORGE_THERMAL_RTC_WARNING_C, 0) +
        " °C, " +
        htmlText(UI_EMERGENCY_FROM) +
        " " +
        String(SENSORFORGE_THERMAL_RTC_EMERGENCY_C, 0) +
        " °C " +
        htmlText(UI_AFTER) +
        " " +
        String((unsigned)SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES) +
        " " +
        htmlText(UI_SAMPLES) +
        ", " +
        htmlText(UI_RECOVERY_BELOW) +
        " " +
        String(SENSORFORGE_THERMAL_RTC_RECOVERY_C, 0) +
        " °C.<br>" +
        htmlText(UI_RTC_TEMP_NOTE) +
        "<br>" +
        htmlText(UI_COOLDOWN) +
        ": " +
        String((unsigned long)(SENSORFORGE_THERMAL_COOLDOWN_SECONDS / 60UL)) +
        " " +
        htmlText(UI_MIN_SHORT) +
        ". " +
        htmlText(UI_THERMAL_EMERGENCY_ACTION) +
        "</div></section>";

    html +=
        "</div>";

    UiTextId automationState =
        recordingPaused
        ? UI_STATUS_PAUSED
        : (
            recordingSafetyCooldown
            ? UI_STATUS_SAFETY_PAUSE
            : (
                recordingArmed
                ? UI_STATUS_ACTIVE
                : UI_STATUS_TIME_LOCK
            )
        );

    UiTextId automationNote =
        recordingPaused
        ? UI_REC_NOTE_PAUSED
        : (
            recordingSafetyCooldown
            ? UI_REC_NOTE_SAFETY
            : (
                recordingArmed
                ? UI_REC_NOTE_ARMED
                : UI_REC_NOTE_TIMELOCK
            )
        );

    html +=
        "<section id='recordingControl' class='recording-control" +
        String(recordingPaused ? " paused" : "") +
        "'><div class='recording-control-copy'><div class='recording-control-title'>" +
        htmlText(UI_RECORDING_AUTOMATION) +
        " &nbsp;<span id='recordingAutomationPill' class='status-pill " +
        String(recordingPaused || recordingSafetyCooldown || !recordingArmed ? "warn" : "ok") +
        "'>" +
        htmlText(automationState) +
        "</span></div><div id='recordingAutomationNote' class='recording-control-note'>" +
        htmlText(automationNote);

    if (recordingPaused || recordingArmed) {
        html +=
            "<br><b>" +
            htmlText(UI_SAFETY_FUNCTION) +
            "</b> " +
            htmlText(UI_PAUSE_AUTO_RELEASE);
    } else if (!recordingSafetyCooldown) {
        html +=
            "<br><b>" +
            htmlText(UI_SAFETY_FUNCTION) +
            "</b> " +
            htmlText(UI_MANUAL_PAUSE_AVAILABLE);
    }

    html +=
        "</div></div><div class='recording-control-actions'>"
        "<button id='recordingPauseButton' type='button'>" +
        htmlText(
            recordingPaused
            ? UI_RESUME_RECORDING
            : UI_PAUSE_RECORDING
        ) +
        "</button></div></section>";

    // Localized browser strings for the dynamic dashboard. Values are carried
    // in HTML data attributes so the JSON/status protocol remains unchanged.
    html +=
        "<div id='dashboardI18n' hidden"
        " data-status-running='" + htmlText(UI_STATUS_RECORDING_RUNNING) + "'"
        " data-status-paused='" + htmlText(UI_STATUS_RECORDING_PAUSED) + "'"
        " data-status-safety='" + htmlText(UI_STATUS_SAFETY_PAUSE) + "'"
        " data-status-not-armed='" + htmlText(UI_STATUS_NOT_ARMED) + "'"
        " data-status-ready='" + htmlText(UI_STATUS_READY) + "'"
        " data-value-safety='" + htmlText(UI_VALUE_SAFETY_PAUSE) + "'"
        " data-value-not-armed='" + htmlText(UI_VALUE_NOT_ARMED) + "'"
        " data-active='" + htmlText(UI_STATUS_ACTIVE) + "'"
        " data-paused='" + htmlText(UI_STATUS_PAUSED) + "'"
        " data-time-lock='" + htmlText(UI_STATUS_TIME_LOCK) + "'"
        " data-note-paused='" + htmlText(UI_REC_NOTE_PAUSED) + "'"
        " data-note-safety='" + htmlText(UI_REC_NOTE_SAFETY) + "'"
        " data-note-armed='" + htmlText(UI_REC_NOTE_ARMED) + "'"
        " data-note-time-lock='" + htmlText(UI_REC_NOTE_TIMELOCK) + "'"
        " data-safety-function='" + htmlText(UI_SAFETY_FUNCTION) + "'"
        " data-auto-release='" + htmlText(UI_PAUSE_AUTO_RELEASE) + "'"
        " data-manual-pause='" + htmlText(UI_MANUAL_PAUSE_AVAILABLE) + "'"
        " data-resume='" + htmlText(UI_RESUME_RECORDING) + "'"
        " data-pause='" + htmlText(UI_PAUSE_RECORDING) + "'"
        " data-activating='" + htmlText(UI_ACTIVATING) + "'"
        " data-pausing='" + htmlText(UI_PAUSING) + "'"
        " data-change-failed='" + htmlText(UI_RECORDING_CHANGE_FAILED) + "'"
        " data-camera-arming='" + htmlText(UI_ARM_CAMERA_ARMING) + "'"
        " data-camera-not-armed='" + htmlText(UI_ARM_CAMERA_NOT_ARMED) + "'"
        " data-camera-armed='" + htmlText(UI_ARM_CAMERA_ARMED) + "'"
        " data-arm-on='" + htmlText(UI_ARM_ON_PREFIX) + "'"
        " data-arm-in='" + htmlText(UI_ARM_IN_PREFIX) + "'"
        " data-arm-scheduled='" + htmlText(UI_ARM_SCHEDULED) + "'"
        " data-arm-clock-invalid='" + htmlText(UI_ARM_CLOCK_INVALID) + "'"
        " data-arm-config-invalid='" + htmlText(UI_ARM_CONFIG_INVALID) + "'"
        " data-arm-check-config='" + htmlText(UI_ARM_CHECK_CONFIG) + "'"
        " data-arm-release-time='" + htmlText(UI_ARM_RELEASE_TIME) + "'"
        " data-arm-no-lock='" + htmlText(UI_ARM_NO_TIME_LOCK) + "'"
        " data-arm-enabled='" + htmlText(UI_ARM_AUTOMATION_RELEASED) + "'"
        " data-new-recordings-in='" + htmlText(UI_SAFETY_NEW_IN) + "'"
        " data-day-one='" + htmlText(UI_DAY_SINGULAR) + "'"
        " data-day-many='" + htmlText(UI_DAY_PLURAL) + "'"
        " data-hours='" + htmlText(UI_HOURS) + "'"
        " data-minutes='" + htmlText(UI_MINUTES) + "'"
        " data-seconds='" + htmlText(UI_SECONDS_UNIT) + "'"
        " data-thermal-ok='" + htmlText(UI_THERMAL_STATE_OK) + "'"
        " data-thermal-warning='" + htmlText(UI_THERMAL_STATE_WARNING) + "'"
        " data-thermal-emergency='" + htmlText(UI_THERMAL_STATE_EMERGENCY) + "'"
        "></div>";

    html +=
        "<script>"
        "(function(){"
        "var i=document.getElementById('dashboardI18n').dataset;"
        "var pill=document.getElementById('recordingStatusPill');"
        "var value=document.getElementById('recordingCardValue');"
        "var panel=document.getElementById('recordingControl');"
        "var autoPill=document.getElementById('recordingAutomationPill');"
        "var note=document.getElementById('recordingAutomationNote');"
        "var btn=document.getElementById('recordingPauseButton');"
        "var globalPill=document.getElementById('recordingPauseGlobal');"
        "var cpuCard=document.getElementById('cpuTempCard');"
        "var rtcCard=document.getElementById('rtcTempCard');"
        "var thermalCard=document.getElementById('thermalStateCard');"
        "var armBanner=document.getElementById('armingBanner');"
        "var armTitleEl=document.getElementById('armingTitle');"
        "var armDeadlineEl=document.getElementById('armingDeadline');"
        "var armCountdownEl=document.getElementById('armingCountdown');"
        "var safetyBanner=document.getElementById('recordingSafetyBanner');"
        "var safetyCountdownEl=document.getElementById('recordingSafetyCountdown');"
        "var paused=false,safetyCooldown=false,safetyRemaining=0,armState='off',armRemaining=0,armDeadlineText='';"
        "function armDuration(sec){"
            "sec=Math.max(0,Math.floor(Number(sec)||0));"
            "var d=Math.floor(sec/86400);sec%=86400;"
            "var h=Math.floor(sec/3600);sec%=3600;"
            "var m=Math.floor(sec/60);var s=sec%60;"
            "var p=function(v){return v<10?'0'+v:String(v);};"
            "return (d>0?(d+' '+(d===1?i.dayOne:i.dayMany)+' '):'')+p(h)+' '+i.hours+' '+p(m)+' '+i.minutes+' '+p(s)+' '+i.seconds;"
        "}"
        "function renderArm(){"
            "if(!armBanner)return;"
            "if(armState==='waiting'){"
                "armBanner.style.borderColor='#d97706';armBanner.style.background='#fff7e8';"
                "if(armTitleEl)armTitleEl.textContent=i.cameraArming;"
                "if(armDeadlineEl)armDeadlineEl.textContent=i.armOn+' '+armDeadlineText;"
                "if(armCountdownEl)armCountdownEl.textContent=i.armIn+' '+armDuration(armRemaining);"
            "}else if(armState==='clock_invalid'){"
                "armBanner.style.borderColor='#b42318';armBanner.style.background='#fff1f0';"
                "if(armTitleEl)armTitleEl.textContent=i.cameraNotArmed;"
                "if(armDeadlineEl)armDeadlineEl.textContent=i.armScheduled+': '+armDeadlineText;"
                "if(armCountdownEl)armCountdownEl.textContent=i.armClockInvalid;"
            "}else if(armState==='invalid'){"
                "armBanner.style.borderColor='#b42318';armBanner.style.background='#fff1f0';"
                "if(armTitleEl)armTitleEl.textContent=i.cameraNotArmed;"
                "if(armDeadlineEl)armDeadlineEl.textContent=i.armConfigInvalid;"
                "if(armCountdownEl)armCountdownEl.textContent=i.armCheckConfig;"
            "}else{"
                "armBanner.style.borderColor='#087a00';armBanner.style.background='#eef9f0';"
                "if(armTitleEl)armTitleEl.textContent=i.cameraArmed;"
                "if(armDeadlineEl)armDeadlineEl.textContent=armState==='reached'?(i.armReleaseTime+': '+armDeadlineText):i.armNoLock;"
                "if(armCountdownEl)armCountdownEl.textContent=i.armEnabled;"
            "}"
        "}"
        "function renderSafety(){"
            "if(!safetyBanner)return;"
            "safetyBanner.style.display=safetyCooldown?'':'none';"
            "if(safetyCountdownEl&&safetyCooldown)safetyCountdownEl.textContent=i.newRecordingsIn+' '+armDuration(safetyRemaining);"
        "}"
        "function setNote(text,tail){if(!note)return;note.textContent='';note.appendChild(document.createTextNode(text));"
            "if(tail){note.appendChild(document.createElement('br'));var b=document.createElement('b');b.textContent=i.safetyFunction+' ';note.appendChild(b);note.appendChild(document.createTextNode(tail));}}"
        "function apply(s){"
            "var active=!!s.recording;var armed=!!s.recording_armed;"
            "paused=!!s.recording_paused;safetyCooldown=!!s.recording_safety_cooldown;"
            "safetyRemaining=Math.max(0,Number(s.recording_safety_cooldown_remaining_sec)||0);"
            "armState=s.recording_arm_state||'off';armRemaining=Math.max(0,Number(s.recording_arm_remaining_sec)||0);"
            "armDeadlineText=s.recording_not_before_display||'';renderArm();renderSafety();"
            "if(pill){if(active){pill.textContent=i.statusRunning;pill.className='status-pill danger';}"
                "else if(paused){pill.textContent=i.statusPaused;pill.className='status-pill warn';}"
                "else if(safetyCooldown){pill.textContent=i.statusSafety;pill.className='status-pill warn';}"
                "else if(!armed){pill.textContent=i.statusNotArmed;pill.className='status-pill warn';}"
                "else{pill.textContent=i.statusReady;pill.className='status-pill ok';}}"
            "if(value)value.textContent=active?i.active:(paused?i.paused:(safetyCooldown?i.valueSafety:(armed?i.statusReady:i.valueNotArmed)));"
            "if(panel)panel.classList.toggle('paused',paused);"
            "if(autoPill){autoPill.textContent=paused?i.paused:(safetyCooldown?i.statusSafety:(armed?i.active:i.timeLock));"
                "autoPill.className='status-pill '+(paused||safetyCooldown||!armed?'warn':'ok');}"
            "if(paused)setNote(i.notePaused,i.autoRelease);else if(safetyCooldown)setNote(i.noteSafety,'');"
                "else if(!armed)setNote(i.noteTimeLock,i.manualPause);else setNote(i.noteArmed,i.autoRelease);"
            "if(btn){btn.textContent=paused?i.resume:i.pause;btn.disabled=false;}"
            "if(globalPill){globalPill.textContent=paused?globalPill.dataset.off:globalPill.dataset.on;"
                "globalPill.classList.toggle('off',paused);globalPill.classList.toggle('on',!paused);}"
            "if(cpuCard)cpuCard.textContent=s.cpu_temp_valid?(Number(s.cpu_temp_c).toFixed(1)+' °C'):'--';"
            "if(rtcCard)rtcCard.textContent=s.rtc_temp_valid?(Number(s.rtc_temp_c).toFixed(1)+' °C'):'--';"
            "if(cpuCard){var ct=Number(s.cpu_temp_c);cpuCard.style.color=s.cpu_temp_valid&&ct>=Number(s.thermal_emergency_c)?'#b42318':"
                "(s.cpu_temp_valid&&ct>=Number(s.thermal_warning_c)?'#9a5a00':'');}"
            "if(rtcCard){var rt=Number(s.rtc_temp_c);rtcCard.style.color=s.rtc_temp_valid&&rt>=Number(s.thermal_rtc_emergency_c)?'#b42318':"
                "(s.rtc_temp_valid&&rt>=Number(s.thermal_rtc_warning_c)?'#9a5a00':'');}"
            "if(thermalCard){var label=s.thermal_state==='WARNING'?i.thermalWarning:(s.thermal_state==='EMERGENCY'?i.thermalEmergency:i.thermalOk);"
                "var src=(s.thermal_source&&s.thermal_source!=='NONE')?(' ('+s.thermal_source+')'):'';thermalCard.textContent=label+src;}"
        "}"
        "function poll(){fetch('/ui_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
            ".then(function(r){if(!r.ok)throw new Error();return r.json();}).then(apply).catch(function(){});}"
        "if(btn)btn.addEventListener('click',function(){btn.disabled=true;btn.textContent=paused?i.activating:i.pausing;"
            "fetch('/recording_pause',{method:'POST',cache:'no-store',credentials:'same-origin',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+(paused?'resume':'pause')})"
            ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
            ".then(apply).catch(function(e){alert(i.changeFailed+': '+e.message);poll();});});"
        "poll();setInterval(poll,2000);"
        "setInterval(function(){if(armState==='waiting'&&armRemaining>0){armRemaining--;if(armRemaining<=0)armState='reached';renderArm();}"
            "if(safetyCooldown&&safetyRemaining>0){safetyRemaining--;if(safetyRemaining<=0)safetyCooldown=false;renderSafety();}},1000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});"
        "})();"
        "</script>";

    html +=
        "<script>"
        "(function(){"
        "var c=document.getElementById('motionLiveConfig');if(!c)return;"
        "var value=document.getElementById('motionCardValue');"
        "var pState=document.getElementById('motionPresenceState');"
        "var radarRow=document.getElementById('motionRadarRow');"
        "var radarLastRow=document.getElementById('motionRadarLastRow');"
        "var radarState=document.getElementById('motionRadarState');"
        "var radarGate=document.getElementById('motionRadarGate');"
        "var radarEnergy=document.getElementById('motionRadarEnergy');"
        "var radarLastAge=document.getElementById('motionRadarLastTriggerAge');"
        "var pirLastRow=document.getElementById('motionPirLastTriggerRow');"
        "var simState=document.getElementById('motionSimulationState');"
        "var lastEl=document.getElementById('motionLastTrigger');"
        "var countEl=document.getElementById('motionTriggerCount');"
        "var imageState=document.getElementById('motionImageState');"
        "var imageLast=document.getElementById('motionImageLast');"
        "var imageLastRow=document.getElementById('motionImageLastRow');"
        "var basePresence=Number(c.dataset.basePresence)||0;"
        "var baseRadar=Number(c.dataset.baseRadar)||0;"
        "function ageText(valid,ms){if(!valid)return c.dataset.noTrigger;ms=Math.max(0,Number(ms)||0);"
            "if(ms<10000)return c.dataset.ago+(c.dataset.ago?' ':'')+(ms/1000).toFixed(1)+' s'+c.dataset.agoSuffix;"
            "if(ms<60000)return c.dataset.ago+(c.dataset.ago?' ':'')+Math.floor(ms/1000)+' s'+c.dataset.agoSuffix;"
            "var sec=Math.floor(ms/1000);var min=Math.floor(sec/60);sec%=60;return c.dataset.ago+(c.dataset.ago?' ':'')+min+' min '+sec+' s'+c.dataset.agoSuffix;}"
        "function imageAgeText(valid,ms){if(!valid)return c.dataset.imageNever;return ageText(true,ms);}"
        "function imageStateText(mode,d){if(mode==='direct')return c.dataset.imageInactive;if(!d||!d.last_analysis_valid||Number(d.last_analysis_age_ms)>2500)return c.dataset.imageWaiting;if(d.motion_active||d.image_motion_state==='confirmed')return c.dataset.imageDetected;if(d.image_motion_state==='background_init')return c.dataset.imageLearning;if(d.image_motion_state==='global_change')return c.dataset.imageLight;if(d.image_motion_state==='error')return c.dataset.imageError;return c.dataset.imageNone;}"
        "function applyMotion(s){"
            "if(value)value.textContent=s.motion_active?c.dataset.detected:c.dataset.none;"
            "if(pState){pState.textContent=s.presence_active?'HIGH':'LOW';"
                "pState.classList.remove('danger','warn','ok');"
                "if(!s.presence_active)pState.classList.add('ok');"
                "else if(s.radar_detected&&!s.radar_motion_active)pState.classList.add('warn');"
                "else pState.classList.add('danger');}"
            "if(simState)simState.textContent=s.simulated_motion?'1':'0';"
            "if(radarRow)radarRow.style.display=s.radar_detected?'':'none';"
            "if(radarLastRow)radarLastRow.style.display=s.radar_detected?'':'none';"
            "if(pirLastRow)pirLastRow.style.display=s.radar_detected?'none':'';"
            "if(radarState)radarState.textContent=s.radar_tracking_available?(s.radar_motion_active?c.dataset.radarActive:c.dataset.radarNone):c.dataset.fallback;"
            "if(radarGate)radarGate.textContent=Number(s.radar_last_gate)>=0?String(Number(s.radar_last_gate)):'-';"
            "if(radarEnergy)radarEnergy.textContent=Number(s.radar_last_gate)>=0?Number(s.radar_last_energy_db).toFixed(1):'-';"
            "if(radarLastAge)radarLastAge.textContent=ageText(!!s.radar_last_trigger_valid,s.radar_last_trigger_age_ms);"
            "if(lastEl)lastEl.textContent=ageText(!!s.primary_last_trigger_valid,s.primary_last_trigger_age_ms);"
            "if(countEl){var base=s.primary_source==='radar'?baseRadar:basePresence;countEl.textContent=String(Math.max(0,(Number(s.primary_trigger_count)||0)-base));}"
            "var im=s.image_motion||{};var imMode=s.image_motion_mode||'direct';"
            "if(imageState){imageState.textContent=imageStateText(imMode,im);imageState.classList.remove('danger','warn','ok');if(imMode==='direct')imageState.classList.add('ok');else if(im.motion_active||im.image_motion_state==='confirmed')imageState.classList.add('danger');else if(!im.last_analysis_valid||Number(im.last_analysis_age_ms)>2500||im.image_motion_state==='background_init'||im.image_motion_state==='global_change')imageState.classList.add('warn');else if(im.image_motion_state==='error')imageState.classList.add('danger');else imageState.classList.add('ok');}"
            "if(imageLastRow)imageLastRow.style.display=imMode==='direct'?'none':'';"
            "if(imageLast)imageLast.textContent=imageAgeText(!!im.last_detection_valid,im.last_detection_age_ms);"
        "}"
        "function pollMotion(){if(document.hidden)return;fetch('/motion_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
            ".then(function(r){if(!r.ok)throw new Error();return r.json();}).then(applyMotion).catch(function(){});}"
        "pollMotion();setInterval(pollMotion,250);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)pollMotion();});"
        "})();"
        "</script>";

    html +=
        "<div class='quick-actions'>"
        "<a class='button primary' href='/files'>" +
        htmlText(UI_NAV_RECORDINGS) +
        "</a><a class='button' href='/preview'>" +
        htmlText(UI_NAV_LIVE_PREVIEW) +
        "</a>";

    if (radarConfigAvailable()) {
        html +=
            "<a class='button' href='/radar_config'>" +
            htmlText(UI_RADAR) +
            "</a>";
    } else {
        html +=
            "<span class='button disabled' title='" +
            htmlText(UI_RADAR_CONFIG_PIR_NOTE) +
            "'>" +
            htmlText(UI_RADAR) +
            " &middot; " +
            htmlText(UI_MOTION_SENSOR_PIR) +
            "</span>";
    }

    html +=
        "<a class='button' href='/config'>" +
        htmlText(UI_NAV_CONFIGURATION) +
        "</a><a class='button' href='/transport'>" +
        htmlText(UI_NAV_TRANSPORT) +
        "</a></div>";

    html +=
        "<section id='simulation' class='settings-section'><h3>" +
        htmlText(UI_NAV_SIMULATE_MOTION) +
        "</h3><p class='muted'>" +
        htmlText(UI_SIM_DESCRIPTION) +
        "</p><form method='POST' action='/simulate_motion'>" +
        htmlText(UI_DURATION) +
        ": <input name='seconds' type='number' min='1' max='3600' value='" +
        String(simulationDurationSeconds) +
        "' style='width:90px;'> " +
        htmlText(UI_SECONDS) +
        " <button type='submit'>" +
        htmlText(UI_START_SIMULATION) +
        "</button>";

    if (webConfigMotionActive()) {
        uint32_t remainingSeconds =
            (
                webConfigMotionRemainingMs() +
                999UL
            ) /
            1000UL;

        html +=
            "<br><span class='status-pill warn'>" +
            htmlText(UI_SIM_ACTIVE_REMAINING) +
            " " +
            String(remainingSeconds) +
            " s</span>";
    }

    html +=
        "</form></section>";

    html +=
        htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}



static const size_t WEB_CONFIG_UPLOAD_MAX_BYTES =
    32U * 1024U;

static String webConfigUploadText;
static String webConfigUploadError;
static bool webConfigUploadAttempted = false;
static bool webConfigUploadSucceeded = false;
static ConfigSaveResult webConfigUploadSaveResult =
    CONFIG_SAVE_INTERNAL_FAILED;


static void webConfigResetUploadState()
{
    webConfigUploadText = "";
    webConfigUploadError = "";
    webConfigUploadAttempted = false;
    webConfigUploadSucceeded = false;
    webConfigUploadSaveResult =
        CONFIG_SAVE_INTERNAL_FAILED;
}


static void handleConfigDownload()
{
    String text;
    String error;

    if (!configReadInternalText(
            text,
            error
        )) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Internal config.txt unavailable")
        );
        return;
    }

    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"config.txt\""
    );
    server.sendHeader(
        "Cache-Control",
        "no-store"
    );
    server.sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );

    server.send(
        200,
        "text/plain; charset=utf-8",
        text
    );
}


static void handleConfigUploadData()
{
    HTTPUpload &upload =
        server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        webConfigResetUploadState();
        webConfigUploadAttempted = true;

        // Multipart callbacks may be invoked while the request body is still
        // being parsed. Protect the upload independently of route middleware.
        if (
            cfg_web_auth_enabled &&
            !server.authenticate(
                cfg_web_username.c_str(),
                cfg_web_password.c_str()
            )
        ) {
            webConfigUploadError =
                "Authentication required for config upload";
            return;
        }

        if (recorderIsOpen()) {
            webConfigUploadError =
                "Recording active - config upload is temporarily unavailable";
            return;
        }

        if (g_storageLocked) {
            webConfigUploadError =
                "Storage is currently locked by another operation";
            return;
        }

        String lowerName =
            upload.filename;
        lowerName.toLowerCase();

        if (!lowerName.endsWith(".txt")) {
            webConfigUploadError =
                "Please select a .txt configuration file";
            return;
        }

        webConfigUploadText.reserve(
            4096
        );
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (webConfigUploadError.length())
            return;

        if (
            webConfigUploadText.length() +
            upload.currentSize >
            WEB_CONFIG_UPLOAD_MAX_BYTES
        ) {
            webConfigUploadError =
                "config.txt is too large (maximum 32 KiB)";
            return;
        }

        if (!webConfigUploadText.concat(
                (const char *)upload.buf,
                upload.currentSize
            )) {
            webConfigUploadError =
                "Not enough memory for config upload";
        }
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        if (webConfigUploadError.length())
            return;

        if (!webConfigUploadText.length()) {
            webConfigUploadError =
                "Uploaded config.txt is empty";
            return;
        }

        String validationError;

        if (!configValidateText(
                webConfigUploadText,
                validationError
            )) {
            webConfigUploadError =
                "Config validation failed: " +
                validationError;
            return;
        }

        // Refresh immediately before saving. The existence of SD /config.txt
        // is the complete synchronization policy: present = internal + SD,
        // absent = internal only. configSaveText enforces the same rule again.
        configRefreshSdStatus();

        String saveError;

        webConfigUploadSaveResult =
            configSaveText(
                webConfigUploadText,
                configSdAvailable() &&
                    configSdPresent(),
                saveError
            );

        if (
            webConfigUploadSaveResult !=
                CONFIG_SAVE_BOTH &&
            webConfigUploadSaveResult !=
                CONFIG_SAVE_INTERNAL_ONLY
        ) {
            webConfigUploadError =
                saveError.length()
                ? saveError
                : String("Config upload could not be saved");
            return;
        }

        webConfigUploadSucceeded =
            true;

        logWrite(
            String("Config uploaded from WebConfig | storage=") +
            (
                webConfigUploadSaveResult ==
                    CONFIG_SAVE_BOTH
                ? "internal+SD"
                : "internal-only"
            )
        );

        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        webConfigUploadError =
            "Config upload was interrupted or aborted";
    }
}


static void handleConfigUploadFinished()
{
    bool success =
        webConfigUploadAttempted &&
        webConfigUploadSucceeded;

    ConfigSaveResult result =
        webConfigUploadSaveResult;

    String error =
        webConfigUploadError;

    webConfigResetUploadState();

    if (!success) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("No valid config file was received")
        );
        return;
    }

    server.sendHeader(
        "Location",
        result == CONFIG_SAVE_BOTH
        ? "/config?notice=config_upload_both"
        : "/config?notice=config_upload_internal"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleConfigSdDelete()
{
    if (rejectWhileRecording("SD config delete"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    if (!configDeleteSdCopy(
            error
        )) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("SD config.txt could not be removed")
        );
        return;
    }

    logWrite(
        "SD config.txt deleted from WebConfig | policy=internal-only"
    );

    server.sendHeader(
        "Location",
        "/config?notice=sd_config_deleted"
    );
    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleConfigSdCopy()
{
    if (rejectWhileRecording("SD config copy"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    if (!configCopyInternalToSd(
            error
        )) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Internal config.txt could not be copied to SD")
        );
        return;
    }

    logWrite(
        "Internal config.txt copied to SD from WebConfig | policy=internal+SD"
    );

    server.sendHeader(
        "Location",
        "/config?notice=sd_config_copied"
    );
    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleConfigFactoryReset()
{
    if (rejectWhileRecording("factory config reset"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    // Re-read the physical SD-config state immediately before saving. Existing
    // SD /config.txt must be reset together with the internal copy; otherwise
    // it would win again on the next boot. A card without /config.txt remains
    // internal-only by the established SensorForge config policy.
    configRefreshSdStatus();

    String error;
    ConfigSaveResult result =
        configResetToFactoryDefaults(
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Factory defaults could not be saved")
        );
        return;
    }

    String factorySsid =
        configDefaultHostname();

    consoleWrite(
        "CONFIG",
        "Factory defaults saved - reboot scheduled"
    );

    logWrite(
        String("Factory config reset from WebConfig | storage=") +
        (
            result == CONFIG_SAVE_BOTH
            ? "internal+SD"
            : "internal-only"
        ) +
        " | default_ap=" +
        factorySsid
    );

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div><h2>Werkseinstellungen gespeichert</h2>"
        "<p>Die Konfiguration wurde vollständig auf die Firmware-Standardwerte zurückgesetzt.</p></div></div>";

    html +=
        "<section class='settings-section' style='border-left:5px solid #d97706'>"
        "<h3>Neustart</h3>"
        "<p>SensorForge startet in wenigen Sekunden neu.</p>"
        "<p><b>Hotspot danach:</b> <code>" +
        htmlEscape(factorySsid) +
        "</code><br><b>Passwort:</b> keines (offener Hotspot)<br>"
        "<b>WiFi-Timeout:</b> aus (Hotspot bleibt aktiv)</p>";

    html +=
        result == CONFIG_SAVE_BOTH
        ? "<p class='muted'>Die Werkseinstellungen wurden intern und in der vorhandenen SD-config.txt gespeichert.</p>"
        : "<p class='muted'>Die Werkseinstellungen wurden intern gespeichert. Es wurde keine neue SD-config.txt erzeugt.</p>";

    html +=
        "<p class='muted'>Aufnahmen, Lizenz, Hardware-/Kryptoschlüssel, Firmware und sonstige Mediendaten wurden nicht gelöscht.</p>"
        "</section>";

    html += htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );

    rebootScheduled = true;
    rebootAtMs = millis() + 3500UL;
}


static void handleConfig()
{
    // SD contents may have changed since boot (wipe/card swap).
    configRefreshSdStatus();

    String html = htmlHeader();

    String configNotice =
        server.arg("notice");

    html += "<div class='page-title'><div><h2>Konfiguration</h2>"
            "<p>Geräteeinstellungen, Aufnahme, WLAN und Speicher.</p></div></div>";

    html += "<p style='padding:10px;background:#f7f7f7;border-radius:6px;'>"
            "<b>Release:</b> " +
            htmlEscape(String(SENSORFORGE_RELEASE_TAG)) +
            " (" + htmlEscape(String(SENSORFORGE_RELEASE_DATE)) + ")" +
            "<br><b>Firmware Build:</b> " +
            htmlEscape(firmwareBuildTimestamp()) +
            "<br><b>Installiert:</b> " +
            htmlEscape(firmwareInstallTimestamp()) +
            "<br><b>Quelle:</b> " +
            htmlEscape(firmwareInstallSource()) +
            "<br><span class='muted'>Git-Tag für diesen Programstand: " +
            htmlEscape(String(SENSORFORGE_RELEASE_TAG)) +
            "</span></p>";

    if (configNotice.length()) {
        html +=
            "<div id='configNotice' class='flash-notice'>";

        if (configNotice == "config_upload_both") {
            html +=
                "<strong>config.txt hochgeladen</strong>"
                "<span class='muted'>Die Datei wurde validiert und intern sowie auf der bereits vorhandenen "
                "SD-config.txt gespeichert. Ein Neustart ist erforderlich, damit alle Einstellungen vollständig aktiv werden.</span>";
        } else if (configNotice == "config_upload_internal") {
            html +=
                "<strong>config.txt hochgeladen</strong>"
                "<span class='muted'>Die Datei wurde validiert und ausschließlich im internen LittleFS gespeichert. "
                "Auf der SD-Karte wurde keine config.txt erzeugt. Ein Neustart ist erforderlich, damit alle Einstellungen vollständig aktiv werden.</span>";
        } else if (configNotice == "sd_config_deleted") {
            html +=
                "<strong>SD-config.txt entfernt</strong>"
                "<span class='muted'>Die gültige interne config.txt bleibt erhalten. SensorForge arbeitet ab jetzt im Internal-only-Modus; "
                "normale Saves, Transportänderungen und SD-Wartung erzeugen keine neue SD-config.txt.</span>";
        } else if (configNotice == "sd_config_copied") {
            html +=
                "<strong>Interne config.txt auf SD kopiert</strong>"
                "<span class='muted'>SD-Synchronisierung ist wieder aktiv. Solange /config.txt auf der SD vorhanden ist, "
                "werden zukünftige Konfigurationsänderungen intern und auf SD gespeichert.</span>";
        } else {
            html +=
                "<strong>Konfiguration aktualisiert</strong>";
        }

        html +=
            "</div>"
            "<script>history.replaceState(null,'','/config');</script>";
    }

    html += "<p><b>Config source:</b> " +
            htmlEscape(String(configSourceName())) +
            "<br><b>SD config:</b> " +
            htmlEscape(String(configSdStatusName())) +
            "<br><b>Internal shadow:</b> " +
            String(
                configInternalValid()
                ? "valid"
                : (
                    configInternalAvailable()
                    ? "missing / invalid"
                    : "unavailable"
                )
            ) +
            "</p>";


    ConfigCopyCompareResult copyCompare =
        compareConfigCopies();


    if (
        copyCompare ==
        CONFIG_COPY_COMPARE_DIFFERENT
    ) {

        html +=
            "<p style='padding:10px;border:2px solid #c00;background:#fff3f3;color:#900;'>"
            "<b>WARNUNG:</b> SD <code>/config.txt</code> und interner "
            "LittleFS-Shadow sind nicht identisch. "
            "Beim normalen Neustart hat eine gültige SD-Konfiguration Vorrang."
            "</p>";

    } else if (
        copyCompare ==
        CONFIG_COPY_COMPARE_IDENTICAL
    ) {

        html +=
            "<p style='color:#087a00;'>"
            "<b>Config-Kopien:</b> SD und interner Shadow sind identisch."
            "</p>";

    } else if (
        copyCompare ==
        CONFIG_COPY_COMPARE_READ_ERROR
    ) {

        html +=
            "<p style='color:#9a5a00;'>"
            "<b>Config-Kopien:</b> Vergleich konnte nicht vollständig gelesen werden."
            "</p>";
    }


    bool sdSyncEnabled =
        configSdAvailable() &&
        configSdPresent();

    html +=
        "<section class='settings-section'>"
        "<h3>Config-Datei</h3>";

    if (sdSyncEnabled) {
        html +=
            "<p><span class='status-pill ok'>INTERN + SD</span></p>"
            "<p class='muted'>Auf der SD-Karte ist <code>/config.txt</code> vorhanden. "
            "Damit ist die SD-Synchronisierung aktiv: normale Saves und Uploads aktualisieren "
            "die interne Kopie und die SD-Datei.</p>";
    } else {
        html +=
            "<p><span class='status-pill warn'>INTERNAL ONLY</span></p>"
            "<p class='muted'>Auf der SD-Karte ist keine <code>/config.txt</code> vorhanden. "
            "Damit arbeitet SensorForge ausschließlich mit der internen LittleFS-Konfiguration. "
            "Normale Saves, Uploads und SD-Wartung erzeugen keine SD-config.txt.</p>";
    }

    html +=
        "<p><a class='button' href='/config_download'>Interne config.txt herunterladen</a></p>"
        "<p class='muted'>" +
        htmlEscape(String(tr(UI_CONFIG_SECRETS_DOWNLOAD_NOTE))) +
        "</p>"
        "<form method='POST' action='/config_upload' enctype='multipart/form-data' "
        "onsubmit=\"return confirm('Ausgewählte Konfigurationsdatei validieren und speichern? Die neuen Werte werden nach einem Neustart vollständig aktiv.');\">"
        "<input type='file' name='config_file' accept='.txt,text/plain' required> "
        "<button class='primary' type='submit'>config.txt hochladen</button>"
        "</form>";

    if (configSdAvailable()) {
        if (configSdPresent()) {
            html +=
                "<form method='POST' action='/config_sd_delete' "
                "onsubmit=\"return confirm('SD-config.txt wirklich löschen? Die gültige interne config.txt bleibt erhalten und SensorForge wechselt auf Internal-only.');\">"
                "<button class='danger' type='submit'>SD-config.txt löschen</button>"
                "</form>";
        } else {
            html +=
                "<form method='POST' action='/config_sd_copy' "
                "onsubmit=\"return confirm('Gültige interne config.txt jetzt auf die SD-Karte kopieren und SD-Synchronisierung aktivieren?');\">"
                "<button type='submit'>Interne config.txt auf SD kopieren</button>"
                "</form>";
        }
    } else {
        html +=
            "<p class='muted'>SD-Karte ist momentan nicht verfügbar. Die interne config.txt bleibt davon unberührt.</p>";
    }

    String factoryDefaultSsid =
        configDefaultHostname();

    html +=
        "<hr style='margin:22px 0;border:0;border-top:1px solid #ddd'>"
        "<h4>Werkseinstellungen</h4>"
        "<p class='muted'>Setzt <b>alle Konfigurationswerte</b> auf die aktuellen Firmware-Standards zurück und startet das Gerät neu. "
        "Die interne config.txt wird immer ersetzt; eine bereits vorhandene SD-config.txt wird ebenfalls ersetzt. "
        "Eine neue SD-config.txt wird nicht automatisch erzeugt.</p>"
        "<p class='muted'>Factory-Hotspot nach dem Neustart: <code>" +
        htmlEscape(factoryDefaultSsid) +
        "</code>, offen ohne Passwort, ohne WiFi-Timeout. Aufnahmen, Lizenz, Kryptoschlüssel und Firmware bleiben erhalten.</p>"
        "<form method='POST' action='/config_factory_reset' "
        "onsubmit=\"return confirm('Wirklich ALLE Konfigurationswerte auf Werkseinstellungen zurücksetzen? Eine vorhandene SD-config.txt wird ebenfalls überschrieben. Das Gerät startet anschließend neu.');\">"
        "<button class='danger' type='submit'>Alle Konfigurationswerte auf Werkseinstellungen</button>"
        "</form>";

    html +=
        "</section>";

    html += "<form id='configForm' method='POST' action='/save'>";

    html += "<div class='settings-section'><h3>Kamera</h3>";
    html += "camera: <input name='camera' value='" +
            htmlEscape(cfg_camera) + "'><br>";

    html += "resolution: <input id='cfgResolution' name='resolution' value='" +
            htmlEscape(cfg_resolution) + "'><br>";

    int recordingPerformanceMaxFps =
        configRecordingPerformanceMaxFps(
            cfg_resolution,
            cfg_quality
        );

    if (recordingPerformanceMaxFps < 1)
        recordingPerformanceMaxFps = 30;

    html += "fps: <input id='cfgFps' name='fps' type='number' min='1' max='" +
            String(recordingPerformanceMaxFps) + "' value='" +
            String(cfg_fps) + "'><br>";

    html += "quality: <input id='cfgQuality' name='quality' type='number' min='0' max='63' value='" +
            String(cfg_quality) + "'>"
            " <small>(kleiner = bessere JPEG-Qualität und meist größere Dateien; Referenzwert: 12)</small><br>";

    html +=
        "<small id='recordingPerformanceHint' class='muted'></small><br>";

    html += "<script>(function(){";
    html += "const cap=" +
            String((unsigned long)configRecordingPerformanceLimit()) +
            ";";
    html +=
        "const px={'160x120':19200,'320x240':76800,'640x480':307200,"
        "'800x600':480000,'1024x768':786432,'1280x1024':1310720,"
        "'1600x1200':1920000,'2048x1536':3145728};"
        "const r=document.getElementById('cfgResolution');"
        "const f=document.getElementById('cfgFps');"
        "const q=document.getElementById('cfgQuality');"
        "const h=document.getElementById('recordingPerformanceHint');"
        "function qw(v){v=Math.max(0,Math.min(63,Number(v)||0));"
        "if(v>=12)return 100;return Math.min(160,100+(12-v)*5);}"
        "function update(){"
        "const p=px[(r.value||'').trim()];"
        "if(!cap){f.max=30;h.textContent='Für dieses Board ist noch kein Performance-Oberdeckel qualifiziert.';return;}"
        "if(!p){f.max=30;h.textContent='Performance-Limit wird nach Eingabe einer unterstützten Auflösung angezeigt.';return;}"
        "const w=qw(q.value);"
        "const m=Math.max(0,Math.min(30,Math.floor((cap*100)/(p*w))));"
        "f.max=Math.max(1,m);"
        "if(m<1){h.textContent='Diese Auflösung/JPEG-Qualität überschreitet bereits bei 1 fps das Board-Limit.';return;}"
        "const load=Math.ceil((p*(Number(f.value)||0)*w)/100);"
        "h.textContent='Board-Leistungsgrenze: max. '+m+' fps für diese Auflösung/Qualität. '+"
        "'Aktueller Lastwert: '+load.toLocaleString('de-DE')+' / '+cap.toLocaleString('de-DE')+'.';"
        "}"
        "r.addEventListener('input',update);q.addEventListener('input',update);f.addEventListener('input',update);update();"
        "})();</script>";

    html += "camera_xclk_mhz: <select name='camera_xclk_mhz'>";
    html += "<option value='10'" +
            String(cfg_camera_xclk_mhz == 10 ? " selected" : "") +
            ">10 MHz</option>";
    html += "<option value='16'" +
            String(cfg_camera_xclk_mhz == 16 ? " selected" : "") +
            ">16 MHz</option>";
    html += "<option value='20'" +
            String(cfg_camera_xclk_mhz == 20 ? " selected" : "") +
            ">20 MHz</option>";
    html += "</select> <small>(Kamera-XCLK; Änderung wird nach Neustart wirksam)</small><br>";

    html += "camera_auto_exposure: <select name='camera_auto_exposure'>";

    html += "<option value='1'" +
            String(cfg_camera_auto_exposure ? " selected" : "") +
            ">1 - Auto Exposure an</option>";

    html += "<option value='0'" +
            String(!cfg_camera_auto_exposure ? " selected" : "") +
            ">0 - Auto Exposure aus</option>";

    html += "</select><br>";

    html += "camera_ae_level: <input name='camera_ae_level' type='number' "
            "min='-2' max='2' value='" +
            String(cfg_camera_ae_level) +
            "'> <small>(-2..2; negativer = dunkleres AE-Ziel)</small><br>";

    html +=
        "<div style='margin:14px 0;padding:14px;border:1px solid #d8dee6;border-radius:8px;background:#f8fbff'>"
        "<b>OV3660 Sensor-Crop</b><br>"
        "<span class='muted'>Verwendet einen kleineren Sensor-Ausschnitt bei gleicher JPEG-Ausgabeauflösung. "
        "1.0x entspricht exakt dem bisherigen Vollbild. Die Position wird als 3x3-Raster gespeichert.</span><br><br>";

    html += "camera_crop_zoom: <select name='camera_crop_zoom'>";
    html += "<option value='1.0'" +
            String(cfg_camera_crop_zoom == "1.0" ? " selected" : "") +
            ">1.0x - volles Sichtfeld</option>";
    html += "<option value='1.5'" +
            String(cfg_camera_crop_zoom == "1.5" ? " selected" : "") +
            ">1.5x - engerer Ausschnitt</option>";
    html += "<option value='2.0'" +
            String(cfg_camera_crop_zoom == "2.0" ? " selected" : "") +
            ">2.0x - enger Ausschnitt</option>";
    html += "</select><br>";

    html += "camera_crop_x: <select name='camera_crop_x'>";
    html += "<option value='0'" +
            String(cfg_camera_crop_x == 0 ? " selected" : "") +
            ">links</option>";
    html += "<option value='1'" +
            String(cfg_camera_crop_x == 1 ? " selected" : "") +
            ">Mitte</option>";
    html += "<option value='2'" +
            String(cfg_camera_crop_x == 2 ? " selected" : "") +
            ">rechts</option>";
    html += "</select><br>";

    html += "camera_crop_y: <select name='camera_crop_y'>";
    html += "<option value='0'" +
            String(cfg_camera_crop_y == 0 ? " selected" : "") +
            ">oben</option>";
    html += "<option value='1'" +
            String(cfg_camera_crop_y == 1 ? " selected" : "") +
            ">Mitte</option>";
    html += "<option value='2'" +
            String(cfg_camera_crop_y == 2 ? " selected" : "") +
            ">unten</option>";
    html += "</select><br>";

    html +=
        "<small class='muted'>Live einstellen: <a href='/preview'>Live Preview öffnen</a>. "
        "Sensor-Crop wird nur bei tatsächlich erkanntem OV3660 angewendet; "
        "für 1.5x/2.0x sind 4:3-Ausgaben bis 1024x768 vorgesehen.</small>"
        "</div>";

    html += "rotation: <select name='rotation'>";

    html += "<option value='0'" +
            String(cfg_rotation == 0 ? " selected" : "") +
            ">0°</option>";

    html += "<option value='180'" +
            String(cfg_rotation == 180 ? " selected" : "") +
            ">180°</option>";

    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>Aufnahme</h3>";

    html +=
        "<div style='margin:0 0 18px 0;padding:14px;border:1px solid #d8dee6;border-radius:8px;background:#f8fbff'>"
        "<b>Recording-Modus</b><br>"
        "<select name='recording_mode' style='min-width:320px;max-width:100%'>"
        "<option value='off'" + String(!cfg_motion_recording_enabled && !cfg_shooter_enabled ? " selected" : "") + ">Aus - keine automatische Aufnahme</option>"
        "<option value='motion'" + String(cfg_motion_recording_enabled && !cfg_shooter_enabled ? " selected" : "") + ">Normal Recording - Motion/Alarm</option>"
        "<option value='shooter'" + String(!cfg_motion_recording_enabled && cfg_shooter_enabled ? " selected" : "") + ">Power Shooter standalone</option>"
        "<option value='motion_shooter'" + String(cfg_motion_recording_enabled && cfg_shooter_enabled ? " selected" : "") + ">Normal Recording + Power Shooter</option>"
        "</select><br>"
        "<small class='muted'>Der Modus steuert intern motion_recording_enabled und shooter_enabled. Im Kombimodus läuft der Power Shooter zusätzlich; ein Alarm-/Motionvideo hat weiterhin Vorrang.</small><br><br>"
        "<b>" + htmlText(UI_RECORDING_TRIGGER_MODE) + "</b><br>"
        "<select name='motion_recording_decision' style='min-width:320px;max-width:100%'>"
        "<option value='direct'" + String(cfg_motion_recording_decision == "direct" ? " selected" : "") + ">" + htmlText(UI_RECORDING_TRIGGER_DIRECT) + "</option>"
        "<option value='image_verify'" + String(cfg_motion_recording_decision == "image_verify" ? " selected" : "") + ">" + htmlText(UI_RECORDING_TRIGGER_VERIFY) + "</option>"
        "<option value='image_only'" + String(cfg_motion_recording_decision == "image_only" ? " selected" : "") + ">" + htmlText(UI_RECORDING_TRIGGER_IMAGE_ONLY) + "</option>"
        "</select><br>"
        "<small class='muted'>" + htmlText(UI_RECORDING_TRIGGER_HELP) + "</small>";

    html +=
        "<div style='margin-top:8px'><a href='/image_motion'>" +
        htmlText(UI_RECORDING_TRIGGER_IMAGE_SETTINGS) +
        "</a></div>";

    html += "</div>";

    html += "recording_format: <select id='cfgRecordingFormat' name='recording_format' onchange='sfAudioUi()'>";

    html += "<option value='avi'" +
            String(cfg_recording_format == "avi" ? " selected" : "") +
            ">AVI (MJPEG + separate SRT)</option>";

    html += "<option value='mkv'" +
            String(cfg_recording_format == "mkv" ? " selected" : "") +
            ">MKV (MJPEG + embedded subtitles)</option>";

    html += "</select><br>";

    html += "timestamp_enabled: <select name='timestamp_enabled'>";

    html += "<option value='1'" +
            String(cfg_timestamp_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_timestamp_enabled ? " selected" : "") +
            ">0 - aus</option>";

    html += "</select><br>";

    html +=
        htmlText(UI_RECORDING_ENCRYPTION) +
        ": <select name='recording_encryption'>";

    html +=
        "<option value='0'" +
        String(!cfg_recording_encryption ? " selected" : "") +
        ">" +
        htmlText(UI_RECORDING_ENCRYPTION_DISABLED) +
        "</option>";

    html +=
        "<option value='1'" +
        String(cfg_recording_encryption ? " selected" : "") +
        ">" +
        htmlText(UI_RECORDING_ENCRYPTION_ENABLED) +
        "</option>";

    html += "</select><br>";

    html +=
        "<small class='muted'>" +
        htmlText(UI_RECORDING_ENCRYPTION_HELP) +
        "</small><br>";

    recordingCryptoBegin();

    if (recordingCryptoReady()) {
        html +=
            "<small style='color:#267326'>" +
            htmlText(UI_RECORDING_ENCRYPTION_KEY_READY) +
            " (eFuse KEY" +
            String(recordingCryptoKeySlot()) +
            ")</small><br>";
    } else {
        RecordingCryptoKeyStatus keyStatus =
            recordingCryptoKeyStatus();

        if (
            keyStatus == RECORDING_CRYPTO_KEY_UNPROVISIONED ||
            keyStatus == RECORDING_CRYPTO_KEY_PROVISION_PENDING
        ) {
            html +=
                "<small style='color:#9a5a00'>" +
                htmlText(UI_RECORDING_ENCRYPTION_KEY_UNINITIALIZED) +
                "</small><br>";
        } else {
            html +=
                "<small style='color:#a00000'>" +
                htmlText(UI_RECORDING_ENCRYPTION_PROVISION_FAILED) +
                ": " +
                htmlEscape(
                    String(recordingCryptoKeyStatusName())
                ) +
                "</small><br>";
        }
    }

    AudioCaptureCapabilities audioCaps =
        audioCaptureCapabilities();

    html +=
        "<div style='margin-top:18px;padding:14px;border:1px solid #8fb5c9;border-radius:8px;background:#f7fbfd'>"
        "<b>" +
        htmlText(UI_AUDIO_TITLE) +
        "</b><br>"
        "<span class='muted'>" +
        htmlText(UI_AUDIO_SIMPLE_HELP) +
        "</span><br><br>";

    html +=
        htmlText(UI_AUDIO_ENABLE) +
        ": <select id='cfgAudioEnabled' name='audio_enabled' onchange='sfAudioUi()'>";
    html += "<option value='0'" +
            String(!cfg_audio_enabled ? " selected" : "") +
            ">0 - " + htmlText(UI_AUDIO_OFF) + "</option>";
    html += "<option value='1'" +
            String(cfg_audio_enabled ? " selected" : "") +
            ">1 - " + htmlText(UI_AUDIO_ON) + "</option>";
    html += "</select><br>";

    html +=
        "<small class='muted'>" +
        htmlText(UI_AUDIO_MKV_REQUIRED) +
        "</small><br><br>";

    html +=
        "<button type='button' onclick=\"sfAudioAdvancedOpen()\">" +
        htmlText(UI_AUDIO_ADVANCED_SETTINGS) +
        "</button> ";

    if (audioCaps.available) {
        html +=
            "<button type='submit' formaction='/audio_test_record' formmethod='post' "
            "onclick=\"return sfAudioProgressSubmit(this,'test')\">" +
            htmlText(UI_AUDIO_TEST) +
            "</button> "
            "<button type='submit' formaction='/audio_benchmark' formmethod='post' "
            "onclick=\"return sfAudioProgressSubmit(this,'benchmark')\">" +
            htmlText(UI_AUDIO_BENCHMARK) +
            "</button><br>"
            "<small class='muted'>" +
            htmlText(UI_AUDIO_TEST_HELP) +
            "</small><br>"
            "<small class='muted'>" +
            htmlText(UI_AUDIO_BENCHMARK_HELP) +
            "</small>";
    } else {
        html +=
            "<br><small style='color:#9a5a00'>" +
            htmlText(UI_AUDIO_NO_INPUT) +
            "</small>";
    }

    html +=
        "<br><small class='muted'>" +
        htmlText(UI_AUDIO_ENCRYPTION_NOTE) +
        "</small>";

    html +=
        "<div id='sfAudioAdvancedModal' style='display:none;position:fixed;z-index:12000;inset:0;background:rgba(0,0,0,.48);padding:18px;overflow:auto'>"
        "<div style='max-width:720px;margin:5vh auto;background:#fff;border-radius:10px;padding:18px;box-shadow:0 10px 36px rgba(0,0,0,.3)'>"
        "<div style='display:flex;justify-content:space-between;align-items:center;gap:12px'>"
        "<h3 style='margin:0'>" + htmlText(UI_AUDIO_ADVANCED_TITLE) + "</h3>"
        "<button type='button' onclick=\"sfAudioAdvancedClose()\">&times;</button>"
        "</div><p class='muted'>" + htmlText(UI_AUDIO_ADVANCED_HELP) + "</p>";

#if BOARD_HAS_INTEGRATED_MIC
    String boardAudioName = BOARD_INTEGRATED_MIC_NAME;
#else
    String boardAudioName = tr(UI_NOT_DETECTED);
#endif

    html +=
        "<p class='muted'>" +
        htmlText(UI_AUDIO_SOURCE_BOARD_DEFAULT) +
        ": <b>" + htmlEscape(boardAudioName) + "</b><br>" +
        htmlText(UI_AUDIO_SOURCE) +
        " (" + htmlText(UI_STATUS_ACTIVE) + "): <b>" +
        htmlEscape(String(audioCaptureBackendName())) +
        "</b></p>";

    html +=
        htmlText(UI_AUDIO_EXPERT_MODE) +
        ": <select id='cfgAudioExpertMode' name='audio_expert_mode' onchange='sfAudioUi()'>"
        "<option value='0'" +
        String(!cfg_audio_expert_mode ? " selected" : "") +
        ">0 - User</option>"
        "<option value='1'" +
        String(cfg_audio_expert_mode ? " selected" : "") +
        ">1 - Expert</option>"
        "</select><br>";

    html +=
        htmlText(UI_AUDIO_SOURCE) +
        ": <select id='cfgAudioSource' name='audio_source' onchange='sfAudioUi()'>"
        "<option value='board_default'" +
        String(cfg_audio_source == "board_default" ? " selected" : "") +
        ">" + htmlText(UI_AUDIO_SOURCE_BOARD_DEFAULT) + "</option>"
        "<option value='external'" +
        String(cfg_audio_source == "external" ? " selected" : "") +
        ">" + htmlText(UI_AUDIO_SOURCE_EXTERNAL) + "</option>"
        "</select><br>";

    html +=
        "audio_sample_rate: <input name='audio_sample_rate' type='number' min='8000' max='96000' step='1000' value='" +
        String(cfg_audio_sample_rate) +
        "' style='width:110px'> Hz<br>";

    html += "audio_bits_per_sample: <select name='audio_bits_per_sample'>";
    html += "<option value='16'" + String(cfg_audio_bits_per_sample == 16 ? " selected" : "") + ">16 bit</option>";
    html += "<option value='24'" + String(cfg_audio_bits_per_sample == 24 ? " selected" : "") + ">24 bit</option>";
    html += "<option value='32'" + String(cfg_audio_bits_per_sample == 32 ? " selected" : "") + ">32 bit</option>";
    html += "</select><br>";

    html += "audio_channels: <select name='audio_channels'>";
    html += "<option value='1'" + String(cfg_audio_channels == 1 ? " selected" : "") + ">1 - mono</option>";
    html += "<option value='2'" + String(cfg_audio_channels == 2 ? " selected" : "") + ">2 - stereo</option>";
    html += "</select><br>";

    html +=
        "<div id='cfgAudioExpertPanel' style='margin-top:12px;padding:12px;border:1px dashed #78909c;border-radius:6px'>"
        "<b>" + htmlText(UI_AUDIO_EXTERNAL_PINS) + "</b><br>"
        "<small class='muted'>" + htmlText(UI_AUDIO_EXPERT_HELP) + "</small><br>"
        "<small style='color:#9a5a00'>" + htmlText(UI_AUDIO_GPIO_WARNING) + "</small><br><br>" +
        htmlText(UI_AUDIO_BACKEND) +
        ": <select id='cfgAudioBackend' name='audio_backend' onchange='sfAudioUi()'>"
        "<option value='pdm'" +
        String(cfg_audio_backend == "pdm" ? " selected" : "") +
        ">" + htmlText(UI_AUDIO_BACKEND_PDM) + "</option>"
        "<option value='i2s'" +
        String(cfg_audio_backend == "i2s" ? " selected" : "") +
        ">" + htmlText(UI_AUDIO_BACKEND_I2S) + "</option>"
        "</select><br>";

    html +=
        "<div id='cfgAudioPdmPanel' style='margin-top:8px'>"
        "audio_pdm_clk_pin: <input name='audio_pdm_clk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_pdm_clk_pin) +
        "' style='width:80px'><br>"
        "audio_pdm_data_pin: <input name='audio_pdm_data_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_pdm_data_pin) +
        "' style='width:80px'><br>"
        "</div>";

    html +=
        "<div id='cfgAudioI2sPanel' style='margin-top:8px'>"
        "audio_i2s_bclk_pin: <input name='audio_i2s_bclk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_bclk_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_ws_pin: <input name='audio_i2s_ws_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_ws_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_data_pin: <input name='audio_i2s_data_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_data_pin) +
        "' style='width:80px'><br>"
        "audio_i2s_mclk_pin: <input name='audio_i2s_mclk_pin' type='number' min='-1' max='48' value='" +
        String(cfg_audio_i2s_mclk_pin) +
        "' style='width:80px'> <small class='muted'>-1 = unused</small><br>" +
        htmlText(UI_AUDIO_I2S_SLOT) +
        ": <select name='audio_i2s_slot'>"
        "<option value='left'" + String(cfg_audio_i2s_slot == "left" ? " selected" : "") + ">left</option>"
        "<option value='right'" + String(cfg_audio_i2s_slot == "right" ? " selected" : "") + ">right</option>"
        "<option value='stereo'" + String(cfg_audio_i2s_slot == "stereo" ? " selected" : "") + ">stereo</option>"
        "</select><br>"
        "</div>"
        "<small class='muted'>" + htmlText(UI_AUDIO_SAVE_HARDWARE_NOTE) + "</small>"
        "</div>";

    if (audioCaps.available) {
        html +=
            "<p class='muted'>Backend: " +
            String((unsigned long)audioCaps.minSampleRate) + ".." +
            String((unsigned long)audioCaps.maxSampleRate) + " Hz; " +
            htmlText(UI_AUDIO_RECOMMENDED) + " " +
            String((unsigned long)audioCaps.recommendedSampleRate) +
            " Hz.</p>";
    }

    html +=
        "<div style='margin-top:16px;text-align:right'>"
        "<button type='button' onclick=\"sfAudioAdvancedClose()\">" +
        htmlText(UI_AUDIO_CLOSE) +
        "</button>"
        "</div></div></div>";

    html +=
        "<div id='sfAudioProgressModal' style='display:none;position:fixed;z-index:13000;inset:0;background:rgba(0,0,0,.56);padding:18px'>"
        "<div style='max-width:520px;margin:18vh auto;background:#fff;border-radius:10px;padding:20px;box-shadow:0 10px 36px rgba(0,0,0,.35)'>"
        "<h3 id='sfAudioProgressTitle' style='margin-top:0'>" + htmlText(UI_AUDIO_TEST_RUNNING) + "</h3>"
        "<p id='sfAudioProgressText' class='muted'>" + htmlText(UI_AUDIO_TEST_RUNNING_HELP) + "</p>"
        "<div style='height:8px;background:#d7dde3;border-radius:999px;overflow:hidden;position:relative;margin:20px 0'>"
        "<span style='position:absolute;top:0;width:18%;height:100%;border-radius:999px;background:#c62828;animation:sfAudioSlide 1.2s ease-in-out infinite alternate'></span>"
        "</div>"
        "<small class='muted'>" + htmlText(UI_AUDIO_PROGRESS_NOTE) + "</small>"
        "</div></div>"
        "<style>@keyframes sfAudioSlide{from{left:0}to{left:82%}}</style>"
        "<script>"
        "function sfAudioAdvancedOpen(){var m=document.getElementById('sfAudioAdvancedModal');if(m)m.style.display='block';}"
        "function sfAudioAdvancedClose(){var m=document.getElementById('sfAudioAdvancedModal');if(m)m.style.display='none';}"
        "function sfAudioProgressStart(kind){"
        "var m=document.getElementById('sfAudioProgressModal');var t=document.getElementById('sfAudioProgressTitle');var p=document.getElementById('sfAudioProgressText');"
        "if(kind==='benchmark'){if(t)t.textContent='" + htmlJsString(tr(UI_AUDIO_BENCHMARK_RUNNING)) + "';if(p)p.textContent='" + htmlJsString(tr(UI_AUDIO_BENCHMARK_RUNNING_HELP)) + "';}"
        "else{if(t)t.textContent='" + htmlJsString(tr(UI_AUDIO_TEST_RUNNING)) + "';if(p)p.textContent='" + htmlJsString(tr(UI_AUDIO_TEST_RUNNING_HELP)) + "';}"
        "if(m)m.style.display='block';return true;}"
        "function sfAudioProgressSubmit(btn,kind){sfAudioProgressStart(kind);setTimeout(function(){var f=btn&&btn.form;if(!f)return;f.action=btn.formAction;f.method='post';f.submit();},80);return false;}"
        "function sfAudioUi(){"
        "var a=document.getElementById('cfgAudioEnabled');var r=document.getElementById('cfgRecordingFormat');var e=document.getElementById('cfgAudioExpertMode');var s=document.getElementById('cfgAudioSource');var p=document.getElementById('cfgAudioExpertPanel');var b=document.getElementById('cfgAudioBackend');var pp=document.getElementById('cfgAudioPdmPanel');var ip=document.getElementById('cfgAudioI2sPanel');"
        "if(a&&r&&a.value==='1')r.value='mkv';"
        "if(!e||!s||!p||!b||!pp||!ip)return;var expert=e.value==='1';if(!expert&&s.value==='external')s.value='board_default';p.style.display=expert?'block':'none';var external=expert&&s.value==='external';b.disabled=!external;pp.style.display=external&&b.value==='pdm'?'block':'none';ip.style.display=external&&b.value==='i2s'?'block':'none';}"
        "sfAudioUi();"
        "</script>"
        "</div>";

    html +=
        "<div style='margin-top:18px;padding:14px;border:1px solid #9cc7ff;border-radius:8px;background:#f5f9ff'>"
        "<b>Power Shooter / Dauershooter</b><br>"
        "<span class='muted'>Aktivierung erfolgt oben über den Recording-Modus. Hier werden nur die Shooter-Parameter eingestellt. Alarm-/Motionvideo hat im Kombimodus Vorrang. "
        "Bei geöffnetem Webinterface wird der Shooter durch die Web-Autopause vorübergehend pausiert und danach automatisch fortgesetzt.</span><br><br>"
        "shooter_storage_format: <select name='shooter_storage_format'>"
        "<option value='mkv'" + String(cfg_shooter_storage_format == "mkv" ? " selected" : "") + ">MKV - Sparse MKV, empfohlen</option>"
        "<option value='jpg'" + String(cfg_shooter_storage_format == "jpg" ? " selected" : "") + ">JPG - einzelne JPEG-Dateien</option>"
        "</select><br>"
        "shooter_interval_ms: <input id='cfgShooterInterval' name='shooter_interval_ms' type='number' min='250' max='86400000' step='1' value='" +
        String(cfg_shooter_interval_ms) +
        "' style='width:120px'> ms "
        "<small id='cfgShooterRateHint' class='muted'></small><br>"
        "<small class='muted'>Beispiele: 250 ms = 4 fps · 500 ms = 2 fps · 1000 ms = 1 fps · 2000 ms = 0,5 fps. Es sind Prüfslots; Filter können weniger Bilder speichern.</small><br><br>"
        "<label for='cfgShooterDark'><b>Dunkelgrenze / Mindesthelligkeit</b></label><br>"
        "<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap'>"
        "<input id='cfgShooterDark' name='shooter_dark_mean_min' type='range' min='0' max='255' step='1' value='" +
        String(cfg_shooter_dark_mean_min) +
        "' style='width:260px;max-width:70vw'>"
        "<span id='cfgShooterDarkValue' style='display:inline-block;min-width:34px;font-weight:600'>" +
        String(cfg_shooter_dark_mean_min) +
        "</span>"
        "<span id='cfgShooterDarkSwatch' title='Helligkeitswert' style='display:inline-block;width:28px;height:28px;border:1px solid #667085;border-radius:4px;vertical-align:middle'></span>"
        "</div>"
        "<small class='muted'>0 = Darkness-Filter praktisch aus. Bilder mit mittlerer Helligkeit unter diesem Wert werden verworfen; das Kästchen zeigt den gewählten Grauwert.</small><br>"
        "shooter_min_change_pct: <input name='shooter_min_change_pct' type='number' min='0' max='100' step='0.1' value='" +
        String(cfg_shooter_min_change_pct, 1) +
        "' style='width:90px'> % <small>(0 = Similarity-Filter aus; Vergleich gegen letztes akzeptiertes Bild)</small><br>"
        "shooter_force_save_seconds: <input name='shooter_force_save_seconds' type='number' min='0' max='86400' step='1' value='" +
        String(cfg_shooter_force_save_seconds) +
        "' style='width:100px'> s <small>(0 = kein Force-Save; umgeht Similarity, nicht Darkness)</small><br>"
        "shooter_flush_seconds: <input name='shooter_flush_seconds' type='number' min='0' max='3600' step='1' value='" +
        String(cfg_shooter_flush_seconds) +
        "' style='width:100px'> s <small>(0 = kein zeitbasierter Flush; Buffer-full/Shutdown/Reboot flushen weiterhin)</small><br>"
        "<small class='muted'>Die tatsächliche Zahl gespeicherter Bilder kann durch Darkness-/Change-Filter niedriger sein. PSRAM-Puffergröße wird automatisch gewählt.</small>"
        "<script>(function(){"
        "const i=document.getElementById('cfgShooterInterval');const h=document.getElementById('cfgShooterRateHint');"
        "function u(){const ms=Number(i&&i.value);if(!h)return;if(!Number.isFinite(ms)||ms<=0){h.textContent='';return;}const fps=1000/ms;h.textContent='≈ '+fps.toLocaleString('de-DE',{maximumFractionDigits:2})+' fps';}"
        "if(i){i.addEventListener('input',u);u();}"
        "const d=document.getElementById('cfgShooterDark');const v=document.getElementById('cfgShooterDarkValue');const sw=document.getElementById('cfgShooterDarkSwatch');"
        "function ud(){let n=Number(d&&d.value);if(!Number.isFinite(n))n=0;n=Math.max(0,Math.min(255,Math.round(n)));if(v)v.textContent=String(n);if(sw)sw.style.backgroundColor='rgb('+n+','+n+','+n+')';}"
        "if(d){d.addEventListener('input',ud);ud();}"
        "})();</script>"
        "</div>";

    html += "post_record_ms: <input name='post_record_ms' type='number' min='0' value='" +
            String(cfg_post_ms) + "'><br>";

    html += "recording_segment_seconds: <input name='recording_segment_seconds' type='number' "
            "min='0' max='86400' value='" +
            String(cfg_recording_segment_seconds) +
            "'> <small>(0 = unbegrenzt)</small><br>";

    html += "recording_segment_max_mb: <input name='recording_segment_max_mb' type='number' "
            "min='0' max='4095' value='" +
            String(cfg_recording_segment_max_mb) +
            "'> <small>(0 = unbegrenzt)</small><br>";

    html +=
        "<div style='margin-top:18px;padding:14px;border:1px solid #d8dee6;border-radius:8px;background:#fff7e8'>"
        "<b>Aufnahme-Sicherheitsgrenze</b><br>"
        "<span class='muted'>Begrenzt ein komplettes Bewegungsereignis über alle Aufnahme-Segmente. "
        "Beim Erreichen wird die laufende Datei sauber abgeschlossen und eine Sicherheitspause gestartet.</span><br><br>"
        "recording_event_max_seconds: <input name='recording_event_max_seconds' type='number' min='0' max='86400' value='" +
        String(cfg_recording_event_max_seconds) +
        "'> <small>(0 = unbegrenzt / deaktiviert)</small><br>"
        "recording_event_cooldown_seconds: <input name='recording_event_cooldown_seconds' type='number' min='0' max='86400' value='" +
        String(cfg_recording_event_cooldown_seconds) +
        "'> <small>(Pause nach Sicherheitsstopp; 0 = keine Pause, nicht empfohlen)</small>"
        "</div>";


    bool armScheduled = false;
    int armYear = 2026;
    int armMonth = 1;
    int armDay = 1;
    int armHour = 0;
    int armMinute = 0;

    recordingArmFormDefaults(
        armScheduled,
        armYear,
        armMonth,
        armDay,
        armHour,
        armMinute
    );

    html +=
        "<div style='margin-top:18px;padding:14px;border:1px solid #d8dee6;border-radius:8px;background:#fff'>"
        "<b>Automatische Aufnahme scharf ab</b><br>"
        "<span class='muted'>Vor diesem lokalen Datum/Zeitpunkt werden keine neuen Aufnahmen gestartet. "
        "Die Zeit wird gemaess der eingestellten Zeitzone interpretiert.</span><br><br>";

    html +=
        "Freigabe: <select id='recordingArmMode' name='recording_arm_mode'>"
        "<option value='off'" +
        String(!armScheduled ? " selected" : "") +
        ">sofort / keine Zeitsperre</option>"
        "<option value='scheduled'" +
        String(armScheduled ? " selected" : "") +
        ">ab festem Datum und Uhrzeit</option>"
        "</select><br>";

    html +=
        "<div id='recordingArmFields'>Datum: ";

    html +=
        "<select id='recordingArmDay' name='recording_arm_day' style='width:82px'>";

    for (int value = 1; value <= 31; ++value) {
        char label[8];
        snprintf(label, sizeof(label), "%02d", value);
        html +=
            "<option value='" + String(value) + "'" +
            String(value == armDay ? " selected" : "") +
            ">" + String(label) + "</option>";
    }

    html += "</select>.";

    html +=
        "<select id='recordingArmMonth' name='recording_arm_month' style='width:82px'>";

    for (int value = 1; value <= 12; ++value) {
        char label[8];
        snprintf(label, sizeof(label), "%02d", value);
        html +=
            "<option value='" + String(value) + "'" +
            String(value == armMonth ? " selected" : "") +
            ">" + String(label) + "</option>";
    }

    html += "</select>.";

    html +=
        "<select id='recordingArmYear' name='recording_arm_year' style='width:105px'>";

    for (int value = 2021; value <= 2099; ++value) {
        html +=
            "<option value='" + String(value) + "'" +
            String(value == armYear ? " selected" : "") +
            ">" + String(value) + "</option>";
    }

    html += "</select><br>Uhrzeit: ";

    html +=
        "<select id='recordingArmHour' name='recording_arm_hour' style='width:82px'>";

    for (int value = 0; value <= 23; ++value) {
        char label[8];
        snprintf(label, sizeof(label), "%02d", value);
        html +=
            "<option value='" + String(value) + "'" +
            String(value == armHour ? " selected" : "") +
            ">" + String(label) + "</option>";
    }

    html += "</select>:";

    html +=
        "<select id='recordingArmMinute' name='recording_arm_minute' style='width:82px'>";

    for (int value = 0; value <= 59; ++value) {
        char label[8];
        snprintf(label, sizeof(label), "%02d", value);
        html +=
            "<option value='" + String(value) + "'" +
            String(value == armMinute ? " selected" : "") +
            ">" + String(label) + "</option>";
    }

    html +=
        "</select>:00"
        "<br><small>Auswahlfelder verhindern Tippfehler; unmoegliche Datumswerte werden zusaetzlich beim Speichern abgelehnt.</small>"
        "</div></div>";

    html +=
        "<script>"
        "(function(){"
            "var mode=document.getElementById('recordingArmMode');"
            "var fields=document.getElementById('recordingArmFields');"
            "var day=document.getElementById('recordingArmDay');"
            "var month=document.getElementById('recordingArmMonth');"
            "var year=document.getElementById('recordingArmYear');"
            "function maxDay(){return new Date(Number(year.value),Number(month.value),0).getDate();}"
            "function syncDate(){"
                "var max=maxDay();var selected=Number(day.value);"
                "for(var i=0;i<day.options.length;i++){var v=Number(day.options[i].value);day.options[i].disabled=v>max;}"
                "if(selected>max)day.value=String(max);"
            "}"
            "function syncMode(){fields.style.display=mode.value==='scheduled'?'block':'none';}"
            "mode.addEventListener('change',syncMode);month.addEventListener('change',syncDate);year.addEventListener('change',syncDate);"
            "syncDate();syncMode();"
        "})();"
        "</script>";


    html += "</div><div class='settings-section'><h3>Sleep / Stromsparen</h3>";

    html += "sleep_mode: <select name='sleep_mode'>";

    html += "<option value='off'" +
            String(cfg_sleep_mode == "off" ? " selected" : "") +
            ">off - kein Sleep</option>";

    html += "<option value='light_sleep'" +
            String(cfg_sleep_mode == "light_sleep" ? " selected" : "") +
            ">light_sleep - schneller Wake</option>";

    html += "<option value='deep_sleep'" +
            String(cfg_sleep_mode == "deep_sleep" ? " selected" : "") +
            ">deep_sleep - maximal stromsparend</option>";

    html += "</select><br>";

    html += "sleep_delay_ms: <input name='sleep_delay_ms' type='number' "
            "min='0' max='60000' value='" +
            String(cfg_sleep_delay_ms) +
            "'> <small>(0..60000 ms)</small><br><br>";

    html +=
        "<b>Bootloop-/Unterspannungsschutz</b><br>"
        "<span class='muted'>Schützt SensorForge vor wiederholten kurzen Neustarts, "
        "z. B. wenn ein fast leerer oder instabiler Akku beim Hochfahren immer wieder einbricht. "
        "Nach mehreren unvollständigen Starts legt das Gerät automatisch eine längere Deep-Sleep-Pause ein. "
        "Normales Ausschalten nach stabilem Betrieb wird nicht als Fehler gewertet.</span><br>";

    html += "bootloop_protection: <select name='bootloop_protection'>";
    html += "<option value='1'" +
            String(cfg_bootloop_protection ? " selected" : "") +
            ">1 - aktiviert (empfohlen)</option>";
    html += "<option value='0'" +
            String(!cfg_bootloop_protection ? " selected" : "") +
            ">0 - deaktiviert</option>";
    html += "</select> <small>(vollständige Wirkung ab dem nächsten Neustart)</small><br>";


    html += "</div><div class='settings-section'><h3>Transportmodus</h3>";

    html +=
        "<p class='muted'>Vor dem Transport Kamera vollständig schwarz abkleben. "
        "Im aktiven Transportmodus wecken Radar/PIR das Gerät nicht; es prüft nur per Timer, "
        "ob die Abdeckung noch vorhanden ist. Nach bestätigtem Licht folgt die Installations-Wartezeit.</p>";

    html += "Status: <span class='status-pill " +
            String(cfg_transport_mode ? "warn" : "ok") +
            "'>" +
            String(cfg_transport_mode ? "AKTIV" : "AUS") +
            "</span><br><br>";

    html += "transport_check_seconds: <input name='transport_check_seconds' type='number' "
            "min='10' max='3600' value='" +
            String(cfg_transport_check_seconds) +
            "'> <small>(Abstand der Schwarzbild-Prüfungen, Standard 120 s)</small><br>";

    html += "transport_light_confirm_seconds: <input name='transport_light_confirm_seconds' type='number' "
            "min='0' max='120' value='" +
            String(cfg_transport_light_confirm_seconds) +
            "'> <small>(Licht muss über diesen Zeitraum mehrfach bestätigt werden)</small><br>";

    html += "transport_install_delay_seconds: <input name='transport_install_delay_seconds' type='number' "
            "min='0' max='86400' value='" +
            String(cfg_transport_install_delay_seconds) +
            "'> <small>(Zeit für Montage und Verlassen des Bildbereichs; Standard 300 s)</small><br>";

    html += "transport_max_duration_seconds: <input name='transport_max_duration_seconds' type='number' "
            "min='3600' max='604800' value='" +
            String(cfg_transport_max_duration_seconds) +
            "'> <small>(harte Sicherheitsgrenze; Standard 86400 s = 24 h)</small><br>";

    html += "transport_black_threshold: <input name='transport_black_threshold' type='number' "
            "min='0' max='255' value='" +
            String(cfg_transport_black_threshold) +
            "'> <small>(Schwarzgrenze der aktuellen Transport-Config; kleiner = strenger schwarz)</small><br>";

    html +=
        "<p class='muted'><b>Wichtig:</b> Für die normale Vorbereitung bitte die eigene "
        "<a href='/transport'>Transportsicherungs-Seite</a> verwenden. Dort wird der Schwarzwert beim Aufruf "
        "automatisch mit der echten Transport-Messmethode ermittelt und ein Grenzwert mit Reserve vorgeschlagen.</p>";


    html += "</div><div class='settings-section'><h3>Speicher / SD-Sicherheit</h3>";

    html += "min_free_space_mb: <input name='min_free_space_mb' type='number' min='0' value='" +
            String(cfg_min_free_space_mb) + "'><br>";

    html += "disk_full_action: <select name='disk_full_action'>";

    html += "<option value='rollover'" +
            String(cfg_disk_full_action == "rollover" ? " selected" : "") +
            ">rollover - älteste Aufnahmen löschen</option>";

    html += "<option value='stop'" +
            String(cfg_disk_full_action == "stop" ? " selected" : "") +
            ">stop - Aufnahme stoppen</option>";

    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>LED</h3>";

    html += "led_enabled: <select name='led_enabled'>";
    html += "<option value='1'" +
            String(cfg_led_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_led_enabled ? " selected" : "") +
            ">0 - aus</option>";
    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>WLAN / NTP</h3>";

    html += "hostname: <input name='hostname' value='" +
            htmlEscape(cfg_hostname) + "'>"
            " <small>(auch Hotspot-SSID)</small><br>";

    html += "timezone: <input name='timezone' maxlength='127' value='" +
            htmlEscape(cfg_timezone) + "'>"
            " <small>POSIX TZ, z.B. Österreich: "
            "CET-1CEST,M3.5.0,M10.5.0/3; UTC: UTC0</small><br>";

    html += "wifi_on_system_start: <select name='wifi_on_system_start'>";

    html += "<option value='off'" +
            String(cfg_wifi_on_system_start == "off" ? " selected" : "") +
            ">off - WLAN beim Systemstart aus</option>";

    html += "<option value='on'" +
            String(cfg_wifi_on_system_start == "on" ? " selected" : "") +
            ">on - WLAN beim Systemstart ein</option>";

    html += "<option value='on_missing_time'" +
            String(cfg_wifi_on_system_start == "on_missing_time" ? " selected" : "") +
            ">on_missing_time - nur einschalten, wenn Uhrzeit fehlt</option>";

    html += "</select><br>";

    html += "wifi_timeout_sec: <input name='wifi_timeout_sec' type='number' min='0' max='86400' value='" +
            String(cfg_wifi_timeout_sec) + "'>"
            " <small>(0 = automatische Abschaltung aus)</small><br>";

    html += "wifi_ssid: <input name='wifi_ssid' value='" +
            htmlEscape(cfg_wifi_ssid) + "'><br>";

    // Passwort absichtlich nicht im HTML zurücksenden.
    // Leeres Feld bedeutet: bestehendes Passwort behalten.
    html += "wifi_pass: <input type='password' name='wifi_pass' "
            "value='' placeholder='leer = unverändert'><br>";


    html += "</div><div class='settings-section'><h3>Hotspot / Access Point</h3>";

    html += "hotspot_enabled: <select name='hotspot_enabled'>";
    html += "<option value='1'" +
            String(cfg_hotspot_enabled ? " selected" : "") +
            ">1 - beim Systemstart automatisch an</option>";
    html += "<option value='0'" +
            String(!cfg_hotspot_enabled ? " selected" : "") +
            ">0 - beim Systemstart aus</option>";
    html += "</select><br>";

    // Hotspot password is never sent back to the browser. Empty is a valid
    // runtime value and means an open AP. In the form, an empty field still
    // means "keep current" so an already-open default remains open on save.
    html += "hotspot_password: <input type='password' name='hotspot_password' "
            "minlength='8' maxlength='63' value='' "
            "placeholder='leer = unverändert'>"
            " <small>(aktuell: ";

    html +=
        cfg_hotspot_password.length()
        ? "passwortgeschützt"
        : "offen / kein Passwort";

    html +=
        "; neues Passwort: 8..63 Zeichen)</small><br>";

    html += "hotspot_hidden: <select name='hotspot_hidden'>";
    html += "<option value='0'" +
            String(!cfg_hotspot_hidden ? " selected" : "") +
            ">0 - SSID sichtbar</option>";
    html += "<option value='1'" +
            String(cfg_hotspot_hidden ? " selected" : "") +
            ">1 - SSID versteckt</option>";
    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>Webinterface / Zugriffsschutz</h3>";

    html += "web_language: <select name='web_language'>";
    html += "<option value='de'" +
            String(cfg_web_language == "de" ? " selected" : "") +
            ">de - Deutsch</option>";
    html += "<option value='en'" +
            String(cfg_web_language == "en" ? " selected" : "") +
            ">en - English</option>";
    html += "</select><br>";

    html += "web_recording_auto_pause: <select name='web_recording_auto_pause'>";
    html += "<option value='1'" +
            String(cfg_web_recording_auto_pause ? " selected" : "") +
            ">1 - Aufnahme beim Oeffnen automatisch pausieren</option>";
    html += "<option value='0'" +
            String(!cfg_web_recording_auto_pause ? " selected" : "") +
            ">0 - Aufnahmezustand beim Oeffnen nicht veraendern</option>";
    html += "</select><br>";

    html += "web_auth_enabled: <select name='web_auth_enabled'>";
    html += "<option value='1'" +
            String(cfg_web_auth_enabled ? " selected" : "") +
            ">1 - Login erforderlich</option>";
    html += "<option value='0'" +
            String(!cfg_web_auth_enabled ? " selected" : "") +
            ">0 - ohne Login</option>";
    html += "</select><br>";

    html += "web_username: <input name='web_username' maxlength='32' value='" +
            htmlEscape(cfg_web_username) +
            "'> <small>(1..32 Zeichen, kein Doppelpunkt)</small><br>";

    // Web-Passwort nie an den Browser zuruecksenden.
    // Leeres Feld bedeutet: bestehendes Passwort behalten.
    html += "web_password: <input type='password' name='web_password' "
            "minlength='8' maxlength='63' value='' "
            "placeholder='leer = unverändert'>"
            " <small>(8..63 Zeichen)</small><br>";


    html += "</div><div class='settings-section'><h3>Debug / Log</h3>";

    html += "debug_enabled: <select name='debug_enabled'>";
    html += "<option value='1'" +
            String(cfg_debug_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_debug_enabled ? " selected" : "") +
            ">0 - aus</option>";
    html += "</select><br>";

    html += "log_file: <input name='log_file' value='" +
            htmlEscape(cfg_log_file) + "'><br>";

    html += "</div>";
    html += "<div class='form-actions'><button type='submit'>Speichern</button></div>";
    html += "</form>";


    html +=
        "<section class='settings-section' style='border-left:5px solid #d97706'>"
        "<h3>Transportmodus starten</h3>"
        "<p class='muted'>1. Kamera vollständig schwarz abkleben. 2. Falls Zeiten/Schwellwerte geändert wurden, "
        "zuerst oben speichern. 3. Dann Transportmodus aktivieren. SensorForge startet neu und verwendet "
        "anschließend ausschließlich Timer-Wake, bis die Abdeckung entfernt wurde.</p>";

    if (!cfg_transport_mode) {
        html +=
            "<form method='POST' action='/transport_activate' "
            "onsubmit=\"return confirm('Kamera ist vollständig schwarz abgeklebt und Transportmodus soll jetzt aktiviert werden?');\">"
            "<button class='primary' type='submit'>Transportmodus aktivieren</button>"
            "</form>";
    } else {
        html +=
            "<p><span class='status-pill warn'>Transportmodus ist aktiviert</span></p>"
            "<form method='POST' action='/transport_cancel' "
            "onsubmit=\"return confirm('Transportmodus wirklich deaktivieren?');\">"
            "<button type='submit'>Transportmodus deaktivieren</button>"
            "</form>";
    }

    html +=
        "</section>";


    html +=
        configSdAvailable() && configSdPresent()
        ? "<p class='muted'>Speicherziel für normale Saves: <b>interner Flash + vorhandene SD-config.txt</b>.</p>"
        : "<p class='muted'>Speicherziel für normale Saves: <b>nur interner Flash</b>. Eine fehlende SD-config.txt wird nicht automatisch erzeugt.</p>";


    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// SAVE CONFIG
// -------------------------------------------------------------

static void handleSave()
{
    if (rejectWhileRecording("config save"))
        return;


    // Never trust cached boot state when deciding whether the
    // user must be asked to create/replace SD config.txt.
    configRefreshSdStatus();


    int fps =
        constrain(
            server.arg("fps").toInt(),
            1,
            30
        );

    int quality =
        constrain(
            server.arg("quality").toInt(),
            0,
            63
        );


    String cameraXclkText =
        server.arg("camera_xclk_mhz");

    cameraXclkText.trim();

    if (
        cameraXclkText != "10" &&
        cameraXclkText != "16" &&
        cameraXclkText != "20"
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger camera_xclk_mhz Wert"
        );
        return;
    }

    int cameraXclkMhz =
        cameraXclkText.toInt();


    int cameraAutoExposure =
        server.arg("camera_auto_exposure").toInt()
        ? 1
        : 0;


    int cameraAeLevel =
        constrain(
            server.arg("camera_ae_level").toInt(),
            -2,
            2
        );


    String cameraCropZoom =
        server.arg("camera_crop_zoom");

    cameraCropZoom.trim();

    if (
        cameraCropZoom != "1.0" &&
        cameraCropZoom != "1.5" &&
        cameraCropZoom != "2.0"
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger camera_crop_zoom Wert"
        );
        return;
    }


    String cameraCropXText =
        server.arg("camera_crop_x");

    String cameraCropYText =
        server.arg("camera_crop_y");

    if (
        (
            cameraCropXText != "0" &&
            cameraCropXText != "1" &&
            cameraCropXText != "2"
        ) ||
        (
            cameraCropYText != "0" &&
            cameraCropYText != "1" &&
            cameraCropYText != "2"
        )
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltige camera_crop_x/camera_crop_y Position"
        );
        return;
    }

    int cameraCropX =
        cameraCropXText.toInt();

    int cameraCropY =
        cameraCropYText.toInt();


    int rotation =
        server.arg("rotation").toInt();

    if (
        rotation != 0 &&
        rotation != 180
    ) {
        rotation = 0;
    }


    String recordingFormat =
        server.arg("recording_format");

    recordingFormat.trim();
    recordingFormat.toLowerCase();

    if (
        recordingFormat != "avi" &&
        recordingFormat != "mkv"
    ) {
        recordingFormat = "avi";
    }


    String motionRecordingDecision =
        server.arg("motion_recording_decision");

    motionRecordingDecision.trim();
    motionRecordingDecision.toLowerCase();

    if (
        motionRecordingDecision != "direct" &&
        motionRecordingDecision != "image_verify" &&
        motionRecordingDecision != "image_only"
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger motion_recording_decision Wert"
        );
        return;
    }

    const bool motionRecordingDecisionChanged =
        motionRecordingDecision != cfg_motion_recording_decision;


    int timestampEnabled =
        server.arg("timestamp_enabled").toInt()
        ? 1
        : 0;

    int recordingEncryption =
        server.arg("recording_encryption").toInt()
        ? 1
        : 0;

    int audioEnabled =
        server.hasArg("audio_enabled")
        ? (server.arg("audio_enabled").toInt() ? 1 : 0)
        : cfg_audio_enabled;

    int audioExpertMode =
        server.hasArg("audio_expert_mode")
        ? (server.arg("audio_expert_mode").toInt() ? 1 : 0)
        : cfg_audio_expert_mode;

    String audioSource =
        server.hasArg("audio_source")
        ? server.arg("audio_source")
        : cfg_audio_source;
    audioSource.trim();
    audioSource.toLowerCase();

    String audioBackend =
        server.hasArg("audio_backend")
        ? server.arg("audio_backend")
        : cfg_audio_backend;
    audioBackend.trim();
    audioBackend.toLowerCase();

    int audioPdmClkPin =
        server.hasArg("audio_pdm_clk_pin")
        ? server.arg("audio_pdm_clk_pin").toInt()
        : cfg_audio_pdm_clk_pin;

    int audioPdmDataPin =
        server.hasArg("audio_pdm_data_pin")
        ? server.arg("audio_pdm_data_pin").toInt()
        : cfg_audio_pdm_data_pin;

    int audioI2sBclkPin =
        server.hasArg("audio_i2s_bclk_pin")
        ? server.arg("audio_i2s_bclk_pin").toInt()
        : cfg_audio_i2s_bclk_pin;

    int audioI2sWsPin =
        server.hasArg("audio_i2s_ws_pin")
        ? server.arg("audio_i2s_ws_pin").toInt()
        : cfg_audio_i2s_ws_pin;

    int audioI2sDataPin =
        server.hasArg("audio_i2s_data_pin")
        ? server.arg("audio_i2s_data_pin").toInt()
        : cfg_audio_i2s_data_pin;

    int audioI2sMclkPin =
        server.hasArg("audio_i2s_mclk_pin")
        ? server.arg("audio_i2s_mclk_pin").toInt()
        : cfg_audio_i2s_mclk_pin;

    String audioI2sSlot =
        server.hasArg("audio_i2s_slot")
        ? server.arg("audio_i2s_slot")
        : cfg_audio_i2s_slot;
    audioI2sSlot.trim();
    audioI2sSlot.toLowerCase();

    int audioSampleRate =
        server.hasArg("audio_sample_rate")
        ? server.arg("audio_sample_rate").toInt()
        : cfg_audio_sample_rate;

    int audioBitsPerSample =
        server.hasArg("audio_bits_per_sample")
        ? server.arg("audio_bits_per_sample").toInt()
        : cfg_audio_bits_per_sample;

    int audioChannels =
        server.hasArg("audio_channels")
        ? server.arg("audio_channels").toInt()
        : cfg_audio_channels;

    if (
        audioSampleRate < 8000 ||
        audioSampleRate > 96000 ||
        (
            audioBitsPerSample != 16 &&
            audioBitsPerSample != 24 &&
            audioBitsPerSample != 32
        ) ||
        (audioChannels != 1 && audioChannels != 2)
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltige Audio-Formatwerte"
        );
        return;
    }

    // Production audio muxing is deliberately limited to MKV. Keep old
    // persisted AVI+audio configurations loadable for backward compatibility;
    // a normal WebConfig save automatically selects MKV when audio is enabled.
    if (audioEnabled)
        recordingFormat = "mkv";

    String recordingMode =
        server.arg("recording_mode");

    recordingMode.trim();
    recordingMode.toLowerCase();

    int motionRecordingEnabled = 0;
    int shooterEnabled = 0;

    if (recordingMode == "off") {
        // Both remain disabled.
    } else if (recordingMode == "motion") {
        motionRecordingEnabled = 1;
    } else if (recordingMode == "shooter") {
        shooterEnabled = 1;
    } else if (recordingMode == "motion_shooter") {
        motionRecordingEnabled = 1;
        shooterEnabled = 1;
    } else if (!recordingMode.length()) {
        // Backward-compatible fallback for an old browser page that may still
        // submit the pre-v31 fields after the firmware has just been updated.
        motionRecordingEnabled =
            server.hasArg("motion_recording_enabled") &&
            server.arg("motion_recording_enabled").toInt()
            ? 1
            : 0;
        shooterEnabled =
            server.hasArg("shooter_enabled") &&
            server.arg("shooter_enabled").toInt()
            ? 1
            : 0;
    } else {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger recording_mode Wert"
        );
        return;
    }

    String shooterStorageFormat =
        server.arg("shooter_storage_format");

    shooterStorageFormat.trim();
    shooterStorageFormat.toLowerCase();

    if (
        shooterStorageFormat != "mkv" &&
        shooterStorageFormat != "jpg"
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger shooter_storage_format Wert"
        );
        return;
    }

    int shooterIntervalMs =
        server.arg("shooter_interval_ms").toInt();

    if (
        shooterIntervalMs < 250 ||
        shooterIntervalMs > 86400000
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "shooter_interval_ms muss zwischen 250 und 86400000 liegen"
        );
        return;
    }

    int shooterDarkMeanMin =
        constrain(
            server.arg("shooter_dark_mean_min").toInt(),
            0,
            255
        );

    String shooterMinChangeText =
        server.arg("shooter_min_change_pct");

    shooterMinChangeText.trim();

    char *shooterMinChangeEnd = nullptr;
    float shooterMinChangePct =
        strtof(
            shooterMinChangeText.c_str(),
            &shooterMinChangeEnd
        );

    if (
        !shooterMinChangeText.length() ||
        !shooterMinChangeEnd ||
        *shooterMinChangeEnd != '\0' ||
        shooterMinChangePct < 0.0f ||
        shooterMinChangePct > 100.0f ||
        (shooterMinChangePct > 0.0f && shooterMinChangePct < 0.1f)
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "shooter_min_change_pct muss 0 oder 0.1..100.0 sein"
        );
        return;
    }

    int shooterForceSaveSeconds =
        constrain(
            server.arg("shooter_force_save_seconds").toInt(),
            0,
            86400
        );

    int shooterFlushSeconds =
        constrain(
            server.arg("shooter_flush_seconds").toInt(),
            0,
            3600
        );


    String recordingNotBefore = "off";

    String recordingArmMode =
        server.arg("recording_arm_mode");

    recordingArmMode.trim();
    recordingArmMode.toLowerCase();

    if (recordingArmMode == "scheduled") {
        int armYear = server.arg("recording_arm_year").toInt();
        int armMonth = server.arg("recording_arm_month").toInt();
        int armDay = server.arg("recording_arm_day").toInt();
        int armHour = server.arg("recording_arm_hour").toInt();
        int armMinute = server.arg("recording_arm_minute").toInt();

        if (
            armYear < 2021 || armYear > 2099 ||
            armMonth < 1 || armMonth > 12 ||
            armDay < 1 || armDay > 31 ||
            armHour < 0 || armHour > 23 ||
            armMinute < 0 || armMinute > 59
        ) {
            server.send(
                400,
                "text/plain; charset=utf-8",
                "Ungueltiges Datum/Uhrzeit fuer recording_not_before"
            );
            return;
        }

        char armValue[32];

        snprintf(
            armValue,
            sizeof(armValue),
            "%04d-%02d-%02dT%02d:%02d:00",
            armYear,
            armMonth,
            armDay,
            armHour,
            armMinute
        );

        recordingNotBefore =
            String(armValue);

    } else if (recordingArmMode != "off") {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungueltiger Scharfschaltungsmodus"
        );
        return;
    }


    int postMs =
        server.arg("post_record_ms").toInt();

    if (postMs < 0)
        postMs = 0;


    int recordingSegmentSeconds =
        constrain(
            server.arg("recording_segment_seconds").toInt(),
            0,
            86400
        );


    int recordingSegmentMaxMb =
        constrain(
            server.arg("recording_segment_max_mb").toInt(),
            0,
            4095
        );


    int recordingEventMaxSeconds =
        constrain(
            server.arg("recording_event_max_seconds").toInt(),
            0,
            86400
        );


    int recordingEventCooldownSeconds =
        constrain(
            server.arg("recording_event_cooldown_seconds").toInt(),
            0,
            86400
        );


    String sleepMode =
        server.arg("sleep_mode");

    sleepMode.trim();
    sleepMode.toLowerCase();

    if (
        sleepMode != "off" &&
        sleepMode != "light_sleep" &&
        sleepMode != "deep_sleep"
    ) {
        sleepMode = "off";
    }


    int sleepDelayMs =
        server.arg("sleep_delay_ms").toInt();

    sleepDelayMs =
        constrain(
            sleepDelayMs,
            0,
            60000
        );


    int bootloopProtection =
        server.hasArg("bootloop_protection")
        ? (server.arg("bootloop_protection").toInt() ? 1 : 0)
        : cfg_bootloop_protection;


    int transportCheckSeconds =
        constrain(
            server.arg("transport_check_seconds").toInt(),
            10,
            3600
        );

    int transportLightConfirmSeconds =
        constrain(
            server.arg("transport_light_confirm_seconds").toInt(),
            0,
            120
        );

    int transportInstallDelaySeconds =
        constrain(
            server.arg("transport_install_delay_seconds").toInt(),
            0,
            86400
        );

    int transportMaxDurationSeconds =
        constrain(
            server.arg("transport_max_duration_seconds").toInt(),
            3600,
            604800
        );

    int transportBlackThreshold =
        constrain(
            server.arg("transport_black_threshold").toInt(),
            0,
            255
        );


    int minFreeSpaceMb =
        server.arg("min_free_space_mb").toInt();

    if (minFreeSpaceMb < 0)
        minFreeSpaceMb = 0;


    String diskFullAction =
        server.arg("disk_full_action");

    diskFullAction.trim();
    diskFullAction.toLowerCase();

    if (
        diskFullAction != "rollover" &&
        diskFullAction != "stop"
    ) {
        diskFullAction = "rollover";
    }


    String wifiOnSystemStart =
        server.arg("wifi_on_system_start");

    wifiOnSystemStart.trim();
    wifiOnSystemStart.toLowerCase();

    if (
        wifiOnSystemStart != "off" &&
        wifiOnSystemStart != "on" &&
        wifiOnSystemStart != "on_missing_time"
    ) {
        wifiOnSystemStart = "off";
    }


    int wifiTimeoutSec =
        server.arg("wifi_timeout_sec").toInt();

    wifiTimeoutSec =
        constrain(
            wifiTimeoutSec,
            0,
            86400
        );


    String newPassword =
        server.arg("wifi_pass");

    // Empty password field means: keep current password.
    if (!newPassword.length()) {
        newPassword =
            cfg_wifi_pass;
    }


    int hotspotEnabled =
        server.arg("hotspot_enabled").toInt()
        ? 1
        : 0;


    String newHotspotPassword =
        server.arg("hotspot_password");

    // Empty password field means: keep current hotspot password.
    if (!newHotspotPassword.length()) {
        newHotspotPassword =
            cfg_hotspot_password;
    }


    int hotspotHidden =
        server.arg("hotspot_hidden").toInt()
        ? 1
        : 0;


    String webLanguage =
        server.hasArg("web_language")
        ? server.arg("web_language")
        : cfg_web_language;

    webLanguage.trim();
    webLanguage.toLowerCase();

    if (!uiLanguageSupported(webLanguage)) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid web_language"
        );
        return;
    }


    int webRecordingAutoPause =
        server.arg("web_recording_auto_pause").toInt()
        ? 1
        : 0;


    int webAuthEnabled =
        server.arg("web_auth_enabled").toInt()
        ? 1
        : 0;


    String webUsername =
        server.arg("web_username");

    webUsername.trim();


    String newWebPassword =
        server.arg("web_password");

    // Empty password field means: keep current web password.
    if (!newWebPassword.length()) {
        newWebPassword =
            cfg_web_password;
    }


    String camera =
        server.arg("camera");

    camera.trim();


    String resolution =
        server.arg("resolution");

    resolution.trim();


    {
        int performanceMaxFps =
            configRecordingPerformanceMaxFps(
                resolution,
                quality
            );

        if (
            configRecordingPerformanceLimit() > 0 &&
            performanceMaxFps > 0
        ) {
            uint32_t performanceLoad = 0;
            uint32_t performanceLimit = 0;

            if (!configRecordingPerformanceAllowed(
                    resolution,
                    fps,
                    quality,
                    performanceLoad,
                    performanceLimit
                )) {

                String message =
                    "Recording-Performance-Limit ueberschritten. "
                    "Fuer " +
                    resolution +
                    " und quality=" +
                    String(quality) +
                    " sind auf diesem Board maximal " +
                    String(performanceMaxFps) +
                    " fps zugelassen. Last=" +
                    String((unsigned long)performanceLoad) +
                    ", Limit=" +
                    String((unsigned long)performanceLimit) +
                    ".";

                server.send(
                    400,
                    "text/plain; charset=utf-8",
                    message
                );
                return;
            }
        }
    }


    String hostname =
        server.arg("hostname");

    hostname.trim();


    String timezone =
        server.arg("timezone");

    timezone.trim();


    String wifiSsid =
        server.arg("wifi_ssid");

    wifiSsid.trim();


    String logFile =
        server.arg("log_file");

    logFile.trim();


    int ledEnabled =
        server.arg("led_enabled").toInt()
        ? 1
        : 0;


    int debugEnabled =
        server.arg("debug_enabled").toInt()
        ? 1
        : 0;


    // Build one canonical config text. Exactly the same text is
    // written to LittleFS and, when requested, to the SD card.
    String text;

    text.reserve(
        3600
    );


    text += "camera=";
    text += camera;
    text += '\n';

    text += "resolution=";
    text += resolution;
    text += '\n';

    text += "fps=";
    text += String(fps);
    text += '\n';

    text += "quality=";
    text += String(quality);
    text += '\n';

    text += "camera_xclk_mhz=";
    text += String(cameraXclkMhz);
    text += '\n';

    text += "camera_auto_exposure=";
    text += String(cameraAutoExposure);
    text += '\n';

    text += "camera_ae_level=";
    text += String(cameraAeLevel);
    text += '\n';

    text += "camera_crop_zoom=";
    text += cameraCropZoom;
    text += '\n';

    text += "camera_crop_x=";
    text += String(cameraCropX);
    text += '\n';

    text += "camera_crop_y=";
    text += String(cameraCropY);
    text += '\n';

    text += "rotation=";
    text += String(rotation);
    text += '\n';

    text += "recording_format=";
    text += recordingFormat;
    text += '\n';

    text += "timestamp_enabled=";
    text += String(timestampEnabled);
    text += '\n';

    text += "recording_encryption=";
    text += String(recordingEncryption);
    text += '\n';

    text += "audio_enabled=";
    text += String(audioEnabled);
    text += '\n';

    text += "audio_expert_mode=";
    text += String(audioExpertMode);
    text += '\n';

    text += "audio_source=";
    text += audioSource;
    text += '\n';

    text += "audio_backend=";
    text += audioBackend;
    text += '\n';

    text += "audio_pdm_clk_pin=";
    text += String(audioPdmClkPin);
    text += '\n';

    text += "audio_pdm_data_pin=";
    text += String(audioPdmDataPin);
    text += '\n';

    text += "audio_i2s_bclk_pin=";
    text += String(audioI2sBclkPin);
    text += '\n';

    text += "audio_i2s_ws_pin=";
    text += String(audioI2sWsPin);
    text += '\n';

    text += "audio_i2s_data_pin=";
    text += String(audioI2sDataPin);
    text += '\n';

    text += "audio_i2s_mclk_pin=";
    text += String(audioI2sMclkPin);
    text += '\n';

    text += "audio_i2s_slot=";
    text += audioI2sSlot;
    text += '\n';

    text += "audio_sample_rate=";
    text += String(audioSampleRate);
    text += '\n';

    text += "audio_bits_per_sample=";
    text += String(audioBitsPerSample);
    text += '\n';

    text += "audio_channels=";
    text += String(audioChannels);
    text += '\n';

    text += "shooter_enabled=";
    text += String(shooterEnabled);
    text += '\n';

    text += "shooter_storage_format=";
    text += shooterStorageFormat;
    text += '\n';

    text += "shooter_interval_ms=";
    text += String(shooterIntervalMs);
    text += '\n';

    text += "shooter_dark_mean_min=";
    text += String(shooterDarkMeanMin);
    text += '\n';

    text += "shooter_min_change_pct=";
    text += String(shooterMinChangePct, 1);
    text += '\n';

    text += "shooter_force_save_seconds=";
    text += String(shooterForceSaveSeconds);
    text += '\n';

    text += "shooter_flush_seconds=";
    text += String(shooterFlushSeconds);
    text += '\n';

    text += "recording_not_before=";
    text += recordingNotBefore;
    text += '\n';

    // Motion recording enable/decision and image-motion settings are part of
    // the canonical config.txt. General Config saves must preserve them instead
    // of accidentally dropping them back to defaults.
    text += "motion_recording_enabled=";
    text += String(motionRecordingEnabled);
    text += '\n';

    text += "motion_recording_decision=";
    text += motionRecordingDecision;
    text += '\n';

    text += "image_motion_sensitivity=";
    text += String(cfg_image_motion_sensitivity);
    text += '\n';

    text += "image_motion_min_area_pct=";
    text += String(cfg_image_motion_min_area_pct);
    text += '\n';

    text += "image_motion_confirm_frames=";
    text += String(cfg_image_motion_confirm_frames);
    text += '\n';

    text += "image_motion_release_frames=";
    text += String(cfg_image_motion_release_frames);
    text += '\n';

    text += "image_motion_background_learning=";
    text += String(cfg_image_motion_background_learning);
    text += '\n';

    text += "image_motion_global_mean_delta=";
    text += String(cfg_image_motion_global_mean_delta);
    text += '\n';

    text += "image_motion_global_change_pct=";
    text += String(cfg_image_motion_global_change_pct);
    text += '\n';

    text += "image_motion_roi_mask=";
    text += cfg_image_motion_roi_mask;
    text += '\n';

    text += "post_record_ms=";
    text += String(postMs);
    text += '\n';

    text += "recording_segment_seconds=";
    text += String(recordingSegmentSeconds);
    text += '\n';

    text += "recording_segment_max_mb=";
    text += String(recordingSegmentMaxMb);
    text += '\n';

    text += "recording_event_max_seconds=";
    text += String(recordingEventMaxSeconds);
    text += '\n';

    text += "recording_event_cooldown_seconds=";
    text += String(recordingEventCooldownSeconds);
    text += '\n';

    text += "sleep_mode=";
    text += sleepMode;
    text += '\n';

    text += "sleep_delay_ms=";
    text += String(sleepDelayMs);
    text += '\n';

    text += "bootloop_protection=";
    text += String(bootloopProtection);
    text += '\n';

    // transport_mode is an operational flag controlled by the dedicated
    // activate/cancel actions. A general settings save preserves its state.
    text += "transport_mode=";
    text += String(cfg_transport_mode);
    text += '\n';

    text += "transport_check_seconds=";
    text += String(transportCheckSeconds);
    text += '\n';

    text += "transport_light_confirm_seconds=";
    text += String(transportLightConfirmSeconds);
    text += '\n';

    text += "transport_install_delay_seconds=";
    text += String(transportInstallDelaySeconds);
    text += '\n';

    text += "transport_max_duration_seconds=";
    text += String(transportMaxDurationSeconds);
    text += '\n';

    text += "transport_black_threshold=";
    text += String(transportBlackThreshold);
    text += '\n';

    text += "led_enabled=";
    text += String(ledEnabled);
    text += '\n';

    text += "min_free_space_mb=";
    text += String(minFreeSpaceMb);
    text += '\n';

    text += "disk_full_action=";
    text += diskFullAction;
    text += '\n';

    text += "wifi_on_system_start=";
    text += wifiOnSystemStart;
    text += '\n';

    text += "wifi_timeout_sec=";
    text += String(wifiTimeoutSec);
    text += '\n';

    text += "hostname=";
    text += hostname;
    text += '\n';

    text += "timezone=";
    text += timezone;
    text += '\n';

    text += "wifi_ssid=";
    text += wifiSsid;
    text += '\n';

    text += "wifi_pass=";
    text += newPassword;
    text += '\n';

    text += "hotspot_enabled=";
    text += String(hotspotEnabled);
    text += '\n';

    text += "hotspot_password=";
    text += newHotspotPassword;
    text += '\n';

    text += "hotspot_hidden=";
    text += String(hotspotHidden);
    text += '\n';

    text += "web_recording_auto_pause=";
    text += String(webRecordingAutoPause);
    text += '\n';

    text += "web_language=";
    text += webLanguage;
    text += '\n';

    text += "web_auth_enabled=";
    text += String(webAuthEnabled);
    text += '\n';

    text += "web_username=";
    text += webUsername;
    text += '\n';

    text += "web_password=";
    text += newWebPassword;
    text += '\n';

    text += "debug_enabled=";
    text += String(debugEnabled);
    text += '\n';

    text += "log_file=";
    text += logFile;
    text += '\n';


    String validationError;


    if (!configValidateText(
            text,
            validationError
        )) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Config validation failed: " +
            validationError
        );

        return;
    }


    // First-time recording encryption provisions the board-bound eFuse HMAC
    // root before the setting is persisted. This is intentionally one-time and
    // irreversible. Existing provisioned boards only perform a read/verify here.
    if (
        recordingEncryption &&
        !recordingCryptoReady()
    ) {
        if (recorderIsOpen()) {
            server.send(
                409,
                "text/plain; charset=utf-8",
                tr(UI_RECORDING_ENCRYPTION_RECORDING_ACTIVE)
            );
            return;
        }

        if (!recordingCryptoEnsureProvisioned()) {
            server.send(
                500,
                "text/plain; charset=utf-8",
                String(tr(UI_RECORDING_ENCRYPTION_PROVISION_FAILED)) +
                ": " +
                recordingCryptoKeyStatusName()
            );
            return;
        }
    }


    // No persistent flag is needed: physical presence of SD /config.txt is
    // the complete synchronization policy. configSaveText enforces this rule
    // again internally so a stale caller cannot accidentally create/suppress it.
    bool writeToSd =
        configSdAvailable() &&
        configSdPresent();


    String saveError;


    ConfigSaveResult result =
        configSaveText(
            text,
            writeToSd,
            saveError
        );


    switch (result) {

        case CONFIG_SAVE_BOTH:

            // Sleep policy is safe to apply immediately. FPS is also a
            // runtime recorder/pacing setting and can be applied safely here
            // because config saves are rejected while recording is active.
            // Camera sensor settings (resolution/quality/etc.) may still
            // require a reboot/reinit before the hardware uses them.
            cfg_fps =
                fps;

            cfg_motion_recording_decision =
                motionRecordingDecision;

            if (motionRecordingDecisionChanged) {
                imageMotionResetBackground();
            }

            cfg_sleep_mode =
                sleepMode;

            cfg_sleep_delay_ms =
                sleepDelayMs;

            // Keep the rendered WebConfig state in sync with the just-saved
            // value. Enabling is armed on the next cold boot; disabling stops
            // the runtime stability timer immediately and clears persisted
            // bootloop state on the next boot.
            cfg_bootloop_protection =
                bootloopProtection;

            cfg_transport_check_seconds =
                transportCheckSeconds;

            cfg_transport_light_confirm_seconds =
                transportLightConfirmSeconds;

            cfg_transport_install_delay_seconds =
                transportInstallDelaySeconds;

            cfg_transport_max_duration_seconds =
                transportMaxDurationSeconds;

            cfg_transport_black_threshold =
                transportBlackThreshold;

            cfg_recording_not_before =
                recordingNotBefore;

            cfg_recording_event_max_seconds =
                recordingEventMaxSeconds;

            cfg_recording_event_cooldown_seconds =
                recordingEventCooldownSeconds;

            // Encryption policy affects only NEW recordings and can therefore
            // be applied immediately after the config save. Existing files are
            // auto-detected independently when they are read.
            cfg_recording_encryption =
                recordingEncryption;

            cfg_audio_enabled =
                audioEnabled;

            cfg_audio_expert_mode =
                audioExpertMode;

            cfg_audio_source =
                audioSource;

            cfg_audio_backend =
                audioBackend;

            cfg_audio_pdm_clk_pin =
                audioPdmClkPin;

            cfg_audio_pdm_data_pin =
                audioPdmDataPin;

            cfg_audio_i2s_bclk_pin =
                audioI2sBclkPin;

            cfg_audio_i2s_ws_pin =
                audioI2sWsPin;

            cfg_audio_i2s_data_pin =
                audioI2sDataPin;

            cfg_audio_i2s_mclk_pin =
                audioI2sMclkPin;

            cfg_audio_i2s_slot =
                audioI2sSlot;

            cfg_audio_sample_rate =
                audioSampleRate;

            cfg_audio_bits_per_sample =
                audioBitsPerSample;

            cfg_audio_channels =
                audioChannels;

            // Shooter and motion enable state are safe to apply immediately.
            // WebConfig auto-pause still suppresses capture while this browser
            // session is active; the shooter resumes with the new settings when
            // the WebConfig pause is released. The shooter scheduler detects
            // enabled/interval changes and resets its slot state itself.
            cfg_shooter_enabled =
                shooterEnabled;

            cfg_shooter_storage_format =
                shooterStorageFormat;

            cfg_shooter_interval_ms =
                shooterIntervalMs;

            cfg_shooter_dark_mean_min =
                shooterDarkMeanMin;

            cfg_shooter_min_change_pct =
                shooterMinChangePct;

            cfg_shooter_force_save_seconds =
                shooterForceSaveSeconds;

            cfg_shooter_flush_seconds =
                shooterFlushSeconds;

            cfg_motion_recording_enabled =
                motionRecordingEnabled;

            cfg_timezone =
                timezone;

            cfg_web_recording_auto_pause =
                webRecordingAutoPause;

            cfg_web_language =
                webLanguage;

            setenv(
                "TZ",
                cfg_timezone.c_str(),
                1
            );
            tzset();

            server.sendHeader(
                "Location",
                "/?notice=config_saved_both"
            );

            server.send(
                303,
                "text/plain; charset=utf-8",
                ""
            );

            return;


        case CONFIG_SAVE_INTERNAL_ONLY:

            // The running system should honor the just-saved FPS and sleep
            // policy immediately even when only the internal fallback was
            // written. Config saves are rejected while recording is active,
            // so changing the recorder/pacing FPS here is safe.
            cfg_fps =
                fps;

            cfg_motion_recording_decision =
                motionRecordingDecision;

            if (motionRecordingDecisionChanged) {
                imageMotionResetBackground();
            }

            cfg_sleep_mode =
                sleepMode;

            cfg_sleep_delay_ms =
                sleepDelayMs;

            // Keep the rendered WebConfig state in sync with the just-saved
            // value. Enabling is armed on the next cold boot; disabling stops
            // the runtime stability timer immediately and clears persisted
            // bootloop state on the next boot.
            cfg_bootloop_protection =
                bootloopProtection;

            cfg_transport_check_seconds =
                transportCheckSeconds;

            cfg_transport_light_confirm_seconds =
                transportLightConfirmSeconds;

            cfg_transport_install_delay_seconds =
                transportInstallDelaySeconds;

            cfg_transport_max_duration_seconds =
                transportMaxDurationSeconds;

            cfg_transport_black_threshold =
                transportBlackThreshold;

            cfg_recording_not_before =
                recordingNotBefore;

            cfg_recording_event_max_seconds =
                recordingEventMaxSeconds;

            cfg_recording_event_cooldown_seconds =
                recordingEventCooldownSeconds;

            // Encryption policy affects only NEW recordings and can therefore
            // be applied immediately after the config save. Existing files are
            // auto-detected independently when they are read.
            cfg_recording_encryption =
                recordingEncryption;

            cfg_audio_enabled =
                audioEnabled;

            cfg_audio_expert_mode =
                audioExpertMode;

            cfg_audio_source =
                audioSource;

            cfg_audio_backend =
                audioBackend;

            cfg_audio_pdm_clk_pin =
                audioPdmClkPin;

            cfg_audio_pdm_data_pin =
                audioPdmDataPin;

            cfg_audio_i2s_bclk_pin =
                audioI2sBclkPin;

            cfg_audio_i2s_ws_pin =
                audioI2sWsPin;

            cfg_audio_i2s_data_pin =
                audioI2sDataPin;

            cfg_audio_i2s_mclk_pin =
                audioI2sMclkPin;

            cfg_audio_i2s_slot =
                audioI2sSlot;

            cfg_audio_sample_rate =
                audioSampleRate;

            cfg_audio_bits_per_sample =
                audioBitsPerSample;

            cfg_audio_channels =
                audioChannels;

            // Shooter and motion enable state are safe to apply immediately.
            // WebConfig auto-pause still suppresses capture while this browser
            // session is active; the shooter resumes with the new settings when
            // the WebConfig pause is released. The shooter scheduler detects
            // enabled/interval changes and resets its slot state itself.
            cfg_shooter_enabled =
                shooterEnabled;

            cfg_shooter_storage_format =
                shooterStorageFormat;

            cfg_shooter_interval_ms =
                shooterIntervalMs;

            cfg_shooter_dark_mean_min =
                shooterDarkMeanMin;

            cfg_shooter_min_change_pct =
                shooterMinChangePct;

            cfg_shooter_force_save_seconds =
                shooterForceSaveSeconds;

            cfg_shooter_flush_seconds =
                shooterFlushSeconds;

            cfg_motion_recording_enabled =
                motionRecordingEnabled;

            cfg_timezone =
                timezone;

            cfg_web_recording_auto_pause =
                webRecordingAutoPause;

            cfg_web_language =
                webLanguage;

            setenv(
                "TZ",
                cfg_timezone.c_str(),
                1
            );
            tzset();

            server.sendHeader(
                "Location",
                "/?notice=config_saved_internal"
            );

            server.send(
                303,
                "text/plain; charset=utf-8",
                ""
            );

            return;


        case CONFIG_SAVE_SD_FAILED:

            server.send(
                500,
                "text/plain; charset=utf-8",
                saveError +
                ". Internal shadow contains the new config."
            );

            return;


        case CONFIG_SAVE_INTERNAL_FAILED:
        default:

            server.send(
                500,
                "text/plain; charset=utf-8",
                saveError.length()
                ? saveError
                : String("Internal config save failed")
            );

            return;
    }
}





static bool transportConfigWriteToSd();


// -------------------------------------------------------------
// TRANSPORT CALIBRATION / SETTINGS PAGE
// -------------------------------------------------------------

static int transportClampByte(int value)
{
    if (value < 0)
        return 0;

    if (value > 255)
        return 255;

    return value;
}


static bool transportPauseRecordingForOperation(
    const char *operation
)
{
    // Entering the transport workflow is explicit maintenance activity.
    // Pause automatic starts first, then finish any file that was already open.
    setRecordingAutomationPaused(
        true,
        operation
    );

    if (recording)
        stopRecording();

    if (recorderIsOpen()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Die laufende Aufnahme wird noch beendet. Bitte in wenigen Sekunden erneut versuchen."
        );
        return false;
    }

    renewRecordingPauseLease(true);
    return true;
}


static void handleTransportMeasure()
{
    if (!transportPauseRecordingForOperation("transport calibration"))
        return;

    String measurementSource =
        server.arg("source");

    measurementSource.trim();

    if (!measurementSource.length())
        measurementSource = "unknown";

    consoleWrite(
        "TRANSPORT",
        "Black-reference measurement started | source=" +
        measurementSource
    );

    logWrite(
        "Transport black-reference measurement started | source=" +
        measurementSource
    );

    if (webConfigCameraPreviewActive()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Live Preview ist noch aktiv. Bitte Preview schließen und die Messung erneut starten."
        );
        return;
    }

    float referenceMean = 0.0f;
    uint8_t referenceP95 = 0;
    String measureError;

    const uint8_t sampleCount = 10;

    bool measured =
        cameraMeasureTransportBlackReference(
            referenceMean,
            referenceP95,
            sampleCount,
            measureError
        );

    // The measurement is synchronous. Refresh the dedicated transport lease only
    // after the camera has been fully restored.
    renewRecordingPauseLease(true);

    if (!measured) {
        String failure =
            measureError.length()
            ? measureError
            : String("Schwarzmessung fehlgeschlagen");

        consoleWrite(
            "TRANSPORT",
            "Black-reference measurement FAILED | source=" +
            measurementSource +
            " | " +
            failure
        );

        logWrite(
            "Transport black-reference measurement FAILED | source=" +
            measurementSource +
            " | " +
            failure
        );

        server.sendHeader("Cache-Control", "no-store");
        server.send(
            500,
            "text/plain; charset=utf-8",
            failure
        );
        return;
    }

    int suggestedMean =
        transportClampByte(
            (int)ceilf(referenceMean) + 10
        );

    int suggestedP95 =
        transportClampByte(
            (int)referenceP95 + 15
        );

    if (suggestedP95 < suggestedMean)
        suggestedP95 = suggestedMean;

    bool suspiciouslyBright =
        referenceMean > 40.0f ||
        referenceP95 > 70;

    String measurementSummary =
        "Transport black-reference measurement complete | source=" +
        measurementSource +
        " | samples=" +
        String((unsigned)sampleCount) +
        " | mean=" +
        String(referenceMean, 1) +
        " | p95=" +
        String((unsigned)referenceP95) +
        " | suggested_mean=" +
        String(suggestedMean) +
        " | suggested_p95=" +
        String(suggestedP95) +
        " | dark_check=" +
        String(suspiciouslyBright ? "WARN_BRIGHT" : "OK");

    consoleWrite(
        "TRANSPORT",
        measurementSummary
    );

    logWrite(
        measurementSummary
    );

    String json;
    json.reserve(192);
    json += "{\"reference_mean\":";
    json += String(referenceMean, 1);
    json += ",\"reference_p95\":";
    json += String((unsigned)referenceP95);
    json += ",\"suggested_mean\":";
    json += String(suggestedMean);
    json += ",\"suggested_p95\":";
    json += String(suggestedP95);
    json += ",\"suspiciously_bright\":";
    json += suspiciouslyBright ? "true" : "false";
    json += ",\"sample_count\":";
    json += String((unsigned)sampleCount);
    json += "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(
        200,
        "application/json",
        json
    );
}


static void handleTransportPage()
{
    bool justSaved =
        server.hasArg("notice") &&
        server.arg("notice") == "saved";

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Transportsicherung</h2>"
        "<p>Kamera abkleben, Grenzwerte prüfen, speichern und Transportmodus aktivieren.</p>"
        "</div></div>";

    if (justSaved) {
        html +=
            "<section class='settings-section' style='border-left:5px solid #087a00;background:#f3fbf2'>"
            "<b>Transportwerte gespeichert.</b> Die Werte sind jetzt in der persistenten config.txt hinterlegt."
            "</section>";
    }

    html +=
        "<section class='settings-section'>"
        "<h3>Aktuelle Schwarzmessung</h3>"
        "<p class='muted'>Beim Öffnen dieser Seite werden automatisch 10 Messframes mit exakt derselben "
        "160x120-Graustufen-Methode wie beim späteren Transport-Wake aufgenommen. Eine laufende Aufnahme "
        "wird vorher sauber beendet und die Aufnahmeautomatik während der Transportvorbereitung pausiert.</p>"
        "<div class='dashboard-grid'>"
        "<div class='dash-card'><div class='card-label'>Durchschnittliche Helligkeit</div>"
        "<div id='transportReferenceMean' class='card-value'>--</div>"
        "<div class='card-note'>höchster Durchschnittswert aus 10 Messbildern</div></div>"
        "<div class='dash-card'><div class='card-label'>Helle Bildbereiche (P95)</div>"
        "<div id='transportReferenceP95' class='card-value'>--</div>"
        "<div class='card-note'>erkennt hellere Bereiche, die im Durchschnitt untergehen können</div></div>"
        "<div class='dash-card'><div class='card-label'>Sicherheitsreserve</div><div class='card-value'>+10 / +15</div>"
        "<div class='card-note'>Reserve über den gemessenen Referenzwerten</div></div>"
        "</div>"
        "<p><span id='transportMeasureState' class='status-pill warn'>Messung wird vorbereitet ...</span></p>"
        "<p id='transportMeasureErrorText' class='muted' hidden></p>"
        "</section>"
        "<form method='POST' action='/transport_save'>"
        "<section class='settings-section'>"
        "<h3>Grenzwerte und Zeiten</h3>"
        "<p class='muted'>Die aktuelle Config-Version verwendet einen gemeinsamen Schwarzwert. "
        "Der automatische Vorschlag basiert auf der gemessenen durchschnittlichen Bildhelligkeit plus Reserve.</p>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportBlackThreshold'><b>Schwarzgrenze</b></label><br>"
        "<input id='transportBlackThreshold' name='transport_black_threshold' "
        "type='number' min='0' max='255' value='" +
        String(cfg_transport_black_threshold) +
        "'> <small id='transportThresholdSuggestion'>Automatischer Vorschlag wird gemessen ...</small>"
        "<div class='muted'>Wie hell das vollständig abgedeckte Bild noch sein darf. "
        "Ein kleinerer Wert bedeutet eine strengere Schwarzerkennung.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportCheckSeconds'><b>Prüfintervall während des Transports</b></label><br>"
        "<input id='transportCheckSeconds' name='transport_check_seconds' type='number' min='10' max='3600' value='" +
        String(cfg_transport_check_seconds) +
        "'> <small>Sekunden · Standard 120 s</small>"
        "<div class='muted'>Nach diesem Abstand wacht SensorForge kurz auf und prüft, ob die Kamera weiterhin abgedeckt ist. "
        "Dazwischen bleibt das Gerät im stromsparenden Timer-Deep-Sleep.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportLightConfirmSeconds'><b>Bestätigungszeit nach erkannter Helligkeit</b></label><br>"
        "<input id='transportLightConfirmSeconds' name='transport_light_confirm_seconds' type='number' min='0' max='120' value='" +
        String(cfg_transport_light_confirm_seconds) +
        "'> <small>Sekunden</small>"
        "<div class='muted'>Die Abdeckung gilt erst als entfernt, wenn die Kamera über diesen Zeitraum mehrfach hell bleibt. "
        "Kurze Lichtblitze lösen den Transportmodus dadurch nicht versehentlich.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportInstallDelaySeconds'><b>Montage-Wartezeit nach Entfernen der Abdeckung</b></label><br>"
        "<input id='transportInstallDelaySeconds' name='transport_install_delay_seconds' type='number' min='0' max='86400' value='" +
        String(cfg_transport_install_delay_seconds) +
        "'> <small>Sekunden</small>"
        "<div class='muted'>Zeit für Montage und Verlassen des Bildbereichs, bevor SensorForge wieder in den normalen Betrieb zurückkehrt.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportMaxDurationSeconds'><b>Maximale Transportdauer</b></label><br>"
        "<input id='transportMaxDurationSeconds' name='transport_max_duration_seconds' type='number' min='3600' max='604800' value='" +
        String(cfg_transport_max_duration_seconds) +
        "'> <small>Sekunden · Standard 86400 s = 24 h</small>"
        "<div class='muted'>Harte Sicherheitsgrenze für die gesamte Transportphase. Nach Ablauf wechselt SensorForge unabhängig von der Lichtmessung in den Normalbetrieb.</div></div>";

    html +=
        "<div class='form-actions'><button id='transportSaveButton' class='primary' type='submit'>Speichern</button>"
        "<button id='transportRemeasureButton' type='button'>Neu messen</button></div>"
        "</section></form>";

    html +=
        "<section class='settings-section' style='border-left:5px solid #d97706'>"
        "<h3>Transportmodus</h3>"
        "<p>Status: <span class='status-pill " +
        String(cfg_transport_mode ? "warn" : "ok") +
        "'>" +
        String(cfg_transport_mode ? "AKTIV" : "AUS") +
        "</span></p>";

    if (!cfg_transport_mode) {
        html +=
            "<p class='muted'>Nach dem Aktivieren wird transport_mode=1 persistent gespeichert und SensorForge neu gestartet. "
            "Beim Boot springt die Firmware direkt in den Timer-only Transportpfad.</p>"
            "<form method='POST' action='/transport_activate' "
            "onsubmit=\"return confirm('Grenzwerte gespeichert und Kamera vollständig schwarz abgeklebt? Transportmodus jetzt aktivieren?');\">"
            "<button id='transportActivateButton' class='primary' type='submit'>Transportmodus aktivieren</button>"
            "</form>";
    } else {
        html +=
            "<form method='POST' action='/transport_cancel' "
            "onsubmit=\"return confirm('Transportmodus wirklich deaktivieren?');\">"
            "<button type='submit'>Transportmodus deaktivieren</button>"
            "</form>";
    }

    html +=
        "</section>";

    // This modal is intentionally present and visible in the very first HTML
    // response. The slow camera operation is started only afterwards via fetch,
    // so the browser can explain the delay instead of looking frozen.
    html +=
        "<style>"
        ".transport-spinner{width:34px;height:34px;margin:4px 0 12px;border:4px solid #d8dee6;"
        "border-top-color:#2563eb;border-radius:50%;animation:transportSpin .8s linear infinite;}"
        "@keyframes transportSpin{to{transform:rotate(360deg);}}"
        "</style>"
        "<div id='transportMeasureModal' class='modal-backdrop'>"
        "<div class='modal-card' role='dialog' aria-modal='true' aria-labelledby='transportMeasureTitle'>"
        "<span class='status-pill warn'>KAMERA-MESSUNG</span>"
        "<div id='transportMeasureSpinner' class='transport-spinner' aria-hidden='true'></div>"
        "<h3 id='transportMeasureTitle'>Messung wird durchgeführt ...</h3>"
        "<p id='transportMeasureText'>Eine laufende Aufnahme wird zuerst sauber beendet. Anschließend wird der "
        "Schwarzwert der abgedeckten Kamera gemessen. Bitte Kamera abgedeckt lassen. Dies kann einige Sekunden dauern.</p>"
        "<div class='modal-actions'><button id='transportMeasureCloseButton' type='button' hidden>Schließen</button></div>"
        "</div></div>";

    html +=
        "<script>"
        "(function(){"
        "var modal=document.getElementById('transportMeasureModal');"
        "var spinner=document.getElementById('transportMeasureSpinner');"
        "var title=document.getElementById('transportMeasureTitle');"
        "var text=document.getElementById('transportMeasureText');"
        "var closeBtn=document.getElementById('transportMeasureCloseButton');"
        "var remeasureBtn=document.getElementById('transportRemeasureButton');"
        "var saveBtn=document.getElementById('transportSaveButton');"
        "var activateBtn=document.getElementById('transportActivateButton');"
        "var meanEl=document.getElementById('transportReferenceMean');"
        "var p95El=document.getElementById('transportReferenceP95');"
        "var stateEl=document.getElementById('transportMeasureState');"
        "var errorEl=document.getElementById('transportMeasureErrorText');"
        "var thresholdInput=document.getElementById('transportBlackThreshold');"
        "var thresholdSuggestion=document.getElementById('transportThresholdSuggestion');"
        "var preserveSavedValues=" + String(justSaved ? "true" : "false") + ";"
        "var measuring=false;"
        "function setControlsDisabled(v){"
        "if(remeasureBtn)remeasureBtn.disabled=v;"
        "if(saveBtn)saveBtn.disabled=v;"
        "if(activateBtn)activateBtn.disabled=v;"
        "}"
        "function showBusy(){"
        "measuring=true;setControlsDisabled(true);"
        "if(modal)modal.hidden=false;if(spinner)spinner.hidden=false;if(closeBtn)closeBtn.hidden=true;"
        "if(title)title.textContent='Messung wird durchgeführt ...';"
        "if(text)text.textContent='Eine laufende Aufnahme wird zuerst sauber beendet. Anschließend wird der Schwarzwert der abgedeckten Kamera gemessen. Bitte Kamera abgedeckt lassen. Dies kann einige Sekunden dauern.';"
        "if(stateEl){stateEl.className='status-pill warn';stateEl.textContent='Messung läuft ...';}"
        "if(errorEl){errorEl.hidden=true;errorEl.textContent='';}"
        "}"
        "function finishControls(){measuring=false;setControlsDisabled(false);}"
        "function hideModal(){if(modal)modal.hidden=true;}"
        "function applyMeasurement(d,applySuggestions){"
        "var m=Number(d.reference_mean),p=Number(d.reference_p95),sm=Number(d.suggested_mean),sp=Number(d.suggested_p95);"
        "if(meanEl)meanEl.textContent=isFinite(m)?m.toFixed(1):'--';"
        "if(p95El)p95El.textContent=isFinite(p)?String(p):'--';"
        "if(thresholdSuggestion)thresholdSuggestion.textContent='Automatischer Vorschlag: '+sm+' (gemessene Durchschnittshelligkeit + 10 Reserve)';"
        "if(applySuggestions&&thresholdInput)thresholdInput.value=sm;"
        "if(stateEl){stateEl.className='status-pill '+(d.suspiciously_bright?'warn':'ok');"
        "stateEl.textContent=d.suspiciously_bright?'Abgedecktes Bild ungewöhnlich hell - Tape/Sitz prüfen':'Messung plausibel dunkel';}"
        "}"
        "function measurementFailed(message){"
        "if(stateEl){stateEl.className='status-pill danger';stateEl.textContent='Schwarzmessung fehlgeschlagen';}"
        "if(errorEl){errorEl.hidden=false;errorEl.textContent=message||'Unbekannter Fehler';}"
        "if(spinner)spinner.hidden=true;if(closeBtn)closeBtn.hidden=false;"
        "if(title)title.textContent='Messung fehlgeschlagen';"
        "if(text)text.textContent=message||'Die Schwarzmessung konnte nicht durchgeführt werden. Die gespeicherten Grenzwerte bleiben unverändert.';"
        "}"
        "function runMeasurement(applySuggestions,source){"
        "if(measuring)return;showBusy();"
        "fetch('/transport_measure?source='+encodeURIComponent(source||'unknown')+'&t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin'})"
        ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
        ".then(function(d){applyMeasurement(d,applySuggestions);hideModal();finishControls();})"
        ".catch(function(e){measurementFailed(e&&e.message?e.message:String(e));finishControls();});"
        "}"
        "function keepTransportPause(){"
        "fetch('/recording_pause_keepalive?transport=1&t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "function releaseTransportHold(){"
        "fetch('/transport_pause_release?t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "if(closeBtn)closeBtn.addEventListener('click',hideModal);"
        "if(remeasureBtn)remeasureBtn.addEventListener('click',function(){runMeasurement(true,'manual-remeasure');});"
        // Unlike the normal maintenance pause, the transport preparation page
        // also renews while hidden. Closing/navigating away stops this timer and
        // the existing 35 s safety timeout resumes automatic recording.
        "keepTransportPause();"
        "setInterval(keepTransportPause,10000);"
        "window.addEventListener('pagehide',releaseTransportHold);"
        "setTimeout(function(){runMeasurement(!preserveSavedValues,'page-open');},0);"
        "})();"
        "</script>";

    html += htmlFooter();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}


static void handleTransportSave()
{
    if (!transportPauseRecordingForOperation("transport settings save"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    int checkSeconds =
        server.arg("transport_check_seconds").toInt();

    int lightConfirmSeconds =
        server.arg("transport_light_confirm_seconds").toInt();

    int installDelaySeconds =
        server.arg("transport_install_delay_seconds").toInt();

    int maxDurationSeconds =
        server.arg("transport_max_duration_seconds").toInt();

    int blackThreshold =
        server.arg("transport_black_threshold").toInt();

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportSettings(
            checkSeconds,
            lightConfirmSeconds,
            installDelaySeconds,
            maxDurationSeconds,
            blackThreshold,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            error.length()
                ? error
                : String("Transportwerte konnten nicht gespeichert werden")
        );
        return;
    }

    String savedSummary =
        "Transport settings saved | black<=" +
        String(blackThreshold) +
        " | check=" +
        String(checkSeconds) +
        " s | light_confirm=" +
        String(lightConfirmSeconds) +
        " s | install_delay=" +
        String(installDelaySeconds) +
        " s | max_duration=" +
        String(maxDurationSeconds) +
        " s";

    consoleWrite(
        "TRANSPORT",
        savedSummary
    );

    logWrite(
        savedSummary
    );

    server.sendHeader(
        "Location",
        "/transport?notice=saved"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// TRANSPORT MODE
// -------------------------------------------------------------

static bool transportConfigWriteToSd()
{
    configRefreshSdStatus();

    return
        configSdAvailable() &&
        configSdPresent();
}


static void sendTransportTransitionPage(
    bool activating
)
{
    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div><h2>" +
        String(
            activating
            ? "Transportmodus aktiviert"
            : "Transportmodus deaktiviert"
        ) +
        "</h2></div></div>";

    html +=
        "<section class='settings-section' style='text-align:center'>";

    if (activating) {
        html +=
            "<p><b>Kamera muss jetzt vollständig schwarz abgeklebt sein.</b></p>"
            "<p class='muted'>SensorForge startet neu. Radar/PIR werden danach nicht als Wake-Quelle verwendet; WLAN bleibt aus. "
            "Das Gerät wacht nur per Timer auf, prüft die Kamera und schläft bei weiterhin schwarzem Bild wieder ein. "
            "Nach bestätigtem Entfernen der Abdeckung läuft die konfigurierte Installations-Wartezeit ab. "
            "Spätestens nach der maximalen Transportdauer wird unabhängig von der Lichtmessung in den Normalbetrieb gewechselt.</p>";
    } else {
        html +=
            "<p><b>Transportmodus ist deaktiviert.</b></p>"
            "<p class='muted'>SensorForge startet neu und kehrt in den normalen Betrieb zurück.</p>";
    }

    html +=
        "<div class='countdown' id='transportCountdown'>3</div>"
        "<div class='muted'>Sekunden bis zum Neustart</div>"
        "</section>"
        "<script>"
        "(function(){var s=3;var e=document.getElementById('transportCountdown');"
        "setInterval(function(){if(s>0)s--;if(e)e.textContent=s;},1000);})();"
        "</script>";

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleTransportActivate()
{
    if (!transportPauseRecordingForOperation("transport mode activation"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportMode(
            1,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Transportmodus konnte nicht gespeichert werden")
        );
        return;
    }

    consoleWrite(
        "TRANSPORT",
        "Transport mode activated - reboot scheduled"
    );

    logWrite(
        "Transport mode activated | check=" +
        String(cfg_transport_check_seconds) +
        " s | light_confirm=" +
        String(cfg_transport_light_confirm_seconds) +
        " s | install_delay=" +
        String(cfg_transport_install_delay_seconds) +
        " s | max_duration=" +
        String(cfg_transport_max_duration_seconds) +
        " s | black<=" +
        String(cfg_transport_black_threshold)
    );

    sendTransportTransitionPage(
        true
    );

    rebootScheduled =
        true;

    rebootAtMs =
        millis() +
        3000UL;
}


static void handleTransportCancel()
{
    if (rejectWhileRecording("transport mode cancel"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportMode(
            0,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Transportmodus konnte nicht deaktiviert werden")
        );
        return;
    }

    consoleWrite(
        "TRANSPORT",
        "Transport mode cancelled - reboot scheduled"
    );

    logWrite(
        "Transport mode cancelled"
    );

    sendTransportTransitionPage(
        false
    );

    rebootScheduled =
        true;

    rebootAtMs =
        millis() +
        3000UL;
}


// -------------------------------------------------------------
// PIR SIMULATION
// -------------------------------------------------------------

static void handleSimulateMotion()
{
    int seconds =
        server.arg("seconds").toInt();

    if (seconds < 1)
        seconds = 5;

    if (seconds > 3600)
        seconds = 3600;

    simulationDurationSeconds =
        (uint32_t)seconds;

    simulatedMotionUntilMs =
        millis() +
        simulationDurationSeconds * 1000UL;

    Serial.printf(
        "PIR simulation started: %lu seconds\n",
        (unsigned long)simulationDurationSeconds
    );

    logWrite(
        "PIR simulation started: " +
        String(simulationDurationSeconds) +
        " s"
    );

    server.sendHeader(
        "Location",
        "/"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// SD maintenance routes and long-running jobs live in web_sd_maintenance.cpp.


// -------------------------------------------------------------
// LIVE PREVIEW
// -------------------------------------------------------------


static bool parseCameraCropRequest(
    String &zoom,
    int &positionX,
    int &positionY,
    String &error
)
{
    error = "";

    zoom =
        server.arg("zoom");

    zoom.trim();

    if (
        zoom != "1.0" &&
        zoom != "1.5" &&
        zoom != "2.0"
    ) {

        error =
            "zoom must be 1.0, 1.5 or 2.0";

        return false;
    }


    String xText =
        server.arg("x");

    String yText =
        server.arg("y");

    if (
        (
            xText != "0" &&
            xText != "1" &&
            xText != "2"
        ) ||
        (
            yText != "0" &&
            yText != "1" &&
            yText != "2"
        )
    ) {

        error =
            "crop position must be 0, 1 or 2";

        return false;
    }


    positionX =
        xText.toInt();

    positionY =
        yText.toInt();

    return true;
}


static void handleCameraCropApply()
{
    if (rejectWhileRecording("camera crop"))
        return;

    String zoom;
    int positionX = 1;
    int positionY = 1;
    String error;

    if (!parseCameraCropRequest(
            zoom,
            positionX,
            positionY,
            error
        )) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            error
        );
        return;
    }


    // A crop test is a Live Preview operation. Keep the preview gate alive
    // before touching sensor geometry so automatic recording cannot start
    // between the crop change and the next snapshot request.
    noteCameraPreviewActivity();

    // Treat the sensor as temporary before programming it. Even a failed raw
    // crop attempt may have changed some geometry registers before returning
    // an error, so the persisted framing must be restored before the gate can
    // later be released.
    cameraPreviewCropTemporary = true;


    if (!cameraApplyCropRuntime(
            zoom,
            positionX,
            positionY,
            error
        )) {

        String applyError =
            error;

        restoreSavedCameraCrop();

        server.send(
            409,
            "text/plain; charset=utf-8",
            applyError
        );
        return;
    }


    cameraPreviewCropTemporary =
        zoom != cfg_camera_crop_zoom ||
        positionX != cfg_camera_crop_x ||
        positionY != cfg_camera_crop_y;


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json; charset=utf-8",
        String("{\"ok\":true,\"saved\":") +
        (cameraPreviewCropTemporary ? "false" : "true") +
        ",\"zoom\":\"" +
        zoom +
        "\",\"x\":" +
        String(positionX) +
        ",\"y\":" +
        String(positionY) +
        "}"
    );
}


static void handleCameraCropSave()
{
    if (rejectWhileRecording("camera crop save"))
        return;

    String zoom;
    int positionX = 1;
    int positionY = 1;
    String error;

    if (!parseCameraCropRequest(
            zoom,
            positionX,
            positionY,
            error
        )) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            error
        );
        return;
    }


    noteCameraPreviewActivity();

    // As with the temporary test path, consider the sensor state temporary
    // before touching raw geometry. A partial driver failure is restored to
    // the last persisted crop before we report the error.
    cameraPreviewCropTemporary =
        true;


    // Validate against the real detected sensor and current output geometry
    // before making the setting persistent.
    if (!cameraApplyCropRuntime(
            zoom,
            positionX,
            positionY,
            error
        )) {

        String applyError =
            error;

        restoreSavedCameraCrop();

        server.send(
            409,
            "text/plain; charset=utf-8",
            applyError
        );
        return;
    }


    configRefreshSdStatus();

    bool writeToSd =
        configSdAvailable() &&
        configSdPresent();


    ConfigSaveResult result =
        configSaveCameraCrop(
            zoom,
            positionX,
            positionY,
            writeToSd,
            error
        );


    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {

        // configSaveCameraCrop leaves the runtime cfg_* values unchanged on
        // failure, so restore the last persistent framing before releasing the
        // operator back to the preview.
        restoreSavedCameraCrop();

        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("camera crop save failed")
        );
        return;
    }


    cameraPreviewCropTemporary =
        false;


    String storageText =
        result == CONFIG_SAVE_BOTH
        ? "SD + interner Shadow"
        : "interner Shadow";


    consoleWrite(
        "CAMERA",
        "Crop saved | zoom=" +
        zoom +
        " x=" +
        String(positionX) +
        " y=" +
        String(positionY) +
        " | " +
        storageText
    );

    logWrite(
        "Camera crop saved | zoom=" +
        zoom +
        " x=" +
        String(positionX) +
        " y=" +
        String(positionY)
    );


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json; charset=utf-8",
        String("{\"ok\":true,\"saved\":true,\"zoom\":\"") +
        zoom +
        "\",\"x\":" +
        String(positionX) +
        ",\"y\":" +
        String(positionY) +
        ",\"storage\":\"" +
        storageText +
        "\"}"
    );
}


static String imageMotionJsonEscape(const String &input)
{
    String out;
    out.reserve(input.length() + 8);

    for (size_t i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if ((uint8_t)c >= 0x20U) {
            out += c;
        }
    }

    return out;
}


static int imageMotionArgInt(const char *name, int fallback)
{
    if (!server.hasArg(name))
        return fallback;

    String value = server.arg(name);
    value.trim();

    if (!value.length())
        return fallback;

    return value.toInt();
}


static void handleImageMotionSave()
{
    String roiMask = server.arg("roi");
    roiMask.trim();
    roiMask.toLowerCase();

    String error;
    ConfigSaveResult result = configSaveImageMotion(
        imageMotionArgInt("sensitivity", cfg_image_motion_sensitivity),
        imageMotionArgInt("min_area", cfg_image_motion_min_area_pct),
        imageMotionArgInt("confirm", cfg_image_motion_confirm_frames),
        imageMotionArgInt("release", cfg_image_motion_release_frames),
        imageMotionArgInt("learning", cfg_image_motion_background_learning),
        imageMotionArgInt("global_mean", cfg_image_motion_global_mean_delta),
        imageMotionArgInt("global_change", cfg_image_motion_global_change_pct),
        roiMask,
        true,
        error
    );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            400,
            "application/json; charset=utf-8",
            String("{\"ok\":false,\"error\":\"") +
            imageMotionJsonEscape(error.length() ? error : String("save failed")) +
            "\"}"
        );
        return;
    }

    // Settings/ROI changes invalidate the old scene model deliberately.
    imageMotionResetBackground();

    server.send(
        200,
        "application/json; charset=utf-8",
        String("{\"ok\":true,\"storage\":\"") +
        (result == CONFIG_SAVE_BOTH ? "sd+internal" : "internal") +
        "\"}"
    );
}


static void handleImageMotionTest()
{
    if (recorderIsOpen()) {
        server.send(
            409,
            "application/json; charset=utf-8",
            "{\"ok\":false,\"error\":\"recording active\"}"
        );
        return;
    }

    // Keep the same camera-ownership gate used by Live Preview. This guarantees
    // that a diagnostic request can never race an automatic recording start.
    noteCameraPreviewActivity();

    String json;
    String error;

    if (!imageMotionWebTest(json, error)) {
        server.send(
            500,
            "application/json; charset=utf-8",
            String("{\"ok\":false,\"error\":\"") +
            imageMotionJsonEscape(error) +
            "\"}"
        );
        return;
    }

    server.send(
        200,
        "application/json; charset=utf-8",
        String("{\"ok\":true,\"diagnostics\":") + json + "}"
    );
}


static void handleImageMotionResetBackground()
{
    imageMotionResetBackground();
    server.send(
        200,
        "application/json; charset=utf-8",
        "{\"ok\":true}"
    );
}


static void handleImageMotionStatus()
{
    String json;
    json.reserve(900);
    json =
        String("{\"ok\":true,\"mode\":\"") +
        cfg_motion_recording_decision +
        "\",\"confirm_required\":" +
        String(cfg_image_motion_confirm_frames) +
        ",\"release_required\":" +
        String(cfg_image_motion_release_frames) +
        ",\"area_limit_pct\":" +
        String(cfg_image_motion_min_area_pct) +
        ",\"diag_count\":" +
        String(imageMotionDiagnosticCount()) +
        ",\"diag_capacity\":" +
        String(imageMotionDiagnosticCapacity()) +
        ",\"analysis_stamp_ms\":" +
        String(imageMotionLastAnalysisCompletedMs()) +
        ",\"diagnostics\":" +
        imageMotionDiagnosticsJson() +
        "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(
        200,
        "application/json; charset=utf-8",
        json
    );
}



static String imageMotionDiagnosticTimestamp(
    const ImageMotionDiagnosticSample &sample
)
{
    if (sample.epochSec < 1577836800UL)
        return "-";

    time_t seconds = (time_t)sample.epochSec;
    struct tm localTime;
    localtime_r(&seconds, &localTime);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);

    char millisPart[8];
    snprintf(
        millisPart,
        sizeof(millisPart),
        ".%03u",
        (unsigned int)sample.epochMs
    );

    return String(buffer) + String(millisPart);
}

static String imageMotionDiagnosticMaskHex(
    const uint8_t mask[IMAGE_MOTION_ROI_BYTES]
)
{
    static const char hex[] = "0123456789abcdef";
    String out;
    out.reserve(IMAGE_MOTION_ROI_HEX_CHARS);

    for (uint8_t i = 0; i < IMAGE_MOTION_ROI_BYTES; ++i) {
        out += hex[(mask[i] >> 4) & 0x0FU];
        out += hex[mask[i] & 0x0FU];
    }

    return out;
}

static String imageMotionDiagnosticDeci(int16_t value)
{
    bool negative = value < 0;
    uint16_t magnitude = negative
        ? (uint16_t)(-(int32_t)value)
        : (uint16_t)value;

    return
        String(negative ? "-" : "") +
        String(magnitude / 10U) +
        "." +
        String(magnitude % 10U);
}

static void handleImageMotionDiagnosticDownload()
{
    uint16_t count = imageMotionDiagnosticCount();
    uint16_t capacity = imageMotionDiagnosticCapacity();

    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"sensorforge_image_motion_diagnostic.txt\""
    );
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain; charset=utf-8", "");

    String line;
    line.reserve(1900);

    line =
        "# SensorForge Image Motion RAM Diagnostic\n"
        "# RAM-only ring buffer; no SD/main-log writes.\n"
        "# One row is one analyzed JPEG frame; oldest retained sample first.\n"
        "# changed = blocks above the real adaptive per-block threshold against the learned background.\n"
        "# frame_delta = diagnostic-only difference to the immediately previous analyzed frame; it does NOT yet influence motion_active/state.\n"
        "# diff_ge_N = ROI blocks whose absolute gray difference from the learned background is >= N, before the adaptive threshold is applied.\n"
        "# frame_diff_ge_N = ROI blocks whose frame-to-frame gray difference is >= N.\n"
        "# changed_mask_hex/frame_changed_mask_hex = one bit per 20x15 grid cell; bit 0 is cell 0.\n";
    server.sendContent(line);

    line =
        "# samples=" + String(count) +
        " capacity=" + String(capacity) +
        " mode=" + cfg_motion_recording_decision +
        " resolution=" + cfg_resolution +
        " crop_zoom=" + cfg_camera_crop_zoom +
        " crop_x=" + String(cfg_camera_crop_x) +
        " crop_y=" + String(cfg_camera_crop_y) +
        " sensitivity=" + String(cfg_image_motion_sensitivity) +
        " min_area_pct=" + String(cfg_image_motion_min_area_pct) +
        " confirm_frames=" + String(cfg_image_motion_confirm_frames) +
        " release_frames=" + String(cfg_image_motion_release_frames) +
        " background_learning=" + String(cfg_image_motion_background_learning) +
        " global_mean_delta=" + String(cfg_image_motion_global_mean_delta) +
        " global_change_pct=" + String(cfg_image_motion_global_change_pct) +
        " roi_mask=" + cfg_image_motion_roi_mask +
        "\n";
    server.sendContent(line);

    server.sendContent(
        "seq\ttime\tuptime_ms\tsource_px\tstate\treject\tbackground_ready\tmotion_active\tconfirm\trelease\tactive_blocks\tchanged_blocks\ttotal_changed_pct\tlargest_cluster\tcluster_pct\tminimum_blocks\tbase_threshold\tdynamic_threshold_min\tdynamic_threshold_avg\tdynamic_threshold_max\tmean_abs_diff\tmax_abs_diff\tframe_delta_ready\tframe_interval_ms\tframe_threshold\tframe_changed_blocks\tframe_changed_pct\tframe_largest_cluster\tframe_cluster_pct\tframe_mean_abs_diff\tframe_max_abs_diff\tframe_diff_ge_5\tframe_diff_ge_10\tframe_diff_ge_15\tframe_diff_ge_20\tframe_cluster_ge_10\tframe_cluster_ge_15\tframe_cluster_ge_20\tdiff_ge_5\tdiff_ge_10\tdiff_ge_15\tdiff_ge_20\tdiff_ge_25\tdiff_ge_30\tdiff_ge_35\tdiff_ge_40\tcluster_ge_10\tcluster_ge_15\tcluster_ge_20\tcluster_ge_25\tcluster_ge_30\tcluster_ge_35\tglobal_mean\tbackground_mean\tglobal_mean_delta\tanalyze_ms\tdecode_ms\tchanged_mask_hex\tframe_changed_mask_hex\n"
    );

    for (uint16_t index = 0; index < count; ++index) {
        ImageMotionDiagnosticSample sample;
        if (!imageMotionDiagnosticGet(index, sample))
            continue;

        float totalPct = sample.activeRoiBlocks > 0
            ? ((float)sample.changedBlocks * 100.0f) / (float)sample.activeRoiBlocks
            : 0.0f;
        float clusterPct = sample.activeRoiBlocks > 0
            ? ((float)sample.largestClusterBlocks * 100.0f) / (float)sample.activeRoiBlocks
            : 0.0f;
        float frameChangedPct = sample.activeRoiBlocks > 0
            ? ((float)sample.frameChangedBlocks * 100.0f) / (float)sample.activeRoiBlocks
            : 0.0f;
        float frameClusterPct = sample.activeRoiBlocks > 0
            ? ((float)sample.frameLargestClusterBlocks * 100.0f) / (float)sample.activeRoiBlocks
            : 0.0f;
        int16_t backgroundMeanX10 = (int16_t)(
            (int32_t)sample.globalMeanX10 -
            (int32_t)sample.globalMeanDeltaX10
        );

        line =
            String(sample.sequence) + "\t" +
            imageMotionDiagnosticTimestamp(sample) + "\t" +
            String(sample.uptimeMs) + "\t" +
            String(sample.sourceWidth) + "x" + String(sample.sourceHeight) + "\t" +
            String(imageMotionStateName((ImageMotionState)sample.state)) + "\t" +
            String(imageMotionRejectReasonName((ImageMotionRejectReason)sample.rejectReason)) + "\t" +
            String((sample.flags & 0x01U) ? "1" : "0") + "\t" +
            String((sample.flags & 0x02U) ? "1" : "0") + "\t" +
            String(sample.confirmCounter) + "\t" +
            String(sample.releaseCounter) + "\t" +
            String(sample.activeRoiBlocks) + "\t" +
            String(sample.changedBlocks) + "\t" +
            String(totalPct, 1) + "\t" +
            String(sample.largestClusterBlocks) + "\t" +
            String(clusterPct, 1) + "\t" +
            String(sample.minimumMotionBlocks) + "\t" +
            String(sample.blockThreshold) + "\t" +
            String(sample.dynamicThresholdMin) + "\t" +
            String((float)sample.dynamicThresholdAvgX10 / 10.0f, 1) + "\t" +
            String(sample.dynamicThresholdMax) + "\t" +
            String((float)sample.meanAbsDiffX10 / 10.0f, 1) + "\t" +
            String(sample.maxAbsDiff) + "\t" +
            String((sample.flags & 0x04U) ? "1" : "0") + "\t" +
            String(sample.frameDeltaIntervalMs) + "\t" +
            String(sample.frameDeltaThreshold) + "\t" +
            String(sample.frameChangedBlocks) + "\t" +
            String(frameChangedPct, 1) + "\t" +
            String(sample.frameLargestClusterBlocks) + "\t" +
            String(frameClusterPct, 1) + "\t" +
            String((float)sample.frameMeanAbsDiffX10 / 10.0f, 1) + "\t" +
            String(sample.frameMaxAbsDiff) + "\t" +
            String(sample.frameDiffGe5) + "\t" +
            String(sample.frameDiffGe10) + "\t" +
            String(sample.frameDiffGe15) + "\t" +
            String(sample.frameDiffGe20) + "\t" +
            String(sample.frameClusterGe10) + "\t" +
            String(sample.frameClusterGe15) + "\t" +
            String(sample.frameClusterGe20) + "\t" +
            String(sample.diffGe5) + "\t" +
            String(sample.diffGe10) + "\t" +
            String(sample.diffGe15) + "\t" +
            String(sample.diffGe20) + "\t" +
            String(sample.diffGe25) + "\t" +
            String(sample.diffGe30) + "\t" +
            String(sample.diffGe35) + "\t" +
            String(sample.diffGe40) + "\t" +
            String(sample.clusterGe10) + "\t" +
            String(sample.clusterGe15) + "\t" +
            String(sample.clusterGe20) + "\t" +
            String(sample.clusterGe25) + "\t" +
            String(sample.clusterGe30) + "\t" +
            String(sample.clusterGe35) + "\t" +
            imageMotionDiagnosticDeci(sample.globalMeanX10) + "\t" +
            imageMotionDiagnosticDeci(backgroundMeanX10) + "\t" +
            imageMotionDiagnosticDeci(sample.globalMeanDeltaX10) + "\t" +
            String(sample.analyzeFrameMs) + "\t" +
            String(sample.decodeMs) + "\t" +
            imageMotionDiagnosticMaskHex(sample.changedMask) + "\t" +
            imageMotionDiagnosticMaskHex(sample.frameChangedMask) +
            "\n";

        server.sendContent(line);
        if ((index % 20U) == 19U)
            serviceWebLongOperation();
    }
}

static String imageMotionInfoButton(
    UiTextId titleId,
    UiTextId helpId
)
{
    return
        "<button type='button' class='im-info' data-title='" +
        htmlText(titleId) +
        "' data-info='" +
        htmlText(helpId) +
        "' aria-label='" +
        htmlText(UI_IMAGE_MOTION_INFO) +
        "'>i</button>";
}


static void handleImageMotionPage()
{
    String html = htmlHeader();

    html += "<h2>" + htmlText(UI_IMAGE_MOTION_TITLE) + "</h2>";
    html += "<p class='muted'>" + htmlText(UI_IMAGE_MOTION_SUBTITLE) + "</p>";

    if (recorderIsOpen()) {
        stopCameraPreview();
        html += "<div class='flash-notice error'><strong>" +
            htmlText(UI_STATUS_RECORDING_RUNNING) +
            "</strong></div>";
    } else if (!esp_camera_sensor_get()) {
        stopCameraPreview();
        html += "<p>" + htmlText(UI_NOT_DETECTED) + "</p>";
    } else {
        noteCameraPreviewActivity();

        // Diagnostic live analysis must start with a fresh temporal vote so a
        // previous operational detection cannot make the page look active. The
        // learned background is intentionally retained.
        imageMotionBeginVerification();
        imageMotionPreviewLastAnalysisMs = 0;

        html += R"HTML(
<style>
.im-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin:12px 0}.im-card{border:1px solid #d7dde5;border-radius:10px;padding:14px;background:#fff}.im-fields{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}.im-field{border:1px solid #e5e7eb;border-radius:8px;padding:10px;background:#fafbfc}.im-label-row{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:6px}.im-label-row label{font-weight:600}.im-help{font-size:.86rem;line-height:1.35;color:#5f6b7a;margin-top:6px}.im-info{flex:0 0 auto;width:26px;height:26px;padding:0;border-radius:50%;font-weight:700;line-height:24px}.im-stage{position:relative;display:inline-block;max-width:100%;touch-action:none}.im-stage img{display:block;max-width:100%;height:auto}.im-stage canvas{position:absolute;inset:0;width:100%;height:100%;cursor:crosshair;touch-action:none}.im-actions{display:flex;flex-wrap:wrap;gap:8px;margin:9px 0}.im-legend{display:flex;flex-wrap:wrap;gap:10px;margin:0 0 8px}.im-legend span{display:inline-flex;align-items:center;gap:6px;font-size:.86rem}.im-roi-hint{font-size:.84rem;line-height:1.25;margin:5px 0 8px}.im-swatch{width:18px;height:14px;border:1px solid #9ca3af;border-radius:3px;background:#fff}.im-swatch.excluded{background:rgba(220,38,38,.35)}.im-result{border:1px solid #d7dde5;border-radius:8px;padding:12px;background:#f8fafc;margin:10px 0}.im-result-title{font-weight:700;margin-bottom:6px}.im-result-text{font-size:1rem;margin-bottom:8px}.im-result-meta{display:flex;flex-wrap:wrap;gap:8px 16px;font-size:.88rem;color:#4b5563}.im-live-card{margin:10px 0;padding:10px 12px}.im-live-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:7px}.im-live-title{display:flex;align-items:center;gap:7px}.im-live-head h3{margin:0}.im-live-pulse{font-size:.72rem;line-height:1;opacity:.22;transition:opacity .08s}.im-live-pulse.tick{opacity:1}.im-live-tools{display:flex;align-items:center;justify-content:flex-end;gap:8px;flex-wrap:wrap;font-size:.84rem}.im-live-tools button{padding:6px 10px;margin:0}.im-live-grid{display:grid;grid-template-columns:minmax(205px,38%) minmax(0,1fr);gap:5px 14px;align-items:center;line-height:1.25}.im-live-label{font-weight:700;white-space:normal;overflow-wrap:anywhere}.im-live-value{min-width:0;min-height:1.25em;overflow-wrap:anywhere}.im-diag{white-space:pre-wrap;overflow-wrap:anywhere;background:#111827;color:#e5e7eb;padding:12px;border-radius:8px;min-height:100px;font-family:monospace;font-size:.82rem}.im-details{margin-top:10px}.im-details summary{cursor:pointer;font-weight:600}.im-modal-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.5);display:none;align-items:center;justify-content:center;padding:18px;z-index:10000}.im-modal-backdrop.open{display:flex}.im-modal{width:min(560px,100%);max-height:80vh;overflow:auto;background:#fff;border-radius:12px;padding:18px;box-shadow:0 18px 50px rgba(0,0,0,.25)}.im-modal h3{margin-top:0}.im-modal-actions{display:flex;justify-content:flex-end;margin-top:14px}@media(max-width:760px){.im-grid,.im-fields{grid-template-columns:1fr}.im-live-head{align-items:flex-start;flex-direction:column}.im-live-tools{justify-content:flex-start}.im-live-grid{grid-template-columns:minmax(145px,42%) minmax(0,1fr);gap:5px 10px}.im-live-label{margin-top:0}.im-live-value{padding-bottom:0}}@media(max-width:480px){.im-live-grid{grid-template-columns:1fr;gap:2px}.im-live-label{margin-top:5px}}
</style>
)HTML";

        html += "<div class='flash-notice' style='border-left-color:var(--accent);background:#eef4ff'><strong>" +
            htmlText(UI_IMAGE_MOTION_TEST_NOTE) + "</strong></div>";

        html += "<div class='im-card im-live-card'><div class='im-live-head'><div class='im-live-title'><h3>" +
            htmlText(UI_IMAGE_MOTION_LIVE_TITLE) +
            "</h3><span id='imLivePulse' class='im-live-pulse' aria-hidden='true'>●</span></div><div class='im-live-tools'><span class='muted'>RAM <b id='imDiagBufferStatus'>0 / " + String(imageMotionDiagnosticCapacity()) + "</b></span>" +
            "<a href='/image_motion_diag_download'><button type='button'>" + htmlText(UI_IMAGE_MOTION_DIAG_DOWNLOAD) + "</button></a></div></div><div class='im-live-grid'>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_LIVE_STATUS) + "</div>" +
            "<div class='im-live-value'><span id='imLiveState' class='status-pill warn'>" + htmlText(UI_IMAGE_MOTION_LIVE_WAITING) + "</span></div>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_LIVE_CONFIRMATION) + "</div>" +
            "<div id='imLiveConfirm' class='im-live-value'>0 / " + String(cfg_image_motion_confirm_frames) + "</div>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_LIVE_CURRENT_MOTION) + "</div>" +
            "<div id='imLiveFrameMotion' class='im-live-value'>-</div>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_LIVE_BACKGROUND_DIFFERENCE) + "</div>" +
            "<div id='imLiveBackground' class='im-live-value'>0.0 %</div>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_RESULT_LIMIT) + "</div>" +
            "<div id='imLiveLimit' class='im-live-value'>" + String(cfg_image_motion_min_area_pct) + " %</div>" +
            "<div class='im-live-label'>" + htmlText(UI_IMAGE_MOTION_LIVE_LAST_DETECTION) + "</div>" +
            "<div id='imLiveLast' class='im-live-value'>" + htmlText(UI_IMAGE_MOTION_LIVE_NEVER) + "</div>" +
            "</div></div>";

        html += "<div class='im-grid'><div class='im-card'>";

        html += "<div class='im-legend'><span><i class='im-swatch'></i>" +
            htmlText(UI_IMAGE_MOTION_ROI_ACTIVE_LEGEND) +
            "</span><span><i class='im-swatch excluded'></i>" +
            htmlText(UI_IMAGE_MOTION_ROI_EXCLUDED_LEGEND) +
            "</span></div>";

        html += "<div class='im-stage' id='imStage'><img id='imImage' alt='" +
            htmlText(UI_IMAGE_MOTION_TITLE) +
            "'><canvas id='imCanvas'></canvas></div>";
        html += "<p class='im-roi-hint muted'>" + htmlText(UI_IMAGE_MOTION_ROI_HELP) + "</p>";
        html += "<div class='im-actions'><button type='button' id='imAll'>" + htmlText(UI_IMAGE_MOTION_SELECT_ALL) +
            "</button><button type='button' id='imClear'>" + htmlText(UI_IMAGE_MOTION_CLEAR) +
            "</button><button type='button' id='imInvert'>" + htmlText(UI_IMAGE_MOTION_INVERT) + "</button></div></div>";

        html += "<div class='im-card'><h3>" + htmlText(UI_IMAGE_MOTION_TITLE) + "</h3><div class='im-fields'>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imSensitivity'>" + htmlText(UI_IMAGE_MOTION_SENSITIVITY) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_SENSITIVITY, UI_IMAGE_MOTION_SENSITIVITY_HELP) +
            "</div><input id='imSensitivity' type='number' min='1' max='10' value='" + String(cfg_image_motion_sensitivity) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_SENSITIVITY_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imMinArea'>" + htmlText(UI_IMAGE_MOTION_MIN_AREA) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_MIN_AREA, UI_IMAGE_MOTION_MIN_AREA_HELP) +
            "</div><input id='imMinArea' type='number' min='1' max='100' value='" + String(cfg_image_motion_min_area_pct) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_MIN_AREA_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imConfirm'>" + htmlText(UI_IMAGE_MOTION_CONFIRM) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_CONFIRM, UI_IMAGE_MOTION_CONFIRM_HELP) +
            "</div><input id='imConfirm' type='number' min='1' max='6' value='" + String(cfg_image_motion_confirm_frames) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_CONFIRM_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imRelease'>" + htmlText(UI_IMAGE_MOTION_RELEASE) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_RELEASE, UI_IMAGE_MOTION_RELEASE_HELP) +
            "</div><input id='imRelease' type='number' min='1' max='10' value='" + String(cfg_image_motion_release_frames) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_RELEASE_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imLearning'>" + htmlText(UI_IMAGE_MOTION_BG_LEARNING) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_BG_LEARNING, UI_IMAGE_MOTION_BG_LEARNING_HELP) +
            "</div><input id='imLearning' type='number' min='1' max='64' value='" + String(cfg_image_motion_background_learning) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_BG_LEARNING_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imGlobalMean'>" + htmlText(UI_IMAGE_MOTION_GLOBAL_MEAN) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_GLOBAL_MEAN, UI_IMAGE_MOTION_GLOBAL_MEAN_HELP) +
            "</div><input id='imGlobalMean' type='number' min='5' max='100' value='" + String(cfg_image_motion_global_mean_delta) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_GLOBAL_MEAN_HELP) + "</div></div>";

        html += "<div class='im-field'><div class='im-label-row'><label for='imGlobalChange'>" + htmlText(UI_IMAGE_MOTION_GLOBAL_CHANGE) + "</label>" +
            imageMotionInfoButton(UI_IMAGE_MOTION_GLOBAL_CHANGE, UI_IMAGE_MOTION_GLOBAL_CHANGE_HELP) +
            "</div><input id='imGlobalChange' type='number' min='20' max='100' value='" + String(cfg_image_motion_global_change_pct) +
            "'><div class='im-help'>" + htmlText(UI_IMAGE_MOTION_GLOBAL_CHANGE_HELP) + "</div></div>";

        html += "</div><div class='im-actions'><button type='button' id='imSave'>" + htmlText(UI_IMAGE_MOTION_SAVE) +
            "</button><button type='button' id='imDefaults'>" + htmlText(UI_IMAGE_MOTION_RESET_DEFAULTS) +
            "</button>" + imageMotionInfoButton(UI_IMAGE_MOTION_RESET_DEFAULTS, UI_IMAGE_MOTION_RESET_DEFAULTS_HELP) + "</div>";

        html += "<div class='im-card' style='margin-top:14px'><h3>" + htmlText(UI_IMAGE_MOTION_TEST) + "</h3><p class='muted'>" +
            htmlText(UI_IMAGE_MOTION_TEST_HELP) + " <strong>" + htmlText(UI_IMAGE_MOTION_TEST_USES_SAVED) + "</strong></p>";
        html += "<div class='im-actions'><button type='button' id='imTest'>" + htmlText(UI_IMAGE_MOTION_TEST) +
            "</button>" + imageMotionInfoButton(UI_IMAGE_MOTION_TEST, UI_IMAGE_MOTION_TEST_HELP) +
            "<button type='button' id='imResetBg'>" + htmlText(UI_IMAGE_MOTION_RESET_BG) +
            "</button>" + imageMotionInfoButton(UI_IMAGE_MOTION_RESET_BG, UI_IMAGE_MOTION_RESET_BG_HELP) + "</div>";
        html += "<div id='imStatus' class='muted'></div><h3>" + htmlText(UI_IMAGE_MOTION_DIAGNOSTICS) + "</h3><p class='muted'>" +
            htmlText(UI_IMAGE_MOTION_DIAGNOSTICS_HELP) + "</p><div class='im-result'><div class='im-result-title'>" +
            htmlText(UI_IMAGE_MOTION_RESULT_TITLE) + "</div><div id='imResultText' class='im-result-text'>-</div><div id='imResultMeta' class='im-result-meta'></div></div>";
        html += "<details class='im-details'><summary>" + htmlText(UI_IMAGE_MOTION_TECH_DETAILS) +
            "</summary><div id='imDiag' class='im-diag'>-</div></details></div></div></div>";

        html += "<div id='imInfoBackdrop' class='im-modal-backdrop' role='dialog' aria-modal='true'><div class='im-modal'><h3 id='imInfoTitle'></h3><div id='imInfoBody'></div><div class='im-modal-actions'><button type='button' id='imInfoClose'>" +
            htmlText(UI_IMAGE_MOTION_INFO_CLOSE) + "</button></div></div></div>";

        String defaultRoi = imageMotionDefaultRoiMask();
        html += "<script>const IM_W=20,IM_H=15;let imMask='" + cfg_image_motion_roi_mask + "';let imSavedMinArea=" + String(cfg_image_motion_min_area_pct) + ";";
        html += "const IM_DEFAULTS={sensitivity:'5',minArea:'6',confirm:'2',release:'2',learning:'4',globalMean:'24',globalChange:'70',roi:'" + defaultRoi + "'};";
        html += "const IM_TEXT={saved:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_SAVED))) +
            "\",saveFailed:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_SAVE_FAILED))) +
            "\",testFailed:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_TEST_FAILED))) +
            "\",defaultsDone:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESET_DEFAULTS_DONE))) +
            "\",resetBgDone:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESET_BG_DONE))) +
            "\",resultMotion:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_MOTION))) +
            "\",resultNone:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_NONE))) +
            "\",resultDisabled:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_DISABLED))) +
            "\",resultLearning:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_LEARNING))) +
            "\",resultConfirming:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_CONFIRMING))) +
            "\",resultGlobalLight:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_GLOBAL_LIGHT))) +
            "\",resultNoRoi:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_NO_ROI))) +
            "\",resultError:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_ERROR))) +
            "\",resultTime:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_TIME))) +
            "\",resultArea:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_AREA))) +
            "\",resultLimit:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_RESULT_LIMIT))) +
            "\",liveTotalArea:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_CHANGED_AREA))) +
            "\",liveCurrentMotion:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_CURRENT_MOTION))) +
            "\",liveBackgroundDifference:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_BACKGROUND_DIFFERENCE))) +
            "\",liveTotalShort:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_TOTAL_SHORT))) +
            "\",liveConnectedShort:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_CONNECTED_SHORT))) +
            "\",liveDetected:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_DETECTED))) +
            "\",liveNone:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_NONE))) +
            "\",liveLearning:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_LEARNING))) +
            "\",liveGlobalLight:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_GLOBAL_LIGHT))) +
            "\",liveError:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_ERROR))) +
            "\",liveConfirmed:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_CONFIRMED))) +
            "\",liveNever:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_NEVER))) +
            "\",liveWaiting:\"" + imageMotionJsonEscape(String(tr(UI_IMAGE_MOTION_LIVE_WAITING))) +
            "\",ago:\"" + imageMotionJsonEscape(String(tr(UI_MOTION_AGO))) +
            "\",agoSuffix:\"" + imageMotionJsonEscape(String(tr(UI_MOTION_AGO_SUFFIX))) + "\"};";

        html += R"JS(
const imImage=document.getElementById('imImage'),imCanvas=document.getElementById('imCanvas'),imCtx=imCanvas.getContext('2d');
const imStatus=document.getElementById('imStatus'),imDiag=document.getElementById('imDiag'),imResultText=document.getElementById('imResultText'),imResultMeta=document.getElementById('imResultMeta'),imDiagBufferStatus=document.getElementById('imDiagBufferStatus');
const imLiveState=document.getElementById('imLiveState'),imLiveConfirm=document.getElementById('imLiveConfirm'),imLiveFrameMotion=document.getElementById('imLiveFrameMotion'),imLiveBackground=document.getElementById('imLiveBackground'),imLiveLimit=document.getElementById('imLiveLimit'),imLiveLast=document.getElementById('imLiveLast'),imLivePulse=document.getElementById('imLivePulse');
const imInfoBackdrop=document.getElementById('imInfoBackdrop'),imInfoTitle=document.getElementById('imInfoTitle'),imInfoBody=document.getElementById('imInfoBody');
function maskBytes(){const a=[];for(let i=0;i<imMask.length;i+=2)a.push(parseInt(imMask.slice(i,i+2),16)||0);return a}
function setMaskBytes(a){imMask=a.map(v=>v.toString(16).padStart(2,'0')).join('')}
function bit(i){const a=maskBytes();return !!(a[i>>3]&(1<<(i&7)))}
function setBit(i,on){const a=maskBytes();if(on)a[i>>3]|=1<<(i&7);else a[i>>3]&=~(1<<(i&7));setMaskBytes(a)}
function drawGrid(){const r=imCanvas.getBoundingClientRect();imCanvas.width=Math.max(1,Math.round(r.width*devicePixelRatio));imCanvas.height=Math.max(1,Math.round(r.height*devicePixelRatio));imCtx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);const w=r.width,h=r.height,cw=w/IM_W,ch=h/IM_H;for(let y=0;y<IM_H;y++)for(let x=0;x<IM_W;x++){const i=y*IM_W+x;if(!bit(i)){imCtx.fillStyle='rgba(220,38,38,.35)';imCtx.fillRect(x*cw,y*ch,cw,ch)}imCtx.strokeStyle='rgba(255,255,255,.55)';imCtx.strokeRect(x*cw,y*ch,cw,ch)}}
let painting=false,paintValue=true;
function cellAt(e){const r=imCanvas.getBoundingClientRect(),x=Math.floor((e.clientX-r.left)/r.width*IM_W),y=Math.floor((e.clientY-r.top)/r.height*IM_H);if(x<0||x>=IM_W||y<0||y>=IM_H)return-1;return y*IM_W+x}
imCanvas.addEventListener('pointerdown',e=>{const i=cellAt(e);if(i<0)return;painting=true;paintValue=!bit(i);setBit(i,paintValue);imCanvas.setPointerCapture(e.pointerId);drawGrid()});
imCanvas.addEventListener('pointermove',e=>{if(!painting)return;const i=cellAt(e);if(i>=0){setBit(i,paintValue);drawGrid()}});imCanvas.addEventListener('pointerup',()=>painting=false);imCanvas.addEventListener('pointercancel',()=>painting=false);
document.getElementById('imAll').onclick=()=>{const a=new Array(38).fill(255);a[37]&=15;setMaskBytes(a);drawGrid()};
document.getElementById('imClear').onclick=()=>{setMaskBytes(new Array(38).fill(0));drawGrid()};
document.getElementById('imInvert').onclick=()=>{const a=maskBytes().map(v=>(~v)&255);a[37]&=15;setMaskBytes(a);drawGrid()};
function liveAgeText(valid,ms){if(!valid)return IM_TEXT.liveNever;ms=Math.max(0,Number(ms)||0);let v='';if(ms<1000)v='<1 s';else if(ms<60000)v=Math.floor(ms/1000)+' s';else{const sec=Math.floor(ms/1000),min=Math.floor(sec/60),rest=sec%60;v=min+' min '+rest+' s'}return IM_TEXT.ago+(IM_TEXT.ago?' ':'')+v+IM_TEXT.agoSuffix}
function liveStateText(d){if(d.motion_active||d.image_motion_state==='confirmed')return IM_TEXT.liveDetected;if(d.image_motion_state==='background_init')return IM_TEXT.liveLearning;if(d.image_motion_state==='global_change')return IM_TEXT.liveGlobalLight;if(d.image_motion_state==='error')return IM_TEXT.liveError;return IM_TEXT.liveNone}
let imLastAnalysisStamp=0,imLastAnalysisLocalMs=0,imStatusInFlight=false;
function setLiveWaiting(clearValues){if(imLivePulse)imLivePulse.classList.remove('tick');if(imLiveState){imLiveState.textContent=IM_TEXT.liveWaiting;imLiveState.classList.remove('danger','ok');imLiveState.classList.add('warn')}if(clearValues){if(imLiveConfirm)imLiveConfirm.textContent='-';if(imLiveFrameMotion)imLiveFrameMotion.textContent='-';if(imLiveBackground)imLiveBackground.textContent='-'}}
function motionPair(total,cluster){return Number(total||0).toFixed(1)+' % '+IM_TEXT.liveTotalShort+' · '+Number(cluster||0).toFixed(1)+' % '+IM_TEXT.liveConnectedShort}function renderLive(payload){const d=(payload&&payload.diagnostics)||{};const stamp=Math.max(0,Number((payload&&payload.analysis_stamp_ms)||0)||0);const age=Math.max(0,Number(d.last_analysis_age_ms)||0);if(imLiveLimit)imLiveLimit.textContent=Number((payload&&payload.area_limit_pct)||imSavedMinArea).toFixed(1)+' %';if(imLiveLast)imLiveLast.textContent=liveAgeText(!!d.last_detection_valid,d.last_detection_age_ms);if(imDiagBufferStatus)imDiagBufferStatus.textContent=String(Number((payload&&payload.diag_count)||0))+' / '+String(Number((payload&&payload.diag_capacity)||0));if(!stamp||!d.last_analysis_valid||age>1500){setLiveWaiting(true);return}if(stamp===imLastAnalysisStamp){return}imLastAnalysisStamp=stamp;imLastAnalysisLocalMs=Date.now();if(imLivePulse){imLivePulse.classList.add('tick');setTimeout(()=>imLivePulse.classList.remove('tick'),180)}const required=Math.max(1,Number((payload&&payload.confirm_required)||0)||1);const current=Math.max(0,Number(d.confirm_counter)||0);if(imLiveState){imLiveState.textContent=liveStateText(d);imLiveState.classList.remove('danger','warn','ok');if(d.motion_active||d.image_motion_state==='confirmed')imLiveState.classList.add('danger');else if(d.image_motion_state==='candidate'||d.image_motion_state==='background_init'||d.image_motion_state==='global_change')imLiveState.classList.add('warn');else if(d.image_motion_state==='error')imLiveState.classList.add('danger');else imLiveState.classList.add('ok')}if(imLiveConfirm){imLiveConfirm.textContent=(d.motion_active||d.image_motion_state==='confirmed')?IM_TEXT.liveConfirmed+' ('+Math.min(required,Math.max(current,required))+' / '+required+')':Math.min(current,required)+' / '+required}if(imLiveFrameMotion){imLiveFrameMotion.textContent=d.frame_delta_ready?motionPair(d.frame_changed_pct,d.frame_cluster_pct):'-'}if(imLiveBackground){imLiveBackground.textContent=motionPair(d.global_change_pct,d.changed_area_pct)}imDiag.textContent=JSON.stringify(d,null,2)}
async function pollLiveStatus(){if(imStatusInFlight||document.hidden)return;imStatusInFlight=true;try{const r=await fetch('/image_motion_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'});if(!r.ok)throw new Error();const j=await r.json();renderLive(j)}catch(e){setLiveWaiting(true)}finally{imStatusInFlight=false}}
let imRefreshTimer=0,imFreshnessTimer=0;function scheduleRefresh(ms){clearTimeout(imRefreshTimer);imRefreshTimer=setTimeout(refresh,ms)}function scheduleFreshness(ms){clearTimeout(imFreshnessTimer);imFreshnessTimer=setTimeout(checkFreshness,ms)}function refresh(){if(document.hidden){scheduleRefresh(1000);return}imImage.src='/snapshot?im=1&t='+Date.now()}function checkFreshness(){if(!document.hidden&&imLastAnalysisLocalMs&&Date.now()-imLastAnalysisLocalMs>1600)setLiveWaiting(true);scheduleFreshness(300)}imImage.onload=()=>{drawGrid();pollLiveStatus();scheduleRefresh(200)};imImage.onerror=()=>{setLiveWaiting(true);scheduleRefresh(500)};window.addEventListener('resize',drawGrid);refresh();pollLiveStatus();scheduleFreshness(300);
function params(){const p=new URLSearchParams();p.set('sensitivity',document.getElementById('imSensitivity').value);p.set('min_area',document.getElementById('imMinArea').value);p.set('confirm',document.getElementById('imConfirm').value);p.set('release',document.getElementById('imRelease').value);p.set('learning',document.getElementById('imLearning').value);p.set('global_mean',document.getElementById('imGlobalMean').value);p.set('global_change',document.getElementById('imGlobalChange').value);p.set('roi',imMask);return p}
function openInfo(title,body){imInfoTitle.textContent=title;imInfoBody.textContent=body;imInfoBackdrop.classList.add('open')}
function closeInfo(){imInfoBackdrop.classList.remove('open')}
document.querySelectorAll('.im-info').forEach(b=>b.addEventListener('click',()=>openInfo(b.dataset.title||'',b.dataset.info||'')));
document.getElementById('imInfoClose').onclick=closeInfo;imInfoBackdrop.addEventListener('click',e=>{if(e.target===imInfoBackdrop)closeInfo()});document.addEventListener('keydown',e=>{if(e.key==='Escape')closeInfo()});
function setDefaults(){document.getElementById('imSensitivity').value=IM_DEFAULTS.sensitivity;document.getElementById('imMinArea').value=IM_DEFAULTS.minArea;document.getElementById('imConfirm').value=IM_DEFAULTS.confirm;document.getElementById('imRelease').value=IM_DEFAULTS.release;document.getElementById('imLearning').value=IM_DEFAULTS.learning;document.getElementById('imGlobalMean').value=IM_DEFAULTS.globalMean;document.getElementById('imGlobalChange').value=IM_DEFAULTS.globalChange;imMask=IM_DEFAULTS.roi;drawGrid();imStatus.textContent=IM_TEXT.defaultsDone}
document.getElementById('imDefaults').onclick=setDefaults;
document.getElementById('imSave').onclick=async()=>{imStatus.textContent='...';try{const r=await fetch('/image_motion_save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:params()});const j=await r.json();if(!r.ok||!j.ok)throw new Error(IM_TEXT.saveFailed+(j.error?': '+j.error:''));imSavedMinArea=parseInt(document.getElementById('imMinArea').value,10)||imSavedMinArea;imStatus.textContent=IM_TEXT.saved}catch(e){imStatus.textContent=e.message}};
function friendlyResult(d){if(d.motion_active||d.image_motion_state==='confirmed')return IM_TEXT.resultMotion;switch(d.reject_reason){case'disabled':return IM_TEXT.resultDisabled;case'background_init':return IM_TEXT.resultLearning;case'confirming':return IM_TEXT.resultConfirming;case'global_light':return IM_TEXT.resultGlobalLight;case'no_roi':return IM_TEXT.resultNoRoi;case'decode':case'invalid_frame':return IM_TEXT.resultError;default:return IM_TEXT.resultNone}}
function renderDiagnostics(d){imResultText.textContent=friendlyResult(d);const active=Math.max(0,Number(d.active_roi_blocks)||0),changed=Math.max(0,Number(d.changed_blocks)||0),cluster=Math.max(0,Number(d.largest_cluster_blocks)||0),frameChanged=Math.max(0,Number(d.frame_changed_blocks)||0),frameCluster=Math.max(0,Number(d.frame_largest_cluster_blocks)||0);const total=Number(d.global_change_pct||0).toFixed(1)+' %'+(active?' ('+changed+' / '+active+')':'');const area=Number(d.changed_area_pct||0).toFixed(1)+' %'+(cluster?' ('+cluster+')':'');const frameTotal=d.frame_delta_ready?(Number(d.frame_changed_pct||0).toFixed(1)+' %'+(active?' ('+frameChanged+' / '+active+')':'')):'-';const frameArea=d.frame_delta_ready?(Number(d.frame_cluster_pct||0).toFixed(1)+' %'+(frameCluster?' ('+frameCluster+')':'')):'-';const limit=imSavedMinArea+' %';imResultMeta.innerHTML='';[[IM_TEXT.resultTime,(d.analyze_frame_ms!==undefined?d.analyze_frame_ms:'-')+' ms'],[IM_TEXT.liveCurrentMotion,frameTotal+' / '+frameArea+' '+IM_TEXT.liveConnectedShort],[IM_TEXT.liveBackgroundDifference,total+' / '+area+' '+IM_TEXT.liveConnectedShort],[IM_TEXT.resultLimit,limit]].forEach(([k,v])=>{const span=document.createElement('span');span.textContent=k+': '+v;imResultMeta.appendChild(span)});imDiag.textContent=JSON.stringify(d,null,2)}
document.getElementById('imTest').onclick=async()=>{imStatus.textContent='...';try{const r=await fetch('/image_motion_test',{method:'POST'}),j=await r.json();if(!r.ok||!j.ok)throw new Error(IM_TEXT.testFailed+(j.error?': '+j.error:''));renderDiagnostics(j.diagnostics||{});imStatus.textContent='OK'}catch(e){imResultText.textContent=IM_TEXT.resultError;imStatus.textContent=e.message}};
document.getElementById('imResetBg').onclick=async()=>{imStatus.textContent='...';try{const r=await fetch('/image_motion_reset',{method:'POST'}),j=await r.json();if(!r.ok||!j.ok)throw new Error(IM_TEXT.testFailed);imResultText.textContent=IM_TEXT.resetBgDone;imResultMeta.textContent='';imDiag.textContent='-';imStatus.textContent=IM_TEXT.resetBgDone}catch(e){imStatus.textContent=e.message}};
function release(){fetch('/preview_stop?im=1',{method:'POST',keepalive:true}).catch(()=>{})}window.addEventListener('pagehide',release);document.addEventListener('visibilitychange',()=>{if(document.hidden){clearTimeout(imRefreshTimer);clearTimeout(imFreshnessTimer);release()}else{clearTimeout(imRefreshTimer);clearTimeout(imFreshnessTimer);imLastAnalysisStamp=0;imLastAnalysisLocalMs=0;refresh();pollLiveStatus();scheduleFreshness(300)}});
)JS";
        html += "</script>";
    }

    html += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}


static void handlePreview()
{
    String html = htmlHeader();

    html += "<h2>Live Kamera-Vorschau</h2>";

    if (recorderIsOpen()) {

        // Existing recordings keep priority. The preview does not take over
        // the camera in the middle of an active recording.
        stopCameraPreview();

        html +=
            "<div class='flash-notice error'>"
            "<strong>Aufnahme läuft</strong>"
            "<span class='muted'>Die Live-Vorschau ist während einer laufenden "
            "Aufnahme nicht verfügbar.</span>"
            "</div>";

    } else if (!esp_camera_sensor_get()) {

        stopCameraPreview();
        html += "<p>Camera is not initialized.</p>";

    } else {

        // Raise the preview gate before the first snapshot request so there is
        // no race where motion could start a recording while the page loads.
        noteCameraPreviewActivity();

        sensor_t *previewSensor =
            esp_camera_sensor_get();

        bool cropSensorAvailable =
            previewSensor &&
            previewSensor->id.PID == 0x3660 &&
            previewSensor->set_res_raw &&
            previewSensor->set_framesize;

        bool cropResolutionAvailable =
            cfg_resolution == "160x120" ||
            cfg_resolution == "320x240" ||
            cfg_resolution == "640x480" ||
            cfg_resolution == "800x600" ||
            cfg_resolution == "1024x768";

        bool cropAvailable =
            cropSensorAvailable &&
            cropResolutionAvailable;


        html += R"HTML(
<div class='flash-notice' style='border-left-color:var(--accent);background:#eef4ff'>
    <strong style='color:#1d4ed8'>Live-Vorschau aktiv</strong>
    <span class='muted'>Detection und Recording sind deaktiviert, solange diese Kameraansicht aktiv ist.</span>
</div>
)HTML";


        html +=
            "<section id='sensorCropPanel' class='sensor-crop-panel' "
            "data-available='" +
            String(cropAvailable ? "1" : "0") +
            "' data-saved-zoom='" +
            htmlEscape(cfg_camera_crop_zoom) +
            "' data-saved-x='" +
            String(cfg_camera_crop_x) +
            "' data-saved-y='" +
            String(cfg_camera_crop_y) +
            "'>";

        html +=
            "<div class='sensor-crop-head'>"
            "<div><strong>Sensor-Ausschnitt (OV3660)</strong>"
            "<div class='sensor-crop-sub'>Dieser Ausschnitt wird direkt im Sensor gewählt. "
            "Nach SAVE gilt er dauerhaft für alle künftigen Aufnahmen.</div></div>"
            "<span id='sensorCropStatus' class='status-pill ok'>Gespeichert</span>"
            "</div>";


        if (!cropAvailable) {

            String cropReason =
                !cropSensorAvailable
                ? String("Nicht verfügbar: erkannter Sensor ist kein unterstützter OV3660 bzw. der Treiber bietet kein Raw-Crop.")
                : String("Nicht verfügbar für die aktuelle Auflösung. Crop unterstützt 4:3 bis 1024x768.");

            html +=
                "<div class='sensor-crop-warning'>" +
                htmlEscape(cropReason) +
                "</div>";
        }


        html +=
            "<div class='sensor-crop-controls'>"
            "<label>Crop Zoom<br><select id='sensorCropZoom'" +
            String(cropAvailable ? "" : " disabled") +
            ">"
            "<option value='1.0'" +
            String(cfg_camera_crop_zoom == "1.0" ? " selected" : "") +
            ">1.0x &ndash; volles Sichtfeld</option>"
            "<option value='1.5'" +
            String(cfg_camera_crop_zoom == "1.5" ? " selected" : "") +
            ">1.5x</option>"
            "<option value='2.0'" +
            String(cfg_camera_crop_zoom == "2.0" ? " selected" : "") +
            ">2.0x</option>"
            "</select></label>"
            "<div><div class='sensor-crop-grid-label'>Position im späteren Bild</div>"
            "<div id='sensorCropGrid' class='sensor-crop-grid'>";

        static const char *cropLabels[9] = {
            "↖", "↑", "↗",
            "←", "●", "→",
            "↙", "↓", "↘"
        };

        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {

                bool selected =
                    x == cfg_camera_crop_x &&
                    y == cfg_camera_crop_y;

                html +=
                    "<button type='button' class='sensor-crop-pos" +
                    String(selected ? " active" : "") +
                    "' data-x='" +
                    String(x) +
                    "' data-y='" +
                    String(y) +
                    "'" +
                    String(cropAvailable ? "" : " disabled") +
                    ">" +
                    String(cropLabels[y * 3 + x]) +
                    "</button>";
            }
        }

        html +=
            "</div></div>"
            "<div class='sensor-crop-save-wrap'>"
            "<button id='sensorCropSaveBtn' type='button' class='primary'" +
            String(cropAvailable ? "" : " disabled") +
            ">Crop SAVE</button>"
            "<div id='sensorCropMessage' class='sensor-crop-message'>"
            "Änderungen werden zuerst nur live getestet.</div>"
            "</div>"
            "</div>"
            "</section>";


        html += R"HTML(
<style>
.sensor-crop-panel{
    border:1px solid #bfd3ef;
    border-left:5px solid var(--accent);
    border-radius:9px;
    background:#f8fbff;
    padding:14px 16px;
    margin:12px 0 16px;
}
.sensor-crop-head{
    display:flex;
    align-items:flex-start;
    justify-content:space-between;
    gap:12px;
    margin-bottom:12px;
}
.sensor-crop-sub{
    color:var(--muted);
    font-size:.88rem;
    line-height:1.4;
    margin-top:4px;
}
.sensor-crop-controls{
    display:flex;
    flex-wrap:wrap;
    align-items:flex-end;
    gap:18px;
}
.sensor-crop-controls label{
    font-weight:700;
}
.sensor-crop-controls select{
    width:210px;
    margin-bottom:0;
}
.sensor-crop-grid-label{
    font-size:.85rem;
    color:var(--muted);
    margin-bottom:5px;
}
.sensor-crop-grid{
    display:grid;
    grid-template-columns:repeat(3,44px);
    gap:4px;
}
.sensor-crop-grid button{
    width:44px;
    height:38px;
    min-width:0;
    margin:0;
    padding:0;
    font-size:1.05rem;
}
.sensor-crop-grid button.active{
    background:var(--accent);
    color:#fff;
}
.sensor-crop-grid button:disabled{
    opacity:.45;
    cursor:not-allowed;
}
.sensor-crop-save-wrap{
    min-width:220px;
}
.sensor-crop-save-wrap button{
    margin:0 0 5px;
}
.sensor-crop-message{
    color:var(--muted);
    font-size:.82rem;
    max-width:320px;
}
.sensor-crop-warning{
    padding:9px 10px;
    margin-bottom:12px;
    border-radius:6px;
    background:#fff1d6;
    color:var(--warn);
    font-size:.88rem;
}
.camera-zoom-controls{
    display:flex;
    align-items:center;
    flex-wrap:wrap;
    gap:6px;
    margin:10px 0;
}
.camera-zoom-controls button{
    margin:0;
    min-width:46px;
}
.camera-zoom-label{
    min-width:64px;
    text-align:center;
    font-weight:700;
    font-variant-numeric:tabular-nums;
}
.camera-zoom-hint{
    color:var(--muted);
    font-size:.85rem;
}
.camera-viewer{
    width:100%;
    max-width:100%;
    overflow:auto;
    background:#111;
    border-radius:8px;
}
.camera-stage-holder{
    width:max-content;
    min-width:100%;
}
.camera-stage{
    display:block;
    width:auto;
    max-width:none;
    margin:0 auto;
    background:#000;
}
#cam{
    display:block;
    width:auto;
    height:auto;
    max-width:none;
}
.camera-viewer.fit .camera-stage-holder{
    width:100%;
    min-width:0;
}
.camera-viewer.fit .camera-stage{
    width:auto!important;
    max-width:100%;
}
.camera-viewer.fit #cam{
    width:auto!important;
    height:auto!important;
    max-width:100%;
    max-height:70vh;
}
</style>

<div class='camera-zoom-controls' aria-label='Bildzoom'>
    <button id='camZoomOutBtn' type='button' title='Zoom out (-)'>-</button>
    <span id='camZoomLabel' class='camera-zoom-label'>Fit</span>
    <button id='camZoomInBtn' type='button' title='Zoom in (+)'>+</button>
    <button id='camZoomFitBtn' type='button' title='Fit to window (F)'>Fit</button>
    <button id='camZoom100Btn' type='button' title='100% (0)'>100%</button>
    <span class='camera-zoom-hint'>Tastatur: - / + / F / 0</span>
</div>

<div id='camViewer' class='camera-viewer fit'>
    <div class='camera-stage-holder'>
        <div id='camStage' class='camera-stage'>
            <img id='cam' alt='Live camera preview'>
        </div>
    </div>
</div>

<script>
const img=document.getElementById('cam');
const camViewer=document.getElementById('camViewer');
const camStage=document.getElementById('camStage');
const camZoomOutBtn=document.getElementById('camZoomOutBtn');
const camZoomInBtn=document.getElementById('camZoomInBtn');
const camZoomFitBtn=document.getElementById('camZoomFitBtn');
const camZoom100Btn=document.getElementById('camZoom100Btn');
const camZoomLabel=document.getElementById('camZoomLabel');

const sensorCropPanel=document.getElementById('sensorCropPanel');
const sensorCropZoomSelect=document.getElementById('sensorCropZoom');
const sensorCropGrid=document.getElementById('sensorCropGrid');
const sensorCropSaveBtn=document.getElementById('sensorCropSaveBtn');
const sensorCropStatus=document.getElementById('sensorCropStatus');
const sensorCropMessage=document.getElementById('sensorCropMessage');
const sensorCropAvailable=!!sensorCropPanel&&sensorCropPanel.dataset.available==='1';

let sensorCropZoom=sensorCropPanel?sensorCropPanel.dataset.savedZoom:'1.0';
let sensorCropX=sensorCropPanel?Number(sensorCropPanel.dataset.savedX):1;
let sensorCropY=sensorCropPanel?Number(sensorCropPanel.dataset.savedY):1;
let sensorCropBusy=false;

function sensorCropHasChanges(){
    if(!sensorCropPanel)return false;
    return sensorCropZoom!==sensorCropPanel.dataset.savedZoom||
        sensorCropX!==Number(sensorCropPanel.dataset.savedX)||
        sensorCropY!==Number(sensorCropPanel.dataset.savedY);
}

function updateSensorCropUi(message){
    if(!sensorCropPanel)return;

    if(sensorCropZoomSelect)sensorCropZoomSelect.value=sensorCropZoom;

    const buttons=sensorCropPanel.querySelectorAll('.sensor-crop-pos');
    buttons.forEach(function(button){
        const active=Number(button.dataset.x)===sensorCropX&&
            Number(button.dataset.y)===sensorCropY;
        button.classList.toggle('active',active);
        button.disabled=!sensorCropAvailable||sensorCropBusy||sensorCropZoom==='1.0';
    });

    const changed=sensorCropHasChanges();

    if(sensorCropZoomSelect)
        sensorCropZoomSelect.disabled=!sensorCropAvailable||sensorCropBusy;

    if(sensorCropSaveBtn)
        sensorCropSaveBtn.disabled=!sensorCropAvailable||sensorCropBusy||!changed;

    if(sensorCropStatus){
        sensorCropStatus.textContent=changed?'Nicht gespeichert':'Gespeichert';
        sensorCropStatus.classList.toggle('ok',!changed);
        sensorCropStatus.classList.toggle('warn',changed);
    }

    if(sensorCropMessage){
        if(message){
            sensorCropMessage.textContent=message;
        }else if(changed){
            sensorCropMessage.textContent='Live-Test aktiv. SAVE speichert diesen Ausschnitt dauerhaft.';
        }else{
            sensorCropMessage.textContent='Gespeicherter Ausschnitt ist aktiv.';
        }
    }
}

function sensorCropRequest(url){
    const body=new URLSearchParams();
    body.set('zoom',sensorCropZoom);
    body.set('x',String(sensorCropX));
    body.set('y',String(sensorCropY));

    return fetch(url,{
        method:'POST',
        cache:'no-store',
        credentials:'same-origin',
        headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:body.toString()
    }).then(function(response){
        if(response.ok)return response.json();
        return response.text().then(function(text){
            throw new Error(text||('HTTP '+response.status));
        });
    });
}

function applySensorCrop(nextZoom,nextX,nextY){
    if(!sensorCropAvailable||sensorCropBusy)return;

    const previousZoom=sensorCropZoom;
    const previousX=sensorCropX;
    const previousY=sensorCropY;

    sensorCropZoom=nextZoom;
    sensorCropX=nextX;
    sensorCropY=nextY;
    sensorCropBusy=true;
    updateSensorCropUi('Sensor-Ausschnitt wird angewendet ...');

    sensorCropRequest('/camera_crop_apply')
    .then(function(){
        sensorCropBusy=false;
        updateSensorCropUi();
    })
    .catch(function(error){
        sensorCropZoom=previousZoom;
        sensorCropX=previousX;
        sensorCropY=previousY;
        sensorCropBusy=false;
        updateSensorCropUi('Änderung fehlgeschlagen: '+error.message);
    });
}

function resetSensorCropUiToSaved(){
    if(!sensorCropPanel)return;
    sensorCropZoom=sensorCropPanel.dataset.savedZoom;
    sensorCropX=Number(sensorCropPanel.dataset.savedX);
    sensorCropY=Number(sensorCropPanel.dataset.savedY);
    sensorCropBusy=false;
    updateSensorCropUi('Gespeicherter Ausschnitt ist aktiv.');
}

if(sensorCropZoomSelect){
    sensorCropZoomSelect.addEventListener('change',function(){
        applySensorCrop(sensorCropZoomSelect.value,sensorCropX,sensorCropY);
    });
}

if(sensorCropGrid){
    sensorCropGrid.addEventListener('click',function(event){
        const button=event.target.closest('.sensor-crop-pos');
        if(!button||button.disabled)return;
        applySensorCrop(
            sensorCropZoom,
            Number(button.dataset.x),
            Number(button.dataset.y)
        );
    });
}

if(sensorCropSaveBtn){
    sensorCropSaveBtn.addEventListener('click',function(){
        if(!sensorCropAvailable||sensorCropBusy||!sensorCropHasChanges())return;

        sensorCropBusy=true;
        updateSensorCropUi('Crop wird gespeichert ...');

        sensorCropRequest('/camera_crop_save')
        .then(function(result){
            sensorCropPanel.dataset.savedZoom=result.zoom;
            sensorCropPanel.dataset.savedX=String(result.x);
            sensorCropPanel.dataset.savedY=String(result.y);
            sensorCropZoom=result.zoom;
            sensorCropX=Number(result.x);
            sensorCropY=Number(result.y);
            sensorCropBusy=false;
            updateSensorCropUi(
                result.storage
                ? ('Gespeichert in '+result.storage+'.')
                : 'Crop dauerhaft gespeichert.'
            );
        })
        .catch(function(error){
            sensorCropBusy=false;
            updateSensorCropUi('Speichern fehlgeschlagen: '+error.message);
        });
    });
}

updateSensorCropUi();

const camZoomLevels=[0.5,0.75,1,1.25,1.5,2,2.5];
const camZoomStorageKey='sensorforge.viewer.zoom';
let camZoomMode='fit';

let previewStopped=false;
let previewPaused=false;

function readCamZoom(){
    try{
        const stored=localStorage.getItem(camZoomStorageKey);
        if(!stored||stored==='fit')return 'fit';
        const value=Number(stored);
        if(camZoomLevels.indexOf(value)>=0)return value;
    }catch(e){}
    return 'fit';
}

function storeCamZoom(){
    try{
        localStorage.setItem(
            camZoomStorageKey,
            camZoomMode==='fit'?'fit':String(camZoomMode)
        );
    }catch(e){}
}

function applyCamZoom(){
    if(camZoomMode==='fit'){
        camViewer.classList.add('fit');
        camStage.style.width='';
        img.style.width='';
        camZoomLabel.textContent='Fit';
        camZoomOutBtn.disabled=false;
        camZoomInBtn.disabled=false;
        return;
    }

    camViewer.classList.remove('fit');

    const baseWidth=img.naturalWidth||1024;
    const width=Math.max(1,Math.round(baseWidth*camZoomMode));

    camStage.style.width=width+'px';
    img.style.width='100%';
    camZoomLabel.textContent=Math.round(camZoomMode*100)+'%';

    camZoomOutBtn.disabled=camZoomMode<=camZoomLevels[0];
    camZoomInBtn.disabled=camZoomMode>=camZoomLevels[camZoomLevels.length-1];
}

function setCamZoom(mode){
    if(mode!=='fit'&&camZoomLevels.indexOf(mode)<0)return;
    camZoomMode=mode;
    storeCamZoom();
    applyCamZoom();
}

function camZoomIn(){
    if(camZoomMode==='fit'){
        setCamZoom(1);
        return;
    }

    const index=camZoomLevels.indexOf(camZoomMode);
    if(index>=0&&index<camZoomLevels.length-1)
        setCamZoom(camZoomLevels[index+1]);
}

function camZoomOut(){
    if(camZoomMode==='fit'){
        setCamZoom(0.75);
        return;
    }

    const index=camZoomLevels.indexOf(camZoomMode);
    if(index>0)
        setCamZoom(camZoomLevels[index-1]);
}

camZoomOutBtn.addEventListener('click',camZoomOut);
camZoomInBtn.addEventListener('click',camZoomIn);
camZoomFitBtn.addEventListener('click',function(){setCamZoom('fit');});
camZoom100Btn.addEventListener('click',function(){setCamZoom(1);});

camZoomMode=readCamZoom();
applyCamZoom();

window.addEventListener('resize',function(){
    if(camZoomMode==='fit')applyCamZoom();
});

document.addEventListener('keydown',function(event){
    const target=event.target;
    const tag=target&&target.tagName?target.tagName.toLowerCase():'';

    if(tag==='input'||tag==='select'||tag==='textarea')return;

    if(event.key==='+'||event.key==='='){
        event.preventDefault();
        camZoomIn();
    }else if(event.key==='-'){
        event.preventDefault();
        camZoomOut();
    }else if(event.key==='f'||event.key==='F'){
        event.preventDefault();
        setCamZoom('fit');
    }else if(event.key==='0'){
        event.preventDefault();
        setCamZoom(1);
    }
});

function releasePreview(){
    fetch('/preview_stop',{method:'POST',keepalive:true,cache:'no-store'}).catch(function(){});
}

function stopPreview(){
    if(previewStopped)return;
    previewStopped=true;
    releasePreview();
}

function nextFrame(){
    if(previewStopped||previewPaused)return;
    img.src='/snapshot?t='+Date.now();
}

img.onload=function(){
    applyCamZoom();
    setTimeout(nextFrame,200);
};

img.onerror=function(){
    setTimeout(nextFrame,500);
};

window.addEventListener('pagehide',stopPreview);

document.addEventListener('visibilitychange',function(){
    if(document.hidden){
        previewPaused=true;
        releasePreview();
        resetSensorCropUiToSaved();
    }else if(!previewStopped){
        previewPaused=false;
        nextFrame();
    }
});

nextFrame();
</script>
)HTML";
    }

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleSnapshot()
{
    // An already-running recording always wins. Normally this cannot happen
    // because the preview gate blocks new starts, but keep the guard for races
    // and direct /snapshot requests.
    if (rejectWhileRecording("camera preview"))
        return;

    if (!esp_camera_sensor_get()) {
        stopCameraPreview();
        server.send(
            503,
            "text/plain; charset=utf-8",
            "Camera is not initialized"
        );
        return;
    }

    noteCameraPreviewActivity();

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            "Camera capture failed"
        );
        return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");

    server.client().write(
        fb->buf,
        fb->len
    );

    // The Image Motion page asks for ?im=1. Analyze exactly the JPEG that was
    // just shown in the browser, so the live status always describes the same
    // frame and no second camera capture is required. This path is diagnostic
    // only; WebConfig's preview gate still prevents it from starting a recording.
    if (
        server.hasArg("im") &&
        server.arg("im") == "1" &&
        fb->format == PIXFORMAT_JPEG &&
        fb->buf &&
        fb->len > 0
    ) {
        uint32_t nowMs = millis();
        if (
            imageMotionPreviewLastAnalysisMs == 0 ||
            (uint32_t)(nowMs - imageMotionPreviewLastAnalysisMs) >=
                IMAGE_MOTION_PREVIEW_ANALYSIS_INTERVAL_MS
        ) {
            imageMotionPreviewLastAnalysisMs = nowMs;

            ImageMotionDiagnostics diagnostics;
            (void)imageMotionAnalyzeJpeg(
                fb->buf,
                fb->len,
                fb->width,
                fb->height,
                diagnostics
            );
        }
    }

    esp_camera_fb_return(fb);
}


static void handlePreviewStop()
{
    stopCameraPreview();

    if (server.hasArg("im") && server.arg("im") == "1") {
        // Keep the learned background, but never let a diagnostic/live-preview
        // confirmation leak into the operational image_only trigger state.
        imageMotionBeginVerification();
        imageMotionPreviewLastAnalysisMs = 0;
    }

    server.send(
        204,
        "text/plain",
        ""
    );
}


// -------------------------------------------------------------
// LD2410S RADAR CONFIG
// -------------------------------------------------------------

static String radarRateText(
    uint32_t rateX10
)
{
    return
        String(rateX10 / 10U) +
        "." +
        String(rateX10 % 10U);
}


static void appendRadarRateOptions(
    String &html,
    uint32_t selected
)
{
    for (
        uint32_t rate = 5;
        rate <= 80;
        rate += 5
    ) {

        html +=
            "<option value='" +
            String(rate) +
            "'" +
            String(
                rate == selected
                ? " selected"
                : ""
            ) +
            ">" +
            radarRateText(rate) +
            " Hz</option>";
    }
}


static void handleRadarLive()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    // This endpoint only returns values already cached in RAM by radarLoop().
    // It does not enter configuration mode and does not touch the SD card.
    //
    // The Radar Config page refreshes this endpoint very frequently. Use that
    // existing traffic as a robust pause-lease heartbeat while the page is
    // actually visible. This avoids a false automatic resume if the browser
    // delays the separate 10-second keepalive behind the live radar requests.
    if (
        recordingAutomationPaused &&
        server.hasArg("visible") &&
        server.arg("visible") == "1"
    ) {
        renewRecordingPauseLease();
    }

    String json;

    json.reserve(
        1024
    );

    json +=
        "{\"recent\":";

    json +=
        radarGateEnergyIsRecent()
        ? "true"
        : "false";

    uint8_t targetState =
        radarLastTargetState();

    RadarSettings liveSettings;
    bool liveSettingsValid =
        radarGetCachedSettings(
            liveSettings
        );

    json +=
        ",\"state\":" +
        String(
            targetState
        );

    json +=
        ",\"sensor_present\":";

    json +=
        (targetState == 2 || targetState == 3)
        ? "true"
        : "false";

    json +=
        ",\"ot2\":";

    json +=
        digitalRead(PIR_PIN) == HIGH
        ? "true"
        : "false";

    json +=
        ",\"absence_sec\":" +
        String(
            liveSettingsValid
            ? liveSettings.absenceSec
            : 0
        );

    json +=
        ",\"distance\":" +
        String(
            radarLastTargetDistanceCm()
        );

    json +=
        ",\"motion\":";

    json +=
        radarMotionActive()
        ? "true"
        : "false";

    json +=
        ",\"remaining_ms\":" +
        String(
            radarMotionRemainingMs()
        );

    json +=
        ",\"last_gate\":" +
        String(
            radarLastMotionGate()
        );

    json +=
        ",\"energy\":[";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        if (gate > 0) {
            json += ',';
        }

        json +=
            String(
                radarGateEnergyDb(
                    gate
                ),
                1
            );
    }


    json +=
        "],\"last_trigger_age_ms\":[";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {
        if (gate > 0) {
            json += ',';
        }

        uint32_t ageMs = 0;

        if (
            radarGateLastTriggerAgeMs(
                gate,
                ageMs
            )
        ) {
            json += String(ageMs);
        } else {
            json += "-1";
        }
    }


    json +=
        "],\"diag_count\":" +
        String(radarDiagnosticCount()) +
        ",\"diag_capacity\":" +
        String(radarDiagnosticCapacity()) +
        "}";


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static String radarDiagnosticCalibrationText(
    uint8_t mode
)
{
    if (mode == (uint8_t)RADAR_CALIBRATION_QUIET)
        return "quiet";

    if (mode == (uint8_t)RADAR_CALIBRATION_MOTION)
        return "motion";

    return "none";
}


static String radarDiagnosticEnergyText(
    uint16_t deciDb
)
{
    if (deciDb == RADAR_DIAGNOSTIC_INVALID_ENERGY)
        return "-";

    return
        String(deciDb / 10U) +
        "." +
        String(deciDb % 10U);
}


static char radarDiagnosticZone(
    uint16_t deciDb,
    uint32_t triggerDb,
    uint32_t holdDb
)
{
    if (deciDb == RADAR_DIAGNOSTIC_INVALID_ENERGY)
        return 'X';

    uint32_t energy = deciDb;

    if (energy >= triggerDb * 10UL)
        return 'T';

    if (energy >= holdDb * 10UL)
        return 'H';

    return 'C';
}


static String radarDiagnosticTimestamp(
    const RadarDiagnosticSample &sample
)
{
    if (sample.epochSec < 1577836800UL)
        return "-";

    time_t seconds =
        (time_t)sample.epochSec;

    struct tm localTime;
    localtime_r(
        &seconds,
        &localTime
    );

    char buffer[32];
    strftime(
        buffer,
        sizeof(buffer),
        "%Y-%m-%d %H:%M:%S",
        &localTime
    );

    char millisPart[8];
    snprintf(
        millisPart,
        sizeof(millisPart),
        ".%03u",
        (unsigned int)sample.epochMs
    );

    return String(buffer) + String(millisPart);
}


static void handleRadarDiagnosticDownload()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    RadarSettings settings;
    bool settingsValid =
        radarGetCachedSettings(
            settings
        );

    uint16_t count =
        radarDiagnosticCount();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"sensorforge_radar_diagnostic.txt\""
    );

    server.setContentLength(
        CONTENT_LENGTH_UNKNOWN
    );

    server.send(
        200,
        "text/plain; charset=utf-8",
        ""
    );

    String line;
    line.reserve(768);

    line =
        "# SensorForge Radar RAM Diagnostic\n"
        "# RAM-only ring buffer; no SD/main-log writes.\n"
        "# Capture: 1 s baseline plus immediate target/OT2/ESP-motion/threshold-zone changes.\n"
        "# Oldest retained sample first; oldest entries are overwritten when the buffer is full.\n"
        "# Zone: T=at/above Trigger, H=below Trigger but at/above Hold, C=below Hold, X=invalid raw zero.\n";

    server.sendContent(line);

    line =
        "# samples=" +
        String(count) +
        " capacity=" +
        String(radarDiagnosticCapacity()) +
        "\n";

    server.sendContent(line);

    if (settingsValid) {
        line =
            "# settings: min_gate=" +
            String(settings.minGate) +
            " max_gate=" +
            String(settings.maxGate) +
            " absence_sec=" +
            String(settings.absenceSec) +
            " status_rate_hz=" +
            radarRateText(settings.statusRateX10) +
            " distance_rate_hz=" +
            radarRateText(settings.distanceRateX10) +
            " response_speed=" +
            String(settings.responseSpeed) +
            "\n";

        server.sendContent(line);

        uint32_t firstGateValue =
            settings.minGate == 0
            ? 0UL
            : settings.minGate + 1UL;

        if (firstGateValue > 15UL)
            firstGateValue = 15UL;

        uint32_t finalGateValue =
            settings.maxGate > 15UL
            ? 15UL
            : settings.maxGate;

        for (uint8_t gate = 0; gate < 16; ++gate) {
            bool operational =
                gate >= firstGateValue &&
                gate <= finalGateValue;

            line =
                "# G" +
                String(gate) +
                " trigger=" +
                String(settings.triggerThreshold[gate]) +
                " hold=" +
                String(settings.holdThreshold[gate]) +
                " operational=" +
                String(operational ? "yes" : "no") +
                "\n";

            server.sendContent(line);
        }
    } else {
        server.sendContent(
            "# settings: unavailable\n"
        );
    }

    line =
        "seq\ttime\tuptime_ms\ttarget_state\ttarget_present\tot2\tdistance_cm\tesp_motion\tcalibration";

    for (uint8_t gate = 0; gate < 16; ++gate) {
        line +=
            "\tG" +
            String(gate) +
            "_db\tG" +
            String(gate) +
            "_zone";
    }

    line += "\n";
    server.sendContent(line);

    for (uint16_t index = 0; index < count; ++index) {
        RadarDiagnosticSample sample;

        if (!radarDiagnosticGet(
                index,
                sample
            )) {
            continue;
        }

        bool targetPresent =
            sample.targetState == 2 ||
            sample.targetState == 3;

        line =
            String(sample.sequence) +
            "\t" +
            radarDiagnosticTimestamp(sample) +
            "\t" +
            String(sample.uptimeMs) +
            "\t" +
            String(sample.targetState) +
            "\t" +
            String(targetPresent ? "1" : "0") +
            "\t" +
            String(sample.ot2High ? "HIGH" : "LOW") +
            "\t" +
            String(sample.targetDistanceCm) +
            "\t" +
            String(sample.espMotionActive ? "1" : "0") +
            "\t" +
            radarDiagnosticCalibrationText(
                sample.calibrationMode
            );

        for (uint8_t gate = 0; gate < 16; ++gate) {
            line += "\t";
            line += radarDiagnosticEnergyText(
                sample.gateEnergyDeciDb[gate]
            );
            line += "\t";

            if (settingsValid) {
                line += radarDiagnosticZone(
                    sample.gateEnergyDeciDb[gate],
                    settings.triggerThreshold[gate],
                    settings.holdThreshold[gate]
                );
            } else {
                line += '?';
            }
        }

        line += "\n";
        server.sendContent(line);

        if ((index % 25U) == 24U)
            serviceWebLongOperation();
    }
}


static const char *radarCalibrationModeJsonName(
    RadarCalibrationMode mode
)
{
    if (mode == RADAR_CALIBRATION_QUIET)
        return "quiet";

    if (mode == RADAR_CALIBRATION_MOTION)
        return "motion";

    return "none";
}


static void appendRadarCalibrationSessionJson(
    String &json,
    RadarCalibrationMode mode
)
{
    json +=
        "{\"elapsed_ms\":" +
        String(
            radarCalibrationElapsedMs(
                mode
            )
        ) +
        ",\"samples\":" +
        String(
            radarCalibrationSampleCount(
                mode
            )
        ) +
        ",\"countdown_ms\":" +
        String(
            radarCalibrationCountdownRemainingMs(
                mode
            )
        ) +
        ",\"measurement_started\":" +
        String(
            radarCalibrationMeasurementStarted(mode)
            ? "true"
            : "false"
        ) +
        ",\"auto_completed\":" +
        String(
            radarCalibrationAutoCompleted(mode)
            ? "true"
            : "false"
        ) +
        ",\"quality_limited\":" +
        String(
            radarCalibrationQualityLimited(mode)
            ? "true"
            : "false"
        ) +
        ",\"aborted\":" +
        String(
            radarCalibrationAborted(mode)
            ? "true"
            : "false"
        ) +
        ",\"gates\":[";

    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {
        if (gate > 0)
            json += ',';

        RadarCalibrationGateStats stats;

        bool valid =
            radarCalibrationGetGateStats(
                mode,
                gate,
                stats
            );

        json +=
            "{\"valid\":";

        json +=
            valid
            ? "true"
            : "false";

        json +=
            ",\"samples\":" +
            String(stats.samples) +
            ",\"discarded\":" +
            String(stats.discardedSamples) +
            ",\"min\":" +
            String(stats.minimumDb, 1) +
            ",\"mean\":" +
            String(stats.meanDb, 1) +
            ",\"p10\":" +
            String(stats.p10Db, 1) +
            ",\"p50\":" +
            String(stats.p50Db, 1) +
            ",\"p95\":" +
            String(stats.p95Db, 1) +
            ",\"p99\":" +
            String(stats.p99Db, 1) +
            ",\"peak\":" +
            String(stats.peakDb, 1) +
            "}";
    }

    json +=
        "]}";
}


static void handleRadarCalibrationStatus()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    String json;

    json.reserve(
        4600
    );

    RadarCalibrationMode active =
        radarCalibrationActiveMode();

    json +=
        "{\"active\":\"" +
        String(
            radarCalibrationModeJsonName(
                active
            )
        ) +
        "\",\"target_samples\":" +
        String(
            radarCalibrationTargetValidSamples()
        ) +
        ",\"max_reports\":" +
        String(
            radarCalibrationMaxReports()
        ) +
        ",\"quiet\":";

    appendRadarCalibrationSessionJson(
        json,
        RADAR_CALIBRATION_QUIET
    );

    json +=
        ",\"motion\":";

    appendRadarCalibrationSessionJson(
        json,
        RADAR_CALIBRATION_MOTION
    );

    json +=
        "}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static void handleRadarCalibrationAction()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    RadarWebMaintenanceGuard maintenanceGuard;

    String action =
        server.arg("action");

    action.trim();
    action.toLowerCase();

    if (
        action == "start_quiet" ||
        action == "start_motion"
    ) {
        RadarCalibrationMode mode =
            action == "start_quiet"
            ? RADAR_CALIBRATION_QUIET
            : RADAR_CALIBRATION_MOTION;

        String calibrationError;

        if (!radarCalibrationStart(
                mode,
                calibrationError
            )) {
            server.send(
                409,
                "text/plain; charset=utf-8",
                "Radar-Kalibrierung konnte nicht gestartet werden: " +
                calibrationError
            );
            return;
        }

        String label =
            mode == RADAR_CALIBRATION_QUIET
            ? "Ruhemessung"
            : "Bewegungsmessung";

        consoleWrite(
            "RADAR",
            "Kalibrierung vorbereitet | " +
            label +
            " | Start in 10 s"
        );

        logWrite(
            "Radar calibration armed | " +
            label +
            " | countdown=10s"
        );

    } else if (action == "stop") {
        RadarCalibrationMode previous =
            radarCalibrationActiveMode();

        String calibrationError;

        if (!radarCalibrationStop(
                calibrationError
            )) {
            server.send(
                500,
                "text/plain; charset=utf-8",
                "Messung wurde gestoppt, aber der normale Gate-Bereich konnte nicht wiederhergestellt werden: " +
                calibrationError
            );
            return;
        }

        if (previous != RADAR_CALIBRATION_NONE) {
            consoleWrite(
                "RADAR",
                "Kalibrierung gestoppt"
            );

            logWrite(
                "Radar calibration stopped"
            );
        }

    } else if (action == "reset_quiet") {
        radarCalibrationReset(
            RADAR_CALIBRATION_QUIET
        );

    } else if (action == "reset_motion") {
        radarCalibrationReset(
            RADAR_CALIBRATION_MOTION
        );

    } else if (action == "reset_all") {
        radarCalibrationResetAll();

    } else {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ungültige Kalibrierungsaktion"
        );
        return;
    }

    handleRadarCalibrationStatus();
}


static void handleRadarConfig()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    RadarWebMaintenanceGuard maintenanceGuard;

    // Opening/navigating to Radar Config is explicit operator activity. If the
    // recording automation is paused, refresh its lease immediately before the
    // synchronous UART settings read below.
    if (recordingAutomationPaused)
        renewRecordingPauseLease();

    RadarSettings settings;
    String error;

    bool recordingActive =
        recorderIsOpen();

    bool justSaved =
        server.hasArg("saved") &&
        server.arg("saved") == "1";

    bool justRestoredDefaults =
        server.hasArg("defaults") &&
        server.arg("defaults") == "1";

    bool readOk =
        false;

    bool uartReadTimedOutButCacheAvailable =
        false;


    // radarWriteSettings() already performs a complete read-back verification
    // inside the SAME configuration session and caches those verified values.
    // Do not immediately open a second LD2410S configuration session after the
    // POST/Redirect/GET cycle. Some modules need a short recovery period after
    // returning to standard-report mode and occasionally miss that second
    // enable-config ACK even though the write itself was fully successful.
    if (
        recordingActive ||
        justSaved ||
        justRestoredDefaults
    ) {

        readOk =
            radarGetCachedSettings(
                settings
            );

        if (!readOk && recordingActive) {
            error =
                "no cached radar settings available while recording";
        }
    }


    // Normal page opens still request a fresh hardware read. If that one
    // synchronous configuration transaction happens to time out while the
    // radar is otherwise alive, fall back to the most recently verified cache
    // instead of presenting a misleading wiring/sensor failure to the user.
    if (!readOk && !recordingActive) {

        String freshReadError;

        bool freshReadOk =
            radarReadSettings(
                settings,
                freshReadError
            );

        if (freshReadOk) {
            readOk = true;
            error = "";

        } else {
            RadarSettings cachedSettings;

            if (radarGetCachedSettings(cachedSettings)) {
                settings = cachedSettings;
                readOk = true;
                uartReadTimedOutButCacheAvailable = true;
                error = freshReadError;

            } else {
                error = freshReadError;
            }
        }
    }


    String html =
        htmlHeader();

    html +=
        "<h2>Radar Config - HLK-LD2410S</h2>";


    if (recordingActive) {
        html +=
            "<p style='color:#9a5a00'><b>Aufnahme laeuft:</b> "
            "Die Seite verwendet gecachte Radar-Einstellungen. "
            "Live-Energien laufen weiter. Speichern ist erlaubt; "
            "waehrend des kurzen UART-Schreibvorgangs kann es zu einer "
            "kleinen Luecke zwischen Videoframes kommen.</p>";

    } else if (uartReadTimedOutButCacheAvailable) {
        html +=
            "<div class='flash-notice' style='border-color:#d6a100'>"
            "<strong>Radar antwortet, Konfigurations-Lesen war kurzzeitig beschaeftigt</strong>"
            "<span class='muted'>Die zuletzt erfolgreich gelesenen bzw. verifizierten Radar-Werte werden angezeigt. "
            "Das ist kein Hinweis auf eine defekte Verdrahtung. Mit <b>Read again</b> kann jederzeit neu gelesen werden.</span>"
            "</div>";
    }


    html +=
        "<p><b>OT2 presence:</b> " +
        String(
            digitalRead(PIR_PIN) == HIGH
            ? "PERSON PRESENT"
            : "clear"
        ) +
        "</p>";


    if (radarReportIsRecent()) {

        html +=
            "<p><b>UART live report:</b> state=" +
            String(
                radarLastTargetState()
            ) +
            ", distance=" +
            String(
                radarLastTargetDistanceCm()
            ) +
            " cm</p>";
    }


    html +=
        "<p><b>ESP motion trigger (2 s):</b> " +
        String(
            radarMotionActive()
            ? "ACTIVE"
            : "clear"
        );

    if (radarLastMotionGate() >= 0) {
        html +=
            " &nbsp; last gate=" +
            String(
                radarLastMotionGate()
            ) +
            ", energy=" +
            String(
                radarLastMotionEnergyDb(),
                1
            ) +
            " dB";
    }

    html +=
        "</p>";


    html +=
        "<p><b>UART wiring:</b> "
        "LD2410S OT1/TX &rarr; ESP RX GPIO " +
        String(radarRxPin()) +
        ", LD2410S RX &larr; ESP TX GPIO " +
        String(radarTxPin()) +
        "</p>";


    if (!readOk) {

        html +=
            "<p style='color:#b00020'><b>Sensor/UART nicht erreichbar:</b> " +
            htmlEscape(error) +
            "</p>";

        html +=
            "<p>Pruefe 3.3V, GND und die gekreuzten TX/RX-Leitungen. "
            "OT2 allein reicht fuer die Aufnahme, aber nicht fuer Radar Config.</p>";

        html +=
            "<br><a href='/radar_config'><button>Retry</button></a>";

        html +=
            "<a href='/'><button>Back</button></a>";

        html +=
            htmlFooter();

        server.send(
            200,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    if (justSaved) {

        bool calibrationApplied =
            server.hasArg("calibration") &&
            server.arg("calibration") == "1";

        html +=
            calibrationApplied
            ? "<p style='color:#087a00'><b>Kalibrierung übernommen, in den LD2410S geschrieben und erfolgreich verifiziert.</b></p>"
            : "<p style='color:#087a00'><b>Gespeichert und erfolgreich verifiziert.</b></p>";
    }


    if (justRestoredDefaults) {

        html +=
            "<div class='flash-notice'>"
            "<strong>Hi-Link Standardwerte wiederhergestellt</strong>"
            "<span class='muted'>Die Radar-Parameter wurden geschrieben und erfolgreich verifiziert.</span>"
            "</div>";
    }


    html +=
        "<h3>Radar Live-Status</h3>"
        "<div class='flash-notice' style='border-color:#8aa6bf'>"
        "<strong>Aktueller Radarstatus</strong>"
        "<div style='display:grid;grid-template-columns:minmax(120px,150px) minmax(0,1fr);column-gap:12px;row-gap:5px;margin-top:8px;align-items:start'>"
        "<b style='white-space:nowrap'>Radar erkennt:</b><span id='radarSensorDecision' style='display:block;min-height:1.35em'>warte auf Daten...</span>"
        "<b style='white-space:nowrap'>OT2-Ausgang:</b><span id='radarOt2Decision' style='display:block;min-height:1.35em'>-</span>"
        "<b style='white-space:nowrap'>Warum:</b><span id='radarDerivedCause' style='display:block;min-height:2.7em;line-height:1.35'>-</span>"
        "</div>"
        "<div style='display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin-top:10px'>"
        "<a href='/radar_diag_download'><button type='button'>Radar-Diagnose herunterladen</button></a>"
        "<span class='muted'>RAM-Puffer: <b id='radarDiagBufferStatus'>- / 400</b> Einträge · älteste werden automatisch überschrieben</span>"
        "</div>"
        "</div>";

    html +=
        "<h3>Live Gate Energy</h3>"
        "<p>Aktualisierung ca. 4x pro Sekunde. "
        "<b>TRIGGER</b> bedeutet: Energie liegt auf/über der Trigger-Schwelle. "
        "<b>HOLD</b> bedeutet: unter Trigger, aber noch auf/über der Hold-Schwelle. "
        "<b>Letzter Trigger</b> wird direkt im ESP aus jedem Radarreport gespeichert; "
        "auch ein kurzer Trigger zwischen zwei Browser-Aktualisierungen bleibt daher sichtbar.</p>";

    html +=
        "<p><b>UART:</b> "
        "<span id='radarLiveStatus'>waiting...</span>"
        " &nbsp; <b>ESP-Schnelltrigger:</b> "
        "<span id='radarLiveMotion'>-</span>"
        " &nbsp; <b>Messstatistik:</b> "
        "<span id='radarCalLiveMode'>keine</span>"
        "</p>";

    html +=
        "<div style='overflow-x:auto'>"
        "<table style='border-collapse:collapse'>"
        "<tr>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>ca. Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Energy dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Mess-Min</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Mess-P99</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Mess-Peak</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hold dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger-Margin</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Status</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Letzter Trigger</th>"
        "</tr>";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        bool inConfiguredRange =
            gate >= settings.minGate &&
            gate <= settings.maxGate;

        html +=
            "<tr id='liveRow" +
            String(gate) +
            "' data-active='" +
            String(
                inConfiguredRange
                ? "1"
                : "0"
            ) +
            "'" +
            String(
                inConfiguredRange
                ? ""
                : " style='opacity:0.45'"
            ) +
            ">"
            "<td style='padding:4px;text-align:center'>" +
            String(gate) +
            "</td>"
            "<td style='padding:4px;text-align:center'>" +
            String(
                gate * 0.7f,
                1
            ) +
            " m</td>"
            "<td id='liveEnergy" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveCalMin" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveCalP99" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveCalPeak" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveThreshold" +
            String(gate) +
            "' style='padding:4px;text-align:right'>" +
            String(
                settings.triggerThreshold[
                    gate
                ]
            ) +
            "</td>"
            "<td id='liveHoldThreshold" +
            String(gate) +
            "' style='padding:4px;text-align:right'>" +
            String(
                settings.holdThreshold[
                    gate
                ]
            ) +
            "</td>"
            "<td id='liveMargin" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveState" +
            String(gate) +
            "' style='padding:4px'>-</td>"
            "<td id='liveLastTrigger" +
            String(gate) +
            "' style='padding:4px;text-align:right;white-space:nowrap'>-</td>"
            "</tr>";
    }


    html +=
        "</table></div>";


    html +=
        "<script>"
        "(function(){"
        "var busy=false;"
        "function formatTriggerAge(ms){"
            "ms=Number(ms);"
            "if(!Number.isFinite(ms)||ms<0)return '-';"
            "if(ms<1000)return '<1s';"
            "var sec=Math.floor(ms/1000);"
            "if(sec<60)return sec+'s';"
            "var min=Math.floor(sec/60);"
            "var rem=sec%60;"
            "if(min<60)return min+'m'+(rem<10?'0':'')+rem+'s';"
            "var hour=Math.floor(min/60);"
            "var minRem=min%60;"
            "return hour+'h'+(minRem<10?'0':'')+minRem+'m';"
        "}"
        "function updateRadarLive(){"
            "if(busy)return;"
            "busy=true;"
            "fetch('/radar_live?visible='+(document.hidden?'0':'1')+'&t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
            ".then(function(d){"
                "var s=document.getElementById('radarLiveStatus');"
                "var m=document.getElementById('radarLiveMotion');"
                "var sd=document.getElementById('radarSensorDecision');"
                "var od=document.getElementById('radarOt2Decision');"
                "var dc=document.getElementById('radarDerivedCause');"
                "var db=document.getElementById('radarDiagBufferStatus');"
                "if(db&&Number.isFinite(Number(d.diag_count))&&Number.isFinite(Number(d.diag_capacity)))db.textContent=Number(d.diag_count)+' / '+Number(d.diag_capacity);"
                "if(!d.recent){"
                    "s.textContent='keine aktuellen Standarddaten';"
                    "if(sd)sd.textContent='keine aktuellen Daten';"
                    "if(od)od.textContent=d.ot2?'HIGH':'LOW';"
                    "if(dc)dc.textContent='nicht bestimmbar';"
                "}else{"
                    "s.textContent='Status '+d.state+' · Distanz '+d.distance+' cm';"
                    "if(sd)sd.textContent=d.sensor_present?'BEWEGUNG':'Keine Bewegung';"
                    "if(od){od.textContent=d.ot2?'HIGH':'LOW';od.style.fontWeight='bold';}"
                "}"
                "m.textContent=d.motion?'AKTIV ('+d.remaining_ms+' ms)':'keine Bewegung';"
                "var bestTrigger=null,bestHold=null;"
                "for(var i=0;i<16;i++){"
                    "var e=document.getElementById('liveEnergy'+i);"
                    "var t=document.getElementById('liveThreshold'+i);"
                    "var h=document.getElementById('liveHoldThreshold'+i);"
                    "var g=document.getElementById('liveMargin'+i);"
                    "var q=document.getElementById('liveState'+i);"
                    "var a=document.getElementById('liveLastTrigger'+i);"
                    "var row=document.getElementById('liveRow'+i);"
                    "if(!e||!t||!h||!g||!q||!a)continue;"
                    "a.textContent=(d.last_trigger_age_ms&&i<d.last_trigger_age_ms.length)?formatTriggerAge(d.last_trigger_age_ms[i]):'-';"
                    "if(!d.recent){"
                        "e.textContent='-';g.textContent='-';q.textContent='-';q.style.fontWeight='normal';"
                        "continue;"
                    "}"
                    "var energy=Number(d.energy[i]);"
                    "var threshold=Number(t.textContent);"
                    "var hold=Number(h.textContent);"
                    "var margin=energy-threshold;"
                    "e.textContent=energy.toFixed(1);"
                    "g.textContent=(margin>=0?'+':'')+margin.toFixed(1);"
                    "var valid=Number.isFinite(energy)&&energy>0;"
                    "if(!valid){q.textContent='keine Daten';q.style.fontWeight='normal';continue;}"
                    "if(energy>=threshold){q.textContent='TRIGGER';q.style.fontWeight='bold';}"
                    "else if(energy>=hold){q.textContent='HOLD';q.style.fontWeight='bold';}"
                    "else{q.textContent='clear';q.style.fontWeight='normal';}"
                    "var active=!row||row.dataset.active!=='0';"
                    "if(active&&energy>=threshold){var tm=energy-threshold;if(!bestTrigger||tm>bestTrigger.margin)bestTrigger={gate:i,energy:energy,threshold:threshold,margin:tm};}"
                    "else if(active&&energy>=hold){var hm=energy-hold;if(!bestHold||hm>bestHold.margin)bestHold={gate:i,energy:energy,hold:hold,margin:hm};}"
                "}"
                "if(d.recent&&dc){"
                    "if(bestTrigger){dc.textContent='Gate '+bestTrigger.gate+' hat ausgelöst.';if(!d.sensor_present&&!d.ot2)dc.textContent+=' Der Sensor hat die Bewegung noch nicht übernommen.';}"
                    "else if(bestHold){if(d.sensor_present||d.ot2)dc.textContent='Gate '+bestHold.gate+' hält die erkannte Bewegung noch aktiv.';else dc.textContent='Gate '+bestHold.gate+' liegt noch im Haltebereich, aber der Sensor meldet keine Bewegung.';}"
                    "else if(d.sensor_present||d.ot2){dc.textContent='Der Sensor hält die letzte Erkennung noch nach.';}"
                    "else{dc.textContent='Kein Gate meldet Bewegung.';}"
                    "if(Boolean(d.sensor_present)!==Boolean(d.ot2))dc.textContent+=' Hinweis: Radarstatus und OT2 passen gerade nicht zusammen.';"
                "}"
            "})"
            ".catch(function(){"
                "var s=document.getElementById('radarLiveStatus');"
                "if(s)s.textContent='live read failed';"
            "})"
            ".then(function(){busy=false;});"
        "}"
        "updateRadarLive();"
        "setInterval(updateRadarLive,250);"
        "})();"
        "</script>";


    html +=
        "<div id='radarCalibration' class='settings-section' style='margin-top:18px'>"
        "<h3>Radar Kalibrierung</h3>"
        "<p class='muted'>Die Kalibrierung ist bewusst einfach: Messart starten, den überwachten Bereich verlassen bzw. "
        "für die Bewegungsmessung die gewünschte Bewegung vorbereiten. Danach läuft zuerst ein <b>10-Sekunden-Countdown</b>. "
        "Erst anschließend werden Messwerte gesammelt. Die Messung beendet sich automatisch, sobald jedes Gate 100 gültige "
        "Werte erreicht hat. Ein Gate mit 100 verworfenen Werten gilt als unzuverlässig; spätestens nach 150 Radar-Reports wird ebenfalls beendet. "
        "Gates mit zu vielen Fehlwerten werden "
        "bei der späteren Empfehlung vom nächstgelegenen brauchbaren Gate abgeleitet.</p>"
        "<p class='muted'>Während der Messung wird der LD2410S vorübergehend auf Gate 0–16 erweitert. Der normale Min./Max.-Gate-Bereich "
        "wird danach automatisch wiederhergestellt. Ausgeblendete Gates werden dadurch nur kalibriert und nicht für die normale "
        "Alarmierung aktiviert.</p>"
        "<p><b>Status:</b> <span id='radarCalState'>keine Messung aktiv</span> "
        "&nbsp; <b>Laufzeit:</b> <span id='radarCalElapsed'>00:00</span> "
        "&nbsp; <b>Messzyklen:</b> <span id='radarCalSamples'>0</span></p>"
        "<div style='display:flex;flex-wrap:wrap;gap:6px;margin:10px 0 14px'>"
        "<button type='button' id='calStartQuiet'>Ruhemessung starten</button>"
        "<button type='button' id='calStartMotion'>Bewegungsmessung starten</button>"
        "</div>"
        "<div id='radarCalProgressModal' class='modal-backdrop' hidden>"
        "<div class='modal-card radar-cal-modal-card' role='dialog' aria-modal='true' aria-labelledby='radarCalModalTitle'>"
        "<div class='radar-cal-kicker'>Radar-Kalibrierung</div>"
        "<h3 id='radarCalModalTitle'>Messung</h3>"
        "<div id='radarCalModalBig' class='radar-cal-big'>10</div>"
        "<div id='radarCalModalBigLabel' class='radar-cal-big-label'>Sekunden bis Messbeginn</div>"
        "<div class='radar-cal-progress'><span id='radarCalModalProgressBar'></span></div>"
        "<div id='radarCalModalHint' class='radar-cal-hint'>Messbereich jetzt verlassen.</div>"
        "<div class='radar-cal-meta'>"
        "<div><b id='radarCalModalCompleted'>0 / 16</b><span>Gates abgeschlossen</span></div>"
        "<div><b id='radarCalModalWeakest'>0 / 100</b><span>Wenigste gültige Werte</span></div>"
        "<div><b id='radarCalModalReports'>0 / 150</b><span>Radar-Reports</span></div>"
        "</div>"
        "<div class='modal-actions'>"
        "<button type='button' id='radarCalModalCancel'>Messung abbrechen</button>"
        "<button type='button' id='radarCalModalApply' class='primary' hidden>Werte übernehmen + auf Radar schreiben</button>"
        "<button type='button' id='radarCalModalClose' hidden>Nur Ergebnisse anzeigen</button>"
        "</div>"
        "</div></div>"

        "<h4>Ruhemessung</h4>"
        "<div style='overflow-x:auto'><table style='border-collapse:collapse;min-width:760px'>"
        "<tr><th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gültig</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Verworfen</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Min</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Ø</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>P95</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>P99</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Peak</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hinweis</th></tr>";

    for (uint8_t gate = 0; gate < 16; ++gate) {
        html +=
            "<tr>"
            "<td style='padding:4px;text-align:center'>" + String(gate) + "</td>"
            "<td style='padding:4px;text-align:center'>" + String(gate * 0.7f, 1) + " m</td>"
            "<td id='calQuietSamples" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietDiscarded" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietMin" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietMean" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietP95" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietP99" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietPeak" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calQuietNote" + String(gate) + "' style='padding:4px'>-</td>"
            "</tr>";
    }

    html +=
        "</table></div>"
        "<p class='muted'>P99 bedeutet: 99 % aller Messwerte lagen auf oder unter diesem Wert. "
        "Der Peak ist nur der höchste Einzelwert. Liegt er deutlich über P99, wird er als Einzelspitze markiert.</p>"

        "<h4>Bewegungsmessung</h4>"
        "<div style='overflow-x:auto'><table style='border-collapse:collapse;min-width:760px'>"
        "<tr><th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gültig</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Verworfen</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Min</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Ø</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>P10</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>P95</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>P99</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Peak</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hinweis</th></tr>";

    for (uint8_t gate = 0; gate < 16; ++gate) {
        html +=
            "<tr>"
            "<td style='padding:4px;text-align:center'>" + String(gate) + "</td>"
            "<td style='padding:4px;text-align:center'>" + String(gate * 0.7f, 1) + " m</td>"
            "<td id='calMotionSamples" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionDiscarded" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionMin" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionMean" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionP10" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionP95" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionP99" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionPeak" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calMotionNote" + String(gate) + "' style='padding:4px'>-</td>"
            "</tr>";
    }

    html +=
        "</table></div>"
        "<p class='muted'>P10 ist ein bewusst niedriger Bewegungswert: Etwa 90 % der gültigen "
        "Bewegungsmessungen liegen auf oder über diesem Wert. Er wird nur für einen vorläufigen "
        "Vorschlag verwendet, wenn keine Ruhemessung vorhanden ist.</p>"

        "<h4>Vergleich und Trigger-/Hold-Vorschlag</h4>"
        "<p class='muted'>Am zuverlässigsten ist eine Messung mit <b>Ruhe und Bewegung</b>. Dann bleibt "
        "die bisherige Vergleichslogik aktiv. Wenn nur eine Phase verfügbar ist, kann SensorForge "
        "trotzdem klar als vorläufig markierte Werte vorschlagen: <b>nur Ruhe: Trigger = P99 + 3 dB</b>, "
        "<b>nur Bewegung: Trigger = P10 - 3 dB</b>. Hold wird automatisch passend darunter gesetzt. "
        "Wenn Ruhewerte vorhanden sind, bleibt Hold oberhalb des gemessenen Ruhepegels; bei nur Bewegung "
        "wird Hold vorläufig 3 dB unter Trigger gesetzt. Pro Gate werden mindestens 100 gültige Messwerte "
        "benötigt. Ein vorläufiger Wert sollte später möglichst mit einer vollständigen Ruhe-/Bewegungsmessung "
        "kontrolliert werden. Erreicht ein Gate innerhalb der maximal 150 Radar-Reports keine 100 gültigen Werte, "
        "wird sein Vorschlag vom nächstgelegenen brauchbaren Gate abgeleitet. Für näher liegende Gates werden "
        "Trigger und Hold dabei konservativ um 3 dB pro Gate angehoben; solche Werte sind ausdrücklich als "
        "<b>abgeleitet</b> gekennzeichnet.</p>"
        "<div style='overflow-x:auto'><table style='border-collapse:collapse;min-width:720px'>"
        "<tr><th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Ruhe P99</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Bewegung P99</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Abstand</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger aktuell</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hold aktuell</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger Vorschlag</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hold Vorschlag</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Qualität</th></tr>";

    for (uint8_t gate = 0; gate < 16; ++gate) {
        html +=
            "<tr>"
            "<td style='padding:4px;text-align:center'>" + String(gate) + "</td>"
            "<td id='calCmpQuiet" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calCmpMotion" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calCmpGap" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calCmpCurrentTrigger" + String(gate) + "' style='padding:4px;text-align:right'>" +
            String(settings.triggerThreshold[gate]) + "</td>"
            "<td id='calCmpCurrentHold" + String(gate) + "' style='padding:4px;text-align:right'>" +
            String(settings.holdThreshold[gate]) + "</td>"
            "<td id='calCmpSuggestedTrigger" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calCmpSuggestedHold" + String(gate) + "' style='padding:4px;text-align:right'>-</td>"
            "<td id='calCmpQuality" + String(gate) + "' style='padding:4px'>-</td>"
            "</tr>";
    }

    html +=
        "</table></div>"
        "<div style='margin-top:12px'>"
        "<button type='button' id='calApplyRecommendations' disabled>"
        "Vorgeschlagene Trigger + Hold-Werte übernehmen + verifizieren</button>"
        "</div>"
        "<p class='muted'>Nach einer erfolgreich abgeschlossenen Messung werden die Vorschlagswerte automatisch in die "
        "Eingabefelder weiter unten übernommen. Erst <b>Werte übernehmen + auf Radar schreiben</b> schreibt die komplette "
        "Radar-Konfiguration mit Write + Verify in den LD2410S. Danach wird die Seite mit den tatsächlich bestätigten "
        "Sensorwerten neu geladen. Die Kalibrierung schlägt für Hold niemals 0 vor.</p>"
        "</div>";


    html +=
        "<script>"
        "(function(){"
        "var busy=false,lastData=null,lastDisplayMode='quiet',modalCompletionVisible=false,suggestedTrigger=new Array(16).fill(null),suggestedHold=new Array(16).fill(null);"
        "function el(id){return document.getElementById(id);}"
        "function db(v){return Number(v).toFixed(1);}"
        "function timeText(ms){var s=Math.floor(Number(ms||0)/1000),m=Math.floor(s/60);s%=60;return String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');}"
        "function setText(id,v){var x=el(id);if(x)x.textContent=v;}"
        "function renderSession(prefix,session,target){"
            "target=Number(target||100);"
            "for(var i=0;i<16;i++){"
                "var g=session&&session.gates?session.gates[i]:null;"
                "var samples=g?Number(g.samples)||0:0;"
                "var ok=!!(g&&g.valid&&samples>0);"
                "setText(prefix+'Samples'+i,g?String(samples):'-');"
                "setText(prefix+'Discarded'+i,g?String(Number(g.discarded)||0):'-');"
                "setText(prefix+'Min'+i,ok?db(g.min):'-');"
                "setText(prefix+'Mean'+i,ok?db(g.mean):'-');"
                "setText(prefix+'P10'+i,ok?db(g.p10):'-');"
                "setText(prefix+'P95'+i,ok?db(g.p95):'-');"
                "setText(prefix+'P99'+i,ok?db(g.p99):'-');"
                "setText(prefix+'Peak'+i,ok?db(g.peak):'-');"
                "var note='-';"
                "if(session&&session.quality_limited&&samples<target)note='zu viele Fehlwerte · wird abgeleitet';"
                "else if(session&&session.aborted&&samples<target)note='abgebrochen · unvollständig';"
                "else if(ok&&samples>=target&&Number(g.peak)-Number(g.p99)>=6)note='Einzelspitze +'+db(Number(g.peak)-Number(g.p99))+' dB';"
                "else if(samples>=target)note='OK';"
                "setText(prefix+'Note'+i,note);"
            "}"
        "}"
        "function holdFromQuiet(trigger,quiet){"
            "if(!isFinite(trigger)||!isFinite(quiet)||trigger<2)return null;"
            "var hold=Math.max(Math.ceil(quiet+1),trigger-3,1);"
            "hold=Math.min(95,trigger-1,hold);"
            "return hold>=1&&hold<trigger?hold:null;"
        "}"
        "function holdFromMotionOnly(trigger){"
            "if(!isFinite(trigger)||trigger<2)return null;"
            "var hold=Math.max(1,Math.min(95,trigger-3));"
            "return hold<trigger?hold:null;"
        "}"
        "function recommendation(q,m){"
            "var qReady=!!(q&&q.valid&&Number(q.samples)>=100&&isFinite(Number(q.p99)));"
            "var mReady=!!(m&&m.valid&&Number(m.samples)>=100&&isFinite(Number(m.p99))&&isFinite(Number(m.p10)));"
            "if(qReady&&mReady){"
                "var quiet=Number(q.p99),move=Number(m.p99),gap=move-quiet;"
                "if(!isFinite(gap)||gap<4)return {gap:gap,trigger:null,hold:null,quality:'unzureichend · Ruhe und Bewegung zu nah'};"
                "var margin=Math.max(3,Math.min(8,gap*0.25));"
                "var trigger=Math.ceil(quiet+margin);"
                "var upper=Math.floor(move-2);"
                "if(trigger>upper)trigger=upper;"
                "trigger=Math.max(0,Math.min(95,trigger));"
                "if(trigger<=quiet)return {gap:gap,trigger:null,hold:null,quality:'unzureichend · kein sicherer Spielraum'};"
                "var hold=holdFromQuiet(trigger,quiet);"
                "if(hold===null)return {gap:gap,trigger:null,hold:null,quality:'unzureichend · kein sicherer Hold-Spielraum'};"
                "var quality=gap>=12?'sehr gut':(gap>=8?'gut':'knapp');"
                "return {gap:gap,trigger:trigger,hold:hold,quality:'vollständig · '+quality};"
            "}"
            "if(qReady){"
                "var quietOnly=Number(q.p99);"
                "var quietTrigger=Math.max(0,Math.min(95,Math.ceil(quietOnly+3)));"
                "if(quietTrigger<=quietOnly)return {gap:null,trigger:null,hold:null,quality:'kein Spielraum über dem Ruhewert'};"
                "var quietHold=holdFromQuiet(quietTrigger,quietOnly);"
                "if(quietHold===null)return {gap:null,trigger:null,hold:null,quality:'kein sicherer Hold-Spielraum'};"
                "return {gap:null,trigger:quietTrigger,hold:quietHold,quality:'vorläufig · nur Ruhe (Trigger +3 dB, Hold über Ruhe)'};"
            "}"
            "if(mReady){"
                "var motionLow=Number(m.p10);"
                "var motionTrigger=Math.max(0,Math.min(95,Math.floor(motionLow-3)));"
                "if(motionTrigger>=motionLow)return {gap:null,trigger:null,hold:null,quality:'kein Spielraum unter dem Bewegungswert'};"
                "var motionHold=holdFromMotionOnly(motionTrigger);"
                "if(motionHold===null)return {gap:null,trigger:null,hold:null,quality:'kein sicherer Hold-Spielraum'};"
                "return {gap:null,trigger:motionTrigger,hold:motionHold,quality:'vorläufig · nur Bewegung (Trigger P10 - 3 dB, Hold -3 dB)'};"
            "}"
            "return null;"
        "}"
        "function gateHasAnyValidSamples(q,m){"
            "return !!((q&&q.valid&&Number(q.samples)>0)||(m&&m.valid&&Number(m.samples)>0));"
        "}"
        "function gateNeedsDerived(index,data){"
            "var target=Number(data.target_samples||100);"
            "var q=data.quiet&&!data.quiet.aborted&&data.quiet.gates?data.quiet.gates[index]:null;"
            "var m=data.motion&&!data.motion.aborted&&data.motion.gates?data.motion.gates[index]:null;"
            "var qBad=!!(data.quiet&&!data.quiet.aborted&&data.quiet.quality_limited&&q&&Number(q.samples)<target);"
            "var mBad=!!(data.motion&&!data.motion.aborted&&data.motion.quality_limited&&m&&Number(m.samples)<target);"
            "return qBad||mBad;"
        "}"
        "function deriveMissingRecommendation(index,direct,data){"
            "var q=data.quiet&&!data.quiet.aborted&&data.quiet.gates?data.quiet.gates[index]:null;"
            "var m=data.motion&&!data.motion.aborted&&data.motion.gates?data.motion.gates[index]:null;"
            "if(!gateNeedsDerived(index,data)&&gateHasAnyValidSamples(q,m))return null;"
            "var best=-1,bestDistance=99;"
            "for(var j=0;j<16;j++){"
                "var candidate=direct[j];"
                "if(!candidate||candidate.trigger===null||candidate.hold===null)continue;"
                "var distance=Math.abs(j-index);"
                "if(distance<bestDistance||(distance===bestDistance&&j<best)){best=j;bestDistance=distance;}"
            "}"
            "if(best<0)return null;"
            "var source=direct[best];"
            "var nearBoost=index<best?(best-index)*3:0;"
            "var trigger=Math.max(2,Math.min(95,Number(source.trigger)+nearBoost));"
            "var hold=Math.max(1,Math.min(trigger-1,Number(source.hold)+nearBoost));"
            "if(!isFinite(trigger)||!isFinite(hold)||hold>=trigger)return null;"
            "var detail=nearBoost>0?' +'+nearBoost+' dB Nahbereich':'';"
            "return {gap:null,trigger:trigger,hold:hold,quality:'abgeleitet · Gate '+best+detail,derived:true,sourceGate:best};"
        "}"
        "function renderComparison(d){"
            "var usable=0,direct=new Array(16).fill(null),finalRec=new Array(16).fill(null);"
            "for(var i=0;i<16;i++){"
                "var q=d.quiet&&!d.quiet.aborted&&d.quiet.gates?d.quiet.gates[i]:null;"
                "var m=d.motion&&!d.motion.aborted&&d.motion.gates?d.motion.gates[i]:null;"
                "direct[i]=recommendation(q,m);"
                "finalRec[i]=gateNeedsDerived(i,d)?null:direct[i];"
            "}"
            "for(var i=0;i<16;i++){if(!finalRec[i]||finalRec[i].trigger===null||finalRec[i].hold===null){var derived=deriveMissingRecommendation(i,direct,d);if(derived)finalRec[i]=derived;}}"
            "for(var i=0;i<16;i++){"
                "var q=d.quiet&&!d.quiet.aborted&&d.quiet.gates?d.quiet.gates[i]:null;"
                "var m=d.motion&&!d.motion.aborted&&d.motion.gates?d.motion.gates[i]:null;"
                "var target=Number(d.target_samples||100),qok=!!(q&&q.valid&&Number(q.samples)>=target),mok=!!(m&&m.valid&&Number(m.samples)>=target);"
                "setText('calCmpQuiet'+i,qok?db(q.p99):'-');"
                "setText('calCmpMotion'+i,mok?db(m.p99):'-');"
                "var r=finalRec[i];"
                "suggestedTrigger[i]=r&&r.trigger!==null?r.trigger:null;"
                "suggestedHold[i]=r&&r.hold!==null?r.hold:null;"
                "setText('calCmpGap'+i,r&&r.gap!==null&&isFinite(r.gap)?((r.gap>=0?'+':'')+db(r.gap)):(r&&r.derived?'abgeleitet':(r&&r.trigger!==null?'nicht messbar':'-')));"
                "setText('calCmpSuggestedTrigger'+i,suggestedTrigger[i]!==null?String(suggestedTrigger[i]):'-');"
                "setText('calCmpSuggestedHold'+i,suggestedHold[i]!==null?String(suggestedHold[i]):'-');"
                "setText('calCmpQuality'+i,r?r.quality:'zu wenig Daten');"
                "if(suggestedTrigger[i]!==null&&suggestedHold[i]!==null)usable++;"
            "}"
            "var b=el('calApplyRecommendations');if(b){b.disabled=usable===0;b.textContent=usable?'Vorgeschlagene Trigger + Hold-Werte übernehmen + verifizieren ('+usable+' Gates)':'Vorgeschlagene Trigger + Hold-Werte übernehmen + verifizieren';}"
        "}"
        "function renderLiveCalibration(d){"
            "var mode=d.active!=='none'?d.active:lastDisplayMode;"
            "var session=mode==='motion'?d.motion:d.quiet;"
            "if(d.active!=='none')lastDisplayMode=d.active;"
            "if((!session||!Number(session.samples))&&d.quiet&&Number(d.quiet.samples)){mode='quiet';session=d.quiet;}"
            "else if((!session||!Number(session.samples))&&d.motion&&Number(d.motion.samples)){mode='motion';session=d.motion;}"
            "setText('radarCalLiveMode',session&&Number(session.samples)?(mode==='quiet'?'Ruhe':'Bewegung'):'keine');"
            "for(var i=0;i<16;i++){"
                "var g=session&&session.gates?session.gates[i]:null;var ok=!!(g&&g.valid);"
                "setText('liveCalMin'+i,ok?db(g.min):'-');"
                "setText('liveCalP99'+i,ok?db(g.p99):'-');"
                "setText('liveCalPeak'+i,ok?db(g.peak):'-');"
            "}"
        "}"
        "function calibrationProgress(session,target){"
            "target=Number(target||100);var completed=0,failed=0,incomplete=0,weakest=target;"
            "for(var i=0;i<16;i++){var g=session&&session.gates?session.gates[i]:null;var samples=g?Number(g.samples)||0:0;var discarded=g?Number(g.discarded)||0:0;var bad=samples<target&&discarded>=target;if(samples<target)incomplete++;if(samples>=target||bad)completed++;if(bad)failed++;if(!bad&&samples<target)weakest=Math.min(weakest,samples);}"
            "if(completed>=16)weakest=target;return {completed:completed,failed:failed,incomplete:incomplete,weakest:weakest};"
        "}"
        "function showCalibrationModal(d,previousActive){"
            "var modal=el('radarCalProgressModal'),cancel=el('radarCalModalCancel'),apply=el('radarCalModalApply'),close=el('radarCalModalClose');if(!modal||!cancel||!apply||!close)return;"
            "var active=d.active||'none';"
            "if(active!=='none'){"
                "modalCompletionVisible=false;modal.hidden=false;cancel.hidden=false;apply.hidden=true;close.hidden=true;"
                "var session=active==='motion'?d.motion:d.quiet,label=active==='motion'?'Bewegungsmessung':'Ruhemessung';var target=Number(d.target_samples||100),maxReports=Number(d.max_reports||150);"
                "setText('radarCalModalTitle',label);setText('radarCalModalReports',String(Number(session&&session.samples||0))+' / '+String(maxReports));"
                "var remaining=Number(session&&session.countdown_ms||0);"
                "if(remaining>0){"
                    "setText('radarCalModalBig',String(Math.max(1,Math.ceil(remaining/1000))));setText('radarCalModalBigLabel','Sekunden bis Messbeginn');setText('radarCalModalHint',active==='quiet'?'Messbereich jetzt verlassen.':'Position einnehmen – die Bewegungsmessung startet danach automatisch.');"
                    "setText('radarCalModalCompleted','0 / 16');setText('radarCalModalWeakest','0 / '+String(target));var bar=el('radarCalModalProgressBar');if(bar)bar.style.width='0%';return;"
                "}"
                "var p=calibrationProgress(session,target);setText('radarCalModalBig',String(p.completed)+' / 16');setText('radarCalModalBigLabel','Gates abgeschlossen');setText('radarCalModalCompleted',String(p.completed)+' / 16');setText('radarCalModalWeakest',String(p.weakest)+' / '+String(target));"
                "setText('radarCalModalHint',p.failed?(String(p.failed)+' Gate'+(p.failed===1?' liefert':'s liefern')+' zu viele Fehlwerte und '+(p.failed===1?'wird':'werden')+' später abgeleitet.'):'Messung läuft automatisch bis genügend gültige Werte vorliegen.');"
                "var bar=el('radarCalModalProgressBar');if(bar)bar.style.width=String(Math.max(0,Math.min(100,p.completed/16*100)))+'%';return;"
            "}"
            "if(previousActive!=='none'){"
                "var session=previousActive==='motion'?d.motion:d.quiet,label=previousActive==='motion'?'Bewegungsmessung':'Ruhemessung';var target=Number(d.target_samples||100),maxReports=Number(d.max_reports||150),p=calibrationProgress(session,target);"
                "modalCompletionVisible=true;modal.hidden=false;cancel.hidden=true;close.hidden=false;setText('radarCalModalTitle',label);var derived=session&&session.quality_limited?p.incomplete:p.failed;"
                "if(session&&session.aborted){apply.hidden=true;close.textContent='Schließen';setText('radarCalModalBig','Abgebrochen');setText('radarCalModalBigLabel','Messung beendet');setText('radarCalModalHint','Die Teilmessung wird nicht für neue Empfehlungen verwendet.');setText('radarCalModalCompleted',String(p.completed)+' / 16');}"
                "else{stageSuggestedValues();apply.hidden=false;close.textContent='Nur Ergebnisse anzeigen';setText('radarCalModalBig','Fertig');setText('radarCalModalBigLabel','Messung abgeschlossen');setText('radarCalModalHint',(derived?(String(derived)+' Gate'+(derived===1?' wird':'s werden')+' aus benachbarten Messwerten abgeleitet. '):'')+'Die neuen Werte sind im Formular vorbereitet. Mit dem grünen Knopf werden sie auf den Radar geschrieben und verifiziert.');setText('radarCalModalCompleted','16 / 16');}"
                "setText('radarCalModalWeakest',String(p.weakest)+' / '+String(target));setText('radarCalModalReports',String(Number(session&&session.samples||0))+' / '+String(maxReports));var bar=el('radarCalModalProgressBar');if(bar)bar.style.width='100%';return;"
            "}"
            "if(!modalCompletionVisible)modal.hidden=true;"
        "}"
        "function stageSuggestedValues(){"
            "var n=0;for(var i=0;i<16;i++){if(suggestedTrigger[i]===null||suggestedHold[i]===null)continue;var triggerInput=el('radarTriggerInput'+i);var holdInput=el('radarHoldInput'+i);if(triggerInput&&holdInput){triggerInput.value=String(suggestedTrigger[i]);holdInput.value=String(suggestedHold[i]);n++;}}return n;"
        "}"
        "function applySuggestedValues(){"
            "var n=stageSuggestedValues();if(!n){alert('Keine verwendbaren Kalibrierungsvorschläge vorhanden.');return;}"
            "var form=el('radarConfigForm');var marker=el('radarCalibrationApply');if(!form||!marker){alert('Radar-Konfigurationsformular nicht gefunden. Seite bitte neu laden.');return;}"
            "if(!confirm(n+' vorgeschlagene Trigger-/Hold-Paare jetzt in den LD2410S schreiben und verifizieren?'))return;"
            "marker.value='1';var applyModal=el('radarCalModalApply');var applyPage=el('calApplyRecommendations');if(applyModal){applyModal.disabled=true;applyModal.textContent='Schreibe + verifiziere...';}if(applyPage){applyPage.disabled=true;applyPage.textContent='Schreibe + verifiziere...';}"
            "if(typeof form.requestSubmit==='function')form.requestSubmit();else form.submit();"
        "}"
        "function renderCalibrationControls(d){"
            "var active=d.active||'none',q=el('calStartQuiet'),m=el('calStartMotion');"
            "if(!q||!m)return;"
            "if(active==='quiet'){q.style.display='';m.style.display='none';q.textContent='Ruhemessung abbrechen';return;}"
            "if(active==='motion'){m.style.display='';q.style.display='none';m.textContent='Bewegungsmessung abbrechen';return;}"
            "q.style.display='';m.style.display='';q.textContent='Ruhemessung starten';m.textContent='Bewegungsmessung starten';"
        "}"
        "function render(d){"
            "var previousActive=lastData&&lastData.active?lastData.active:'none';lastData=d;"
            "var active=d.active||'none',mode=active!=='none'?active:lastDisplayMode;"
            "var session=mode==='motion'?d.motion:d.quiet;"
            "if(active!=='none')lastDisplayMode=active;"
            "var label=mode==='motion'?'Bewegungsmessung':'Ruhemessung';"
            "var state='keine Messung aktiv';"
            "if(active!=='none'){"
                "var remaining=Number(session&&session.countdown_ms||0);"
                "if(remaining>0)state=label+' startet in '+Math.max(1,Math.ceil(remaining/1000))+' s – Messbereich jetzt verlassen';"
                "else if(session&&!session.measurement_started)state=label+' startet …';"
                "else state=label+' läuft · Ziel '+String(d.target_samples||100)+' gültige Werte je Gate';"
            "}else if(session&&session.auto_completed){"
                "state=label+(session.quality_limited?' abgeschlossen · fehlerhafte Gates werden abgeleitet':' automatisch abgeschlossen');"
            "}else if(session&&session.aborted){state=label+' abgebrochen';}"
            "setText('radarCalState',state);"
            "setText('radarCalElapsed',session?timeText(session.elapsed_ms):'00:00');"
            "setText('radarCalSamples',session?(String(session.samples||0)+' / '+String(d.max_reports||150)):'0');"
            "renderSession('calQuiet',d.quiet,d.target_samples);renderSession('calMotion',d.motion,d.target_samples);renderComparison(d);renderLiveCalibration(d);renderCalibrationControls(d);showCalibrationModal(d,previousActive);"
        "}"
        "function poll(){"
            "if(busy)return;busy=true;"
            "fetch('/radar_calibration_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
            ".then(render).catch(function(){}).then(function(){busy=false;});"
        "}"
        "function action(name,confirmText){"
            "if(confirmText&&!confirm(confirmText))return;"
            "fetch('/radar_calibration_action',{method:'POST',cache:'no-store',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'action='+encodeURIComponent(name)})"
            ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
            ".then(render).catch(function(e){alert('Radar-Kalibrierung: '+e.message);});"
        "}"
        "var b;"
        "b=el('calStartQuiet');if(b)b.onclick=function(){lastDisplayMode='quiet';if(lastData&&lastData.active==='quiet'){action('stop');return;}if(lastData&&lastData.active!=='none')return;action('start_quiet','Neue Ruhemessung starten? Die bisherigen Ruhewerte werden ersetzt. Danach bleiben 10 Sekunden, um den Messbereich zu verlassen.');};"
        "b=el('calStartMotion');if(b)b.onclick=function(){lastDisplayMode='motion';if(lastData&&lastData.active==='motion'){action('stop');return;}if(lastData&&lastData.active!=='none')return;action('start_motion','Neue Bewegungsmessung starten? Die bisherigen Bewegungswerte werden ersetzt. Die Messung beginnt nach 10 Sekunden.');};"
        "b=el('radarCalModalCancel');if(b)b.onclick=function(){if(lastData&&lastData.active!=='none')action('stop');};"
        "b=el('radarCalModalApply');if(b)b.onclick=applySuggestedValues;"
        "b=el('radarCalModalClose');if(b)b.onclick=function(){modalCompletionVisible=false;var modal=el('radarCalProgressModal');if(modal)modal.hidden=true;};"
        "b=el('calApplyRecommendations');if(b)b.onclick=applySuggestedValues;"
        "poll();setInterval(poll,1000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});"
        "})();"
        "</script>";


    html +=
        "<form id='radarConfigForm' method='POST' action='/radar_config_save'>"
        "<input type='hidden' id='radarCalibrationApply' name='calibration_apply' value='0'>";

    html +=
        "<h3>Allgemeine Parameter</h3>";

    html +=
        "Min. distance gate: "
        "<input type='number' name='min_gate' min='0' max='16' value='" +
        String(settings.minGate) +
        "' style='width:80px;'> "
        "(ca. " +
        String(
            settings.minGate * 0.7f,
            1
        ) +
        " m)<br>";

    html +=
        "Max. distance gate: "
        "<input type='number' name='max_gate' min='1' max='16' value='" +
        String(settings.maxGate) +
        "' style='width:80px;'> "
        "(ca. " +
        String(
            settings.maxGate * 0.7f,
            1
        ) +
        " m)<br>";

    html +=
        "No-person delay: "
        "<input type='number' name='absence_sec' min='10' max='120' value='" +
        String(settings.absenceSec) +
        "' style='width:80px;'> s<br>";


    html +=
        "Status report rate: "
        "<select name='status_rate'>";
    appendRadarRateOptions(
        html,
        settings.statusRateX10
    );
    html +=
        "</select><br>";


    html +=
        "Distance report rate: "
        "<select name='distance_rate'>";
    appendRadarRateOptions(
        html,
        settings.distanceRateX10
    );
    html +=
        "</select><br>";


    html +=
        "Response speed: "
        "<select name='response_speed'>";

    html +=
        "<option value='5'" +
        String(
            settings.responseSpeed == 5
            ? " selected"
            : ""
        ) +
        ">normal</option>";

    html +=
        "<option value='10'" +
        String(
            settings.responseSpeed == 10
            ? " selected"
            : ""
        ) +
        ">fast</option>";

    html +=
        "</select><br>";


    html +=
        "<h3>Distance Gates / Thresholds</h3>";

    html +=
        "<p>Niedrigerer dB-Wert = empfindlicher. "
        "Die Hi-Link-Protokollbeispiele enthalten auch Werte unter 10; "
        "daher erlaubt diese Seite 0..95 und bewahrt vorhandene Werte. "
        "<b>Hinweis:</b> Hold=0 kann einen bereits erkannten Presence-Zustand sehr lange festhalten. "
        "Die Kalibrierung schlägt deshalb für Hold niemals 0 vor.</p>";

    html +=
        "<div style='overflow-x:auto'>"
        "<table style='border-collapse:collapse'>"
        "<tr>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>ca. Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hold dB</th>"
        "</tr>";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        html +=
            "<tr>"
            "<td style='padding:4px;text-align:center'>" +
            String(gate) +
            "</td>"
            "<td style='padding:4px;text-align:center'>" +
            String(
                gate * 0.7f,
                1
            ) +
            " m</td>"
            "<td style='padding:4px'>"
            "<input type='number' min='0' max='95' "
            "id='radarTriggerInput" +
            String(gate) +
            "' name='trigger_" +
            String(gate) +
            "' value='" +
            String(
                settings.triggerThreshold[gate]
            ) +
            "' style='width:70px'></td>"
            "<td style='padding:4px'>"
            "<input type='number' min='0' max='95' "
            "id='radarHoldInput" +
            String(gate) +
            "' name='hold_" +
            String(gate) +
            "' value='" +
            String(
                settings.holdThreshold[gate]
            ) +
            "' style='width:70px'></td>"
            "</tr>";
    }


    html +=
        "</table></div>";

    html +=
        "<br><button type='submit'>Write + Verify</button>";

    html +=
        "</form>";


    html +=
        "<div class='settings-section' style='margin-top:18px'>"
        "<h3>Standardwerte</h3>"
        "<p class='muted'>Falls die Radar-Parameter unbrauchbar verstellt wurden, können hier "
        "die Hi-Link-Standardwerte wiederhergestellt werden. Die Werte werden direkt in den "
        "LD2410S geschrieben und anschließend verifiziert.</p>"
        "<form method='POST' action='/radar_config_defaults' "
        "onsubmit=\"return confirm('Alle aktuellen Radar-Einstellungen werden mit den Hi-Link-Standardwerten überschrieben. Fortfahren?');\">"
        "<button type='submit' style='background:#fff1d6;color:#7a4700;border:1px solid #e3b96a'>"
        "Standardwerte wiederherstellen</button>"
        "</form>"
        "</div>";


    html +=
        "<br><a href='/radar_config'><button>Read again</button></a>";

    html +=
        "<a href='/'><button>Back</button></a>";

    html +=
        htmlFooter();


    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleRadarConfigSave()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    RadarWebMaintenanceGuard maintenanceGuard;

    bool recordingActive =
        recorderIsOpen();


    RadarSettings settings;

    settings.minGate =
        (uint32_t)(
            server.arg(
                "min_gate"
            ).toInt()
        );

    settings.maxGate =
        (uint32_t)(
            server.arg(
                "max_gate"
            ).toInt()
        );

    settings.absenceSec =
        (uint32_t)(
            server.arg(
                "absence_sec"
            ).toInt()
        );

    settings.statusRateX10 =
        (uint32_t)(
            server.arg(
                "status_rate"
            ).toInt()
        );

    settings.distanceRateX10 =
        (uint32_t)(
            server.arg(
                "distance_rate"
            ).toInt()
        );

    settings.responseSpeed =
        (uint32_t)(
            server.arg(
                "response_speed"
            ).toInt()
        );


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        settings.triggerThreshold[gate] =
            (uint32_t)(
                server.arg(
                    "trigger_" +
                    String(gate)
                ).toInt()
            );

        settings.holdThreshold[gate] =
            (uint32_t)(
                server.arg(
                    "hold_" +
                    String(gate)
                ).toInt()
            );
    }


    String error;

    bool writeOk =
        radarWriteSettings(
            settings,
            error
        );


    // The UART configuration transaction is synchronous and temporarily
    // interrupts normal standard reports. If recording was already active,
    // keep the ESP-side motion state alive after the transaction so the
    // normal post-record timer cannot expire merely because of WebConfig.
    if (recordingActive) {
        radarHoldMotion(
            2500UL
        );
    }


    if (!writeOk) {

        String html =
            htmlHeader();

        html +=
            "<h2>Radar Config Error</h2>"
            "<p style='color:#b00020'>" +
            htmlEscape(error) +
            "</p>"
            "<a href='/radar_config'><button>Back</button></a>";

        html +=
            htmlFooter();

        server.send(
            400,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    Serial.println(
        "LD2410S config written and verified"
    );

    logWrite(
        "LD2410S config written and verified"
    );


    bool calibrationApply =
        server.hasArg("calibration_apply") &&
        server.arg("calibration_apply") == "1";

    server.sendHeader(
        "Location",
        calibrationApply
        ? "/radar_config?saved=1&calibration=1"
        : "/radar_config?saved=1"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleRadarConfigDefaults()
{
    if (rejectRadarConfigurationUnavailable())
        return;

    RadarWebMaintenanceGuard maintenanceGuard;

    Serial.println(
        "LD2410S defaults restore | begin"
    );

    logWrite(
        "LD2410S defaults restore started"
    );

    bool recordingActive =
        recorderIsOpen();


    RadarSettings settings;

    radarGetHiLinkDefaultSettings(
        settings
    );


    String error;

    bool writeOk =
        radarWriteSettings(
            settings,
            error
        );


    // Match the normal Radar Config save path: if a recording was already
    // running, keep the ESP-side motion state alive across the short UART
    // configuration transaction.
    if (recordingActive) {
        radarHoldMotion(
            2500UL
        );
    }


    if (!writeOk) {

        Serial.println(
            "LD2410S defaults restore | failed | " +
            error
        );

        logWrite(
            "LD2410S defaults restore failed | " +
            error
        );

        String html =
            htmlHeader();

        html +=
            "<h2>Radar Standardwerte</h2>"
            "<div class='flash-notice error'>"
            "<strong>Wiederherstellung fehlgeschlagen</strong>"
            "<span class='muted'>" +
            htmlEscape(error) +
            "</span></div>"
            "<a href='/radar_config'><button>Zurück</button></a>";

        html +=
            htmlFooter();

        server.send(
            400,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    Serial.println(
        "LD2410S defaults restore | success | written and verified"
    );

    logWrite(
        "LD2410S defaults restore successful | written and verified"
    );


    server.sendHeader(
        "Location",
        "/radar_config?defaults=1"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// WIFI FIRMWARE UPDATE
// -------------------------------------------------------------
//
// Browser uploads are staged directly in the inactive internal OTA partition.
// The SD card is deliberately not part of the WiFi update path. This keeps
// remote recovery available when the SD card is absent, damaged or unstable.
//
// Safety model:
//   1. Upload writes only to the inactive OTA partition.
//   2. ESP image magic, partition capacity and SensorForge board marker are
//      checked while streaming.
//   3. esp_ota_end() validates the completed application image.
//   4. The running/boot partition is NOT changed by the upload.
//   5. Only the explicit INSTALL action calls esp_ota_set_boot_partition().
//
// A failed/interrupted upload therefore leaves the currently running firmware
// selected. The next upload simply overwrites the inactive OTA partition again.

static const uint8_t WEB_FW_ESP_IMAGE_MAGIC = 0xE9U;
static const size_t WEB_FW_MARKER_MAX_BYTES = 96U;

static bool webFirmwareUploadAttempted = false;
static bool webFirmwareUploadSucceeded = false;
static bool webFirmwareUploadLocksHeld = false;
static bool webFirmwarePreviousRecordingBlock = false;
static size_t webFirmwareUploadBytes = 0;
static size_t webFirmwareUploadCapacity = 0;
static String webFirmwareUploadFilename;
static String webFirmwareUploadError;

static bool webFirmwareOtaActive = false;
static esp_ota_handle_t webFirmwareOtaHandle = 0;
static const esp_partition_t *webFirmwareUploadPartition = nullptr;

// A successfully uploaded image remains staged only in RAM state until the
// operator either installs it or discards it. The image bytes themselves live
// in the inactive OTA partition, but that partition is not selected for boot.
static bool webFirmwareReady = false;
static const esp_partition_t *webFirmwareReadyPartition = nullptr;
static size_t webFirmwareReadyBytes = 0;
static String webFirmwareReadyFilename;

// Streaming SensorForge compatibility-marker matcher. The prefix table allows
// matches to span arbitrary HTTP upload chunk boundaries without buffering the
// complete firmware image in RAM.
static size_t webFirmwareMarkerLength = 0;
static size_t webFirmwareMarkerMatched = 0;
static size_t webFirmwareMarkerPrefix[WEB_FW_MARKER_MAX_BYTES] = {};
static bool webFirmwareMarkerFound = false;
static bool webFirmwareImageMagicChecked = false;


static String webFirmwareBaseName(
    const String &path
)
{
    int slash =
        path.lastIndexOf('/');

    return
        slash >= 0
        ? path.substring(slash + 1)
        : path;
}


static String webFirmwareFormatBytes(
    size_t bytes
)
{
    if (bytes >= 1024U * 1024U) {
        return
            String(
                (double)bytes /
                (1024.0 * 1024.0),
                2
            ) +
            " MB";
    }

    if (bytes >= 1024U) {
        return
            String(
                (double)bytes /
                1024.0,
                1
            ) +
            " KB";
    }

    return
        String((unsigned long)bytes) +
        " B";
}


static String webFirmwareEspError(
    esp_err_t result
)
{
    const char *name =
        esp_err_to_name(
            result
        );

    if (name && name[0] != '\0')
        return String(name);

    return
        String("ESP error ") +
        String((int)result);
}


static void webFirmwareReleaseUploadLocks()
{
    if (!webFirmwareUploadLocksHeld)
        return;

    g_recordingStartBlocked =
        webFirmwarePreviousRecordingBlock;

    webFirmwareUploadLocksHeld = false;
}


static void webFirmwareAbortActiveOta()
{
    if (webFirmwareOtaActive) {
        esp_ota_abort(
            webFirmwareOtaHandle
        );
    }

    webFirmwareOtaActive = false;
    webFirmwareOtaHandle = 0;
    webFirmwareUploadPartition = nullptr;
}


static void webFirmwareCleanupPartial()
{
    webFirmwareAbortActiveOta();
}


static void webFirmwareFailUpload(
    const String &error
)
{
    if (!webFirmwareUploadError.length())
        webFirmwareUploadError = error;

    webFirmwareUploadSucceeded = false;
}


static bool webFirmwarePrepareMarkerMatcher(
    String &error
)
{
    error = "";

    const char *marker =
        firmwareExpectedCompatibilityMarker();

    if (!marker || marker[0] == '\0') {
        error =
            "Firmware compatibility marker is unavailable.";
        return false;
    }

    webFirmwareMarkerLength =
        strlen(marker);

    if (
        webFirmwareMarkerLength == 0 ||
        webFirmwareMarkerLength > WEB_FW_MARKER_MAX_BYTES
    ) {
        error =
            "Firmware compatibility marker has an unsupported length.";
        return false;
    }

    memset(
        webFirmwareMarkerPrefix,
        0,
        sizeof(webFirmwareMarkerPrefix)
    );

    for (
        size_t i = 1, prefix = 0;
        i < webFirmwareMarkerLength;
        ++i
    ) {
        while (
            prefix > 0 &&
            marker[i] != marker[prefix]
        ) {
            prefix =
                webFirmwareMarkerPrefix[
                    prefix - 1
                ];
        }

        if (marker[i] == marker[prefix])
            ++prefix;

        webFirmwareMarkerPrefix[i] =
            prefix;
    }

    webFirmwareMarkerMatched = 0;
    webFirmwareMarkerFound = false;
    return true;
}


static void webFirmwareScanCompatibilityMarker(
    const uint8_t *data,
    size_t length
)
{
    if (
        webFirmwareMarkerFound ||
        !data ||
        length == 0 ||
        webFirmwareMarkerLength == 0
    ) {
        return;
    }

    const char *marker =
        firmwareExpectedCompatibilityMarker();

    if (!marker)
        return;

    for (size_t i = 0; i < length; ++i) {
        char current =
            (char)data[i];

        while (
            webFirmwareMarkerMatched > 0 &&
            current != marker[webFirmwareMarkerMatched]
        ) {
            webFirmwareMarkerMatched =
                webFirmwareMarkerPrefix[
                    webFirmwareMarkerMatched - 1
                ];
        }

        if (
            current ==
            marker[webFirmwareMarkerMatched]
        ) {
            ++webFirmwareMarkerMatched;
        }

        if (
            webFirmwareMarkerMatched ==
            webFirmwareMarkerLength
        ) {
            webFirmwareMarkerFound = true;
            webFirmwareMarkerMatched =
                webFirmwareMarkerPrefix[
                    webFirmwareMarkerMatched - 1
                ];
            return;
        }
    }
}


static void webFirmwareClearReadyState()
{
    webFirmwareReady = false;
    webFirmwareReadyPartition = nullptr;
    webFirmwareReadyBytes = 0;
    webFirmwareReadyFilename = "";
}


static bool webFirmwareReadyStateValid()
{
    if (
        !webFirmwareReady ||
        !webFirmwareReadyPartition ||
        webFirmwareReadyBytes == 0
    ) {
        return false;
    }

    const esp_partition_t *running =
        esp_ota_get_running_partition();

    return
        running &&
        webFirmwareReadyPartition != running &&
        webFirmwareReadyBytes <=
            webFirmwareReadyPartition->size;
}


static bool webFirmwareActionTokenValid()
{
    if (!server.hasArg("token"))
        return false;

    const String token =
        server.arg("token");

    if (!token.length())
        return false;

    char *endPtr = nullptr;
    unsigned long parsed =
        strtoul(
            token.c_str(),
            &endPtr,
            10
        );

    return
        endPtr &&
        *endPtr == '\0' &&
        (uint32_t)parsed ==
            webBootSessionId;
}


static void handleFirmwareUploadData()
{
    HTTPUpload &upload =
        server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        // Multipart callbacks can begin before the browser has had a chance to
        // send another keepalive. Re-establish the maintenance pause before
        // checking recorder state so firmware upload cannot race a new capture.
        maybeAutoPauseRecordingForWebUi();

        webFirmwareUploadAttempted = true;
        webFirmwareUploadSucceeded = false;
        webFirmwareUploadBytes = 0;
        webFirmwareUploadCapacity = 0;
        webFirmwareUploadFilename =
            webFirmwareBaseName(
                upload.filename
            );
        webFirmwareUploadError = "";
        webFirmwareImageMagicChecked = false;
        webFirmwareMarkerFound = false;
        webFirmwareMarkerMatched = 0;
        webFirmwareMarkerLength = 0;

        // Recover from a malformed/interrupted previous HTTP request before
        // accepting a new upload. A successfully staged image is retained and
        // must be installed or discarded explicitly.
        webFirmwareAbortActiveOta();
        webFirmwareReleaseUploadLocks();

        // Defense in depth: multipart upload callbacks may run while the
        // request body is being parsed. Never write unauthenticated firmware
        // bytes even if route middleware behavior changes in a future core.
        if (
            cfg_web_auth_enabled &&
            !server.authenticate(
                cfg_web_username.c_str(),
                cfg_web_password.c_str()
            )
        ) {
            webFirmwareFailUpload(
                "Authentication required for firmware upload."
            );
            return;
        }

        String lowerName =
            webFirmwareUploadFilename;
        lowerName.toLowerCase();

        if (!lowerName.endsWith(".bin")) {
            webFirmwareFailUpload(
                "Please select a compiled .bin firmware image."
            );
            return;
        }

        if (recorderIsOpen()) {
            webFirmwareFailUpload(
                "Recording active - firmware upload is temporarily unavailable."
            );
            return;
        }

        if (webFirmwareReady) {
            webFirmwareFailUpload(
                "A validated firmware image is already staged. Install or discard it first."
            );
            return;
        }

        const esp_partition_t *target =
            esp_ota_get_next_update_partition(
                nullptr
            );

        const esp_partition_t *running =
            esp_ota_get_running_partition();

        if (
            !target ||
            !running ||
            target == running ||
            target->size == 0
        ) {
            webFirmwareFailUpload(
                "No inactive OTA application partition is available."
            );
            return;
        }

        webFirmwareUploadCapacity =
            target->size;

        String markerError;

        if (!webFirmwarePrepareMarkerMatcher(
                markerError
            )) {
            webFirmwareFailUpload(
                markerError
            );
            return;
        }

        webFirmwarePreviousRecordingBlock =
            g_recordingStartBlocked;
        g_recordingStartBlocked = true;
        webFirmwareUploadLocksHeld = true;

        // Release a playback file and reduce unnecessary SD activity. The SD
        // itself is not required for this update path and is never locked here.
        webPlayerStop();

        esp_ota_handle_t otaHandle = 0;
        esp_err_t beginResult =
            esp_ota_begin(
                target,
                OTA_WITH_SEQUENTIAL_WRITES,
                &otaHandle
            );

        if (beginResult != ESP_OK) {
            webFirmwareFailUpload(
                "Could not prepare the internal OTA partition: " +
                webFirmwareEspError(
                    beginResult
                )
            );
            webFirmwareReleaseUploadLocks();
            return;
        }

        webFirmwareOtaHandle =
            otaHandle;
        webFirmwareOtaActive = true;
        webFirmwareUploadPartition =
            target;
        return;
    }

    if (upload.status == UPLOAD_FILE_WRITE) {
        if (
            webFirmwareUploadError.length() ||
            !webFirmwareOtaActive ||
            !webFirmwareUploadPartition
        ) {
            return;
        }

        if (upload.currentSize == 0)
            return;

        if (!webFirmwareImageMagicChecked) {
            if (
                webFirmwareUploadBytes != 0 ||
                upload.buf[0] !=
                    WEB_FW_ESP_IMAGE_MAGIC
            ) {
                webFirmwareFailUpload(
                    "Firmware rejected: invalid ESP32 application image header."
                );
                webFirmwareAbortActiveOta();
                return;
            }

            webFirmwareImageMagicChecked = true;
        }

        if (
            webFirmwareUploadBytes >
                webFirmwareUploadCapacity ||
            upload.currentSize >
                webFirmwareUploadCapacity -
                    webFirmwareUploadBytes
        ) {
            webFirmwareFailUpload(
                "Firmware rejected: image is larger than the inactive OTA partition."
            );
            webFirmwareAbortActiveOta();
            return;
        }

        webFirmwareScanCompatibilityMarker(
            upload.buf,
            upload.currentSize
        );

        esp_err_t writeResult =
            esp_ota_write(
                webFirmwareOtaHandle,
                upload.buf,
                upload.currentSize
            );

        if (writeResult != ESP_OK) {
            webFirmwareFailUpload(
                "Firmware upload failed while writing internal flash: " +
                webFirmwareEspError(
                    writeResult
                )
            );
            webFirmwareAbortActiveOta();
            return;
        }

        webFirmwareUploadBytes +=
            upload.currentSize;

        serviceWebLongOperation();
        return;
    }

    if (upload.status == UPLOAD_FILE_END) {
        if (webFirmwareUploadError.length()) {
            webFirmwareCleanupPartial();
            webFirmwareReleaseUploadLocks();
            return;
        }

        if (
            !webFirmwareOtaActive ||
            !webFirmwareUploadPartition ||
            webFirmwareUploadBytes == 0 ||
            !webFirmwareImageMagicChecked
        ) {
            webFirmwareFailUpload(
                "Firmware upload did not contain a complete application image."
            );
            webFirmwareCleanupPartial();
            webFirmwareReleaseUploadLocks();
            return;
        }

        if (
            upload.totalSize != 0 &&
            upload.totalSize !=
                webFirmwareUploadBytes
        ) {
            webFirmwareFailUpload(
                "Firmware upload size mismatch - upload may have been interrupted."
            );
            webFirmwareCleanupPartial();
            webFirmwareReleaseUploadLocks();
            return;
        }

        if (!webFirmwareMarkerFound) {
            webFirmwareFailUpload(
                "Firmware rejected: compatibility marker missing or wrong board."
            );
            webFirmwareCleanupPartial();
            webFirmwareReleaseUploadLocks();
            return;
        }

        const esp_partition_t *completedPartition =
            webFirmwareUploadPartition;

        esp_err_t endResult =
            esp_ota_end(
                webFirmwareOtaHandle
            );

        // esp_ota_end() consumes the OTA handle whether validation succeeds or
        // fails. Do not call esp_ota_abort() on this handle afterwards.
        webFirmwareOtaActive = false;
        webFirmwareOtaHandle = 0;
        webFirmwareUploadPartition = nullptr;

        if (endResult != ESP_OK) {
            webFirmwareFailUpload(
                "Firmware rejected during final OTA image verification: " +
                webFirmwareEspError(
                    endResult
                )
            );
            webFirmwareReleaseUploadLocks();
            return;
        }

        webFirmwareReady = true;
        webFirmwareReadyPartition =
            completedPartition;
        webFirmwareReadyBytes =
            webFirmwareUploadBytes;
        webFirmwareReadyFilename =
            webFirmwareUploadFilename;
        webFirmwareUploadSucceeded = true;

        webFirmwareReleaseUploadLocks();
        return;
    }

    if (upload.status == UPLOAD_FILE_ABORTED) {
        webFirmwareFailUpload(
            "Firmware upload was interrupted or aborted."
        );
        webFirmwareCleanupPartial();
        webFirmwareReleaseUploadLocks();
    }
}


static void handleFirmwareUploadFinished()
{
    // Defensive cleanup: malformed/interrupted multipart requests must never
    // leave an OTA handle or the recording-start gate behind.
    if (!webFirmwareUploadSucceeded)
        webFirmwareCleanupPartial();

    webFirmwareReleaseUploadLocks();

    if (!webFirmwareUploadAttempted) {
        webFirmwareUploadSucceeded = false;
        webFirmwareUploadError =
            "No firmware file was received.";
    }

    server.sendHeader(
        "Location",
        webFirmwareUploadSucceeded
        ? "/firmware_update?notice=upload_ok"
        : "/firmware_update?notice=upload_failed"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void sendFirmwareUpdatePage(
    int statusCode,
    const String &forcedError = ""
)
{
    bool blockedByRecording =
        recorderIsOpen();

    bool readyExists =
        webFirmwareReady;

    bool readyValid =
        webFirmwareReadyStateValid();

    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Firmware Update</h2>"
        "<p>Neue SensorForge-Firmware sicher über WiFi bereitstellen</p>"
        "</div></div>";

    html +=
        "<div class='settings-section'>"
        "<h3>Aktuelle Firmware</h3>"
        "<p><b>Build:</b> " +
        htmlEscape(
            firmwareBuildTimestamp()
        ) +
        "<br><b>Installiert:</b> " +
        htmlEscape(
            firmwareInstallTimestamp()
        ) +
        "<br><b>Quelle:</b> " +
        htmlEscape(
            firmwareInstallSource()
        ) +
        "</p></div>";

    String notice =
        server.arg("notice");

    String displayError =
        forcedError.length()
        ? forcedError
        : webFirmwareUploadError;

    if (notice == "upload_ok") {
        html +=
            "<div class='flash-notice'>"
            "<strong>Firmware erfolgreich hochgeladen und geprüft</strong>"
            "<span class='muted'>Die neue Firmware liegt sicher im internen Update-Speicher. "
            "Die aktuell laufende Firmware wurde noch nicht umgeschaltet.</span>"
            "</div>";

    } else if (
        notice == "upload_failed" ||
        forcedError.length()
    ) {
        html +=
            "<div class='flash-notice error'>"
            "<strong>Firmware-Upload nicht freigegeben</strong>"
            "<span class='muted'>" +
            htmlEscape(
                displayError.length()
                ? displayError
                : String("Unbekannter Upload-Fehler")
            ) +
            "</span></div>";
    }

    if (blockedByRecording) {
        html +=
            "<div class='flash-notice error'>"
            "<strong>Aufnahme läuft</strong>"
            "<span class='muted'>Firmware-Upload und Installation sind bis zum sauberen Ende der laufenden Aufnahme gesperrt.</span>"
            "</div>";
    }

    html +=
        "<div class='settings-section'>"
        "<h3>1. Firmware auswählen</h3>"
        "<p class='muted'>Wähle die kompilierte <code>.bin</code>-Datei. SensorForge prüft automatisch, "
        "ob das Image vollständig ist, in den internen Update-Speicher passt und zu diesem Gerät gehört. "
        "Die SD-Karte wird für ein WiFi-Update nicht benötigt.</p>";

    if (
        !blockedByRecording &&
        !readyExists
    ) {
        html +=
            "<form id='firmwareUploadForm' method='POST' action='/firmware_upload' enctype='multipart/form-data' "
            "onsubmit=\"var b=document.getElementById('fwUploadButton');if(b){b.disabled=true;b.textContent='Upload läuft ...';}" 
            "var s=document.getElementById('fwUploadState');if(s)s.hidden=false;\">"
            "<input type='file' name='firmware' accept='.bin,application/octet-stream' required>"
            "<br><button id='fwUploadButton' class='primary' type='submit'>UPLOAD &amp; PRÜFEN</button>"
            "<span id='fwUploadState' class='muted' hidden> Bitte Verbindung und Stromversorgung nicht unterbrechen.</span>"
            "</form>";

    } else if (readyExists) {
        html +=
            "<p><span class='status-pill ok'>UPLOAD BEREITS BEREIT</span></p>";
    }

    html +=
        "</div>";

    if (readyExists) {
        html +=
            "<div class='settings-section'>"
            "<h3>2. Prüfung &amp; Installation</h3>";

        if (readyValid) {
            html +=
                "<p><span class='status-pill ok'>IMAGE GÜLTIG</span></p>"
                "<p>✓ ESP32 Application Image<br>"
                "✓ Größe: <b>" +
                webFirmwareFormatBytes(
                    webFirmwareReadyBytes
                ) +
                "</b><br>"
                "✓ Interner Update-Speicher: <b>" +
                webFirmwareFormatBytes(
                    webFirmwareReadyPartition->size
                ) +
                "</b><br>"
                "✓ Firmware ist mit diesem Gerät kompatibel</p>"
                "<p class='muted'>Mit <b>JETZT INSTALLIEREN</b> wird die bereits geprüfte Firmware "
                "für den nächsten Neustart aktiviert. Bis dahin bleibt die aktuell laufende Firmware unverändert.</p>";

            if (!blockedByRecording) {
                html +=
                    "<form method='POST' action='/firmware_install' "
                    "onsubmit=\"return confirm('Firmware wirklich installieren? Das Gerät startet danach automatisch neu.');\">"
                    "<input type='hidden' name='token' value='" +
                    String(webBootSessionId) +
                    "'>"
                    "<button class='danger' type='submit'>JETZT INSTALLIEREN</button>"
                    "</form>";
            }

        } else {
            html +=
                "<div class='flash-notice error'>"
                "<strong>Bereitgestellte Firmware ist nicht mehr verfügbar</strong>"
                "<span class='muted'>Bitte die Firmware erneut hochladen und prüfen.</span></div>";
        }

        if (!blockedByRecording) {
            html +=
                "<form method='POST' action='/firmware_discard' "
                "onsubmit=\"return confirm('Bereitgestellte Firmware verwerfen?');\">"
                "<input type='hidden' name='token' value='" +
                String(webBootSessionId) +
                "'>"
                "<button type='submit'>Bereitgestellte Firmware verwerfen</button>"
                "</form>";
        }

        html +=
            "</div>";
    }

    html +=
        "<div class='settings-section'>"
        "<h3>Sicherheitsablauf</h3>"
        "<p class='muted'>WiFi-Upload → inaktive interne OTA-Partition → vollständige Image- und Geräteprüfung → "
        "manuelle Bestätigung → neue Boot-Partition freigeben → kontrollierter Neustart.</p>"
        "<p class='muted'>Bricht der Upload vorher ab, bleibt die bisherige Firmware als Boot-Firmware ausgewählt. "
        "Auch eine fehlende oder defekte SD-Karte verhindert das WiFi-Update nicht.</p>"
        "<p><b>Wichtig:</b> Ein technisch gültiges, aber fehlerhaft programmiertes Image kann nach dem Update trotzdem den WiFi-Zugang verlieren. "
        "Remote daher nur Builds installieren, die vorher auf einem passenden zweiten Gerät getestet wurden.</p>"
        "</div>";

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        statusCode,
        "text/html; charset=utf-8",
        html
    );

    // Clear only the one-shot upload result. A successfully staged image has
    // separate RAM state and remains available until install/discard/reboot.
    if (
        notice == "upload_ok" ||
        notice == "upload_failed"
    ) {
        webFirmwareUploadAttempted = false;
        webFirmwareUploadSucceeded = false;
        webFirmwareUploadError = "";
        webFirmwareUploadFilename = "";
        webFirmwareUploadBytes = 0;
    }
}


static void handleFirmwareUpdate()
{
    // Entering the firmware page is maintenance activity. Establish the
    // configured automatic pause immediately instead of waiting for the first
    // browser-side heartbeat after the page has rendered.
    maybeAutoPauseRecordingForWebUi();

    sendFirmwareUpdatePage(
        200
    );
}


static void handleFirmwareDiscard()
{
    maybeAutoPauseRecordingForWebUi();

    if (!webFirmwareActionTokenValid()) {
        server.send(
            403,
            "text/plain; charset=utf-8",
            "Invalid firmware action token"
        );
        return;
    }

    if (recorderIsOpen()) {
        sendFirmwareUpdatePage(
            409,
            "Recording active - staged firmware cannot be changed."
        );
        return;
    }

    webFirmwareClearReadyState();

    server.sendHeader(
        "Location",
        "/firmware_update"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleFirmwareInstall()
{
    maybeAutoPauseRecordingForWebUi();

    if (!webFirmwareActionTokenValid()) {
        server.send(
            403,
            "text/plain; charset=utf-8",
            "Invalid firmware action token"
        );
        return;
    }

    if (recorderIsOpen()) {
        sendFirmwareUpdatePage(
            409,
            "Recording active - firmware installation is temporarily unavailable."
        );
        return;
    }

    if (!webFirmwareReadyStateValid()) {
        webFirmwareClearReadyState();
        sendFirmwareUpdatePage(
            404,
            "No validated WiFi firmware image is staged."
        );
        return;
    }

    bool previousRecordingBlock =
        g_recordingStartBlocked;

    g_recordingStartBlocked = true;
    webPlayerStop();

    // The first boot of the newly selected WiFi image must not immediately be
    // replaced by a stale .bin file that happens to be present on SD. The
    // one-shot NVS flag is consumed at the next firmware boot before the SD
    // auto-updater is considered.
    if (!firmwareInfoArmDirectOtaBoot()) {
        g_recordingStartBlocked =
            previousRecordingBlock;

        sendFirmwareUpdatePage(
            500,
            "Could not persist the safe first-boot guard for the firmware update."
        );
        return;
    }

    esp_err_t bootResult =
        esp_ota_set_boot_partition(
            webFirmwareReadyPartition
        );

    if (bootResult != ESP_OK) {
        firmwareInfoCancelDirectOtaBoot();
        g_recordingStartBlocked =
            previousRecordingBlock;

        sendFirmwareUpdatePage(
            500,
            "Could not activate the validated firmware image: " +
            webFirmwareEspError(
                bootResult
            )
        );
        return;
    }

    if (!firmwareInfoMarkWifiUpdate(
            webFirmwareReadyFilename
        )) {
        consoleWrite(
            "FW",
            "WiFi OTA selected, but installation metadata could not be persisted"
        );
    }

    logWrite(
        "Firmware WiFi OTA armed | file=" +
        webFirmwareReadyFilename +
        " | bytes=" +
        String((unsigned long)webFirmwareReadyBytes)
    );
    logFlush();

    webFirmwareClearReadyState();

    rebootScheduled = true;
    rebootAtMs =
        millis() + 3000UL;

    server.sendHeader(
        "Location",
        "/rebooting?reason=firmware_update"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}




// -------------------------------------------------------------
// REBOOT
// -------------------------------------------------------------

static String rebootRecordingBlockedModalHtml()
{
    return
        "<div id='rebootBlockedModal' class='modal-backdrop'>"
        "<div class='modal-card' role='dialog' aria-modal='true' "
        "aria-labelledby='rebootBlockedTitle'>"
        "<span class='status-pill danger'>AUFNAHME AKTIV</span>"
        "<h3 id='rebootBlockedTitle'>Neustart momentan gesperrt</h3>"
        "<p>Eine Aufnahme läuft gerade. Zum Schutz der laufenden Videodatei "
        "und der SD-Karte ist ein Neustart vorübergehend deaktiviert.</p>"
        "<p>Dieses Fenster verschwindet automatisch, sobald die Aufnahme beendet ist.</p>"
        "<div class='modal-actions'>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "</div>"
        "<div id='rebootBlockedState' class='modal-state'>Warte auf Aufnahmeende ...</div>"
        "</div></div>"
        "<script>"
        "(function(){"
            "var modal=document.getElementById('rebootBlockedModal');"
            "var state=document.getElementById('rebootBlockedState');"
            "function poll(){"
                "fetch('/ui_status?ts='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
                ".then(function(s){"
                    "if(!s.recorder_open){"
                        "if(state)state.textContent='Aufnahme beendet - Neustart wieder verfügbar.';"
                        "setTimeout(function(){if(modal)modal.hidden=true;},450);"
                        "return;"
                    "}"
                    "setTimeout(poll,1000);"
                "})"
                ".catch(function(){setTimeout(poll,1500);});"
            "}"
            "setTimeout(poll,800);"
        "})();"
        "</script>";
}


static void sendRebootPage(
    int statusCode
)
{
    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Reboot</h2>"
        "<p>SensorForge kontrolliert neu starten</p>"
        "</div></div>"
        "<div class='settings-section'>"
        "<h3>System neu starten</h3>"
        "<p class='muted'>Ein Neustart beendet die aktuelle Sitzung und startet das Board anschließend neu.</p>"
        "<form method='POST' action='/reboot_do'>"
        "<button class='danger' type='submit'>Jetzt neu starten</button>"
        "<a class='button' href='/'>Abbrechen</a>"
        "</form>"
        "</div>";

    if (recorderIsOpen()) {
        html +=
            rebootRecordingBlockedModalHtml();
    }

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        statusCode,
        "text/html; charset=utf-8",
        html
    );
}


static void handleReboot()
{
    sendRebootPage(
        200
    );
}


static void handleRebooting()
{
    String html =
        htmlHeader();

    String reason =
        server.arg("reason");

    bool afterSdFormat =
        reason == "sd_format";

    bool afterSecureErase =
        reason == "sd_secure";

    bool afterSecureAbort =
        reason == "sd_secure_abort";

    bool afterFirmwareUpdate =
        reason == "firmware_update";

    html +=
        "<div class='operation-card'>"
        "<span class='status-pill warn'>SYSTEM RESTART</span>";

    if (afterSdFormat) {
        html +=
            "<h2 style='margin-top:16px'>SD Format abgeschlossen</h2>"
            "<p class='muted'>Das Dateisystem wurde neu aufgebaut und die vorherige Config-Policy beibehalten. "
            "SensorForge startet jetzt automatisch neu, damit alle Storage-Komponenten mit einem "
            "frischen SD-Mount weiterarbeiten.</p>";

    } else if (afterSecureErase) {
        html +=
            "<h2 style='margin-top:16px'>Secure Erase abgeschlossen</h2>"
            "<p class='muted'>Secure Erase und Neuformatierung sind abgeschlossen; die vorherige Config-Policy wurde beibehalten. "
            "SensorForge startet jetzt automatisch neu, damit alle Storage-Komponenten mit einem "
            "frischen SD-Mount weiterarbeiten.</p>";

    } else if (afterSecureAbort) {
        html +=
            "<h2 style='margin-top:16px'>Secure Erase abgebrochen</h2>"
            "<p class='muted'>Das weitere Überschreiben wurde auf Wunsch beendet. Die SD-Karte wurde anschließend "
            "neu formatiert und die vorherige Config-Policy beibehalten. SensorForge startet jetzt automatisch neu.</p>";

    } else if (afterFirmwareUpdate) {
        html +=
            "<h2 style='margin-top:16px'>Firmware wird installiert</h2>"
            "<p class='muted'>Das validierte WiFi-Image wurde für den bestehenden SD-Auto-Updater freigegeben. "
            "SensorForge startet jetzt neu, prüft das Image beim Boot erneut und schreibt es anschließend in die inaktive OTA-Partition.</p>";

    } else {
        html +=
            "<h2 style='margin-top:16px'>SensorForge wird neu gestartet</h2>"
            "<p class='muted'>Die Verbindung zum Fabric Node wird kurz unterbrochen. "
            "Danach öffnet sich automatisch wieder die Übersicht.</p>";
    }

    html +=
        "<div id='rebootCountdown' class='countdown'>10</div>"
        "<div class='muted'>Sekunden bis zur Rückkehr</div>"
        "</div>"
        "<script>"
        // Make a manual refresh safe immediately. The current document stays
        // visible, but the browser URL/history already points to the dashboard.
        "history.replaceState(null,'','/');"
        "(function(){"
            "var seconds=10;"
            "var el=document.getElementById('rebootCountdown');"
            "var timer=setInterval(function(){"
                "seconds--;"
                "if(seconds<0)seconds=0;"
                "if(el)el.textContent=seconds;"
                "if(seconds===0){clearInterval(timer);tryHome();}"
            "},1000);"
            "function tryHome(){"
                "fetch('/?reconnect='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);location.replace('/');})"
                ".catch(function(){setTimeout(tryHome,1000);});"
            "}"
        "})();"
        "</script>";

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleRebootDo()
{
    // Keep the server-side guard even though /reboot already shows the
    // recording modal. The recorder may start in the short interval between
    // opening the page and pressing the reboot button.
    if (recorderIsOpen()) {
        sendRebootPage(
            409
        );
        return;
    }

    // POST/Redirect/GET prevents browser refresh from repeating the reboot POST.
    // Three seconds are intentionally left before restart so the browser can
    // fetch and render /rebooting first, even on a slightly slow WiFi link.
    rebootScheduled =
        true;

    rebootAtMs =
        millis() + 3000UL;

    server.sendHeader(
        "Location",
        "/rebooting"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// SHUTDOWN
// -------------------------------------------------------------

static String shutdownRecordingBlockedModalHtml()
{
    bool de =
        cfg_web_language == "de";

    return
        "<div id='shutdownBlockedModal' class='modal-backdrop'>"
        "<div class='modal-card' role='dialog' aria-modal='true' "
        "aria-labelledby='shutdownBlockedTitle'>"
        "<span class='status-pill danger'>" +
        String(de ? "AUFNAHME AKTIV" : "RECORDING ACTIVE") +
        "</span>"
        "<h3 id='shutdownBlockedTitle'>" +
        String(de ? "Herunterfahren momentan gesperrt" : "Shutdown currently blocked") +
        "</h3><p>" +
        String(
            de
            ? "Eine Aufnahme läuft gerade. Zum Schutz der Videodatei und der SD-Karte ist das Herunterfahren vorübergehend deaktiviert."
            : "A recording is currently running. Shutdown is temporarily disabled to protect the video file and SD card."
        ) +
        "</p><p>" +
        String(
            de
            ? "Dieses Fenster verschwindet automatisch, sobald die Aufnahme beendet ist."
            : "This window closes automatically when the recording has finished."
        ) +
        "</p><div class='modal-actions'><a class='button' href='/'>" +
        String(de ? "Zur Übersicht" : "Back to overview") +
        "</a></div>"
        "<div id='shutdownBlockedState' class='modal-state'>" +
        String(de ? "Warte auf Aufnahmeende ..." : "Waiting for recording to finish ...") +
        "</div></div></div>"
        "<script>"
        "(function(){"
            "var modal=document.getElementById('shutdownBlockedModal');"
            "var state=document.getElementById('shutdownBlockedState');"
            "function poll(){"
                "fetch('/ui_status?ts='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
                ".then(function(s){"
                    "if(!s.recorder_open){"
                        "if(state)state.textContent='" +
                        String(de ? "Aufnahme beendet - Herunterfahren wieder verfügbar." : "Recording finished - shutdown available again.") +
                        "';setTimeout(function(){if(modal)modal.hidden=true;},450);return;"
                    "}setTimeout(poll,1000);"
                "}).catch(function(){setTimeout(poll,1500);});"
            "}setTimeout(poll,800);"
        "})();"
        "</script>";
}


static void sendShutdownPage(
    int statusCode
)
{
    bool de =
        cfg_web_language == "de";

    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div><h2>" +
        String(de ? "Herunterfahren" : "Shutdown") +
        "</h2><p>" +
        String(de ? "SensorForge in den Aus-Zustand versetzen" : "Put SensorForge into its off state") +
        "</p></div></div>"
        "<div class='settings-section'><h3>" +
        String(de ? "System herunterfahren" : "Shut down system") +
        "</h3><p class='muted'>" +
        String(
            de
            ? "SensorForge beendet die Web-Sitzung, deaktiviert alle Wakequellen und geht in Deep Sleep. Radar, Magnetkontakt und der Snapshot-Timer können das Board danach nicht mehr aufwecken."
            : "SensorForge ends the web session, disables all wake sources and enters deep sleep. Radar, magnet switch and the snapshot timer cannot wake the board afterwards."
        ) +
        "</p><p class='muted'><b>" +
        String(de ? "Wieder einschalten:" : "Power on again:") +
        "</b> " +
        String(
            de
            ? "Stromversorgung trennen und wieder anlegen oder den Hardware-RESET betätigen."
            : "Remove and reapply power, or press the hardware RESET button."
        ) +
        "</p><p class='muted'>" +
        String(
            de
            ? "Hinweis: Das ist der tiefste Software-Aus-Zustand des ESP32-S3. Direkt versorgte Peripherie kann weiterhin einen kleinen Strom aufnehmen."
            : "Note: this is the deepest software-off state of the ESP32-S3. Directly powered peripherals may still consume a small amount of current."
        ) +
        "</p>"
        "<form method='POST' action='/shutdown_do' onsubmit=\"return confirm('" +
        String(
            de
            ? "SensorForge wirklich herunterfahren? Danach ist das Webinterface nicht mehr erreichbar, bis die Stromversorgung neu angelegt oder RESET gedrückt wurde."
            : "Really shut down SensorForge? The web interface will remain unavailable until power is reapplied or RESET is pressed."
        ) +
        "');\">"
        "<button class='danger' type='submit'>" +
        String(de ? "Jetzt herunterfahren" : "Shut down now") +
        "</button><a class='button' href='/'>" +
        String(de ? "Abbrechen" : "Cancel") +
        "</a></form></div>";

    if (recorderIsOpen())
        html += shutdownRecordingBlockedModalHtml();

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        statusCode,
        "text/html; charset=utf-8",
        html
    );
}


static void handleShutdown()
{
    sendShutdownPage(
        200
    );
}


static void handleShuttingDown()
{
    bool de =
        cfg_web_language == "de";

    String html =
        htmlHeader();

    html +=
        "<div class='operation-card'>"
        "<span class='status-pill danger'>SHUTDOWN</span>"
        "<h2 style='margin-top:16px'>" +
        String(de ? "SensorForge wird heruntergefahren" : "SensorForge is shutting down") +
        "</h2><p class='muted'>" +
        String(
            de
            ? "Die Verbindung wird gleich beendet. Danach bleibt das Board ausgeschaltet, bis die Stromversorgung neu angelegt oder RESET gedrückt wird."
            : "The connection will close shortly. The board then remains off until power is reapplied or RESET is pressed."
        ) +
        "</p><div class='countdown'>OFF</div></div>";

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleShutdownDo()
{
    // Keep a server-side guard in case a recording starts between opening the
    // page and pressing the shutdown button.
    if (recorderIsOpen()) {
        sendShutdownPage(
            409
        );
        return;
    }

    // Prevent a new recording from starting during the short HTTP grace period
    // before the actual power-down sequence.
    g_recordingStartBlocked = true;

    shutdownScheduled = true;
    shutdownAtMs =
        millis() + 2000UL;

    server.sendHeader(
        "Location",
        "/shutting_down"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void performManualShutdown()
{
    shutdownScheduled = false;
    rebootScheduled = false;
    g_recordingStartBlocked = true;

    if (recorderIsOpen())
        stopRecording();

    consoleWrite(
        "POWER",
        "Manual shutdown | no wake sources"
    );
    logWrite(
        "Manual shutdown | deep sleep without wake sources"
    );
    logFlush();

    // webConfigStop() also restores a temporary LD2410S all-gate calibration
    // range before the UART/WebConfig session disappears.
    webConfigStop();

    stopCameraPreview();
    esp_camera_deinit();

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(
        true,
        false
    );
    WiFi.mode(WIFI_OFF);

    delay(100);

    // Deliberately remove every normal wake source. Unlike ordinary light/deep
    // sleep, no presence, magnet or timer wake is armed for manual shutdown.
    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    Serial.flush();
    esp_deep_sleep_start();
}


// -------------------------------------------------------------
// WEB LOG READER
// -------------------------------------------------------------
// V22: Log Viewer / live log / download / clear are owned by the external
// log-reader module. The common navigation, authentication middleware and
// shared WebConfig HTML/CSS remain here. Routes are registered in
// webConfigStart() via webLogReaderRegisterRoutes(server).


// -------------------------------------------------------------
// RECORDING BROWSER
//
// Important:
// /files only scans the SD root and therefore stays fast.
// A day's recordings are loaded only when that day is opened.
// -------------------------------------------------------------

struct RecordingEntry {
    String name;
    String fullPath;
    String annotation;
    uint64_t size;
    uint64_t durationMs;
    bool durationValid;
    bool isMkv;
    bool isJpeg;
    bool hasSrt;
    bool hasAudio;
    bool corrupt;
};


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


static String displayDateFolder(const String &folderName)
{
    if (!isDateFolderName(folderName))
        return folderName;

    return
        folderName.substring(6, 8) + "." +
        folderName.substring(4, 6) + "." +
        folderName.substring(0, 4);
}


static String displayRecordingTime(const String &fileName)
{
    int dotPos =
        fileName.lastIndexOf('.');

    String baseName =
        dotPos >= 0
        ? fileName.substring(0, dotPos)
        : fileName;

    // Normal recordings use HHMMSS.ext. Continuous-shooter files append
    // a sub-second/role suffix, e.g. HHMMSS_500_shooter.mkv. For list
    // display both formats share the same HH:MM:SS start time.
    if (
        baseName.length() >= 6 &&
        (baseName.length() == 6 || baseName[6] == '_')
    ) {

        bool numeric = true;

        for (size_t i = 0; i < 6; ++i) {
            if (!isDigit(baseName[i])) {
                numeric = false;
                break;
            }
        }

        if (numeric) {
            return
                baseName.substring(0, 2) + ":" +
                baseName.substring(2, 4) + ":" +
                baseName.substring(4, 6);
        }
    }

    return baseName;
}


static bool recordingStartSecondOfDay(
    const String &fileName,
    uint32_t &secondOfDay
)
{
    int dotPos =
        fileName.lastIndexOf('.');

    String baseName =
        dotPos >= 0
        ? fileName.substring(0, dotPos)
        : fileName;

    // Accept both normal HHMMSS names and shooter names such as
    // HHMMSS_500_shooter. The first six digits remain the wall-clock start.
    if (
        baseName.length() < 6 ||
        (baseName.length() > 6 && baseName[6] != '_')
    ) {
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        if (!isDigit(baseName[i]))
            return false;
    }

    uint8_t hour =
        (uint8_t)baseName.substring(0, 2).toInt();

    uint8_t minute =
        (uint8_t)baseName.substring(2, 4).toInt();

    uint8_t second =
        (uint8_t)baseName.substring(4, 6).toInt();

    if (
        hour > 23 ||
        minute > 59 ||
        second > 59
    ) {
        return false;
    }

    secondOfDay =
        (uint32_t)hour * 3600UL +
        (uint32_t)minute * 60UL +
        (uint32_t)second;

    return true;
}


static String formatSecondOfDay(uint32_t secondOfDay)
{
    secondOfDay %=
        24UL * 60UL * 60UL;

    uint8_t hour =
        (uint8_t)(secondOfDay / 3600UL);

    uint8_t minute =
        (uint8_t)((secondOfDay / 60UL) % 60UL);

    uint8_t second =
        (uint8_t)(secondOfDay % 60UL);

    char buffer[9];

    snprintf(
        buffer,
        sizeof(buffer),
        "%02u:%02u:%02u",
        (unsigned)hour,
        (unsigned)minute,
        (unsigned)second
    );

    return String(buffer);
}


static String displayRecordingTimeRange(
    const RecordingEntry &entry
)
{
    uint32_t startSecond = 0;

    if (!recordingStartSecondOfDay(
            entry.name,
            startSecond
        )) {
        return displayRecordingTime(
            entry.name
        );
    }

    String startText =
        formatSecondOfDay(
            startSecond
        );

    if (
        entry.isJpeg ||
        !entry.durationValid
    ) {
        return startText;
    }

    uint64_t roundedDurationSeconds =
        (entry.durationMs + 500ULL) /
        1000ULL;

    uint32_t endSecond =
        (uint32_t)(
            (
                (uint64_t)startSecond +
                roundedDurationSeconds
            ) %
            (24ULL * 60ULL * 60ULL)
        );

    return
        startText +
        " - " +
        formatSecondOfDay(
            endSecond
        );
}


static const size_t RECORDING_LIST_ANNOTATION_MAX_BYTES = 240U;


static bool loadRecordingAnnotationForList(
    const String &mediaPath,
    String &text
)
{
    text =
        "";

    String notePath =
        mediaPath +
        ".note";

    RecordingStorageFile file;

    if (!file.openRead(notePath)) {
        // Missing annotation is a normal state. An existing but unreadable note
        // is treated as a note-read error, not as media corruption.
        return
            !STORAGE.exists(
                notePath.c_str()
            );
    }

    if (file.isDirectory()) {
        file.close();
        return false;
    }

    while (
        file.available() &&
        text.length() < RECORDING_LIST_ANNOTATION_MAX_BYTES
    ) {
        int c =
            file.read();

        if (c < 0)
            break;

        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';

        text +=
            (char)c;
    }

    file.close();

    text.trim();

    while (text.indexOf("  ") >= 0)
        text.replace("  ", " ");

    if (text.length() > RECORDING_LIST_ANNOTATION_MAX_BYTES)
        text.remove(RECORDING_LIST_ANNOTATION_MAX_BYTES);

    return true;
}


static String formatFileSize(uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024ULL) {

        float mb =
            (float)bytes /
            (1024.0f * 1024.0f);

        return String(mb, 1) + " MB";
    }

    if (bytes >= 1024ULL) {

        float kb =
            (float)bytes /
            1024.0f;

        return String(kb, 0) + " KB";
    }

    return
        String((unsigned long)bytes) +
        " B";
}


static bool recordingNameDescending(
    const RecordingEntry &a,
    const RecordingEntry &b
)
{
    return
        a.name.compareTo(b.name) > 0;
}


static bool stringDescending(
    const String &a,
    const String &b
)
{
    return
        a.compareTo(b) > 0;
}


static bool hasMatchingSrt(
    const String &videoName,
    const std::vector<String> &srtBaseNames
)
{
    if (videoName.length() < 5)
        return false;

    String base =
        videoName.substring(
            0,
            videoName.length() - 4
        );

    base.toLowerCase();

    for (
        const String &srtBase :
        srtBaseNames
    ) {
        if (base == srtBase)
            return true;
    }

    return false;
}


// -------------------------------------------------------------
// Return one day's recordings as an HTML fragment.
//
// This handler intentionally streams rows instead of building
// one huge HTML String in RAM.
// -------------------------------------------------------------

static void handleFilesDay()
{
    if (recorderIsOpen()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "recording_active"
        );
        return;
    }

    String day =
        server.arg("day");

    bool validDay =
        isDateFolderName(day) ||
        day == "fallback";

    if (!validDay) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    std::vector<RecordingEntry> recordings;
    std::vector<String> srtBaseNames;

    uint16_t scannedEntries =
        0;

    File file =
        root.openNextFile();


    while (file) {

        if (!file.isDirectory()) {

            String name =
                String(file.name());

            int slashPos =
                name.lastIndexOf('/');

            if (slashPos >= 0) {
                name =
                    name.substring(
                        slashPos + 1
                    );
            }


            String lowerName =
                name;

            lowerName.toLowerCase();


            if (lowerName.endsWith(".srt")) {

                srtBaseNames.push_back(
                    lowerName.substring(
                        0,
                        lowerName.length() - 4
                    )
                );

            } else {

                bool isAvi =
                    lowerName.endsWith(".avi");

                bool isMkv =
                    lowerName.endsWith(".mkv");

                bool isJpeg =
                    lowerName.endsWith(".jpg") ||
                    lowerName.endsWith(".jpeg");


                if (isAvi || isMkv || isJpeg) {

                    RecordingEntry entry;

                    entry.name =
                        name;

                    entry.fullPath =
                        folderPath +
                        "/" +
                        name;

                    entry.annotation =
                        "";

                    entry.size =
                        file.size();

                    entry.durationMs =
                        0;

                    entry.durationValid =
                        false;

                    entry.isMkv =
                        isMkv;

                    entry.isJpeg =
                        isJpeg;

                    entry.hasSrt =
                        false;

                    entry.hasAudio =
                        false;

                    entry.corrupt =
                        false;


                    recordings.push_back(
                        entry
                    );
                }
            }
        }


        file.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        file =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    // Sort immediately after the single directory pass. Expensive per-file
    // work (decryptability/size, AVI/MKV duration probe, annotation load) is
    // intentionally deferred until AFTER the HTTP response has started. The
    // browser can therefore render each completed row while the next file is
    // being inspected instead of waiting for the entire day.
    std::sort(
        recordings.begin(),
        recordings.end(),
        recordingNameDescending
    );


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "X-Media-Count",
        String((unsigned long)recordings.size())
    );

    server.setContentLength(
        CONTENT_LENGTH_UNKNOWN
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        ""
    );


    if (recordings.empty()) {

        server.sendContent(
            "<div class='recording emptyrecording'>"
            "Keine Aufnahmen oder Snapshots an diesem Tag."
            "</div><!--SFCHUNK-->"
        );

        return;
    }


    // The explicit marker gives the browser a safe framing boundary. TCP /
    // fetch stream chunks may split or combine arbitrary bytes, so client code
    // must never assume one sendContent() call equals one ReadableStream chunk.
    server.sendContent(
        "<div class='dayMediaFilter'>"
        "<button type='button' class='mediaFilterMode active' "
        "onclick=\"filterDayMedia(this,'all')\">Show all</button>"
        "<button type='button' class='mediaFilterMode' "
        "onclick=\"filterDayMedia(this,'videos')\">Show videos</button>"
        "<button type='button' class='mediaFilterMode' "
        "onclick=\"filterDayMedia(this,'images')\">Show images/snapshots</button>"
        "<span class='selectionHint'>Shift oder Ziehen = Bereich</span>"
        "<span class='daySelectionSpacer'></span>"
        "<button type='button' class='selectionAction' "
        "onclick=\"setDaySelection(this,true)\">Select all</button>"
        "<button type='button' class='selectionAction' "
        "onclick=\"setDaySelection(this,false)\">Deselect all</button>"
        "<button type='button' class='deleteSelectedBtn' disabled "
        "onclick=\"deleteSelectedMedia(this)\">Delete selected</button>"
        "</div><!--SFCHUNK-->"
    );


    uint16_t matchedEntries =
        0;

    for (
        RecordingEntry &entry :
        recordings
    ) {

        matchedEntries++;

        if ((matchedEntries & 0x1FU) == 0)
            serviceWebLongOperation();

        uint64_t logicalSize = 0;

        bool logicalSizeOk =
            recordingStorageLogicalSize(
                entry.fullPath,
                logicalSize
            );

        if (!logicalSizeOk) {
            // Same defensive second chance as the day-ZIP path: a transient
            // lightweight header probe must not create a false corruption flag.
            RecordingStorageFile probe;

            if (
                probe.openRead(entry.fullPath) &&
                !probe.isDirectory()
            ) {
                logicalSize =
                    (uint64_t)probe.size();
                logicalSizeOk =
                    true;
            }

            probe.close();
        }

        if (logicalSizeOk) {
            entry.size = logicalSize;
        } else {
            entry.corrupt = true;
        }

        if (!entry.isJpeg) {
            uint64_t durationMs = 0;
            bool hasAudio = false;

            if (webPlayerProbeMediaInfo(
                    entry.fullPath,
                    durationMs,
                    hasAudio
                )) {
                entry.durationMs =
                    durationMs;

                entry.durationValid =
                    true;

                entry.hasAudio =
                    hasAudio;
            } else {
                // Lightweight AVI/MKV parsing failed: mark the file visibly
                // instead of discovering the problem only after opening it.
                entry.corrupt = true;
            }
        }

        if (!entry.isJpeg) {
            String annotation;

            if (loadRecordingAnnotationForList(
                    entry.fullPath,
                    annotation
                )) {
                entry.annotation =
                    annotation;
            }
        }

        if (!entry.isMkv && !entry.isJpeg) {
            entry.hasSrt =
                hasMatchingSrt(
                    entry.name,
                    srtBaseNames
                );
        }

        String row;
        row.reserve(640);


        row +=
            entry.isJpeg
            ? "<div class='recording' data-media='image'>"
            : "<div class='recording' data-media='video'>";


        row +=
            "<label class='mediaSelectWrap' title='Auswählen – ziehen oder Shift für Bereich'>"
            "<input class='mediaSelect' type='checkbox' data-path='" +
            htmlEscape(entry.fullPath) +
            "' onchange='updateDaySelection(this)'>"
            "</label>";


        if (entry.corrupt) {
            row +=
                "<span class='mediaCorruptIcon' "
                "title='Datei beschädigt, unvollständig oder nicht entschlüsselbar' "
                "aria-label='Datei beschädigt oder nicht lesbar'>"
                "<svg viewBox='0 0 24 24' aria-hidden='true'>"
                "<path d='M12 3L2.8 20h18.4L12 3z'></path>"
                "<path d='M12 8v5'></path>"
                "<circle cx='12' cy='16.5' r='.7'></circle>"
                "</svg></span>";
        }


        row +=
            entry.isJpeg
            ? "<span class='mediaKindIcon' title='Bild' aria-label='Bild'>"
              "<svg viewBox='0 0 24 24' aria-hidden='true'>"
              "<rect x='3' y='4' width='18' height='16' rx='2'></rect>"
              "<circle cx='8' cy='9' r='2'></circle>"
              "<path d='M5 17l4-4 3 3 3-4 4 5'></path>"
              "</svg></span>"
            : "<span class='mediaKindIcon' title='Video' aria-label='Video'>"
              "<svg viewBox='0 0 24 24' aria-hidden='true'>"
              "<rect x='2.5' y='5' width='13.5' height='14' rx='2'></rect>"
              "<path d='M16 9l5-3v12l-5-3z'></path>"
              "</svg></span>";


        if (entry.hasAudio) {
            row +=
                "<span class='mediaAudioIcon' title='Audio' aria-label='Audio'>"
                "<svg viewBox='0 0 24 24' aria-hidden='true'>"
                "<path d='M4 10v4h4l5 4V6L8 10H4z'></path>"
                "<path d='M16 9c1.3 1.3 1.3 4.7 0 6'></path>"
                "<path d='M18.5 6.5c3 3 3 8 0 11'></path>"
                "</svg></span>";
        }


        row +=
            "<span class='recname'>";

        row +=
            htmlEscape(
                displayRecordingTimeRange(
                    entry
                )
            );

        row +=
            "</span>";


        row +=
            "<span class='recmeta'>";

        row +=
            formatFileSize(
                entry.size
            );

        row +=
            "</span>";


        // Images and videos share the same viewer page so Previous/Next can
        // navigate consistently according to the active day filter. The
        // client-side openDayMedia() helper appends mode=all|videos|images
        // immediately before navigation.
        row +=
            "<a class='mediaOpen' onclick='return openDayMedia(this)' href='/play?path=";

        row +=
            urlEncode(
                entry.fullPath
            );

        row +=
            "&mode=all'><button>";

        row +=
            entry.isJpeg
            ? "Show"
            : "Play";

        row +=
            "</button></a>";


        row +=
            "<a href='/file?path=";

        row +=
            urlEncode(
                entry.fullPath
            );

        row +=
            "'><button>Download</button></a>";


        if (
            !entry.isMkv &&
            !entry.isJpeg &&
            entry.hasSrt
        ) {

            String srtPath =
                entry.fullPath.substring(
                    0,
                    entry.fullPath.length() - 4
                ) +
                ".srt";


            row +=
                "<a href='/file?path=";

            row +=
                urlEncode(
                    srtPath
                );

            row +=
                "'><button>SRT</button></a>";
        }


        if (!entry.isJpeg && entry.annotation.length()) {
            row +=
                "<span class='recannotation' title='";

            row +=
                htmlEscape(
                    entry.annotation
                );

            row +=
                "'>"
                "<svg viewBox='0 0 24 24' aria-hidden='true'>"
                "<path d='M4 4h16v12H8l-4 4z'></path>"
                "<path d='M8 8h8M8 12h6'></path>"
                "</svg>"
                "<span>";

            row +=
                htmlEscape(
                    entry.annotation
                );

            row +=
                "</span></span>";
        }


        row +=
            "</div>";


        row +=
            "<!--SFCHUNK-->";

        server.sendContent(
            row
        );
    }
}


// -------------------------------------------------------------
// DOWNLOAD ALL RECORDINGS OF ONE DAY AS STREAMING ZIP
//
// The ZIP is generated directly while sending it to the browser.
// No complete archive is created in RAM or on the SD card.
//
// Compression method: STORE (no compression). AVI/MKV already
// contain JPEG-compressed image data, so recompression would waste
// CPU without meaningfully reducing size.
//
// ZIP32 is intentionally used for simplicity and compatibility.
// A single file / complete archive >= 4 GB is rejected cleanly.
// -------------------------------------------------------------

struct DayZipEntry {
    String name;
    uint32_t size;
    uint32_t crc32;
    uint32_t localHeaderOffset;
    bool synthetic;
    String syntheticData;
};


// Day ZIP downloads are deliberately serialized. Arduino WebServer handlers run
// synchronously, so a second browser request cannot truly stream in parallel,
// but it can wait in the TCP queue and start immediately after the first one.
// Keep a short post-download guard so such queued requests are rejected instead
// of unexpectedly starting another large SD/crypto transfer.
static bool dayZipDownloadActive = false;
static uint32_t dayZipDownloadLastFinishedMs = 0;
static const uint32_t DAY_ZIP_REQUEUE_GUARD_MS = 3000UL;


static uint32_t dayZipDownloadCooldownRemainingMs()
{
    if (dayZipDownloadActive)
        return DAY_ZIP_REQUEUE_GUARD_MS;

    if (dayZipDownloadLastFinishedMs == 0)
        return 0;

    uint32_t elapsed =
        (uint32_t)(
            millis() -
            dayZipDownloadLastFinishedMs
        );

    if (elapsed >= DAY_ZIP_REQUEUE_GUARD_MS)
        return 0;

    return
        DAY_ZIP_REQUEUE_GUARD_MS -
        elapsed;
}


class DayZipDownloadGuard {
public:
    DayZipDownloadGuard()
    {
        dayZipDownloadActive = true;
    }

    ~DayZipDownloadGuard()
    {
        dayZipDownloadActive = false;
        dayZipDownloadLastFinishedMs = millis();
    }

    DayZipDownloadGuard(const DayZipDownloadGuard &) = delete;
    DayZipDownloadGuard &operator=(const DayZipDownloadGuard &) = delete;
};


static void handleDownloadDayStatus()
{
    uint32_t cooldownMs =
        dayZipDownloadCooldownRemainingMs();

    String json =
        String("{\"active\":") +
        (dayZipDownloadActive ? "true" : "false") +
        ",\"cooldown_ms\":" +
        String(cooldownMs) +
        "}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json; charset=utf-8",
        json
    );
}


static bool isRecordingFileForDayDownload(
    const String &name
)
{
    String lower =
        name;

    lower.toLowerCase();

    return
        lower.endsWith(".avi") ||
        lower.endsWith(".mkv") ||
        lower.endsWith(".srt") ||
        lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg") ||
        lower.endsWith(".note");
}


static bool dayZipResolveLogicalSize(
    const String &fullPath,
    uint64_t physicalSize,
    uint64_t &logicalSize,
    bool &encrypted
)
{
    encrypted = false;

    String lowerPath =
        fullPath;

    lowerPath.toLowerCase();

    // SRT sidecars are always plain files. Annotation sidecars may follow the
    // recording-encryption policy and therefore continue through SFENC1 probing.
    if (lowerPath.endsWith(".srt")) {
        logicalSize =
            physicalSize;
        return true;
    }

    // Fast path: validate the SFENC1 header without allocating the full
    // encrypted-reader chunk buffers.
    if (recordingStorageLogicalSize(
            fullPath,
            logicalSize,
            &encrypted
        )) {
        return true;
    }

    // Defensive fallback: use exactly the same reader path as the working
    // individual file download. This also gives transient crypto/storage state
    // one clean second chance before a file is classified as unreadable.
    RecordingStorageFile probe;

    if (
        probe.openRead(fullPath) &&
        !probe.isDirectory()
    ) {
        logicalSize =
            (uint64_t)probe.size();
        encrypted =
            probe.isEncrypted();

        probe.close();
        return true;
    }

    probe.close();
    return false;
}


// Header validation alone is not sufficient for SFENC1: a damaged encrypted
// chunk can fail authentication only when that part of the logical file is read.
// Validate every encrypted file fully BEFORE HTTP ZIP headers are sent. Plain
// files do not need the extra pass because they can be copied byte-for-byte even
// when their AVI/MKV structure is imperfect.
static bool dayZipValidateEncryptedReadable(
    const String &fullPath,
    uint64_t logicalSize,
    uint8_t *buffer,
    size_t bufferSize
)
{
    if (!buffer || bufferSize == 0)
        return false;

    RecordingStorageFile probe;

    if (
        !probe.openRead(fullPath) ||
        probe.isDirectory() ||
        !probe.isEncrypted()
    ) {
        probe.close();
        return false;
    }

    uint64_t totalRead = 0;

    while (totalRead < logicalSize) {
        uint64_t remaining =
            logicalSize -
            totalRead;

        size_t wanted =
            remaining < (uint64_t)bufferSize
            ? (size_t)remaining
            : bufferSize;

        size_t got =
            probe.read(
                buffer,
                wanted
            );

        if (got == 0) {
            probe.close();
            return false;
        }

        totalRead +=
            (uint64_t)got;

        serviceWebLongOperation();
    }

    bool ok =
        totalRead == logicalSize &&
        !probe.failed();

    probe.close();
    return ok;
}


static bool zipClientWriteAll(
    const uint8_t *data,
    size_t length
)
{
    size_t offset =
        0;

    uint32_t stalledSince =
        millis();


    while (offset < length) {

        if (!server.client().connected())
            return false;


        size_t written =
            server.client().write(
                data + offset,
                length - offset
            );


        if (written > 0) {

            offset +=
                written;

            stalledSince =
                millis();

        } else {

            if (
                millis() -
                stalledSince >
                5000UL
            ) {
                return false;
            }

            delay(1);
        }


        // The main task is subscribed to our task watchdog.
        // A large HTTP download may run much longer than 30 s.
        esp_task_wdt_reset();

        yield();
    }


    return true;
}


static bool zipWriteU16(
    uint16_t value
)
{
    uint8_t data[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU)
    };

    return
        zipClientWriteAll(
            data,
            sizeof(data)
        );
}


static bool zipWriteU32(
    uint32_t value
)
{
    uint8_t data[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU)
    };

    return
        zipClientWriteAll(
            data,
            sizeof(data)
        );
}


static uint32_t zipCrc32Update(
    uint32_t crc,
    const uint8_t *data,
    size_t length
)
{
    // 16-entry nibble table: much faster than the previous
    // bit-by-bit implementation while using only 64 bytes.
    static const uint32_t table[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };


    for (
        size_t i = 0;
        i < length;
        ++i
    ) {

        crc ^=
            data[i];


        crc =
            (crc >> 4) ^
            table[
                crc & 0x0FU
            ];


        crc =
            (crc >> 4) ^
            table[
                crc & 0x0FU
            ];
    }


    return
        crc;
}


static bool zipWriteLocalHeader(
    const DayZipEntry &entry
)
{
    const uint16_t flags =
        0x0808U; // data descriptor + UTF-8 filename

    uint16_t nameLength =
        (uint16_t)entry.name.length();


    return
        zipWriteU32(0x04034B50UL) &&
        zipWriteU16(20) &&
        zipWriteU16(flags) &&
        zipWriteU16(0) &&     // STORE
        zipWriteU16(0) &&     // DOS time
        zipWriteU16(0) &&     // DOS date
        zipWriteU32(0) &&     // CRC follows in descriptor
        zipWriteU32(0) &&
        zipWriteU32(0) &&
        zipWriteU16(nameLength) &&
        zipWriteU16(0) &&
        zipClientWriteAll(
            (const uint8_t *)entry.name.c_str(),
            nameLength
        );
}


static bool zipWriteDataDescriptor(
    const DayZipEntry &entry
)
{
    return
        zipWriteU32(0x08074B50UL) &&
        zipWriteU32(entry.crc32) &&
        zipWriteU32(entry.size) &&
        zipWriteU32(entry.size);
}


static bool zipWriteCentralHeader(
    const DayZipEntry &entry
)
{
    const uint16_t flags =
        0x0808U;

    uint16_t nameLength =
        (uint16_t)entry.name.length();


    return
        zipWriteU32(0x02014B50UL) &&
        zipWriteU16(20) &&     // version made by
        zipWriteU16(20) &&     // version needed
        zipWriteU16(flags) &&
        zipWriteU16(0) &&      // STORE
        zipWriteU16(0) &&
        zipWriteU16(0) &&
        zipWriteU32(entry.crc32) &&
        zipWriteU32(entry.size) &&
        zipWriteU32(entry.size) &&
        zipWriteU16(nameLength) &&
        zipWriteU16(0) &&      // extra
        zipWriteU16(0) &&      // comment
        zipWriteU16(0) &&      // disk
        zipWriteU16(0) &&      // internal attrs
        zipWriteU32(0) &&      // external attrs
        zipWriteU32(
            entry.localHeaderOffset
        ) &&
        zipClientWriteAll(
            (const uint8_t *)entry.name.c_str(),
            nameLength
        );
}


static void handleDownloadDay()
{
    if (rejectWhileRecording("download day archive"))
        return;


    String day =
        server.arg("day");

    day.trim();


    if (
        !isDateFolderName(day) &&
        day != "fallback"
    ) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    uint32_t dayZipCooldownMs =
        dayZipDownloadCooldownRemainingMs();

    if (
        dayZipDownloadActive ||
        dayZipCooldownMs > 0
    ) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Another day ZIP download is already active or has just finished."
        );
        return;
    }

    DayZipDownloadGuard dayZipGuard;


    // Release any old WebPlayer SD handles before starting a
    // potentially long sequential SD read.
    webPlayerStop();


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    std::vector<DayZipEntry> entries;

    entries.reserve(
        32
    );


    uint64_t totalZipBytes =
        22ULL; // end of central directory

    uint64_t localAreaBytes =
        0;

    uint64_t centralAreaBytes =
        0;


    uint16_t scannedEntries =
        0;

    File file =
        root.openNextFile();


    while (file) {

        if (!file.isDirectory()) {

            String name =
                String(file.name());

            int slashPos =
                name.lastIndexOf('/');


            if (slashPos >= 0) {
                name =
                    name.substring(
                        slashPos + 1
                    );
            }


            if (
                isRecordingFileForDayDownload(
                    name
                )
            ) {

                uint64_t fileSize =
                    file.size();


                if (
                    name.length() >
                    0xFFFFU
                ) {

                    file.close();
                    root.close();

                    server.send(
                        413,
                        "text/plain; charset=utf-8",
                        "Day archive exceeds ZIP32 limits."
                    );

                    return;
                }


                DayZipEntry entry;

                entry.name =
                    name;

                entry.size =
                    (uint32_t)fileSize;

                entry.crc32 =
                    0;

                entry.localHeaderOffset =
                    0;

                entry.synthetic =
                    false;

                entry.syntheticData =
                    "";


                entries.push_back(
                    entry
                );


            }
        }


        file.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        file =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    if (entries.empty()) {

        server.send(
            404,
            "text/plain; charset=utf-8",
            "No recordings found for this day."
        );

        return;
    }


    // ZIP sizes and CRCs must describe the logical plaintext files, not the
    // physical SFENC1 bytes stored on SD. Unreadable/corrupt files are skipped
    // instead of aborting the entire day archive. Encrypted files are read once
    // completely during preflight so a damaged SFENC1 chunk cannot break the ZIP
    // after HTTP headers have already been sent.
    localAreaBytes = 0;
    centralAreaBytes = 0;

    static const size_t DAY_ZIP_VALIDATE_BUFFER_SIZE =
        4U * 1024U;

    uint8_t *validationBuffer =
        (uint8_t *)malloc(
            DAY_ZIP_VALIDATE_BUFFER_SIZE
        );

    if (!validationBuffer) {
        server.send(
            503,
            "text/plain; charset=utf-8",
            "ZIP validation buffer unavailable."
        );
        return;
    }

    std::vector<DayZipEntry> readableEntries;
    readableEntries.reserve(entries.size() + 1U);

    std::vector<String> skippedEntries;
    skippedEntries.reserve(8);

    for (DayZipEntry &entry : entries) {
        String fullPath =
            folderPath +
            "/" +
            entry.name;

        uint64_t logicalSize = 0;
        bool encrypted = false;

        bool readable =
            dayZipResolveLogicalSize(
                fullPath,
                (uint64_t)entry.size,
                logicalSize,
                encrypted
            ) &&
            logicalSize <= 0xFFFFFFFFULL;

        if (
            readable &&
            encrypted
        ) {
            readable =
                dayZipValidateEncryptedReadable(
                    fullPath,
                    logicalSize,
                    validationBuffer,
                    DAY_ZIP_VALIDATE_BUFFER_SIZE
                );
        }

        // Keep ZIP behavior consistent with the red corruption marker used by
        // the recordings list. A video whose AVI/MKV metadata cannot be parsed
        // is skipped even if its raw/plain bytes could still be copied.
        if (readable) {
            String lowerName =
                entry.name;
            lowerName.toLowerCase();

            bool isVideo =
                lowerName.endsWith(".avi") ||
                lowerName.endsWith(".mkv");

            if (isVideo) {
                uint64_t ignoredDurationMs = 0;

                if (!webPlayerProbeDurationMs(
                        fullPath,
                        ignoredDurationMs
                    )) {
                    readable = false;
                }
            }
        }

        if (!readable) {
            skippedEntries.push_back(
                entry.name
            );

            Serial.println(
                "ZIP download: skipping unreadable/corrupt file " +
                fullPath
            );

            logWrite(
                "ZIP download skipped unreadable/corrupt file: " +
                fullPath
            );

            continue;
        }

        entry.size =
            (uint32_t)logicalSize;

        readableEntries.push_back(
            entry
        );

        localAreaBytes +=
            30ULL +
            (uint64_t)entry.name.length() +
            logicalSize +
            16ULL;

        centralAreaBytes +=
            46ULL +
            (uint64_t)entry.name.length();
    }

    free(validationBuffer);
    validationBuffer = nullptr;

    entries.swap(
        readableEntries
    );


    // Make skipped files visible to the operator inside the otherwise valid
    // archive. This synthetic text entry is generated in RAM and does not touch
    // the SD card.
    if (!skippedEntries.empty()) {
        String report =
            "SensorForge Tagesarchiv\r\n"
            "Folgende Dateien wurden wegen Lese-/Entschluesselungsfehlern nicht in das ZIP aufgenommen:\r\n\r\n";

        for (const String &name : skippedEntries) {
            report +=
                "- " +
                name +
                "\r\n";
        }

        report +=
            "\r\nDie uebrigen lesbaren Dateien wurden normal archiviert.\r\n";

        DayZipEntry reportEntry;
        reportEntry.name =
            "_SENSORFORGE_SKIPPED_CORRUPT.txt";
        reportEntry.size =
            (uint32_t)report.length();
        reportEntry.crc32 = 0;
        reportEntry.localHeaderOffset = 0;
        reportEntry.synthetic = true;
        reportEntry.syntheticData = report;

        entries.push_back(
            reportEntry
        );

        localAreaBytes +=
            30ULL +
            (uint64_t)reportEntry.name.length() +
            (uint64_t)reportEntry.size +
            16ULL;

        centralAreaBytes +=
            46ULL +
            (uint64_t)reportEntry.name.length();
    }


    if (entries.empty()) {
        server.send(
            404,
            "text/plain; charset=utf-8",
            "No readable recordings found for this day."
        );
        return;
    }


    if (
        entries.size() >
        0xFFFFU
    ) {

        server.send(
            413,
            "text/plain; charset=utf-8",
            "Too many files for ZIP32 archive."
        );

        return;
    }


    totalZipBytes +=
        localAreaBytes +
        centralAreaBytes;


    if (
        totalZipBytes >
        0xFFFFFFFFULL ||
        localAreaBytes >
        0xFFFFFFFFULL ||
        centralAreaBytes >
        0xFFFFFFFFULL
    ) {

        server.send(
            413,
            "text/plain; charset=utf-8",
            "Day archive exceeds ZIP32 4 GB limit."
        );

        return;
    }


    String archiveName =
        "recordings_" +
        day +
        ".zip";


    archiveName.replace(
        "\"",
        "_"
    );

    archiveName.replace(
        "\r",
        "_"
    );

    archiveName.replace(
        "\n",
        "_"
    );


    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" +
        archiveName +
        "\""
    );

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );

    server.setContentLength(
        (size_t)totalZipBytes
    );

    server.send(
        200,
        "application/zip",
        ""
    );


    static const size_t ZIP_BUFFER_SIZE =
        8U * 1024U;

    uint8_t *buffer =
        (uint8_t *)malloc(
            ZIP_BUFFER_SIZE
        );


    if (!buffer) {

        Serial.println(
            "ZIP download: buffer allocation failed"
        );

        logWrite(
            "ZIP download failed: buffer allocation"
        );

        server.client().stop();

        return;
    }


    bool ok =
        true;

    uint32_t outputOffset =
        0;


    for (
        DayZipEntry &entry :
        entries
    ) {

        entry.localHeaderOffset =
            outputOffset;


        if (!zipWriteLocalHeader(
                entry
            )) {

            ok =
                false;

            break;
        }


        outputOffset +=
            30U +
            (uint32_t)entry.name.length();


        uint32_t crc =
            0xFFFFFFFFUL;

        uint32_t sentForFile =
            0;


        if (entry.synthetic) {
            size_t dataLength =
                entry.syntheticData.length();

            if (dataLength > 0) {
                const uint8_t *data =
                    (const uint8_t *)entry.syntheticData.c_str();

                crc =
                    zipCrc32Update(
                        crc,
                        data,
                        dataLength
                    );

                if (!zipClientWriteAll(
                        data,
                        dataLength
                    )) {
                    ok = false;
                } else {
                    sentForFile =
                        (uint32_t)dataLength;
                }
            }

        } else {
            String fullPath =
                folderPath +
                "/" +
                entry.name;

            RecordingStorageFile input;

            if (!input.openRead(fullPath)) {

                Serial.println(
                    "ZIP download: cannot open after successful preflight " +
                    fullPath
                );

                ok =
                    false;

            } else {
                while (input.available()) {

                    size_t got =
                        input.read(
                            buffer,
                            ZIP_BUFFER_SIZE
                        );


                    if (got == 0) {

                        ok =
                            false;

                        break;
                    }


                    crc =
                        zipCrc32Update(
                            crc,
                            buffer,
                            got
                        );


                    if (!zipClientWriteAll(
                            buffer,
                            got
                        )) {

                        ok =
                            false;

                        break;
                    }


                    sentForFile +=
                        (uint32_t)got;
                }
            }

            input.close();
        }


        if (
            !ok ||
            sentForFile !=
                entry.size
        ) {

            ok =
                false;

            break;
        }


        entry.crc32 =
            crc ^
            0xFFFFFFFFUL;


        outputOffset +=
            entry.size;


        if (!zipWriteDataDescriptor(
                entry
            )) {

            ok =
                false;

            break;
        }


        outputOffset +=
            16U;
    }


    uint32_t centralOffset =
        outputOffset;

    uint32_t centralSize =
        0;


    if (ok) {

        for (
            const DayZipEntry &entry :
            entries
        ) {

            if (!zipWriteCentralHeader(
                    entry
                )) {

                ok =
                    false;

                break;
            }


            uint32_t headerSize =
                46U +
                (uint32_t)entry.name.length();


            centralSize +=
                headerSize;

            outputOffset +=
                headerSize;
        }
    }


    if (ok) {

        uint16_t entryCount =
            (uint16_t)entries.size();


        ok =
            zipWriteU32(0x06054B50UL) &&
            zipWriteU16(0) &&
            zipWriteU16(0) &&
            zipWriteU16(entryCount) &&
            zipWriteU16(entryCount) &&
            zipWriteU32(centralSize) &&
            zipWriteU32(centralOffset) &&
            zipWriteU16(0);


        outputOffset +=
            22U;
    }


    free(
        buffer
    );


    if (
        !ok ||
        outputOffset !=
            (uint32_t)totalZipBytes
    ) {

        Serial.println(
            "ZIP download interrupted or failed"
        );

        logWrite(
            "ZIP download failed/interrupted: " +
            day
        );

        server.client().stop();

        return;
    }


    Serial.println(
        "ZIP download completed: " +
        archiveName +
        " (" +
        String(entries.size()) +
        " archive entries, skipped=" +
        String(skippedEntries.size()) +
        ")"
    );

    logWrite(
        "ZIP download completed: " +
        archiveName +
        " (" +
        String(entries.size()) +
        " archive entries, skipped=" +
        String(skippedEntries.size()) +
        ")"
    );
}


// -------------------------------------------------------------
// DELETE ALL RECORDINGS OF ONE DAY
//
// Safety rules:
// - POST only (route registration below)
// - YYYYMMDD or "fallback" only
// - blocked while recording
// - only known recording files are removed
// - unknown files/directories are never touched
// - day directory is removed only when it is really empty
// -------------------------------------------------------------

static bool isRecordingFileForDayDelete(
    const String &name
)
{
    String lower =
        name;

    lower.toLowerCase();

    return
        lower.endsWith(".avi") ||
        lower.endsWith(".mkv") ||
        lower.endsWith(".srt") ||
        lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg") ||
        lower.endsWith(".note") ||
        lower.endsWith(".note.tmp") ||
        lower.endsWith(".avi.part") ||
        lower.endsWith(".mkv.part") ||
        lower.endsWith(".srt.part") ||
        lower.endsWith(".jpg.part") ||
        lower.endsWith(".jpeg.part");
}


static bool directoryReallyEmpty(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        return false;
    }


    File entry =
        root.openNextFile();

    bool empty =
        !entry;


    if (entry)
        entry.close();

    root.close();

    return empty;
}


static void handleDeleteDay()
{
    if (rejectWhileRecording("delete recordings"))
        return;


    String day =
        server.arg("day");

    day.trim();


    if (
        !isDateFolderName(day) &&
        day != "fallback"
    ) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    // First collect filenames and close the directory handle.
    // Deleting files while iterating an open FAT directory can
    // otherwise cause entries to be skipped on some FS versions.
    std::vector<String> filesToDelete;

    filesToDelete.reserve(
        32
    );


    uint16_t scannedEntries =
        0;

    File entry =
        root.openNextFile();


    while (entry) {

        bool isDir =
            entry.isDirectory();

        String name =
            String(entry.name());

        int slashPos =
            name.lastIndexOf('/');


        if (slashPos >= 0) {
            name =
                name.substring(
                    slashPos + 1
                );
        }


        if (
            !isDir &&
            isRecordingFileForDayDelete(
                name
            )
        ) {

            filesToDelete.push_back(
                name
            );
        }


        entry.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        entry =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    uint32_t deleted =
        0;

    uint32_t failed =
        0;


    for (
        const String &name :
        filesToDelete
    ) {

        String fullPath =
            folderPath +
            "/" +
            name;


        if (STORAGE.remove(
                fullPath.c_str()
            )) {

            deleted++;

        } else {

            failed++;

            Serial.println(
                "Delete day: failed to remove " +
                fullPath
            );

            logWrite(
                "Delete day failed: " +
                fullPath
            );
        }


        // This handler is synchronous inside loopTask.
        serviceWebLongOperation();

        delay(1);
    }


    // Remove the date directory only if no unknown file or
    // subdirectory remains.
    if (directoryReallyEmpty(
            folderPath
        )) {

        STORAGE.rmdir(
            folderPath.c_str()
        );
    }


    if (failed > 0) {

        server.send(
            500,
            "text/plain; charset=utf-8",
            "Deleted " +
            String(deleted) +
            " files, but " +
            String(failed) +
            " files could not be deleted."
        );

        return;
    }


    Serial.println(
        "Delete day: " +
        day +
        " -> " +
        String(deleted) +
        " recording file(s) deleted"
    );

    logWrite(
        "Delete day: " +
        day +
        " -> " +
        String(deleted) +
        " recording file(s) deleted"
    );


    server.send(
        200,
        "text/plain; charset=utf-8",
        String(deleted) +
        " recording file(s) deleted."
    );
}


// -------------------------------------------------------------
// Main recording page.
//
// Only root directories are scanned here.
// -------------------------------------------------------------

static void handleFiles()
{
    maybeAutoPauseRecordingForWebUi();

    if (recorderIsOpen()) {
        String html =
            htmlHeader();

        html +=
            "<div class='page-title'><div>"
            "<h2>Aufnahmen</h2>"
            "<p>Aufnahmen verwalten und wiedergeben</p>"
            "</div></div>";

        html +=
            recordingBrowserModalHtml(
                true
            );

        html +=
            "<script>"
            "(function(){"
            "var state=document.getElementById('recordingBlockedState');"
            "function check(){"
                "fetch('/ui_status?t='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error();return r.json();})"
                ".then(function(s){"
                    "if(!s.recorder_open){"
                        "if(state)state.textContent='Aufnahme beendet – lade Verzeichnis ...';"
                        "setTimeout(function(){location.reload();},350);"
                    "}"
                "}).catch(function(){});"
            "}"
            "setInterval(check,1500);"
            "check();"
            "})();"
            "</script>";

        html +=
            htmlFooter();

        server.sendHeader(
            "Cache-Control",
            "no-store"
        );

        server.send(
            409,
            "text/html; charset=utf-8",
            html
        );

        return;
    }

    String html =
        htmlHeader();

    html +=
        "<h2>Aufnahmen</h2>";

    html +=
        "<div class='recordingsToolbar'>"
        "<button type='button' id='reloadRecordingsBtn' "
        "onclick='reloadRecordingTree()'>"
        "Verzeichnis neu laden"
        "</button>"
        "</div>";

    html +=
        "<p>Tage anklicken, um die Aufnahmen zu laden.</p>";

    html +=
        recordingBrowserModalHtml(
            false
        );


    std::vector<String> dayFolders;

    bool fallbackExists =
        false;


    File root =
        STORAGE.open(
            "/",
            FILE_READ
        );


    if (root && root.isDirectory()) {

        uint16_t scannedEntries =
            0;

        File file =
            root.openNextFile();


        while (file) {

            if (file.isDirectory()) {

                String name =
                    String(file.name());

                int slashPos =
                    name.lastIndexOf('/');

                if (slashPos >= 0) {
                    name =
                        name.substring(
                            slashPos + 1
                        );
                }


                if (isDateFolderName(name)) {

                    dayFolders.push_back(
                        name
                    );

                } else if (
                    name == "fallback"
                ) {

                    fallbackExists =
                        true;
                }
            }


            file.close();

            scannedEntries++;

            if ((scannedEntries & 0x0FU) == 0)
                serviceWebLongOperation();

            file =
                root.openNextFile();
        }


        root.close();

        serviceWebLongOperation();
    }


    std::sort(
        dayFolders.begin(),
        dayFolders.end(),
        stringDescending
    );


    if (
        dayFolders.empty() &&
        !fallbackExists
    ) {

        html +=
            "<p>Keine Aufnahmen gefunden.</p>";
    }


    for (
        const String &folderName :
        dayFolders
    ) {

        html +=
            "<details class='day' data-day='" +
            htmlEscape(folderName) +
            "'>";

        html +=
            "<summary>"
            "<span class='dayLabel'>" +
            htmlEscape(
                displayDateFolder(
                    folderName
                )
            ) +
            "<span class='count'></span>"
            "</span>"
            "<span class='dayActions'>"
            "<button type='button' class='downloadDayBtn' "
            "onclick=\"downloadDay(event,'" +
            htmlEscape(folderName) +
            "')\">"
            "Download ZIP"
            "</button>"
            "<button type='button' class='deleteDayBtn' "
            "onclick=\"deleteDay(event,'" +
            htmlEscape(folderName) +
            "')\">"
            "Alle löschen"
            "</button>"
            "</span>"
            "</summary>";

        html +=
            "<div class='daycontent' "
            "style='padding:8px 12px;'>"
            "Zum Laden aufklappen..."
            "</div>";

        html +=
            "</details>";
    }


    if (fallbackExists) {

        html +=
            "<details class='day' data-day='fallback'>"
            "<summary>"
            "<span class='dayLabel'>"
            "Zeit unbekannt / fallback"
            "<span class='count'></span>"
            "</span>"
            "<span class='dayActions'>"
            "<button type='button' class='downloadDayBtn' "
            "onclick=\"downloadDay(event,'fallback')\">"
            "Download ZIP"
            "</button>"
            "<button type='button' class='deleteDayBtn' "
            "onclick=\"deleteDay(event,'fallback')\">"
            "Alle löschen"
            "</button>"
            "</span>"
            "</summary>"
            "<div class='daycontent' "
            "style='padding:8px 12px;'>"
            "Zum Laden aufklappen..."
            "</div>"
            "</details>";
    }


    // Preserve the opened day and its already loaded HTML in the
    // browser session. Returning from the player therefore does not
    // require another SD directory scan.
    html +=
        "<style>"
        ".recordingsToolbar{"
            "display:flex;justify-content:flex-end;align-items:center;"
            "margin:0 0 10px 0;"
        "}"
        ".recordingsToolbar button{"
            "width:auto;margin:0;padding:7px 11px;"
            "border:1px solid #98a2b3;background:#f8fafc;color:#344054;"
            "border-radius:5px;cursor:pointer;"
        "}"
        ".recordingsToolbar button:disabled{"
            "opacity:.65;cursor:default;"
        "}"
        ".day{"
            "margin:0;border:0;border-bottom:1px solid #d6d6d6;"
            "background:#ffffff;"
        "}"
        ".day:nth-of-type(even){background:#eef3f7;}"
        ".day summary{"
            "cursor:pointer;display:grid;"
            "grid-template-columns:14px minmax(185px,max-content) auto;"
            "align-items:center;column-gap:10px;"
            "padding:9px 10px;min-height:38px;"
            "list-style:none;"
        "}"
        ".day summary::-webkit-details-marker{display:none;}"
        ".day summary::before{"
            "content:'';display:block;width:0;height:0;"
            "border-top:5px solid transparent;"
            "border-bottom:5px solid transparent;"
            "border-left:7px solid #555;"
            "transform-origin:40% 50%;"
            "transition:transform .12s ease;"
        "}"
        ".day[open]>summary::before{transform:rotate(90deg);}"
        ".dayLabel{"
            "display:inline-flex;align-items:center;gap:6px;"
            "font-weight:600;"
        "}"
        ".count{font-weight:normal;color:#666;}"
        ".dayActions{"
            "display:inline-flex;align-items:center;gap:6px;"
            "flex-wrap:wrap;"
        "}"
        ".downloadDayBtn,.deleteDayBtn{"
            "width:auto;margin:0;padding:5px 9px;"
            "border-radius:4px;font-size:12px;cursor:pointer;"
        "}"
        ".downloadDayBtn{"
            "border:1px solid #777;background:#f7f7f7;color:#222;"
        "}"
        ".downloadDayBtn:disabled{opacity:.55;cursor:default;}"
        ".deleteDayBtn{"
            "background:#b00020;color:white;border:1px solid #b00020;"
        "}"
        ".dayMediaFilter{"
            "display:flex;gap:6px;align-items:center;flex-wrap:wrap;"
            "margin:2px 0 10px 0;padding-bottom:8px;"
            "border-bottom:1px solid #d6d6d6;"
        "}"
        ".dayMediaFilter button{"
            "width:auto;margin:0;padding:5px 9px;"
            "border:1px solid #98a2b3;background:#f8fafc;color:#344054;"
            "border-radius:4px;font-size:12px;cursor:pointer;"
        "}"
        ".dayMediaFilter button.active{"
            "background:#344054;color:#fff;border-color:#344054;"
        "}"
        ".selectionHint{font-size:11px;color:#667085;white-space:nowrap;}"
        ".daySelectionSpacer{flex:1 1 18px;}"
        ".dayMediaFilter .selectionAction{background:#fff;color:#344054;}"
        ".dayMediaFilter .deleteSelectedBtn{background:#b42318;color:#fff;border-color:#b42318;}"
        ".dayMediaFilter .deleteSelectedBtn:disabled{opacity:.45;cursor:default;}"
        ".mediaSelectWrap{display:inline-flex;align-items:center;justify-content:center;width:32px;min-height:28px;margin-right:3px;vertical-align:middle;cursor:crosshair;user-select:none;}"
        ".mediaSelect{width:17px;height:17px;margin:0;cursor:crosshair;}"
        ".recording.mediaSelected{background:#eef4ff;box-shadow:inset 3px 0 0 #2563eb;}"
        ".mediaRangeSelecting,.mediaRangeSelecting *{user-select:none!important;}"
        ".mediaCorruptIcon{display:inline-flex;width:20px;height:20px;align-items:center;justify-content:center;margin-right:5px;vertical-align:middle;color:#b42318;}"
        ".mediaCorruptIcon svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;}"
        ".mediaKindIcon{"
            "display:inline-flex;width:20px;height:20px;"
            "align-items:center;justify-content:center;"
            "margin-right:7px;vertical-align:middle;color:#475467;"
        "}"
        ".mediaKindIcon svg{"
            "width:18px;height:18px;fill:none;stroke:currentColor;"
            "stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;"
        "}"
        ".mediaAudioIcon{display:inline-flex;width:20px;height:20px;align-items:center;justify-content:center;margin-right:5px;vertical-align:middle;color:#1769aa;}"
        ".mediaAudioIcon svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;}"
        ".recname{display:inline-block;min-width:150px;}"
        ".recmeta{display:inline-block;min-width:70px;color:#667085;}"
        ".recannotation{display:inline-flex;align-items:center;gap:5px;max-width:min(440px,38vw);margin-left:10px;padding:3px 8px;border:1px solid #cbd5e1;border-radius:999px;background:#f8fafc;color:#475467;font-size:12px;line-height:1.3;vertical-align:middle;white-space:nowrap;overflow:hidden;}"
        ".recannotation svg{flex:0 0 auto;width:15px;height:15px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;}"
        ".recannotation span{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
        ".daycontent{background:rgba(255,255,255,0.55);}"
        "@media(max-width:700px){"
            ".day summary{"
                "grid-template-columns:14px 1fr;"
                "row-gap:7px;align-items:center;"
            "}"
            ".dayActions{grid-column:2;padding-left:0;}"
            ".recannotation{max-width:calc(100vw - 92px);margin:6px 0 2px 47px;}"
        "}"
        "</style>"
        "<script>"
        "const OPEN_DAY_KEY='recordings.openDay';"
        "const CACHE_PREFIX='recordings.v54.day.';"
        "const BOOT_KEY='recordings.bootId';"
        "const BOOT_ID='" +
        String(webBootSessionId, HEX) +
        "';"

        "let sameBoot=false;"
        "try{"
            "const dirtyDay=sessionStorage.getItem('recordings.annotationDirtyDay');"
            "if(dirtyDay){sessionStorage.removeItem(CACHE_PREFIX+dirtyDay);sessionStorage.removeItem('recordings.annotationDirtyDay');}"
            "const previousBoot=sessionStorage.getItem(BOOT_KEY);"
            "sameBoot=(previousBoot===BOOT_ID);"
            "if(!sameBoot){"
                "sessionStorage.removeItem(OPEN_DAY_KEY);"
                "const removeKeys=[];"
                "for(let i=0;i<sessionStorage.length;i++){"
                    "const k=sessionStorage.key(i);"
                    "if(k&&k.indexOf(CACHE_PREFIX)===0)removeKeys.push(k);"
                "}"
                "removeKeys.forEach(function(k){sessionStorage.removeItem(k);});"
                "sessionStorage.setItem(BOOT_KEY,BOOT_ID);"
            "}"
        "}catch(e){}"

        "function reloadRecordingTree(){"
            "const b=document.getElementById('reloadRecordingsBtn');"
            "if(b){b.disabled=true;b.textContent='Lade neu...';}"
            "try{"
                "const removeKeys=[];"
                "for(let i=0;i<sessionStorage.length;i++){"
                    "const k=sessionStorage.key(i);"
                    "if(k&&k.indexOf('recordings.')===0)removeKeys.push(k);"
                "}"
                "removeKeys.forEach(function(k){sessionStorage.removeItem(k);});"
            "}catch(e){}"
            "window.location.reload();"
        "}"

        "function showRecordingBlockedDialog(){"
            "const m=document.getElementById('recordingBlockedModal');"
            "const state=document.getElementById('recordingBlockedState');"
            "if(m)m.hidden=false;"
            "if(state)state.textContent='Warte auf Aufnahmeende ...';"
        "}"

        "function watchRecordingBlockedDialog(){"
            "const m=document.getElementById('recordingBlockedModal');"
            "if(!m||m.hidden)return;"
            "fetch('/ui_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error();return r.json();})"
            ".then(function(s){"
                "if(!s.recorder_open){"
                    "const state=document.getElementById('recordingBlockedState');"
                    "if(state)state.textContent='Aufnahme beendet – Verzeichnis kann wieder geladen werden.';"
                    "setTimeout(function(){reloadRecordingTree();},350);"
                "}"
            "}).catch(function(){});"
        "}"

        "setInterval(watchRecordingBlockedDialog,1500);"

        "let dayDownloadUiBusy=false;"
        "let dayDownloadPollTimer=0;"

        "function setDayDownloadUiBusy(busy,sourceButton){"
            "dayDownloadUiBusy=busy;"
            "document.querySelectorAll('.downloadDayBtn').forEach(function(b){"
                "if(!b.dataset.normalText)b.dataset.normalText=b.textContent;"
                "b.disabled=busy;"
                "b.textContent=(busy&&b===sourceButton)?'Download läuft...':b.dataset.normalText;"
            "});"
        "}"

        "function pollDayDownloadStatus(){"
            "if(!dayDownloadUiBusy)return;"
            "fetch('/download_day_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
            ".then(function(s){"
                "const cooldown=Number(s.cooldown_ms)||0;"
                "if(s.active||cooldown>0){"
                    "dayDownloadPollTimer=setTimeout(pollDayDownloadStatus,1000);"
                    "return;"
                "}"
                "setDayDownloadUiBusy(false,null);"
            "})"
            ".catch(function(){dayDownloadPollTimer=setTimeout(pollDayDownloadStatus,1500);});"
        "}"

        "function startDayDownload(day,sourceButton){"
            "let frame=document.getElementById('dayDownloadFrame');"
            "if(!frame){"
                "frame=document.createElement('iframe');"
                "frame.id='dayDownloadFrame';"
                "frame.name='dayDownloadFrame';"
                "frame.hidden=true;"
                "document.body.appendChild(frame);"
            "}"
            "frame.src='/download_day?day='+encodeURIComponent(day)+'&t='+Date.now();"
            "dayDownloadPollTimer=setTimeout(pollDayDownloadStatus,1500);"
        "}"

        "function downloadDay(ev,day){"
            "ev.preventDefault();"
            "ev.stopPropagation();"
            "if(dayDownloadUiBusy)return;"
            "const sourceButton=ev.currentTarget||ev.target;"
            "setDayDownloadUiBusy(true,sourceButton);"
            "fetch('/download_day_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
            ".then(function(s){"
                "const cooldown=Number(s.cooldown_ms)||0;"
                "if(s.active||cooldown>0){"
                    "setDayDownloadUiBusy(false,null);"
                    "alert('Ein Tagesdownload läuft bereits oder wurde gerade beendet. Bitte kurz warten.');"
                    "return;"
                "}"
                "startDayDownload(day,sourceButton);"
            "})"
            ".catch(function(e){"
                "setDayDownloadUiBusy(false,null);"
                "alert('Download konnte nicht gestartet werden: '+e.message);"
            "});"
        "}"

        "function deleteDay(ev,day){"
            "ev.preventDefault();"
            "ev.stopPropagation();"
            "const d=ev.target.closest('details.day');"
            "let label=day;"
            "if(/^\\d{8}$/.test(day)){"
                "label=day.substring(6,8)+'.'+day.substring(4,6)+'.'+day.substring(0,4);"
            "}"
            "if(!confirm('Alle Videos und Bilder vom '+label+' wirklich löschen?\\n\\nDiese Aktion kann nicht rückgängig gemacht werden.'))return;"
            "ev.target.disabled=true;"
            "ev.target.textContent='Lösche...';"
            "fetch('/delete_day',{"
                "method:'POST',"
                "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
                "body:'day='+encodeURIComponent(day)"
            "})"
            ".then(function(r){"
                "return r.text().then(function(t){"
                    "if(!r.ok)throw new Error(t||('HTTP '+r.status));"
                    "return t;"
                "});"
            "})"
            ".then(function(){"
                "try{"
                    "sessionStorage.removeItem(CACHE_PREFIX+day);"
                    "if(sessionStorage.getItem(OPEN_DAY_KEY)===day){"
                        "sessionStorage.removeItem(OPEN_DAY_KEY);"
                    "}"
                "}catch(e){}"
                "if(d)d.remove();"
                "if(!document.querySelector('details.day'))location.reload();"
            "})"
            ".catch(function(e){"
                "ev.target.disabled=false;"
                "ev.target.textContent='Alle löschen';"
                "alert('Löschen fehlgeschlagen: '+e.message);"
            "});"
        "}"

        "function filterDayMedia(button,mode){"
            "const d=button.closest('details.day');"
            "if(!d)return;"
            "const content=d.querySelector('.daycontent');"
            "if(!content)return;"
            "if(mode!=='videos'&&mode!=='images')mode='all';"
            "content.dataset.mediaMode=mode;"
            "content._sfSelectionAnchor=null;"
            "content.querySelectorAll('.recording[data-media]').forEach(function(row){"
                "const hidden=(mode==='videos'&&row.dataset.media!=='video')||(mode==='images'&&row.dataset.media!=='image');"
                "row.hidden=hidden;"
                "if(hidden){const cb=row.querySelector('.mediaSelect');if(cb)setMediaCheckboxState(cb,false);}"
            "});"
            "content.querySelectorAll('.dayMediaFilter .mediaFilterMode').forEach(function(b){b.classList.remove('active');});"
            "button.classList.add('active');"
            "updateDaySelection(content);"
        "}"

        "function setMediaCheckboxState(cb,selected){"
        "    if(!cb)return;"
        "    cb.checked=!!selected;"
        "    const row=cb.closest('.recording[data-media]');"
        "    if(row)row.classList.toggle('mediaSelected',cb.checked);"
        "}"
        ""
        "function syncMediaSelectionRows(content){"
        "    if(!content)return;"
        "    content.querySelectorAll('.recording[data-media] .mediaSelect').forEach(function(cb){"
        "        const row=cb.closest('.recording[data-media]');"
        "        if(row)row.classList.toggle('mediaSelected',cb.checked);"
        "    });"
        "}"
        ""
        "function updateDaySelection(source){"
        "    const content=source&&source.closest?source.closest('.daycontent'):source;"
        "    if(!content)return;"
        "    syncMediaSelectionRows(content);"
        "    const selected=visibleSelectableRows(content).reduce(function(n,row){const cb=row.querySelector('.mediaSelect');return n+(cb&&cb.checked?1:0);},0);"
        "    const btn=content.querySelector('.deleteSelectedBtn');"
        "    if(btn){btn.disabled=selected===0;btn.textContent=selected?'Delete selected ('+selected+')':'Delete selected';}"
        "}"
        ""
        "function setDaySelection(button,selected){"
        "    const content=button.closest('.daycontent');"
        "    if(!content)return;"
        "    visibleSelectableRows(content).forEach(function(row){const cb=row.querySelector('.mediaSelect');if(cb)setMediaCheckboxState(cb,selected);});"
        "    content._sfSelectionAnchor=null;"
        "    updateDaySelection(content);"
        "}"
        ""
        "function visibleSelectableRows(content){"
        "    return Array.from(content.querySelectorAll('.recording[data-media]')).filter(function(row){return !row.hidden;});"
        "}"
        ""
        "function selectMediaRange(content,anchorRow,targetRow,selected){"
        "    const rows=visibleSelectableRows(content);"
        "    const a=rows.indexOf(anchorRow),b=rows.indexOf(targetRow);"
        "    if(a<0||b<0)return false;"
        "    const from=Math.min(a,b),to=Math.max(a,b);"
        "    for(let i=from;i<=to;i++){"
        "        const cb=rows[i].querySelector('.mediaSelect');"
        "        if(cb)setMediaCheckboxState(cb,selected);"
        "    }"
        "    return true;"
        "}"
        ""
        "let mediaMouseDrag=null;"
        "let suppressMediaSelectionClickUntil=0;"
        ""
        "function finishMediaMouseDrag(){"
        "    if(!mediaMouseDrag)return;"
        "    mediaMouseDrag=null;"
        "    document.body.classList.remove('mediaRangeSelecting');"
        "}"
        ""
        "document.addEventListener('mousedown',function(ev){"
        "    if(ev.button!==0)return;"
        "    const wrap=ev.target.closest?ev.target.closest('.mediaSelectWrap'):null;"
        "    if(!wrap)return;"
        "    const content=wrap.closest('.daycontent');"
        "    const row=wrap.closest('.recording[data-media]');"
        "    const cb=wrap.querySelector('.mediaSelect');"
        "    if(!content||!row||!cb||row.hidden)return;"
        "    ev.preventDefault();"
        "    try{cb.focus({preventScroll:true});}catch(e){try{cb.focus();}catch(ignore){}}"
        "    const selected=!cb.checked;"
        "    const anchor=content._sfSelectionAnchor;"
        "    if(ev.shiftKey&&anchor&&content.contains(anchor)){"
        "        if(!selectMediaRange(content,anchor,row,selected))setMediaCheckboxState(cb,selected);"
        "    }else{"
        "        setMediaCheckboxState(cb,selected);"
        "        content._sfSelectionAnchor=row;"
        "    }"
        "    mediaMouseDrag={content:content,selected:selected,lastRow:row};"
        "    suppressMediaSelectionClickUntil=Date.now()+700;"
        "    document.body.classList.add('mediaRangeSelecting');"
        "    updateDaySelection(content);"
        "},true);"
        ""
        "document.addEventListener('mouseover',function(ev){"
        "    if(!mediaMouseDrag)return;"
        "    const row=ev.target.closest?ev.target.closest('.recording[data-media]'):null;"
        "    if(!row||row.hidden||row===mediaMouseDrag.lastRow)return;"
        "    const content=row.closest('.daycontent');"
        "    if(content!==mediaMouseDrag.content)return;"
        "    const cb=row.querySelector('.mediaSelect');"
        "    if(!cb)return;"
        "    setMediaCheckboxState(cb,mediaMouseDrag.selected);"
        "    mediaMouseDrag.lastRow=row;"
        "    content._sfSelectionAnchor=row;"
        "    updateDaySelection(content);"
        "},true);"
        ""
        "document.addEventListener('mouseup',finishMediaMouseDrag,true);"
        "window.addEventListener('blur',finishMediaMouseDrag);"
        ""
        "document.addEventListener('click',function(ev){"
        "    const wrap=ev.target.closest?ev.target.closest('.mediaSelectWrap'):null;"
        "    if(wrap&&Date.now()<suppressMediaSelectionClickUntil){"
        "        ev.preventDefault();"
        "        ev.stopPropagation();"
        "    }"
        "},true);"

        "async function deleteSelectedMedia(button){"
            "const content=button.closest('.daycontent');"
            "const dayDetails=button.closest('details.day');"
            "if(!content||!dayDetails)return;"
            "const selected=visibleSelectableRows(content).map(function(row){return row.querySelector('.mediaSelect');}).filter(function(cb){return cb&&cb.checked;});"
            "if(!selected.length)return;"
            "if(!confirm(selected.length+' ausgewählte Datei(en) wirklich löschen?\\n\\nDiese Aktion kann nicht rückgängig gemacht werden.'))return;"
            "button.disabled=true;button.textContent='Lösche 0 / '+selected.length+' ...';"
            "let deleted=0;const failed=[];"
            "for(let i=0;i<selected.length;i++){"
                "const cb=selected[i];const path=cb.dataset.path||'';"
                "try{"
                    "const r=await fetch('/player_delete?path='+encodeURIComponent(path),{method:'POST',cache:'no-store'});"
                    "const t=await r.text();"
                    "if(!r.ok&&r.status!==202)throw new Error(t||('HTTP '+r.status));"
                    "const row=cb.closest('.recording[data-media]');if(row)row.remove();"
                    "deleted++;"
                "}catch(e){failed.push(path+' – '+e.message);cb.checked=false;}"
                "button.textContent='Lösche '+(i+1)+' / '+selected.length+' ...';"
            "}"
            "updateDayCount(dayDetails);"
            "updateDaySelection(content);"
            "if(content.querySelectorAll('.recording[data-media]').length===0){"
                "const empty=document.createElement('div');empty.className='recording emptyrecording';empty.textContent='Keine Aufnahmen oder Snapshots an diesem Tag.';content.appendChild(empty);"
            "}"
            "try{sessionStorage.setItem(CACHE_PREFIX+dayDetails.dataset.day,content.innerHTML);}catch(e){}"
            "if(failed.length){alert(deleted+' Datei(en) gelöscht. '+failed.length+' Datei(en) konnten nicht gelöscht werden.\\n\\n'+failed.join('\\n'));}"
        "}"

        "function openDayMedia(link){"
            "if(!link)return false;"
            "const content=link.closest('.daycontent');"
            "let mode=content&&content.dataset.mediaMode?content.dataset.mediaMode:'all';"
            "if(mode!=='videos'&&mode!=='images')mode='all';"
            "try{"
                "const u=new URL(link.getAttribute('href'),location.origin);"
                "u.searchParams.set('mode',mode);"
                "location.href=u.pathname+u.search;"
            "}catch(e){location.href=link.href;}"
            "return false;"
        "}"

        "function updateDayCount(d){"
            "const c=d.querySelector('.daycontent');"
            "const n=c.querySelectorAll('.recording[data-media]').length;"
            "const count=d.querySelector('.count');"
            "if(count){"
                "count.textContent=n===1?' (1 Datei)':' ('+n+' Dateien)';"
            "}"
        "}"

        "function restoreDay(d){"
            "const day=d.dataset.day;"
            "let cached=null;"
            "try{cached=sessionStorage.getItem(CACHE_PREFIX+day);}catch(e){}"
            "if(!cached)return false;"
            "const c=d.querySelector('.daycontent');"
            "c.innerHTML=cached;"
            "c.dataset.mediaMode='all';"
            "d.dataset.loaded='1';"
            "syncMediaSelectionRows(c);"
            "updateDayCount(d);"
            "updateDaySelection(c);"
            "return true;"
        "}"

        "async function loadDay(d){"
        "    if(d.dataset.loaded==='1')return;"
        "    if(restoreDay(d))return;"
        "    d.dataset.loaded='1';"
        "    const c=d.querySelector('.daycontent');"
        "    c.textContent='Verzeichnis wird gelesen ...';"
        "    try{"
        "        const r=await fetch('/files_day?day='+encodeURIComponent(d.dataset.day),{cache:'no-store'});"
        "        if(r.status===409){"
        "            showRecordingBlockedDialog();"
        "            const blocked=new Error('recording_active');"
        "            blocked.recordingBlocked=true;"
        "            throw blocked;"
        "        }"
        "        if(!r.ok)throw new Error('HTTP '+r.status);"
        "        const total=Math.max(0,Number(r.headers.get('X-Media-Count'))||0);"
        "        const marker='<!--SFCHUNK-->';"
        "        let loaded=0;"
        "        c.innerHTML='';"
        "        c.dataset.mediaMode='all';"
        ""
        "        function appendFragment(fragment){"
        "            if(!fragment)return;"
        "            c.insertAdjacentHTML('beforeend',fragment);"
        "            const last=c.lastElementChild;"
        "            if(last&&last.matches&&last.matches('.recording[data-media]')){"
        "                loaded++;"
        "                const mode=c.dataset.mediaMode||'all';"
        "                last.hidden=(mode==='videos'&&last.dataset.media!=='video')||(mode==='images'&&last.dataset.media!=='image');"
        "                const cb=last.querySelector('.mediaSelect');"
        "                if(cb)setMediaCheckboxState(cb,cb.checked);"
        "            }"
        "            const count=d.querySelector('.count');"
        "            if(count&&total>0)count.textContent=' ('+loaded+' / '+total+' geladen)';"
        "        }"
        ""
        "        if(r.body&&r.body.getReader&&window.TextDecoder){"
        "            const reader=r.body.getReader();"
        "            const decoder=new TextDecoder();"
        "            let pending='';"
        "            while(true){"
        "                const part=await reader.read();"
        "                if(part.value)pending+=decoder.decode(part.value,{stream:!part.done});"
        "                let split;"
        "                while((split=pending.indexOf(marker))>=0){"
        "                    appendFragment(pending.slice(0,split));"
        "                    pending=pending.slice(split+marker.length);"
        "                }"
        "                if(part.done)break;"
        "            }"
        "            if(pending.length)appendFragment(pending);"
        "        }else{"
        "            const t=await r.text();"
        "            t.split(marker).forEach(appendFragment);"
        "        }"
        ""
        "        if(c.querySelectorAll('.recording[data-media]').length===0&&!c.querySelector('.emptyrecording')){"
        "            c.innerHTML=\"<div class='recording emptyrecording'>Keine Aufnahmen oder Snapshots an diesem Tag.</div>\";"
        "        }"
        "        updateDayCount(d);"
        "        updateDaySelection(c);"
        "        try{sessionStorage.setItem(CACHE_PREFIX+d.dataset.day,c.innerHTML);}catch(e){}"
        "    }catch(e){"
        "        d.dataset.loaded='0';"
        "        if(e&&e.recordingBlocked){"
        "            c.textContent='Wird nach Aufnahmeende neu geladen ...';"
        "            return;"
        "        }"
        "        c.textContent='Fehler beim Laden: '+e.message;"
        "    }"
        "}"

        "const dayElements=Array.from(document.querySelectorAll('details.day'));"

        "dayElements.forEach(function(d){"
            "d.addEventListener('toggle',function(){"
                "if(d.open){"
                    "dayElements.forEach(function(other){"
                        "if(other!==d&&other.open)other.open=false;"
                    "});"
                    "try{sessionStorage.setItem(OPEN_DAY_KEY,d.dataset.day);}catch(e){}"
                    "loadDay(d);"
                "}else{"
                    "try{"
                        "if(sessionStorage.getItem(OPEN_DAY_KEY)===d.dataset.day){"
                            "sessionStorage.removeItem(OPEN_DAY_KEY);"
                        "}"
                    "}catch(e){}"
                "}"
            "});"
            "restoreDay(d);"
        "});"

        // Only restore the open day when this browser session still
        // belongs to the same ESP32 boot. After reset/reboot all days
        // intentionally start collapsed.
        "if(sameBoot){"
            "let openDay=null;"
            "try{openDay=sessionStorage.getItem(OPEN_DAY_KEY);}catch(e){}"
            "if(openDay){"
                "const d=dayElements.find(function(x){return x.dataset.day===openDay;});"
                "if(d){"
                    "d.open=true;"
                    "loadDay(d);"
                "}"
            "}"
        "}"

        "window.addEventListener('pageshow',function(){"
            "let dirtyDay=null;"
            "try{dirtyDay=sessionStorage.getItem('recordings.annotationDirtyDay');}catch(e){}"
            "if(!dirtyDay)return;"
            "try{sessionStorage.removeItem(CACHE_PREFIX+dirtyDay);sessionStorage.removeItem('recordings.annotationDirtyDay');}catch(e){}"
            "const d=dayElements.find(function(x){return x.dataset.day===dirtyDay;});"
            "if(!d)return;"
            "d.dataset.loaded='0';"
            "const c=d.querySelector('.daycontent');"
            "if(c)c.innerHTML='';"
            "if(d.open)loadDay(d);"
        "});"
        "</script>";


    html +=
        "<br><a href='/'><button>Back</button></a>";

    html +=
        htmlFooter();


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


// -------------------------------------------------------------
// AUDIO CAPTURE DIAGNOSTIC
// -------------------------------------------------------------

static bool audioPostedHardwareDiffers()
{
    String postedAudioSource = server.arg("audio_source");
    postedAudioSource.trim();
    postedAudioSource.toLowerCase();

    String postedAudioBackend = server.arg("audio_backend");
    postedAudioBackend.trim();
    postedAudioBackend.toLowerCase();

    String postedAudioI2sSlot = server.arg("audio_i2s_slot");
    postedAudioI2sSlot.trim();
    postedAudioI2sSlot.toLowerCase();

    return
        (
            server.hasArg("audio_expert_mode") &&
            (server.arg("audio_expert_mode").toInt() ? 1 : 0) !=
                cfg_audio_expert_mode
        ) ||
        (
            server.hasArg("audio_source") &&
            postedAudioSource != cfg_audio_source
        ) ||
        (
            server.hasArg("audio_backend") &&
            postedAudioBackend != cfg_audio_backend
        ) ||
        (
            server.hasArg("audio_pdm_clk_pin") &&
            server.arg("audio_pdm_clk_pin").toInt() != cfg_audio_pdm_clk_pin
        ) ||
        (
            server.hasArg("audio_pdm_data_pin") &&
            server.arg("audio_pdm_data_pin").toInt() != cfg_audio_pdm_data_pin
        ) ||
        (
            server.hasArg("audio_i2s_bclk_pin") &&
            server.arg("audio_i2s_bclk_pin").toInt() != cfg_audio_i2s_bclk_pin
        ) ||
        (
            server.hasArg("audio_i2s_ws_pin") &&
            server.arg("audio_i2s_ws_pin").toInt() != cfg_audio_i2s_ws_pin
        ) ||
        (
            server.hasArg("audio_i2s_data_pin") &&
            server.arg("audio_i2s_data_pin").toInt() != cfg_audio_i2s_data_pin
        ) ||
        (
            server.hasArg("audio_i2s_mclk_pin") &&
            server.arg("audio_i2s_mclk_pin").toInt() != cfg_audio_i2s_mclk_pin
        ) ||
        (
            server.hasArg("audio_i2s_slot") &&
            postedAudioI2sSlot != cfg_audio_i2s_slot
        );
}


static AudioFormat audioPostedFormat()
{
    AudioFormat format = {};
    format.sampleRate =
        (uint32_t)server.arg("audio_sample_rate").toInt();
    format.bitsPerSample =
        (uint16_t)server.arg("audio_bits_per_sample").toInt();
    format.channels =
        (uint8_t)server.arg("audio_channels").toInt();
    return format;
}


static void handleAudioTestRecord()
{
    if (rejectWhileRecording("audio test"))
        return;

    if (!sdReady) {
        server.send(
            503,
            "text/plain; charset=utf-8",
            "SD storage is not available"
        );
        return;
    }

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage maintenance is already active"
        );
        return;
    }

    // Hardware routing is intentionally persistent and explicit. Do not
    // silently test a different microphone when the operator changed Expert
    // fields in the form but has not saved them yet.
    if (audioPostedHardwareDiffers()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            tr(UI_AUDIO_SAVE_HARDWARE_NOTE)
        );
        return;
    }

    AudioFormat format = audioPostedFormat();

    String formatError;

    if (!audioCaptureFormatSupported(format, formatError)) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Unsupported audio format: " + formatError
        );
        return;
    }

    // The diagnostic owns storage for the short capture so no new recording can
    // start while the WAV writer and microphone exercise the storage path.
    bool previousRecordingBlock = g_recordingStartBlocked;
    g_recordingStartBlocked = true;
    g_storageLocked = true;

    AudioWavResult result = {};
    String error;
    const String testPath = "/audio_test.wav";

    bool ok = audioWavRecordTest(
        testPath,
        10000UL,
        format,
        cfg_recording_encryption != 0,
        result,
        error,
        serviceWebLongOperation
    );

    g_storageLocked = false;
    g_recordingStartBlocked = previousRecordingBlock;

    String html = htmlHeader();
    html += "<h2>Audio diagnostic</h2>";

    if (!ok) {
        html +=
            "<section class='settings-section' style='border-left:5px solid #b91c1c'>"
            "<h3>Test failed</h3><p>" +
            htmlEscape(error) +
            "</p></section>";
    } else {
        html +=
            "<section class='settings-section' style='border-left:5px solid #15803d'>"
            "<h3>10 s WAV test completed</h3>"
            "<p><span class='status-pill ok'>AUDIO OK</span></p>";

        html += "Backend: <b>" +
                htmlEscape(String(audioCaptureBackendName())) +
                "</b><br>";
        html += "Format: <b>" +
                String((unsigned long)format.sampleRate) +
                " Hz / " +
                String((unsigned int)format.bitsPerSample) +
                " bit / " +
                String((unsigned int)format.channels) +
                (format.channels == 1 ? " channel" : " channels") +
                "</b><br>";
        html += "PCM: " +
                String((unsigned long)result.pcmBytes) +
                " bytes<br>";
        html += "Capture time: " +
                String((unsigned long)result.captureMs) +
                " ms<br>";
        html += "Dropped audio: <b>" +
                String((unsigned long)result.droppedBytes) +
                " bytes</b><br>";
        html += "PSRAM buffer high-water: " +
                String((unsigned long)result.bufferHighWater) +
                " / " +
                String((unsigned long)result.bufferCapacity) +
                " bytes<br>";

        if (format.bitsPerSample == 16) {
            html += "Signal peak: " +
                    String((long)result.peakAbs16) +
                    " / 32768<br>";
            html += "Signal RMS: " +
                    String(result.rms16, 1) +
                    " / 32768<br>";
        }

        html +=
            "<p><a class='button primary' href='/file?path=%2Faudio_test.wav'>"
            "WAV herunterladen</a></p>"
            "<p class='muted'>" +
            htmlText(UI_AUDIO_TEST_HELP) +
            " " +
            htmlText(UI_AUDIO_ENCRYPTION_NOTE) +
            "</p>"
            "</section>";
    }

    html += "<p><a href='/config'><button>Back to Config</button></a></p>";
    html += htmlFooter();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}


static void handleAudioBenchmark()
{
    if (rejectWhileRecording("audio benchmark"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage maintenance is already active"
        );
        return;
    }

    if (audioPostedHardwareDiffers()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            tr(UI_AUDIO_SAVE_HARDWARE_NOTE)
        );
        return;
    }

    AudioFormat format = audioPostedFormat();
    String formatError;

    if (!audioCaptureFormatSupported(format, formatError)) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Unsupported audio format: " + formatError
        );
        return;
    }

    static const uint32_t BENCHMARK_DURATION_MS = 10000UL;
    static const size_t BENCHMARK_READ_BYTES = 8U * 1024U;

    uint32_t internalBefore =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    uint32_t psramBefore =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    uint8_t *buffer =
        (uint8_t *)heap_caps_malloc(
            BENCHMARK_READ_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );

    if (!buffer) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            "Audio benchmark buffer allocation failed"
        );
        return;
    }

    uint32_t internalMin =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    uint32_t psramMin =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    bool previousRecordingBlock = g_recordingStartBlocked;
    g_recordingStartBlocked = true;

    String error;
    bool started = audioCaptureStart(format, error);

    uint64_t deliveredBytes = 0;
    uint32_t emptyReads = 0;
    uint32_t maxDrainGapMs = 0;
    uint32_t benchmarkStartMs = millis();
    uint32_t lastDrainMs = benchmarkStartMs;

    if (started) {
        while (
            (uint32_t)(millis() - benchmarkStartMs) <
                BENCHMARK_DURATION_MS
        ) {
            size_t got =
                audioCaptureRead(
                    buffer,
                    BENCHMARK_READ_BYTES,
                    50UL
                );

            uint32_t now = millis();
            uint32_t gap = now - lastDrainMs;
            if (gap > maxDrainGapMs)
                maxDrainGapMs = gap;
            lastDrainMs = now;

            if (got)
                deliveredBytes += (uint64_t)got;
            else
                emptyReads++;

            uint32_t internalNow =
                (uint32_t)heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
                );
            uint32_t psramNow =
                (uint32_t)heap_caps_get_free_size(
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                );

            if (internalNow < internalMin)
                internalMin = internalNow;
            if (psramNow < psramMin)
                psramMin = psramNow;

            serviceWebLongOperation();
        }
    }

    uint32_t elapsedMs =
        started
        ? (uint32_t)(millis() - benchmarkStartMs)
        : 0;

    AudioCaptureStats stats = {};
    if (started)
        stats = audioCaptureStats();

    if (started)
        audioCaptureStop();

    g_recordingStartBlocked = previousRecordingBlock;

    free(buffer);

    uint32_t internalAfter =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
    uint32_t psramAfter =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    String html = htmlHeader();
    html += "<h2>" + htmlText(UI_AUDIO_BENCHMARK) + "</h2>";

    if (!started) {
        html +=
            "<section class='settings-section' style='border-left:5px solid #b91c1c'>"
            "<h3>Test failed</h3><p>" +
            htmlEscape(error) +
            "</p></section>";
    } else {
        uint64_t bytesPerSecond =
            (uint64_t)format.sampleRate *
            (uint64_t)format.channels *
            (uint64_t)(format.bitsPerSample / 8U);

        uint64_t expectedBytes =
            elapsedMs > 0
            ? (bytesPerSecond * (uint64_t)elapsedMs) / 1000ULL
            : 0;

        float deliveredPct =
            expectedBytes > 0
            ? (100.0f * (float)deliveredBytes / (float)expectedBytes)
            : 0.0f;

        float highWaterPct =
            stats.bufferCapacity > 0
            ? (100.0f * (float)stats.bufferHighWater / (float)stats.bufferCapacity)
            : 0.0f;

        float droppedPct =
            stats.bytesCaptured > 0
            ? (100.0f * (float)stats.bytesDropped / (float)stats.bytesCaptured)
            : 0.0f;

        enum AudioLoadLevel {
            AUDIO_LOAD_GREEN = 0,
            AUDIO_LOAD_ORANGE = 1,
            AUDIO_LOAD_RED = 2
        };

        static const float AUDIO_DELIVERY_GREEN_MIN_PCT = 98.0f;
        static const float AUDIO_DELIVERY_RED_BELOW_PCT = 95.0f;
        static const float AUDIO_DROPS_RED_FROM_PCT = 1.0f;
        static const float AUDIO_BUFFER_ORANGE_FROM_PCT = 50.0f;
        static const float AUDIO_BUFFER_RED_FROM_PCT = 90.0f;
        static const uint32_t AUDIO_DRAIN_ORANGE_ABOVE_MS = 500UL;
        static const uint32_t AUDIO_DRAIN_RED_ABOVE_MS = 2000UL;
        static const uint32_t AUDIO_HEAP_ORANGE_BELOW_BYTES = 64UL * 1024UL;
        static const uint32_t AUDIO_HEAP_RED_BELOW_BYTES = 32UL * 1024UL;
        static const uint32_t AUDIO_PSRAM_ORANGE_BELOW_BYTES = 512UL * 1024UL;
        static const uint32_t AUDIO_PSRAM_RED_BELOW_BYTES = 256UL * 1024UL;

        auto maxLevel = [](int a, int b) -> int {
            return a > b ? a : b;
        };

        int deliveryLevel =
            deliveredPct < AUDIO_DELIVERY_RED_BELOW_PCT
            ? AUDIO_LOAD_RED
            : deliveredPct < AUDIO_DELIVERY_GREEN_MIN_PCT
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int dropsLevel =
            droppedPct >= AUDIO_DROPS_RED_FROM_PCT
            ? AUDIO_LOAD_RED
            : stats.bytesDropped > 0
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int bufferLevel =
            highWaterPct >= AUDIO_BUFFER_RED_FROM_PCT
            ? AUDIO_LOAD_RED
            : highWaterPct >= AUDIO_BUFFER_ORANGE_FROM_PCT
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int drainLevel =
            maxDrainGapMs > AUDIO_DRAIN_RED_ABOVE_MS
            ? AUDIO_LOAD_RED
            : maxDrainGapMs > AUDIO_DRAIN_ORANGE_ABOVE_MS
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int heapLevel =
            internalMin < AUDIO_HEAP_RED_BELOW_BYTES
            ? AUDIO_LOAD_RED
            : internalMin < AUDIO_HEAP_ORANGE_BELOW_BYTES
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int psramLevel =
            psramMin < AUDIO_PSRAM_RED_BELOW_BYTES
            ? AUDIO_LOAD_RED
            : psramMin < AUDIO_PSRAM_ORANGE_BELOW_BYTES
              ? AUDIO_LOAD_ORANGE
              : AUDIO_LOAD_GREEN;

        int overallLevel = AUDIO_LOAD_GREEN;
        overallLevel = maxLevel(overallLevel, deliveryLevel);
        overallLevel = maxLevel(overallLevel, dropsLevel);
        overallLevel = maxLevel(overallLevel, bufferLevel);
        overallLevel = maxLevel(overallLevel, drainLevel);
        overallLevel = maxLevel(overallLevel, heapLevel);
        overallLevel = maxLevel(overallLevel, psramLevel);

        auto levelClass = [](int level) -> const char * {
            return
                level == AUDIO_LOAD_RED
                ? "danger"
                : level == AUDIO_LOAD_ORANGE
                  ? "warn"
                  : "ok";
        };

        auto levelTextId = [](int level) -> UiTextId {
            return
                level == AUDIO_LOAD_RED
                ? UI_AUDIO_LOAD_RED
                : level == AUDIO_LOAD_ORANGE
                  ? UI_AUDIO_LOAD_ORANGE
                  : UI_AUDIO_LOAD_GREEN;
        };

        auto levelPill = [&](int level) -> String {
            return
                "<span class='status-pill " +
                String(levelClass(level)) +
                "'>" +
                htmlText(levelTextId(level)) +
                "</span>";
        };

        const char *border =
            overallLevel == AUDIO_LOAD_RED
            ? "#b91c1c"
            : overallLevel == AUDIO_LOAD_ORANGE
              ? "#c47a00"
              : "#15803d";

        UiTextId verdictId =
            overallLevel == AUDIO_LOAD_RED
            ? UI_AUDIO_LOAD_NOT_RECOMMENDED
            : overallLevel == AUDIO_LOAD_ORANGE
              ? UI_AUDIO_LOAD_CHECK
              : UI_AUDIO_LOAD_GOOD;

        html +=
            "<section class='settings-section' style='border-left:5px solid " +
            String(border) +
            "'><h3>10 s capture-only result</h3>"
            "<p>" + levelPill(overallLevel) +
            " <b>" + htmlText(verdictId) + "</b></p>"
            "<p class='muted'>" + htmlText(UI_AUDIO_LOAD_SCOPE) + "</p>";

        html +=
            "<div style='line-height:1.9'>" +
            levelPill(deliveryLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_DELIVERY) +
            ":</b> " + String(deliveredPct, 1) +
            "% <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " &ge; " + String(AUDIO_DELIVERY_GREEN_MIN_PCT, 0) +
            "%, " + htmlText(UI_AUDIO_LOAD_ORANGE) + " " +
            String(AUDIO_DELIVERY_RED_BELOW_PCT, 0) + "..&lt;" +
            String(AUDIO_DELIVERY_GREEN_MIN_PCT, 0) +
            "%, " + htmlText(UI_AUDIO_LOAD_RED) + " &lt;" +
            String(AUDIO_DELIVERY_RED_BELOW_PCT, 0) + "%)</small><br>" +

            levelPill(dropsLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_DROPS) +
            ":</b> " + String((unsigned long)stats.bytesDropped) +
            " B (" + String(droppedPct, 3) +
            "%) <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " = 0, " + htmlText(UI_AUDIO_LOAD_ORANGE) +
            " &gt;0..&lt;" + String(AUDIO_DROPS_RED_FROM_PCT, 0) +
            "%, " + htmlText(UI_AUDIO_LOAD_RED) + " &ge;" +
            String(AUDIO_DROPS_RED_FROM_PCT, 0) + "%)</small><br>" +

            levelPill(bufferLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_BUFFER) +
            ":</b> " + String(highWaterPct, 1) +
            "% <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " &lt;" + String(AUDIO_BUFFER_ORANGE_FROM_PCT, 0) +
            "%, " + htmlText(UI_AUDIO_LOAD_ORANGE) + " " +
            String(AUDIO_BUFFER_ORANGE_FROM_PCT, 0) + "..&lt;" +
            String(AUDIO_BUFFER_RED_FROM_PCT, 0) +
            "%, " + htmlText(UI_AUDIO_LOAD_RED) + " &ge;" +
            String(AUDIO_BUFFER_RED_FROM_PCT, 0) + "%)</small><br>" +

            levelPill(drainLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_DRAIN) +
            ":</b> " + String((unsigned long)maxDrainGapMs) +
            " ms <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " &le;" + String((unsigned long)AUDIO_DRAIN_ORANGE_ABOVE_MS) +
            " ms, " + htmlText(UI_AUDIO_LOAD_ORANGE) + " &gt;" +
            String((unsigned long)AUDIO_DRAIN_ORANGE_ABOVE_MS) + ".." +
            String((unsigned long)AUDIO_DRAIN_RED_ABOVE_MS) +
            " ms, " + htmlText(UI_AUDIO_LOAD_RED) + " &gt;" +
            String((unsigned long)AUDIO_DRAIN_RED_ABOVE_MS) + " ms)</small><br>" +

            levelPill(heapLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_HEAP) +
            ":</b> " + String((double)internalMin / 1024.0, 1) +
            " KiB min <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " &ge;" + String((unsigned long)(AUDIO_HEAP_ORANGE_BELOW_BYTES / 1024UL)) +
            " KiB, " + htmlText(UI_AUDIO_LOAD_ORANGE) + " " +
            String((unsigned long)(AUDIO_HEAP_RED_BELOW_BYTES / 1024UL)) + "..&lt;" +
            String((unsigned long)(AUDIO_HEAP_ORANGE_BELOW_BYTES / 1024UL)) +
            " KiB, " + htmlText(UI_AUDIO_LOAD_RED) + " &lt;" +
            String((unsigned long)(AUDIO_HEAP_RED_BELOW_BYTES / 1024UL)) + " KiB)</small><br>" +

            levelPill(psramLevel) + " <b>" +
            htmlText(UI_AUDIO_LOAD_PSRAM) +
            ":</b> " + String((double)psramMin / 1024.0, 1) +
            " KiB min <small class='muted'>(" + htmlText(UI_AUDIO_LOAD_GREEN) +
            " &ge;" + String((unsigned long)(AUDIO_PSRAM_ORANGE_BELOW_BYTES / 1024UL)) +
            " KiB, " + htmlText(UI_AUDIO_LOAD_ORANGE) + " " +
            String((unsigned long)(AUDIO_PSRAM_RED_BELOW_BYTES / 1024UL)) + "..&lt;" +
            String((unsigned long)(AUDIO_PSRAM_ORANGE_BELOW_BYTES / 1024UL)) +
            " KiB, " + htmlText(UI_AUDIO_LOAD_RED) + " &lt;" +
            String((unsigned long)(AUDIO_PSRAM_RED_BELOW_BYTES / 1024UL)) + " KiB)</small>" +
            "</div>";

        html +=
            "<details style='margin-top:14px'><summary>" +
            htmlText(UI_AUDIO_LOAD_TECH_DETAILS) +
            "</summary><div style='margin-top:10px'>";

        html += "Backend: <b>" +
                htmlEscape(String(audioCaptureBackendName())) +
                "</b><br>";
        html += "Format: <b>" +
                String((unsigned long)format.sampleRate) + " Hz / " +
                String((unsigned int)format.bitsPerSample) + " bit / " +
                String((unsigned int)format.channels) +
                (format.channels == 1 ? " channel" : " channels") +
                "</b><br>";
        html += "Nominal PCM rate: <b>" +
                String((double)bytesPerSecond / 1024.0, 1) +
                " KiB/s</b><br>";
        html += "Elapsed: " + String((unsigned long)elapsedMs) + " ms<br>";
        html += "Captured: " +
                String((unsigned long)stats.bytesCaptured) + " bytes<br>";
        html += "Delivered/drained: " +
                String((unsigned long)deliveredBytes) + " bytes (" +
                String(deliveredPct, 1) + "% of nominal)<br>";
        html += "Dropped: <b>" +
                String((unsigned long)stats.bytesDropped) +
                " bytes</b><br>";
        html += "PSRAM ring high-water: " +
                String((unsigned long)stats.bufferHighWater) + " / " +
                String((unsigned long)stats.bufferCapacity) + " bytes (" +
                String(highWaterPct, 1) + "%)<br>";
        html += "Max drain-loop gap: " +
                String((unsigned long)maxDrainGapMs) + " ms<br>";
        html += "Empty reads: " +
                String((unsigned long)emptyReads) + "<br><br>";

        html += "Internal heap free before/min/after: <b>" +
                String((unsigned long)internalBefore) + " / " +
                String((unsigned long)internalMin) + " / " +
                String((unsigned long)internalAfter) + " B</b><br>";
        html += "PSRAM free before/min/after: <b>" +
                String((unsigned long)psramBefore) + " / " +
                String((unsigned long)psramMin) + " / " +
                String((unsigned long)psramAfter) + " B</b><br>";

        html +=
            "</div></details><p class='muted'>" +
            htmlText(UI_AUDIO_BENCHMARK_HELP) +
            "</p></section>";
    }

    html += "<p><a href='/config'><button>Back to Config</button></a></p>";
    html += htmlFooter();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}


static void handleFile()
{
    if (rejectWhileRecording("file download"))
        return;

    String path = server.arg("path");

    if (!path.length() ||
        !path.startsWith("/") ||
        path.indexOf("..") >= 0) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid path"
        );

        return;
    }

    String lowerPath = path;
    lowerPath.toLowerCase();

    bool allowedFile =
        lowerPath.endsWith(".avi") ||
        lowerPath.endsWith(".mkv") ||
        lowerPath.endsWith(".srt") ||
        lowerPath.endsWith(".wav") ||
        lowerPath.endsWith(".jpg") ||
        lowerPath.endsWith(".jpeg");

    if (!allowedFile) {

        server.send(
            403,
            "text/plain; charset=utf-8",
            "Only AVI, MKV, WAV, SRT and JPEG files are allowed"
        );

        return;
    }


    // Release any old playback session before allocating encrypted-read
    // buffers for a potentially long browser download.
    webPlayerStop();

    RecordingStorageFile f;

    if (!f.openRead(path) || f.isDirectory()) {

        f.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "File not found or cannot be decrypted"
        );

        return;
    }


    // ---------------------------------------------------------
    // Tatsächlichen Dateinamen ermitteln
    // z.B. /20260907/012345.avi -> 012345.avi
    // ---------------------------------------------------------

    int slashPos = path.lastIndexOf('/');

    String downloadName =
        slashPos >= 0
        ? path.substring(slashPos + 1)
        : path;


    // Sicherheitsbereinigung für HTTP Header
    downloadName.replace("\"", "_");
    downloadName.replace("\r", "_");
    downloadName.replace("\n", "_");


    // ---------------------------------------------------------
    // Download Header
    // ---------------------------------------------------------

    bool inlineDisplay =
        (
            lowerPath.endsWith(".jpg") ||
            lowerPath.endsWith(".jpeg")
        ) &&
        server.hasArg("inline") &&
        server.arg("inline") == "1";

    server.sendHeader(
        "Content-Disposition",
        String(inlineDisplay ? "inline" : "attachment") +
        "; filename=\"" +
        downloadName +
        "\""
    );

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );


    // ---------------------------------------------------------
    // Datei senden
    // ---------------------------------------------------------

    const char *contentType =
        lowerPath.endsWith(".mkv")
        ? "video/x-matroska"
        : (
            lowerPath.endsWith(".wav")
            ? "audio/wav"
            : (
                lowerPath.endsWith(".srt")
                ? "application/x-subrip"
                : (
                    lowerPath.endsWith(".jpg") ||
                    lowerPath.endsWith(".jpeg")
                    ? "image/jpeg"
                    : "video/x-msvideo"
                )
            )
        );

    uint64_t logicalSize =
        (uint64_t)f.size();

    static const size_t DOWNLOAD_BUFFER_SIZE =
        8U * 1024U;

    uint8_t *downloadBuffer =
        (uint8_t *)malloc(DOWNLOAD_BUFFER_SIZE);

    if (!downloadBuffer) {
        f.close();
        server.send(
            503,
            "text/plain; charset=utf-8",
            "Download buffer unavailable"
        );
        return;
    }

    server.setContentLength(
        (size_t)logicalSize
    );

    server.send(
        200,
        contentType,
        ""
    );

    uint64_t sent = 0;
    bool ok = true;

    while (sent < logicalSize) {
        size_t wanted =
            (logicalSize - sent) < DOWNLOAD_BUFFER_SIZE
            ? (size_t)(logicalSize - sent)
            : DOWNLOAD_BUFFER_SIZE;

        size_t got =
            f.read(
                downloadBuffer,
                wanted
            );

        if (got == 0 ||
            !zipClientWriteAll(
                downloadBuffer,
                got
            )) {
            ok = false;
            break;
        }

        sent +=
            (uint64_t)got;
    }

    free(downloadBuffer);
    f.close();

    if (!ok || sent != logicalSize) {
        Serial.printf(
            "Download incomplete: %llu / %llu bytes\n",
            (unsigned long long)sent,
            (unsigned long long)logicalSize
        );

        server.client().stop();
    }
}


// -------------------------------------------------------------
// LICENSE
// -------------------------------------------------------------

static UiTextId licenseStatusUiTextId(
    LicenseStatus status
)
{
    switch (status) {
        case LICENSE_STATUS_VALID:
            return UI_LICENSE_STATUS_VALID;
        case LICENSE_STATUS_MISSING:
            return UI_LICENSE_STATUS_MISSING;
        case LICENSE_STATUS_INVALID_FORMAT:
            return UI_LICENSE_STATUS_INVALID_FORMAT;
        case LICENSE_STATUS_INVALID_SIGNATURE:
            return UI_LICENSE_STATUS_INVALID_SIGNATURE;
        case LICENSE_STATUS_WRONG_DEVICE:
            return UI_LICENSE_STATUS_WRONG_DEVICE;
        case LICENSE_STATUS_WRONG_PRODUCT:
            return UI_LICENSE_STATUS_WRONG_PRODUCT;
        case LICENSE_STATUS_UNSUPPORTED_VERSION:
            return UI_LICENSE_STATUS_UNSUPPORTED_VERSION;
        case LICENSE_STATUS_EXPIRED:
            return UI_LICENSE_STATUS_EXPIRED;
        case LICENSE_STATUS_TIME_UNAVAILABLE:
            return UI_LICENSE_STATUS_TIME_UNAVAILABLE;
        case LICENSE_STATUS_STORAGE_ERROR:
        default:
            return UI_LICENSE_STATUS_STORAGE_ERROR;
    }
}

static const char *licenseStatusCssClass(
    LicenseStatus status
)
{
    if (status == LICENSE_STATUS_VALID)
        return "ok";

    if (status == LICENSE_STATUS_MISSING)
        return "warn";

    return "danger";
}

static String licenseFeatureText()
{
    if (!licenseIsValid())
        return htmlText(UI_LICENSE_NONE);

    uint32_t flags =
        licenseFeatureFlags();

    String text;

    auto appendFeature = [&text](UiTextId id) {
        if (text.length())
            text += " &middot; ";
        text += htmlText(id);
    };

    if (flags & LICENSE_FEATURE_RECORDING)
        appendFeature(UI_LICENSE_FEATURE_RECORDING);

    if (flags & LICENSE_FEATURE_RADAR)
        appendFeature(UI_LICENSE_FEATURE_RADAR);

    if (flags & LICENSE_FEATURE_SYNC_API)
        appendFeature(UI_LICENSE_FEATURE_SYNC_API);

    if (flags & LICENSE_FEATURE_ADVANCED_ANALYTICS)
        appendFeature(UI_LICENSE_FEATURE_ANALYTICS);

    if (!text.length())
        text = htmlText(UI_LICENSE_NONE);

    return text;
}

static void handleLicensePage()
{
    LicenseStatus status =
        licenseStatus();

    String hardwareId =
        licenseHardwareId();

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div><h2>" +
        htmlText(UI_LICENSE_TITLE) +
        "</h2><p>" +
        htmlText(UI_LICENSE_SUBTITLE) +
        "</p></div></div>";

    if (server.hasArg("notice")) {
        String notice =
            server.arg("notice");

        if (notice == "installed") {
            html +=
                "<div class='flash-notice success'><strong>" +
                htmlText(UI_LICENSE_NOTICE_INSTALLED) +
                "</strong></div>";

        } else if (notice == "removed") {
            html +=
                "<div class='flash-notice success'><strong>" +
                htmlText(UI_LICENSE_NOTICE_REMOVED) +
                "</strong></div>";

        } else if (notice == "activation_failed") {
            LicenseStatus failedStatus =
                LICENSE_STATUS_INVALID_FORMAT;

            if (server.hasArg("status")) {
                int value =
                    server.arg("status").toInt();

                if (
                    value >= LICENSE_STATUS_MISSING &&
                    value <= LICENSE_STATUS_STORAGE_ERROR
                ) {
                    failedStatus =
                        (LicenseStatus)value;
                }
            }

            html +=
                "<div class='flash-notice error'><strong>" +
                htmlText(UI_LICENSE_ACTIVATION_FAILED) +
                "</strong><span>" +
                htmlText(
                    licenseStatusUiTextId(
                        failedStatus
                    )
                ) +
                "</span></div>";

        } else if (notice == "remove_failed") {
            html +=
                "<div class='flash-notice error'><strong>" +
                htmlText(UI_LICENSE_REMOVE_FAILED) +
                "</strong></div>";
        }
    }

    html +=
        "<section class='settings-section'>"
        "<h3>" + htmlText(UI_LICENSE_STATUS) + "</h3>"
        "<div class='dashboard-grid'>";

    html +=
        "<div class='dash-card'><div class='card-label'>" +
        htmlText(UI_LICENSE_STATUS) +
        "</div><div class='card-value'><span class='status-pill " +
        String(licenseStatusCssClass(status)) +
        "'>" +
        htmlText(
            licenseStatusUiTextId(
                status
            )
        ) +
        "</span></div></div>";

    html +=
        "<div class='dash-card'><div class='card-label'>" +
        htmlText(UI_LICENSE_HARDWARE_ID) +
        "</div><div class='card-value' style='font-size:.96rem'><code id='licenseHardwareId'>" +
        htmlEscape(hardwareId) +
        "</code><div style='margin-top:8px'><button type='button' id='licenseHardwareCopy'>" +
        htmlText(UI_LICENSE_HARDWARE_COPY) +
        "</button></div></div></div>";

    html +=
        "<div class='dash-card'><div class='card-label'>" +
        htmlText(UI_LICENSE_EDITION) +
        "</div><div class='card-value'>" +
        htmlEscape(
            String(licenseEditionName())
        ) +
        "</div></div>";

    String currentLicenseId =
        licenseId();

    html +=
        "<div class='dash-card'><div class='card-label'>" +
        htmlText(UI_LICENSE_LICENSE_ID) +
        "</div><div class='card-value' style='font-size:.86rem'><code>" +
        htmlEscape(
            currentLicenseId.length()
            ? currentLicenseId
            : String("--")
        ) +
        "</code></div></div>";

    html +=
        "</div>";

    if (licenseIsValid()) {
        String issued =
            licenseIssuedDateText();

        String expiry =
            licenseExpiryDateText();

        html +=
            "<p><b>" +
            htmlText(UI_LICENSE_ISSUED) +
            ":</b> " +
            htmlEscape(
                issued.length()
                ? issued
                : String("--")
            ) +
            " &nbsp; <b>" +
            htmlText(UI_LICENSE_VALIDITY) +
            ":</b> " +
            (
                expiry == "never"
                ? htmlText(UI_LICENSE_PERMANENT)
                : htmlEscape(expiry)
            ) +
            "</p>";

        html +=
            "<p><b>" +
            htmlText(UI_LICENSE_FEATURES) +
            ":</b> " +
            licenseFeatureText() +
            "</p>";
    }

    html +=
        "</section>";

    html +=
        "<section class='settings-section'>"
        "<h3>" + htmlText(UI_LICENSE_ACTIVATION_TITLE) + "</h3>"
        "<p class='muted'>" + htmlText(UI_LICENSE_ACTIVATION_HELP) + "</p>"
        "<form method='POST' action='/license_activate'>"
        "<input type='hidden' name='token' value='" +
        String(webBootSessionId) +
        "'>"
        "<label for='licenseActivationCode'><b>" +
        htmlText(UI_LICENSE_ACTIVATION_CODE) +
        "</b></label><br>"
        "<textarea id='licenseActivationCode' name='activation_code' rows='5' "
        "maxlength='512' required autocomplete='off' autocapitalize='off' "
        "spellcheck='false' style='width:100%;font-family:monospace;resize:vertical' "
        "placeholder='SF1:...'></textarea>"
        "<div style='margin-top:10px'><button class='primary' type='submit'>" +
        htmlText(UI_LICENSE_ACTIVATE_BUTTON) +
        "</button></div></form>";

    if (status != LICENSE_STATUS_MISSING) {
        html +=
            "<form method='POST' action='/license_remove' style='margin-top:14px' "
            "onsubmit=\"return confirm('" +
            htmlText(UI_LICENSE_REMOVE_CONFIRM) +
            "');\">"
            "<input type='hidden' name='token' value='" +
            String(webBootSessionId) +
            "'>"
            "<button class='danger' type='submit'>" +
            htmlText(UI_LICENSE_REMOVE_BUTTON) +
            "</button></form>";
    }

    html +=
        "</section>";

    html +=
        "<script>"
        "(function(){"
        "var b=document.getElementById('licenseHardwareCopy');"
        "var e=document.getElementById('licenseHardwareId');"
        "if(!b||!e)return;"
        "b.addEventListener('click',function(){"
        "var v=e.textContent||'';"
        "if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(v).catch(function(){});return;}"
        "var t=document.createElement('textarea');t.value=v;t.setAttribute('readonly','');"
        "t.style.position='fixed';t.style.opacity='0';document.body.appendChild(t);t.select();"
        "try{document.execCommand('copy');}catch(x){}document.body.removeChild(t);"
        "});"
        "})();"
        "</script>";

    html += htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleLicenseActivate()
{
    if (!webFirmwareActionTokenValid()) {
        server.send(
            403,
            "text/plain; charset=utf-8",
            "Invalid action token"
        );
        return;
    }

    if (rejectWhileRecording("license activation"))
        return;

    LicenseStatus installStatus =
        LICENSE_STATUS_INVALID_FORMAT;

    String error;

    if (!server.hasArg("activation_code")) {
        error =
            "activation code is missing";
    } else {
        String activationCode =
            server.arg("activation_code");

        activationCode.trim();

        if (licenseInstallCode(
                activationCode,
                error,
                &installStatus
            )) {
            logWrite(
                "LICENSE | installed | hardware_id=" +
                licenseHardwareId() +
                " | license_id=" +
                licenseId() +
                " | edition=" +
                String(licenseEditionName())
            );

            server.sendHeader(
                "Location",
                "/license?notice=installed"
            );

            server.send(
                303,
                "text/plain; charset=utf-8",
                ""
            );
            return;
        }
    }

    consoleWrite(
        "LICENSE",
        "Install failed | " +
        error
    );

    logWrite(
        "LICENSE | install failed | status=" +
        String(
            licenseStatusName(
                installStatus
            )
        ) +
        " | reason=" +
        error
    );

    server.sendHeader(
        "Location",
        "/license?notice=activation_failed&status=" +
        String((unsigned)installStatus)
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleLicenseRemove()
{
    if (!webFirmwareActionTokenValid()) {
        server.send(
            403,
            "text/plain; charset=utf-8",
            "Invalid action token"
        );
        return;
    }

    if (rejectWhileRecording("license removal"))
        return;

    String hardwareId =
        licenseHardwareId();

    String error;

    if (!licenseRemove(error)) {
        logWrite(
            "LICENSE | remove failed | reason=" +
            error
        );

        server.sendHeader(
            "Location",
            "/license?notice=remove_failed"
        );

        server.send(
            303,
            "text/plain; charset=utf-8",
            ""
        );
        return;
    }

    logWrite(
        "LICENSE | removed | hardware_id=" +
        hardwareId
    );

    server.sendHeader(
        "Location",
        "/license?notice=removed"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// SYSTEM INFO
// -------------------------------------------------------------

static void handleSysInfo()
{
    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>System Info</h2>"
        "<p>Hardware-, Speicher- und Zeitdiagnose des Fabric Node.</p>"
        "</div></div>";

    html +=
        "<section class='settings-section'>"
        "<h3>Systemressourcen</h3>"
        "Flash Size: " +
        String(ESP.getFlashChipSize() / 1024 / 1024) +
        " MB<br>"
        "PSRAM Size: " +
        String(ESP.getPsramSize() / 1024 / 1024) +
        " MB<br>"
        "Heap Free: " +
        String(ESP.getFreeHeap() / 1024) +
        " KB<br>"
        "Sketch Size: " +
        String(ESP.getSketchSize() / 1024) +
        " KB"
        "</section>";

    html +=
        "<section class='settings-section'>"
        "<h3>" +
        htmlText(UI_MOTION_SENSOR_TYPE) +
        "</h3><p><span class='status-pill ok'>" +
        htmlText(motionSensorTypeUiId()) +
        "</span></p>" +
        htmlText(UI_PRESENCE_INPUT) +
        ": GPIO" +
        String((int)PIR_PIN);

    if (radarConfigAvailable()) {
        html +=
            "<br>UART: RX=GPIO" +
            String((int)RADAR_RX_PIN) +
            " / TX=GPIO" +
            String((int)RADAR_TX_PIN);
    } else {
        html +=
            "<p class='muted'>" +
            htmlText(UI_RADAR_CONFIG_PIR_NOTE) +
            "</p>";
    }

    html +=
        "</section>";

    html +=
        "<section class='settings-section'>"
        "<h3>Echtzeituhr (RTC)</h3>";

    if (rtcDetected()) {

        html +=
            "<p><span class='status-pill " +
            String(rtcClockValid() ? "ok" : "warn") +
            "'>" +
            String(rtcClockValid() ? "DIAGNOSE OK" : "ERKANNT / ZEIT UNGÜLTIG") +
            "</span></p>"
            "Typ: <b>" +
            htmlEscape(String(rtcTypeName())) +
            "</b><br>"
            "I2C-Adresse: <code>0x68</code><br>"
            "I2C-Pins: SDA=GPIO" +
            String((int)RTC_SDA_PIN) +
            " / SCL=GPIO" +
            String((int)RTC_SCL_PIN) +
            "<br>"
            "RTC-Zeit: <b>" +
            htmlEscape(rtcTimeText()) +
            "</b><br>"
            "Oszillator-Status: " +
            String(rtcOscillatorStopped() ? "OSF gesetzt" : "OK") +
            "<br>"
            "Systemzeit beim Boot aus RTC übernommen: " +
            String(rtcRestoredSystemTime() ? "ja" : "nein") +
            "<br>"
            "AT24C32 EEPROM @0x57: " +
            String(rtcEepromDetected() ? "erkannt" : "nicht erkannt") +
            "<br>"
            "<p class='muted'>Die RTC wird automatisch erkannt. Nach erfolgreicher NTP-Synchronisation "
            "wird sie automatisch aktualisiert; dafür sind keine RTC-Einträge in config.txt nötig.</p>";

    } else {

        html +=
            "<p><span class='status-pill'>OPTIONAL / NICHT ERKANNT</span></p>"
            "Keine unterstützte externe RTC auf SDA=GPIO" +
            String((int)RTC_SDA_PIN) +
            " / SCL=GPIO" +
            String((int)RTC_SCL_PIN) +
            " erkannt."
            "<p class='muted'>Das ist kein Systemfehler. Ohne RTC verwendet SensorForge wie bisher "
            "die normale System-/NTP-Zeit.</p>";
    }

    html +=
        "</section>";

    html +=
        "<br><a href='/'><button>Back</button></a>";

    html += htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}



// -------------------------------------------------------------
// BOARD INFO
// -------------------------------------------------------------

static void handleBoardInfo()
{
    String html = htmlHeader();

    html += "<h2>Board Info</h2>";

#if defined(BOARD_FREENOVE)

    html += "Board: Freenove FNK0085 ESP32-S3 WROOM<br>";
    html += "Storage: SDMMC 1-bit<br>";

#elif defined(BOARD_XIAO)

    html += "Board: Seeed XIAO ESP32S3 Sense<br>";
    html += "Storage: SPI SD<br>";

#endif

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// PSRAM TEST
// -------------------------------------------------------------

static void handlePSRAM()
{
    if (rejectWhileRecording("PSRAM test"))
        return;

    String html = htmlHeader();

    html += "<h2>PSRAM Test</h2>";

    const size_t testSize = 1024 * 1024;

    uint8_t *buf =
        (uint8_t *)ps_malloc(testSize);

    if (!buf) {

        html += "<p style='color:red;'>"
                "PSRAM Allocation FAILED"
                "</p>";

    } else {

        for (size_t i = 0; i < testSize; ++i)
            buf[i] = (uint8_t)(i & 0xFF);

        bool ok = true;

        for (size_t i = 0; i < testSize; ++i) {

            if (buf[i] !=
                (uint8_t)(i & 0xFF)) {

                ok = false;
                break;
            }
        }

        free(buf);

        if (ok) {
            html += "<p style='color:green;'>PSRAM OK</p>";
        } else {
            html += "<p style='color:red;'>PSRAM ERROR</p>";
        }
    }

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// START SERVER
// -------------------------------------------------------------

void webConfigStart()
{
    if (webActive)
        return;

    if (webBootSessionId == 0) {
        webBootSessionId =
            esp_random();

        if (webBootSessionId == 0)
            webBootSessionId = 1;
    }

    if (!webRoutesRegistered) {

        server.addMiddleware([](
            WebServer &requestServer,
            Middleware::Callback next
        ) -> bool {

            // Protect every route, including player, downloads, destructive
            // tools and the activity heartbeat. Unauthorized requests do not
            // keep the WiFi inactivity timer alive.
            if (
                cfg_web_auth_enabled &&
                !requestServer.authenticate(
                    cfg_web_username.c_str(),
                    cfg_web_password.c_str()
                )
            ) {
                requestServer.requestAuthentication();
                return true;
            }

            noteWebActivity();

            bool handled =
                next();

            // Long synchronous file/ZIP transfers cannot be interrupted by
            // the main loop. Refresh once more at completion so the timeout
            // starts only after the transfer is finished.
            noteWebActivity();

            return handled;
        });

        server.on("/activity", HTTP_GET, []() {
            // /activity is the common visible-browser heartbeat used by both
            // normal WebConfig pages and the standalone player. It therefore
            // owns automatic maintenance-pause recovery as well as WiFi liveness.
            maybeAutoPauseRecordingForWebUi();

            server.send(
                204,
                "text/plain",
                ""
            );
        });

        server.on("/ui_status", HTTP_GET, handleUiStatus);
        server.on("/motion_status", HTTP_GET, handleMotionStatus);
        server.on("/language", HTTP_POST, handleLanguageChange);
        server.on("/recording_pause", HTTP_POST, handleRecordingPause);
        server.on("/recording_pause_keepalive", HTTP_POST, handleRecordingPauseKeepalive);
        server.on("/transport_pause_release", HTTP_POST, handleTransportPauseRelease);

        server.on("/", HTTP_GET, handleRoot);
        server.on("/config", HTTP_GET, handleConfig);
        server.on("/save", HTTP_POST, handleSave);
        server.on("/audio_test_record", HTTP_POST, handleAudioTestRecord);
        server.on("/audio_benchmark", HTTP_POST, handleAudioBenchmark);
        server.on("/config_download", HTTP_GET, handleConfigDownload);
        server.on(
            "/config_upload",
            HTTP_POST,
            handleConfigUploadFinished,
            handleConfigUploadData
        );
        server.on("/config_sd_delete", HTTP_POST, handleConfigSdDelete);
        server.on("/config_sd_copy", HTTP_POST, handleConfigSdCopy);
        server.on("/config_factory_reset", HTTP_POST, handleConfigFactoryReset);
        server.on("/transport", HTTP_GET, handleTransportPage);
        server.on("/transport_measure", HTTP_POST, handleTransportMeasure);
        server.on("/transport_save", HTTP_POST, handleTransportSave);
        server.on("/transport_activate", HTTP_POST, handleTransportActivate);
        server.on("/transport_cancel", HTTP_POST, handleTransportCancel);
    server.on("/simulate_motion", HTTP_POST, handleSimulateMotion);

    server.on("/radar_config", HTTP_GET, handleRadarConfig);
    server.on("/radar_live", HTTP_GET, handleRadarLive);
    server.on("/radar_diag_download", HTTP_GET, handleRadarDiagnosticDownload);
    server.on("/radar_calibration_status", HTTP_GET, handleRadarCalibrationStatus);
    server.on("/radar_calibration_action", HTTP_POST, handleRadarCalibrationAction);
    server.on("/radar_config_save", HTTP_POST, handleRadarConfigSave);
    server.on("/radar_config_defaults", HTTP_POST, handleRadarConfigDefaults);

    server.on("/image_motion", HTTP_GET, handleImageMotionPage);
    server.on("/image_motion_save", HTTP_POST, handleImageMotionSave);
    server.on("/image_motion_test", HTTP_POST, handleImageMotionTest);
    server.on("/image_motion_reset", HTTP_POST, handleImageMotionResetBackground);
    server.on("/image_motion_status", HTTP_GET, handleImageMotionStatus);
    server.on("/image_motion_diag_download", HTTP_GET, handleImageMotionDiagnosticDownload);

    WebSdMaintenanceUiHooks sdMaintenanceHooks = {
        htmlHeader,
        htmlFooter,
        [](uint32_t delayMs) {
            rebootScheduled = true;
            rebootAtMs = millis() + delayMs;
        }
    };
    webSdMaintenanceRegisterRoutes(
        server,
        sdMaintenanceHooks
    );

    server.on("/preview", HTTP_GET, handlePreview);
    server.on("/snapshot", HTTP_GET, handleSnapshot);
    server.on("/preview_stop", HTTP_POST, handlePreviewStop);
    server.on("/camera_crop_apply", HTTP_POST, handleCameraCropApply);
    server.on("/camera_crop_save", HTTP_POST, handleCameraCropSave);

    server.on("/license", HTTP_GET, handleLicensePage);
    server.on("/license_activate", HTTP_POST, handleLicenseActivate);
    server.on("/license_remove", HTTP_POST, handleLicenseRemove);

    server.on("/firmware_update", HTTP_GET, handleFirmwareUpdate);
    server.on(
        "/firmware_upload",
        HTTP_POST,
        handleFirmwareUploadFinished,
        handleFirmwareUploadData
    );
    server.on("/firmware_install", HTTP_POST, handleFirmwareInstall);
    server.on("/firmware_discard", HTTP_POST, handleFirmwareDiscard);

    server.on("/reboot", HTTP_GET, handleReboot);
    server.on("/rebooting", HTTP_GET, handleRebooting);
    server.on("/reboot_do", HTTP_POST, handleRebootDo);
    server.on("/shutdown", HTTP_GET, handleShutdown);
    server.on("/shutting_down", HTTP_GET, handleShuttingDown);
    server.on("/shutdown_do", HTTP_POST, handleShutdownDo);

    // V22: all /log* routes are registered by the dedicated log-reader module.
    webLogReaderRegisterRoutes(server);
    server.on("/files", HTTP_GET, handleFiles);
    server.on("/files_day", HTTP_GET, handleFilesDay);
    server.on("/download_day", HTTP_GET, handleDownloadDay);
    server.on("/download_day_status", HTTP_GET, handleDownloadDayStatus);
    server.on("/delete_day", HTTP_POST, handleDeleteDay);
    server.on("/file", HTTP_GET, handleFile);

    // Stateless SensorForge Sync API v1. Existing WebConfig/file routes remain
    // unchanged; the API adds browser/Python/app access on the same server.
    syncApiRegisterRoutes(server);

    webPlayerRegisterRoutes(server);

    server.on("/sysinfo", HTTP_GET, handleSysInfo);
    server.on("/board", HTTP_GET, handleBoardInfo);
    server.on("/psram", HTTP_GET, handlePSRAM);
   

        server.onNotFound([]() {
            server.send(
                404,
                "text/plain; charset=utf-8",
                "Not found"
            );
        });

        webRoutesRegistered = true;
    }

    noteWebActivity();

    server.begin();
    webActive = true;

    Serial.println("WebConfig started");
}


// -------------------------------------------------------------
// STOP SERVER
// -------------------------------------------------------------

void webConfigStop()
{
    if (!webActive)
        return;

    // Never leave the LD2410S in the temporary all-gate calibration range when
    // WebConfig/WiFi is closed. This is especially important for an inactivity
    // timeout after the operator has left a calibration page open.
    if (
        radarCalibrationActiveMode() !=
            RADAR_CALIBRATION_NONE
    ) {
        String calibrationError;

        if (!radarCalibrationStop(
                calibrationError
            )) {
            Serial.println(
                "LD2410S calibration cleanup before WebConfig stop failed | " +
                calibrationError
            );

            logWrite(
                "Radar calibration cleanup before WebConfig stop failed | " +
                calibrationError
            );
        } else {
            Serial.println(
                "LD2410S calibration stopped before WebConfig shutdown"
            );

            logWrite(
                "Radar calibration stopped before WebConfig shutdown"
            );
        }
    }

    // A temporary maintenance pause is never allowed to survive the WebConfig
    // session that created it.
    if (recordingAutomationPaused) {
        setRecordingAutomationPaused(
            false,
            "WebConfig stopped"
        );
    }

    webPlayerStop();

    server.stop();
    webActive = false;

    simulatedMotionUntilMs = 0;
    stopCameraPreview();

    Serial.println("WebConfig stopped");
}


// -------------------------------------------------------------
// INACTIVITY STATUS
// -------------------------------------------------------------

bool webConfigInactiveFor(
    unsigned long timeoutSec
)
{
    if (
        !webActive ||
        timeoutSec == 0 ||
        webSdMaintenanceBusy() ||
        radarWebMaintenanceDepth > 0 ||
        radarWebMaintenanceGraceActive()
    ) {
        return false;
    }

    uint32_t elapsedMs =
        (uint32_t)(
            millis() -
            webLastActivityMs
        );

    // During an intentional radar calibration the operator may move away from
    // the browser for several minutes. Keep WebConfig available for a bounded
    // maintenance window even if browser timers are throttled. A truly
    // abandoned calibration still falls back to the normal WiFi timeout after
    // ten minutes without any authenticated web activity.
    if (
        radarCalibrationActiveMode() !=
            RADAR_CALIBRATION_NONE &&
        elapsedMs < RADAR_CALIBRATION_MAX_IDLE_HOLD_MS
    ) {
        return false;
    }

    uint64_t timeoutMs =
        (uint64_t)timeoutSec *
        1000ULL;

    return
        (uint64_t)elapsedMs >=
        timeoutMs;
}


// -------------------------------------------------------------
// LOOP
// -------------------------------------------------------------

void webConfigLoop()
{
    if (
        shutdownScheduled &&
        (int32_t)(
            millis() -
            shutdownAtMs
        ) >= 0
    ) {
        performManualShutdown();
        return;
    }

    if (
        rebootScheduled &&
        (int32_t)(
            millis() -
            rebootAtMs
        ) >= 0
    ) {
        rebootScheduled =
            false;

        ESP.restart();
        return;
    }

    if (!webActive) {
        // A manually closed WebConfig must not strand an in-progress secure
        // erase with the storage gate held. Continue the maintenance job even
        // without HTTP; only progress/abort control is unavailable then.
        webSdMaintenanceLoop();
        return;
    }

    server.handleClient();

    // The operator pause is lease-based. Only a visible SensorForge page
    // renews the lease; a closed/hidden/abandoned UI therefore restores
    // automatic recording without requiring any manual cleanup.
    releaseRecordingPauseIfLeaseExpired();

    // Secure Erase writes one small zero-filled chunk per loop iteration.
    // Keeping it here lets the WebServer answer progress/abort requests
    // between chunks instead of blocking for the duration of the whole card.
    webSdMaintenanceLoop();

    webPlayerLoop();

    // Expire a preview session if the browser vanished without sending the
    // explicit /preview_stop request.
    webConfigCameraPreviewActive();
}