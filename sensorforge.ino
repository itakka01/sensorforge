// =============================================================
// ESP32-S3 Sensor Camera
// Freenove FNK0085 / Seeed XIAO ESP32S3 Sense
// Camera + PIR + SD + AVI/MKV + WiFi + Deep Sleep
// =============================================================



#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "driver/rtc_io.h"

#include "board_config.h"
#include "camera_select.h"
#include "config.h"
#include "recorder.h"
#include "logger.h"
#include "webconfig.h"
#include "storage_guard.h"
#include "radar.h"
#include "rtc.h"
#include "thermal.h"
#include "sync_api.h"
#include "license.h"
#include "recording_crypto.h"
#include "log_storage.h"


// =============================================================
// BOARD PINS
// =============================================================



// =============================================================
// SETTINGS
// =============================================================

// Sleep mode is configured at runtime via config.txt:
//   sleep_mode=off|light_sleep|deep_sleep
//   sleep_delay_ms=0..60000
//
// Firmware default is "off", so installing this version does not
// unexpectedly change the current always-awake behavior.

// 1 = bei normalem Boot mit aktivem WebConfig wach bleiben
// 0 = auch dann sofort schlafen, wenn keine Bewegung erkannt wird
#define WEB_CONFIG_KEEP_AWAKE 1


// Single authoritative firmware build timestamp.
// Kept in the main translation unit so boot diagnostics, log and WebConfig
// all report the same compile time.
String firmwareBuildTimestamp()
{
    return
        String(__DATE__) +
        " " +
        String(__TIME__);
}


// =============================================================
// GLOBAL STATE
// =============================================================

bool recording = false;
bool sdReady = false;
bool webConfigStarted = false;
bool cameraInitialized = false;

// Camera state retained across light sleep. Fast light-sleep standby is enabled
// only after the physical sensor has been identified as an OV3660. Other
// sensors keep the legacy deinit/reinit behavior.
static uint8_t cameraFramebufferCount = 0;
static bool cameraSoftPowerDownActive = false;
static bool cameraFastSleepCapabilityKnown = false;
static bool cameraFastSleepSupported = false;
static uint16_t cameraDetectedPid = 0;

// Light-sleep wake timing diagnostics. These timestamps are collected only
// when debug logging is enabled and the wake source is EXT1/presence. No
// timing log is printed until after the first frame has been written, so the
// diagnostic itself does not add serial-output latency to the measured path.
static bool wakeTimingActive = false;
static uint64_t wakeTimingStartUs = 0;
static uint64_t wakeTimingCameraReadyUs = 0;
static uint64_t wakeTimingMotionAcceptedUs = 0;
static uint64_t wakeTimingRecorderReadyUs = 0;
static uint32_t wakeTimingFrameAttempts = 0;
static uint64_t wakeTimingStorageUs = 0;
static uint64_t wakeTimingCameraPrepareUs = 0;
static uint64_t wakeTimingPathUs = 0;
static uint64_t wakeTimingRecorderOpenUs = 0;
// Fine-grained wake-path diagnostics. These are timestamps only; output is
// deferred until after the first frame write so the measurements do not add
// console latency to the critical wake-to-recording path.
static uint64_t wakeTimingMotionLogStartUs = 0;
static uint64_t wakeTimingMotionLogDoneUs = 0;
static uint64_t wakeTimingStartRecordingEnterUs = 0;
static uint64_t wakeTimingPrechecksDoneUs = 0;

// Protect the first post-wake frame from USB/Serial stalls. After an EXT1
// light-sleep wake, non-essential console output and the REC START SD-log
// write are deferred until the first video frame has been written safely.
// This is independent of cfg_debug_enabled so production builds get the same
// low-latency wake behavior.
static bool wakeCriticalPathActive = false;
static bool deferredWakeConsolePending = false;
static esp_sleep_wakeup_cause_t deferredWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
static uint64_t deferredWakeSleptUs = 0;
static uint64_t deferredWakeCameraWakeUs = 0;
static bool deferredWakeFastCameraSleep = false;
static bool deferredWakeCameraWakeOk = false;
static bool deferredMotionConsolePending = false;
static String deferredMotionConsoleMessage = "";
static bool deferredRecordingStartPending = false;
static String deferredRecordingStartMessage = "";
// Exact light-sleep duration is logged only after the first post-wake frame on
// the latency-sensitive EXT1 path. This keeps the existing wake-to-frame path
// free of SD logger flushes while still giving the Log Analyzer exact data.
static bool deferredSleepLogPending = false;

// True only when this boot had to start without an SD card.
// If a valid SD config later becomes available through recovery,
// reboot through the normal config-priority path so SD again becomes
// authoritative and the LittleFS shadow is synchronized 1:1.
static bool bootStartedWithoutSd = false;

unsigned long lastMotionMs = 0;
uint32_t lastFrameUs = 0;
static unsigned long recordingSegmentStartMs = 0;

// Recording-event safety limit. Unlike recording_segment_seconds, this timer
// spans every segment that belongs to the same continuous recording event.
// When the configured maximum is reached, the current recording is finalized
// and NEW recording starts are suppressed for the configured cooldown period.
static unsigned long recordingEventStartMs = 0;
static bool recordingSafetyCooldownState = false;
static unsigned long recordingSafetyCooldownStartedMs = 0;
static uint32_t recordingSafetyCooldownDurationSeconds = 0;

// Start of the current idle period used by sleep_delay_ms.
// Reset after recording ends, after WiFi shuts down, and whenever
// motion is seen while the recorder is idle.
static unsigned long sleepIdleSinceMs = 0;


// Sleep diagnostics are state-based so the serial console stays readable.
// Blocking reasons are printed only when they change; the pending countdown
// is printed at most once per second.
enum SleepDiagState : uint8_t {
    SLEEP_DIAG_UNKNOWN = 0,
    SLEEP_DIAG_DISABLED,
    SLEEP_DIAG_MOTION,
    SLEEP_DIAG_SD,
    SLEEP_DIAG_WIFI,
    SLEEP_DIAG_PRESENCE,
    SLEEP_DIAG_MAGNET,
    SLEEP_DIAG_PENDING
};

// Explicit prototype: prevents Arduino .ino auto-prototype generation
// from placing a prototype before SleepDiagState is declared.
static void setSleepDiagState(
    SleepDiagState state,
    const char *message
);

static SleepDiagState sleepDiagState =
    SLEEP_DIAG_UNKNOWN;

static unsigned long lastSleepPendingDiagMs =
    0;


bool diskSpaceBlocked = false;
uint8_t consecutiveFrameFailures = 0;

unsigned long lastSpaceCheckMs = 0;
unsigned long lastBlockedSpaceRetryMs = 0;

static const unsigned long SPACE_CHECK_INTERVAL_MS = 5000UL;
static const unsigned long SPACE_BLOCK_RETRY_MS = 30000UL;
static const unsigned long SD_RECOVERY_RETRY_MS = 60000UL;

// SD startup/recovery policy. These are firmware-internal reliability
// settings and intentionally do not add config.txt options.
static const uint8_t SD_BOOT_MAX_ATTEMPTS = 5;
static const uint8_t SD_RUNTIME_RECOVERY_MAX_ATTEMPTS = 3;
static const unsigned long SD_COLD_BOOT_SETTLE_MS = 1200UL;
static const unsigned long SD_SPI_PRECLOCK_HZ = 400000UL;
static const uint8_t MAX_CONSECUTIVE_FRAME_FAILURES = 5;

// Thermal protection is intentionally firmware-owned rather than configurable
// through config.txt. A malformed or over-permissive field configuration must
// never be able to disable the hardware safety guard.
static const uint32_t THERMAL_COOLDOWN_MAGIC = 0x53465448UL; // "SFTH"
static const uint8_t THERMAL_SOURCE_CPU = 0x01U;
static const uint8_t THERMAL_SOURCE_RTC = 0x02U;

RTC_DATA_ATTR uint32_t thermalCooldownMarker = 0;
RTC_DATA_ATTR uint8_t thermalCooldownSourceMask = 0;

// Transport-mode phase survives timer deep sleep. The persistent enable flag
// itself lives in config.txt; RTC memory only remembers whether the black-cover
// checks or the post-uncover installation delay is currently in progress.
static const uint32_t TRANSPORT_RTC_MAGIC = 0x53465452UL; // "SFTR"
static const char TRANSPORT_STATE_FILE[] = "/transport.state";
static const char TRANSPORT_STATE_TEMP[] = "/transport.state.tmp";
static const uint8_t TRANSPORT_PHASE_NONE = 0;
static const uint8_t TRANSPORT_PHASE_CHECKING = 1;
static const uint8_t TRANSPORT_PHASE_INSTALL_DELAY = 2;

RTC_DATA_ATTR uint32_t transportRtcMagic = 0;
RTC_DATA_ATTR uint8_t transportRtcPhase = TRANSPORT_PHASE_NONE;
RTC_DATA_ATTR uint32_t transportRtcWakeCount = 0;
// Hard transport timeout state. The absolute start timestamp is preferred when
// RTC/system time is valid; the fallback counters keep the safety limit working
// across timer deep-sleep cycles even without a valid wall clock.
RTC_DATA_ATTR int64_t transportRtcStartedEpoch = 0;
RTC_DATA_ATTR uint32_t transportRtcElapsedFallbackSeconds = 0;
RTC_DATA_ATTR uint32_t transportRtcLastSleepSeconds = 0;

// Normal deep-sleep accounting for the Log Analyzer. RTC slow memory survives
// normal ESP32 deep sleep, so the next boot can compute an elapsed duration
// after the DS3231 has restored wall-clock time. This state is never used by
// transport mode or the thermal cooldown paths.
static const uint32_t NORMAL_DEEP_SLEEP_RTC_MAGIC = 0x53464453UL; // "SFDS"
RTC_DATA_ATTR uint32_t normalDeepSleepRtcMagic = 0;
RTC_DATA_ATTR int64_t normalDeepSleepStartedEpoch = 0;

static bool thermalWarningState = false;
static bool thermalEmergencyState = false;
static bool thermalRecoveredFromCooldownBoot = false;
static uint8_t thermalRecoveredSourceMask = 0;
static uint8_t thermalWarningSourceMask = 0;
static uint8_t thermalEmergencySourceMask = 0;
static uint8_t thermalCpuEmergencyHighSamples = 0;
static uint8_t thermalRtcEmergencyHighSamples = 0;
static uint32_t lastThermalSampleMs = 0;
static float lastCpuTemperatureC = NAN;
static float lastRtcTemperatureC = NAN;
static bool lastRtcTemperatureValid = false;
static uint32_t lastRtcTemperatureReadMs = 0;

// RAM-only temperature statistics for one complete recording event.
// Samples are taken by the existing 5-second thermal monitor, so no extra
// sensor polling or SD writes are added to the active recording path.
// One compact summary is written when the event ends.
struct RecordingThermalStats {
    uint32_t cpuSamples;
    double cpuSumC;
    float cpuMinC;
    float cpuMaxC;

    uint32_t rtcSamples;
    double rtcSumC;
    float rtcMinC;
    float rtcMaxC;

    uint32_t lastSampleMs;
};

static RecordingThermalStats recordingThermalStats = {};

static bool watchdogEnabled = false;


// Sleep helper is defined later next to the power-management code,
// but stopRecording() starts the idle timer.
static void resetSleepDelayTimer();
static bool rotateRecordingSegment(const String &reason);
static void serviceRecordingSafetyCooldown();
static void triggerRecordingEventSafetyLimit();

// Exposed to WebConfig for live dashboard/status reporting.
bool recordingSafetyCooldownActive();
uint32_t recordingSafetyCooldownRemainingSeconds();

// OV3660 crop runtime control. Live Preview uses the same implementation as
// normal camera initialization so tested and recorded framing cannot diverge.
bool cameraApplyCropRuntime(
    const String &zoom,
    int positionX,
    int positionY,
    String &error
);

// A magnet wake from light sleep starts WebConfig immediately.
void startWebConfig();
void stopWebConfigWifi(const char *reason);

// Thermal helpers are implemented near the power-management code.
static void thermalMonitorLoop();
static void enterThermalCooldownSleepEarly(
    float cpuTempC,
    bool rtcTempValid,
    float rtcTempC
);
static void enterThermalEmergencySleep(
    float cpuTempC,
    uint8_t sourceMask
);
static void recordingThermalReset();
static void recordingThermalAccumulate(
    float cpuTempC,
    bool rtcTempValid,
    float rtcTempC
);
static void recordingThermalFinish(
    const char *reason
);

// Transport mode is handled immediately after config load and before logger,
// radar or WiFi startup. Returning false means normal boot may continue.
static bool handleTransportModeBoot(
    esp_sleep_wakeup_cause_t wakeCause
);


// =============================================================
// PIR STARTUP GUARD
// =============================================================

// The SR602 output can be HIGH for roughly two seconds after its
// supply is applied. On a normal/cold boot this must not be treated
// as real motion. Three seconds gives a conservative margin.
//
// On wake from deep sleep the sensor remains powered, so the guard
// is deliberately skipped: PIR HIGH is then the real wake event.
static const unsigned long PIR_STARTUP_GUARD_MS = 3000UL;
static unsigned long pirStartupGuardUntilMs = 0;


// =============================================================
// MAGNET / REED SWITCH
// =============================================================

// Active LOW:
//   switch open   -> HIGH via internal pull-up
//   switch closed -> LOW
//
// The state is runtime-only. It does not alter config.txt.
// While awake, a stable HIGH->LOW edge toggles WiFi immediately.
// During light/deep sleep, LOW is also a wake source; a magnet wake
// explicitly starts WiFi/WebConfig.
static const unsigned long MAGNET_DEBOUNCE_MS = 60UL;

static int magnetRawState = HIGH;
static int magnetStableState = HIGH;
static unsigned long magnetLastChangeMs = 0;


// =============================================================
// STATUS LED / FAULT INDICATION
// =============================================================
//
// Normal:
//   recording -> LED steady ON
//   idle      -> LED OFF
//
// Fault overlay:
//   SD unavailable -> 3 short inverse pulses every 5 seconds.
//   - idle:      3 short flashes
//   - recording: 3 short dark gaps in the steady recording LED
//
// The implementation is non-blocking. No delay() is used here.
// If led_enabled=0, the LED is forced OFF unconditionally.
//
// This is intentionally small but can later be extended with more
// StatusFault values and different pulse counts.
enum StatusFault : uint8_t {
    STATUS_FAULT_NONE = 0,
    STATUS_FAULT_SD
};

// Explicit prototypes: prevent Arduino .ino auto-prototype generation
// from placing prototypes before StatusFault is declared.
static StatusFault currentStatusFault();
static uint8_t statusFaultPulseCount(
    StatusFault fault
);

static StatusFault lastStatusFault =
    STATUS_FAULT_NONE;


// Some XIAO Sense revisions route microSD CS through GPIO21, which is
// physically shared with the onboard USER LED. On those boards the status
// LED must not be driven independently because doing so would corrupt SD CS.
static inline void statusLedSetOutput()
{
#if STATUS_LED_AVAILABLE
    pinMode(
        LED_PIN,
        OUTPUT
    );
#endif
}


static inline void statusLedWriteLevel(
    int level
)
{
#if STATUS_LED_AVAILABLE
    digitalWrite(
        LED_PIN,
        level
    );
#else
    (void)level;
#endif
}


static inline int statusLedReadLevel()
{
#if STATUS_LED_AVAILABLE
    return
        digitalRead(
            LED_PIN
        );
#else
    return
        LED_OFF_LEVEL;
#endif
}


static StatusFault currentStatusFault()
{
    if (!sdReady)
        return STATUS_FAULT_SD;

    return STATUS_FAULT_NONE;
}


static uint8_t statusFaultPulseCount(
    StatusFault fault
)
{
    switch (fault) {
        case STATUS_FAULT_SD:
            return 3;

        default:
            return 0;
    }
}


static void updateStatusLed()
{
#if !STATUS_LED_AVAILABLE
    return;
#else
    if (!cfg_led_enabled) {

        statusLedWriteLevel(
            LED_OFF_LEVEL
        );

        return;
    }


    bool baseOn =
        recording;

    StatusFault fault =
        currentStatusFault();


    if (
        fault !=
        lastStatusFault
    ) {

        if (
            fault ==
            STATUS_FAULT_SD
        ) {

            powerConsole(
                "Status LED fault | SD unavailable | pattern=3 short pulses / 5 s"
            );

        } else if (
            lastStatusFault ==
            STATUS_FAULT_SD
        ) {

            powerConsole(
                "Status LED fault cleared | SD ready"
            );
        }

        lastStatusFault =
            fault;
    }


    if (
        fault ==
        STATUS_FAULT_NONE
    ) {

        statusLedWriteLevel(
            baseOn
                ? LED_ON_LEVEL
                : LED_OFF_LEVEL
        );

        return;
    }


    // Keep the normal LED state for the first second of each cycle,
    // then insert short inverse pulses. This remains readable even if
    // the baseline is steady ON during recording.
    static const uint32_t FAULT_CYCLE_MS =
        5000UL;

    static const uint32_t FAULT_PATTERN_START_MS =
        1000UL;

    static const uint32_t FAULT_PULSE_MS =
        100UL;

    static const uint32_t FAULT_GAP_MS =
        150UL;


    uint32_t phase =
        millis() %
        FAULT_CYCLE_MS;

    bool invert =
        false;

    uint8_t pulses =
        statusFaultPulseCount(
            fault
        );

    for (
        uint8_t i = 0;
        i < pulses;
        ++i
    ) {

        uint32_t pulseStart =
            FAULT_PATTERN_START_MS +
            (uint32_t)i *
            (
                FAULT_PULSE_MS +
                FAULT_GAP_MS
            );

        if (
            phase >= pulseStart &&
            phase <
                pulseStart +
                FAULT_PULSE_MS
        ) {

            invert =
                true;

            break;
        }
    }


    bool ledOn =
        invert
            ? !baseOn
            : baseOn;

    statusLedWriteLevel(
        ledOn
            ? LED_ON_LEVEL
            : LED_OFF_LEVEL
    );
#endif
}


// =============================================================
// WATCHDOG / BOOT DIAGNOSTICS
// =============================================================

static const char *resetReasonName(
    esp_reset_reason_t reason
)
{
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}


static void initWatchdog()
{
#if ESP_IDF_VERSION_MAJOR >= 5

    esp_task_wdt_config_t watchdogConfig = {};

    watchdogConfig.timeout_ms =
        30000;

    watchdogConfig.idle_core_mask =
        0;

    watchdogConfig.trigger_panic =
        true;


    // Arduino-ESP32 can initialize TWDT before setup().
    // Query first to avoid the noisy "TWDT already initialized" error.
    esp_err_t status =
        esp_task_wdt_status(
            NULL
        );

    esp_err_t configErr =
        ESP_OK;


    if (
        status ==
        ESP_ERR_INVALID_STATE
    ) {

        configErr =
            esp_task_wdt_init(
                &watchdogConfig
            );

    } else {

        configErr =
            esp_task_wdt_reconfigure(
                &watchdogConfig
            );
    }


    if (configErr == ESP_OK) {

        status =
            esp_task_wdt_status(
                NULL
            );


        if (
            status ==
            ESP_ERR_NOT_FOUND
        ) {

            esp_task_wdt_add(
                NULL
            );

            status =
                esp_task_wdt_status(
                    NULL
                );
        }


        watchdogEnabled =
            status ==
            ESP_OK;
    }

#else

    if (
        esp_task_wdt_init(
            30,
            true
        ) == ESP_OK
    ) {
        esp_task_wdt_add(NULL);
        watchdogEnabled = true;
    }

#endif

    Serial.println(
        watchdogEnabled
        ? "Watchdog enabled: 30 s"
        : "Watchdog setup not available"
    );
}



static void feedWatchdog()
{
    if (watchdogEnabled) {
        esp_task_wdt_reset();
    }
}


static void formatConsoleTimestamp(
    char *buffer,
    size_t bufferSize
)
{
    if (
        buffer == nullptr ||
        bufferSize == 0
    ) {
        return;
    }


    if (timeIsValid()) {

        struct timeval tv;
        gettimeofday(
            &tv,
            nullptr
        );

        struct tm tmNow;
        localtime_r(
            &tv.tv_sec,
            &tmNow
        );

        snprintf(
            buffer,
            bufferSize,
            "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
            tmNow.tm_year + 1900,
            tmNow.tm_mon + 1,
            tmNow.tm_mday,
            tmNow.tm_hour,
            tmNow.tm_min,
            tmNow.tm_sec,
            (long)(
                tv.tv_usec /
                1000L
            )
        );

        return;
    }


    // Before NTP/RTC time is valid, use an unambiguous boot-relative
    // timestamp instead of printing a bogus calendar date.
    uint64_t totalMs =
        (uint64_t)millis();

    unsigned long days =
        (unsigned long)(
            totalMs /
            86400000ULL
        );

    totalMs %=
        86400000ULL;

    unsigned long hours =
        (unsigned long)(
            totalMs /
            3600000ULL
        );

    totalMs %=
        3600000ULL;

    unsigned long minutes =
        (unsigned long)(
            totalMs /
            60000ULL
        );

    totalMs %=
        60000ULL;

    unsigned long seconds =
        (unsigned long)(
            totalMs /
            1000ULL
        );

    unsigned long milliseconds =
        (unsigned long)(
            totalMs %
            1000ULL
        );

    snprintf(
        buffer,
        bufferSize,
        "UP+%03lu:%02lu:%02lu:%02lu.%03lu",
        days,
        hours,
        minutes,
        seconds,
        milliseconds
    );
}


static void powerConsole(
    const char *format,
    ...
)
{
    char timestamp[40];
    formatConsoleTimestamp(
        timestamp,
        sizeof(timestamp)
    );

    char message[224];

    va_list args;
    va_start(
        args,
        format
    );

    vsnprintf(
        message,
        sizeof(message),
        format,
        args
    );

    va_end(args);

    Serial.printf(
        "[%s] [POWER] %s\n",
        timestamp,
        message
    );
}


static const char *sleepWakeCauseName(
    esp_sleep_wakeup_cause_t cause
)
{
    switch (cause) {
        case ESP_SLEEP_WAKEUP_EXT0:
            return "EXT0";

        case ESP_SLEEP_WAKEUP_EXT1:
            return "EXT1";

        case ESP_SLEEP_WAKEUP_TIMER:
            return "TIMER";

        case ESP_SLEEP_WAKEUP_TOUCHPAD:
            return "TOUCH";

        case ESP_SLEEP_WAKEUP_ULP:
            return "ULP";

        case ESP_SLEEP_WAKEUP_GPIO:
            return "GPIO";

        case ESP_SLEEP_WAKEUP_UART:
            return "UART";

        default:
            return "OTHER";
    }
}


static void logSleepCycle(
    const char *mode,
    esp_sleep_wakeup_cause_t wakeCause,
    uint64_t durationMs
)
{
    if (!sdReady)
        return;

    char line[160];

    snprintf(
        line,
        sizeof(line),
        "SLEEP | mode=%s | duration_ms=%llu | cause=%s(%d)",
        mode ? mode : "unknown",
        (unsigned long long)durationMs,
        sleepWakeCauseName(wakeCause),
        (int)wakeCause
    );

    logWrite(
        String(line)
    );
}


static void logTrackedNormalDeepSleepWake(
    esp_sleep_wakeup_cause_t wakeCause,
    esp_reset_reason_t resetReason
)
{
    if (
        normalDeepSleepRtcMagic !=
            NORMAL_DEEP_SLEEP_RTC_MAGIC
    ) {
        return;
    }

    int64_t startedEpoch =
        normalDeepSleepStartedEpoch;

    // Consume the marker exactly once. A later reset must never attribute an
    // old sleep interval to a new boot.
    normalDeepSleepRtcMagic = 0;
    normalDeepSleepStartedEpoch = 0;

    if (
        resetReason != ESP_RST_DEEPSLEEP ||
        !sdReady
    ) {
        return;
    }

    time_t now =
        time(nullptr);

    if (
        startedEpoch > 0 &&
        now >= (time_t)1609459200 &&
        (int64_t)now >= startedEpoch
    ) {
        uint64_t durationMs =
            (uint64_t)(
                (int64_t)now -
                startedEpoch
            ) * 1000ULL;

        logSleepCycle(
            "deep",
            wakeCause,
            durationMs
        );
        return;
    }

    // The cycle itself is known, but an exact duration is impossible without a
    // valid wall clock on both sides. Keep that distinction explicit instead of
    // inventing a duration.
    logWrite(
        "SLEEP | mode=deep | duration_ms=unknown | cause=" +
        String(sleepWakeCauseName(wakeCause)) +
        "(" +
        String((int)wakeCause) +
        ")"
    );
}


static void setSleepDiagState(
    SleepDiagState state,
    const char *message
)
{
    if (sleepDiagState == state)
        return;

    sleepDiagState =
        state;

    if (
        message &&
        message[0]
    ) {
        powerConsole(
            "%s",
            message
        );
    }
}


static void logBootDiagnostics(
    esp_sleep_wakeup_cause_t wakeCause
)
{
    esp_reset_reason_t resetReason =
        esp_reset_reason();

    uint64_t freeMb =
        storageFreeBytes() /
        (1024ULL * 1024ULL);

    String line =
        "BOOT | reset=" +
        String(resetReasonName(resetReason)) +
        "(" +
        String((int)resetReason) +
        ")" +
        " | wake=" +
        String((int)wakeCause) +
        " | heap=" +
        String(ESP.getFreeHeap()) +
        " | psram_free=" +
        String(ESP.getFreePsram()) +
        " | sd_free_mb=" +
        String((unsigned long)freeMb) +
        " | build=" +
        String(__DATE__) +
        " " +
        String(__TIME__);

    Serial.println(line);
    logWrite(line);
}


