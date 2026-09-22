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
#include <Preferences.h>

#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"

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
#include "recording_storage.h"
#include "mkv_writer.h"
#include "log_storage.h"
#include "image_motion.h"
#include "motion_diagnostics.h"


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

// Runtime recording-performance monitor. This intentionally does NOT add a
// frame queue or change/drop frames. It only measures how long the existing
// synchronous camera+container+storage call takes compared with the configured
// frame budget. Logging is deferred to safe segment/event boundaries so the
// monitor itself does not add SD/Serial work to the per-frame critical path.
struct RecordingPerformanceStats {
    uint32_t frameCalls;
    uint32_t nearBudgetFrames;
    uint32_t overBudgetFrames;
    uint32_t worstCallUs;
    uint64_t totalCallUs;
};

static RecordingPerformanceStats recordingPerformanceSegment = {};
static RecordingPerformanceStats recordingPerformanceEvent = {};

// Explicit prototypes are required in the .ino because Arduino's sketch
// preprocessor otherwise auto-generates prototypes before the custom struct
// above is visible, which causes "RecordingPerformanceStats was not declared".
static void recordingPerformanceReset(
    RecordingPerformanceStats &stats
);
static void recordingPerformanceNoteCall(
    RecordingPerformanceStats &stats,
    uint32_t callUs,
    uint32_t frameBudgetUs
);
static void recordingPerformanceLogSummary(
    const char *scope,
    const RecordingPerformanceStats &stats
);

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

// Image verification is a recording-decision layer above the existing radar/OT2
// motion source. A rejected trigger is latched until physical motion clears so a
// 2-second radar hold cannot repeatedly wake the camera analyzer in a tight loop.
static bool imageVerifyRejectedUntilMotionClear = false;

// image_only continuously observes the camera while the ESP is awake and idle.
// The physical presence sensor remains the hardware wake source in sleep, but
// it is not allowed to decide whether a recording starts in this mode.
static const uint32_t IMAGE_ONLY_IDLE_SCAN_INTERVAL_MS = 250UL;
static uint32_t imageOnlyLastIdleScanMs = 0;

// WiFi-off motion diagnostics. The baseline is captured when WebConfig/WiFi
// shuts down and summarized once when the magnet brings WiFi back. This adds
// no SD writes to the motion-critical offline period itself.
static bool offlineMotionBaselineValid = false;
static uint32_t offlinePresenceTriggerBaseline = 0;
static uint32_t offlinePresenceRawRiseBaseline = 0;
static uint32_t offlinePresenceGlitchBaseline = 0;
static bool offlinePresenceRtcBackendActive = false;


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

// A persistent storage fault must not keep the ESP32-S3, camera and radio
// awake indefinitely. After the normal boot/runtime recovery attempts fail,
// SensorForge enters a dedicated deep-sleep fault mode as soon as WebConfig
// is no longer active. Only the magnet/reed service input and a periodic timer
// can wake it. Periodic fault wakes retry SD quickly without starting WiFi,
// radar or the normal camera stack when the card is still unavailable.
static const uint32_t STORAGE_FAULT_RETRY_SECONDS = 300UL;
static const uint32_t STORAGE_FAULT_WEB_IDLE_TIMEOUT_SECONDS = 120UL;
static const uint8_t SD_STORAGE_FAULT_WAKE_MAX_ATTEMPTS = 2;

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

// Continuous-shooter deep-sleep marker. It preserves the exact scheduled
// wall-clock slot across a deep-sleep reboot. PSRAM itself does not survive
// deep sleep, therefore queued JPEGs are flushed before this path is entered.
static const uint32_t SHOOTER_DEEP_SLEEP_RTC_MAGIC = 0x53465348UL; // "SFSH"
RTC_DATA_ATTR uint32_t shooterDeepSleepRtcMagic = 0;
RTC_DATA_ATTR int64_t shooterDeepSleepScheduledWallUs = 0;

// Runtime continuous-shooter scheduler and PSRAM queue.
static int shooterScheduleCachedIntervalMs = -1;
static int shooterScheduleCachedEnabled = -1;
static bool shooterScheduleWallClockMode = false;
static int64_t shooterLastHandledWallSlotUs = -1;
static uint64_t shooterFallbackNextDueUs = 0;
static int64_t shooterLightSleepScheduledWallUs = 0;
static uint8_t shooterLightSleepWakeKind = 0;

static uint8_t *shooterBuffer = nullptr;
static size_t shooterBufferCapacity = 0;
static size_t shooterBufferUsed = 0;
static uint32_t shooterBufferFrames = 0;
static uint32_t shooterBufferFirstQueuedMs = 0;
static bool shooterBufferFlushDue = false;
static uint64_t shooterLastAcceptedMonoUs = 0;
static uint32_t shooterAcceptedFrames = 0;
static uint64_t shooterAcceptedJpegBytes = 0;
static uint32_t shooterRejectedDark = 0;
static uint32_t shooterRejectedSimilar = 0;
static bool shooterLastBrightnessValid = false;
static float shooterLastBrightnessMean = 0.0f;
static uint8_t shooterLastBrightnessPeak = 0;
static uint32_t shooterLastBrightnessMs = 0;
static uint32_t shooterLastAnalyzerErrorLogMs = 0;
static uint32_t shooterLastBufferAllocErrorLogMs = 0;
static uint32_t shooterLastFailureLogMs = 0;
static bool shooterInitialWarmupDone = false;

// Pure continuous-shooter timer wakes can happen multiple times per second.
// Keep their light-sleep diagnostics in RAM and fold them into the next
// shooter flush summary instead of generating one SD-backed log line per wake.
static uint32_t shooterSleepCyclesPending = 0;
static uint64_t shooterSleepTotalMsPending = 0;
static uint32_t shooterSleepMinMsPending = 0;
static uint32_t shooterSleepMaxMsPending = 0;

// Dedicated storage-fault sleep marker. This is deliberately independent of
// normal deep sleep and transport mode so a timer wake can retry only the SD
// path and return to low power immediately if storage is still unavailable.
static const uint32_t STORAGE_FAULT_RTC_MAGIC = 0x53465344UL; // "SFSD"
RTC_DATA_ATTR uint32_t storageFaultRtcMagic = 0;
RTC_DATA_ATTR uint32_t storageFaultRtcCycle = 0;

static void storageFaultClearRtcState();
static bool enterStorageFaultLowPowerSleep(const char *reason);
static bool tryEnterStorageFaultLowPower();

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
bool initCamera(const String &model, const String &resolution, int quality);
static bool cameraExitSoftPowerDown();
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

// Small digital PIR modules such as AM312/SR602 can assert their output
// briefly while settling after power is applied. On a normal/cold boot this
// must not be treated as real motion. Three seconds gives a conservative margin.
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
// While awake, a stable HIGH->LOW edge can only switch WiFi/WebConfig ON.
// If WiFi/WebConfig is already running, further magnet pulses are ignored.
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


// =============================================================
// LOW-POWER / BOOT-LOOP PROTECTION
// =============================================================
//
// Goal:
// Prevent a depleted/unstable supply from repeatedly booting SensorForge,
// starting high-load subsystems, collapsing, and immediately booting again.
//
// This deliberately does NOT interpret every hard power cut as a fault. The
// persistent marker is cleared after 60 seconds of stable runtime. Therefore
// a normal user power-off after a healthy session does not contribute to the
// failure streak. Only consecutive cold boots that never become stable count.
// Normal deep-sleep wakes are explicitly excluded.
//
// The state lives in ESP32 NVS, so no DS3231/AT24C32 is required. The feature
// can be disabled with bootloop_protection=0 in config.txt. When disabled, all
// stored protection state is cleared immediately after the config is loaded.
static const char BOOTLOOP_NVS_NAMESPACE[] = "sfboot";
static const uint8_t BOOTLOOP_FAILURE_THRESHOLD = 3;
static const uint32_t BOOTLOOP_STABLE_AFTER_MS = 60000UL;

static bool bootloopProtectionArmed = false;
static uint32_t bootloopProtectionArmedMs = 0;


static uint64_t bootloopProtectionBackoffUs(
    uint8_t level
)
{
    // level 0 -> 30 min, level 1 -> 2 h, level >=2 -> 6 h.
    if (level == 0) {
        return 30ULL * 60ULL * 1000000ULL;
    }

    if (level == 1) {
        return 2ULL * 60ULL * 60ULL * 1000000ULL;
    }

    return 6ULL * 60ULL * 60ULL * 1000000ULL;
}


static void bootloopProtectionClearPersistentState()
{
    Preferences prefs;

    if (!prefs.begin(
            BOOTLOOP_NVS_NAMESPACE,
            false
        )) {
        Serial.println(
            "BOOTLOOP protection: NVS unavailable while clearing state"
        );
        return;
    }

    bool hasState =
        prefs.getBool("pending", false) ||
        prefs.getUChar("failures", 0) != 0 ||
        prefs.getUChar("level", 0) != 0 ||
        prefs.getBool("guard_sleep", false);

    if (hasState) {
        prefs.clear();
    }

    prefs.end();
}


static void bootloopProtectionEnterSleep(
    uint64_t sleepUs,
    uint8_t failureCount,
    uint8_t backoffLevel
)
{
    uint64_t sleepSeconds =
        sleepUs /
        1000000ULL;

    Serial.println();
    Serial.println(
        "============================================================"
    );
    Serial.println(
        "LOW-POWER BOOT-LOOP PROTECTION ACTIVE"
    );
    Serial.printf(
        "Detected %u consecutive unstable cold boots.\n",
        (unsigned)failureCount
    );
    Serial.printf(
        "High-load startup is stopped. Sleeping for %llu seconds.\n",
        (unsigned long long)sleepSeconds
    );
    Serial.printf(
        "Backoff level: %u | config: bootloop_protection=1\n",
        (unsigned)backoffLevel
    );
    Serial.println(
        "After the timer wake SensorForge will make one new normal boot attempt."
    );
    Serial.println(
        "Set bootloop_protection=0 to disable this protection if required."
    );
    Serial.println(
        "============================================================"
    );

    // Config has already been loaded, but camera/WiFi/recording have not been
    // started. Release the SD bus before sleeping so the low-power lockout is
    // as quiet as possible.
    if (sdReady) {
        STORAGE.end();
        sdReady = false;
    }

#if defined(STORAGE_SPI)
    SPI.end();
#endif

    // Protection sleep must be timer-only. Presence/magnet wake sources would
    // otherwise turn a depleted unit back into a tight wake loop.
    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    esp_sleep_enable_timer_wakeup(
        sleepUs
    );

    Serial.flush();
    delay(30);
    esp_deep_sleep_start();

    // Defensive fallback; deep sleep should never return.
    ESP.restart();
}


static bool bootloopProtectionHandleBoot(
    esp_reset_reason_t resetReason
)
{
    bootloopProtectionArmed = false;
    bootloopProtectionArmedMs = 0;

    if (!cfg_bootloop_protection) {
        bootloopProtectionClearPersistentState();
        Serial.println(
            "BOOTLOOP protection: disabled by config"
        );
        return false;
    }

    Preferences prefs;

    if (!prefs.begin(
            BOOTLOOP_NVS_NAMESPACE,
            false
        )) {
        // Fail open: this protection is a resilience feature and must never
        // brick a unit merely because NVS is unavailable/corrupt.
        Serial.println(
            "BOOTLOOP protection: NVS unavailable - protection skipped"
        );
        return false;
    }

    bool pending =
        prefs.getBool(
            "pending",
            false
        );

    uint8_t failures =
        prefs.getUChar(
            "failures",
            0
        );

    uint8_t backoffLevel =
        prefs.getUChar(
            "level",
            0
        );

    bool protectionSleep =
        prefs.getBool(
            "guard_sleep",
            false
        );

    // A timer wake from our own protection sleep is a deliberate retry, not a
    // failed boot. Preserve the backoff level so repeated low-battery episodes
    // escalate 30 min -> 2 h -> 6 h.
    if (protectionSleep) {
        pending = false;
        failures = 0;
        prefs.putBool(
            "guard_sleep",
            false
        );

        Serial.printf(
            "BOOTLOOP protection: retry after protection sleep | level=%u\n",
            (unsigned)backoffLevel
        );
    }

    // Any ordinary deep-sleep wake proves that the preceding SensorForge boot
    // progressed far enough to enter an intentional sleep path. Do not let
    // normal deep-sleep operation look like a battery boot loop.
    if (
        resetReason == ESP_RST_DEEPSLEEP &&
        !protectionSleep
    ) {
        if (
            pending ||
            failures != 0 ||
            backoffLevel != 0
        ) {
            prefs.clear();
        }

        prefs.end();

        Serial.println(
            "BOOTLOOP protection: normal deep-sleep wake - not counted"
        );
        return false;
    }

    const bool powerRelatedColdReset =
        resetReason == ESP_RST_POWERON ||
        resetReason == ESP_RST_BROWNOUT;

    if (powerRelatedColdReset) {
        if (pending) {
            if (failures < 255) {
                ++failures;
            }

            Serial.printf(
                "BOOTLOOP protection: previous cold boot did not become stable | count=%u/%u | reset=%s\n",
                (unsigned)failures,
                (unsigned)BOOTLOOP_FAILURE_THRESHOLD,
                resetReasonName(resetReason)
            );
        } else {
            failures = 0;
        }
    } else if (!protectionSleep) {
        // PANIC/WDT/SW/EXT are software/service/reset categories, not evidence
        // of a depleted battery. Break a possible power-failure streak so a
        // firmware problem cannot be hidden behind the low-power lockout.
        failures = 0;
        backoffLevel = 0;
    }

    if (
        powerRelatedColdReset &&
        failures >= BOOTLOOP_FAILURE_THRESHOLD
    ) {
        uint8_t sleepLevel =
            backoffLevel;

        uint64_t sleepUs =
            bootloopProtectionBackoffUs(
                sleepLevel
            );

        uint8_t nextLevel =
            sleepLevel < 2
            ? (uint8_t)(sleepLevel + 1)
            : (uint8_t)2;

        // Reset the short-boot counter before sleeping. If power is physically
        // removed during the protection sleep, the next real power-on gets one
        // clean recovery attempt instead of being permanently locked out.
        prefs.putBool(
            "pending",
            false
        );
        prefs.putUChar(
            "failures",
            0
        );
        prefs.putUChar(
            "level",
            nextLevel
        );
        prefs.putBool(
            "guard_sleep",
            true
        );
        prefs.end();

        bootloopProtectionEnterSleep(
            sleepUs,
            failures,
            sleepLevel
        );

        return true;
    }

    // Arm the current COLD/recovery boot. A later POWERON/BROWNOUT before the
    // stable timer expires will see this marker and count one incomplete boot.
    prefs.putBool(
        "pending",
        true
    );
    prefs.putUChar(
        "failures",
        failures
    );
    prefs.putUChar(
        "level",
        backoffLevel
    );
    prefs.putBool(
        "guard_sleep",
        false
    );
    prefs.end();

    bootloopProtectionArmed = true;
    bootloopProtectionArmedMs = millis();

    Serial.printf(
        "BOOTLOOP protection: armed | stable_after=%lu s | incomplete_count=%u/%u\n",
        (unsigned long)(BOOTLOOP_STABLE_AFTER_MS / 1000UL),
        (unsigned)failures,
        (unsigned)BOOTLOOP_FAILURE_THRESHOLD
    );

    return false;
}


static void bootloopProtectionLoop()
{
    if (
        !bootloopProtectionArmed ||
        !cfg_bootloop_protection
    ) {
        return;
    }

    if (
        (uint32_t)(
            millis() -
            bootloopProtectionArmedMs
        ) <
        BOOTLOOP_STABLE_AFTER_MS
    ) {
        return;
    }

    bootloopProtectionClearPersistentState();
    bootloopProtectionArmed = false;
    bootloopProtectionArmedMs = 0;

    Serial.println(
        "BOOTLOOP protection: boot stable for 60 s - failure streak cleared"
    );
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
);


// Emit the post-light-sleep wake diagnostic as one fully formatted Serial
// buffer. Native USB/CDC can be fragile around light sleep; avoiding a printf
// made of multiple formatting/write steps reduces the chance of a visibly
// truncated prefix. This is diagnostic-only and runs after the critical first
// frame path (or after that path is abandoned).
static void emitDeferredWakeConsole(
    const char *outcome
)
{
    if (!deferredWakeConsolePending)
        return;

    char timestamp[40];
    formatConsoleTimestamp(
        timestamp,
        sizeof(timestamp)
    );

    char consoleState[72];
    if (outcome && outcome[0]) {
        snprintf(
            consoleState,
            sizeof(consoleState),
            "deferred-%s",
            outcome
        );
    } else {
        snprintf(
            consoleState,
            sizeof(consoleState),
            "deferred"
        );
    }

    char message[224];
    snprintf(
        message,
        sizeof(message),
        "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s | console=%s",
        sleepWakeCauseName(deferredWakeCause),
        (int)deferredWakeCause,
        (unsigned long long)(deferredWakeSleptUs / 1000ULL),
        (unsigned long long)deferredWakeCameraWakeUs,
        deferredWakeFastCameraSleep
            ? (deferredWakeCameraWakeOk ? "" : " FAILED")
            : " (normal-init fallback)",
        consoleState
    );

    char line[320];
    const int lineLength = snprintf(
        line,
        sizeof(line),
        "[%s] [POWER] %s\n",
        timestamp,
        message
    );

    if (lineLength > 0) {
        size_t bytesToWrite = (size_t)lineLength;
        if (bytesToWrite >= sizeof(line))
            bytesToWrite = sizeof(line) - 1;

        Serial.write(
            (const uint8_t *)line,
            bytesToWrite
        );
    }

    deferredWakeConsolePending = false;
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
    // Hybrid LD2410S motion semantics:
    // - the fast UART gate-energy detector may assert first and therefore keeps
    //   the low-latency recording start path unchanged;
    // - OT2 HIGH is the LD2410S' confirmed presence output and keeps an already
    //   running recording alive for the sensor's own trigger/hold/absence logic.
    //
    // The two sources are deliberately ORed. A fast UART trigger that the
    // LD2410S never confirms through OT2 therefore receives only the existing
    // short ESP-side radar hold plus cfg_post_ms. Once OT2 becomes HIGH, its
    // level is authoritative until the sensor releases it again.
    if (radarMotionTrackingAvailable()) {
        return
            radarMotionActive() ||
            motionDiagnosticsPresenceActive();
    }

    // Standalone PIR, or detected radar with stale/unavailable UART tracking:
    // the physical presence input remains authoritative.
    return motionDiagnosticsPresenceActive();
}