// =============================================================
// MOTION INPUT
// =============================================================

bool simulatedMotionActive()
{
    return
        webConfigStarted &&
        webConfigMotionActive();
}


bool physicalMotionActive()
{
    // Normal awake recording uses the fast UART gate-energy detector.
    // OT2 is intentionally NOT used here because its internal presence
    // state has a minimum ~10-second no-person hold.
    //
    // If the UART motion mode could not be initialized, fall back to OT2
    // so the camera still remains functional.
    if (radarMotionTrackingAvailable()) {
        return radarMotionActive();
    }

    return
        digitalRead(PIR_PIN) == HIGH;
}


bool motionDetected()
{
    // Thermal emergency has absolute priority over every motion source.
    if (thermalEmergencyState)
        return false;

    // API-exclusive mode is a temporary RAM-only lease. While active, raw
    // sensor parsing continues, but operational motion must not start a new
    // recording. The lease expires automatically if the API client disappears.
    if (syncApiExclusiveActive())
        return false;

    // After the maximum recording-event duration is reached, suppress all NEW
    // operational motion triggers until the configured safety cooldown ends.
    // Raw sensor state remains visible in STATUS/WebConfig diagnostics.
    if (recordingSafetyCooldownActive())
        return false;

    // Installation/arming gate. A configured future deadline suppresses all
    // operational motion triggers. If the system clock is invalid, a configured
    // deadline fails safe and also blocks recording. Raw OT2/radar state remains
    // visible to diagnostics and WebConfig.
    if (!configRecordingAllowedNow())
        return false;

    // WebConfig can temporarily pause the recording automation for maintenance
    // and configuration work. Sensors continue to be parsed for diagnostics,
    // but operational motion is suppressed until the pause lease ends.
    if (
        webConfigStarted &&
        webConfigRecordingPaused()
    ) {
        return false;
    }

    // Live camera preview owns the camera pipeline. Keep sensor parsing alive
    // for status/diagnostics, but suppress operational motion detection and
    // therefore all motion-triggered recording while the preview is active.
    if (
        webConfigStarted &&
        webConfigCameraPreviewActive()
    ) {
        return false;
    }

    return
        physicalMotionActive() ||
        simulatedMotionActive();
}


// =============================================================
// THERMAL STATUS API
// =============================================================

float thermalCpuTemperatureC()
{
    float temperatureC =
        temperatureRead();

    if (isfinite(temperatureC)) {
        lastCpuTemperatureC =
            temperatureC;
    }

    return
        lastCpuTemperatureC;
}


bool thermalRtcTemperatureC(
    float &temperatureC
)
{
    uint32_t now =
        millis();

    bool cacheFresh =
        lastRtcTemperatureValid &&
        (uint32_t)(
            now -
            lastRtcTemperatureReadMs
        ) <
        10000UL;

    if (!cacheFresh) {
        float current = 0.0f;

        if (
            rtcReadTemperatureC(current) &&
            isfinite(current) &&
            current >= -40.0f &&
            current <= 85.0f
        ) {
            lastRtcTemperatureC = current;
            lastRtcTemperatureValid = true;
            lastRtcTemperatureReadMs = now;
        }
    }

    if (!lastRtcTemperatureValid) {
        temperatureC = 0.0f;
        return false;
    }

    temperatureC =
        lastRtcTemperatureC;

    return true;
}


bool thermalWarningActive()
{
    return
        thermalWarningState;
}


bool thermalEmergencyActive()
{
    return
        thermalEmergencyState;
}


static const char *thermalSourceNameForMask(
    uint8_t sourceMask
)
{
    if (
        (sourceMask & THERMAL_SOURCE_CPU) &&
        (sourceMask & THERMAL_SOURCE_RTC)
    ) {
        return "CPU+RTC";
    }

    if (sourceMask & THERMAL_SOURCE_CPU)
        return "CPU";

    if (sourceMask & THERMAL_SOURCE_RTC)
        return "RTC";

    return "NONE";
}


const char *thermalStateName()
{
    if (thermalEmergencyState)
        return "EMERGENCY";

    if (thermalWarningState)
        return "WARNING";

    return "OK";
}


const char *thermalSourceName()
{
    if (thermalEmergencyState) {
        return thermalSourceNameForMask(
            thermalEmergencySourceMask
        );
    }

    if (thermalWarningState) {
        return thermalSourceNameForMask(
            thermalWarningSourceMask
        );
    }

    return "NONE";
}


// =============================================================
// WIFI + TIME SYNC
// =============================================================

void wifiSyncTime() {

    if (cfg_wifi_ssid.length() == 0)
        return;

    Serial.println();
    Serial.println("Connecting to WiFi...");
    Serial.println("SSID: " + cfg_wifi_ssid);
    Serial.println("HOSTNAME: " + cfg_hostname);

    // Ensure the STA interface is fully down before assigning the
    // configured hostname. This makes sure DHCP sees cfg_hostname
    // when the interface is enabled again.
    WiFi.mode(WIFI_OFF);
    delay(50);

    bool hostnameOk =
        WiFi.setHostname(
            cfg_hostname.c_str()
        );

    Serial.println(
        "WiFi setHostname: " +
        String(
            hostnameOk
                ? "OK"
                : "FAILED"
        )
    );

    WiFi.mode(WIFI_STA);

    Serial.println(
        "WiFi hostname active: " +
        String(WiFi.getHostname())
    );

    WiFi.begin(
        cfg_wifi_ssid.c_str(),
        cfg_wifi_pass.c_str()
    );

    unsigned long start = millis();

    while (
        WiFi.status() != WL_CONNECTED &&
        millis() - start < 8000
    ) {
        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {

        Serial.println(
            "Connected. IP: " +
            WiFi.localIP().toString()
        );

        Serial.println(
            "Time zone: " +
            cfg_timezone
        );

        configTzTime(
            cfg_timezone.c_str(),
            "pool.ntp.org",
            "time.google.com"
        );

        // maximal ca. 4 Sekunden auf gültige Zeit warten
        unsigned long timeStart = millis();

        while (
            !timeIsValid() &&
            millis() - timeStart < 4000
        ) {
            delay(100);
        }

        if (timeIsValid()) {

            Serial.println("Time synchronized");

            // A successful NTP result is authoritative. If an optional
            // hardware RTC is present, refresh it in the background so the
            // next offline/power-cycle boot starts with a valid clock.
            if (rtcDetected()) {

                if (rtcSyncFromSystemTime()) {
                    Serial.println(
                        "RTC synchronized from system/NTP time"
                    );
                } else {
                    Serial.println(
                        "RTC synchronization FAILED"
                    );
                }
            }

        } else {

            Serial.println("Time sync timeout");
        }

    } else {

        Serial.println("WiFi connection FAILED");
    }

    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
}


// =============================================================
// TIME SYNC POLICY
// =============================================================

bool wifiRequestedOnSystemStart()
{
    if (cfg_wifi_on_system_start == "on") {
        return true;
    }

    if (cfg_wifi_on_system_start == "on_missing_time") {
        return !timeIsValid();
    }

    return false;
}


void maybeSyncTime(
    bool wifiRequestedAtBoot
) {
    if (!wifiRequestedAtBoot) {
        return;
    }

    // Only use the configured infrastructure WiFi when the clock is
    // actually missing. This is a single boot-time attempt; wifiSyncTime()
    // switches WiFi fully OFF again afterwards, so there is no background
    // reconnect loop.
    if (timeIsValid()) {
        return;
    }

    wifiSyncTime();
}


// =============================================================
// SD INIT
// =============================================================

// Keep SPI microSD deselected as early and as consistently as possible.
// This matters after an abrupt power loss: some cards need a clean CS=HIGH
// interval and idle clocks before they will answer a fresh SPI init sequence.
static void sdSpiDeselect()
{
#if defined(STORAGE_SPI)
    pinMode(
        SD_CS_PIN,
        OUTPUT
    );

    digitalWrite(
        SD_CS_PIN,
        HIGH
    );
#endif
}


static void sdSpiSendIdleClocks()
{
#if defined(STORAGE_SPI)
    // SD SPI power-up requires at least 74 clocks while CS is HIGH.
    // 16 bytes provide 128 clocks at a conservative 400 kHz.
    sdSpiDeselect();

    SPI.beginTransaction(
        SPISettings(
            SD_SPI_PRECLOCK_HZ,
            MSBFIRST,
            SPI_MODE0
        )
    );

    for (uint8_t i = 0; i < 16; ++i) {
        SPI.transfer(0xFF);
    }

    SPI.endTransaction();

    delay(2);
#endif
}


bool initSD() {

#if defined(STORAGE_SDMMC)

    Serial.println();
    Serial.println("Initializing SD_MMC...");
    Serial.printf(
        "CLK=%d CMD=%d D0=%d\n",
        SD_MMC_CLK,
        SD_MMC_CMD,
        SD_MMC_D0
    );

    STORAGE.end();

    SD_MMC.setPins(
        SD_MMC_CLK,
        SD_MMC_CMD,
        SD_MMC_D0
    );

    if (!SD_MMC.begin(
            "/sdcard",
            true,                  // 1-bit mode
            false,                 // no auto-format
            SDMMC_FREQ_DEFAULT,
            5
        )) {

        Serial.println("SD_MMC mount failed");
        return false;
    }

#elif defined(STORAGE_SPI)

    Serial.println();
    Serial.println("Initializing SPI SD...");
    Serial.printf(
        "CS=%d SCK=%d MISO=%d MOSI=%d\n",
        SD_CS_PIN,
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN
    );

    STORAGE.end();
    SPI.end();

    sdSpiDeselect();

    SPI.begin(
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    sdSpiSendIdleClocks();

    if (!SD.begin(
            SD_CS_PIN,
            SPI,
            SD_SPI_NORMAL_FREQUENCY_HZ,
            "/sd",
            5,
            false
        )) {

        Serial.println("SPI SD mount failed");
        return false;
    }

#else
    #error "Kein Storage-Typ in board_config.h definiert."
#endif

    uint8_t cardType = STORAGE.cardType();

    if (cardType == CARD_NONE) {
        Serial.println("No SD card detected");
        STORAGE.end();
        return false;
    }

    Serial.print("SD card: ");

    if (cardType == CARD_MMC)
        Serial.println("MMC");
    else if (cardType == CARD_SD)
        Serial.println("SDSC");
    else if (cardType == CARD_SDHC)
        Serial.println("SDHC");
    else
        Serial.println("UNKNOWN");

    uint64_t cardSize =
        STORAGE.cardSize() / (1024ULL * 1024ULL);

    Serial.printf(
        "SD size: %llu MB\n",
        cardSize
    );

    // A successful card init is not enough: verify that the mounted
    // filesystem root is actually readable before declaring SD ready.
    File root =
        STORAGE.open(
            "/",
            FILE_READ
        );

    if (
        !root ||
        !root.isDirectory()
    ) {
        if (root)
            root.close();

        Serial.println(
            "SD filesystem root unavailable after mount"
        );

        STORAGE.end();
#if defined(STORAGE_SPI)
        SPI.end();
        sdSpiDeselect();
#endif
        return false;
    }

    root.close();

    Serial.println(
        "SD filesystem root: OK"
    );

    return true;
}


// =============================================================
// ROBUST SD INIT / RETRY
// =============================================================
//
// A reset during an active SD write resets the ESP32 immediately,
// but does not power-cycle the SD card. The card can therefore still
// be internally busy when the ESP starts again.
//
// Retry with increasing delays and fully end the storage interface
// between failed attempts. This also helps transient SD_MMC 0x107
// initialization timeouts without changing normal recording behavior.

static void resetStorageInterface()
{
    STORAGE.end();

#if defined(STORAGE_SPI)
    SPI.end();

    // Leave the card explicitly deselected between attempts.
    sdSpiDeselect();
#endif
}


static uint32_t sdRetryDelayMs(
    uint8_t attempt
)
{
    switch (attempt) {

        case 1:
            return 200UL;

        case 2:
            return 500UL;

        case 3:
            return 1000UL;

        case 4:
            return 2000UL;

        default:
            return 4000UL;
    }
}


static bool initSDWithRetries(
    const char *context,
    uint8_t maxAttempts
)
{
    if (maxAttempts == 0)
        return false;


    for (
        uint8_t attempt = 1;
        attempt <= maxAttempts;
        ++attempt
    ) {

        uint32_t waitMs =
            sdRetryDelayMs(
                attempt
            );


        Serial.printf(
            "SD %s attempt %u/%u | wait=%lu ms\n",
            context,
            (unsigned)attempt,
            (unsigned)maxAttempts,
            (unsigned long)waitMs
        );


        resetStorageInterface();


        uint32_t waitStart =
            millis();

        while (
            (uint32_t)(
                millis() -
                waitStart
            ) <
            waitMs
        ) {

            // Safe before watchdog initialization because feedWatchdog()
            // checks watchdogEnabled internally.
            feedWatchdog();

            delay(10);
        }


        if (initSD()) {

            if (attempt > 1) {

                Serial.printf(
                    "SD %s recovered on attempt %u/%u\n",
                    context,
                    (unsigned)attempt,
                    (unsigned)maxAttempts
                );
            }

            return true;
        }


        Serial.printf(
            "SD %s attempt %u/%u failed\n",
            context,
            (unsigned)attempt,
            (unsigned)maxAttempts
        );


        resetStorageInterface();
    }


    Serial.printf(
        "SD %s failed after %u attempts\n",
        context,
        (unsigned)maxAttempts
    );

    return false;
}


// =============================================================
// SD FIRMWARE AUTO-UPDATE
// =============================================================
//
// Trigger: exactly ONE file ending in .bin inside /firmware.
//
// Accepted examples:
//   /firmware/sensorforge.ino.bin
//   /firmware/release_2026-09-08.bin
//
// Safety:
//   - 0 *.bin: normal boot
//   - >1 *.bin: no update (ambiguous)
//   - ESP image magic checked
//   - size checked against inactive OTA partition
//   - board-specific SENSORFORGE compatibility marker required
//   - candidate renamed before flash to prevent automatic reflash loops
//
// Success: original.bin -> original.bin.done
// Failure: original.bin -> original.bin.failed
// Interrupted update: update.inprogress -> update.interrupted.failed

static const char *FW_UPDATE_DIR =
    "/firmware";

static const char *FW_UPDATE_WORK =
    "/firmware/update.inprogress";

static const char *FW_UPDATE_INTERRUPTED =
    "/firmware/update.interrupted.failed";


#ifdef BOARD_FREENOVE
static const char FW_COMPAT_MARKER[] =
    "SENSORFORGE|ESP32S3|FREENOVE_FNK0085|COMPAT=1";
static const char FW_COMPAT_MARKER_LEGACY[] =
    "OLDNAME_SENSORCAM|ESP32S3|FREENOVE_FNK0085|COMPAT=1";
#elif defined(BOARD_XIAO)
static const char FW_COMPAT_MARKER[] =
    "SENSORFORGE|ESP32S3|XIAO_SENSE|COMPAT=1";
static const char FW_COMPAT_MARKER_LEGACY[] =
    "OLDNAME_SENSORCAM|ESP32S3|XIAO_SENSE|COMPAT=1";

#else
static const char FW_COMPAT_MARKER[] =
    "SENSORFORGE|ESP32S3|UNKNOWN_BOARD|COMPAT=1";
static const char FW_COMPAT_MARKER_LEGACY[] =
    "OLDNAME_SENSORCAM|ESP32S3|UNKNOWN_BOARD|COMPAT=1";
#endif


static bool firmwareUpdateLedActive =
    false;

static uint32_t firmwareUpdateLedLastToggleMs =
    0;


static void firmwareUpdateLedStart()
{
    statusLedSetOutput();
    statusLedWriteLevel(
        LED_ON_LEVEL
    );

    firmwareUpdateLedActive = true;
    firmwareUpdateLedLastToggleMs = millis();
}


static void firmwareUpdateLedService()
{
    if (!firmwareUpdateLedActive)
        return;

    uint32_t now = millis();

    if ((uint32_t)(now - firmwareUpdateLedLastToggleMs) < 100UL)
        return;

    firmwareUpdateLedLastToggleMs = now;

    int currentLevel =
        statusLedReadLevel();

    statusLedWriteLevel(
        currentLevel == LED_ON_LEVEL
        ? LED_OFF_LEVEL
        : LED_ON_LEVEL
    );
}


static void firmwareUpdateLedStop()
{
    statusLedSetOutput();
    statusLedWriteLevel(
        LED_OFF_LEVEL
    );
    firmwareUpdateLedActive = false;
}


static String firmwareBaseName(const String &path)
{
    int slash = path.lastIndexOf('/');

    if (slash >= 0)
        return path.substring(slash + 1);

    return path;
}


static String firmwareStatusPath(
    const String &sourceFilename,
    const char *suffix
)
{
    return
        String(FW_UPDATE_DIR) +
        "/" +
        firmwareBaseName(sourceFilename) +
        suffix;
}


static void firmwareMoveWorkTo(
    const String &destination
)
{
    if (!STORAGE.exists(FW_UPDATE_WORK))
        return;

    if (STORAGE.exists(destination.c_str()))
        STORAGE.remove(destination.c_str());

    if (!STORAGE.rename(
            FW_UPDATE_WORK,
            destination.c_str()
        )) {

        Serial.println(
            "Firmware update: WARNING - could not archive update.inprogress as " +
            destination
        );
    }
}


static void firmwareUpdateHandleInterruptedFile()
{
    if (!STORAGE.exists(FW_UPDATE_WORK))
        return;

    Serial.println(
        "Firmware update: stale update.inprogress found - previous update was interrupted"
    );

    firmwareMoveWorkTo(
        String(FW_UPDATE_INTERRUPTED)
    );
}


static bool firmwareFindSingleCandidate(
    String &candidatePath,
    String &candidateName,
    String &error
)
{
    candidatePath = "";
    candidateName = "";
    error = "";

    File directory =
        STORAGE.open(
            FW_UPDATE_DIR,
            FILE_READ
        );

    if (!directory)
        return false;

    if (!directory.isDirectory()) {
        directory.close();
        error = "/firmware exists but is not a directory";
        return false;
    }

    int candidateCount = 0;

    File entry =
        directory.openNextFile();

    while (entry) {

        if (!entry.isDirectory()) {

            String entryName =
                String(entry.name());

            String lower =
                entryName;

            lower.toLowerCase();

            if (lower.endsWith(".bin")) {

                ++candidateCount;

                if (candidateCount == 1) {

                    candidateName =
                        firmwareBaseName(
                            entryName
                        );

                    candidatePath =
                        entryName.startsWith("/")
                        ? entryName
                        : (
                            String(FW_UPDATE_DIR) +
                            "/" +
                            entryName
                        );
                }
            }
        }

        entry.close();

        if (candidateCount > 1)
            break;

        entry =
            directory.openNextFile();
    }

    directory.close();

    if (candidateCount == 0)
        return false;

    if (candidateCount > 1) {
        candidatePath = "";
        candidateName = "";
        error =
            "more than one .bin file found in /firmware - update skipped";
        return false;
    }

    return true;
}


static bool firmwareFileContainsMarker(
    File &firmware,
    const char *marker
)
{
    if (!marker)
        return false;

    const size_t markerLength =
        strlen(marker);

    if (
        markerLength == 0 ||
        markerLength >= 128U
    ) {
        return false;
    }

    static uint8_t scanBuffer[4096 + 128];

    size_t carry = 0;

    if (!firmware.seek(0))
        return false;

    while (firmware.available()) {

        size_t got =
            firmware.read(
                scanBuffer + carry,
                4096
            );

        if (got == 0)
            break;

        size_t total =
            carry + got;

        if (total >= markerLength) {

            size_t lastStart =
                total - markerLength;

            for (
                size_t i = 0;
                i <= lastStart;
                ++i
            ) {

                if (
                    memcmp(
                        scanBuffer + i,
                        marker,
                        markerLength
                    ) == 0
                ) {

                    firmware.seek(0);
                    return true;
                }
            }
        }

        carry =
            total < (markerLength - 1U)
            ? total
            : (markerLength - 1U);

        if (carry > 0) {
            memmove(
                scanBuffer,
                scanBuffer + total - carry,
                carry
            );
        }

        yield();
    }

    firmware.seek(0);
    return false;
}


static bool firmwareFileContainsCompatMarker(
    File &firmware
)
{
    // Current SensorForge releases.
    if (
        firmwareFileContainsMarker(
            firmware,
            FW_COMPAT_MARKER
        )
    ) {
        return true;
    }

    // Temporary bridge compatibility with old releases with different names.
    if (
        firmwareFileContainsMarker(
            firmware,
            FW_COMPAT_MARKER_LEGACY
        )
    ) {
        return true;
    }

    return false;
}



static bool firmwareValidateCandidate(
    const String &candidatePath,
    size_t &firmwareSize,
    String &error
)
{
    error = "";
    firmwareSize = 0;

    File firmware =
        STORAGE.open(
            candidatePath.c_str(),
            FILE_READ
        );

    if (!firmware) {
        error = "cannot open firmware candidate";
        return false;
    }

    firmwareSize = firmware.size();

    const esp_partition_t *targetPartition =
        esp_ota_get_next_update_partition(
            nullptr
        );

    if (!targetPartition) {
        firmware.close();
        error = "no inactive OTA application partition found";
        return false;
    }

    if (
        firmwareSize == 0 ||
        firmwareSize > targetPartition->size
    ) {
        firmware.close();
        error = "firmware file is empty or too large for OTA partition";
        return false;
    }

    int firstByte =
        firmware.read();

    if (firstByte != 0xE9) {
        firmware.close();
        error = "invalid ESP32 application image header";
        return false;
    }

    if (!firmwareFileContainsCompatMarker(firmware)) {
        firmware.close();
        error = "firmware compatibility marker missing/wrong board";
        return false;
    }

    firmware.close();
    return true;
}


// Public read-only helpers for WebConfig's staged WiFi firmware upload.
// The web layer deliberately reuses the exact same validator as the SD
// auto-update path so both installation paths enforce identical image,
// partition-size and board-compatibility checks.
bool firmwareValidateStagedImage(
    const String &candidatePath,
    size_t &firmwareSize,
    String &error
)
{
    return
        firmwareValidateCandidate(
            candidatePath,
            firmwareSize,
            error
        );
}


size_t firmwareInactiveOtaCapacity()
{
    const esp_partition_t *targetPartition =
        esp_ota_get_next_update_partition(
            nullptr
        );

    return
        targetPartition
        ? targetPartition->size
        : 0U;
}


const char *firmwareExpectedCompatibilityMarker()
{
    return
        FW_COMPAT_MARKER;
}


static bool firmwareUpdateFromSdIfPresent()
{
    if (!sdReady)
        return false;

    firmwareUpdateHandleInterruptedFile();

    String candidatePath;
    String candidateName;
    String findError;

    bool found =
        firmwareFindSingleCandidate(
            candidatePath,
            candidateName,
            findError
        );

    if (!found) {

        if (findError.length()) {
            Serial.println(
                "Firmware update: " +
                findError
            );
        }

        return false;
    }

    Serial.println();
    Serial.println(
        "Firmware update: candidate found: " +
        candidatePath
    );

    size_t firmwareSize = 0;
    String validationError;

    if (!firmwareValidateCandidate(
            candidatePath,
            firmwareSize,
            validationError
        )) {

        Serial.println(
            "Firmware update: REJECTED - " +
            validationError
        );

        return false;
    }

    const esp_partition_t *targetPartition =
        esp_ota_get_next_update_partition(
            nullptr
        );

    Serial.printf(
        "Firmware update: file=%u bytes | target=%s | capacity=%u bytes\n",
        (unsigned)firmwareSize,
        targetPartition ? targetPartition->label : "<none>",
        targetPartition ? (unsigned)targetPartition->size : 0U
    );

    Serial.println(
        "Firmware update: compatibility OK: " +
        String(FW_COMPAT_MARKER)
    );

    // Rename before flash: after a power loss there is no .bin left
    // that could be flashed repeatedly on every boot.
    if (!STORAGE.rename(
            candidatePath.c_str(),
            FW_UPDATE_WORK
        )) {

        Serial.println(
            "Firmware update: ERROR - could not rename candidate to update.inprogress"
        );

        return false;
    }

    firmwareUpdateLedStart();

    File firmware =
        STORAGE.open(
            FW_UPDATE_WORK,
            FILE_READ
        );

    String failedPath =
        firmwareStatusPath(
            candidateName,
            ".failed"
        );

    String donePath =
        firmwareStatusPath(
            candidateName,
            ".done"
        );

    if (!firmware) {

        Serial.println(
            "Firmware update: ERROR - cannot open update.inprogress"
        );

        firmwareUpdateLedStop();
        firmwareMoveWorkTo(failedPath);
        return false;
    }

    if (!Update.begin(
            firmwareSize,
            U_FLASH
        )) {

        Serial.print(
            "Firmware update: ERROR - Update.begin failed: "
        );

        Serial.println(
            Update.errorString()
        );

        firmware.close();
        firmwareUpdateLedStop();
        firmwareMoveWorkTo(failedPath);
        return false;
    }

    Serial.println(
        "Firmware update: flashing inactive OTA partition..."
    );

    static uint8_t updateBuffer[4096];

    size_t totalWritten = 0;
    int lastProgress = -1;
    bool writeOk = true;

    while (totalWritten < firmwareSize) {

        size_t remaining =
            firmwareSize - totalWritten;

        size_t requested =
            remaining < sizeof(updateBuffer)
            ? remaining
            : sizeof(updateBuffer);

        size_t got =
            firmware.read(
                updateBuffer,
                requested
            );

        if (got == 0) {
            writeOk = false;
            Serial.println(
                "Firmware update: ERROR - unexpected end/read failure"
            );
            break;
        }

        size_t written =
            Update.write(
                updateBuffer,
                got
            );

        if (written != got) {
            writeOk = false;
            Serial.print(
                "Firmware update: ERROR - flash write failed: "
            );
            Serial.println(
                Update.errorString()
            );
            break;
        }

        totalWritten += written;

        int progress =
            (int)(
                (totalWritten * 100ULL) /
                firmwareSize
            );

        if (progress / 10 != lastProgress / 10) {
            lastProgress = progress;
            Serial.printf(
                "Firmware update: %d%%\n",
                progress
            );
        }

        firmwareUpdateLedService();
        yield();
    }

    firmware.close();

    if (!writeOk) {
        Update.abort();
        firmwareUpdateLedStop();
        firmwareMoveWorkTo(failedPath);
        return false;
    }

    if (!Update.end(false)) {

        Serial.print(
            "Firmware update: ERROR - final verification/activation failed: "
        );

        Serial.println(
            Update.errorString()
        );

        Update.abort();
        firmwareUpdateLedStop();
        firmwareMoveWorkTo(failedPath);
        return false;
    }

    if (!Update.isFinished()) {

        Serial.println(
            "Firmware update: ERROR - update did not finish completely"
        );

        firmwareUpdateLedStop();
        firmwareMoveWorkTo(failedPath);
        return false;
    }

    Serial.println(
        "Firmware update: image written and verified successfully"
    );

    if (!firmwareInfoMarkSdUpdate(
            candidateName
        )) {

        Serial.println(
            "Firmware update: WARNING - could not persist installation metadata"
        );
    }

    firmwareMoveWorkTo(donePath);

    Serial.println(
        "Firmware update: archived as " +
        donePath
    );

    Serial.println(
        "Firmware update: restarting into new firmware..."
    );

    firmwareUpdateLedStop();
    Serial.flush();
    STORAGE.end();
    delay(250);
    ESP.restart();

    return true;
}


// =============================================================
// SD CONFIG CHECK AFTER RECOVERY
// =============================================================

static bool recoveredSdHasValidConfig(
    String &error
)
{
    error = "";

    File file =
        STORAGE.open(
            "/config.txt",
            FILE_READ
        );

    if (!file) {
        error =
            "missing /config.txt";
        return false;
    }


    const size_t size =
        file.size();

    if (
        size == 0 ||
        size > 32768U
    ) {
        file.close();

        error =
            "invalid config size";

        return false;
    }


    String text;

    if (!text.reserve(size + 1U)) {
        file.close();

        error =
            "config buffer allocation failed";

        return false;
    }


    while (file.available()) {
        int value =
            file.read();

        if (value < 0)
            break;

        text +=
            (char)value;

        feedWatchdog();
    }


    file.close();


    if (text.length() != size) {
        error =
            "config read incomplete";

        return false;
    }


    return
        configValidateText(
            text,
            error
        );
}


// =============================================================
// SD RECOVERY
// =============================================================

bool recoverSD()
{
    Serial.println(
        "Attempting SD recovery..."
    );

    logClose();

    resetStorageInterface();

    sdReady =
        false;


    if (
        !initSDWithRetries(
            "recovery",
            SD_RUNTIME_RECOVERY_MAX_ATTEMPTS
        )
    ) {

        Serial.println(
            "SD recovery failed"
        );

        return false;
    }


    sdReady =
        true;

    logInit();

    logWrite(
        "SD recovery successful"
    );


    int recoveredFiles =
        storageRecoverIncompleteRecordings();

    if (recoveredFiles > 0) {

        logWrite(
            "SD recovery removed " +
            String(recoveredFiles) +
            " incomplete recording file(s)"
        );
    }


    if (bootStartedWithoutSd) {

        String configError;

        if (
            recoveredSdHasValidConfig(
                configError
            )
        ) {

            Serial.println(
                "SD recovery: valid /config.txt found after SD-less boot"
            );

            Serial.println(
                "SD recovery: restarting to restore SD config priority"
            );

            logWrite(
                "SD recovery: valid config found after SD-less boot - restarting"
            );

            logFlush();
            logClose();

            STORAGE.end();

            sdReady =
                false;


            // Allow the card and UART output to settle before reset.
            uint32_t restartWaitMs =
                millis();

            while (
                (uint32_t)(
                    millis() -
                    restartWaitMs
                ) <
                500UL
            ) {
                feedWatchdog();
                delay(10);
            }


            ESP.restart();

            return true;
        }


        Serial.println(
            "SD recovery: no valid SD config for priority restart (" +
            configError +
            ")"
        );

        logWrite(
            "SD recovery: no valid SD config for priority restart"
        );
    }


    return true;
}


// =============================================================
// CAMERA INIT
// =============================================================


// -------------------------------------------------------------
// OV3660 SENSOR CROP
// -------------------------------------------------------------
//
// The normal XGA path uses the complete 2048x1536 4:3 OV3660 image area and
// 2x binning to produce 1024x768. For 1.5x/2.0x crop we instead select a
// smaller raw 4:3 sensor window and keep the configured JPEG output dimensions.
// This spends the same output pixels on a smaller field of view without
// increasing the nominal recording resolution.
//
// The raw geometry constants below are the OV3660 driver's official 4:3
// settings. We preserve its blanking/timing values and output offsets.
// 1.0x deliberately calls the driver's standard set_framesize() path so the
// exact legacy behavior (including binning/scaling policy) is restored.

static bool cameraCropOutputSize(
    const String &resolutionText,
    uint16_t &width,
    uint16_t &height
)
{
    if (resolutionText == "160x120") {
        width = 160;
        height = 120;
    } else if (resolutionText == "320x240") {
        width = 320;
        height = 240;
    } else if (resolutionText == "640x480") {
        width = 640;
        height = 480;
    } else if (resolutionText == "800x600") {
        width = 800;
        height = 600;
    } else if (resolutionText == "1024x768") {
        width = 1024;
        height = 768;
    } else {
        width = 0;
        height = 0;
        return false;
    }

    return true;
}


static uint16_t cameraCropAxisStart(
    uint16_t maximumStart,
    int position
)
{
    uint16_t value = 0;

    if (position <= 0) {
        value = 0;
    } else if (position >= 2) {
        value = maximumStart;
    } else {
        value =
            maximumStart / 2U;
    }

    // OV3660 crop coordinates are kept on an even boundary.
    return
        (uint16_t)(
            value &
            0xFFFEU
        );
}


static int cameraEffectiveRotationDegrees()
{
    int rotation =
        cfg_rotation +
        CAMERA_BASE_ROTATION_DEGREES;

    rotation %= 360;

    if (rotation < 0)
        rotation += 360;

    return rotation;
}


static void cameraDiscardQueuedFramesAfterCrop()
{
    if (
        !cameraInitialized ||
        cameraFramebufferCount == 0
    ) {
        return;
    }

    // Current SensorForge camera config uses at most two frame buffers.
    // Holding every queued buffer before returning any of them guarantees that
    // frames captured with the previous crop cannot leak into a recording.
    camera_fb_t *held[2] = {
        nullptr,
        nullptr
    };

    uint8_t wanted =
        cameraFramebufferCount;

    if (wanted > 2)
        wanted = 2;

    uint8_t count = 0;

    for (
        uint8_t i = 0;
        i < wanted;
        ++i
    ) {

        feedWatchdog();
        yield();

        camera_fb_t *fb =
            esp_camera_fb_get();

        feedWatchdog();

        if (!fb)
            break;

        held[count++] =
            fb;
    }

    for (
        uint8_t i = 0;
        i < count;
        ++i
    ) {

        esp_camera_fb_return(
            held[i]
        );
    }
}


bool cameraApplyCropRuntime(
    const String &zoom,
    int positionX,
    int positionY,
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
            "crop zoom must be 1.0, 1.5 or 2.0";

        return false;
    }

    if (
        positionX < 0 ||
        positionX > 2 ||
        positionY < 0 ||
        positionY > 2
    ) {

        error =
            "crop position must be in range 0..2";

        return false;
    }

    if (!cameraInitialized) {

        error =
            "camera is not initialized";

        return false;
    }


    sensor_t *sensor =
        esp_camera_sensor_get();

    if (!sensor) {

        error =
            "camera sensor is unavailable";

        return false;
    }


    if (sensor->id.PID != 0x3660) {

        error =
            "sensor crop is supported only on detected OV3660";

        return false;
    }


    if (!sensor->set_framesize) {

        error =
            "camera driver does not expose set_framesize";

        return false;
    }


    // 1.0x is intentionally the driver's untouched standard path. This also
    // provides a deterministic restore after an unsaved Live Preview test.
    if (normalizedZoom == "1.0") {

        int result =
            sensor->set_framesize(
                sensor,
                getFrameSize(
                    cfg_resolution
                )
            );

        if (result != 0) {

            error =
                "OV3660 full-frame restore failed";

            return false;
        }

        cameraDiscardQueuedFramesAfterCrop();

        return true;
    }


    if (!sensor->set_res_raw) {

        error =
            "camera driver does not expose OV3660 raw crop";

        return false;
    }


    uint16_t outputWidth = 0;
    uint16_t outputHeight = 0;

    if (!cameraCropOutputSize(
            cfg_resolution,
            outputWidth,
            outputHeight
        )) {

        error =
            "sensor crop requires a 4:3 output up to 1024x768";

        return false;
    }


    uint16_t cropWidth =
        normalizedZoom == "1.5"
        ? 1360U
        : 1024U;

    uint16_t cropHeight =
        normalizedZoom == "1.5"
        ? 1020U
        : 768U;


    if (
        outputWidth > cropWidth ||
        outputHeight > cropHeight
    ) {

        error =
            "configured output is larger than selected crop window";

        return false;
    }


    // Crop position is defined in DISPLAY coordinates. The board-specific
    // camera mounting correction and the user-selected rotation are combined
    // into one effective sensor rotation. If that effective rotation is 180
    // degrees, both raw axes must be inverted so that e.g. "top-left" still
    // means top-left in Live Preview and recordings.
    int rawPositionX =
        positionX;

    int rawPositionY =
        positionY;

    if (cameraEffectiveRotationDegrees() == 180) {
        rawPositionX =
            2 - rawPositionX;

        rawPositionY =
            2 - rawPositionY;
    }


    static const uint16_t OV3660_ACTIVE_WIDTH = 2048U;
    static const uint16_t OV3660_ACTIVE_HEIGHT = 1536U;

    static const uint16_t OV3660_WINDOW_EXTRA_X = 32U;
    static const uint16_t OV3660_WINDOW_EXTRA_Y = 12U;

    static const uint16_t OV3660_OFFSET_X = 16U;
    static const uint16_t OV3660_OFFSET_Y = 6U;

    static const uint16_t OV3660_TOTAL_X = 2300U;
    static const uint16_t OV3660_TOTAL_Y = 1564U;


    uint16_t maximumStartX =
        OV3660_ACTIVE_WIDTH -
        cropWidth;

    uint16_t maximumStartY =
        OV3660_ACTIVE_HEIGHT -
        cropHeight;


    uint16_t startX =
        cameraCropAxisStart(
            maximumStartX,
            rawPositionX
        );

    uint16_t startY =
        cameraCropAxisStart(
            maximumStartY,
            rawPositionY
        );


    uint16_t endX =
        (uint16_t)(
            startX +
            cropWidth +
            OV3660_WINDOW_EXTRA_X -
            1U
        );

    uint16_t endY =
        (uint16_t)(
            startY +
            cropHeight +
            OV3660_WINDOW_EXTRA_Y -
            1U
        );


    bool scale =
        outputWidth != cropWidth ||
        outputHeight != cropHeight;


    int result =
        sensor->set_res_raw(
            sensor,
            startX,
            startY,
            endX,
            endY,
            OV3660_OFFSET_X,
            OV3660_OFFSET_Y,
            OV3660_TOTAL_X,
            OV3660_TOTAL_Y,
            outputWidth,
            outputHeight,
            scale,
            false
        );


    if (result != 0) {

        // Fail back to the standard configured frame size. A crop failure must
        // never leave the camera in a partially programmed state.
        sensor->set_framesize(
            sensor,
            getFrameSize(
                cfg_resolution
            )
        );

        cameraDiscardQueuedFramesAfterCrop();

        error =
            "OV3660 raw crop programming failed";

        return false;
    }


    cameraDiscardQueuedFramesAfterCrop();

    return true;
}


bool initCamera(
    const String &model,
    const String &resolution,
    int quality
) {

    // Kamera läuft bereits, z. B. für WebConfig/Preview.
    if (cameraInitialized)
        return true;

    camera_pins_t pins = selectCamera(model);

    // Wichtig: Struktur komplett initialisieren
    camera_config_t cfg = {};

    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;

    cfg.pin_pwdn     = pins.pin_pwdn;
    cfg.pin_reset    = pins.pin_reset;

    cfg.pin_xclk     = pins.pin_xclk;

    cfg.pin_sccb_sda = pins.pin_sscb_sda;
    cfg.pin_sccb_scl = pins.pin_sscb_scl;

    cfg.pin_d7       = pins.pin_d7;
    cfg.pin_d6       = pins.pin_d6;
    cfg.pin_d5       = pins.pin_d5;
    cfg.pin_d4       = pins.pin_d4;
    cfg.pin_d3       = pins.pin_d3;
    cfg.pin_d2       = pins.pin_d2;
    cfg.pin_d1       = pins.pin_d1;
    cfg.pin_d0       = pins.pin_d0;

    cfg.pin_vsync    = pins.pin_vsync;
    cfg.pin_href     = pins.pin_href;
    cfg.pin_pclk     = pins.pin_pclk;

#if defined(BOARD_FREENOVE) || defined(BOARD_XIAO)
    // Configurable camera master clock. config.cpp validates the value to
    // 10/16/20 MHz. Legacy defaults remain 10 MHz on Freenove and 20 MHz
    // on XIAO when camera_xclk_mhz is absent from an older config.txt.
    cfg.xclk_freq_hz =
        (uint32_t)cfg_camera_xclk_mhz * 1000000UL;
#endif

    cfg.frame_size =
        getFrameSize(resolution);

    cfg.jpeg_quality =
        quality;

    cfg.pixel_format =
        PIXFORMAT_JPEG;

    if (psramFound()) {

        if (cfg_debug_enabled) {
            Serial.println("PSRAM detected");
        }

        cfg.fb_location =
            CAMERA_FB_IN_PSRAM;

        cfg.fb_count = 2;

    } else {

        Serial.println(
            "WARNING: PSRAM not detected"
        );

        cfg.fb_location =
            CAMERA_FB_IN_DRAM;

        cfg.fb_count = 1;
    }

    cfg.grab_mode =
        CAMERA_GRAB_WHEN_EMPTY;


    esp_err_t err =
        esp_camera_init(&cfg);

    if (err != ESP_OK) {

        Serial.printf(
            "Camera init failed: 0x%x\n",
            err
        );

        cameraInitialized = false;
        return false;
    }
    
    sensor_t *sensor = esp_camera_sensor_get();

    if (sensor) {

        int exposureResult = 0;
        int aeLevelResult = 0;

        if (sensor->set_exposure_ctrl) {
            exposureResult =
                sensor->set_exposure_ctrl(
                    sensor,
                    cfg_camera_auto_exposure
                );
        }

        if (sensor->set_ae_level) {
            aeLevelResult =
                sensor->set_ae_level(
                    sensor,
                    cfg_camera_ae_level
                );
        }

        if (cfg_debug_enabled) {
            Serial.printf(
                "Camera tuning: quality=%d auto_exposure=%d ae_level=%d\n",
                quality,
                cfg_camera_auto_exposure,
                cfg_camera_ae_level
            );
        }

        if (
            exposureResult != 0 ||
            aeLevelResult != 0
        ) {
            Serial.printf(
                "WARNING: camera exposure tuning returned exposure=%d ae_level=%d\n",
                exposureResult,
                aeLevelResult
            );
        }


        const int effectiveRotation =
            cameraEffectiveRotationDegrees();

        switch (effectiveRotation) {

            case 0:
                sensor->set_hmirror(sensor, 0);
                sensor->set_vflip(sensor, 0);
                break;

            case 180:
                sensor->set_hmirror(sensor, 1);
                sensor->set_vflip(sensor, 1);
                break;

            default:
                // Config validation currently allows only 0/180 degrees and
                // each board profile allows only a 0/180-degree base rotation.
                // Keep a deterministic fallback if those invariants are ever
                // changed without updating this camera path.
                sensor->set_hmirror(sensor, 0);
                sensor->set_vflip(sensor, 0);

                Serial.printf(
                    "Effective camera rotation %d unsupported | config=%d base=%d | using 0 degrees\n",
                    effectiveRotation,
                    cfg_rotation,
                    CAMERA_BASE_ROTATION_DEGREES
                );
                break;
        }
    }





    cameraInitialized = true;
    cameraFramebufferCount = cfg.fb_count;
    cameraSoftPowerDownActive = false;

    // Cache the capability by the actually detected sensor, not by board or
    // config string. This cache deliberately survives camera deinit so an
    // unsupported sensor is not reinitialized merely to test standby again.
    if (sensor) {
        cameraDetectedPid =
            sensor->id.PID;

        cameraFastSleepSupported =
            sensor->set_reg &&
            cameraDetectedPid == 0x3660;

        cameraFastSleepCapabilityKnown =
            true;
    }


    // Apply the persisted sensor crop only after the physical PID is known and
    // after rotation has been configured. 1.0x is deliberately a no-op here so
    // an upgraded installation keeps the exact legacy camera-init path.
    if (cfg_camera_crop_zoom != "1.0") {

        String cropError;

        if (!cameraApplyCropRuntime(
                cfg_camera_crop_zoom,
                cfg_camera_crop_x,
                cfg_camera_crop_y,
                cropError
            )) {

            Serial.println(
                "WARNING: camera crop not applied | " +
                cropError
            );

        } else if (cfg_debug_enabled) {

            Serial.println(
                "Camera crop: zoom=" +
                cfg_camera_crop_zoom +
                " x=" +
                String(cfg_camera_crop_x) +
                " y=" +
                String(cfg_camera_crop_y)
            );
        }
    }

    if (cfg_debug_enabled) {
        Serial.println("Camera initialized");
    }

    return true;
}


// =============================================================
// OV3660 FAST LIGHT-SLEEP POWER CONTROL
// =============================================================
//
// OV3660 register 0x3008 bit 6 is software power-down. Keeping the
// esp_camera driver initialized avoids the full camera-init latency after
// a light-sleep wake. XCLK/SCCB remain owned by the camera driver.

static bool cameraSupportsFastLightSleepNow()
{
    if (!cameraInitialized)
        return false;

    sensor_t *sensor =
        esp_camera_sensor_get();

    if (!sensor)
        return false;

    cameraDetectedPid =
        sensor->id.PID;

    cameraFastSleepSupported =
        sensor->set_reg &&
        cameraDetectedPid == 0x3660;

    cameraFastSleepCapabilityKnown =
        true;

    return cameraFastSleepSupported;
}


static void cameraDeinitRuntime()
{
    if (cameraInitialized) {
        esp_camera_deinit();
    }

    cameraInitialized = false;
    cameraFramebufferCount = 0;
    cameraSoftPowerDownActive = false;
}


// =============================================================
// TRANSPORT MODE / BLACK-COVER DETECTION
// =============================================================
//
// Transport mode is deliberately isolated from the normal recording camera
// path. It uses a single low-resolution grayscale framebuffer only long enough
// to decide whether the lens is still covered. Radar, PIR wake and WiFi are
// never started while this mode is active.

struct TransportImageMetrics {
    float mean;
    uint8_t p95;
};


// Explicit prototypes: prevent Arduino .ino auto-prototype generation
// from placing prototypes before TransportImageMetrics is declared.
static bool transportMeasureImage(
    TransportImageMetrics &metrics,
    String &error
);

static bool transportImageIsBlack(
    const TransportImageMetrics &metrics
);


// The normal logger is deliberately not opened on the timer-only transport
// path. These helpers append short durable events directly to the same logfile
// and immediately close the file again. That keeps the transport history
// reconstructable without starting the full logger/radar/WiFi stack.
static const uint64_t TRANSPORT_JOURNAL_ROTATE_BYTES =
    5ULL * 1024ULL * 1024ULL;


static String transportJournalPath()
{
    return
        cfg_log_file.length()
        ? cfg_log_file
        : String("/log.txt");
}


static String transportJournalBackupPath(
    const String &path
)
{
    return logStorageRotatedPath(path);
}


static bool transportLoadPersistentStartEpoch(
    int64_t &epoch
)
{
    epoch =
        0;

    if (!LittleFS.exists(TRANSPORT_STATE_FILE))
        return false;

    File file =
        LittleFS.open(
            TRANSPORT_STATE_FILE,
            FILE_READ
        );

    if (!file)
        return false;

    String text =
        file.readStringUntil('\n');

    file.close();
    text.trim();

    if (!text.length())
        return false;

    char *endPtr =
        nullptr;

    long long parsed =
        strtoll(
            text.c_str(),
            &endPtr,
            10
        );

    if (
        !endPtr ||
        *endPtr != '\0' ||
        parsed < 1609459200LL
    ) {
        return false;
    }

    epoch =
        (int64_t)parsed;

    return true;
}


static bool transportStorePersistentStartEpoch(
    int64_t epoch
)
{
    if (epoch < 1609459200LL)
        return false;

    LittleFS.remove(
        TRANSPORT_STATE_TEMP
    );

    File file =
        LittleFS.open(
            TRANSPORT_STATE_TEMP,
            FILE_WRITE
        );

    if (!file)
        return false;

    char stored[32];
    int storedLength =
        snprintf(
            stored,
            sizeof(stored),
            "%lld\n",
            (long long)epoch
        );

    size_t written =
        storedLength > 0
        ? file.print(stored)
        : 0;

    file.flush();
    file.close();

    if (
        storedLength <= 0 ||
        written != (size_t)storedLength
    ) {
        LittleFS.remove(
            TRANSPORT_STATE_TEMP
        );
        return false;
    }

    File verify =
        LittleFS.open(
            TRANSPORT_STATE_TEMP,
            FILE_READ
        );

    if (!verify) {
        LittleFS.remove(
            TRANSPORT_STATE_TEMP
        );
        return false;
    }

    String verifyText =
        verify.readStringUntil('\n');

    verify.close();
    verifyText.trim();

    char expected[32];
    snprintf(
        expected,
        sizeof(expected),
        "%lld",
        (long long)epoch
    );

    if (verifyText != expected) {
        LittleFS.remove(
            TRANSPORT_STATE_TEMP
        );
        return false;
    }

    LittleFS.remove(
        TRANSPORT_STATE_FILE
    );

    if (!LittleFS.rename(
            TRANSPORT_STATE_TEMP,
            TRANSPORT_STATE_FILE
        )) {
        LittleFS.remove(
            TRANSPORT_STATE_TEMP
        );
        return false;
    }

    return true;
}


static void transportClearPersistentStartEpoch()
{
    LittleFS.remove(
        TRANSPORT_STATE_TEMP
    );

    LittleFS.remove(
        TRANSPORT_STATE_FILE
    );
}


static void transportPrepareJournalClock()
{
    // Deep sleep normally preserves ESP system time. If it is not valid (for
    // example after a real power interruption), use the fitted DS3231 to
    // restore wall-clock time before writing the journal entry.
    if (!timeIsValid()) {
        rtcBegin();
    }

    if (cfg_timezone.length()) {
        setenv(
            "TZ",
            cfg_timezone.c_str(),
            1
        );
        tzset();
    }
}


static void transportJournalTimestamp(
    char *buffer,
    size_t bufferSize
)
{
    if (!buffer || bufferSize == 0)
        return;

    time_t now =
        time(nullptr);

    if (now >= (time_t)1609459200) {
        struct tm localTime;

        if (localtime_r(
                &now,
                &localTime
            )) {
            if (strftime(
                    buffer,
                    bufferSize,
                    "%Y-%m-%d %H:%M:%S",
                    &localTime
                )) {
                return;
            }
        }
    }

    // Fallback remains unambiguous even if no valid RTC/time source exists:
    // the transport wake-cycle number survives timer deep sleep.
    snprintf(
        buffer,
        bufferSize,
        "TR#%06lu+%lu.%03lu",
        (unsigned long)transportRtcWakeCount,
        (unsigned long)(millis() / 1000UL),
        (unsigned long)(millis() % 1000UL)
    );
}


static uint64_t transportJournalEstimatedPhysicalBytes(
    size_t plaintextBytes
)
{
    if (!cfg_recording_encryption)
        return plaintextBytes;

    uint64_t records =
        (plaintextBytes + 1023U) / 1024U;

    return
        (uint64_t)plaintextBytes +
        records * 32ULL;
}


static void transportJournalRotateIfNeeded(
    const String &path,
    size_t nextPlaintextBytes
)
{
    if (!sdReady)
        return;

    File current =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!current)
        return;

    uint64_t currentSize =
        current.size();

    current.close();

    if (
        currentSize +
        transportJournalEstimatedPhysicalBytes(
            nextPlaintextBytes
        ) <=
        TRANSPORT_JOURNAL_ROTATE_BYTES
    ) {
        return;
    }

    String backup =
        transportJournalBackupPath(
            path
        );

    if (STORAGE.exists(backup.c_str())) {
        STORAGE.remove(backup.c_str());
    }

    if (!STORAGE.rename(
            path.c_str(),
            backup.c_str()
        )) {
        Serial.println(
            "TRANSPORT journal rotation failed"
        );
    }
}