static const uint32_t AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS =
    30000UL;

// Capture the accepted awake PIR/OT2 trigger immediately before entering
// startRecording(). This keeps presence_age_ms meaningful even if camera or
// storage preparation takes a few hundred milliseconds, and it does not rely
// on the one-shot diagnostics latch still being pending afterwards.
static bool recordingStartPresenceDecisionValid = false;
static uint32_t recordingStartPresenceDecisionMs = 0;
static bool recordingStartPresenceDecisionRtcBackend = false;

// Cosmetic/logging counter only: one count per successfully opened recording
// whose start source was the physical PIR/OT2 presence input. Unlike the raw
// diagnostics counter, this does not increase for PIR retriggers while the
// same recording is already running.
static uint32_t presenceRecordingEventCount = 0;

static void clearRecordingStartPresenceDiagnostics()
{
    recordingStartPresenceDecisionValid = false;
    recordingStartPresenceDecisionMs = 0;
    recordingStartPresenceDecisionRtcBackend = false;
}

static void captureRecordingStartPresenceDiagnostics()
{
    MotionDiagnosticsSnapshot snapshot;
    motionDiagnosticsGetSnapshot(snapshot);

    const uint32_t now = millis();

    recordingStartPresenceDecisionValid = true;
    recordingStartPresenceDecisionRtcBackend =
        motionDiagnosticsPresenceRtcSamplingActive();

    if (
        snapshot.presenceLastTriggerValid &&
        snapshot.presenceLastTriggerAgeMs <=
            AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS
    ) {
        recordingStartPresenceDecisionMs =
            now - snapshot.presenceLastTriggerAgeMs;
    } else {
        // Raw HIGH is operationally authoritative. If diagnostics did not have
        // a latched edge (for example after a backend ownership transition),
        // use the actual recording decision time rather than omitting latency
        // information from the REC START line.
        recordingStartPresenceDecisionMs = now;
    }
}


static bool physicalRecordingStartTriggerActive()
{
    // Raw GPIO HIGH is the primary, authoritative start signal. The RAM latch
    // below is only a backup for a pulse observed between recording decisions.
    const bool presenceLevel =
        motionDiagnosticsPresenceActive();

    const bool pendingPresenceEdge =
        motionDiagnosticsPresenceStartPending(
            AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS
        );

    // A standalone PIR is itself the authoritative motion source. Use the
    // debounced operational level plus the one-shot RAM trigger retained by
    // motion_diagnostics. The raw GPIO level is diagnostic-only.
    if (!radarSensorDetected()) {
        return
            presenceLevel ||
            pendingPresenceEdge;
    }

    // For a detected LD2410S, whichever trustworthy signal arrives first may
    // START the recording:
    //   1) fast UART gate threshold -> lowest awake latency, or
    //   2) OT2 HIGH / its latched rising edge -> sensor-confirmed presence.
    //
    // Including the current OT2 level (not only its edge) also makes a HIGH that
    // was already present before this loop iteration authoritative. This does not
    // delay the fast path; radarMotionActive() is still evaluated immediately.
    if (radarMotionTrackingAvailable()) {
        return
            radarMotionActive() ||
            presenceLevel ||
            pendingPresenceEdge;
    }

    // Detected radar but no fresh UART tracking: fall back to OT2 exactly as
    // before, with the same pending edge event for robustness.
    return
        presenceLevel ||
        pendingPresenceEdge;
}


static bool recordingAutomationAllowed()
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

    return true;
}


bool motionDetected()
{
    if (!recordingAutomationAllowed())
        return false;

    const bool simulated =
        simulatedMotionActive();

    // The persistent motion switch controls automatic physical/image triggers.
    // Manual WebConfig simulation remains available as a deterministic recorder
    // diagnostic even when automatic motion recording is disabled.
    if (!cfg_motion_recording_enabled)
        return simulated;

    return
        (recording
            ? physicalMotionActive()
            : physicalRecordingStartTriggerActive()) ||
        simulated;
}


static bool imageVerificationRequired()
{
    return cfg_motion_recording_decision == "image_verify";
}


static bool imageOnlyModeActive()
{
    return cfg_motion_recording_decision == "image_only";
}