static void transportJournalWrite(
    const String &message
)
{
    if (!sdReady || g_storageLocked)
        return;

    char timestamp[48];
    transportJournalTimestamp(
        timestamp,
        sizeof(timestamp)
    );

    String line;
    line.reserve(
        strlen(timestamp) +
        message.length() +
        16U
    );

    line += '[';
    line += timestamp;
    line += "] [INFO ] ";
    line += message;
    line += "\r\n";

    String path =
        transportJournalPath();

    String prepareError;

    if (!logStoragePrepareGenerations(
            path,
            cfg_recording_encryption,
            prepareError
        )) {
        Serial.println(
            "TRANSPORT journal storage prepare failed | " +
            prepareError
        );
        return;
    }

    transportJournalRotateIfNeeded(
        path,
        line.length()
    );

    LogStorageWriter writer;
    String error;

    if (!logStorageOpenWriter(
            path,
            cfg_recording_encryption,
            writer,
            error
        )) {
        if (path != "/log.txt") {
            path = "/log.txt";

            if (!logStorageOpenWriter(
                    path,
                    cfg_recording_encryption,
                    writer,
                    error
                )) {
                Serial.println(
                    "TRANSPORT journal open failed | " +
                    error
                );
                return;
            }
        } else {
            Serial.println(
                "TRANSPORT journal open failed | " +
                error
            );
            return;
        }
    }

    uint64_t written = 0;

    if (!logStorageAppend(
            writer,
            reinterpret_cast<const uint8_t *>(line.c_str()),
            line.length(),
            written,
            error
        )) {
        Serial.println(
            "TRANSPORT journal write failed | " +
            error
        );
    }

    logStorageFlushWriter(writer);
    logStorageCloseWriter(writer);
}


static void transportJournalPrintf(
    const char *format,
    ...
)
{
    char message[320];

    va_list args;
    va_start(args, format);

    vsnprintf(
        message,
        sizeof(message),
        format,
        args
    );

    va_end(args);

    transportJournalWrite(
        String(message)
    );
}


static void transportWaitMs(
    uint32_t waitMs
)
{
    uint32_t started =
        millis();

    while (
        (uint32_t)(
            millis() -
            started
        ) < waitMs
    ) {
        feedWatchdog();
        delay(10);
    }
}


static bool initTransportCheckCamera(
    String &error
)
{
    error =
        "";

    if (cameraInitialized) {
        cameraDeinitRuntime();
    }

    camera_pins_t pins =
        selectCamera(
            cfg_camera
        );

    camera_config_t cfg = {};

    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;

    cfg.pin_pwdn = pins.pin_pwdn;
    cfg.pin_reset = pins.pin_reset;
    cfg.pin_xclk = pins.pin_xclk;
    cfg.pin_sccb_sda = pins.pin_sscb_sda;
    cfg.pin_sccb_scl = pins.pin_sscb_scl;

    cfg.pin_d7 = pins.pin_d7;
    cfg.pin_d6 = pins.pin_d6;
    cfg.pin_d5 = pins.pin_d5;
    cfg.pin_d4 = pins.pin_d4;
    cfg.pin_d3 = pins.pin_d3;
    cfg.pin_d2 = pins.pin_d2;
    cfg.pin_d1 = pins.pin_d1;
    cfg.pin_d0 = pins.pin_d0;

    cfg.pin_vsync = pins.pin_vsync;
    cfg.pin_href = pins.pin_href;
    cfg.pin_pclk = pins.pin_pclk;

    cfg.xclk_freq_hz =
        (uint32_t)cfg_camera_xclk_mhz *
        1000000UL;

    // Low resolution is intentional: transport mode needs only luminance
    // statistics, not a recordable image. One grayscale buffer is enough and
    // keeps wake time/memory use small.
    cfg.frame_size =
        FRAMESIZE_QQVGA;

    cfg.pixel_format =
        PIXFORMAT_GRAYSCALE;

    cfg.jpeg_quality =
        12;

    cfg.fb_count =
        1;

    cfg.fb_location =
        CAMERA_FB_IN_DRAM;

    cfg.grab_mode =
        CAMERA_GRAB_WHEN_EMPTY;


    esp_err_t initError =
        esp_camera_init(
            &cfg
        );

    if (initError != ESP_OK) {
        char buffer[96];

        snprintf(
            buffer,
            sizeof(buffer),
            "transport camera init failed: 0x%x",
            initError
        );

        error =
            String(buffer);

        cameraInitialized =
            false;

        return false;
    }


    cameraInitialized =
        true;

    cameraFramebufferCount =
        1;

    cameraSoftPowerDownActive =
        false;


    sensor_t *sensor =
        esp_camera_sensor_get();

    if (sensor) {
        cameraDetectedPid =
            sensor->id.PID;

        // For cover detection we intentionally bias the sensor toward seeing
        // whatever light is available. A genuinely taped lens should remain
        // dark; a very dim room gets the best chance of being recognized as
        // uncovered.
        if (sensor->set_exposure_ctrl) {
            sensor->set_exposure_ctrl(
                sensor,
                1
            );
        }

        if (sensor->set_gain_ctrl) {
            sensor->set_gain_ctrl(
                sensor,
                1
            );
        }

        if (sensor->set_ae_level) {
            sensor->set_ae_level(
                sensor,
                2
            );
        }
    }


    return true;
}


static bool transportWarmupCamera(
    String &error
)
{
    error =
        "";

    // Several frames give auto exposure / gain time to react after power-up.
    // This is especially important in a dark installation room: we prefer to
    // classify a dim but uncovered scene as OPEN rather than BLACK.
    for (uint8_t i = 0; i < 6; ++i) {
        camera_fb_t *frame =
            esp_camera_fb_get();

        if (!frame) {
            error =
                "transport camera warmup frame unavailable";

            return false;
        }

        esp_camera_fb_return(
            frame
        );

        transportWaitMs(
            80
        );
    }

    return true;
}


static bool transportMeasureImage(
    TransportImageMetrics &metrics,
    String &error
)
{
    error =
        "";

    camera_fb_t *frame =
        esp_camera_fb_get();

    if (!frame) {
        error =
            "transport camera frame unavailable";

        return false;
    }


    bool formatOk =
        frame->format == PIXFORMAT_GRAYSCALE;

    size_t pixelCount =
        (size_t)frame->width *
        (size_t)frame->height;

    bool sizeOk =
        pixelCount > 0 &&
        frame->buf != nullptr &&
        frame->len >= pixelCount;

    if (
        !formatOk ||
        !sizeOk
    ) {
        esp_camera_fb_return(
            frame
        );

        error =
            "transport camera returned unexpected grayscale frame";

        return false;
    }


    uint32_t histogram[256] = {};
    uint64_t sum = 0;

    for (size_t i = 0; i < pixelCount; ++i) {
        uint8_t value =
            frame->buf[i];

        sum +=
            value;

        histogram[value]++;
    }


    metrics.mean =
        (float)sum /
        (float)pixelCount;


    uint32_t percentileTarget =
        (uint32_t)(
            (
                pixelCount *
                95ULL +
                99ULL
            ) /
            100ULL
        );

    if (percentileTarget < 1)
        percentileTarget = 1;

    uint32_t cumulative = 0;
    uint8_t p95 = 255;

    for (uint16_t value = 0; value < 256; ++value) {
        cumulative +=
            histogram[value];

        if (cumulative >= percentileTarget) {
            p95 =
                (uint8_t)value;
            break;
        }
    }

    metrics.p95 =
        p95;


    esp_camera_fb_return(
        frame
    );

    return true;
}


// WebConfig calibration helper. It deliberately reuses the exact low-resolution
// grayscale transport camera setup and the exact per-frame Mean/P95 statistics
// used later after timer wake. The reported reference is the highest observed
// value across the sample set, giving a conservative taped-camera baseline.
bool cameraMeasureTransportBlackReference(
    float &referenceMean,
    uint8_t &referenceP95,
    uint8_t sampleCount,
    String &error
)
{
    error = "";
    referenceMean = 0.0f;
    referenceP95 = 0;

    if (sampleCount < 1)
        sampleCount = 1;

    if (sampleCount > 20)
        sampleCount = 20;

    bool restoreNormalCamera =
        cameraInitialized;

    String localError;

    if (!initTransportCheckCamera(localError)) {
        // initTransportCheckCamera() may already have deinitialized the normal
        // camera. Restore the pre-page state as far as possible before returning.
        if (restoreNormalCamera) {
            if (!initCamera(
                    cfg_camera,
                    cfg_resolution,
                    cfg_quality
                )) {
                localError +=
                    " | normal camera restore also failed";
            }
        }

        error = localError;
        return false;
    }

    bool ok =
        transportWarmupCamera(localError);

    if (ok) {
        for (uint8_t i = 0; i < sampleCount; ++i) {
            TransportImageMetrics metrics = {};

            if (!transportMeasureImage(metrics, localError)) {
                ok = false;
                break;
            }

            if (metrics.mean > referenceMean)
                referenceMean = metrics.mean;

            if (metrics.p95 > referenceP95)
                referenceP95 = metrics.p95;

            if (i + 1U < sampleCount) {
                transportWaitMs(80);
            }
        }
    }

    cameraDeinitRuntime();

    // If WebConfig had the normal recording camera initialized before the
    // calibration, restore it with all persisted exposure/rotation/crop values.
    // When it was not initialized, leave it off just as before opening the page.
    if (restoreNormalCamera) {
        if (!initCamera(
                cfg_camera,
                cfg_resolution,
                cfg_quality
            )) {
            if (ok) {
                localError =
                    "transport calibration measured successfully, but normal camera restore failed";
            }
            ok = false;
        }
    }

    if (!ok) {
        error = localError.length()
            ? localError
            : String("transport calibration failed");
        return false;
    }

    Serial.printf(
        "TRANSPORT calibration | samples=%u | reference mean=%.1f | p95=%u\n",
        (unsigned)sampleCount,
        referenceMean,
        (unsigned)referenceP95
    );

    // During WebConfig calibration the normal logger is already active, so the
    // persistent log gets the same raw reference values as the serial console.
    logWrite(
        "Transport calibration camera result | samples=" +
        String((unsigned)sampleCount) +
        " | reference_mean=" +
        String(referenceMean, 1) +
        " | reference_p95=" +
        String((unsigned)referenceP95)
    );

    return true;
}


static bool transportImageIsBlack(
    const TransportImageMetrics &metrics
)
{
    return
        metrics.mean <=
            (float)cfg_transport_black_mean_max &&
        metrics.p95 <=
            (uint8_t)cfg_transport_black_p95_max;
}


static bool transportMeasureAndReport(
    const char *label,
    bool &isBlack,
    String &error
)
{
    TransportImageMetrics metrics = {};

    if (!transportMeasureImage(
            metrics,
            error
        )) {
        return false;
    }

    isBlack =
        transportImageIsBlack(
            metrics
        );

    Serial.printf(
        "TRANSPORT image %s | mean=%.1f | p95=%u | limits mean<=%d p95<=%d | %s\n",
        label ? label : "check",
        metrics.mean,
        (unsigned)metrics.p95,
        cfg_transport_black_mean_max,
        cfg_transport_black_p95_max,
        isBlack ? "BLACK" : "OPEN"
    );

    transportJournalPrintf(
        "TRANSPORT image %s | mean=%.1f | p95=%u | limits mean<=%d p95<=%d | %s",
        label ? label : "check",
        metrics.mean,
        (unsigned)metrics.p95,
        cfg_transport_black_mean_max,
        cfg_transport_black_p95_max,
        isBlack ? "BLACK" : "OPEN"
    );

    return true;
}


static uint32_t transportElapsedSecondsNow()
{
    uint64_t fallbackElapsed =
        (uint64_t)transportRtcElapsedFallbackSeconds +
        (uint64_t)(millis() / 1000UL);

    time_t now =
        time(nullptr);

    if (
        transportRtcStartedEpoch >= (int64_t)1609459200 &&
        now >= (time_t)1609459200 &&
        (int64_t)now >= transportRtcStartedEpoch
    ) {
        uint64_t wallElapsed =
            (uint64_t)(
                (int64_t)now -
                transportRtcStartedEpoch
            );

        // Safety semantics: never let a backward/adjusted wall clock extend
        // the transport window beyond the independently accumulated budget.
        uint64_t effectiveElapsed =
            wallElapsed > fallbackElapsed
            ? wallElapsed
            : fallbackElapsed;

        if (effectiveElapsed > 0xFFFFFFFFULL)
            return 0xFFFFFFFFUL;

        return
            (uint32_t)effectiveElapsed;
    }

    if (fallbackElapsed > 0xFFFFFFFFULL)
        return 0xFFFFFFFFUL;

    return
        (uint32_t)fallbackElapsed;
}


static uint32_t transportRemainingSeconds()
{
    uint32_t maximum =
        (uint32_t)cfg_transport_max_duration_seconds;

    uint32_t elapsed =
        transportElapsedSecondsNow();

    if (elapsed >= maximum)
        return 0;

    return
        maximum -
        elapsed;
}


static void enterTransportTimerSleep(
    uint32_t seconds,
    const char *reason,
    bool enforceMaxDuration
)
{
    if (seconds < 1)
        seconds = 1;

    uint32_t requestedSeconds =
        seconds;

    if (enforceMaxDuration) {
        uint32_t remainingSeconds =
            transportRemainingSeconds();

        // If awake work reached the deadline between two checks, use one final
        // one-second timer wake. The next boot exits before any camera check.
        if (remainingSeconds == 0) {
            seconds =
                1;

        } else if (seconds > remainingSeconds) {
            seconds =
                remainingSeconds;
        }

        if (seconds != requestedSeconds) {
            transportJournalPrintf(
                "TRANSPORT sleep capped by maximum duration | requested=%lu s | actual=%lu s | remaining=%lu s",
                (unsigned long)requestedSeconds,
                (unsigned long)seconds,
                (unsigned long)remainingSeconds
            );
        }
    }

    // Keep a wall-clock-independent fallback budget across deep sleep. The
    // current boot's active seconds are committed now; the sleep itself is
    // added on the next timer wake.
    uint64_t activeSeconds =
        (uint64_t)(millis() / 1000UL);

    uint64_t fallbackTotal =
        (uint64_t)transportRtcElapsedFallbackSeconds +
        activeSeconds;

    transportRtcElapsedFallbackSeconds =
        fallbackTotal > 0xFFFFFFFFULL
        ? 0xFFFFFFFFUL
        : (uint32_t)fallbackTotal;

    transportRtcLastSleepSeconds =
        seconds;


    Serial.printf(
        "TRANSPORT deep sleep | reason=%s | timer=%lu s | radar/presence/magnet wake disabled\n",
        reason ? reason : "transport",
        (unsigned long)seconds
    );

    transportJournalPrintf(
        "TRANSPORT sleep | reason=%s | timer=%lu s | wake_sources=timer-only",
        reason ? reason : "transport",
        (unsigned long)seconds
    );


    if (cfg_led_enabled) {
        statusLedWriteLevel(
            LED_OFF_LEVEL
        );
    }


    if (cameraInitialized) {
        cameraDeinitRuntime();
    }


    // Transport mode is intentionally timer-only. In particular, LD2410S OT2
    // must not be able to wake the ESP32 continuously during a vehicle trip.
    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    esp_err_t timerError =
        esp_sleep_enable_timer_wakeup(
            (uint64_t)seconds *
            1000000ULL
        );

    if (timerError != ESP_OK) {
        Serial.printf(
            "TRANSPORT timer wake setup failed: 0x%x - restarting\n",
            timerError
        );

        transportJournalPrintf(
            "TRANSPORT ERROR | timer wake setup failed: 0x%x | restarting",
            timerError
        );

        Serial.flush();
        delay(50);
        ESP.restart();

        while (true) {
            delay(1000);
        }
    }


    // No logger has been opened yet on the transport path. Close the mounted
    // card cleanly before deep sleep to minimize active time and avoid leaving
    // SPI/SD state half-open across the reset-style wake.
    if (sdReady) {
        STORAGE.end();
        sdReady =
            false;
    }

#if defined(STORAGE_SPI)
    SPI.end();
#endif


    Serial.flush();
    delay(30);

    esp_deep_sleep_start();

    // Deep sleep should never return. If the platform ever does return here,
    // restart instead of falling into loop() with only a partial boot.
    ESP.restart();

    while (true) {
        delay(1000);
    }
}


static bool persistTransportModeValue(
    int mode
)
{
    bool writeToSd =
        configGetSource() == CONFIG_SOURCE_SD &&
        configSdAvailable() &&
        configSdValid();

    String error;

    ConfigSaveResult result =
        configSaveTransportMode(
            mode,
            writeToSd,
            error
        );

    bool ok =
        result == CONFIG_SAVE_BOTH ||
        result == CONFIG_SAVE_INTERNAL_ONLY;

    if (!ok) {
        String failure =
            "TRANSPORT config update failed | " +
            error;

        Serial.println(
            failure
        );

        transportJournalWrite(
            failure
        );
    }

    return ok;
}


static bool handleTransportModeBoot(
    esp_sleep_wakeup_cause_t wakeCause
)
{
    if (cfg_transport_mode != 1) {
        // A normal/configured boot always clears stale RTC-only phase state.
        transportRtcMagic =
            0;

        transportRtcPhase =
            TRANSPORT_PHASE_NONE;

        transportRtcWakeCount =
            0;

        transportRtcStartedEpoch =
            0;

        transportRtcElapsedFallbackSeconds =
            0;

        transportRtcLastSleepSeconds =
            0;

        // transport_mode=0 is authoritative. Remove any stale absolute start
        // marker left by an interrupted/previous transport session.
        transportClearPersistentStartEpoch();

        return false;
    }


    bool newTransportSession =
        transportRtcMagic != TRANSPORT_RTC_MAGIC;

    if (newTransportSession) {
        transportRtcMagic =
            TRANSPORT_RTC_MAGIC;

        transportRtcPhase =
            TRANSPORT_PHASE_CHECKING;

        transportRtcWakeCount =
            0;

        transportRtcStartedEpoch =
            0;

        transportRtcElapsedFallbackSeconds =
            0;

        transportRtcLastSleepSeconds =
            0;
    }

    transportRtcWakeCount++;

    // The full RTC/logger stack still stays off. We only make sure wall-clock
    // time is available for durable transport-journal timestamps.
    transportPrepareJournalClock();

    // Account for the completed timer interval in the no-clock fallback.
    // Non-timer resets intentionally do not claim that the planned sleep
    // duration actually elapsed.
    if (
        !newTransportSession &&
        wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
        transportRtcLastSleepSeconds > 0
    ) {
        uint64_t fallbackTotal =
            (uint64_t)transportRtcElapsedFallbackSeconds +
            (uint64_t)transportRtcLastSleepSeconds;

        transportRtcElapsedFallbackSeconds =
            fallbackTotal > 0xFFFFFFFFULL
            ? 0xFFFFFFFFUL
            : (uint32_t)fallbackTotal;
    }

    transportRtcLastSleepSeconds =
        0;

    // Prefer an absolute deadline when the DS3231/system clock is valid.
    // /transport.state keeps the original start epoch across a real ESP power
    // interruption as well; RTC_DATA_ATTR alone only guarantees deep-sleep
    // continuity.
    time_t transportNow =
        time(nullptr);

    int64_t persistedStartEpoch =
        0;

    bool persistentStartAvailable =
        transportLoadPersistentStartEpoch(
            persistedStartEpoch
        );

    if (
        newTransportSession &&
        persistentStartAvailable &&
        transportNow >= (time_t)1609459200 &&
        persistedStartEpoch <=
            (int64_t)transportNow
    ) {
        transportRtcStartedEpoch =
            persistedStartEpoch;
    }

    if (
        transportRtcStartedEpoch < (int64_t)1609459200 &&
        transportNow >= (time_t)1609459200
    ) {
        uint32_t fallbackElapsed =
            transportRtcElapsedFallbackSeconds;

        transportRtcStartedEpoch =
            (int64_t)transportNow -
            (int64_t)fallbackElapsed;

        if (!transportStorePersistentStartEpoch(
                transportRtcStartedEpoch
            )) {
            transportJournalWrite(
                "TRANSPORT WARNING | persistent maximum-duration start marker could not be written"
            );
        }

    } else if (
        transportRtcStartedEpoch >= (int64_t)1609459200 &&
        !persistentStartAvailable
    ) {
        if (!transportStorePersistentStartEpoch(
                transportRtcStartedEpoch
            )) {
            transportJournalWrite(
                "TRANSPORT WARNING | persistent maximum-duration start marker could not be written"
            );
        }
    }

    uint32_t transportElapsedSeconds =
        transportElapsedSecondsNow();

    uint32_t transportRemaining =
        transportRemainingSeconds();

    Serial.printf(
        "TRANSPORT mode ACTIVE | wake=%s(%d) | cycle=%lu | check=%d s | confirm=%d s | install_delay=%d s | max_duration=%d s | elapsed=%lu s | remaining=%lu s\n",
        sleepWakeCauseName(wakeCause),
        (int)wakeCause,
        (unsigned long)transportRtcWakeCount,
        cfg_transport_check_seconds,
        cfg_transport_light_confirm_seconds,
        cfg_transport_install_delay_seconds,
        cfg_transport_max_duration_seconds,
        (unsigned long)transportElapsedSeconds,
        (unsigned long)transportRemaining
    );

    transportJournalPrintf(
        "TRANSPORT mode ACTIVE | session=%s | wake=%s(%d) | cycle=%lu | check=%d s | confirm=%d s | install_delay=%d s | max_duration=%d s | elapsed=%lu s | remaining=%lu s | mean<=%d | p95<=%d",
        newTransportSession ? "NEW" : "CONTINUE",
        sleepWakeCauseName(wakeCause),
        (int)wakeCause,
        (unsigned long)transportRtcWakeCount,
        cfg_transport_check_seconds,
        cfg_transport_light_confirm_seconds,
        cfg_transport_install_delay_seconds,
        cfg_transport_max_duration_seconds,
        (unsigned long)transportElapsedSeconds,
        (unsigned long)transportRemaining,
        cfg_transport_black_mean_max,
        cfg_transport_black_p95_max
    );


    // Hard safety fallback has priority over camera state and installation
    // delay. Once the configured maximum duration expires, force normal mode.
    if (transportRemaining == 0) {
        Serial.printf(
            "TRANSPORT maximum duration reached | max=%d s | elapsed=%lu s - forcing normal operation\n",
            cfg_transport_max_duration_seconds,
            (unsigned long)transportElapsedSeconds
        );

        transportJournalPrintf(
            "TRANSPORT maximum duration reached | max_duration=%d s | elapsed=%lu s | forcing normal operation",
            cfg_transport_max_duration_seconds,
            (unsigned long)transportElapsedSeconds
        );

        if (!persistTransportModeValue(0)) {
            Serial.println(
                "TRANSPORT max-duration release persistence failed - retrying in 60 s"
            );

            transportJournalWrite(
                "TRANSPORT max-duration release persistence failed | retrying in 60 s"
            );

            enterTransportTimerSleep(
                60,
                "max-duration-release-save-retry",
                false
            );

            return true;
        }

        transportRtcMagic =
            0;

        transportRtcPhase =
            TRANSPORT_PHASE_NONE;

        transportRtcWakeCount =
            0;

        transportRtcStartedEpoch =
            0;

        transportRtcElapsedFallbackSeconds =
            0;

        transportRtcLastSleepSeconds =
            0;

        transportClearPersistentStartEpoch();

        Serial.println(
            "TRANSPORT mode OFF - maximum duration fallback"
        );

        transportJournalWrite(
            "TRANSPORT mode OFF | reason=max-duration | normal boot continues"
        );

        return false;
    }


    // The installation delay is a one-shot timer. Only its timer wake proves
    // that the requested delay elapsed. A reset/power interruption restarts
    // the delay rather than arming the camera early.
    if (
        transportRtcPhase ==
        TRANSPORT_PHASE_INSTALL_DELAY
    ) {

        if (
            wakeCause ==
            ESP_SLEEP_WAKEUP_TIMER
        ) {
            Serial.println(
                "TRANSPORT installation delay elapsed - enabling normal operation"
            );

            transportJournalWrite(
                "TRANSPORT installation delay elapsed | enabling normal operation"
            );

            if (!persistTransportModeValue(0)) {
                Serial.println(
                    "TRANSPORT release persistence failed - retrying in 60 s"
                );

                transportJournalWrite(
                    "TRANSPORT release persistence failed | retrying in 60 s"
                );

                enterTransportTimerSleep(
                    60,
                    "release-save-retry",
                    false
                );

                return true;
            }

            transportRtcMagic =
                0;

            transportRtcPhase =
                TRANSPORT_PHASE_NONE;

            Serial.println(
                "TRANSPORT mode OFF - normal boot continues"
            );

            transportJournalWrite(
                "TRANSPORT mode OFF | normal boot continues"
            );

            transportRtcWakeCount =
                0;

            transportRtcStartedEpoch =
                0;

            transportRtcElapsedFallbackSeconds =
                0;

            transportRtcLastSleepSeconds =
                0;

            transportClearPersistentStartEpoch();

            return false;
        }


        Serial.println(
            "TRANSPORT installation delay interrupted - restarting full delay"
        );

        transportJournalWrite(
            "TRANSPORT installation delay interrupted | restarting full delay"
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_install_delay_seconds,
            "installation-delay-restart",
            true
        );

        return true;
    }


    transportRtcPhase =
        TRANSPORT_PHASE_CHECKING;


    String error;

    if (!initTransportCheckCamera(
            error
        )) {
        String failure =
            "TRANSPORT camera check unavailable | " +
            error;

        Serial.println(
            failure
        );

        transportJournalWrite(
            failure
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_check_seconds,
            "camera-init-failed",
            true
        );

        return true;
    }


    if (!transportWarmupCamera(
            error
        )) {
        String failure =
            "TRANSPORT camera warmup failed | " +
            error;

        Serial.println(
            failure
        );

        transportJournalWrite(
            failure
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_check_seconds,
            "camera-warmup-failed",
            true
        );

        return true;
    }


    bool firstBlack =
        true;

    if (!transportMeasureAndReport(
            "1/3",
            firstBlack,
            error
        )) {
        String failure =
            "TRANSPORT image measurement failed | " +
            error;

        Serial.println(
            failure
        );

        transportJournalWrite(
            failure
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_check_seconds,
            "measurement-failed",
            true
        );

        return true;
    }


    if (firstBlack) {
        enterTransportTimerSleep(
            (uint32_t)cfg_transport_check_seconds,
            "cover-still-black",
            true
        );

        return true;
    }


    Serial.println(
        "TRANSPORT light detected - starting confirmation"
    );

    transportJournalWrite(
        "TRANSPORT light detected | confirmation started"
    );


    bool confirmedOpen =
        true;

    uint32_t confirmMs =
        (uint32_t)cfg_transport_light_confirm_seconds *
        1000UL;

    if (confirmMs > 0) {
        uint32_t firstWaitMs =
            confirmMs /
            2UL;

        uint32_t secondWaitMs =
            confirmMs -
            firstWaitMs;

        if (firstWaitMs > 0) {
            transportWaitMs(
                firstWaitMs
            );
        }

        bool secondBlack =
            true;

        if (!transportMeasureAndReport(
                "2/3",
                secondBlack,
                error
            )) {
            String failure =
                "TRANSPORT confirmation measurement failed | " +
                error;

            Serial.println(
                failure
            );

            transportJournalWrite(
                failure
            );

            confirmedOpen =
                false;

        } else if (secondBlack) {
            confirmedOpen =
                false;
        }


        if (
            confirmedOpen &&
            secondWaitMs > 0
        ) {
            transportWaitMs(
                secondWaitMs
            );
        }


        if (confirmedOpen) {
            bool thirdBlack =
                true;

            if (!transportMeasureAndReport(
                    "3/3",
                    thirdBlack,
                    error
                )) {
                String failure =
                    "TRANSPORT confirmation measurement failed | " +
                    error;

                Serial.println(
                    failure
                );

                transportJournalWrite(
                    failure
                );

                confirmedOpen =
                    false;

            } else if (thirdBlack) {
                confirmedOpen =
                    false;
            }
        }
    }


    if (!confirmedOpen) {
        Serial.println(
            "TRANSPORT light not confirmed - returning to covered checks"
        );

        transportJournalWrite(
            "TRANSPORT light not confirmed | returning to covered checks"
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_check_seconds,
            "light-not-confirmed",
            true
        );

        return true;
    }


    Serial.println(
        "TRANSPORT cover removal confirmed"
    );

    transportJournalWrite(
        "TRANSPORT cover removal confirmed"
    );

    cameraDeinitRuntime();


    if (cfg_transport_install_delay_seconds > 0) {
        transportRtcPhase =
            TRANSPORT_PHASE_INSTALL_DELAY;

        transportJournalPrintf(
            "TRANSPORT installation delay started | duration=%d s",
            cfg_transport_install_delay_seconds
        );

        enterTransportTimerSleep(
            (uint32_t)cfg_transport_install_delay_seconds,
            "installation-delay",
            true
        );

        return true;
    }


    // A configured zero-second installation delay arms immediately after the
    // same multi-frame light confirmation.
    if (!persistTransportModeValue(0)) {
        transportRtcPhase =
            TRANSPORT_PHASE_INSTALL_DELAY;

        enterTransportTimerSleep(
            60,
            "release-save-retry",
            false
        );

        return true;
    }


    transportRtcMagic =
        0;

    transportRtcPhase =
        TRANSPORT_PHASE_NONE;

    Serial.println(
        "TRANSPORT mode OFF - normal boot continues"
    );

    transportJournalWrite(
        "TRANSPORT mode OFF | normal boot continues"
    );

    transportRtcWakeCount =
        0;

    transportRtcStartedEpoch =
        0;

    transportRtcElapsedFallbackSeconds =
        0;

    transportRtcLastSleepSeconds =
        0;

    transportClearPersistentStartEpoch();

    return false;
}


static bool cameraEnterSoftPowerDown()
{
    if (!cameraInitialized)
        return false;

    if (cameraSoftPowerDownActive)
        return true;

    if (!cameraSupportsFastLightSleepNow())
        return false;

    sensor_t *sensor =
        esp_camera_sensor_get();

    int result =
        sensor->set_reg(
            sensor,
            0x3008,
            0x40,
            0x40
        );

    if (result != 0)
        return false;

    cameraSoftPowerDownActive = true;
    return true;
}


static bool cameraExitSoftPowerDown()
{
    if (!cameraInitialized)
        return false;

    if (!cameraSoftPowerDownActive)
        return true;

    sensor_t *sensor =
        esp_camera_sensor_get();

    if (
        !sensor ||
        !sensor->set_reg ||
        sensor->id.PID != 0x3660
    ) {
        return false;
    }

    int result =
        sensor->set_reg(
            sensor,
            0x3008,
            0x40,
            0x00
        );

    if (result != 0)
        return false;

    cameraSoftPowerDownActive = false;
    return true;
}


// =============================================================
// CAMERA RECOVERY
// =============================================================

bool recoverCamera()
{
    Serial.println(
        "Attempting camera recovery..."
    );

    logWrite(
        "Camera recovery started"
    );

    if (cameraInitialized) {
        cameraDeinitRuntime();
    }

    for (
        int attempt = 1;
        attempt <= 3;
        ++attempt
    ) {

        delay(100);

        if (initCamera(
                cfg_camera,
                cfg_resolution,
                cfg_quality
            )) {

            consecutiveFrameFailures =
                0;

            logWrite(
                "Camera recovery successful"
            );

            return true;
        }
    }

    // Preserve the current container if possible before reboot.
    if (recording) {
        stopRecording();
    }

    logWrite(
        "Camera recovery failed - rebooting"
    );

    logFlush();

    Serial.println(
        "Camera recovery failed - rebooting"
    );

    delay(100);
    ESP.restart();

    return false;
}


// =============================================================
// START RECORDING
// =============================================================

bool startRecording() {

    // Timestamp function entry before doing any recorder prechecks. This lets
    // debug builds distinguish time spent in the preceding MOTION console
    // message from time actually spent inside startRecording().
    if (
        wakeTimingActive &&
        wakeTimingStartRecordingEnterUs == 0
    ) {
        wakeTimingStartRecordingEnterUs =
            (uint64_t)esp_timer_get_time();
    }

    if (recording)
        return true;

    const bool measureWakeStages =
        wakeTimingActive &&
        cfg_debug_enabled;

    uint64_t stageStartUs = 0;
    uint64_t stageDoneUs = 0;

    // Thermal emergency blocks every new recording start, independent of
    // WebConfig/operator state.
    if (thermalEmergencyState)
        return false;

    // Defense in depth for API-exclusive mode. motionDetected() already
    // suppresses automatic triggers, but direct callers must be blocked too.
    if (syncApiExclusiveActive())
        return false;

    // Defense in depth for the event safety cooldown. motionDetected() already
    // suppresses automatic triggers, but direct callers must be blocked too.
    if (recordingSafetyCooldownActive())
        return false;

    // Defense in depth for the installation deadline. motionDetected() already
    // suppresses automatic triggers, but direct callers must be blocked too.
    if (!configRecordingAllowedNow())
        return false;

    // Defense in depth: even callers that bypass motionDetected() must not
    // start a new file while the operator has paused the recording automation.
    if (
        webConfigStarted &&
        webConfigRecordingPaused()
    ) {
        return false;
    }

    if (!sdReady) {

        consoleWrite(
            "REC",
            "ERROR | SD not ready"
        );

        logWrite(
            "Recording failed: SD not ready"
        );

        return false;
    }

    if (
        measureWakeStages &&
        wakeTimingPrechecksDoneUs == 0
    ) {
        wakeTimingPrechecksDoneUs =
            (uint64_t)esp_timer_get_time();
    }

    if (measureWakeStages) {
        stageStartUs =
            (uint64_t)esp_timer_get_time();
    }

    bool storageReadyForRecording =
        storagePrepareForRecording();

    if (measureWakeStages) {
        stageDoneUs =
            (uint64_t)esp_timer_get_time();

        wakeTimingStorageUs =
            stageDoneUs - stageStartUs;
    }

    if (!storageReadyForRecording) {

        diskSpaceBlocked =
            true;

        logWrite(
            "Recording blocked by storage reserve"
        );

        consoleWrite(
            "REC",
            "BLOCKED | storage reserve"
        );

        return false;
    }

    diskSpaceBlocked =
        false;


    if (cfg_led_enabled)
        statusLedWriteLevel(LED_ON_LEVEL);


    if (measureWakeStages) {
        stageStartUs =
            (uint64_t)esp_timer_get_time();
    }

    if (!initCamera(
            cfg_camera,
            cfg_resolution,
            cfg_quality
        )) {

        logWrite("Camera init failed");

        if (cfg_led_enabled)
            statusLedWriteLevel(LED_OFF_LEVEL);

        return false;
    }


    // Normally enterLightSleep() already wakes the OV3660 immediately after
    // the ESP resumes. This is a defensive guard for any alternate call path.
    if (!cameraExitSoftPowerDown()) {

        consoleWrite(
            "CAM",
            "Fast wake failed - camera recovery"
        );

        if (!recoverCamera()) {
            return false;
        }
    }


    if (measureWakeStages) {
        stageDoneUs =
            (uint64_t)esp_timer_get_time();

        wakeTimingCameraPrepareUs =
            stageDoneUs - stageStartUs;

        stageStartUs =
            stageDoneUs;
    }

    String folder =
        makeFolder();

    String filename =
        makeFilename();

    String fullpath =
        folder + "/" + filename;

    if (measureWakeStages) {
        stageDoneUs =
            (uint64_t)esp_timer_get_time();

        wakeTimingPathUs =
            stageDoneUs - stageStartUs;

        stageStartUs =
            stageDoneUs;
    }

    bool recorderStarted =
        recorderStart(
            fullpath,
            cfg_fps
        );

    if (measureWakeStages) {
        stageDoneUs =
            (uint64_t)esp_timer_get_time();

        wakeTimingRecorderOpenUs =
            stageDoneUs - stageStartUs;
    }

    if (!recorderStarted) {

        logWrite(
            "Recorder open failed: " +
            cfg_recording_format
        );

        consoleWrite(
            "REC",
            "ERROR | recorder open failed | " +
            cfg_recording_format
        );

        if (!webConfigStarted && cameraInitialized) {
            cameraDeinitRuntime();
        }

        if (cfg_led_enabled)
            statusLedWriteLevel(LED_OFF_LEVEL);

        return false;
    }


    if (
        wakeTimingActive &&
        wakeTimingRecorderReadyUs == 0
    ) {
        wakeTimingRecorderReadyUs =
            (uint64_t)esp_timer_get_time();
    }


    String formatName =
        cfg_recording_format;

    formatName.toUpperCase();

    String startMessage =
        "START | " +
        formatName +
        " | " +
        fullpath +
        " | " +
        cfg_resolution +
        " @ " +
        String(cfg_fps) +
        " fps";

    if (wakeCriticalPathActive) {
        deferredRecordingStartMessage = startMessage;
        deferredRecordingStartPending = true;
    } else {
        consoleWrite(
            "REC",
            startMessage
        );

        logWrite(
            "Recording " + startMessage
        );
    }

    unsigned long recordingStartMs = millis();

    lastMotionMs = recordingStartMs;
    lastFrameUs = 0;
    lastSpaceCheckMs = recordingStartMs;
    recordingSegmentStartMs = recordingStartMs;
    recordingEventStartMs = recordingStartMs;
    consecutiveFrameFailures = 0;

    // Start a fresh event-level thermal accumulator. The regular thermal
    // monitor will add samples in RAM while recording is active.
    recordingThermalReset();

    recording = true;

    return true;
}


// =============================================================
// ROTATE RECORDING SEGMENT
// =============================================================

static bool rotateRecordingSegment(const String &reason)
{
    if (!recording)
        return false;


    // Finalize the current container first. For AVI this also creates
    // and promotes the matching .srt sidecar; MKV subtitles are already
    // embedded in the segment itself.
    bool finalized =
        recorderEnd();

    if (!finalized) {

        consoleWrite(
            "REC",
            "ERROR | segment finalization failed"
        );

        logWrite(
            "Recording segment finalization failed"
        );

        recordingThermalFinish(
            "segment_finalization_failed"
        );

        recording = false;

        if (cfg_led_enabled)
            statusLedWriteLevel(LED_OFF_LEVEL);

        if (!webConfigStarted && cameraInitialized) {
            cameraDeinitRuntime();
        }

        recoverSD();
        resetSleepDelayTimer();

        return false;
    }


    // A segment boundary is also a safe point for the storage guard:
    // the previous recording is already complete, so rollover may delete
    // old finalized recordings if the configured reserve requires it.
    if (!storagePrepareForRecording()) {

        diskSpaceBlocked =
            true;

        consoleWrite(
            "REC",
            "BLOCKED | storage reserve"
        );

        logWrite(
            "Recording segment blocked by storage reserve"
        );

        recordingThermalFinish(
            "segment_storage_blocked"
        );

        recording = false;

        if (cfg_led_enabled)
            statusLedWriteLevel(LED_OFF_LEVEL);

        if (!webConfigStarted && cameraInitialized) {
            cameraDeinitRuntime();
        }

        resetSleepDelayTimer();

        return false;
    }

    diskSpaceBlocked =
        false;


    String folder =
        makeFolder();

    String filename =
        makeFilename();

    String fullpath =
        folder + "/" + filename;


    if (!recorderStart(
            fullpath,
            cfg_fps
        )) {

        consoleWrite(
            "REC",
            "ERROR | next segment open failed | " +
            cfg_recording_format
        );

        logWrite(
            "Recording next segment open failed: " +
            cfg_recording_format
        );

        recordingThermalFinish(
            "next_segment_open_failed"
        );

        recording = false;

        if (cfg_led_enabled)
            statusLedWriteLevel(LED_OFF_LEVEL);

        if (!webConfigStarted && cameraInitialized) {
            cameraDeinitRuntime();
        }

        resetSleepDelayTimer();

        return false;
    }


    String formatName =
        cfg_recording_format;

    formatName.toUpperCase();

    String nextMessage =
        "NEXT | " +
        formatName +
        " | " +
        fullpath +
        " | limit=" +
        reason;

    consoleWrite(
        "REC",
        nextMessage
    );

    logWrite(
        "Recording " + nextMessage
    );


    recordingSegmentStartMs =
        millis();

    lastFrameUs = 0;
    lastSpaceCheckMs = millis();
    consecutiveFrameFailures = 0;

    return true;
}


// =============================================================
// STOP RECORDING
// =============================================================

void stopRecording() {

    if (!recording)
        return;

    bool finalized =
        recorderEnd();

    if (!finalized) {

        consoleWrite(
            "REC",
            "ERROR | finalization failed"
        );

        logWrite(
            "Recording finalization failed"
        );

    } else {

        logWrite(
            "Recording stop | finalized"
        );
    }

    recordingThermalFinish(
        finalized
        ? "finalized"
        : "finalization_failed"
    );

    // Preserve the legacy behavior for every sensor except a physically
    // detected OV3660. Only that sensor has a verified fast standby path.
    // WebConfig still keeps the camera active for live preview as before.
    bool keepCameraForFastLightSleep =
        !webConfigStarted &&
        cameraInitialized &&
        cfg_sleep_mode == "light_sleep" &&
        cameraSupportsFastLightSleepNow();

    if (
        !webConfigStarted &&
        cameraInitialized &&
        !keepCameraForFastLightSleep
    ) {
        cameraDeinitRuntime();
    }

    recording = false;
    recordingEventStartMs = 0;

    if (cfg_led_enabled)
        statusLedWriteLevel(LED_OFF_LEVEL);

    if (!finalized) {
        recoverSD();
    }


    // sleep_delay_ms starts only after the recording has been fully
    // finalized and all immediate recorder cleanup is complete.
    resetSleepDelayTimer();
}


// =============================================================
// RECORDING EVENT SAFETY LIMIT / COOLDOWN
// =============================================================

bool recordingSafetyCooldownActive()
{
    if (!recordingSafetyCooldownState)
        return false;

    uint32_t elapsedMs =
        (uint32_t)(
            millis() -
            recordingSafetyCooldownStartedMs
        );

    uint64_t totalMs =
        (uint64_t)recordingSafetyCooldownDurationSeconds *
        1000ULL;

    if ((uint64_t)elapsedMs < totalMs)
        return true;

    // Expire the gate without console I/O. This function is also queried in
    // the immediate light-sleep wake path; printing here could reintroduce the
    // USB/Serial latency that was deliberately removed from first-frame wake.
    recordingSafetyCooldownState = false;
    recordingSafetyCooldownStartedMs = 0;
    recordingSafetyCooldownDurationSeconds = 0;

    return false;
}


uint32_t recordingSafetyCooldownRemainingSeconds()
{
    if (!recordingSafetyCooldownActive())
        return 0;

    uint32_t elapsedMs =
        (uint32_t)(
            millis() -
            recordingSafetyCooldownStartedMs
        );

    uint64_t totalMs =
        (uint64_t)recordingSafetyCooldownDurationSeconds *
        1000ULL;

    uint64_t remainingMs =
        totalMs -
        (uint64_t)elapsedMs;

    return
        (uint32_t)(
            (remainingMs + 999ULL) /
            1000ULL
        );
}


static void serviceRecordingSafetyCooldown()
{
    bool wasActive =
        recordingSafetyCooldownState;

    if (!wasActive)
        return;

    if (recordingSafetyCooldownActive())
        return;

    consoleWrite(
        "REC",
        "Safety cooldown ended | recording enabled"
    );

    logWrite(
        "Recording safety cooldown ended | recording enabled"
    );

    // The cooldown itself counts as controlled idle time. Start a fresh sleep
    // delay now so an idle unit does not enter sleep in the same loop turn in
    // which the safety gate is lifted.
    resetSleepDelayTimer();
}


static void triggerRecordingEventSafetyLimit()
{
    if (!recording)
        return;

    uint32_t elapsedMs =
        recordingEventStartMs != 0
        ? (uint32_t)(
            millis() -
            recordingEventStartMs
        )
        : 0;

    uint32_t cooldownSeconds =
        cfg_recording_event_cooldown_seconds > 0
        ? (uint32_t)cfg_recording_event_cooldown_seconds
        : 0U;

    bool rawMotionActive =
        physicalMotionActive() ||
        simulatedMotionActive();

    // Finalize the active AVI/MKV cleanly before starting the lockout. This
    // preserves all existing recorder finalization, SRT/MKV and SD-recovery
    // behavior instead of bypassing stopRecording().
    stopRecording();

    recordingSafetyCooldownStartedMs =
        millis();

    recordingSafetyCooldownDurationSeconds =
        cooldownSeconds;

    recordingSafetyCooldownState =
        cooldownSeconds > 0;

    char message[224];

    snprintf(
        message,
        sizeof(message),
        "Recording safety limit reached | event_max=%d s | actual=%.1f s | cooldown=%lu s | motion=%d",
        cfg_recording_event_max_seconds,
        (double)elapsedMs / 1000.0,
        (unsigned long)cooldownSeconds,
        rawMotionActive ? 1 : 0
    );

    consoleWrite(
        "WARNING",
        String(message)
    );

    logWrite(
        "WARNING | " +
        String(message)
    );

    if (cooldownSeconds == 0) {
        consoleWrite(
            "REC",
            "Safety cooldown disabled (0 s) | recording may start again immediately"
        );
    }
}


// =============================================================
// SLEEP / POWER MANAGEMENT
// =============================================================

static void resetSleepDelayTimer()
{
    sleepIdleSinceMs =
        millis();
}


static bool sleepModeEnabled()
{
    return
        cfg_sleep_mode == "light_sleep" ||
        cfg_sleep_mode == "deep_sleep";
}


static bool presenceWakeIsClear()
{
    // Both a classic PIR and LD2410S OT2 use this same physical
    // presence input. Sleeping while HIGH would cause an immediate
    // presence wake.
    return
        digitalRead(PIR_PIN) == LOW;
}


static bool magnetWakeIsClear()
{
    // Reed switch is active LOW through INPUT_PULLUP.
    // Do not enter sleep while the magnet is already present,
    // otherwise the EXT0 LOW wake source would fire immediately.
    return
        digitalRead(MAGNET_SWITCH_PIN) == HIGH;
}


static bool configureSleepWakeSources()
{
    // We need opposite wake polarities:
    //   MAGNET_SWITCH_PIN: LOW
    //   PRESENCE/PIR_PIN:  HIGH
    //
    // ESP32-S3 EXT1 cannot mix different levels across multiple pins.
    // Therefore:
    //   EXT0 = magnet LOW
    //   EXT1 = presence HIGH
    //
    // Both pins are RTC GPIOs on ESP32-S3 (GPIO0..21).

    // Clear any previously configured wake sources in one call.
    // Using EXT0/EXT1 individually logs ESP_ERR_INVALID_STATE when a
    // source has not been enabled yet (e.g. first sleep after boot).
    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );


    // Keep the RTC peripheral powered so the internal pull-up on the
    // reed contact remains defined during both light and deep sleep.
    esp_sleep_pd_config(
        ESP_PD_DOMAIN_RTC_PERIPH,
        ESP_PD_OPTION_ON
    );


    // Magnet / reed: active LOW.
    rtc_gpio_init(
        MAGNET_SWITCH_PIN
    );

    rtc_gpio_set_direction(
        MAGNET_SWITCH_PIN,
        RTC_GPIO_MODE_INPUT_ONLY
    );

    rtc_gpio_pullup_en(
        MAGNET_SWITCH_PIN
    );

    rtc_gpio_pulldown_dis(
        MAGNET_SWITCH_PIN
    );


    // Presence / PIR / LD2410S OT2: active HIGH.
    rtc_gpio_init(
        PIR_PIN
    );

    rtc_gpio_set_direction(
        PIR_PIN,
        RTC_GPIO_MODE_INPUT_ONLY
    );

    rtc_gpio_pullup_dis(
        PIR_PIN
    );

    rtc_gpio_pulldown_en(
        PIR_PIN
    );


    esp_err_t magnetErr =
        esp_sleep_enable_ext0_wakeup(
            MAGNET_SWITCH_PIN,
            0
        );


    uint64_t presenceMask =
        1ULL <<
        (uint32_t)PIR_PIN;

    esp_err_t presenceErr =
        esp_sleep_enable_ext1_wakeup(
            presenceMask,
            ESP_EXT1_WAKEUP_ANY_HIGH
        );


    if (
        magnetErr != ESP_OK ||
        presenceErr != ESP_OK
    ) {

        powerConsole(
            "Sleep wake setup failed | magnet=0x%x | presence=0x%x",
            magnetErr,
            presenceErr
        );

        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT0
        );

        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT1
        );

        rtc_gpio_deinit(
            MAGNET_SWITCH_PIN
        );

        rtc_gpio_deinit(
            PIR_PIN
        );

        pinMode(
            MAGNET_SWITCH_PIN,
            INPUT_PULLUP
        );

        pinMode(
            PIR_PIN,
            INPUT_PULLDOWN
        );

        return false;
    }


    return true;
}


static void prepareCommonSleepState()
{
    if (cfg_led_enabled) {
        statusLedWriteLevel(
            LED_OFF_LEVEL
        );
    }


    // Deep/thermal sleep keeps the original full camera shutdown behavior.
    // Fast sensor standby is used only by enterLightSleep().
    if (cameraInitialized) {
        cameraDeinitRuntime();
    }


    feedWatchdog();
    Serial.flush();
}


static String thermalEventDetails(
    float cpuTempC
)
{
    String details =
        "CPU=";

    if (isfinite(cpuTempC)) {
        details +=
            String(cpuTempC, 1) +
            " C";
    } else {
        details += "--";
    }

    float rtcTempC = 0.0f;

    if (thermalRtcTemperatureC(rtcTempC)) {
        details +=
            " | RTC=" +
            String(rtcTempC, 1) +
            " C";
    } else {
        details +=
            " | RTC=--";
    }

    return details;
}


static void logThermalEvent(
    const char *eventName,
    float cpuTempC
)
{
    String details =
        String(eventName) +
        " | " +
        thermalEventDetails(cpuTempC);

    consoleWrite(
        "THERMAL",
        details
    );

    if (sdReady) {
        logWrite(
            "THERMAL | " +
            details
        );
    }
}


static void recordingThermalReset()
{
    recordingThermalStats =
        {};
}