static void finishDeferredWakeWithoutRecording(
    const char *outcome
)
{
    if (!wakeCriticalPathActive)
        return;

    wakeCriticalPathActive = false;
    wakeTimingActive = false;

    if (deferredWakeConsolePending) {
        emitDeferredWakeConsole(
            outcome ? outcome : "no-recording"
        );
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


static bool runImageMotionVerification(
    const char *context,
    String *diagnosticsJson = nullptr,
    String *errorOut = nullptr,
    bool failOpenOnAnalyzerError = true
)
{
    if (errorOut)
        *errorOut = "";

    if (!initCamera(
            cfg_camera,
            cfg_resolution,
            cfg_quality
        )) {
        if (errorOut)
            *errorOut = "camera initialization failed";

        // image_verify deliberately fails open so an analyzer fault cannot
        // silently suppress a real sensor event. image_only fails closed because
        // the image itself is the authoritative recording decision.
        return failOpenOnAnalyzerError;
    }

    if (cameraSoftPowerDownActive) {
        if (!cameraExitSoftPowerDown()) {
            if (errorOut)
                *errorOut = "camera wake failed";
            return failOpenOnAnalyzerError;
        }
    }

    imageMotionBeginVerification();

    uint8_t framesToAnalyze = (uint8_t)constrain(
        cfg_image_motion_confirm_frames + 2,
        3,
        7
    );

    bool analyzerFailure = false;
    String analyzerError;

    for (uint8_t frameIndex = 0; frameIndex < framesToAnalyze; ++frameIndex) {
        feedWatchdog();

        camera_fb_t *frame = esp_camera_fb_get();

        if (!frame) {
            analyzerFailure = true;
            analyzerError = "camera frame unavailable";
            break;
        }

        ImageMotionDiagnostics diagnostics;
        bool analyzed = false;

        if (
            frame->format == PIXFORMAT_JPEG &&
            frame->buf &&
            frame->len > 0
        ) {
            analyzed = imageMotionAnalyzeJpeg(
                frame->buf,
                frame->len,
                frame->width,
                frame->height,
                diagnostics
            );
        }

        esp_camera_fb_return(frame);

        if (!analyzed) {
            analyzerFailure = true;
            analyzerError = "JPEG analysis failed";
            break;
        }

        if (diagnostics.motionActive) {
            if (diagnosticsJson)
                *diagnosticsJson = imageMotionDiagnosticsJson();

            if (!wakeCriticalPathActive) {
                consolePrintf(
                    "IMAGE_MOTION",
                    "accepted | context=%s | frame_ms=%lu | decode_ms=%lu | cluster=%u/%u | area=%.1f%% | mean=%.1f | delta=%.1f | score=%.2f",
                    context ? context : "trigger",
                    (unsigned long)diagnostics.analyzeFrameMs,
                    (unsigned long)diagnostics.decodeMs,
                    (unsigned)diagnostics.largestClusterBlocks,
                    (unsigned)diagnostics.activeRoiBlocks,
                    diagnostics.changedAreaPct,
                    diagnostics.globalMean,
                    diagnostics.globalMeanDelta,
                    diagnostics.motionScore
                );
            }

            return true;
        }
    }

    if (diagnosticsJson)
        *diagnosticsJson = imageMotionDiagnosticsJson();

    if (analyzerFailure) {
        if (errorOut)
            *errorOut = analyzerError;

        if (!wakeCriticalPathActive) {
            consoleWrite(
                "IMAGE_MOTION",
                "analyzer error | " + analyzerError +
                (failOpenOnAnalyzerError ? " | fail-open" : " | fail-closed")
            );
        }

        return failOpenOnAnalyzerError;
    }

    const ImageMotionDiagnostics &diagnostics = imageMotionLastDiagnostics();

    String rejectMessage =
        "rejected | context=" +
        String(context ? context : "trigger") +
        " | state=" +
        imageMotionStateName(diagnostics.state) +
        " | reason=" +
        imageMotionRejectReasonName(diagnostics.rejectReason) +
        " | frame_ms=" +
        String(diagnostics.analyzeFrameMs) +
        " | cluster=" +
        String(diagnostics.largestClusterBlocks) +
        "/" +
        String(diagnostics.activeRoiBlocks) +
        " | area=" +
        String(diagnostics.changedAreaPct, 1) +
        "% | global_change=" +
        String(diagnostics.globalChangePct, 1) +
        "%";

    // Do not reintroduce blocking Serial/SD logging into the optimized
    // post-light-sleep decision path. Detailed reject diagnostics are available
    // in WebConfig; optional debug output is deferred until the critical path is
    // released by finishDeferredWakeWithoutRecording().
    if (wakeCriticalPathActive) {
        if (cfg_debug_enabled) {
            deferredMotionConsolePending = true;
            deferredMotionConsoleMessage =
                "IMAGE_MOTION | " + rejectMessage;
        }
    } else if (cfg_debug_enabled) {
        consoleWrite(
            "IMAGE_MOTION",
            rejectMessage
        );

        if (sdReady) {
            logWrite(
                "IMAGE_MOTION | " +
                rejectMessage
            );
        }
    }

    return false;
}


static bool recordingDecisionAllowsStart(
    bool physicalTrigger,
    const char *context
)
{
    // Simulation remains a deterministic test of the recorder itself. For real
    // sensor triggers, motion_recording_decision is the sole source of truth.
    if (!physicalTrigger)
        return true;

    if (cfg_motion_recording_decision == "direct")
        return true;

    if (cfg_motion_recording_decision == "image_only") {
        return runImageMotionVerification(
            context,
            nullptr,
            nullptr,
            false
        );
    }

    if (!imageVerificationRequired())
        return true;

    if (imageVerifyRejectedUntilMotionClear)
        return false;

    bool accepted = runImageMotionVerification(context);

    if (!accepted)
        imageVerifyRejectedUntilMotionClear = true;

    return accepted;
}


static bool imageOnlyIdleMotionTrigger()
{
    if (
        recording ||
        wakeCriticalPathActive ||
        !cfg_motion_recording_enabled ||
        !imageOnlyModeActive() ||
        !recordingAutomationAllowed()
    ) {
        return false;
    }

    uint32_t now = millis();
    if (
        imageOnlyLastIdleScanMs != 0 &&
        (uint32_t)(now - imageOnlyLastIdleScanMs) <
            IMAGE_ONLY_IDLE_SCAN_INTERVAL_MS
    ) {
        return false;
    }

    imageOnlyLastIdleScanMs = now;

    if (!initCamera(
            cfg_camera,
            cfg_resolution,
            cfg_quality
        )) {
        return false;
    }

    if (cameraSoftPowerDownActive) {
        if (!cameraExitSoftPowerDown())
            return false;
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame)
        return false;

    ImageMotionDiagnostics diagnostics;
    bool analyzed = false;

    if (
        frame->format == PIXFORMAT_JPEG &&
        frame->buf &&
        frame->len > 0
    ) {
        analyzed = imageMotionAnalyzeJpeg(
            frame->buf,
            frame->len,
            frame->width,
            frame->height,
            diagnostics
        );
    }

    esp_camera_fb_return(frame);

    return
        analyzed &&
        diagnostics.motionActive;
}


static bool recordingContinuationMotionActive()
{
    // In image_only, radar/PIR is not allowed to keep a recording alive. The
    // recorder feeds its already-written JPEGs back into image_motion, so the
    // configured release_frames determine when image motion ends. The normal
    // cfg_post_ms window then runs exactly as in every other mode.
    if (imageOnlyModeActive()) {
        return
            imageMotionMotionActive() ||
            simulatedMotionActive();
    }

    return motionDetected();
}


bool imageMotionWebTest(
    String &json,
    String &error
)
{
    json = "";
    error = "";

    if (recording || recorderIsOpen()) {
        error = "recording active";
        return false;
    }

    // Test mode never starts a recording. WebConfig's preview gate remains the
    // owner of the camera while the browser is on the Image Motion page.
    bool result = runImageMotionVerification(
        "web_test",
        &json,
        &error
    );

    if (error.length())
        return false;

    // A negative motion decision is still a successful diagnostic request.
    (void)result;
    return true;
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
// STORAGE-FAULT LOW-POWER STATE
// =============================================================

static void storageFaultClearRtcState()
{
    storageFaultRtcMagic = 0;
    storageFaultRtcCycle = 0;
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

    storageFaultClearRtcState();

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
// MANUAL READ-ONLY SD RECOVERY / DIAGNOSTIC (XIAO SPI)
// =============================================================
//
// This path is intentionally operator-triggered from WebConfig. It is meant
// for an SD card that failed the normal boot/runtime mount attempts.
//
// Safety properties:
// - no format
// - no wipe
// - no raw-sector writes
// - SD.begin(..., format_if_mount_failed=false)
// - no automatic reboot
//
// If a filesystem mount succeeds, SensorForge adopts the recovered mount and
// returns storage to normal operation. Normal operation may then write logs or
// recordings as usual; the recovery probes themselves do not deliberately
// modify card contents.

#if defined(STORAGE_SPI)

static void sdManualRecoveryReportf(
    String &report,
    const char *format,
    ...
)
{
    char buffer[256];

    va_list args;
    va_start(args, format);

    vsnprintf(
        buffer,
        sizeof(buffer),
        format,
        args
    );

    va_end(args);

    report += buffer;
}


static void sdManualRecoveryDeselect()
{
    digitalWrite(
        SD_CS_PIN,
        HIGH
    );

    SPI.transfer(0xFF);
}


static void sdManualRecoverySelect()
{
    digitalWrite(
        SD_CS_PIN,
        LOW
    );
}


static bool sdManualRecoveryWaitReady(
    uint32_t timeoutMs
)
{
    uint32_t start =
        millis();

    do {
        if (SPI.transfer(0xFF) == 0xFF)
            return true;

        delayMicroseconds(50);
        feedWatchdog();

    } while (
        (uint32_t)(
            millis() -
            start
        ) <
        timeoutMs
    );

    return false;
}


static uint8_t sdManualRecoveryCommandSelected(
    uint8_t command,
    uint32_t argument,
    uint8_t crc
)
{
    if (!sdManualRecoveryWaitReady(300UL))
        return 0xFF;

    SPI.transfer(
        0x40U |
        command
    );

    SPI.transfer(
        (uint8_t)(
            argument >>
            24
        )
    );

    SPI.transfer(
        (uint8_t)(
            argument >>
            16
        )
    );

    SPI.transfer(
        (uint8_t)(
            argument >>
            8
        )
    );

    SPI.transfer(
        (uint8_t)argument
    );

    SPI.transfer(crc);

    for (
        uint8_t i = 0;
        i < 16;
        ++i
    ) {
        uint8_t response =
            SPI.transfer(0xFF);

        if (
            (response & 0x80U) ==
            0
        ) {
            return response;
        }
    }

    return 0xFF;
}


static uint8_t sdManualRecoveryCommand(
    uint8_t command,
    uint32_t argument,
    uint8_t crc
)
{
    sdManualRecoveryDeselect();
    sdManualRecoverySelect();

    uint8_t response =
        sdManualRecoveryCommandSelected(
            command,
            argument,
            crc
        );

    sdManualRecoveryDeselect();

    return response;
}


static bool sdManualRecoveryRawInit(
    String &report,
    bool &blockAddressing
)
{
    report +=
        "=== RAW SPI RESET / INIT @ 400 kHz ===\n";

    blockAddressing =
        false;

    resetStorageInterface();

    delay(100);

    pinMode(
        SD_CS_PIN,
        OUTPUT
    );

    digitalWrite(
        SD_CS_PIN,
        HIGH
    );

    SPI.begin(
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    SPI.beginTransaction(
        SPISettings(
            400000UL,
            MSBFIRST,
            SPI_MODE0
        )
    );

    // SD SPI power-up requires >=74 clocks while CS is HIGH.
    digitalWrite(
        SD_CS_PIN,
        HIGH
    );

    for (
        uint8_t i = 0;
        i < 20;
        ++i
    ) {
        SPI.transfer(0xFF);
    }

    uint8_t cmd0 =
        0xFF;

    for (
        uint8_t attempt = 1;
        attempt <= 5;
        ++attempt
    ) {
        cmd0 =
            sdManualRecoveryCommand(
                0,
                0,
                0x95
            );

        sdManualRecoveryReportf(
            report,
            "CMD0 attempt %u -> R1=0x%02X\n",
            (unsigned)attempt,
            (unsigned)cmd0
        );

        if (cmd0 == 0x01)
            break;

        delay(50);
        feedWatchdog();
    }

    if (cmd0 != 0x01) {
        report +=
            "RAW: card did not enter SPI idle state.\n";

        SPI.endTransaction();
        SPI.end();

        return false;
    }


    sdManualRecoveryDeselect();
    sdManualRecoverySelect();

    uint8_t cmd8 =
        sdManualRecoveryCommandSelected(
            8,
            0x000001AAUL,
            0x87
        );

    uint8_t r7[4] = {
        0xFF,
        0xFF,
        0xFF,
        0xFF
    };

    if (cmd8 != 0xFF) {
        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {
            r7[i] =
                SPI.transfer(0xFF);
        }
    }

    sdManualRecoveryDeselect();

    sdManualRecoveryReportf(
        report,
        "CMD8 -> R1=0x%02X R7=%02X %02X %02X %02X\n",
        (unsigned)cmd8,
        (unsigned)r7[0],
        (unsigned)r7[1],
        (unsigned)r7[2],
        (unsigned)r7[3]
    );

    bool sdV2 =
        cmd8 == 0x01 &&
        r7[2] == 0x01 &&
        r7[3] == 0xAA;

    bool legacy =
        (cmd8 & 0x04U) !=
        0;

    if (
        !sdV2 &&
        !legacy
    ) {
        report +=
            "RAW warning: unexpected CMD8 response.\n";
    }


    uint32_t acmdArgument =
        sdV2
        ? 0x40000000UL
        : 0UL;

    uint8_t acmd41 =
        0xFF;

    uint32_t initStart =
        millis();

    while (
        (uint32_t)(
            millis() -
            initStart
        ) <
        3000UL
    ) {
        uint8_t cmd55 =
            sdManualRecoveryCommand(
                55,
                0,
                0x01
            );

        if (cmd55 != 0xFF) {
            acmd41 =
                sdManualRecoveryCommand(
                    41,
                    acmdArgument,
                    0x01
                );

            if (acmd41 == 0x00)
                break;
        }

        delay(20);
        feedWatchdog();
    }

    sdManualRecoveryReportf(
        report,
        "ACMD41 final -> R1=0x%02X\n",
        (unsigned)acmd41
    );

    if (acmd41 != 0x00) {
        report +=
            "RAW: card stayed in idle / initialization failed.\n";

        SPI.endTransaction();
        SPI.end();

        return false;
    }


    sdManualRecoveryDeselect();
    sdManualRecoverySelect();

    uint8_t cmd58 =
        sdManualRecoveryCommandSelected(
            58,
            0,
            0x01
        );

    uint8_t ocr[4] = {
        0,
        0,
        0,
        0
    };

    if (cmd58 != 0xFF) {
        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {
            ocr[i] =
                SPI.transfer(0xFF);
        }
    }

    sdManualRecoveryDeselect();

    sdManualRecoveryReportf(
        report,
        "CMD58 -> R1=0x%02X OCR=%02X %02X %02X %02X\n",
        (unsigned)cmd58,
        (unsigned)ocr[0],
        (unsigned)ocr[1],
        (unsigned)ocr[2],
        (unsigned)ocr[3]
    );

    if (cmd58 != 0x00) {
        report +=
            "RAW: card initialized, but OCR read failed.\n";

        SPI.endTransaction();
        SPI.end();

        return false;
    }


    blockAddressing =
        (ocr[0] & 0x40U) !=
        0;

    sdManualRecoveryReportf(
        report,
        "RAW addressing: %s\n",
        blockAddressing
        ? "SDHC/SDXC block"
        : "SDSC byte"
    );


    if (!blockAddressing) {
        uint8_t cmd16 =
            sdManualRecoveryCommand(
                16,
                512,
                0x01
            );

        sdManualRecoveryReportf(
            report,
            "CMD16(512) -> R1=0x%02X\n",
            (unsigned)cmd16
        );
    }


    report +=
        "RAW: card controller reached READY.\n";

    SPI.endTransaction();

    return true;
}


static bool sdManualRecoveryReadSector0(
    bool blockAddressing,
    uint8_t *sector,
    String &report
)
{
    if (!sector)
        return false;


    SPI.beginTransaction(
        SPISettings(
            400000UL,
            MSBFIRST,
            SPI_MODE0
        )
    );

    sdManualRecoveryDeselect();
    sdManualRecoverySelect();

    // Sector zero is address zero for both byte and block addressed cards.
    (void)blockAddressing;

    uint8_t cmd17 =
        sdManualRecoveryCommandSelected(
            17,
            0,
            0x01
        );

    if (cmd17 != 0x00) {
        sdManualRecoveryReportf(
            report,
            "CMD17 sector0 -> R1=0x%02X\n",
            (unsigned)cmd17
        );

        sdManualRecoveryDeselect();
        SPI.endTransaction();

        return false;
    }


    uint8_t token =
        0xFF;

    uint32_t tokenStart =
        millis();

    while (
        (uint32_t)(
            millis() -
            tokenStart
        ) <
        1000UL
    ) {
        token =
            SPI.transfer(0xFF);

        if (
            token == 0xFE ||
            token != 0xFF
        ) {
            break;
        }

        feedWatchdog();
    }


    if (token != 0xFE) {
        sdManualRecoveryReportf(
            report,
            "CMD17 sector0 -> data token=0x%02X\n",
            (unsigned)token
        );

        sdManualRecoveryDeselect();
        SPI.endTransaction();

        return false;
    }


    for (
        size_t i = 0;
        i < 512U;
        ++i
    ) {
        sector[i] =
            SPI.transfer(0xFF);
    }

    // Discard data CRC.
    SPI.transfer(0xFF);
    SPI.transfer(0xFF);

    sdManualRecoveryDeselect();
    SPI.endTransaction();

    bool signature =
        sector[510] == 0x55 &&
        sector[511] == 0xAA;

    bool directFat32 =
        memcmp(
            &sector[82],
            "FAT32   ",
            8
        ) == 0;

    bool directFat16 =
        memcmp(
            &sector[54],
            "FAT16   ",
            8
        ) == 0;

    bool directFat12 =
        memcmp(
            &sector[54],
            "FAT12   ",
            8
        ) == 0;

    bool directExfat =
        memcmp(
            &sector[3],
            "EXFAT   ",
            8
        ) == 0;

    sdManualRecoveryReportf(
        report,
        "Sector 0 read: OK | signature_55AA=%s | FAT32=%s | FAT16=%s | FAT12=%s | exFAT=%s\n",
        signature ? "yes" : "no",
        directFat32 ? "yes" : "no",
        directFat16 ? "yes" : "no",
        directFat12 ? "yes" : "no",
        directExfat ? "yes" : "no"
    );


    if (
        signature &&
        !directFat32 &&
        !directFat16 &&
        !directFat12 &&
        !directExfat
    ) {
        for (
            uint8_t partition = 0;
            partition < 4;
            ++partition
        ) {
            const uint8_t *entry =
                &sector[
                    446U +
                    (uint16_t)partition *
                    16U
                ];

            uint8_t type =
                entry[4];

            uint32_t startLba =
                (uint32_t)entry[8] |
                ((uint32_t)entry[9] << 8) |
                ((uint32_t)entry[10] << 16) |
                ((uint32_t)entry[11] << 24);

            uint32_t sectorCount =
                (uint32_t)entry[12] |
                ((uint32_t)entry[13] << 8) |
                ((uint32_t)entry[14] << 16) |
                ((uint32_t)entry[15] << 24);

            if (
                type == 0 &&
                startLba == 0 &&
                sectorCount == 0
            ) {
                continue;
            }

            sdManualRecoveryReportf(
                report,
                "Partition %u: type=0x%02X start_lba=%lu sectors=%lu\n",
                (unsigned)(
                    partition +
                    1
                ),
                (unsigned)type,
                (unsigned long)startLba,
                (unsigned long)sectorCount
            );
        }
    }


    return true;
}


static bool sdManualRecoveryTryMountAtHz(
    uint32_t frequencyHz,
    String &report
)
{
    resetStorageInterface();

    delay(100);

    pinMode(
        SD_CS_PIN,
        OUTPUT
    );

    digitalWrite(
        SD_CS_PIN,
        HIGH
    );

    SPI.begin(
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    sdSpiSendIdleClocks();

    bool mounted =
        SD.begin(
            SD_CS_PIN,
            SPI,
            frequencyHz,
            "/sd",
            5,
            false
        );

    if (!mounted) {
        sdManualRecoveryReportf(
            report,
            "Mount @ %.3f MHz -> FAILED\n",
            (double)frequencyHz /
                1000000.0
        );

        resetStorageInterface();

        return false;
    }


    uint8_t cardType =
        SD.cardType();

    File root =
        SD.open(
            "/",
            FILE_READ
        );

    bool rootOk =
        root &&
        root.isDirectory();

    if (root)
        root.close();


    if (
        cardType == CARD_NONE ||
        !rootOk
    ) {
        sdManualRecoveryReportf(
            report,
            "Mount @ %.3f MHz -> card/filesystem unavailable\n",
            (double)frequencyHz /
                1000000.0
        );

        resetStorageInterface();

        return false;
    }


    uint64_t cardSizeMb =
        SD.cardSize() /
        (1024ULL * 1024ULL);

    sdManualRecoveryReportf(
        report,
        "Mount @ %.3f MHz -> OK | card=%s | size=%llu MB | root=OK\n",
        (double)frequencyHz /
            1000000.0,
        cardType == CARD_MMC
            ? "MMC"
            : (
                cardType == CARD_SD
                ? "SDSC"
                : (
                    cardType == CARD_SDHC
                    ? "SDHC/SDXC"
                    : "UNKNOWN"
                )
            ),
        (unsigned long long)cardSizeMb
    );

    return true;
}


bool sdManualReadOnlyRecovery(
    String &report,
    uint32_t &mountedFrequencyHz,
    bool &rawCardReady,
    bool &sector0Readable
)
{
    report = "";
    report.reserve(4096);

    mountedFrequencyHz =
        0;

    rawCardReady =
        false;

    sector0Readable =
        false;


    report +=
        "SensorForge manual SD recovery\n"
        "Mode: non-destructive / no format / no wipe / no raw writes\n\n";


    if (recording) {
        report +=
            "ABORTED: recording is active.\n";

        return false;
    }


    if (g_storageLocked) {
        report +=
            "ABORTED: storage is currently locked by another operation.\n";

        return false;
    }


    if (sdReady) {
        report +=
            "ABORTED: SD is already mounted and marked ready.\n";

        return true;
    }


    bool previousRecordingBlock =
        g_recordingStartBlocked;

    bool previousStorageLock =
        g_storageLocked;


    g_recordingStartBlocked =
        true;

    g_storageLocked =
        true;


    // The boot path has no active SD logger when sdReady=false, but close it
    // defensively before taking exclusive ownership of the SPI storage bus.
    logClose();

    resetStorageInterface();

    sdReady =
        false;


    bool blockAddressing =
        false;

    rawCardReady =
        sdManualRecoveryRawInit(
            report,
            blockAddressing
        );


    if (rawCardReady) {
        uint8_t sector0[512];

        memset(
            sector0,
            0,
            sizeof(sector0)
        );

        sector0Readable =
            sdManualRecoveryReadSector0(
                blockAddressing,
                sector0,
                report
            );

        if (!sector0Readable) {
            report +=
                "Sector 0 read: FAILED\n";
        }

        resetStorageInterface();
    }


    report +=
        "\n=== FILESYSTEM MOUNT SWEEP ===\n";

    const uint32_t frequencies[] = {
        400000UL,
        1000000UL,
        2000000UL,
        4000000UL,
        8000000UL,
        SD_SPI_NORMAL_FREQUENCY_HZ
    };

    uint32_t bestFrequency =
        0;

    uint32_t lastFrequency =
        0;


    for (
        size_t i = 0;
        i <
        sizeof(frequencies) /
        sizeof(frequencies[0]);
        ++i
    ) {
        uint32_t frequency =
            frequencies[i];

        if (frequency == lastFrequency)
            continue;

        lastFrequency =
            frequency;


        bool mounted =
            sdManualRecoveryTryMountAtHz(
                frequency,
                report
            );


        if (mounted) {
            bestFrequency =
                frequency;

            // Continue upwards to find the highest currently stable clock.
            resetStorageInterface();

            delay(100);
            feedWatchdog();

            continue;
        }


        // Once a lower clock has mounted successfully, stop at the first
        // higher failure and fall back to the best known working clock.
        if (bestFrequency != 0)
            break;
    }


    bool recovered =
        false;


    if (bestFrequency != 0) {
        report +=
            "\nRemounting best working clock...\n";

        recovered =
            sdManualRecoveryTryMountAtHz(
                bestFrequency,
                report
            );

        if (recovered) {
            mountedFrequencyHz =
                bestFrequency;

            sdReady =
                true;

            storageFaultClearRtcState();

            sdManualRecoveryReportf(
                report,
                "\nRECOVERY SUCCESS | mounted=%.3f MHz%s\n",
                (double)bestFrequency /
                    1000000.0,
                bestFrequency <
                    SD_SPI_NORMAL_FREQUENCY_HZ
                    ? " | DEGRADED CLOCK"
                    : ""
            );


            if (bootStartedWithoutSd) {
                String configError;

                bool validSdConfig =
                    recoveredSdHasValidConfig(
                        configError
                    );

                if (validSdConfig) {
                    report +=
                        "Valid SD /config.txt detected. Current RAM configuration is kept until a deliberate reboot.\n";
                } else {
                    report +=
                        "No valid SD /config.txt requiring priority restoration: " +
                        configError +
                        "\n";
                }
            }


            report +=
                "Incomplete-recording cleanup was NOT run by this recovery action.\n";
        }
    }


    if (!recovered) {
        resetStorageInterface();

        sdReady =
            false;

        report +=
            "\nRECOVERY FAILED: no readable filesystem mount was obtained.\n";

        if (rawCardReady) {
            report +=
                "The SD controller answered at raw SPI level; filesystem/partition damage remains plausible.\n";
        } else {
            report +=
                "The SD controller did not reach READY at raw SPI level; card/controller/contact/power failure is more likely.\n";
        }
    }


    g_storageLocked =
        previousStorageLock;

    g_recordingStartBlocked =
        previousRecordingBlock;


    if (recovered) {
        // Recovery probing itself is read-only. Once exclusive recovery ends,
        // restore the normal logger so the running SensorForge instance can
        // resume ordinary operation on the recovered mount.
        logInit();

        logWrite(
            "SD manual recovery successful | mounted_hz=" +
            String(mountedFrequencyHz)
        );
    }


    return recovered;
}

#endif // STORAGE_SPI


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


static void cameraEffectiveOrientation(
    bool &hmirror,
    bool &vflip
)
{
    // Board-specific native sensor correction comes first. The user-facing
    // rotation setting is relative to that corrected product orientation.
    hmirror =
        CAMERA_BASE_HMIRROR != 0;

    vflip =
        CAMERA_BASE_VFLIP != 0;

    // A true 180-degree rotation is equivalent to toggling both axes.
    // Config validation currently allows only 0 or 180 degrees.
    if (cfg_rotation == 180) {
        hmirror =
            !hmirror;

        vflip =
            !vflip;
    }
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


    // Crop position is defined in DISPLAY coordinates. Convert it back to raw
    // sensor coordinates using the same effective mirror/flip orientation that
    // is applied to Live Preview and recordings. This keeps e.g. "top-left"
    // visually top-left even when the board needs a native mirror correction.
    int rawPositionX =
        positionX;

    int rawPositionY =
        positionY;

    bool effectiveHMirror = false;
    bool effectiveVFlip = false;

    cameraEffectiveOrientation(
        effectiveHMirror,
        effectiveVFlip
    );

    if (effectiveHMirror) {
        rawPositionX =
            2 - rawPositionX;
    }

    if (effectiveVFlip) {
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


        bool effectiveHMirror = false;
        bool effectiveVFlip = false;

        cameraEffectiveOrientation(
            effectiveHMirror,
            effectiveVFlip
        );

        sensor->set_hmirror(
            sensor,
            effectiveHMirror ? 1 : 0
        );

        sensor->set_vflip(
            sensor,
            effectiveVFlip ? 1 : 0
        );

        if (cfg_debug_enabled) {
            Serial.printf(
                "Camera orientation: config_rotation=%d base_hmirror=%d base_vflip=%d effective_hmirror=%d effective_vflip=%d\n",
                cfg_rotation,
                CAMERA_BASE_HMIRROR,
                CAMERA_BASE_VFLIP,
                effectiveHMirror ? 1 : 0,
                effectiveVFlip ? 1 : 0
            );
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
            (float)cfg_transport_black_threshold &&
        metrics.p95 <=
            (uint8_t)configTransportBlackP95Limit();
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

    int p95Limit =
        configTransportBlackP95Limit();

    Serial.printf(
        "TRANSPORT image %s | mean=%.1f | p95=%u | black_threshold=%d | p95_limit_auto=%d | %s\n",
        label ? label : "check",
        metrics.mean,
        (unsigned)metrics.p95,
        cfg_transport_black_threshold,
        p95Limit,
        isBlack ? "BLACK" : "OPEN"
    );

    transportJournalPrintf(
        "TRANSPORT image %s | mean=%.1f | p95=%u | black_threshold=%d | p95_limit_auto=%d | %s",
        label ? label : "check",
        metrics.mean,
        (unsigned)metrics.p95,
        cfg_transport_black_threshold,
        p95Limit,
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


static void restartFromTransportIntoNormalBoot(
    const char *reason
)
{
    const char *restartReason =
        (reason && reason[0])
        ? reason
        : "transport-release";

    // transport_mode=0 has already been persisted before this helper is called.
    // Write the final durable journal entry while the transport wake-cycle
    // context still exists, then clear every RTC/persistent transport marker.
    // A software restart guarantees that normal operation begins from the same
    // clean setup() path as any ordinary SensorForge boot instead of continuing
    // inside the special minimal transport boot.
    Serial.printf(
        "TRANSPORT mode OFF - restarting into normal boot | reason=%s\n",
        restartReason
    );

    transportJournalWrite(
        String("TRANSPORT mode OFF | reason=") +
        restartReason +
        " | restart=normal_boot"
    );

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

    // Config persistence and the transport journal both close/flush their files
    // before returning. Flush the UART too so the restart reason remains visible
    // during service diagnostics.
    Serial.flush();
    delay(50);

    ESP.restart();

    // Defensive only: ESP.restart() does not return on a healthy platform.
    while (true) {
        delay(1000);
    }
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
        "TRANSPORT mode ACTIVE | session=%s | wake=%s(%d) | cycle=%lu | check=%d s | confirm=%d s | install_delay=%d s | max_duration=%d s | elapsed=%lu s | remaining=%lu s | black_threshold=%d | p95_limit_auto=%d",
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
        cfg_transport_black_threshold,
        configTransportBlackP95Limit()
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

        restartFromTransportIntoNormalBoot(
            "max-duration"
        );

        return true;
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

            restartFromTransportIntoNormalBoot(
                "installation-delay-elapsed"
            );

            return true;
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


    restartFromTransportIntoNormalBoot(
        "zero-install-delay"
    );

    return true;
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

static void recordingPerformanceReset(
    RecordingPerformanceStats &stats
)
{
    stats.frameCalls = 0;
    stats.nearBudgetFrames = 0;
    stats.overBudgetFrames = 0;
    stats.worstCallUs = 0;
    stats.totalCallUs = 0;
}


static void recordingPerformanceNoteCall(
    RecordingPerformanceStats &stats,
    uint32_t callUs,
    uint32_t frameBudgetUs
)
{
    stats.frameCalls++;
    stats.totalCallUs +=
        (uint64_t)callUs;

    if (callUs > stats.worstCallUs)
        stats.worstCallUs = callUs;

    // 80% is an early pressure indicator. It does not mean a frame was lost;
    // only calls above 100% have consumed more than one complete frame budget.
    uint32_t nearBudgetUs =
        (uint32_t)(
            ((uint64_t)frameBudgetUs * 80ULL) /
            100ULL
        );

    if (callUs >= nearBudgetUs)
        stats.nearBudgetFrames++;

    if (callUs > frameBudgetUs)
        stats.overBudgetFrames++;
}


static void recordingPerformanceLogSummary(
    const char *scope,
    const RecordingPerformanceStats &stats
)
{
    if (stats.frameCalls == 0)
        return;

    uint32_t frameBudgetUs =
        1000000UL /
        (uint32_t)max(cfg_fps, 1);

    uint32_t averageUs =
        (uint32_t)(
            stats.totalCallUs /
            (uint64_t)stats.frameCalls
        );

    String message =
        String(
            stats.overBudgetFrames > 0
                ? "RECORDING PERFORMANCE WARNING"
                : "RECORDING PERFORMANCE"
        ) +
        " | scope=" +
        String(scope ? scope : "unknown") +
        " | target_fps=" +
        String(cfg_fps) +
        " | budget_ms=" +
        String((float)frameBudgetUs / 1000.0f, 1) +
        " | calls=" +
        String((unsigned long)stats.frameCalls) +
        " | near80=" +
        String((unsigned long)stats.nearBudgetFrames) +
        " | over_budget=" +
        String((unsigned long)stats.overBudgetFrames) +
        " | avg_ms=" +
        String((float)averageUs / 1000.0f, 1) +
        " | worst_ms=" +
        String((float)stats.worstCallUs / 1000.0f, 1);

    logWrite(
        message
    );

    // Only surface an immediate console warning when the actual frame budget
    // was exceeded. Normal statistics stay in the log to keep the console quiet.
    if (stats.overBudgetFrames > 0) {
        consoleWrite(
            "REC",
            message
        );
    }
}


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

    const bool cameraInitializedAtEntry =
        cameraInitialized;

    const bool cameraStandbyAtEntry =
        cameraSoftPowerDownActive;

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

    {
        uint32_t performanceLoad = 0;
        uint32_t performanceLimit = 0;

        if (!configRecordingPerformanceAllowed(
                cfg_resolution,
                cfg_fps,
                cfg_quality,
                performanceLoad,
                performanceLimit
            )) {

            String message =
                "BLOCKED | recording performance limit | score=" +
                String((unsigned long)performanceLoad) +
                " | limit=" +
                String((unsigned long)performanceLimit);

            consoleWrite(
                "REC",
                message
            );

            logWrite(
                "Recording " +
                message
            );

            return false;
        }
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


    // Cosmetic event counter for REC START diagnostics. Count only a physical
    // PIR/OT2 start that actually opened the recorder successfully. Retriggers
    // while the same recording is running therefore do not inflate the value.
    const bool presenceEventForThisStart =
        recordingStartPresenceDecisionValid ||
        motionDiagnosticsPresenceStartPending(
            AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS
        );

    uint32_t presenceEventCountForThisStart = 0;
    if (presenceEventForThisStart) {
        if (presenceRecordingEventCount != UINT32_MAX)
            ++presenceRecordingEventCount;

        presenceEventCountForThisStart =
            presenceRecordingEventCount;
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

    // Add trigger/start diagnostics to the existing REC START line. Awake
    // PIR/OT2 starts use the decision snapshot captured immediately before
    // startRecording(), so diagnostics remain present even after a cold camera
    // init or another preparation delay. Boot/light-sleep paths retain the
    // existing one-shot fallback. No additional SD write is created.
    if (recordingStartPresenceDecisionValid) {
        startMessage +=
            " | presence_age_ms=" +
            String((uint32_t)(
                millis() - recordingStartPresenceDecisionMs
            )) +
            " | presence_count=" +
            String(presenceEventCountForThisStart) +
            " | presence_backend=" +
            String(
                recordingStartPresenceDecisionRtcBackend
                    ? "rtc"
                    : "digital"
            );
    } else if (motionDiagnosticsPresenceStartPending(
            AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS
        )) {

        MotionDiagnosticsSnapshot startDiagnostics;
        motionDiagnosticsGetSnapshot(startDiagnostics);

        if (startDiagnostics.presenceLastTriggerValid) {
            startMessage +=
                " | presence_age_ms=" +
                String(startDiagnostics.presenceLastTriggerAgeMs) +
                " | presence_count=" +
                String(presenceEventCountForThisStart) +
                " | presence_backend=" +
                String(
                    motionDiagnosticsPresenceRtcSamplingActive()
                        ? "rtc"
                        : "digital"
                );
        }
    }

    startMessage +=
        " | camera_entry=";

    if (!cameraInitializedAtEntry) {
        startMessage += "cold";
    } else if (cameraStandbyAtEntry) {
        startMessage += "standby";
    } else {
        startMessage += "warm";
    }

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

    recordingPerformanceReset(
        recordingPerformanceSegment
    );
    recordingPerformanceReset(
        recordingPerformanceEvent
    );

    // Start a fresh event-level thermal accumulator. The regular thermal
    // monitor will add samples in RAM while recording is active.
    recordingThermalReset();

    recording = true;

    // Consume the one-shot physical event only after the recorder has opened
    // successfully. A transient start failure may therefore retry.
    motionDiagnosticsConsumePresenceStartTrigger();
    clearRecordingStartPresenceDiagnostics();

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


    recordingPerformanceLogSummary(
        "segment",
        recordingPerformanceSegment
    );


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

    recordingPerformanceReset(
        recordingPerformanceSegment
    );

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

    recordingPerformanceLogSummary(
        "event",
        recordingPerformanceEvent
    );

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

    // Camera lifetime policy:
    // - sleep_mode=off means the unit is intentionally kept awake, so retain
    //   the initialized camera for the lowest possible next-trigger latency.
    // - light_sleep retains the verified OV3660 fast-standby path.
    // - all other cases preserve the legacy deinit/reinit behavior.
    // WebConfig still keeps the camera active for live preview as before.
    bool keepCameraReadyWhileAwake =
        !webConfigStarted &&
        cameraInitialized &&
        cfg_sleep_mode == "off";

    bool keepCameraForFastLightSleep =
        !webConfigStarted &&
        cameraInitialized &&
        cfg_sleep_mode == "light_sleep" &&
        cameraSupportsFastLightSleepNow();

    if (
        !webConfigStarted &&
        cameraInitialized &&
        !keepCameraReadyWhileAwake &&
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
// CONTINUOUS JPEG SHOOTER
// =============================================================
//
// Single timed JPEG scheduler for the SensorForge continuous shooter.
// It supports intervals down to 250 ms, optional dark/similarity rejection,
// a forced-keep interval, and a PSRAM queue that batches SD writes. Accepted
// JPEGs can be persisted either individually or as a sparse MKV with their real
// capture timing. No JPEG re-encoding is performed.
//
// Alarm video always has priority. The shooter never starts an SD write while
// recording, WebConfig preview/maintenance, Sync Exclusive or thermal emergency
// owns the device.

enum ContinuousShooterWakeKind : uint8_t {
    SHOOTER_WAKE_NONE = 0,
    SHOOTER_WAKE_SAMPLE = 1,
    SHOOTER_WAKE_FLUSH = 2
};

struct ShooterBufferedFrameHeader {
    int64_t scheduledWallUs;
    uint64_t captureMonotonicUs;
    uint32_t jpegBytes;
    uint16_t width;
    uint16_t height;
};

static const uint8_t SHOOTER_LONG_INTERVAL_WARMUP_FRAMES = 3;
static const uint32_t SHOOTER_LONG_INTERVAL_MS = 5000UL;
// High-rate continuous capture is kept awake deliberately. Repeated 500/1000 ms
// light-sleep camera standby/wake cycles are not allowed to become a single
// point of failure for unattended capture. Slower shooter intervals may still
// use light sleep, protected by the mandatory timer-arm check below.
static const uint32_t SHOOTER_LIGHT_SLEEP_AWAKE_MAX_INTERVAL_MS = 1000UL;
static const size_t SHOOTER_WRITE_CHUNK = 8U * 1024U;
static const uint8_t SHOOTER_PSRAM_USE_PERCENT = 80U;
static const size_t SHOOTER_PSRAM_RESERVE_BYTES = 1024U * 1024U;
static const size_t SHOOTER_PSRAM_CONTIGUOUS_RESERVE_BYTES = 256U * 1024U;
static const size_t SHOOTER_PSRAM_MIN_BUFFER_BYTES = 64U * 1024U;
static const size_t SHOOTER_PSRAM_ALIGNMENT_BYTES = 4U * 1024U;

static void continuousShooterRefreshScheduleState()
{
    int enabled = cfg_shooter_enabled ? 1 : 0;
    int intervalMs = cfg_shooter_interval_ms;

    if (
        shooterScheduleCachedEnabled == enabled &&
        shooterScheduleCachedIntervalMs == intervalMs
    ) {
        return;
    }

    shooterScheduleCachedEnabled = enabled;
    shooterScheduleCachedIntervalMs = intervalMs;
    shooterScheduleWallClockMode = false;
    shooterLastHandledWallSlotUs = -1;
    shooterFallbackNextDueUs = 0;
    shooterLightSleepScheduledWallUs = 0;
    shooterLightSleepWakeKind = SHOOTER_WAKE_NONE;

    if (!enabled && shooterBufferUsed > 0)
        shooterBufferFlushDue = true;

    imageMotionResetShooterReference();
}


static bool continuousShooterWallClockNowUs(
    int64_t &nowUs
)
{
    nowUs = 0;

    struct timeval tv;

    if (gettimeofday(&tv, nullptr) != 0)
        return false;

    if (tv.tv_sec < (time_t)1609459200)
        return false;

    nowUs =
        (int64_t)tv.tv_sec * 1000000LL +
        (int64_t)tv.tv_usec;

    return true;
}


static uint64_t continuousShooterIntervalUs()
{
    if (
        !cfg_shooter_enabled ||
        cfg_shooter_interval_ms < 250
    ) {
        return 0;
    }

    return
        (uint64_t)cfg_shooter_interval_ms *
        1000ULL;
}


static uint64_t continuousShooterDueToleranceUs()
{
    uint64_t intervalUs = continuousShooterIntervalUs();
    if (intervalUs == 0)
        return 0;

    uint64_t toleranceUs = intervalUs / 2ULL;

    if (toleranceUs < 50000ULL)
        toleranceUs = 50000ULL;
    if (toleranceUs > 2000000ULL)
        toleranceUs = 2000000ULL;

    return toleranceUs;
}


static uint64_t continuousShooterFlushRemainingUs()
{
    if (shooterBufferUsed == 0)
        return UINT64_MAX;

    if (shooterBufferFlushDue)
        return 1000ULL;

    if (cfg_shooter_flush_seconds <= 0)
        return 1000ULL;

    if (shooterBufferFirstQueuedMs == 0)
        return 1000ULL;

    uint64_t timeoutMs =
        (uint64_t)cfg_shooter_flush_seconds *
        1000ULL;

    uint32_t elapsedMs =
        (uint32_t)(millis() - shooterBufferFirstQueuedMs);

    if ((uint64_t)elapsedMs >= timeoutMs)
        return 1000ULL;

    return
        (timeoutMs - (uint64_t)elapsedMs) *
        1000ULL;
}


static bool continuousShooterNextWakeDelayUs(
    uint64_t &delayUs,
    int64_t &scheduledWallUs,
    uint8_t &wakeKind
)
{
    delayUs = 0;
    scheduledWallUs = 0;
    wakeKind = SHOOTER_WAKE_NONE;

    continuousShooterRefreshScheduleState();

    uint64_t sampleDelayUs = UINT64_MAX;
    int64_t sampleScheduledWallUs = 0;
    uint64_t intervalUs = continuousShooterIntervalUs();

    if (intervalUs > 0) {
        int64_t wallNowUs = 0;

        if (continuousShooterWallClockNowUs(wallNowUs)) {
            shooterScheduleWallClockMode = true;
            shooterFallbackNextDueUs = 0;

            int64_t targetUs =
                (
                    wallNowUs /
                    (int64_t)intervalUs +
                    1LL
                ) *
                (int64_t)intervalUs;

            int64_t remainingUs =
                targetUs - wallNowUs;

            if (remainingUs < 1000LL)
                remainingUs = 1000LL;

            sampleDelayUs =
                (uint64_t)remainingUs;

            sampleScheduledWallUs =
                targetUs;
        } else {
            shooterScheduleWallClockMode = false;
            shooterLastHandledWallSlotUs = -1;

            uint64_t nowMonoUs =
                (uint64_t)esp_timer_get_time();

            if (shooterFallbackNextDueUs == 0) {
                shooterFallbackNextDueUs =
                    nowMonoUs + intervalUs;
            }

            while (shooterFallbackNextDueUs <= nowMonoUs) {
                shooterFallbackNextDueUs += intervalUs;
            }

            sampleDelayUs =
                shooterFallbackNextDueUs - nowMonoUs;
        }
    }

    uint64_t flushDelayUs =
        continuousShooterFlushRemainingUs();

    if (flushDelayUs < sampleDelayUs) {
        delayUs = flushDelayUs;
        wakeKind = SHOOTER_WAKE_FLUSH;
        return true;
    }

    if (sampleDelayUs != UINT64_MAX) {
        delayUs = sampleDelayUs;
        scheduledWallUs = sampleScheduledWallUs;
        wakeKind = SHOOTER_WAKE_SAMPLE;
        return true;
    }

    return false;
}


static void continuousShooterMarkScheduledHandled(
    int64_t scheduledWallUs
)
{
    continuousShooterRefreshScheduleState();

    if (scheduledWallUs > 0) {
        shooterScheduleWallClockMode = true;

        if (scheduledWallUs > shooterLastHandledWallSlotUs) {
            shooterLastHandledWallSlotUs =
                scheduledWallUs;
        }

        return;
    }

    uint64_t intervalUs = continuousShooterIntervalUs();
    if (intervalUs == 0)
        return;

    uint64_t nowMonoUs =
        (uint64_t)esp_timer_get_time();

    if (shooterFallbackNextDueUs == 0) {
        shooterFallbackNextDueUs =
            nowMonoUs + intervalUs;
        return;
    }

    while (shooterFallbackNextDueUs <= nowMonoUs) {
        shooterFallbackNextDueUs += intervalUs;
    }
}


static bool continuousShooterAlarmHasPriority()
{
    radarLoop();
    motionDiagnosticsLoop();

    return motionDetected();
}


static void continuousShooterLogFailure(const String &message)
{
    uint32_t nowMs = millis();

    if (
        shooterLastFailureLogMs != 0 &&
        (uint32_t)(nowMs - shooterLastFailureLogMs) < 10000UL
    ) {
        return;
    }

    shooterLastFailureLogMs = nowMs;
    logWrite("Shooter failed | " + message);
}


static bool continuousShooterBuildJpegPath(
    int64_t scheduledWallUs,
    uint64_t captureMonotonicUs,
    String &finalPath,
    String &tempPath
)
{
    finalPath = "";
    tempPath = "";

    String folder;
    String filename;

    if (scheduledWallUs >= (int64_t)1609459200 * 1000000LL) {
        time_t slotSeconds =
            (time_t)(scheduledWallUs / 1000000LL);

        uint16_t slotMs =
            (uint16_t)((scheduledWallUs % 1000000LL) / 1000LL);

        struct tm localTime;

        if (!localtime_r(&slotSeconds, &localTime))
            return false;

        char folderBuffer[24];
        char timeBuffer[16];
        char fileBuffer[32];

        if (
            strftime(
                folderBuffer,
                sizeof(folderBuffer),
                "/%Y%m%d",
                &localTime
            ) == 0 ||
            strftime(
                timeBuffer,
                sizeof(timeBuffer),
                "%H%M%S",
                &localTime
            ) == 0
        ) {
            return false;
        }

        snprintf(
            fileBuffer,
            sizeof(fileBuffer),
            "%s_%03u.jpg",
            timeBuffer,
            (unsigned)slotMs
        );

        folder = String(folderBuffer);
        filename = String(fileBuffer);
    } else {
        folder = "/fallback";

        uint64_t captureMs =
            captureMonotonicUs /
            1000ULL;

        char fileBuffer[48];
        snprintf(
            fileBuffer,
            sizeof(fileBuffer),
            "shooter_%llu.jpg",
            (unsigned long long)captureMs
        );
        filename = String(fileBuffer);
    }

    if (!STORAGE.exists(folder.c_str())) {
        if (!STORAGE.mkdir(folder.c_str()))
            return false;
    }

    finalPath = folder + "/" + filename;
    tempPath = finalPath + ".part";
    return true;
}


static bool continuousShooterBuildMkvPath(
    int64_t firstScheduledWallUs,
    uint64_t firstCaptureMonotonicUs,
    String &finalPath,
    String &tempPath
)
{
    finalPath = "";
    tempPath = "";

    String folder;
    String filename;

    if (firstScheduledWallUs >= (int64_t)1609459200 * 1000000LL) {
        time_t slotSeconds = (time_t)(firstScheduledWallUs / 1000000LL);
        uint16_t slotMs = (uint16_t)((firstScheduledWallUs % 1000000LL) / 1000LL);
        struct tm localTime;

        if (!localtime_r(&slotSeconds, &localTime))
            return false;

        char folderBuffer[24];
        char timeBuffer[16];
        char fileBuffer[48];

        if (
            strftime(folderBuffer, sizeof(folderBuffer), "/%Y%m%d", &localTime) == 0 ||
            strftime(timeBuffer, sizeof(timeBuffer), "%H%M%S", &localTime) == 0
        ) {
            return false;
        }

        snprintf(
            fileBuffer,
            sizeof(fileBuffer),
            "%s_%03u_shooter.mkv",
            timeBuffer,
            (unsigned)slotMs
        );
        folder = String(folderBuffer);
        filename = String(fileBuffer);
    } else {
        folder = "/fallback";
        char fileBuffer[56];
        snprintf(
            fileBuffer,
            sizeof(fileBuffer),
            "shooter_%llu.mkv",
            (unsigned long long)(firstCaptureMonotonicUs / 1000ULL)
        );
        filename = String(fileBuffer);
    }

    if (!STORAGE.exists(folder.c_str())) {
        if (!STORAGE.mkdir(folder.c_str()))
            return false;
    }

    finalPath = folder + "/" + filename;
    tempPath = finalPath + ".part";
    return true;
}


static bool continuousShooterWriteJpeg(
    int64_t scheduledWallUs,
    uint64_t captureMonotonicUs,
    const uint8_t *jpeg,
    size_t jpegBytes,
    bool ignoreAlarmPriority = false
)
{
    if (!jpeg || jpegBytes == 0)
        return false;

    if (
        recording ||
        recorderIsOpen() ||
        !sdReady ||
        g_storageLocked ||
        syncApiExclusiveActive() ||
        thermalEmergencyState
    ) {
        return false;
    }

    // Persisting an already accepted PSRAM frame is allowed while WebConfig is
    // open. Capture itself remains blocked by preview/maintenance, but delaying a
    // due flush would violate the user's configured persistence window.
    if (!ignoreAlarmPriority && continuousShooterAlarmHasPriority())
        return false;

    if (!storagePrepareForRecording())
        return false;

    String finalPath;
    String tempPath;

    if (!continuousShooterBuildJpegPath(
            scheduledWallUs,
            captureMonotonicUs,
            finalPath,
            tempPath
        )) {
        return false;
    }

    if (STORAGE.exists(finalPath.c_str()))
        return true;

    if (STORAGE.exists(tempPath.c_str()))
        STORAGE.remove(tempPath.c_str());

    RecordingStorageFile output;

    if (!output.openWrite(
            tempPath,
            cfg_recording_encryption != 0
        )) {
        return false;
    }

    bool ok = true;
    size_t offset = 0;

    while (offset < jpegBytes) {
        if (!ignoreAlarmPriority && continuousShooterAlarmHasPriority()) {
            ok = false;
            break;
        }

        size_t remaining = jpegBytes - offset;
        size_t chunk =
            remaining < SHOOTER_WRITE_CHUNK
            ? remaining
            : SHOOTER_WRITE_CHUNK;

        size_t written =
            output.write(
                jpeg + offset,
                chunk
            );

        if (written != chunk) {
            ok = false;
            break;
        }

        offset += written;
        feedWatchdog();
        yield();
    }

    bool closeOk = output.closeChecked();
    ok = ok && closeOk;

    if (!ok) {
        STORAGE.remove(tempPath.c_str());
        return false;
    }

    if (!STORAGE.rename(
            tempPath.c_str(),
            finalPath.c_str()
        )) {
        STORAGE.remove(tempPath.c_str());
        return false;
    }

    return true;
}


static bool continuousShooterWriteSparseMkvSingle(
    int64_t scheduledWallUs,
    uint64_t captureMonotonicUs,
    const uint8_t *jpeg,
    size_t jpegBytes,
    uint16_t width,
    uint16_t height
)
{
    if (!jpeg || jpegBytes == 0 || width == 0 || height == 0)
        return false;

    if (
        recording || recorderIsOpen() || !sdReady || g_storageLocked ||
        syncApiExclusiveActive() || thermalEmergencyState ||
        continuousShooterAlarmHasPriority()
    ) {
        return false;
    }

    if (!storagePrepareForRecording())
        return false;

    String finalPath;
    String tempPath;
    if (!continuousShooterBuildMkvPath(
            scheduledWallUs,
            captureMonotonicUs,
            finalPath,
            tempPath
        )) {
        return false;
    }

    if (STORAGE.exists(finalPath.c_str()))
        return true;

    if (STORAGE.exists(tempPath.c_str()))
        STORAGE.remove(tempPath.c_str());

    time_t startEpoch = 0;
    if (scheduledWallUs >= (int64_t)1609459200 * 1000000LL)
        startEpoch = (time_t)(scheduledWallUs / 1000000LL);

    bool ok =
        mkvStartSparseJpeg(
            tempPath,
            width,
            height,
            startEpoch,
            (uint32_t)cfg_shooter_interval_ms
        ) &&
        mkvAddSparseJpeg(
            jpeg,
            jpegBytes,
            width,
            height,
            0
        ) &&
        mkvEnd();

    if (!ok) {
        if (mkvIsOpen())
            mkvEnd();
        STORAGE.remove(tempPath.c_str());
        return false;
    }

    if (!STORAGE.rename(tempPath.c_str(), finalPath.c_str())) {
        STORAGE.remove(tempPath.c_str());
        return false;
    }

    return true;
}


static bool continuousShooterWriteSparseMkvFromBuffer(
    size_t &consumedBytes,
    uint32_t &flushedFrames,
    uint64_t &flushedJpegBytes,
    bool ignoreAlarmPriority
)
{
    consumedBytes = 0;
    flushedFrames = 0;
    flushedJpegBytes = 0;

    if (shooterBufferUsed < sizeof(ShooterBufferedFrameHeader))
        return false;

    if (!storagePrepareForRecording())
        return false;

    ShooterBufferedFrameHeader firstHeader = {};
    memcpy(&firstHeader, shooterBuffer, sizeof(firstHeader));

    if (
        firstHeader.jpegBytes == 0 ||
        sizeof(firstHeader) + (size_t)firstHeader.jpegBytes > shooterBufferUsed ||
        firstHeader.width == 0 ||
        firstHeader.height == 0
    ) {
        return false;
    }

    String finalPath;
    String tempPath;

    if (!continuousShooterBuildMkvPath(
            firstHeader.scheduledWallUs,
            firstHeader.captureMonotonicUs,
            finalPath,
            tempPath
        )) {
        return false;
    }

    // A successful previous retry may already have committed exactly this
    // segment. Treat it as persisted so reboot/shutdown retry loops can drain.
    if (STORAGE.exists(finalPath.c_str())) {
        size_t offset = 0;
        while (offset + sizeof(ShooterBufferedFrameHeader) <= shooterBufferUsed) {
            ShooterBufferedFrameHeader h = {};
            memcpy(&h, shooterBuffer + offset, sizeof(h));
            size_t recordBytes = sizeof(h) + (size_t)h.jpegBytes;
            if (h.jpegBytes == 0 || recordBytes > shooterBufferUsed - offset)
                return false;
            offset += recordBytes;
            ++flushedFrames;
            flushedJpegBytes += h.jpegBytes;
        }
        consumedBytes = offset;
        return consumedBytes > 0;
    }

    if (STORAGE.exists(tempPath.c_str()))
        STORAGE.remove(tempPath.c_str());

    time_t startEpoch = 0;
    if (firstHeader.scheduledWallUs >= (int64_t)1609459200 * 1000000LL)
        startEpoch = (time_t)(firstHeader.scheduledWallUs / 1000000LL);

    if (!mkvStartSparseJpeg(
            tempPath,
            firstHeader.width,
            firstHeader.height,
            startEpoch,
            (uint32_t)cfg_shooter_interval_ms
        )) {
        STORAGE.remove(tempPath.c_str());
        return false;
    }

    const int64_t firstWallUs = firstHeader.scheduledWallUs;
    const uint64_t firstMonoUs = firstHeader.captureMonotonicUs;
    size_t offset = 0;
    bool ok = true;

    while (offset + sizeof(ShooterBufferedFrameHeader) <= shooterBufferUsed) {
        if (!ignoreAlarmPriority && continuousShooterAlarmHasPriority()) {
            ok = false;
            break;
        }

        ShooterBufferedFrameHeader header = {};
        memcpy(&header, shooterBuffer + offset, sizeof(header));

        size_t recordBytes = sizeof(header) + (size_t)header.jpegBytes;
        if (
            header.jpegBytes == 0 ||
            recordBytes > shooterBufferUsed - offset ||
            header.width != firstHeader.width ||
            header.height != firstHeader.height
        ) {
            ok = false;
            break;
        }

        uint64_t relativeMs = 0;
        if (
            firstWallUs > 0 &&
            header.scheduledWallUs >= firstWallUs
        ) {
            relativeMs = (uint64_t)(header.scheduledWallUs - firstWallUs) / 1000ULL;
        } else if (header.captureMonotonicUs >= firstMonoUs) {
            relativeMs = (header.captureMonotonicUs - firstMonoUs) / 1000ULL;
        } else {
            ok = false;
            break;
        }

        const uint8_t *jpeg =
            shooterBuffer + offset + sizeof(header);

        if (!mkvAddSparseJpeg(
                jpeg,
                header.jpegBytes,
                header.width,
                header.height,
                relativeMs
            )) {
            ok = false;
            break;
        }

        offset += recordBytes;
        ++flushedFrames;
        flushedJpegBytes += header.jpegBytes;
        feedWatchdog();
        yield();
    }

    // A sparse MKV is committed only if the entire current RAM batch made it
    // into the container. If alarm priority interrupts a normal flush, discard
    // the .part and keep every frame in PSRAM for the next attempt. This avoids
    // duplicate/ambiguous partial segments.
    if (!ok || offset != shooterBufferUsed || !mkvEnd()) {
        if (mkvIsOpen())
            mkvEnd();
        STORAGE.remove(tempPath.c_str());
        consumedBytes = 0;
        flushedFrames = 0;
        flushedJpegBytes = 0;
        return false;
    }

    if (!STORAGE.rename(tempPath.c_str(), finalPath.c_str())) {
        STORAGE.remove(tempPath.c_str());
        consumedBytes = 0;
        flushedFrames = 0;
        flushedJpegBytes = 0;
        return false;
    }

    consumedBytes = offset;
    return true;
}


static void continuousShooterReleaseBuffer()
{
    if (shooterBuffer) {
        heap_caps_free(shooterBuffer);
        shooterBuffer = nullptr;
    }

    shooterBufferCapacity = 0;
    shooterBufferUsed = 0;
    shooterBufferFrames = 0;
    shooterBufferFirstQueuedMs = 0;
    shooterBufferFlushDue = false;
}


static size_t continuousShooterAlignDown(
    size_t value,
    size_t alignment
)
{
    if (alignment == 0)
        return value;

    return value - (value % alignment);
}


static bool continuousShooterEnsureBuffer()
{
    if (cfg_shooter_flush_seconds <= 0)
        return false;

    if (shooterBuffer)
        return true;

    const uint32_t caps =
        MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT;

    size_t freeBefore =
        heap_caps_get_free_size(caps);

    size_t largestBefore =
        heap_caps_get_largest_free_block(caps);

    if (
        freeBefore <=
            SHOOTER_PSRAM_RESERVE_BYTES +
            SHOOTER_PSRAM_MIN_BUFFER_BYTES ||
        largestBefore <=
            SHOOTER_PSRAM_CONTIGUOUS_RESERVE_BYTES +
            SHOOTER_PSRAM_MIN_BUFFER_BYTES
    ) {
        uint32_t nowMs = millis();
        if (
            shooterLastBufferAllocErrorLogMs == 0 ||
            (uint32_t)(nowMs - shooterLastBufferAllocErrorLogMs) >= 60000UL
        ) {
            shooterLastBufferAllocErrorLogMs = nowMs;
            logWrite(
                "Shooter PSRAM buffer unavailable | free_kb=" +
                String((unsigned long)(freeBefore / 1024U)) +
                " | largest_kb=" +
                String((unsigned long)(largestBefore / 1024U)) +
                " | direct SD fallback"
            );
        }
        return false;
    }

    size_t usableAfterReserve =
        freeBefore -
        SHOOTER_PSRAM_RESERVE_BYTES;

    size_t target =
        (usableAfterReserve *
         (size_t)SHOOTER_PSRAM_USE_PERCENT) /
        100U;

    size_t largestSafe =
        largestBefore -
        SHOOTER_PSRAM_CONTIGUOUS_RESERVE_BYTES;

    if (target > largestSafe)
        target = largestSafe;

    target = continuousShooterAlignDown(
        target,
        SHOOTER_PSRAM_ALIGNMENT_BYTES
    );

    // Fragmentation may change between the size query and allocation. Retry
    // with smaller blocks instead of abandoning batching after one failure.
    size_t attempt = target;

    while (attempt >= SHOOTER_PSRAM_MIN_BUFFER_BYTES) {
        shooterBuffer =
            (uint8_t *)heap_caps_malloc(
                attempt,
                caps
            );

        if (shooterBuffer) {
            shooterBufferCapacity = attempt;

            logWrite(
                "Shooter PSRAM buffer auto | capacity_kb=" +
                String((unsigned long)(attempt / 1024U)) +
                " | free_before_kb=" +
                String((unsigned long)(freeBefore / 1024U)) +
                " | largest_before_kb=" +
                String((unsigned long)(largestBefore / 1024U)) +
                " | reserve_kb=" +
                String((unsigned long)(SHOOTER_PSRAM_RESERVE_BYTES / 1024U)) +
                " | use_pct=" +
                String((unsigned)SHOOTER_PSRAM_USE_PERCENT)
            );

            return true;
        }

        attempt = continuousShooterAlignDown(
            (attempt * 3U) / 4U,
            SHOOTER_PSRAM_ALIGNMENT_BYTES
        );
    }

    shooterBufferCapacity = 0;

    uint32_t nowMs = millis();
    if (
        shooterLastBufferAllocErrorLogMs == 0 ||
        (uint32_t)(nowMs - shooterLastBufferAllocErrorLogMs) >= 60000UL
    ) {
        shooterLastBufferAllocErrorLogMs = nowMs;
        logWrite(
            "Shooter PSRAM buffer allocation failed | free_kb=" +
            String((unsigned long)(freeBefore / 1024U)) +
            " | target_kb=" +
            String((unsigned long)(target / 1024U)) +
            " | direct SD fallback"
        );
    }

    return false;
}


static bool continuousShooterFlushBuffer(
    const char *reason,
    bool ignoreAlarmPriority = false
)
{
    if (shooterBufferUsed == 0) {
        shooterBufferFrames = 0;
        shooterBufferFirstQueuedMs = 0;
        shooterBufferFlushDue = false;
        return true;
    }

    if (
        recording ||
        recorderIsOpen() ||
        !sdReady ||
        g_storageLocked ||
        syncApiExclusiveActive() ||
        thermalEmergencyState ||
        (
            !ignoreAlarmPriority &&
            continuousShooterAlarmHasPriority()
        )
    ) {
        shooterBufferFlushDue = true;
        return false;
    }

    size_t offset = 0;
    uint32_t flushedFrames = 0;
    uint64_t flushedJpegBytes = 0;
    uint32_t startedMs = millis();
    uint32_t originalFirstQueuedMs = shooterBufferFirstQueuedMs;

    if (cfg_shooter_storage_format == "mkv") {
        if (!continuousShooterWriteSparseMkvFromBuffer(
                offset,
                flushedFrames,
                flushedJpegBytes,
                ignoreAlarmPriority
            )) {
            shooterBufferFlushDue = true;
            return false;
        }
    } else while (
        offset + sizeof(ShooterBufferedFrameHeader) <=
        shooterBufferUsed
    ) {
        ShooterBufferedFrameHeader header;
        memcpy(
            &header,
            shooterBuffer + offset,
            sizeof(header)
        );

        size_t recordBytes =
            sizeof(header) +
            (size_t)header.jpegBytes;

        if (
            header.jpegBytes == 0 ||
            recordBytes > shooterBufferUsed - offset
        ) {
            logWrite("Shooter buffer corrupted | dropping queue");
            continuousShooterReleaseBuffer();
            return false;
        }

        if (
            !ignoreAlarmPriority &&
            continuousShooterAlarmHasPriority()
        ) {
            break;
        }

        const uint8_t *jpeg =
            shooterBuffer +
            offset +
            sizeof(header);

        if (!continuousShooterWriteJpeg(
                header.scheduledWallUs,
                header.captureMonotonicUs,
                jpeg,
                header.jpegBytes,
                ignoreAlarmPriority
            )) {
            break;
        }

        offset += recordBytes;
        ++flushedFrames;
        flushedJpegBytes += header.jpegBytes;

        feedWatchdog();
        yield();
    }

    if (offset > 0) {
        size_t remaining =
            shooterBufferUsed - offset;

        if (remaining > 0) {
            memmove(
                shooterBuffer,
                shooterBuffer + offset,
                remaining
            );
        }

        shooterBufferUsed = remaining;

        if (flushedFrames >= shooterBufferFrames)
            shooterBufferFrames = 0;
        else
            shooterBufferFrames -= flushedFrames;

        if (remaining == 0) {
            shooterBufferFirstQueuedMs = 0;
            shooterBufferFlushDue = false;
        } else {
            // Preserve the age of the oldest still-unflushed frame. The user's
            // configured persistence timeout is a maximum loss window, not a
            // timer that restarts after a partial flush/preemption.
            shooterBufferFirstQueuedMs = originalFirstQueuedMs;
            shooterBufferFlushDue = true;
        }

        char flushSummary[448];

        uint32_t sleepAverageMs =
            shooterSleepCyclesPending > 0
            ? (uint32_t)(
                shooterSleepTotalMsPending /
                (uint64_t)shooterSleepCyclesPending
            )
            : 0;

        snprintf(
            flushSummary,
            sizeof(flushSummary),
            "Shooter flush | reason=%s | format=%s | frames=%lu | jpeg_bytes=%llu | remaining=%lu | elapsed_ms=%lu | accepted=%lu | rejected_dark=%lu | rejected_similar=%lu | sleep_cycles=%lu | sleep_avg_ms=%lu | sleep_min_ms=%lu | sleep_max_ms=%lu",
            reason ? reason : "unknown",
            cfg_shooter_storage_format.c_str(),
            (unsigned long)flushedFrames,
            (unsigned long long)flushedJpegBytes,
            (unsigned long)shooterBufferUsed,
            (unsigned long)(millis() - startedMs),
            (unsigned long)shooterAcceptedFrames,
            (unsigned long)shooterRejectedDark,
            (unsigned long)shooterRejectedSimilar,
            (unsigned long)shooterSleepCyclesPending,
            (unsigned long)sleepAverageMs,
            (unsigned long)shooterSleepMinMsPending,
            (unsigned long)shooterSleepMaxMsPending
        );
        logWrite(String(flushSummary));

        // The SD is already active for the image batch. Persist the accumulated
        // low-power log queue now so logging does not create a separate card wake.
        logFlush();

        shooterSleepCyclesPending = 0;
        shooterSleepTotalMsPending = 0;
        shooterSleepMinMsPending = 0;
        shooterSleepMaxMsPending = 0;
    }

    return shooterBufferUsed == 0;
}


// WebConfig uses this before deliberate reboot/shutdown so accepted RAM-only
// frames are not discarded by an operator action.
bool continuousShooterFlushBeforeRestart()
{
    if (shooterBufferUsed == 0)
        return true;

    // A deliberate reboot/shutdown has already decided to stop normal
    // operation. Physical motion must therefore not strand RAM-only frames
    // forever; active recording/storage/thermal guards still remain enforced.
    return continuousShooterFlushBuffer(
        "before_restart",
        true
    );
}

bool continuousShooterFlushNow()
{
    if (shooterBufferUsed == 0)
        return true;

    return continuousShooterFlushBuffer(
        "manual_web",
        false
    );
}

uint32_t continuousShooterBufferedFrameCount()
{
    return shooterBufferFrames;
}

uint32_t continuousShooterBufferUsedBytes()
{
    return (uint32_t)shooterBufferUsed;
}

uint32_t continuousShooterBufferCapacityBytes()
{
    return (uint32_t)shooterBufferCapacity;
}

uint32_t continuousShooterAcceptedFrameCount()
{
    return shooterAcceptedFrames;
}

uint32_t continuousShooterRejectedDarkCount()
{
    return shooterRejectedDark;
}

uint32_t continuousShooterRejectedSimilarCount()
{
    return shooterRejectedSimilar;
}

uint64_t continuousShooterAcceptedJpegByteCount()
{
    return shooterAcceptedJpegBytes;
}

bool continuousShooterLastBrightnessValid()
{
    return shooterLastBrightnessValid;
}

float continuousShooterLastBrightnessMean()
{
    return shooterLastBrightnessMean;
}

uint8_t continuousShooterLastBrightnessPeak()
{
    return shooterLastBrightnessPeak;
}

uint32_t continuousShooterLastBrightnessAgeMs()
{
    if (!shooterLastBrightnessValid)
        return 0;

    return (uint32_t)(
        millis() -
        shooterLastBrightnessMs
    );
}


static bool continuousShooterQueueJpeg(
    int64_t scheduledWallUs,
    uint64_t captureMonotonicUs,
    const uint8_t *jpeg,
    size_t jpegBytes,
    uint16_t width,
    uint16_t height
)
{
    if (cfg_shooter_flush_seconds <= 0) {
        if (cfg_shooter_storage_format == "mkv") {
            return continuousShooterWriteSparseMkvSingle(
                scheduledWallUs,
                captureMonotonicUs,
                jpeg,
                jpegBytes,
                width,
                height
            );
        }
        return continuousShooterWriteJpeg(
            scheduledWallUs,
            captureMonotonicUs,
            jpeg,
            jpegBytes
        );
    }

    if (!continuousShooterEnsureBuffer()) {
        if (cfg_shooter_storage_format == "mkv") {
            return continuousShooterWriteSparseMkvSingle(
                scheduledWallUs,
                captureMonotonicUs,
                jpeg,
                jpegBytes,
                width,
                height
            );
        }
        return continuousShooterWriteJpeg(
            scheduledWallUs,
            captureMonotonicUs,
            jpeg,
            jpegBytes
        );
    }

    ShooterBufferedFrameHeader header = {};
    header.scheduledWallUs = scheduledWallUs;
    header.captureMonotonicUs = captureMonotonicUs;
    header.jpegBytes = (uint32_t)jpegBytes;
    header.width = width;
    header.height = height;

    size_t recordBytes =
        sizeof(header) +
        jpegBytes;

    if (recordBytes > shooterBufferCapacity) {
        continuousShooterFlushBuffer("oversize_frame");
        if (cfg_shooter_storage_format == "mkv") {
            return continuousShooterWriteSparseMkvSingle(
                scheduledWallUs,
                captureMonotonicUs,
                jpeg,
                jpegBytes,
                width,
                height
            );
        }
        return continuousShooterWriteJpeg(
            scheduledWallUs,
            captureMonotonicUs,
            jpeg,
            jpegBytes
        );
    }

    if (
        shooterBufferUsed + recordBytes >
        shooterBufferCapacity
    ) {
        if (!continuousShooterFlushBuffer("buffer_full")) {
            return false;
        }
    }

    if (
        shooterBufferUsed + recordBytes >
        shooterBufferCapacity
    ) {
        return false;
    }

    if (shooterBufferUsed == 0) {
        shooterBufferFirstQueuedMs = millis();
    }

    memcpy(
        shooterBuffer + shooterBufferUsed,
        &header,
        sizeof(header)
    );
    shooterBufferUsed += sizeof(header);

    memcpy(
        shooterBuffer + shooterBufferUsed,
        jpeg,
        jpegBytes
    );
    shooterBufferUsed += jpegBytes;
    shooterBufferFrames++;

    if (
        shooterBufferUsed >=
        shooterBufferCapacity
    ) {
        shooterBufferFlushDue = true;
    }

    return true;
}


static bool continuousShooterForceSaveDue(
    uint64_t nowMonoUs
)
{
    if (
        cfg_shooter_force_save_seconds <= 0 ||
        shooterLastAcceptedMonoUs == 0
    ) {
        return false;
    }

    uint64_t forceUs =
        (uint64_t)cfg_shooter_force_save_seconds *
        1000000ULL;

    return
        nowMonoUs - shooterLastAcceptedMonoUs >=
        forceUs;
}


static bool continuousShooterCapture(
    int64_t scheduledWallUs
)
{
    if (
        !cfg_shooter_enabled ||
        cfg_shooter_interval_ms < 250 ||
        recording ||
        recorderIsOpen() ||
        !sdReady ||
        g_storageLocked ||
        syncApiExclusiveActive() ||
        thermalEmergencyState
    ) {
        return false;
    }

    // WebConfig's recording-automation pause is intentionally limited to the
    // motion/alarm recording path. The independent time-based Dauershooter must
    // keep running while an operator merely has WebConfig open. Only an active
    // live preview owns the camera pipeline and therefore blocks a shooter frame.
    if (
        webConfigStarted &&
        webConfigCameraPreviewActive()
    ) {
        return false;
    }

    if (continuousShooterAlarmHasPriority())
        return false;

    bool cameraWasInitialized =
        cameraInitialized;

    if (!initCamera(
            cfg_camera,
            cfg_resolution,
            cfg_quality
        )) {
        continuousShooterLogFailure("camera init");
        return false;
    }

    bool wasSoftPoweredDown =
        cameraSoftPowerDownActive;

    if (!cameraExitSoftPowerDown()) {
        if (!recoverCamera()) {
            continuousShooterLogFailure("camera wake/recovery");
            return false;
        }
    }

    uint8_t warmupFrames =
        (
            cfg_shooter_interval_ms >=
                (int)SHOOTER_LONG_INTERVAL_MS ||
            !shooterInitialWarmupDone ||
            !cameraWasInitialized ||
            (
                wasSoftPoweredDown &&
                cfg_shooter_interval_ms >=
                    (int)SHOOTER_LONG_INTERVAL_MS
            )
        )
        ? SHOOTER_LONG_INTERVAL_WARMUP_FRAMES
        : 0U;

    for (uint8_t i = 0; i < warmupFrames; ++i) {
        if (continuousShooterAlarmHasPriority())
            return false;

        camera_fb_t *warmup = esp_camera_fb_get();
        if (!warmup) {
            continuousShooterLogFailure("warmup frame unavailable");
            return false;
        }

        esp_camera_fb_return(warmup);
        feedWatchdog();
        yield();
    }

    shooterInitialWarmupDone = true;

    if (continuousShooterAlarmHasPriority())
        return false;

    camera_fb_t *frame = esp_camera_fb_get();

    if (!frame) {
        continuousShooterLogFailure("capture frame unavailable");
        return false;
    }

    if (
        frame->format != PIXFORMAT_JPEG ||
        !frame->buf ||
        frame->len == 0
    ) {
        esp_camera_fb_return(frame);
        continuousShooterLogFailure("camera frame is not JPEG");
        return false;
    }

    ShooterImageMetrics metrics;
    bool analyzed = imageMotionAnalyzeShooterJpeg(
        frame->buf,
        frame->len,
        (uint16_t)frame->width,
        (uint16_t)frame->height,
        metrics
    );

    uint64_t captureMonoUs =
        (uint64_t)esp_timer_get_time();

    if (analyzed) {
        shooterLastBrightnessMean = metrics.globalMean;
        shooterLastBrightnessPeak = metrics.brightestBlockMean;
        shooterLastBrightnessMs = millis();
        shooterLastBrightnessValid = true;
    }

    // A whole-frame mean alone can misclassify a valuable night scene: a
    // person with a flashlight may occupy only a small part of an otherwise
    // dark image. The shooter analyzer already has 20x15 block means, so use
    // the brightest local block as a virtually free fail-open guard. Static
    // bright spots are still suppressed by the independent similarity filter.
    float localBrightnessProtectThreshold =
        (float)cfg_shooter_dark_mean_min + 20.0f;

    if (localBrightnessProtectThreshold < 40.0f)
        localBrightnessProtectThreshold = 40.0f;
    if (localBrightnessProtectThreshold > 255.0f)
        localBrightnessProtectThreshold = 255.0f;

    bool dark =
        analyzed &&
        cfg_shooter_dark_mean_min > 0 &&
        metrics.globalMean <
            (float)cfg_shooter_dark_mean_min &&
        (float)metrics.brightestBlockMean <
            localBrightnessProtectThreshold;

    bool forceSave =
        continuousShooterForceSaveDue(
            captureMonoUs
        );

    float changedPct =
        metrics.referenceReady
        ? 100.0f - metrics.similarityPct
        : 100.0f;

    if (changedPct < 0.0f)
        changedPct = 0.0f;

    bool tooSimilar =
        analyzed &&
        !forceSave &&
        cfg_shooter_min_change_pct > 0.0f &&
        metrics.referenceReady &&
        changedPct < cfg_shooter_min_change_pct;

    if (dark || tooSimilar) {
        esp_camera_fb_return(frame);

        if (dark)
            ++shooterRejectedDark;
        else
            ++shooterRejectedSimilar;

        return false;
    }

    // Analyzer failure is fail-open: a filtering fault must not silently erase
    // the source material. It is rate-limited to avoid a log storm at 2 fps.
    if (!analyzed) {
        uint32_t nowMs = millis();
        if (
            shooterLastAnalyzerErrorLogMs == 0 ||
            (uint32_t)(nowMs - shooterLastAnalyzerErrorLogMs) >= 60000UL
        ) {
            shooterLastAnalyzerErrorLogMs = nowMs;
            logWrite("Shooter analyzer failed | frame accepted fail-open");
        }
    }

    if (continuousShooterAlarmHasPriority()) {
        esp_camera_fb_return(frame);
        return false;
    }

    bool queued = continuousShooterQueueJpeg(
        scheduledWallUs,
        captureMonoUs,
        frame->buf,
        frame->len,
        (uint16_t)frame->width,
        (uint16_t)frame->height
    );

    size_t acceptedBytes = frame->len;
    esp_camera_fb_return(frame);

    if (!queued) {
        continuousShooterLogFailure("queue/SD persistence");
        return false;
    }

    if (analyzed)
        imageMotionCommitShooterReference();

    shooterLastAcceptedMonoUs = captureMonoUs;
    ++shooterAcceptedFrames;
    shooterAcceptedJpegBytes += acceptedBytes;

    return true;
}


static void continuousShooterServiceFlush()
{
    if (shooterBufferUsed == 0) {
        if (
            shooterBuffer &&
            (
                !cfg_shooter_enabled ||
                cfg_shooter_flush_seconds <= 0
            )
        ) {
            continuousShooterReleaseBuffer();
        }
        return;
    }

    if (
        shooterBufferFlushDue ||
        continuousShooterFlushRemainingUs() <= 1000ULL
    ) {
        continuousShooterFlushBuffer(
            shooterBufferFlushDue
            ? "buffer_due"
            : "timeout"
        );
    }
}


static bool continuousShooterServiceDue()
{
    continuousShooterRefreshScheduleState();

    uint64_t intervalUs = continuousShooterIntervalUs();
    if (intervalUs == 0)
        return false;

    int64_t wallNowUs = 0;

    if (continuousShooterWallClockNowUs(wallNowUs)) {
        if (!shooterScheduleWallClockMode) {
            shooterScheduleWallClockMode = true;
            shooterFallbackNextDueUs = 0;
            shooterLastHandledWallSlotUs = -1;
        }

        int64_t slotUs =
            (
                wallNowUs /
                (int64_t)intervalUs
            ) *
            (int64_t)intervalUs;

        if (slotUs <= shooterLastHandledWallSlotUs)
            return false;

        int64_t slotAgeUs =
            wallNowUs - slotUs;

        shooterLastHandledWallSlotUs = slotUs;

        if (
            slotAgeUs >
            (int64_t)continuousShooterDueToleranceUs()
        ) {
            return false;
        }

        if (continuousShooterAlarmHasPriority())
            return false;

        return continuousShooterCapture(slotUs);
    }

    shooterScheduleWallClockMode = false;
    shooterLastHandledWallSlotUs = -1;

    uint64_t nowMonoUs =
        (uint64_t)esp_timer_get_time();

    if (shooterFallbackNextDueUs == 0) {
        shooterFallbackNextDueUs =
            nowMonoUs + intervalUs;
        return false;
    }

    if (nowMonoUs < shooterFallbackNextDueUs)
        return false;

    uint64_t dueUs = shooterFallbackNextDueUs;

    do {
        shooterFallbackNextDueUs += intervalUs;
    } while (shooterFallbackNextDueUs <= nowMonoUs);

    if (
        nowMonoUs - dueUs >
        continuousShooterDueToleranceUs()
    ) {
        return false;
    }

    if (continuousShooterAlarmHasPriority())
        return false;

    return continuousShooterCapture(0);
}


static bool continuousShooterHandleScheduledWake(
    int64_t scheduledWallUs
)
{
    continuousShooterRefreshScheduleState();

    uint64_t intervalUs = continuousShooterIntervalUs();
    if (intervalUs == 0)
        return false;

    if (
        scheduledWallUs > 0 &&
        scheduledWallUs % (int64_t)intervalUs != 0
    ) {
        continuousShooterMarkScheduledHandled(
            scheduledWallUs
        );
        return false;
    }

    continuousShooterMarkScheduledHandled(
        scheduledWallUs
    );

    if (continuousShooterAlarmHasPriority())
        return false;

    return continuousShooterCapture(
        scheduledWallUs
    );
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
    return !motionDiagnosticsPresenceActive();
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
    // Hand PRESENCE_PIN from the normal GPIO interrupt path to the RTC wake
    // controller. The interrupt is restored after a light-sleep wake; deep
    // sleep reboots and initializes it again in setup().
    motionDiagnosticsSuspendPresenceInterrupt();

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


    const bool presenceWakeEnabled =
        cfg_motion_recording_enabled != 0;

    if (presenceWakeEnabled) {
        // Presence / PIR / LD2410S OT2: active HIGH.
        rtc_gpio_init(PIR_PIN);
        rtc_gpio_set_direction(PIR_PIN, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(PIR_PIN);
        rtc_gpio_pulldown_en(PIR_PIN);
    }


    esp_err_t magnetErr =
        esp_sleep_enable_ext0_wakeup(
            MAGNET_SWITCH_PIN,
            0
        );


    esp_err_t presenceErr = ESP_OK;

    if (presenceWakeEnabled) {
        uint64_t presenceMask =
            1ULL <<
            (uint32_t)PIR_PIN;

        presenceErr =
            esp_sleep_enable_ext1_wakeup(
                presenceMask,
                ESP_EXT1_WAKEUP_ANY_HIGH
            );
    }


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

        if (presenceWakeEnabled) {
            rtc_gpio_deinit(PIR_PIN);
            pinMode(PIR_PIN, INPUT_PULLDOWN);
        }

        pinMode(
            MAGNET_SWITCH_PIN,
            INPUT_PULLUP
        );

        motionDiagnosticsResumePresenceInterrupt();

        return false;
    }

    shooterDeepSleepRtcMagic = 0;
    shooterDeepSleepScheduledWallUs = 0;

    uint64_t shooterDelayUs = 0;
    int64_t shooterScheduledWallUs = 0;
    uint8_t shooterWakeKind = SHOOTER_WAKE_NONE;

    if (continuousShooterNextWakeDelayUs(
            shooterDelayUs,
            shooterScheduledWallUs,
            shooterWakeKind
        ) &&
        shooterWakeKind == SHOOTER_WAKE_SAMPLE
    ) {
        esp_err_t timerErr =
            esp_sleep_enable_timer_wakeup(
                shooterDelayUs
            );

        if (timerErr == ESP_OK) {
            shooterDeepSleepRtcMagic =
                SHOOTER_DEEP_SLEEP_RTC_MAGIC;

            shooterDeepSleepScheduledWallUs =
                shooterScheduledWallUs;
        } else if (cfg_debug_enabled) {
            powerConsole(
                "Shooter deep-sleep timer setup failed | error=0x%x",
                timerErr
            );
        }
    }


    return true;
}


static bool configureLightSleepWakeSources()
{
    // Light sleep uses the dedicated RTC wake controllers:
    //
    //   EXT0 = presence / PIR / LD2410S OT2, active HIGH
    //   EXT1 = magnet / reed, active LOW
    //
    // Both Freenove pins are RTC GPIOs on ESP32-S3. Keeping the two signals on
    // separate RTC wake sources avoids the generic GPIO-wakeup re-arm path and
    // gives each input the polarity it actually needs.
    motionDiagnosticsSuspendPresenceInterrupt();

    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    // EXT0 requires RTC_PERIPH to remain powered. Keeping it ON also keeps the
    // internal pull-down/pull-up levels defined for both wake inputs.
    esp_sleep_pd_config(
        ESP_PD_DOMAIN_RTC_PERIPH,
        ESP_PD_OPTION_ON
    );

    rtc_gpio_hold_dis(PIR_PIN);
    rtc_gpio_hold_dis(MAGNET_SWITCH_PIN);

    const bool presenceWakeEnabled =
        cfg_motion_recording_enabled != 0;

    // Presence / PIR / OT2: active HIGH only while automatic motion recording
    // is enabled. Shooter-only mode therefore ignores presence wake events.
    esp_err_t presenceRtcErr = ESP_OK;

    if (presenceWakeEnabled) {
        presenceRtcErr = rtc_gpio_init(PIR_PIN);

        if (presenceRtcErr == ESP_OK) {
            presenceRtcErr =
                rtc_gpio_set_direction(
                    PIR_PIN,
                    RTC_GPIO_MODE_INPUT_ONLY
                );
        }

        if (presenceRtcErr == ESP_OK) {
            rtc_gpio_pullup_dis(PIR_PIN);
            rtc_gpio_pulldown_en(PIR_PIN);
        }
    }

    // Magnet / reed: active LOW.
    esp_err_t magnetRtcErr =
        rtc_gpio_init(MAGNET_SWITCH_PIN);

    if (magnetRtcErr == ESP_OK) {
        magnetRtcErr =
            rtc_gpio_set_direction(
                MAGNET_SWITCH_PIN,
                RTC_GPIO_MODE_INPUT_ONLY
            );
    }

    if (magnetRtcErr == ESP_OK) {
        rtc_gpio_pullup_en(MAGNET_SWITCH_PIN);
        rtc_gpio_pulldown_dis(MAGNET_SWITCH_PIN);
    }

    esp_err_t presenceErr = ESP_OK;
    esp_err_t magnetErr = ESP_FAIL;

    if (magnetRtcErr == ESP_OK) {
        if (presenceWakeEnabled && presenceRtcErr == ESP_OK) {
            // Dedicated RTC_IO wake: PIR/OT2 HIGH.
            presenceErr =
                esp_sleep_enable_ext0_wakeup(
                    PIR_PIN,
                    1
                );
        }

        // Dedicated RTC controller wake: magnet LOW.
        const uint64_t magnetMask =
            1ULL <<
            (uint32_t)MAGNET_SWITCH_PIN;

        magnetErr =
            esp_sleep_enable_ext1_wakeup(
                magnetMask,
                ESP_EXT1_WAKEUP_ANY_LOW
            );
    }

    if (
        presenceRtcErr != ESP_OK ||
        magnetRtcErr != ESP_OK ||
        presenceErr != ESP_OK ||
        magnetErr != ESP_OK
    ) {
        powerConsole(
            "Light sleep RTC wake setup failed | presence_rtc=0x%x | magnet_rtc=0x%x | ext0_presence=0x%x | ext1_magnet=0x%x",
            presenceRtcErr,
            magnetRtcErr,
            presenceErr,
            magnetErr
        );

        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT0
        );

        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT1
        );

        if (presenceWakeEnabled) {
            rtc_gpio_deinit(PIR_PIN);
            pinMode(PIR_PIN, INPUT_PULLDOWN);
        }
        rtc_gpio_deinit(MAGNET_SWITCH_PIN);

        pinMode(MAGNET_SWITCH_PIN, INPUT_PULLUP);

        motionDiagnosticsResumePresenceInterrupt();
        return false;
    }

    shooterLightSleepScheduledWallUs = 0;
    shooterLightSleepWakeKind = SHOOTER_WAKE_NONE;

    uint64_t shooterDelayUs = 0;
    int64_t shooterScheduledWallUs = 0;
    uint8_t shooterWakeKind = SHOOTER_WAKE_NONE;

    const bool shooterWakeRequired =
        cfg_shooter_enabled != 0;

    bool shooterWakeArmed =
        !shooterWakeRequired;

    bool shooterWakeAvailable =
        continuousShooterNextWakeDelayUs(
            shooterDelayUs,
            shooterScheduledWallUs,
            shooterWakeKind
        );

    if (shooterWakeAvailable) {
        esp_err_t timerErr =
            esp_sleep_enable_timer_wakeup(
                shooterDelayUs
            );

        if (timerErr == ESP_OK) {
            shooterLightSleepScheduledWallUs =
                shooterScheduledWallUs;
            shooterLightSleepWakeKind =
                shooterWakeKind;
            shooterWakeArmed = true;
        } else {
            powerConsole(
                "Light sleep blocked | shooter timer setup failed | error=0x%x",
                timerErr
            );
        }
    } else if (shooterWakeRequired) {
        powerConsole(
            "Light sleep blocked | shooter timer unavailable"
        );
    }

    // Fail safe: an enabled continuous shooter must never enter light sleep
    // unless its next sample/flush wake is definitely armed. Otherwise a
    // shooter-only installation without an active presence wake source could
    // sleep indefinitely after WiFi shuts down.
    if (!shooterWakeArmed) {
        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT0
        );
        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_EXT1
        );
        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_TIMER
        );

        if (presenceWakeEnabled) {
            rtc_gpio_deinit(PIR_PIN);
            pinMode(PIR_PIN, INPUT_PULLDOWN);
        }

        rtc_gpio_deinit(MAGNET_SWITCH_PIN);
        pinMode(MAGNET_SWITCH_PIN, INPUT_PULLUP);
        motionDiagnosticsResumePresenceInterrupt();

        shooterLightSleepScheduledWallUs = 0;
        shooterLightSleepWakeKind = SHOOTER_WAKE_NONE;
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


static bool enterStorageFaultLowPowerSleep(
    const char *reason
)
{
    // This path is intentionally independent of cfg_sleep_mode. A missing SD
    // disables recording anyway; remaining fully awake would only turn a
    // storage/contact fault into a battery-depletion fault.
    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_ALL
    );

    esp_sleep_pd_config(
        ESP_PD_DOMAIN_RTC_PERIPH,
        ESP_PD_OPTION_ON
    );

    // Service wake: magnet/reed LOW only. Presence/radar is deliberately not a
    // wake source while recording storage is unavailable.
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

    esp_err_t magnetErr =
        esp_sleep_enable_ext0_wakeup(
            MAGNET_SWITCH_PIN,
            0
        );

    esp_err_t timerErr =
        esp_sleep_enable_timer_wakeup(
            (uint64_t)STORAGE_FAULT_RETRY_SECONDS *
            1000000ULL
        );

    if (
        magnetErr != ESP_OK ||
        timerErr != ESP_OK
    ) {
        powerConsole(
            "STORAGE FAULT sleep setup failed | magnet=0x%x | timer=0x%x",
            magnetErr,
            timerErr
        );

        // Do not enter a sleep from which service/retry wake is uncertain.
        esp_sleep_disable_wakeup_source(
            ESP_SLEEP_WAKEUP_ALL
        );

        rtc_gpio_deinit(
            MAGNET_SWITCH_PIN
        );

        pinMode(
            MAGNET_SWITCH_PIN,
            INPUT_PULLUP
        );

        return false;
    }

    storageFaultRtcMagic =
        STORAGE_FAULT_RTC_MAGIC;

    if (storageFaultRtcCycle < UINT32_MAX)
        storageFaultRtcCycle++;

    powerConsole(
        "STORAGE FAULT low-power sleep | reason=%s | retry=%lu s | cycle=%lu | wake=timer OR magnet",
        reason ? reason : "SD unavailable",
        (unsigned long)STORAGE_FAULT_RETRY_SECONDS,
        (unsigned long)storageFaultRtcCycle
    );

    prepareCommonSleepState();

    // The filesystem is already unavailable, but force the backend into a
    // known quiescent state before cutting CPU power.
    logClose();
    STORAGE.end();
    sdReady = false;

#if defined(STORAGE_SPI)
    SPI.end();
    sdSpiDeselect();
#endif

    delay(100);
    esp_deep_sleep_start();

    // Defensive only: esp_deep_sleep_start() does not return on success.
    return true;
}


static bool tryEnterStorageFaultLowPower()
{
    if (sdReady)
        return false;

    if (recording)
        return false;

    // An operator actively using WebConfig must retain control. Once the normal
    // WiFi inactivity timeout shuts the UI/radio down, the next loop turn may
    // enter the storage-fault sleeper.
    if (
        webConfigStarted ||
        WiFi.getMode() != WIFI_OFF
    ) {
        return false;
    }

    // If the service magnet is already present, remain awake so the ordinary
    // magnet/WebConfig path can establish service access instead of immediately
    // sleeping on an already-active wake level.
    if (!magnetWakeIsClear())
        return false;

    return
        enterStorageFaultLowPowerSleep(
            "SD unavailable after recovery"
        );
}


static void enterDeepSleep()
{
    // PSRAM does not survive deep sleep. Preserve every accepted shooter frame
    // before the reboot-style sleep transition. If persistence cannot complete,
    // stay awake rather than knowingly discarding queued images.
    if (
        shooterBufferUsed > 0 &&
        !continuousShooterFlushBuffer("before_deep_sleep")
    ) {
        return;
    }

    if (!configureSleepWakeSources())
        return;


    if (cfg_motion_recording_enabled) {
        powerConsole(
            "Entering deep sleep | wake=presence GPIO%d HIGH OR magnet GPIO%d LOW%s",
            PIR_PIN,
            MAGNET_SWITCH_PIN,
            cfg_shooter_enabled ? " OR shooter timer" : ""
        );
    } else {
        powerConsole(
            "Entering deep sleep | wake=magnet GPIO%d LOW%s",
            MAGNET_SWITCH_PIN,
            cfg_shooter_enabled ? " OR shooter timer" : ""
        );
    }

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

    if (!configureLightSleepWakeSources())
        return false;


    const bool repetitiveShooterSleep =
        cfg_shooter_enabled &&
        shooterLightSleepWakeKind != SHOOTER_WAKE_NONE &&
        !cfg_debug_enabled;

    if (!repetitiveShooterSleep) {
        if (cfg_motion_recording_enabled) {
            powerConsole(
                "Entering light sleep | RTC wake=presence EXT0 GPIO%d HIGH OR magnet EXT1 GPIO%d LOW%s",
                PIR_PIN,
                MAGNET_SWITCH_PIN,
                cfg_shooter_enabled ? " OR shooter timer" : ""
            );
        } else {
            powerConsole(
                "Entering light sleep | RTC wake=magnet EXT1 GPIO%d LOW%s",
                MAGNET_SWITCH_PIN,
                cfg_shooter_enabled ? " OR shooter timer" : ""
            );
        }
    }

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


    // Execution continues here after a light-sleep wake. EXT0 is dedicated
    // to presence/PIR HIGH; EXT1 is dedicated to magnet LOW. The wake cause
    // therefore identifies the source directly without sampling a second
    // generic GPIO wake layer.
    esp_sleep_wakeup_cause_t wakeCause =
        esp_sleep_get_wakeup_cause();

    const bool wokeByPresence =
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT0;

    const bool wokeByMagnet =
        wakeCause ==
        ESP_SLEEP_WAKEUP_EXT1;

    const bool wokeByShooterTimer =
        wakeCause ==
        ESP_SLEEP_WAKEUP_TIMER;

    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_EXT0
    );

    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_EXT1
    );

    esp_sleep_disable_wakeup_source(
        ESP_SLEEP_WAKEUP_TIMER
    );

    // Magnet always returns to the normal awake GPIO backend.
    rtc_gpio_hold_dis(MAGNET_SWITCH_PIN);
    rtc_gpio_deinit(MAGNET_SWITCH_PIN);
    pinMode(MAGNET_SWITCH_PIN, INPUT_PULLUP);

    // Restore normal awake presence sampling. In Freenove offline-awake mode
    // the diagnostics layer intentionally keeps GPIO21 in RTC sampling; with
    // WebConfig active it returns to the normal digital GPIO backend.
    rtc_gpio_hold_dis(PIR_PIN);
    motionDiagnosticsResumePresenceInterrupt();


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


    // Preserve a real PIR/OT2 EXT0 wake immediately. A timer wake may coincide
    // with a rising presence signal; sample that physical input before doing any
    // shooter work so alarm recording still gets the same priority.
    if (
        wokeByPresence ||
        (
            wokeByShooterTimer &&
            digitalRead(PIR_PIN) == HIGH
        )
    ) {
        motionDiagnosticsNotePresenceTrigger();
    }

    if (wokeByShooterTimer) {
        // Drain the live radar/diagnostics once before classifying a simultaneous
        // TIMER + motion condition. This is RAM/UART work only; no SD/log delay.
        radarLoop();
        motionDiagnosticsLoop();
    }

    const bool timerWakeMotionActive =
        wokeByShooterTimer &&
        motionDetected();

    const bool recordingWakeFromSleep =
        wokeByPresence ||
        timerWakeMotionActive;

    const bool wakeCanStartRecording =
        configRecordingAllowedNow() &&
        !recordingSafetyCooldownActive();

    // Arm the end-to-end timing sample for every sleep wake that can immediately
    // lead to recording. A coincident TIMER + motion event is treated exactly
    // like a presence wake so shooter scheduling cannot add console latency.
    if (
        cfg_debug_enabled &&
        recordingWakeFromSleep &&
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

    // A recording-capable sleep wake is time critical. Do not touch USB/Serial
    // here: some hosts stall the first Serial.printf() for hundreds of
    // milliseconds after light sleep. This also covers a timer wake that lands
    // at the same moment as motion; the alarm path must remain faster than the
    // continuous shooter path.
    if (
        recordingWakeFromSleep &&
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
        const bool pureShooterTimerWake =
            wokeByShooterTimer &&
            !timerWakeMotionActive &&
            !wokeByMagnet;

        if (pureShooterTimerWake) {
            uint32_t sleptMs =
                (uint32_t)(sleptUs / 1000ULL);

            shooterSleepCyclesPending++;
            shooterSleepTotalMsPending += sleptMs;

            if (
                shooterSleepMinMsPending == 0 ||
                sleptMs < shooterSleepMinMsPending
            ) {
                shooterSleepMinMsPending = sleptMs;
            }

            if (sleptMs > shooterSleepMaxMsPending)
                shooterSleepMaxMsPending = sleptMs;

            // Keep per-cycle console diagnostics available only in debug mode.
            // Normal low-power operation avoids formatting/USB traffic twice a
            // second solely for repetitive shooter timer wakes.
            if (cfg_debug_enabled) {
                powerConsole(
                    "Wake from light sleep | cause=%s(%d) | slept=%llu ms | camera_wake=%llu us%s",
                    sleepWakeCauseName(wakeCause),
                    (int)wakeCause,
                    (unsigned long long)(sleptUs / 1000ULL),
                    (unsigned long long)(cameraWakeDoneUs - wakeStartUs),
                    fastCameraSleep
                        ? (cameraWakeOk ? "" : " FAILED")
                        : " (normal-init fallback)"
                );
            }
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
    }


    if (wokeByShooterTimer) {
        // A timer wake may be either a sample slot or a persistence deadline.
        // Motion/magnet always wins over shooter work.
        if (
            timerWakeMotionActive ||
            digitalRead(MAGNET_SWITCH_PIN) == LOW
        ) {
            if (shooterLightSleepWakeKind == SHOOTER_WAKE_SAMPLE) {
                continuousShooterMarkScheduledHandled(
                    shooterLightSleepScheduledWallUs
                );
            }
        } else if (shooterLightSleepWakeKind == SHOOTER_WAKE_FLUSH) {
            continuousShooterFlushBuffer("light_sleep_timeout");
        } else if (shooterLightSleepWakeKind == SHOOTER_WAKE_SAMPLE) {
            continuousShooterHandleScheduledWake(
                shooterLightSleepScheduledWallUs
            );
        }

        shooterLightSleepScheduledWallUs = 0;
        shooterLightSleepWakeKind = SHOOTER_WAKE_NONE;
    }


    if (wokeByMagnet) {

        // Light-sleep EXT1 wake detected the active-LOW magnet switch.
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


    // A pure shooter timer wake should return to light sleep immediately after
    // capture/flush. Presence or magnet wakes retain the ordinary idle delay.
    if (
        wokeByShooterTimer &&
        !timerWakeMotionActive &&
        !wokeByMagnet
    ) {
        sleepIdleSinceMs =
            millis() -
            (uint32_t)max(cfg_sleep_delay_ms, 0);
    } else {
        resetSleepDelayTimer();
    }

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


    // Storage failure has its own dedicated low-power path and is handled
    // before configured sleep. Keep this defensive guard in case a future
    // caller reaches this function directly while storage is unavailable.
    if (!sdReady) {

        setSleepDiagState(
            SLEEP_DIAG_SD,
            "Configured sleep bypassed | storage fault mode owns power state"
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


    // Reliability-first fallback for high-rate continuous capture. The
    // 250..1000 ms shooter range remains fully operational after WiFi turns
    // off, but stays awake instead of cycling the camera through light sleep
    // for every frame. This isolates unattended capture from the rapid
    // light-sleep wake path while preserving light sleep for slower shooters.
    if (
        cfg_sleep_mode == "light_sleep" &&
        cfg_shooter_enabled &&
        cfg_shooter_interval_ms >= 250 &&
        (uint32_t)cfg_shooter_interval_ms <=
            SHOOTER_LIGHT_SLEEP_AWAKE_MAX_INTERVAL_MS
    ) {
        static bool highRateShooterAwakeLogged = false;

        if (!highRateShooterAwakeLogged) {
            String message =
                "Light sleep bypassed | high-rate shooter awake fallback | interval_ms=" +
                String(cfg_shooter_interval_ms);

            powerConsole(
                "%s",
                message.c_str()
            );

            logWrite(message);
            highRateShooterAwakeLogged = true;
        }

        return false;
    }


    if (
        cfg_motion_recording_enabled &&
        !presenceWakeIsClear()
    ) {

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
#if defined(BOARD_FREENOVE)
    // GPIO21 is sampled through the normal digital GPIO path while WebConfig
    // is active. Offline-awake mode switches it to RTC GPIO below.
    motionDiagnosticsSetPresenceRtcSampling(false);
#endif

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
// MAGNET / REED WIFI ON SWITCH
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

    // Keep the physical motion input completely independent of the WiFi
    // lifecycle. On Freenove, GPIO21 is an RTC-capable input and the RTC path
    // is already proven by reliable EXT1 wake. Use that same hardware input
    // path while WiFi is OFF even when sleep_mode=off. This avoids losing
    // presence after the radio stack is torn down.
#if defined(BOARD_FREENOVE)
    offlinePresenceRtcBackendActive =
        motionDiagnosticsSetPresenceRtcSampling(true);
#else
    offlinePresenceRtcBackendActive = false;
    motionDiagnosticsResumePresenceInterrupt();
#endif

    MotionDiagnosticsSnapshot offlineBaseline;
    motionDiagnosticsGetSnapshot(offlineBaseline);
    offlinePresenceTriggerBaseline =
        offlineBaseline.presenceTriggerCount;
    offlinePresenceRawRiseBaseline =
        offlineBaseline.presenceRawRiseCount;
    offlinePresenceGlitchBaseline =
        offlineBaseline.presenceRejectedGlitchCount;
    offlineMotionBaselineValid = true;

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


void enableWifiByMagnet()
{
    // Magnet action is intentionally ON-only. The reed contact is a reliable
    // service/wake request, never a toggle: repeated passes with the magnet must
    // not accidentally turn WiFi/WebConfig off. Automatic inactivity shutdown
    // remains controlled exclusively by cfg_wifi_timeout_sec.
    if (webConfigStarted) {
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

        if (offlineMotionBaselineValid) {
            MotionDiagnosticsSnapshot offlineSummary;
            motionDiagnosticsGetSnapshot(offlineSummary);

            uint32_t offlinePresenceTriggers =
                offlineSummary.presenceTriggerCount -
                offlinePresenceTriggerBaseline;

            uint32_t offlineRawRises =
                offlineSummary.presenceRawRiseCount -
                offlinePresenceRawRiseBaseline;

            uint32_t offlineRejectedGlitches =
                offlineSummary.presenceRejectedGlitchCount -
                offlinePresenceGlitchBaseline;

            String summary =
                "MOTION OFFLINE SUMMARY | presence_triggers=" +
                String(offlinePresenceTriggers) +
                " | raw_rises=" +
                String(offlineRawRises) +
                " | rejected_glitches=" +
                String(offlineRejectedGlitches) +
                " | input_backend=" +
                String(offlinePresenceRtcBackendActive ? "rtc" : "digital");

            if (offlineSummary.presenceLastTriggerValid) {
                summary +=
                    " | last_presence_age_ms=" +
                    String(offlineSummary.presenceLastTriggerAgeMs);
            }

            logWrite(summary);
            offlineMotionBaselineValid = false;
            offlinePresenceRtcBackendActive = false;
        }

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
    // once before a later close can request WiFi ON.
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


        // Handle exactly once on the stable HIGH -> LOW edge. The action is ON-only;
        // when WiFi/WebConfig is already active this becomes a deliberate no-op.
        if (
            magnetStableState ==
            LOW
        ) {

            enableWifiByMagnet();
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


    if (radarSensorDetected()) {
        Serial.println(
            "LD2410S detected but fast motion tracking unavailable after retries: " +
            lastError +
            " - falling back to OT2"
        );

        logWrite(
            "LD2410S detected | fast motion tracking unavailable | OT2 fallback: " +
            lastError
        );
    } else {
        Serial.println(
            "LD2410S not detected after 4 attempts: " +
            lastError +
            " - motion sensor mode=PIR/digital input"
        );

        logWrite(
            "Motion sensor mode=PIR/digital input | LD2410S unavailable after retries: " +
            lastError
        );
    }


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

    const bool storageFaultTimerWake =
        resetReason == ESP_RST_DEEPSLEEP &&
        wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
        storageFaultRtcMagic == STORAGE_FAULT_RTC_MAGIC;

    const bool shooterTimerWake =
        resetReason == ESP_RST_DEEPSLEEP &&
        wakeCause == ESP_SLEEP_WAKEUP_TIMER &&
        shooterDeepSleepRtcMagic == SHOOTER_DEEP_SLEEP_RTC_MAGIC;

    const int64_t shooterWakeWallUs =
        shooterTimerWake
        ? shooterDeepSleepScheduledWallUs
        : 0;

    // Consume the shooter sleep marker exactly once. Presence/magnet wakes from
    // the same deep-sleep cycle must never leave a stale TIMER classification.
    shooterDeepSleepRtcMagic = 0;
    shooterDeepSleepScheduledWallUs = 0;

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

    if (storageFaultTimerWake) {
        // Storage-fault sleep configured the magnet pad as RTC EXT0 even though
        // this particular wake came from the timer. Restore it before normal
        // GPIO setup touches the pin.
        rtc_gpio_deinit(
            MAGNET_SWITCH_PIN
        );
    }

    if (shooterTimerWake) {
        // Normal deep sleep configured BOTH service inputs as RTC wake pads.
        // A timer wake leaves neither one as the reported wake cause, so restore
        // both explicitly before ordinary GPIO/diagnostic initialization.
        rtc_gpio_deinit(
            MAGNET_SWITCH_PIN
        );
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
        "Presence/PIR GPIO: %d\n",
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
    const uint8_t sdBootAttempts =
        storageFaultTimerWake
        ? SD_STORAGE_FAULT_WAKE_MAX_ATTEMPTS
        : SD_BOOT_MAX_ATTEMPTS;

    if (storageFaultTimerWake) {
        Serial.printf(
            "STORAGE FAULT timer wake | cycle=%lu | SD retry attempts=%u\n",
            (unsigned long)storageFaultRtcCycle,
            (unsigned)sdBootAttempts
        );
    }

    sdReady =
        initSDWithRetries(
            "boot",
            sdBootAttempts
        );


    if (sdReady) {
        if (storageFaultRtcMagic == STORAGE_FAULT_RTC_MAGIC) {
            Serial.println(
                "STORAGE FAULT recovered - normal operation restored"
            );
        }

        storageFaultClearRtcState();
    } else {

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

    bool skipSdFirmwareUpdateOnce =
        firmwareInfoConsumeSkipSdUpdateOnce();

    if (skipSdFirmwareUpdateOnce) {
        Serial.println(
            "Firmware update: SD auto-update skipped once after direct WiFi OTA"
        );
    }

    if (
        sdReady &&
        !skipSdFirmwareUpdateOnce
    ) {

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
    // Low-power / boot-loop protection
    //
    // Config must be known first because bootloop_protection is an explicit
    // operator switch. This still runs before license crypto provisioning,
    // camera, radar and WiFi startup. A depleted unit therefore avoids the
    // dominant high-load startup work once a short-boot streak is detected.
    // ---------------------------------------------------------

    if (bootloopProtectionHandleBoot(
            resetReason
        )) {
        // Protection deep sleep normally never returns.
        return;
    }


    // ---------------------------------------------------------
    // Periodic storage-fault retry fast path
    //
    // When the preceding storage-fault deep sleep woke only to retry SD, do
    // not start normal product subsystems if the card is still unavailable.
    // Active transport mode keeps precedence because it has its own timer-only
    // power policy. A magnet wake is not a timer wake and therefore always
    // reaches the normal service/WebConfig path.
    // ---------------------------------------------------------

    if (
        storageFaultTimerWake &&
        !sdReady &&
        cfg_transport_mode == 0 &&
        magnetWakeIsClear()
    ) {
        Serial.println(
            "STORAGE FAULT periodic retry failed - returning to low-power sleep"
        );

        if (enterStorageFaultLowPowerSleep(
                "periodic SD retry failed"
            )) {
            return;
        }

        Serial.println(
            "STORAGE FAULT low-power sleep unavailable - continuing normal boot"
        );
    }


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

    const bool radarTrackingReady =
        startRadarMotionTrackingRobust();

    if (radarSensorDetected()) {
        Serial.printf(
            "Motion sensor mode: LD2410S RADAR | fast_tracking=%s\n",
            radarTrackingReady ? "ready" : "fallback-to-OT2"
        );
        logWrite(
            String("Motion sensor mode=LD2410S radar | fast_tracking=") +
            (radarTrackingReady ? "ready" : "OT2-fallback")
        );
    } else {
        Serial.printf(
            "Motion sensor mode: PIR/digital input | GPIO=%d\n",
            PIR_PIN
        );
    }


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
                    "Presence/PIR startup guard active"
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
            "Presence input ready, state=%s\n",
            digitalRead(PIR_PIN) == HIGH
                ? "HIGH"
                : "LOW"
        );
    }

    // Start RAM-only motion diagnostics only after the normal PIR startup guard
    // has completed. A presence wake is recorded explicitly so a short pulse
    // remains visible later in WebConfig.
    motionDiagnosticsBegin(wokeByPresence);


    // With sleep explicitly disabled, keep the camera initialized continuously
    // so the first and all subsequent awake motion events avoid a full camera
    // cold-start. If WebConfig was started above, its preview init already did
    // this and the call is skipped. A failed eager init is non-fatal because
    // startRecording() retains its normal recovery/init path.
    if (
        cfg_sleep_mode == "off" &&
        !cameraInitialized
    ) {
        Serial.println(
            "Sleep off - initializing camera for fast motion start..."
        );

        if (!initCamera(
                cfg_camera,
                cfg_resolution,
                cfg_quality
            )) {
            logWrite(
                "Awake-ready camera init failed - recording will retry on trigger"
            );
        }
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

        if (recordingDecisionAllowsStart(true, "deep_sleep_presence")) {
            if (!startRecording()) {
                resetSleepDelayTimer();
            }
        } else {
            motionDiagnosticsConsumePresenceStartTrigger();
            resetSleepDelayTimer();
        }

    }

    else if (
        motionDetected()
    ) {

        // On a normal power-on, use the fast UART detector when LD2410S was
        // detected. Otherwise physicalMotionActive() uses the standalone
        // PIR/digital presence input automatically.

        Serial.println(
            "Motion active on normal boot"
        );

        logWrite(
            "Motion active on boot"
        );

        bool bootPhysicalMotion =
            physicalRecordingStartTriggerActive();

        if (recordingDecisionAllowsStart(bootPhysicalMotion, "normal_boot")) {
            if (!startRecording()) {
                resetSleepDelayTimer();
            }
        } else {
            if (bootPhysicalMotion)
                motionDiagnosticsConsumePresenceStartTrigger();
            resetSleepDelayTimer();
        }

    }

    else if (shooterTimerWake) {

        Serial.println(
            "Wake reason: CONTINUOUS SHOOTER"
        );

        continuousShooterHandleScheduledWake(
            shooterWakeWallUs
        );

        // Deep sleep reboots the device, so there is no retained PSRAM queue
        // from the previous cycle. Start the normal idle delay after capture.
        resetSleepDelayTimer();

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

    // A cold boot that survives the configured stabilization period is marked
    // healthy in NVS. This is intentionally tiny/non-blocking after arming.
    bootloopProtectionLoop();

    // Non-blocking recording/fault LED state.
    updateStatusLed();

    // Reed contact / test pushbutton.
    // Active only while the ESP32 is awake.
    handleMagnetSwitch();

    // Consume the LD2410S UART report stream. Fast radar decisions use the
    // gate-energy reports; PRESENCE_PIN remains the wake/fallback input.
    radarLoop();

    // RAM-only level/edge tracking for the WebConfig live motion display and
    // backup awake trigger. Raw GPIO HIGH remains authoritative. A trigger that
    // occurs while a recording is
    // already active belongs to that event; it must not queue a second event
    // immediately after finalization.
    motionDiagnosticsLoop();

    if (recording) {
        motionDiagnosticsConsumePresenceStartTrigger();
    }

    if (
        imageVerifyRejectedUntilMotionClear &&
        !physicalMotionActive()
    ) {
        imageVerifyRejectedUntilMotionClear = false;
    }

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
                emitDeferredWakeConsole("timeout");
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
            motionDiagnosticsPresenceActive() ? 1 : 0;

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


    // Service persistence before sampling. A due flush is harmless while the
    // recorder owns storage: it simply remains queued until the recorder stops.
    continuousShooterServiceFlush();

    // Keep the shooter clock serviced even while an alarm video is running.
    // The fixed slot is consumed before capture, so an overlapping alarm skips
    // that sample instead of shifting every later slot. Shooter work deliberately
    // does NOT reset the idle-sleep timer; at high sample rates this allows the
    // device to return to light sleep between timer wakes.
    continuousShooterServiceDue();


    // ---------------------------------------------------------
    // Nothing to record
    // ---------------------------------------------------------

    if (!recording) {

        if (!sdReady) {

            static unsigned long lastSdRetryMs = 0;

            const bool previewActive =
                webConfigStarted &&
                webConfigCameraPreviewActive();

            // Storage failure gets its own finite unattended service window,
            // even if the normal WiFi timeout is configured as unlimited or
            // much longer. An actively used browser keeps the session alive via
            // the existing heartbeat, so operator service is never cut off.
            if (
                webConfigStarted &&
                webConfigInactiveFor(
                    STORAGE_FAULT_WEB_IDLE_TIMEOUT_SECONDS
                )
            ) {
                stopWebConfigWifi(
                    "WiFi OFF: storage fault idle timeout"
                );
            }

            // Preserve the existing one-minute active recovery opportunity.
            // On the initial fault boot this overlaps the normal WebConfig/WiFi
            // service window. If recovery still fails and the UI/radio has gone
            // inactive, transition to periodic low-power retries instead of
            // remaining awake indefinitely.
            if (
                !previewActive &&
                millis() - lastSdRetryMs >
                SD_RECOVERY_RETRY_MS
            ) {

                lastSdRetryMs =
                    millis();

                recoverSD();
            }

            if (!sdReady) {
                if (tryEnterStorageFaultLowPower()) {
                    return;
                }

                delay(10);
                return;
            }
        }


        if (tryEnterConfiguredSleep()) {
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


        bool imageOnlyTrigger =
            imageOnlyIdleMotionTrigger();

        if (motionDetected() || imageOnlyTrigger) {

            bool physicalTrigger =
                physicalRecordingStartTriggerActive();
            bool simulatedTrigger = simulatedMotionActive();
            bool imageOnlyVerifiedPhysical = false;

            if (
                physicalTrigger &&
                !simulatedTrigger
            ) {
                if (imageOnlyModeActive()) {
                    // While already awake, the physical sensor is only a hint in
                    // image_only. Continuous camera scans decide the start. On
                    // the latency-sensitive post-sleep wake path, verify in one
                    // immediate burst so we do not wait for the 250-ms poll.
                    if (!imageOnlyTrigger) {
                        if (wakeCriticalPathActive) {
                            if (!recordingDecisionAllowsStart(
                                    true,
                                    "awake_trigger"
                                )) {
                                motionDiagnosticsConsumePresenceStartTrigger();
                                resetSleepDelayTimer();
                                finishDeferredWakeWithoutRecording("image-rejected");
                                delay(10);
                                return;
                            }

                            imageOnlyVerifiedPhysical = true;
                        } else {
                            resetSleepDelayTimer();
                            delay(10);
                            return;
                        }
                    }
                } else if (!recordingDecisionAllowsStart(
                               true,
                               "awake_trigger"
                           )) {
                    motionDiagnosticsConsumePresenceStartTrigger();
                    resetSleepDelayTimer();
                    finishDeferredWakeWithoutRecording("image-rejected");
                    delay(10);
                    return;
                }
            }

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
                imageOnlyTrigger ||
                imageOnlyVerifiedPhysical
            ) {
                motionMessage = "image motion";
            } else if (
                simulatedTrigger &&
                !physicalTrigger
            ) {
                motionMessage = "simulated";
            } else if (
                radarMotionTrackingAvailable() &&
                radarMotionActive()
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
            } else if (
                radarSensorDetected()
            ) {
                motionMessage = "OT2 edge/fallback";
            } else {
                motionMessage = "PIR/digital input";
            }

            if (wakeCriticalPathActive) {
                deferredMotionConsoleMessage = motionMessage;
                deferredMotionConsolePending = true;
            }

            if (
                wakeTimingActive &&
                wakeTimingMotionLogDoneUs == 0
            ) {
                wakeTimingMotionLogDoneUs =
                    (uint64_t)esp_timer_get_time();
            }

            const bool presenceStartSource =
                !simulatedTrigger &&
                !imageOnlyTrigger &&
                (
                    motionDiagnosticsPresenceActive() ||
                    motionDiagnosticsPresenceStartPending(
                        AWAKE_PRESENCE_TRIGGER_PENDING_MAX_MS
                    )
                );

            if (presenceStartSource) {
                captureRecordingStartPresenceDiagnostics();
            } else {
                clearRecordingStartPresenceDiagnostics();
            }

            bool recordingStarted =
                startRecording();

            if (
                recordingStarted &&
                !wakeCriticalPathActive
            ) {
                // Serial output must not sit in front of recorder initialization.
                consoleWrite(
                    "MOTION",
                    motionMessage
                );
            }

            if (!recordingStarted) {
                // No first frame will arrive to release the deferred console
                // messages. Restore normal diagnostics immediately on failure.
                if (wakeCriticalPathActive) {
                    wakeCriticalPathActive = false;

                    if (deferredWakeConsolePending) {
                        emitDeferredWakeConsole(nullptr);
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

    if (recordingContinuationMotionActive()) {

        // direct/image_verify use the physical sensor state. image_only is fed
        // from the JPEGs already written by the recorder, so its configured
        // release_frames own the motion state before the normal post_record_ms
        // timer begins.

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

        uint64_t frameCallStartUs =
            (uint64_t)esp_timer_get_time();

        if (wakeTimingActive) {
            wakeTimingFrameAttempts++;
        }

        recorderAddFrame();

        uint64_t frameCallDoneUs =
            (uint64_t)esp_timer_get_time();

        uint32_t frameCallDurationUs =
            (uint32_t)(
                frameCallDoneUs -
                frameCallStartUs
            );

        recordingPerformanceNoteCall(
            recordingPerformanceSegment,
            frameCallDurationUs,
            frameIntervalUs
        );

        recordingPerformanceNoteCall(
            recordingPerformanceEvent,
            frameCallDurationUs,
            frameIntervalUs
        );

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
                    emitDeferredWakeConsole(nullptr);
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