static void recordingThermalAccumulate(
    float cpuTempC,
    bool rtcTempValid,
    float rtcTempC
)
{
    bool sampled = false;

    if (isfinite(cpuTempC)) {
        if (recordingThermalStats.cpuSamples == 0) {
            recordingThermalStats.cpuMinC =
                cpuTempC;
            recordingThermalStats.cpuMaxC =
                cpuTempC;
        } else {
            if (cpuTempC < recordingThermalStats.cpuMinC)
                recordingThermalStats.cpuMinC = cpuTempC;

            if (cpuTempC > recordingThermalStats.cpuMaxC)
                recordingThermalStats.cpuMaxC = cpuTempC;
        }

        recordingThermalStats.cpuSumC +=
            (double)cpuTempC;

        recordingThermalStats.cpuSamples++;
        sampled = true;
    }

    if (
        rtcTempValid &&
        isfinite(rtcTempC)
    ) {
        if (recordingThermalStats.rtcSamples == 0) {
            recordingThermalStats.rtcMinC =
                rtcTempC;
            recordingThermalStats.rtcMaxC =
                rtcTempC;
        } else {
            if (rtcTempC < recordingThermalStats.rtcMinC)
                recordingThermalStats.rtcMinC = rtcTempC;

            if (rtcTempC > recordingThermalStats.rtcMaxC)
                recordingThermalStats.rtcMaxC = rtcTempC;
        }

        recordingThermalStats.rtcSumC +=
            (double)rtcTempC;

        recordingThermalStats.rtcSamples++;
        sampled = true;
    }

    if (sampled) {
        recordingThermalStats.lastSampleMs =
            millis();
    }
}


static void recordingThermalCollectCurrent()
{
    // If the normal 5-second monitor sampled very recently, do not duplicate
    // that measurement merely because the event is ending.
    if (
        recordingThermalStats.lastSampleMs != 0 &&
        (uint32_t)(
            millis() -
            recordingThermalStats.lastSampleMs
        ) < 1000UL
    ) {
        return;
    }

    float cpuTempC =
        thermalCpuTemperatureC();

    float rtcTempC = 0.0f;

    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    recordingThermalAccumulate(
        cpuTempC,
        rtcTempValid,
        rtcTempC
    );
}


static void recordingThermalLogSummary(
    const char *reason
)
{
    String message =
        "THERMAL | RECORDING_SUMMARY | reason=" +
        String(
            reason && reason[0]
            ? reason
            : "unknown"
        );

    message +=
        " | cpu_samples=" +
        String(recordingThermalStats.cpuSamples);

    if (recordingThermalStats.cpuSamples > 0) {
        message +=
            " | cpu_avg=" +
            String(
                recordingThermalStats.cpuSumC /
                (double)recordingThermalStats.cpuSamples,
                1
            ) +
            " C | cpu_min=" +
            String(
                recordingThermalStats.cpuMinC,
                1
            ) +
            " C | cpu_max=" +
            String(
                recordingThermalStats.cpuMaxC,
                1
            ) +
            " C";
    } else {
        message +=
            " | cpu_avg=-- | cpu_min=-- | cpu_max=--";
    }

    message +=
        " | rtc_samples=" +
        String(recordingThermalStats.rtcSamples);

    if (recordingThermalStats.rtcSamples > 0) {
        message +=
            " | rtc_avg=" +
            String(
                recordingThermalStats.rtcSumC /
                (double)recordingThermalStats.rtcSamples,
                1
            ) +
            " C | rtc_min=" +
            String(
                recordingThermalStats.rtcMinC,
                1
            ) +
            " C | rtc_max=" +
            String(
                recordingThermalStats.rtcMaxC,
                1
            ) +
            " C";
    } else {
        message +=
            " | rtc_avg=-- | rtc_min=-- | rtc_max=--";
    }

    if (sdReady) {
        logWrite(
            message
        );
    }

    consoleWrite(
        "THERMAL",
        message.substring(
            String("THERMAL | ").length()
        )
    );
}


static void recordingThermalFinish(
    const char *reason
)
{
    recordingThermalCollectCurrent();

    recordingThermalLogSummary(
        reason
    );

    recordingThermalReset();
}


static void enterThermalCooldownSleepEarly(
    float cpuTempC,
    bool rtcTempValid,
    float rtcTempC
)
{
    thermalCooldownMarker =
        THERMAL_COOLDOWN_MAGIC;

    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    esp_sleep_enable_timer_wakeup(
        SENSORFORGE_THERMAL_COOLDOWN_US
    );

    if (rtcTempValid) {
        Serial.printf(
            "THERMAL cooldown continues | source=%s | CPU=%.1f C | RTC=%.1f C | recovery CPU<%.1f C RTC<%.1f C | sleep=%lu s\n",
            thermalSourceNameForMask(thermalCooldownSourceMask),
            cpuTempC,
            rtcTempC,
            SENSORFORGE_THERMAL_RECOVERY_C,
            SENSORFORGE_THERMAL_RTC_RECOVERY_C,
            (unsigned long)SENSORFORGE_THERMAL_COOLDOWN_SECONDS
        );
    } else {
        Serial.printf(
            "THERMAL cooldown continues | source=%s | CPU=%.1f C | RTC=-- | recovery CPU<%.1f C RTC<%.1f C | sleep=%lu s\n",
            thermalSourceNameForMask(thermalCooldownSourceMask),
            cpuTempC,
            SENSORFORGE_THERMAL_RECOVERY_C,
            SENSORFORGE_THERMAL_RTC_RECOVERY_C,
            (unsigned long)SENSORFORGE_THERMAL_COOLDOWN_SECONDS
        );
    }

    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}


static void enterThermalEmergencySleep(
    float cpuTempC,
    uint8_t sourceMask
)
{
    if (thermalEmergencyState)
        return;

    thermalEmergencyState =
        true;

    thermalEmergencySourceMask =
        sourceMask;

    g_recordingStartBlocked =
        true;

    thermalCooldownMarker =
        THERMAL_COOLDOWN_MAGIC;

    thermalCooldownSourceMask =
        sourceMask;

    const char *eventName =
        sourceMask == (THERMAL_SOURCE_CPU | THERMAL_SOURCE_RTC)
        ? "EMERGENCY_CPU_RTC"
        : (
            sourceMask & THERMAL_SOURCE_RTC
            ? "EMERGENCY_RTC"
            : "EMERGENCY_CPU"
        );

    logThermalEvent(
        eventName,
        cpuTempC
    );


    // Finalize the active container before taking the SD offline.
    if (recording) {
        stopRecording();
    }


    // WiFi/WebConfig and the camera are the largest avoidable heat sources.
    // The helper also stops the web server cleanly.
    if (webConfigStarted) {
        stopWebConfigWifi(
            "Thermal emergency: WiFi OFF"
        );
    } else {
        MDNS.end();
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }


    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    // During thermal cooldown, motion and magnet wake sources are deliberately
    // disabled. Otherwise a person remaining in front of the radar could wake
    // the hot unit immediately and defeat the cooldown.
    esp_sleep_enable_timer_wakeup(
        SENSORFORGE_THERMAL_COOLDOWN_US
    );

    powerConsole(
        "Thermal emergency sleep | source=%s | CPU=%.1f C | CPU limit=%.1f C | RTC limit=%.1f C | cooldown=%lu s",
        thermalSourceNameForMask(sourceMask),
        cpuTempC,
        SENSORFORGE_THERMAL_EMERGENCY_C,
        SENSORFORGE_THERMAL_RTC_EMERGENCY_C,
        (unsigned long)SENSORFORGE_THERMAL_COOLDOWN_SECONDS
    );


    prepareCommonSleepState();


    if (sdReady) {
        logClose();
        STORAGE.end();
        sdReady = false;
    }

#if defined(STORAGE_SPI)
    SPI.end();
#endif

    delay(100);
    esp_deep_sleep_start();
}


static void thermalMonitorLoop()
{
    if (thermalEmergencyState)
        return;

    uint32_t now =
        millis();

    if (
        (uint32_t)(
            now -
            lastThermalSampleMs
        ) <
        SENSORFORGE_THERMAL_SAMPLE_INTERVAL_MS
    ) {
        return;
    }

    lastThermalSampleMs =
        now;

    float cpuTempC =
        thermalCpuTemperatureC();

    bool cpuTempValid =
        isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    if (recording) {
        recordingThermalAccumulate(
            cpuTempC,
            rtcTempValid,
            rtcTempC
        );
    }


    // Emergency confirmation counters are independent. Either the ESP32 chip
    // or the RTC/enclosure indicator can force the same safe cooldown path.
    if (
        cpuTempValid &&
        cpuTempC >=
            SENSORFORGE_THERMAL_EMERGENCY_C
    ) {
        if (
            thermalCpuEmergencyHighSamples <
            SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES
        ) {
            thermalCpuEmergencyHighSamples++;
        }
    } else {
        thermalCpuEmergencyHighSamples = 0;
    }

    if (
        rtcTempValid &&
        rtcTempC >=
            SENSORFORGE_THERMAL_RTC_EMERGENCY_C
    ) {
        if (
            thermalRtcEmergencyHighSamples <
            SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES
        ) {
            thermalRtcEmergencyHighSamples++;
        }
    } else {
        thermalRtcEmergencyHighSamples = 0;
    }


    // CPU warning with hysteresis.
    bool cpuWarningActive =
        (thermalWarningSourceMask & THERMAL_SOURCE_CPU) != 0;

    if (cpuTempValid) {
        if (
            !cpuWarningActive &&
            cpuTempC >=
                SENSORFORGE_THERMAL_WARNING_C
        ) {
            thermalWarningSourceMask |=
                THERMAL_SOURCE_CPU;

            logThermalEvent(
                "WARNING_CPU",
                cpuTempC
            );

        } else if (
            cpuWarningActive &&
            cpuTempC <
                SENSORFORGE_THERMAL_RECOVERY_C
        ) {
            thermalWarningSourceMask &=
                (uint8_t)~THERMAL_SOURCE_CPU;

            logThermalEvent(
                "RECOVERED_CPU",
                cpuTempC
            );
        }
    }


    // RTC warning with its own lower enclosure-oriented thresholds.
    bool rtcWarningActive =
        (thermalWarningSourceMask & THERMAL_SOURCE_RTC) != 0;

    if (rtcTempValid) {
        if (
            !rtcWarningActive &&
            rtcTempC >=
                SENSORFORGE_THERMAL_RTC_WARNING_C
        ) {
            thermalWarningSourceMask |=
                THERMAL_SOURCE_RTC;

            logThermalEvent(
                "WARNING_RTC",
                cpuTempC
            );

        } else if (
            rtcWarningActive &&
            rtcTempC <
                SENSORFORGE_THERMAL_RTC_RECOVERY_C
        ) {
            thermalWarningSourceMask &=
                (uint8_t)~THERMAL_SOURCE_RTC;

            logThermalEvent(
                "RECOVERED_RTC",
                cpuTempC
            );
        }
    }


    thermalWarningState =
        thermalWarningSourceMask != 0;


    uint8_t emergencySourceMask = 0;

    if (
        thermalCpuEmergencyHighSamples >=
        SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES
    ) {
        emergencySourceMask |=
            THERMAL_SOURCE_CPU;
    }

    if (
        thermalRtcEmergencyHighSamples >=
        SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES
    ) {
        emergencySourceMask |=
            THERMAL_SOURCE_RTC;
    }

    if (emergencySourceMask != 0) {
        enterThermalEmergencySleep(
            cpuTempC,
            emergencySourceMask
        );
    }
}


static void enterDeepSleep()
{
    if (!configureSleepWakeSources())
        return;


    powerConsole(
        "Entering deep sleep | wake=presence GPIO%d HIGH OR magnet GPIO%d LOW",
        PIR_PIN,
        MAGNET_SWITCH_PIN
    );

    sleepDiagState =
        SLEEP_DIAG_UNKNOWN;


    // Mark only the normal configured deep-sleep path. Transport and thermal
    // cooldown use separate functions and therefore cannot be mixed into this
    // accounting. A valid wall clock gives exact elapsed seconds after reboot;
    // an invalid clock still leaves a one-cycle marker with unknown duration.
    normalDeepSleepRtcMagic =
        NORMAL_DEEP_SLEEP_RTC_MAGIC;

    time_t deepSleepStartEpoch =
        time(nullptr);

    normalDeepSleepStartedEpoch =
        deepSleepStartEpoch >= (time_t)1609459200
        ? (int64_t)deepSleepStartEpoch
        : 0;


    prepareCommonSleepState();


    // Deep sleep reboots the ESP32 on wake, therefore close storage
    // cleanly before entering it.
    if (sdReady) {

        // Close the logger file handle explicitly before unmounting SD.
        // This flushes any final filesystem metadata and prevents an
        // open File object from surviving STORAGE.end().
        logClose();

        STORAGE.end();

        sdReady =
            false;
    }


#if defined(STORAGE_SPI)
    SPI.end();
#endif


    delay(100);

    esp_deep_sleep_start();
}


static bool enterLightSleep()
{
    // A new sleep cycle invalidates any unfinished timing sample from an
    // earlier wake.
    wakeTimingActive = false;
    wakeTimingStartUs = 0;
    wakeTimingCameraReadyUs = 0;
    wakeTimingMotionAcceptedUs = 0;
    wakeTimingRecorderReadyUs = 0;
    wakeTimingFrameAttempts = 0;
    wakeTimingStorageUs = 0;
    wakeTimingCameraPrepareUs = 0;
    wakeTimingPathUs = 0;
    wakeTimingRecorderOpenUs = 0;
    wakeTimingMotionLogStartUs = 0;
    wakeTimingMotionLogDoneUs = 0;
    wakeTimingStartRecordingEnterUs = 0;
    wakeTimingPrechecksDoneUs = 0;

    wakeCriticalPathActive = false;
    deferredWakeConsolePending = false;
    deferredWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
    deferredWakeSleptUs = 0;
    deferredWakeCameraWakeUs = 0;
    deferredWakeFastCameraSleep = false;
    deferredWakeCameraWakeOk = false;
    deferredMotionConsolePending = false;
    deferredMotionConsoleMessage = "";
    deferredRecordingStartPending = false;
    deferredRecordingStartMessage = "";
    deferredSleepLogPending = false;

    if (!configureSleepWakeSources())
        return false;


    powerConsole(
        "Entering light sleep | wake=presence GPIO%d HIGH OR magnet GPIO%d LOW",
        PIR_PIN,
        MAGNET_SWITCH_PIN
    );

    sleepDiagState =
        SLEEP_DIAG_UNKNOWN;


    // ---------------------------------------------------------
    // Keep the camera driver alive, but stop the OV3660 itself.
    // ---------------------------------------------------------
    //
    // With CAMERA_GRAB_WHEN_EMPTY there can be completed pre-sleep frames
    // waiting in the driver queue. Hold every configured framebuffer before
    // power-down. They are returned only after the sensor wakes, which makes
    // the next frame delivered to the recorder a genuinely post-wake frame.

    camera_fb_t *heldFrame1 = nullptr;
    camera_fb_t *heldFrame2 = nullptr;
    bool fastCameraSleep = false;


    // On the first quiet boot the physical sensor may still be unknown. Prime
    // once so we can identify it. If it was previously identified as
    // unsupported, do not initialize it merely for sleep: use legacy behavior.
    bool shouldPrimeCamera =
        !cameraInitialized &&
        (
            !cameraFastSleepCapabilityKnown ||
            cameraFastSleepSupported
        );

    if (shouldPrimeCamera) {
        if (!initCamera(
                cfg_camera,
                cfg_resolution,
                cfg_quality
            )) {

            powerConsole(
                "Light sleep | camera prime failed - wake will use normal init"
            );
        }
    }


    if (
        cameraInitialized &&
        cameraSupportsFastLightSleepNow()
    ) {

        bool buffersHeld = true;

        if (cameraFramebufferCount >= 1) {
            heldFrame1 =
                esp_camera_fb_get();

            if (!heldFrame1)
                buffersHeld = false;
        }

        if (
            buffersHeld &&
            cameraFramebufferCount >= 2
        ) {
            heldFrame2 =
                esp_camera_fb_get();

            if (!heldFrame2)
                buffersHeld = false;
        }


        if (
            buffersHeld &&
            cameraEnterSoftPowerDown()
        ) {
            fastCameraSleep = true;

        } else {

            // Unsupported sensor or unexpected camera state: fall back to the
            // previous safe behavior. Recording will perform a normal camera
            // initialization after wake.
            if (heldFrame1) {
                esp_camera_fb_return(heldFrame1);
                heldFrame1 = nullptr;
            }

            if (heldFrame2) {
                esp_camera_fb_return(heldFrame2);
                heldFrame2 = nullptr;
            }

            cameraDeinitRuntime();

            powerConsole(
                "Light sleep | fast camera standby failed - normal-init fallback"
            );
        }
    } else if (cameraInitialized) {

        // The physical sensor is not an OV3660. Preserve the original
        // deinit/reinit behavior rather than applying a sensor-specific
        // register sequence to unsupported hardware.
        uint16_t unsupportedPid =
            cameraDetectedPid;

        cameraDeinitRuntime();

        powerConsole(
            "Light sleep | sensor PID=0x%04x uses normal-init fallback",
            (unsigned int)unsupportedPid
        );
    }


    // Keep SD mounted for fast recording start after wake.
    if (cfg_led_enabled) {
        statusLedWriteLevel(
            LED_OFF_LEVEL
        );
    }

    feedWatchdog();
    Serial.flush();


    uint64_t sleepStartUs =
        (uint64_t)esp_timer_get_time();

    esp_err_t sleepErr =
        esp_light_sleep_start();

    uint64_t wakeStartUs =
        (uint64_t)esp_timer_get_time();

    uint64_t sleptUs =
        wakeStartUs -
        sleepStartUs;


    // ---------------------------------------------------------
    // Wake the camera FIRST.
    // ---------------------------------------------------------
    // Do this before console logging and normal GPIO restoration. While the
    // rest of the wake path runs, the camera can already begin acquiring its
    // first fresh frame as soon as the held buffers are returned.

    bool cameraWakeOk = true;

    if (fastCameraSleep) {
        cameraWakeOk =
            cameraExitSoftPowerDown();
    }


    if (heldFrame1) {
        esp_camera_fb_return(heldFrame1);
        heldFrame1 = nullptr;
    }

    if (heldFrame2) {
        esp_camera_fb_return(heldFrame2);
        heldFrame2 = nullptr;
    }


    uint64_t cameraWakeDoneUs =
        (uint64_t)esp_timer_get_time();


    if (
        fastCameraSleep &&
        !cameraWakeOk
    ) {
        // Do not leave a logically initialized but powered-down camera behind.
        // startRecording() will perform the normal recovery/init path.
        cameraDeinitRuntime();
    }


    // Execution continues here after a light-sleep wake.
    // EXT1 may have used the HOLD mechanism; release both RTC pads before
    // returning them to normal GPIO operation.
    rtc_gpio_hold_dis(
        PIR_PIN
    );

    rtc_gpio_hold_dis(
        MAGNET_SWITCH_PIN
    );

    rtc_gpio_deinit(
        PIR_PIN
    );

    rtc_gpio_deinit(
        MAGNET_SWITCH_PIN
    );

    pinMode(
        PIR_PIN,
        INPUT_PULLDOWN
    );

    pinMode(
        MAGNET_SWITCH_PIN,
        INPUT_PULLUP
    );


    feedWatchdog();


    if (sleepErr != ESP_OK) {

        powerConsole(
            "Light sleep failed | error=0x%x",
            sleepErr
        );

        sleepDiagState =
            SLEEP_DIAG_UNKNOWN;

        resetSleepDelayTimer();

        return false;
    }


    esp_sleep_wakeup_cause_t wakeCause =
        esp_sleep_get_wakeup_cause();

    const bool wakeCanStartRecording =
        configRecordingAllowedNow() &&
        !recordingSafetyCooldownActive();

    // Arm the end-to-end timing sample only for a presence/OT2 wake that can
    // lead directly to a recording. A wake before recording_not_before is not
    // a recording wake and therefore must not enter the deferred-console path.
    if (
        cfg_debug_enabled &&
        wakeCause == ESP_SLEEP_WAKEUP_EXT1 &&
        wakeCanStartRecording
    ) {
        wakeTimingActive = true;
        wakeTimingStartUs = wakeStartUs;
        wakeTimingCameraReadyUs = cameraWakeDoneUs;
        wakeTimingMotionAcceptedUs = 0;
        wakeTimingRecorderReadyUs = 0;
        wakeTimingFrameAttempts = 0;
        wakeTimingStorageUs = 0;
        wakeTimingCameraPrepareUs = 0;
        wakeTimingPathUs = 0;
        wakeTimingRecorderOpenUs = 0;
        wakeTimingMotionLogStartUs = 0;
        wakeTimingMotionLogDoneUs = 0;
        wakeTimingStartRecordingEnterUs = 0;
        wakeTimingPrechecksDoneUs = 0;
    }

    // EXT1 is the time-critical recording wake. Do not touch USB/Serial here:
    // some hosts stall the first Serial.printf() for hundreds of milliseconds
    // after light sleep. Preserve the same information and print it only after
    // the first frame has been written. Non-recording wake causes keep the
    // immediate diagnostic behavior.
    if (
        wakeCause == ESP_SLEEP_WAKEUP_EXT1 &&
        wakeCanStartRecording
    ) {
        wakeCriticalPathActive = true;
        deferredWakeConsolePending = true;
        deferredWakeCause = wakeCause;
        deferredWakeSleptUs = sleptUs;
        deferredWakeCameraWakeUs = cameraWakeDoneUs - wakeStartUs;
        deferredWakeFastCameraSleep = fastCameraSleep;
        deferredWakeCameraWakeOk = cameraWakeOk;
        deferredSleepLogPending = true;
    } else {
        powerConsole(
            "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s",
            sleepWakeCauseName(
                wakeCause
            ),
            (int)wakeCause,
            (unsigned long long)(
                sleptUs /
                1000ULL
            ),
            (unsigned long long)(
                cameraWakeDoneUs -
                wakeStartUs
            ),
            fastCameraSleep
                ? (cameraWakeOk ? "" : " FAILED")
                : " (normal-init fallback)"
        );

        logSleepCycle(
            "light",
            wakeCause,
            sleptUs / 1000ULL
        );
    }


    if (
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT0
    ) {

        // EXT0 is reserved for the active-LOW magnet switch.
        // Adopt LOW as the stable state so the same physical magnet
        // does not immediately create a second toggle event.
        magnetRawState =
            LOW;

        magnetStableState =
            LOW;

        magnetLastChangeMs =
            millis();

        powerConsole(
            "Magnet wake | WiFi/WebConfig ON requested"
        );

        startWebConfig();

        if (webConfigStarted) {
            powerConsole(
                "Magnet wake | WiFi/WebConfig ON"
            );
        } else {
            powerConsole(
                "Magnet wake | WiFi/WebConfig start failed"
            );
        }
    }


    sleepDiagState =
        SLEEP_DIAG_UNKNOWN;


    // Give the next idle period a fresh delay. If presence caused the wake,
    // the next loop iteration starts recording immediately.
    resetSleepDelayTimer();

    return true;
}


static bool tryEnterConfiguredSleep()
{
    if (!sleepModeEnabled()) {

        setSleepDiagState(
            SLEEP_DIAG_DISABLED,
            "Sleep disabled | mode=off"
        );

        return false;
    }


    if (
        recording ||
        motionDetected()
    ) {

        if (wakeCriticalPathActive) {
            // Avoid a potentially blocking POWER/Serial line before the first
            // post-wake frame. The state is still updated so diagnostics remain
            // coherent once the critical path has finished.
            sleepDiagState = SLEEP_DIAG_MOTION;
        } else {
            setSleepDiagState(
                SLEEP_DIAG_MOTION,
                "Sleep blocked | motion active"
            );
        }

        return false;
    }


    // Stay awake on storage failure so the visible fault pattern remains
    // available and periodic recoverSD() attempts can continue.
    if (!sdReady) {

        setSleepDiagState(
            SLEEP_DIAG_SD,
            "Sleep blocked | SD unavailable"
        );

        return false;
    }


    // Never sleep while WebConfig/WiFi is active.
    if (
        webConfigStarted ||
        WiFi.getMode() != WIFI_OFF
    ) {

        setSleepDiagState(
            SLEEP_DIAG_WIFI,
            "Sleep blocked | WiFi/WebConfig active"
        );

        return false;
    }


    if (!presenceWakeIsClear()) {

        if (
            sleepDiagState !=
            SLEEP_DIAG_PRESENCE
        ) {

            sleepDiagState =
                SLEEP_DIAG_PRESENCE;

            if (cfg_debug_enabled) {
                powerConsole(
                    "Sleep blocked | PRESENCE_PIN HIGH | GPIO=%d",
                    PIR_PIN
                );
            }
        }

        return false;
    }


    if (!magnetWakeIsClear()) {

        if (
            sleepDiagState !=
            SLEEP_DIAG_MAGNET
        ) {

            sleepDiagState =
                SLEEP_DIAG_MAGNET;

            powerConsole(
                "Sleep blocked | magnet switch LOW | GPIO=%d",
                MAGNET_SWITCH_PIN
            );
        }

        return false;
    }


    if (sleepIdleSinceMs == 0) {
        resetSleepDelayTimer();
        return false;
    }


    uint32_t elapsedMs =
        (uint32_t)(
            millis() -
            sleepIdleSinceMs
        );

    uint32_t delayMs =
        (uint32_t)cfg_sleep_delay_ms;


    if (elapsedMs < delayMs) {

        uint32_t remainingMs =
            delayMs -
            elapsedMs;

        bool stateChanged =
            sleepDiagState !=
            SLEEP_DIAG_PENDING;

        if (stateChanged) {
            sleepDiagState =
                SLEEP_DIAG_PENDING;
        }

        if (
            stateChanged ||
            (uint32_t)(
                millis() -
                lastSleepPendingDiagMs
            ) >=
            1000UL
        ) {

            lastSleepPendingDiagMs =
                millis();

            if (cfg_debug_enabled) {
                powerConsole(
                    "Sleep pending | mode=%s | remaining=%lu ms",
                    cfg_sleep_mode.c_str(),
                    (unsigned long)remainingMs
                );
            }
        }

        return false;
    }


    if (
        cfg_sleep_mode ==
        "light_sleep"
    ) {

        return
            enterLightSleep();
    }


    if (
        cfg_sleep_mode ==
        "deep_sleep"
    ) {

        // The event safety cooldown is RAM-timed. Do not reboot through deep
        // sleep while it is active, otherwise the lockout would be lost. This
        // is a rare fault path; staying awake until the cooldown expires is
        // safer than silently shortening the protection interval.
        if (recordingSafetyCooldownActive()) {
            return false;
        }

        enterDeepSleep();

        // Deep sleep does not return. If it did because wake setup
        // failed, restart the delay instead of hammering the API.
        sleepDiagState =
            SLEEP_DIAG_UNKNOWN;

        resetSleepDelayTimer();
    }


    return false;
}



// =============================================================
// WEB CONFIG WIFI
// =============================================================

void startWebConfig()
{
    Serial.println(
        "Starting WiFi hotspot for web config..."
    );

    // WebConfig runs as its own access point. Infrastructure WiFi is used
    // only by wifiSyncTime() for the single boot-time NTP attempt.
    WiFi.mode(WIFI_OFF);
    delay(50);

    String apSsid = cfg_hostname;

    if (apSsid.length() == 0) {
        apSsid = "sensorforge";
    }

    // IEEE 802.11 SSIDs are limited to 32 bytes. cfg_hostname is ASCII in
    // normal use, so truncating here also keeps unusually long values safe.
    if (apSsid.length() > 32) {
        apSsid = apSsid.substring(0, 32);
    }

    WiFi.mode(WIFI_AP);

    if (
        cfg_hotspot_password.length() < 8 ||
        cfg_hotspot_password.length() > 63
    ) {

        consoleWrite(
            "WIFI",
            "Hotspot start blocked | invalid password length"
        );

        WiFi.mode(WIFI_OFF);
        return;
    }

    if (!WiFi.softAP(
            apSsid.c_str(),
            cfg_hotspot_password.c_str(),
            1,
            cfg_hotspot_hidden ? 1 : 0
        )) {

        consoleWrite(
            "WIFI",
            "Hotspot start failed"
        );

        WiFi.mode(WIFI_OFF);
        return;
    }

    consoleWrite(
        "WIFI",
        "Hotspot ON | SSID=" + apSsid +
        " | hidden=" + String(cfg_hotspot_hidden) +
        " | IP=" + WiFi.softAPIP().toString()
    );


    if (MDNS.begin(
            cfg_hostname.c_str()
        )) {

        consoleWrite(
            "WIFI",
            "mDNS | http://" +
            cfg_hostname +
            ".local"
        );
    }


    // Kamera bereits für Live-Preview initialisieren.
    // startRecording() verwendet später dieselbe laufende Kamera.
    if (!cameraInitialized) {

        Serial.println(
            "Initializing camera for WebConfig preview..."
        );

        if (!initCamera(
                cfg_camera,
                cfg_resolution,
                cfg_quality
            )) {

            Serial.println(
                "WebConfig: camera init failed - preview unavailable"
            );
        }
    }


    webConfigStart();

    webConfigStarted = true;
}


// =============================================================
// MAGNET / REED WIFI TOGGLE
// =============================================================

void stopWebConfigWifi(
    const char *reason
)
{
    if (webConfigStarted) {

        webConfigStop();

        webConfigStarted =
            false;
    }

    MDNS.end();

    WiFi.softAPdisconnect(true);

    WiFi.disconnect(
        true,
        false
    );

    WiFi.mode(
        WIFI_OFF
    );

    Serial.println(
        reason
    );

    logWrite(
        reason
    );


    // If the system is otherwise idle, begin a fresh sleep delay
    // after WiFi has been shut down.
    resetSleepDelayTimer();
}


void toggleWifiByMagnet()
{
    // Manual magnet action is always immediate.
    // cfg_wifi_timeout_sec controls ONLY automatic inactivity shutdown.
    if (webConfigStarted) {

        stopWebConfigWifi(
            "Magnet switch: WiFi OFF"
        );

        return;
    }


    Serial.println(
        "Magnet switch: WiFi ON requested"
    );

    logWrite(
        "Magnet switch: WiFi ON requested"
    );


    startWebConfig();


    if (webConfigStarted) {

        Serial.println(
            "Magnet switch: WiFi ON"
        );

        logWrite(
            "Magnet switch: WiFi ON"
        );

    } else {

        Serial.println(
            "Magnet switch: WiFi ON failed"
        );

        logWrite(
            "Magnet switch: WiFi ON failed"
        );
    }
}


bool handleWifiInactivityTimeout()
{
    if (
        !webConfigStarted ||
        cfg_wifi_timeout_sec <= 0
    ) {
        return false;
    }


    if (!webConfigInactiveFor(
            (unsigned long)cfg_wifi_timeout_sec
        )) {
        return false;
    }


    Serial.printf(
        "WiFi inactivity timeout: %d s\n",
        cfg_wifi_timeout_sec
    );


    stopWebConfigWifi(
        "WiFi OFF: inactivity timeout"
    );

    return true;
}


void initMagnetSwitch()
{
    pinMode(
        MAGNET_SWITCH_PIN,
        INPUT_PULLUP
    );

    // Adopt the physical boot state without generating an event.
    // If a magnet/button is already present, it must be released
    // once before a later close can toggle WiFi.
    magnetRawState =
        digitalRead(
            MAGNET_SWITCH_PIN
        );

    magnetStableState =
        magnetRawState;

    magnetLastChangeMs =
        millis();


    Serial.printf(
        "Magnet switch GPIO: %d (active LOW)\n",
        MAGNET_SWITCH_PIN
    );
}


void handleMagnetSwitch()
{
    int rawState =
        digitalRead(
            MAGNET_SWITCH_PIN
        );


    if (
        rawState !=
        magnetRawState
    ) {

        magnetRawState =
            rawState;

        magnetLastChangeMs =
            millis();
    }


    if (
        rawState !=
            magnetStableState &&
        millis() -
            magnetLastChangeMs >=
            MAGNET_DEBOUNCE_MS
    ) {

        magnetStableState =
            rawState;


        // Toggle exactly once on the stable HIGH -> LOW edge.
        if (
            magnetStableState ==
            LOW
        ) {

            toggleWifiByMagnet();
        }
    }
}


// =============================================================
// SETUP
// =============================================================


// =============================================================
// LD2410S ROBUST COLD-START
// =============================================================
//
// A full power-on can expose a startup race because the ESP32 can be
// ready before the LD2410S is fully responsive. Retry the UART/config
// handshake and require a real standard report before accepting fast
// UART motion tracking.

static bool startRadarMotionTrackingRobust()
{
    static const uint8_t MAX_ATTEMPTS =
        4;

    static const uint32_t RETRY_DELAY_MS =
        500UL;

    static const uint32_t VERIFY_STANDARD_MS =
        2500UL;


    String lastError;


    for (
        uint8_t attempt = 1;
        attempt <= MAX_ATTEMPTS;
        ++attempt
    ) {

        feedWatchdog();

        Serial.printf(
            "LD2410S fast motion init: attempt %u/%u\n",
            (unsigned)attempt,
            (unsigned)MAX_ATTEMPTS
        );


        String error;

        bool commandOk =
            radarStartMotionTracking(
                error
            );


        if (commandOk) {

            uint32_t verifyStartMs =
                millis();

            bool standardReportSeen =
                false;


            while (
                (uint32_t)(
                    millis() -
                    verifyStartMs
                ) <
                VERIFY_STANDARD_MS
            ) {

                feedWatchdog();
                radarLoop();


                if (
                    radarGateEnergyIsRecent(
                        5000UL
                    )
                ) {

                    standardReportSeen =
                        true;

                    break;
                }


                delay(10);
            }


            if (standardReportSeen) {

                if (attempt > 1) {

                    Serial.printf(
                        "LD2410S fast motion recovered on attempt %u\n",
                        (unsigned)attempt
                    );

                    logWrite(
                        "LD2410S fast motion recovered on retry " +
                        String(attempt)
                    );
                }


                return true;
            }


            lastError =
                "standard mode set but no standard report received";

        } else {

            lastError =
                error;
        }


        if (
            attempt <
            MAX_ATTEMPTS
        ) {

            Serial.println(
                "LD2410S fast motion retry: " +
                lastError
            );


            uint32_t retryStartMs =
                millis();


            while (
                (uint32_t)(
                    millis() -
                    retryStartMs
                ) <
                RETRY_DELAY_MS
            ) {

                feedWatchdog();
                radarLoop();
                delay(10);
            }
        }
    }


    Serial.println(
        "LD2410S fast motion unavailable after retries: " +
        lastError +
        " - falling back to OT2"
    );

    logWrite(
        "LD2410S fast motion unavailable after retries: " +
        lastError
    );


    return false;
}


void setup() {

    // On SPI boards assert SD CS HIGH before Serial, camera, WiFi or any
    // other subsystem starts touching GPIO/SPI state. This gives the card
    // a deterministic deselected state immediately after reset.
    sdSpiDeselect();

    Serial.begin(115200);

    delay(300);

    Serial.println();
    Serial.println("==============================");

#if defined(BOARD_FREENOVE)
    Serial.println("FREENOVE FNK0085 CAMERA");
#elif defined(BOARD_XIAO)
    Serial.println("XIAO ESP32S3 SENSE CAMERA");
#endif

    Serial.println("==============================");


    // ---------------------------------------------------------
    // Wake reason
    // ---------------------------------------------------------

    esp_sleep_wakeup_cause_t wakeCause =
        esp_sleep_get_wakeup_cause();

    esp_reset_reason_t resetReason =
        esp_reset_reason();

    if (resetReason != ESP_RST_DEEPSLEEP) {
        normalDeepSleepRtcMagic = 0;
        normalDeepSleepStartedEpoch = 0;
    }

    const bool wokeByMagnet =
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT0;

    const bool wokeByPresence =
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT1;


    // Restore whichever RTC wake pads were active before deep sleep.
    if (
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT0
    ) {

        // EXT0 is the magnet switch.
        rtc_gpio_deinit(
            MAGNET_SWITCH_PIN
        );
    }


    if (
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT1
    ) {

        // EXT1 is the presence / PIR / LD2410S OT2 input.
        rtc_gpio_deinit(
            PIR_PIN
        );
    }


    // ---------------------------------------------------------
    // Thermal cooldown wake
    // ---------------------------------------------------------

    if (
        thermalCooldownMarker ==
        THERMAL_COOLDOWN_MAGIC
    ) {
        float cpuTempC =
            temperatureRead();

        // Initialize only the lightweight RTC/I2C path before SD, camera and
        // WiFi. This allows an RTC/enclosure-triggered emergency to remain in
        // cooldown even when the ESP32 chip itself has already cooled down.
        rtcBegin();

        float rtcTempC = 0.0f;
        bool rtcTempValid =
            rtcReadTemperatureC(
                rtcTempC
            ) &&
            isfinite(rtcTempC) &&
            rtcTempC >= -40.0f &&
            rtcTempC <= 85.0f;

        bool cpuRecovered =
            isfinite(cpuTempC) &&
            cpuTempC <
                SENSORFORGE_THERMAL_RECOVERY_C;

        bool rtcWasEmergencySource =
            (
                thermalCooldownSourceMask &
                THERMAL_SOURCE_RTC
            ) != 0;

        // If RTC caused the emergency, a missing RTC reading is treated
        // fail-safe and cooldown continues. If CPU alone caused it, an
        // optional/missing RTC does not prevent recovery. Whenever a valid RTC
        // reading is available, it must also be below its recovery threshold.
        bool rtcRecovered =
            rtcTempValid
            ? rtcTempC <
                SENSORFORGE_THERMAL_RTC_RECOVERY_C
            : !rtcWasEmergencySource;

        if (rtcTempValid) {
            Serial.printf(
                "THERMAL cooldown check | wake=%d | source=%s | CPU=%.1f C | RTC=%.1f C | recovery CPU<%.1f C RTC<%.1f C\n",
                (int)wakeCause,
                thermalSourceNameForMask(thermalCooldownSourceMask),
                cpuTempC,
                rtcTempC,
                SENSORFORGE_THERMAL_RECOVERY_C,
                SENSORFORGE_THERMAL_RTC_RECOVERY_C
            );
        } else {
            Serial.printf(
                "THERMAL cooldown check | wake=%d | source=%s | CPU=%.1f C | RTC=-- | recovery CPU<%.1f C RTC<%.1f C\n",
                (int)wakeCause,
                thermalSourceNameForMask(thermalCooldownSourceMask),
                cpuTempC,
                SENSORFORGE_THERMAL_RECOVERY_C,
                SENSORFORGE_THERMAL_RTC_RECOVERY_C
            );
        }

        if (
            !cpuRecovered ||
            !rtcRecovered
        ) {
            enterThermalCooldownSleepEarly(
                cpuTempC,
                rtcTempValid,
                rtcTempC
            );
            return;
        }

        thermalRecoveredSourceMask =
            thermalCooldownSourceMask;

        thermalCooldownMarker = 0;
        thermalCooldownSourceMask = 0;
        thermalRecoveredFromCooldownBoot = true;
        lastCpuTemperatureC = cpuTempC;

        if (rtcTempValid) {
            lastRtcTemperatureC = rtcTempC;
            lastRtcTemperatureValid = true;
            lastRtcTemperatureReadMs = millis();
        }

        Serial.println(
            "THERMAL recovered - normal boot allowed"
        );
    }


    // ---------------------------------------------------------
    // GPIO
    // ---------------------------------------------------------

    pinMode(PIR_PIN, INPUT_PULLDOWN);

    pirStartupGuardUntilMs =
        millis() +
        PIR_STARTUP_GUARD_MS;

    initMagnetSwitch();

    statusLedSetOutput();

    statusLedWriteLevel(
        LED_OFF_LEVEL
    );


    Serial.printf(
        "Presence/OT2 GPIO: %d\n",
        PIR_PIN
    );


    // ---------------------------------------------------------
    // SD
    // ---------------------------------------------------------

    // A hard power loss during a write is the dominant field failure mode.
    // On a real cold/brownout boot, hold SPI SD deselected for an additional
    // settling period before the first command. Deep-sleep wakes deliberately
    // skip this extra delay so motion-trigger latency stays low.
#if defined(STORAGE_SPI)
    const bool sdColdBoot =
        wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED &&
        (
            resetReason == ESP_RST_POWERON ||
            resetReason == ESP_RST_BROWNOUT
        );

    if (sdColdBoot) {
        Serial.printf(
            "SD cold-boot settle | CS=HIGH | wait=%lu ms\n",
            SD_COLD_BOOT_SETTLE_MS
        );

        uint32_t settleStart =
            millis();

        while (
            (uint32_t)(
                millis() -
                settleStart
            ) <
            SD_COLD_BOOT_SETTLE_MS
        ) {
            feedWatchdog();
            delay(10);
        }
    }
#endif

    // Use more attempts at boot than during normal operation. Each failed
    // attempt fully tears down SPI/SD, leaves CS HIGH, waits with backoff,
    // sends 128 idle clocks, and starts the card/filesystem again.
    sdReady =
        initSDWithRetries(
            "boot",
            SD_BOOT_MAX_ATTEMPTS
        );


    if (!sdReady) {

        Serial.println(
            "WARNING: SD init failed after robust cold-boot recovery"
        );
    }


    bootStartedWithoutSd =
        !sdReady;


    // ---------------------------------------------------------
    // SD firmware auto-update
    //
    // Run before config/logger/camera/radar initialization so the
    // old firmware does as little work as possible before rebooting
    // into the new image.
    // ---------------------------------------------------------

    if (sdReady) {

        if (firmwareUpdateFromSdIfPresent()) {

            // ESP.restart() above should not return.
            return;
        }
    }


    // ---------------------------------------------------------
    // Config + logger
    //
    // Config is always loaded, even when SD is unavailable,
    // because LittleFS provides the internal shadow fallback.
    // ---------------------------------------------------------

    loadConfig(
        sdReady
    );


    // ---------------------------------------------------------
    // License
    //
    // Config loading mounts/recoveres the internal LittleFS shadow first.
    // License state is therefore available before any normal product
    // functionality starts. V1 observes the license state but does not yet
    // apply product restrictions.
    // ---------------------------------------------------------

    licenseBegin();


    // ---------------------------------------------------------
    // Transport mode
    //
    // Handle this before the normal RTC/logger/radar/WiFi startup. During transport
    // the device remains a timer-only sleeper and must not react to OT2/PIR.
    // A minimal RTC restore is used only when a journal timestamp would otherwise
    // be unavailable; the persistent logger itself is still not started here.
    // ---------------------------------------------------------

    if (handleTransportModeBoot(
            wakeCause
        )) {
        // Active transport mode normally enters deep sleep and never returns.
        // Keep this guard so a future platform/API change cannot continue into
        // a partial normal boot accidentally.
        return;
    }


    // ---------------------------------------------------------
    // Recording encryption hardware key
    //
    // If encryption is already enabled in config.txt, provision/verify the
    // board-bound ESP32-S3 eFuse HMAC root before camera/WiFi startup. This
    // keeps the one-time key creation out of the motion-to-first-frame path and
    // allows the hardware RNG's internal entropy source to be used safely.
    // Existing keys are only read and verified; no eFuse is written twice.
    // ---------------------------------------------------------

    if (cfg_recording_encryption) {
        if (recordingCryptoEnsureProvisioned()) {
            Serial.printf(
                "Recording encryption hardware key: READY | eFuse KEY%d | source=%s\n",
                recordingCryptoKeySlot(),
                recordingCryptoKeySourceName()
            );
        } else {
            Serial.printf(
                "Recording encryption hardware key: ERROR | status=%s\n",
                recordingCryptoKeyStatusName()
            );
        }
    } else {
        // Read-only discovery only. This never provisions an eFuse while the
        // feature is disabled, but lets diagnostics/WebConfig report a key that
        // may already exist from an earlier activation.
        recordingCryptoBegin();
    }


    // ---------------------------------------------------------
    // Optional realtime clock
    //
    // Auto-detect a DS3231 on the board-specific I2C pins. A valid
    // RTC restores the ESP system clock before logger/recording startup.
    // Missing or invalid RTC hardware is non-fatal; the existing NTP
    // policy remains the fallback. No config.txt keys are required.
    // ---------------------------------------------------------

    rtcBegin();

    {
        time_t armDeadlineEpoch = 0;
        int64_t armRemainingSeconds = 0;

        RecordingNotBeforeState armState =
            configRecordingNotBeforeState(
                &armDeadlineEpoch,
                &armRemainingSeconds
            );

        if (armState == RECORDING_NOT_BEFORE_WAITING) {
            Serial.printf(
                "[ARM] Recording disabled until %s | remaining=%lld s\n",
                configRecordingNotBeforeDisplay().c_str(),
                (long long)armRemainingSeconds
            );
        } else if (armState == RECORDING_NOT_BEFORE_CLOCK_INVALID) {
            Serial.printf(
                "[ARM] Recording BLOCKED | clock invalid | configured=%s\n",
                configRecordingNotBeforeDisplay().c_str()
            );
        } else if (armState == RECORDING_NOT_BEFORE_INVALID) {
            Serial.printf(
                "[ARM] Recording BLOCKED | invalid recording_not_before=%s\n",
                cfg_recording_not_before.c_str()
            );
        } else {
            Serial.printf(
                "[ARM] Recording enabled | schedule=%s\n",
                cfg_recording_not_before.c_str()
            );
        }
    }

    // Prime the thermal diagnostics now. The first loop iteration is allowed
    // to sample immediately so a unit that already starts hot is detected
    // without waiting for a full sample interval.
    thermalCpuTemperatureC();

    float initialRtcTempC = 0.0f;
    thermalRtcTemperatureC(
        initialRtcTempC
    );

    lastThermalSampleMs =
        millis() -
        SENSORFORGE_THERMAL_SAMPLE_INTERVAL_MS;


    // Start an initial idle interval. If there is no motion and WiFi
    // stays off, sleep begins only after cfg_sleep_delay_ms.
    resetSleepDelayTimer();


    if (sdReady) {

        logInit();
        logBlankLine();

        logWrite(
            "RTC | " +
            rtcDiagnosticSummary()
        );

        logWrite(
            "LICENSE | status=" +
            String(licenseStatusName()) +
            " | hardware_id=" +
            licenseHardwareId() +
            " | edition=" +
            String(licenseEditionName())
        );

        logTrackedNormalDeepSleepWake(
            wakeCause,
            resetReason
        );

        logWrite(
            "THERMAL policy | CPU warning=" +
            String(SENSORFORGE_THERMAL_WARNING_C, 1) +
            " C emergency=" +
            String(SENSORFORGE_THERMAL_EMERGENCY_C, 1) +
            " C x" +
            String((unsigned)SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES) +
            " recovery<" +
            String(SENSORFORGE_THERMAL_RECOVERY_C, 1) +
            " C | RTC warning=" +
            String(SENSORFORGE_THERMAL_RTC_WARNING_C, 1) +
            " C emergency=" +
            String(SENSORFORGE_THERMAL_RTC_EMERGENCY_C, 1) +
            " C x" +
            String((unsigned)SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES) +
            " recovery<" +
            String(SENSORFORGE_THERMAL_RTC_RECOVERY_C, 1) +
            " C | cooldown=" +
            String((unsigned long)SENSORFORGE_THERMAL_COOLDOWN_SECONDS) +
            " s"
        );

        if (thermalRecoveredFromCooldownBoot) {
            String recoveredEvent =
                "RECOVERED_AFTER_COOLDOWN_" +
                String(
                    thermalSourceNameForMask(
                        thermalRecoveredSourceMask
                    )
                );

            logThermalEvent(
                recoveredEvent.c_str(),
                thermalCpuTemperatureC()
            );
        }

        int recoveredFiles =
            storageRecoverIncompleteRecordings();

        if (recoveredFiles > 0) {
            logWrite(
                "Boot recovery removed " +
                String(recoveredFiles) +
                " incomplete recording file(s)"
            );
        }

        logBootDiagnostics(
            wakeCause
        );

        // This already contains an exact install time when the clock was
        // valid during the SD update. Otherwise it reports pending time sync.
        firmwareInfoLogStatus();

    } else {

        normalDeepSleepRtcMagic = 0;
        normalDeepSleepStartedEpoch = 0;

        Serial.println(
            "SD unavailable - logger/recording storage disabled"
        );
    }


    debugDump();

    initWatchdog();


    // ---------------------------------------------------------
    // LD2410S UART
    //
    // Keep SD initialization identical to the previously stable
    // startup sequence. The radar UART is intentionally started
    // only AFTER SD mount/config/logger/recovery have completed.
    // ---------------------------------------------------------

    radarBegin();


    // ---------------------------------------------------------
    // Time
    // ---------------------------------------------------------

    // WiFi settings may come from the internal config shadow.
    //
    // Important: evaluate on_missing_time BEFORE NTP can make the
    // clock valid. The result describes only what should happen
    // during this system start.
    const bool wifiAtSystemStart =
        wifiRequestedOnSystemStart();

    maybeSyncTime(
        wifiAtSystemStart
    );


    // If the SD update happened before the clock was valid, use the first
    // valid RTC/NTP-backed system time as the installation timestamp.
    if (
        sdReady &&
        firmwareInfoFinalizePendingInstallTime()
    ) {

        logWrite(
            "Firmware installation timestamp finalized after time sync"
        );

        firmwareInfoLogStatus();
    }


    powerConsole(
        "Sleep configuration | mode=%s | delay=%d ms",
        cfg_sleep_mode.c_str(),
        cfg_sleep_delay_ms
    );


    if (
        wokeByMagnet ||
        wokeByPresence
    ) {

        powerConsole(
            "Wake from deep sleep | source=%s | cause=%s(%d)",
            wokeByMagnet
                ? "MAGNET"
                : "PRESENCE",
            sleepWakeCauseName(
                wakeCause
            ),
            (int)wakeCause
        );
    }


    // ---------------------------------------------------------
    // Web config
    // ---------------------------------------------------------

    // Infrastructure WiFi/NTP and the local hotspot are independent.
    // hotspot_enabled controls automatic AP startup. A magnet wake remains
    // an explicit manual request and therefore overrides this setting.
    if (
        cfg_hotspot_enabled ||
        wokeByMagnet
    ) {

        if (wokeByMagnet) {
            powerConsole(
                "Magnet wake | WiFi/WebConfig ON requested"
            );
        }

        startWebConfig();

        if (wokeByMagnet) {
            powerConsole(
                webConfigStarted
                    ? "Magnet wake | WiFi/WebConfig ON"
                    : "Magnet wake | WiFi/WebConfig start failed"
            );
        }
    }


    // ---------------------------------------------------------
    // LD2410S fast UART motion mode
    // ---------------------------------------------------------

    startRadarMotionTrackingRobust();


    // Sleep wake sources are configured only immediately before
    // entering light/deep sleep:
    //   EXT0 = magnet LOW
    //   EXT1 = presence HIGH


    // ---------------------------------------------------------
    // PIR startup stabilization on normal boot
    // ---------------------------------------------------------

    if (!wokeByPresence) {

        bool guardAnnounced = false;

        while (
            (int32_t)(
                pirStartupGuardUntilMs -
                millis()
            ) > 0
        ) {

            if (!guardAnnounced) {
                Serial.println(
                    "PIR startup guard active"
                );
                guardAnnounced = true;
            }

            feedWatchdog();

            // Keep the 8-Hz standard radar stream drained while waiting.
            radarLoop();

            if (webConfigStarted) {
                webConfigLoop();
            }

            delay(10);
        }

        Serial.printf(
            "Presence/OT2 ready, state=%s\n",
            digitalRead(PIR_PIN) == HIGH
                ? "HIGH"
                : "LOW"
        );
    }


    // ---------------------------------------------------------
    // Decide what to do after boot
    // ---------------------------------------------------------

    if (wokeByPresence) {

        Serial.println(
            "Wake reason: PRESENCE"
        );

        logWrite(
            "Wake by presence"
        );

        if (!startRecording()) {
            resetSleepDelayTimer();
        }

    }

    else if (
        motionDetected()
    ) {

        // On a normal power-on, use the fast UART motion detector.
        // OT2 is used here only automatically if UART motion tracking
        // failed and physicalMotionActive() falls back to it.

        Serial.println(
            "Motion active on normal boot"
        );

        logWrite(
            "Motion active on boot"
        );

        if (!startRecording()) {
            resetSleepDelayTimer();
        }

    }

    else {

        Serial.println(
            "Normal boot - no motion"
        );

        // Start the idle delay only after all boot-time initialization
        // and the presence startup guard have completed.
        resetSleepDelayTimer();

#if WEB_CONFIG_KEEP_AWAKE
        if (webConfigStarted) {

            Serial.println(
                "WebConfig active - staying awake"
            );

            logWrite(
                "Normal boot - WebConfig active"
            );

        } else {
#endif

            logWrite(
                "Normal boot - idle"
            );

            // Sleep configuration was already printed with a timestamp
            // after time synchronization.

#if WEB_CONFIG_KEEP_AWAKE
        }
#endif
    }
}


// =============================================================
// LOOP
// =============================================================

void loop() {

    feedWatchdog();

    // Non-blocking recording/fault LED state.
    updateStatusLed();

    // Reed contact / test pushbutton.
    // Active only while the ESP32 is awake.
    handleMagnetSwitch();

    // Consume the LD2410S compact UART report stream.
    // Recording decisions still use the dedicated OT2 GPIO.
    radarLoop();

    // Independent thermal guard. It remains active during WebConfig and
    // recording and can force a timer-only deep-sleep cooldown.
    thermalMonitorLoop();

    // Release the recording-event safety lockout exactly once its configured
    // cooldown has elapsed. This runs before motion decisions and WebConfig.
    serviceRecordingSafetyCooldown();

    static unsigned long lastDebug = 0;


    // Cancel a stale timing sample if a presence wake did not result in a
    // successfully written frame within five seconds. This runs before any
    // idle-path early return, so a later unrelated motion event cannot be
    // attributed to an old wake.
    if (
        wakeTimingActive &&
        (uint64_t)esp_timer_get_time() -
            wakeTimingStartUs >
            5000000ULL
    ) {
        if (cfg_debug_enabled) {
            consolePrintf(
                "DEBUG",
                "WAKE_TIMING | timeout | elapsed=%llu us | motion=%llu us | recorder_ready=%llu us | attempts=%lu",
                (unsigned long long)(
                    (uint64_t)esp_timer_get_time() -
                    wakeTimingStartUs
                ),
                (unsigned long long)(
                    wakeTimingMotionAcceptedUs > wakeTimingStartUs
                    ? wakeTimingMotionAcceptedUs - wakeTimingStartUs
                    : 0
                ),
                (unsigned long long)(
                    wakeTimingRecorderReadyUs > wakeTimingStartUs
                    ? wakeTimingRecorderReadyUs - wakeTimingStartUs
                    : 0
                ),
                (unsigned long)wakeTimingFrameAttempts
            );
        }

        wakeTimingActive = false;

        // No recording reached a first frame inside the diagnostic window.
        // There is no longer a latency-sensitive first-frame path to protect,
        // so restore normal console behavior and emit the deferred wake line.
        if (wakeCriticalPathActive) {
            wakeCriticalPathActive = false;

            if (deferredWakeConsolePending) {
                powerConsole(
                    "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s | console=deferred-timeout",
                    sleepWakeCauseName(deferredWakeCause),
                    (int)deferredWakeCause,
                    (unsigned long long)(deferredWakeSleptUs / 1000ULL),
                    (unsigned long long)deferredWakeCameraWakeUs,
                    deferredWakeFastCameraSleep
                        ? (deferredWakeCameraWakeOk ? "" : " FAILED")
                        : " (normal-init fallback)"
                );
                deferredWakeConsolePending = false;
            }

            if (deferredMotionConsolePending) {
                consoleWrite(
                    "MOTION",
                    deferredMotionConsoleMessage
                );
                deferredMotionConsolePending = false;
                deferredMotionConsoleMessage = "";
            }

            if (deferredSleepLogPending) {
                logSleepCycle(
                    "light",
                    deferredWakeCause,
                    deferredWakeSleptUs / 1000ULL
                );
                deferredSleepLogPending = false;
            }
        }
    }


    // ---------------------------------------------------------
    // Debug output
    // ---------------------------------------------------------

    if (
        !wakeCriticalPathActive &&
        millis() - lastDebug >=
        30000UL
    ) {

        int ot2 =
            digitalRead(PIR_PIN);

        int radarMotion =
            radarMotionActive() ? 1 : 0;

        int simulatedPir =
            simulatedMotionActive() ? 1 : 0;

        float cpuTempC =
            thermalCpuTemperatureC();

        float rtcTempC = 0.0f;
        bool rtcTempValid =
            thermalRtcTemperatureC(
                rtcTempC
            );

        char rtcTempText[20];

        if (rtcTempValid) {
            snprintf(
                rtcTempText,
                sizeof(rtcTempText),
                "%.1fC",
                rtcTempC
            );
        } else {
            strncpy(
                rtcTempText,
                "--",
                sizeof(rtcTempText)
            );
            rtcTempText[
                sizeof(rtcTempText) - 1
            ] = '\0';
        }

        char timestamp[40];
        formatConsoleTimestamp(
            timestamp,
            sizeof(timestamp)
        );

        const char *thermalSource =
            thermalSourceName();

        Serial.printf(
            "[%s] [STATUS] OT2=%d | RADAR=%d | SIM=%d | MOTION=%d | RECORDING=%d | CPU=%.1fC | RTC=%s | THERMAL=%s%s%s\n",
            timestamp,
            ot2,
            radarMotion,
            simulatedPir,
            motionDetected() ? 1 : 0,
            recording ? 1 : 0,
            cpuTempC,
            rtcTempText,
            thermalStateName(),
            strcmp(thermalSource, "NONE") != 0 ? "(" : "",
            strcmp(thermalSource, "NONE") != 0 ? thermalSource : "",
            strcmp(thermalSource, "NONE") != 0 ? ")" : ""
        );

        lastDebug =
            millis();
    }


    // ---------------------------------------------------------
    // Web Config
    // ---------------------------------------------------------

    if (webConfigStarted)
        webConfigLoop();


    bool wifiTimedOut =
        handleWifiInactivityTimeout();


    if (
        wifiTimedOut &&
        !recording &&
        !motionDetected()
    ) {
        // WiFi has just gone OFF. Start the configured idle delay
        // instead of sleeping immediately.
        resetSleepDelayTimer();
    }


    // ---------------------------------------------------------
    // Nothing to record
    // ---------------------------------------------------------

    if (!recording) {

        if (tryEnterConfiguredSleep()) {
            return;
        }


        if (!sdReady) {

            static unsigned long lastSdRetryMs = 0;

            const bool previewActive =
                webConfigStarted &&
                webConfigCameraPreviewActive();

            if (
                !previewActive &&
                millis() - lastSdRetryMs >
                SD_RECOVERY_RETRY_MS
            ) {

                lastSdRetryMs =
                    millis();

                recoverSD();
            }

            delay(10);
            return;
        }


        if (diskSpaceBlocked) {

            if (
                millis() -
                lastBlockedSpaceRetryMs >
                SPACE_BLOCK_RETRY_MS
            ) {

                lastBlockedSpaceRetryMs =
                    millis();

                if (storagePrepareForRecording()) {

                    diskSpaceBlocked =
                        false;

                    logWrite(
                        "Storage reserve available again"
                    );
                }
            }

            if (diskSpaceBlocked) {
                delay(10);
                return;
            }
        }


        if (motionDetected()) {

            if (
                wakeTimingActive &&
                wakeTimingMotionAcceptedUs == 0
            ) {
                wakeTimingMotionAcceptedUs =
                    (uint64_t)esp_timer_get_time();
            }

            // Any new idle-time activity restarts sleep_delay_ms.
            resetSleepDelayTimer();

            // Prepare the MOTION message, but on the post-light-sleep critical
            // path do not send it to USB/Serial yet. The previous measurement
            // showed that the first console write can block for hundreds of ms.
            if (
                wakeTimingActive &&
                wakeTimingMotionLogStartUs == 0
            ) {
                wakeTimingMotionLogStartUs =
                    (uint64_t)esp_timer_get_time();
            }

            String motionMessage;

            if (
                simulatedMotionActive() &&
                !physicalMotionActive()
            ) {
                motionMessage = "simulated";
            } else if (
                radarMotionTrackingAvailable()
            ) {
                char motionBuffer[96];
                snprintf(
                    motionBuffer,
                    sizeof(motionBuffer),
                    "radar | gate=%d | energy=%.1f dB",
                    radarLastMotionGate(),
                    radarLastMotionEnergyDb()
                );
                motionMessage = String(motionBuffer);
            } else {
                motionMessage = "OT2 fallback";
            }

            if (wakeCriticalPathActive) {
                deferredMotionConsoleMessage = motionMessage;
                deferredMotionConsolePending = true;
            } else {
                consoleWrite(
                    "MOTION",
                    motionMessage
                );
            }

            if (
                wakeTimingActive &&
                wakeTimingMotionLogDoneUs == 0
            ) {
                wakeTimingMotionLogDoneUs =
                    (uint64_t)esp_timer_get_time();
            }

            if (!startRecording()) {
                // No first frame will arrive to release the deferred console
                // messages. Restore normal diagnostics immediately on failure.
                if (wakeCriticalPathActive) {
                    wakeCriticalPathActive = false;

                    if (deferredWakeConsolePending) {
                        powerConsole(
                            "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s | console=deferred",
                            sleepWakeCauseName(deferredWakeCause),
                            (int)deferredWakeCause,
                            (unsigned long long)(deferredWakeSleptUs / 1000ULL),
                            (unsigned long long)deferredWakeCameraWakeUs,
                            deferredWakeFastCameraSleep
                                ? (deferredWakeCameraWakeOk ? "" : " FAILED")
                                : " (normal-init fallback)"
                        );
                        deferredWakeConsolePending = false;
                    }

                    if (deferredMotionConsolePending) {
                        consoleWrite(
                            "MOTION",
                            deferredMotionConsoleMessage
                        );
                        deferredMotionConsolePending = false;
                        deferredMotionConsoleMessage = "";
                    }

                    if (deferredSleepLogPending) {
                        logSleepCycle(
                            "light",
                            deferredWakeCause,
                            deferredWakeSleptUs / 1000ULL
                        );
                        deferredSleepLogPending = false;
                    }
                }

                delay(100);
            }
        }

        delay(10);
        return;
    }


    // ---------------------------------------------------------
    // Motion detected
    // ---------------------------------------------------------

    if (motionDetected()) {

        // While fast radar motion (2 s hold) or software simulation
        // is active, reset the existing post-recording timer.

        lastMotionMs =
            millis();
    }


    // ---------------------------------------------------------
    // Safety limit for the COMPLETE recording event.
    // This deliberately runs before segment rotation and therefore spans
    // every AVI/MKV segment belonging to the same continuous event.
    // ---------------------------------------------------------

    if (
        cfg_recording_event_max_seconds > 0 &&
        recordingEventStartMs != 0 &&
        (uint64_t)(uint32_t)(
            millis() -
            recordingEventStartMs
        ) >=
        (uint64_t)cfg_recording_event_max_seconds *
        1000ULL
    ) {
        triggerRecordingEventSafetyLimit();
        return;
    }


    // ---------------------------------------------------------
    // Add camera frame
    // ---------------------------------------------------------

    uint32_t nowUs = micros();

    uint32_t frameIntervalUs =
        1000000UL /
        (uint32_t)max(cfg_fps, 1);

    if (
        lastFrameUs == 0 ||
        (uint32_t)(nowUs - lastFrameUs) >= frameIntervalUs
    ) {

        uint32_t framesBefore =
            recorderGetFrameCount();

        uint64_t frameCallStartUs = 0;

        if (wakeTimingActive) {
            frameCallStartUs =
                (uint64_t)esp_timer_get_time();

            wakeTimingFrameAttempts++;
        }

        recorderAddFrame();

        uint64_t frameCallDoneUs = 0;

        if (wakeTimingActive) {
            frameCallDoneUs =
                (uint64_t)esp_timer_get_time();
        }

        uint32_t framesAfter =
            recorderGetFrameCount();

        lastFrameUs =
            nowUs;


        if (!recorderIsHealthy()) {

            logWrite(
                "Recorder write error - stopping recording"
            );

            Serial.println(
                "Recorder write error - stopping recording"
            );

            stopRecording();

            return;
        }


        if (framesAfter == framesBefore) {

            consecutiveFrameFailures++;

            if (
                consecutiveFrameFailures >=
                MAX_CONSECUTIVE_FRAME_FAILURES
            ) {

                recoverCamera();
            }

        } else {

            consecutiveFrameFailures =
                0;

            // The first frame is now safely inside the video container. Release
            // all non-essential wake/start diagnostics that were held back to
            // prevent USB/Serial and logger flush latency from delaying capture.
            uint64_t deferredLogFlushUs = 0;

            if (wakeCriticalPathActive) {
                uint64_t deferredLogFlushStartUs =
                    wakeTimingActive
                        ? (uint64_t)esp_timer_get_time()
                        : 0;

                wakeCriticalPathActive = false;

                if (deferredWakeConsolePending) {
                    powerConsole(
                        "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s | console=deferred",
                        sleepWakeCauseName(deferredWakeCause),
                        (int)deferredWakeCause,
                        (unsigned long long)(deferredWakeSleptUs / 1000ULL),
                        (unsigned long long)deferredWakeCameraWakeUs,
                        deferredWakeFastCameraSleep
                            ? (deferredWakeCameraWakeOk ? "" : " FAILED")
                            : " (normal-init fallback)"
                    );
                    deferredWakeConsolePending = false;
                }

                if (deferredMotionConsolePending) {
                    consoleWrite(
                        "MOTION",
                        deferredMotionConsoleMessage
                    );
                    deferredMotionConsolePending = false;
                    deferredMotionConsoleMessage = "";
                }

                if (deferredRecordingStartPending) {
                    consoleWrite(
                        "REC",
                        deferredRecordingStartMessage
                    );

                    logWrite(
                        "Recording " +
                        deferredRecordingStartMessage
                    );

                    deferredRecordingStartPending = false;
                    deferredRecordingStartMessage = "";
                }

                if (deferredSleepLogPending) {
                    logSleepCycle(
                        "light",
                        deferredWakeCause,
                        deferredWakeSleptUs / 1000ULL
                    );
                    deferredSleepLogPending = false;
                }

                if (wakeTimingActive) {
                    deferredLogFlushUs =
                        (uint64_t)esp_timer_get_time() -
                        deferredLogFlushStartUs;
                }
            }

            // First successfully written post-wake frame. All timestamps were
            // captured before this print, so the log itself cannot inflate the
            // measured values. recorderAddFrame() includes JPEG acquisition,
            // first-frame container/header work and the SD write.
            if (wakeTimingActive) {

                uint64_t motionUs =
                    wakeTimingMotionAcceptedUs > wakeTimingStartUs
                    ? wakeTimingMotionAcceptedUs - wakeTimingStartUs
                    : 0;

                uint64_t recorderUs =
                    wakeTimingRecorderReadyUs > wakeTimingStartUs
                    ? wakeTimingRecorderReadyUs - wakeTimingStartUs
                    : 0;

                uint64_t frameRequestUs =
                    frameCallStartUs > wakeTimingStartUs
                    ? frameCallStartUs - wakeTimingStartUs
                    : 0;

                uint64_t firstFrameWrittenUs =
                    frameCallDoneUs > wakeTimingStartUs
                    ? frameCallDoneUs - wakeTimingStartUs
                    : 0;

                uint64_t cameraReadyUs =
                    wakeTimingCameraReadyUs > wakeTimingStartUs
                    ? wakeTimingCameraReadyUs - wakeTimingStartUs
                    : 0;

                uint64_t frameCallUs =
                    frameCallDoneUs >= frameCallStartUs
                    ? frameCallDoneUs - frameCallStartUs
                    : 0;

                consolePrintf(
                    "DEBUG",
                    "WAKE_TIMING | camera_ready=%llu us | motion=%llu us | recorder_ready=%llu us | frame_request=%llu us | first_frame_written=%llu us | frame_call=%llu us | attempts=%lu",
                    (unsigned long long)cameraReadyUs,
                    (unsigned long long)motionUs,
                    (unsigned long long)recorderUs,
                    (unsigned long long)frameRequestUs,
                    (unsigned long long)firstFrameWrittenUs,
                    (unsigned long long)frameCallUs,
                    (unsigned long)wakeTimingFrameAttempts
                );

                consolePrintf(
                    "DEBUG",
                    "WAKE_STAGES | storage=%llu us | camera_prepare=%llu us | path=%llu us | recorder_open=%llu us | motion_to_recorder=%llu us | recorder_to_first_frame=%llu us",
                    (unsigned long long)wakeTimingStorageUs,
                    (unsigned long long)wakeTimingCameraPrepareUs,
                    (unsigned long long)wakeTimingPathUs,
                    (unsigned long long)wakeTimingRecorderOpenUs,
                    (unsigned long long)(
                        recorderUs >= motionUs
                            ? recorderUs - motionUs
                            : 0ULL
                    ),
                    (unsigned long long)(
                        firstFrameWrittenUs >= recorderUs
                            ? firstFrameWrittenUs - recorderUs
                            : 0ULL
                    )
                );

                consolePrintf(
                    "DEBUG",
                    "WAKE_DEFER | recorder_to_request=%llu us | postframe_log_flush=%llu us | console_deferred=1",
                    (unsigned long long)(
                        frameRequestUs >= recorderUs
                            ? frameRequestUs - recorderUs
                            : 0ULL
                    ),
                    (unsigned long long)deferredLogFlushUs
                );

                uint64_t motionLogStartOffsetUs =
                    wakeTimingMotionLogStartUs > wakeTimingStartUs
                    ? wakeTimingMotionLogStartUs - wakeTimingStartUs
                    : 0;

                uint64_t motionLogDoneOffsetUs =
                    wakeTimingMotionLogDoneUs > wakeTimingStartUs
                    ? wakeTimingMotionLogDoneUs - wakeTimingStartUs
                    : 0;

                uint64_t startRecordingEnterOffsetUs =
                    wakeTimingStartRecordingEnterUs > wakeTimingStartUs
                    ? wakeTimingStartRecordingEnterUs - wakeTimingStartUs
                    : 0;

                uint64_t prechecksDoneOffsetUs =
                    wakeTimingPrechecksDoneUs > wakeTimingStartUs
                    ? wakeTimingPrechecksDoneUs - wakeTimingStartUs
                    : 0;

                uint64_t motionLogDurationUs =
                    wakeTimingMotionLogDoneUs >= wakeTimingMotionLogStartUs &&
                    wakeTimingMotionLogStartUs != 0
                    ? wakeTimingMotionLogDoneUs - wakeTimingMotionLogStartUs
                    : 0;

                uint64_t motionToLogStartUs =
                    wakeTimingMotionLogStartUs >= wakeTimingMotionAcceptedUs &&
                    wakeTimingMotionAcceptedUs != 0
                    ? wakeTimingMotionLogStartUs - wakeTimingMotionAcceptedUs
                    : 0;

                uint64_t logToStartRecordingUs =
                    wakeTimingStartRecordingEnterUs >= wakeTimingMotionLogDoneUs &&
                    wakeTimingMotionLogDoneUs != 0
                    ? wakeTimingStartRecordingEnterUs - wakeTimingMotionLogDoneUs
                    : 0;

                uint64_t motionToStartRecordingUs =
                    wakeTimingStartRecordingEnterUs >= wakeTimingMotionAcceptedUs &&
                    wakeTimingMotionAcceptedUs != 0
                    ? wakeTimingStartRecordingEnterUs - wakeTimingMotionAcceptedUs
                    : 0;

                uint64_t startPrechecksUs =
                    wakeTimingPrechecksDoneUs >= wakeTimingStartRecordingEnterUs &&
                    wakeTimingStartRecordingEnterUs != 0
                    ? wakeTimingPrechecksDoneUs - wakeTimingStartRecordingEnterUs
                    : 0;

                // Keep each line below consolePrintf()'s fixed message buffer.
                consolePrintf(
                    "DEBUG",
                    "WAKE_PATH | motion=%llu us | log_start=%llu us | log_done=%llu us | start_enter=%llu us | prechecks=%llu us",
                    (unsigned long long)motionUs,
                    (unsigned long long)motionLogStartOffsetUs,
                    (unsigned long long)motionLogDoneOffsetUs,
                    (unsigned long long)startRecordingEnterOffsetUs,
                    (unsigned long long)prechecksDoneOffsetUs
                );

                consolePrintf(
                    "DEBUG",
                    "WAKE_PATH_DUR | motion_to_log=%llu us | motion_log=%llu us | log_to_start=%llu us | motion_to_start=%llu us | start_prechecks=%llu us | console_deferred=1",
                    (unsigned long long)motionToLogStartUs,
                    (unsigned long long)motionLogDurationUs,
                    (unsigned long long)logToStartRecordingUs,
                    (unsigned long long)motionToStartRecordingUs,
                    (unsigned long long)startPrechecksUs
                );

                wakeTimingActive = false;
                    }
        }
    }


    // ---------------------------------------------------------
    // Free-space guard during long recordings.
    // Finalize first; delete/roll over only afterwards.
    // ---------------------------------------------------------

    if (
        millis() -
        lastSpaceCheckMs >=
        SPACE_CHECK_INTERVAL_MS
    ) {

        lastSpaceCheckMs =
            millis();

        if (!storageHasRequiredFreeSpace()) {

            logWrite(
                "Storage reserve reached during recording"
            );

            stopRecording();

            if (
                cfg_disk_full_action ==
                "rollover"
            ) {

                diskSpaceBlocked =
                    !storagePrepareForRecording();

            } else {

                diskSpaceBlocked =
                    true;
            }

            return;
        }
    }


    // ---------------------------------------------------------
    // Rotate long recordings into bounded segments.
    // Motion/post-recording continues; only the file changes.
    // ---------------------------------------------------------

    bool postWindowActive =
        (uint32_t)(millis() - lastMotionMs) <=
        (uint32_t)cfg_post_ms;

    if (postWindowActive) {

        bool containerLimitReached =
            recorderHitSizeLimit();

        bool timeLimitReached =
            cfg_recording_segment_seconds > 0 &&
            (uint64_t)(uint32_t)(
                millis() - recordingSegmentStartMs
            ) >=
            (uint64_t)cfg_recording_segment_seconds *
            1000ULL;

        bool sizeLimitReached =
            false;

        if (cfg_recording_segment_max_mb > 0) {

            uint64_t maxSegmentBytes =
                (uint64_t)cfg_recording_segment_max_mb *
                1024ULL *
                1024ULL;

            sizeLimitReached =
                recorderGetBytesWritten() >=
                maxSegmentBytes;
        }


        if (
            containerLimitReached ||
            timeLimitReached ||
            sizeLimitReached
        ) {

            String reason;

            if (containerLimitReached) {
                reason = "container";
            } else if (
                timeLimitReached &&
                sizeLimitReached
            ) {
                reason =
                    String(cfg_recording_segment_seconds) +
                    "s/" +
                    String(cfg_recording_segment_max_mb) +
                    "MB";
            } else if (timeLimitReached) {
                reason =
                    String(cfg_recording_segment_seconds) +
                    "s";
            } else {
                reason =
                    String(cfg_recording_segment_max_mb) +
                    "MB";
            }

            rotateRecordingSegment(reason);
            return;
        }
    }


    // ---------------------------------------------------------
    // Stop after post-motion time
    // ---------------------------------------------------------

    if (
        millis() - lastMotionMs >
        (unsigned long)cfg_post_ms
    ) {

        stopRecording();

#if WEB_CONFIG_KEEP_AWAKE
        if (webConfigStarted) {
            return;
        }
#endif

        // stopRecording() started sleep_delay_ms. The normal idle
        // loop decides later whether to enter light/deep sleep.
        return;
    }
}
