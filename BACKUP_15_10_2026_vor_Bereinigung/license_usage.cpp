#include "license_usage.h"

#include <Preferences.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

namespace {

static const char USAGE_NVS_NAMESPACE[] = "sflusage";
static const char USAGE_SLOT_A_KEY[] = "a";
static const char USAGE_SLOT_B_KEY[] = "b";

static const uint32_t USAGE_MAGIC = 0x31554653UL; // "SFU1" little-endian
static const uint16_t USAGE_FORMAT_VERSION = 3U;
static const int64_t MIN_TRUSTED_EPOCH = 1609459200LL; // 2021-01-01 UTC

// Uptime is informational for now and deliberately approximate. Keep RTC-RAM
// bookkeeping cheap and flush to NVS only after six accumulated awake hours,
// or piggy-back it onto another usage-state write.
static const uint64_t UPTIME_PERIODIC_ACCOUNT_US = 10000000ULL; // 10 s
static const uint64_t UPTIME_NVS_FLUSH_SECONDS = 6ULL * 60ULL * 60ULL;
static const uint64_t UPTIME_NVS_RETRY_US = 10ULL * 60ULL * 1000000ULL;
static const uint32_t UPTIME_RTC_MAGIC = 0x32505553UL; // "SUP2"

// Streaming accounting is intentionally coarse. Active time is accumulated in
// RTC-retained RAM and only persisted every 30 minutes of stream wall time, or
// piggy-backed onto another usage-state write. A hard power loss can therefore
// lose at most roughly one checkpoint interval, which is acceptable for trial
// metering and keeps NVS wear low.
static const uint64_t STREAM_PERIODIC_ACCOUNT_US = 10000000ULL; // 10 s
static const uint64_t STREAM_NVS_FLUSH_SECONDS = 30ULL * 60ULL;
static const uint64_t STREAM_NVS_RETRY_US = 10ULL * 60ULL * 1000000ULL;
static const uint32_t STREAM_RTC_MAGIC = 0x31525453UL; // "STR1"

#pragma pack(push, 1)
struct StoredUsageStateV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint64_t recordedSecondsTotal;
    uint32_t recordingsTotal;
    uint32_t interruptedRecordingsTotal;
    int64_t lastTrustedEpoch;
    uint8_t recordingActive;
    uint8_t reserved[7];
    uint32_t crc32;
};

struct StoredUsageStateV2 {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint64_t recordedSecondsTotal;
    uint64_t systemUptimeSecondsTotal;
    uint32_t recordingsTotal;
    uint32_t interruptedRecordingsTotal;
    int64_t lastTrustedEpoch;
    uint8_t recordingActive;
    uint8_t reserved[7];
    uint32_t crc32;
};

struct StoredUsageState {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;
    uint64_t recordedSecondsTotal;
    uint64_t streamedSecondsTotal;
    uint64_t systemUptimeSecondsTotal;
    uint32_t recordingsTotal;
    uint32_t interruptedRecordingsTotal;
    int64_t lastTrustedEpoch;
    uint8_t recordingActive;
    uint8_t reserved[7];
    uint32_t crc32;
};
#pragma pack(pop)

static StoredUsageState state = {};
static bool initialized = false;
static bool storageHealthy = true;
static bool integrityError = false;
static bool nextWriteSlotA = true;

static bool runtimeRecordingActive = false;
static uint64_t runtimeRecordingStartUs = 0;
static uint64_t runtimeCommittedSeconds = 0;

static bool runtimeStreamingActive = false;
static uint64_t runtimeStreamingLastAccountUs = 0;
static uint64_t runtimeStreamingLastFlushAttemptUs = 0;

// Survives deep sleep and software reset, but not a true power loss. That is
// intentional: a few lost minutes/hours of informational uptime are preferable
// to frequent NVS writes. Filming counters remain separately persisted at safe
// recording boundaries.
RTC_DATA_ATTR static uint32_t rtcUptimeMagic = 0;
RTC_DATA_ATTR static uint64_t rtcUptimeCarrySeconds = 0;
RTC_DATA_ATTR static uint32_t rtcStreamingMagic = 0;
RTC_DATA_ATTR static uint64_t rtcStreamingCarrySeconds = 0;
static uint64_t runtimeUptimeLastAccountUs = 0;
static uint64_t runtimeUptimeLastFlushAttemptUs = 0;

static uint64_t saturatingAddU64(
    uint64_t value,
    uint64_t increment
)
{
    if (UINT64_MAX - value < increment)
        return UINT64_MAX;

    return value + increment;
}

static uint32_t saturatingIncrementU32(
    uint32_t value
)
{
    return
        value == UINT32_MAX
        ? UINT32_MAX
        : value + 1U;
}

static uint32_t crc32Update(
    uint32_t crc,
    const uint8_t *data,
    size_t length
)
{
    crc = ~crc;

    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];

        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask =
                0U - (crc & 1U);

            crc =
                (crc >> 1) ^
                (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static uint32_t stateCrc(
    const StoredUsageState &value
)
{
    return crc32Update(
        0U,
        reinterpret_cast<const uint8_t *>(&value),
        offsetof(StoredUsageState, crc32)
    );
}

static uint32_t stateV1Crc(
    const StoredUsageStateV1 &value
)
{
    return crc32Update(
        0U,
        reinterpret_cast<const uint8_t *>(&value),
        offsetof(StoredUsageStateV1, crc32)
    );
}

static uint32_t stateV2Crc(
    const StoredUsageStateV2 &value
)
{
    return crc32Update(
        0U,
        reinterpret_cast<const uint8_t *>(&value),
        offsetof(StoredUsageStateV2, crc32)
    );
}

static void initializeEmptyState()
{
    memset(&state, 0, sizeof(state));
    state.magic = USAGE_MAGIC;
    state.version = USAGE_FORMAT_VERSION;
    state.size = (uint16_t)sizeof(StoredUsageState);
    state.recordingActive = 0;
    state.crc32 = stateCrc(state);
}

static bool stateValid(
    const StoredUsageState &value
)
{
    if (
        value.magic != USAGE_MAGIC ||
        value.version != USAGE_FORMAT_VERSION ||
        value.size != sizeof(StoredUsageState) ||
        value.recordingActive > 1U
    ) {
        return false;
    }

    for (size_t i = 0; i < sizeof(value.reserved); ++i) {
        if (value.reserved[i] != 0U)
            return false;
    }

    return value.crc32 == stateCrc(value);
}

static bool stateV1Valid(
    const StoredUsageStateV1 &value
)
{
    if (
        value.magic != USAGE_MAGIC ||
        value.version != 1U ||
        value.size != sizeof(StoredUsageStateV1) ||
        value.recordingActive > 1U
    ) {
        return false;
    }

    for (size_t i = 0; i < sizeof(value.reserved); ++i) {
        if (value.reserved[i] != 0U)
            return false;
    }

    return value.crc32 == stateV1Crc(value);
}

static bool stateV2Valid(
    const StoredUsageStateV2 &value
)
{
    if (
        value.magic != USAGE_MAGIC ||
        value.version != 2U ||
        value.size != sizeof(StoredUsageStateV2) ||
        value.recordingActive > 1U
    ) {
        return false;
    }

    for (size_t i = 0; i < sizeof(value.reserved); ++i) {
        if (value.reserved[i] != 0U)
            return false;
    }

    return value.crc32 == stateV2Crc(value);
}

static StoredUsageState migrateV1(
    const StoredUsageStateV1 &oldState
)
{
    StoredUsageState migrated = {};
    migrated.magic = USAGE_MAGIC;
    migrated.version = USAGE_FORMAT_VERSION;
    migrated.size = (uint16_t)sizeof(StoredUsageState);
    migrated.generation = oldState.generation;
    migrated.recordedSecondsTotal = oldState.recordedSecondsTotal;
    migrated.streamedSecondsTotal = 0;
    migrated.systemUptimeSecondsTotal = 0;
    migrated.recordingsTotal = oldState.recordingsTotal;
    migrated.interruptedRecordingsTotal = oldState.interruptedRecordingsTotal;
    migrated.lastTrustedEpoch = oldState.lastTrustedEpoch;
    migrated.recordingActive = oldState.recordingActive;
    memset(migrated.reserved, 0, sizeof(migrated.reserved));
    migrated.crc32 = stateCrc(migrated);
    return migrated;
}

static StoredUsageState migrateV2(
    const StoredUsageStateV2 &oldState
)
{
    StoredUsageState migrated = {};
    migrated.magic = USAGE_MAGIC;
    migrated.version = USAGE_FORMAT_VERSION;
    migrated.size = (uint16_t)sizeof(StoredUsageState);
    migrated.generation = oldState.generation;
    migrated.recordedSecondsTotal = oldState.recordedSecondsTotal;
    migrated.streamedSecondsTotal = 0;
    migrated.systemUptimeSecondsTotal = oldState.systemUptimeSecondsTotal;
    migrated.recordingsTotal = oldState.recordingsTotal;
    migrated.interruptedRecordingsTotal = oldState.interruptedRecordingsTotal;
    migrated.lastTrustedEpoch = oldState.lastTrustedEpoch;
    migrated.recordingActive = oldState.recordingActive;
    memset(migrated.reserved, 0, sizeof(migrated.reserved));
    migrated.crc32 = stateCrc(migrated);
    return migrated;
}

static bool generationNewer(
    uint32_t candidate,
    uint32_t reference
)
{
    return (int32_t)(candidate - reference) > 0;
}

static bool readSlot(
    Preferences &prefs,
    const char *key,
    StoredUsageState &output,
    bool &present
)
{
    present = false;
    memset(&output, 0, sizeof(output));

    size_t length =
        prefs.getBytesLength(key);

    if (length == 0)
        return true;

    present = true;

    if (length == sizeof(StoredUsageState)) {
        StoredUsageState current = {};
        size_t read =
            prefs.getBytes(
                key,
                &current,
                sizeof(current)
            );

        if (
            read != sizeof(current) ||
            !stateValid(current)
        ) {
            return false;
        }

        output = current;
        return true;
    }

    // Beta 26 used format V2. Preserve all existing counters and introduce
    // streaming time at zero without resetting filming or uptime.
    if (length == sizeof(StoredUsageStateV2)) {
        StoredUsageStateV2 oldState = {};
        size_t read =
            prefs.getBytes(
                key,
                &oldState,
                sizeof(oldState)
            );

        if (
            read != sizeof(oldState) ||
            !stateV2Valid(oldState)
        ) {
            return false;
        }

        output = migrateV2(oldState);
        return true;
    }

    // Beta 25 used format V1. Preserve every existing filming/time counter and
    // transparently introduce the newer uptime/streaming fields.
    if (length == sizeof(StoredUsageStateV1)) {
        StoredUsageStateV1 oldState = {};
        size_t read =
            prefs.getBytes(
                key,
                &oldState,
                sizeof(oldState)
            );

        if (
            read != sizeof(oldState) ||
            !stateV1Valid(oldState)
        ) {
            return false;
        }

        output = migrateV1(oldState);
        return true;
    }

    return false;
}

static void accountRuntimeUptimeToRtc(
    bool force
)
{
    if (!initialized)
        return;

    const uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    if (nowUs < runtimeUptimeLastAccountUs) {
        runtimeUptimeLastAccountUs = nowUs;
        return;
    }

    const uint64_t elapsedUs =
        nowUs - runtimeUptimeLastAccountUs;

    if (!force && elapsedUs < UPTIME_PERIODIC_ACCOUNT_US)
        return;

    const uint64_t wholeSeconds =
        elapsedUs / 1000000ULL;

    if (wholeSeconds == 0)
        return;

    rtcUptimeCarrySeconds =
        saturatingAddU64(
            rtcUptimeCarrySeconds,
            wholeSeconds
        );

    // Keep any sub-second remainder for the next account pass.
    runtimeUptimeLastAccountUs +=
        wholeSeconds * 1000000ULL;
}

static void accountRuntimeStreamingToRtc(
    bool force
)
{
    if (!initialized || !runtimeStreamingActive)
        return;

    const uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    if (nowUs < runtimeStreamingLastAccountUs) {
        runtimeStreamingLastAccountUs = nowUs;
        return;
    }

    const uint64_t elapsedUs =
        nowUs - runtimeStreamingLastAccountUs;

    if (!force && elapsedUs < STREAM_PERIODIC_ACCOUNT_US)
        return;

    const uint64_t wholeSeconds =
        elapsedUs / 1000000ULL;

    if (wholeSeconds == 0)
        return;

    rtcStreamingCarrySeconds =
        saturatingAddU64(
            rtcStreamingCarrySeconds,
            wholeSeconds
        );

    runtimeStreamingLastAccountUs +=
        wholeSeconds * 1000000ULL;
}

static bool writeState()
{
    if (!initialized)
        return false;

    accountRuntimeUptimeToRtc(true);
    accountRuntimeStreamingToRtc(true);

    StoredUsageState candidate = state;
    candidate.streamedSecondsTotal =
        saturatingAddU64(
            candidate.streamedSecondsTotal,
            rtcStreamingCarrySeconds
        );
    candidate.systemUptimeSecondsTotal =
        saturatingAddU64(
            candidate.systemUptimeSecondsTotal,
            rtcUptimeCarrySeconds
        );
    candidate.generation++;
    candidate.magic = USAGE_MAGIC;
    candidate.version = USAGE_FORMAT_VERSION;
    candidate.size = (uint16_t)sizeof(StoredUsageState);
    memset(candidate.reserved, 0, sizeof(candidate.reserved));
    candidate.crc32 = stateCrc(candidate);

    Preferences prefs;

    if (!prefs.begin(
            USAGE_NVS_NAMESPACE,
            false
        )) {
        storageHealthy = false;
        return false;
    }

    const char *key =
        nextWriteSlotA
        ? USAGE_SLOT_A_KEY
        : USAGE_SLOT_B_KEY;

    size_t written =
        prefs.putBytes(
            key,
            &candidate,
            sizeof(candidate)
        );

    prefs.end();

    if (written != sizeof(candidate)) {
        storageHealthy = false;
        return false;
    }

    state = candidate;
    rtcUptimeCarrySeconds = 0;
    rtcStreamingCarrySeconds = 0;
    nextWriteSlotA = !nextWriteSlotA;
    storageHealthy = true;
    return true;
}

static bool commitRecordingSeconds(
    uint64_t targetSessionSeconds,
    bool closeRecording
)
{
    if (!runtimeRecordingActive)
        return false;

    if (targetSessionSeconds < runtimeCommittedSeconds)
        targetSessionSeconds = runtimeCommittedSeconds;

    uint64_t delta =
        targetSessionSeconds - runtimeCommittedSeconds;

    StoredUsageState previous = state;

    if (delta > 0) {
        state.recordedSecondsTotal =
            saturatingAddU64(
                state.recordedSecondsTotal,
                delta
            );
    }

    if (closeRecording)
        state.recordingActive = 0;

    if (delta == 0 && !closeRecording)
        return true;

    if (!writeState()) {
        state = previous;
        return false;
    }

    runtimeCommittedSeconds =
        targetSessionSeconds;

    return true;
}

} // namespace

void licenseUsageBegin()
{
    if (initialized)
        return;

    initializeEmptyState();
    initialized = true;
    storageHealthy = true;
    integrityError = false;

    if (rtcUptimeMagic != UPTIME_RTC_MAGIC) {
        rtcUptimeMagic = UPTIME_RTC_MAGIC;
        rtcUptimeCarrySeconds = 0;
    }

    if (rtcStreamingMagic != STREAM_RTC_MAGIC) {
        rtcStreamingMagic = STREAM_RTC_MAGIC;
        rtcStreamingCarrySeconds = 0;
    }

    // Count this boot from reset onward, including setup time before this module
    // is initialized. The monotonic timer starts at zero on each boot.
    runtimeUptimeLastAccountUs = 0;
    runtimeUptimeLastFlushAttemptUs = 0;
    runtimeStreamingActive = false;
    runtimeStreamingLastAccountUs = 0;
    runtimeStreamingLastFlushAttemptUs = 0;

    Preferences prefs;

    if (!prefs.begin(
            USAGE_NVS_NAMESPACE,
            true
        )) {
        storageHealthy = false;
        return;
    }

    StoredUsageState slotA = {};
    StoredUsageState slotB = {};
    bool slotAPresent = false;
    bool slotBPresent = false;

    bool slotAValid =
        readSlot(
            prefs,
            USAGE_SLOT_A_KEY,
            slotA,
            slotAPresent
        );

    bool slotBValid =
        readSlot(
            prefs,
            USAGE_SLOT_B_KEY,
            slotB,
            slotBPresent
        );

    prefs.end();

    if (slotAValid && slotAPresent && slotBValid && slotBPresent) {
        if (generationNewer(
                slotB.generation,
                slotA.generation
            )) {
            state = slotB;
            nextWriteSlotA = true;
        } else {
            state = slotA;
            nextWriteSlotA = false;
        }
    } else if (slotAValid && slotAPresent) {
        state = slotA;
        nextWriteSlotA = false;

        if (slotBPresent && !slotBValid)
            integrityError = true;
    } else if (slotBValid && slotBPresent) {
        state = slotB;
        nextWriteSlotA = true;

        if (slotAPresent && !slotAValid)
            integrityError = true;
    } else {
        if (slotAPresent || slotBPresent) {
            integrityError = true;
            storageHealthy = false;
        }

        initializeEmptyState();
        nextWriteSlotA = true;
    }

    runtimeRecordingActive = false;
    runtimeRecordingStartUs = 0;
    runtimeCommittedSeconds = 0;

    // A previous boot that died while a recording was open already counted the
    // recording event at start. Preserve that count, note the interruption and
    // clear only the in-progress marker. No duration is invented because an
    // RTC may be absent and the monotonic timer does not survive a power loss.
    if (state.recordingActive != 0U) {
        StoredUsageState previous = state;

        state.interruptedRecordingsTotal =
            saturatingIncrementU32(
                state.interruptedRecordingsTotal
            );

        state.recordingActive = 0;

        if (!writeState()) {
            state = previous;
            storageHealthy = false;
        }
    }
}

void licenseUsagePeriodic()
{
    if (!initialized)
        licenseUsageBegin();

    accountRuntimeUptimeToRtc(false);
    accountRuntimeStreamingToRtc(false);

    // Thirty minutes of active stream wall time is the coarse checkpoint.
    // This is independent of frame rate/client count and therefore essentially
    // free in the media path. Other usage writes piggy-back the carry earlier.
    if (rtcStreamingCarrySeconds >= STREAM_NVS_FLUSH_SECONDS) {
        const uint64_t nowUs =
            (uint64_t)esp_timer_get_time();

        if (
            runtimeStreamingLastFlushAttemptUs == 0 ||
            nowUs < runtimeStreamingLastFlushAttemptUs ||
            nowUs - runtimeStreamingLastFlushAttemptUs >= STREAM_NVS_RETRY_US
        ) {
            runtimeStreamingLastFlushAttemptUs = nowUs;
            writeState();
        }
    }

    // Six-hour threshold keeps NVS wear extremely low. Other normal usage
    // writes (recording stop/checkpoint or trusted-time update) flush the carry
    // earlier automatically at no additional write cost.
    if (
        rtcUptimeCarrySeconds >=
        UPTIME_NVS_FLUSH_SECONDS
    ) {
        const uint64_t nowUs =
            (uint64_t)esp_timer_get_time();

        if (
            runtimeUptimeLastFlushAttemptUs == 0 ||
            nowUs < runtimeUptimeLastFlushAttemptUs ||
            nowUs - runtimeUptimeLastFlushAttemptUs >= UPTIME_NVS_RETRY_US
        ) {
            runtimeUptimeLastFlushAttemptUs = nowUs;
            writeState();
        }
    }
}

bool licenseUsageObserveTrustedTimeNow()
{
    if (!initialized)
        licenseUsageBegin();

    time_t now =
        time(nullptr);

    if ((int64_t)now < MIN_TRUSTED_EPOCH)
        return false;

    int64_t observed =
        (int64_t)now;

    if (observed <= state.lastTrustedEpoch)
        return true;

    // SensorForge may reboot/wake from deep sleep very frequently. Trial
    // expiry is day-based, so persisting the time anchor more than once per
    // UTC day would add NVS wear without improving the licensing decision.
    if (
        state.lastTrustedEpoch >= MIN_TRUSTED_EPOCH &&
        (uint64_t)observed / 86400ULL ==
            (uint64_t)state.lastTrustedEpoch / 86400ULL
    ) {
        return true;
    }

    StoredUsageState previous = state;
    state.lastTrustedEpoch = observed;

    if (!writeState()) {
        state = previous;
        return false;
    }

    return true;
}

void licenseUsageRecordingStarted()
{
    if (!initialized)
        licenseUsageBegin();

    if (runtimeRecordingActive)
        return;

    // Deliberately RAM-only: do not place an NVS/flash write between a
    // successful recorderStart() and the first media frames. The count and
    // active marker are persisted at the next safe segment boundary or stop.
    state.recordingsTotal =
        saturatingIncrementU32(
            state.recordingsTotal
        );

    state.recordingActive = 1U;

    runtimeRecordingActive = true;
    runtimeRecordingStartUs =
        (uint64_t)esp_timer_get_time();
    runtimeCommittedSeconds = 0;
}

bool licenseUsageRecordingCheckpoint()
{
    if (!runtimeRecordingActive)
        return true;

    uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    uint64_t elapsedSeconds =
        nowUs >= runtimeRecordingStartUs
        ? (nowUs - runtimeRecordingStartUs) / 1000000ULL
        : runtimeCommittedSeconds;

    return commitRecordingSeconds(
        elapsedSeconds,
        false
    );
}

bool licenseUsageRecordingStopped()
{
    if (!runtimeRecordingActive)
        return true;

    uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    uint64_t elapsedUs =
        nowUs >= runtimeRecordingStartUs
        ? nowUs - runtimeRecordingStartUs
        : 0ULL;

    // Round the final event duration up to the next full second. Trial policy
    // later works in seconds; rounding up prevents systematic undercounting of
    // many short recordings while adding at most one second per event.
    uint64_t finalSessionSeconds =
        (elapsedUs + 999999ULL) /
        1000000ULL;

    bool persisted =
        commitRecordingSeconds(
            finalSessionSeconds,
            true
        );

    runtimeRecordingActive = false;
    runtimeRecordingStartUs = 0;
    runtimeCommittedSeconds = 0;

    return persisted;
}

void licenseUsageStreamingSetActive(
    bool active
)
{
    if (!initialized)
        licenseUsageBegin();

    if (active == runtimeStreamingActive)
        return;

    if (runtimeStreamingActive)
        accountRuntimeStreamingToRtc(true);

    runtimeStreamingActive = active;
    runtimeStreamingLastAccountUs =
        active
        ? (uint64_t)esp_timer_get_time()
        : 0ULL;
}

LicenseUsageSnapshot licenseUsageSnapshot()
{
    if (!initialized)
        licenseUsageBegin();

    accountRuntimeUptimeToRtc(true);
    accountRuntimeStreamingToRtc(true);

    LicenseUsageSnapshot snapshot = {};
    snapshot.recordedSecondsTotal =
        state.recordedSecondsTotal;
    snapshot.recordedSecondsEffectiveTotal =
        state.recordedSecondsTotal;

    if (runtimeRecordingActive) {
        const uint64_t nowUs =
            (uint64_t)esp_timer_get_time();

        const uint64_t elapsedSeconds =
            nowUs >= runtimeRecordingStartUs
            ? (nowUs - runtimeRecordingStartUs) / 1000000ULL
            : runtimeCommittedSeconds;

        if (elapsedSeconds > runtimeCommittedSeconds) {
            snapshot.recordedSecondsEffectiveTotal =
                saturatingAddU64(
                    snapshot.recordedSecondsEffectiveTotal,
                    elapsedSeconds - runtimeCommittedSeconds
                );
        }
    }

    snapshot.streamedSecondsTotal =
        state.streamedSecondsTotal;
    snapshot.streamedSecondsEffectiveTotal =
        saturatingAddU64(
            state.streamedSecondsTotal,
            rtcStreamingCarrySeconds
        );
    snapshot.systemUptimeSecondsTotal =
        saturatingAddU64(
            state.systemUptimeSecondsTotal,
            rtcUptimeCarrySeconds
        );
    snapshot.recordingsTotal =
        state.recordingsTotal;
    snapshot.interruptedRecordingsTotal =
        state.interruptedRecordingsTotal;
    snapshot.lastTrustedEpoch =
        state.lastTrustedEpoch;
    snapshot.recordingActive =
        state.recordingActive != 0U ||
        runtimeRecordingActive;
    snapshot.streamingActive =
        runtimeStreamingActive;
    snapshot.storageHealthy =
        storageHealthy;
    snapshot.integrityError =
        integrityError;

    return snapshot;
}

String licenseUsageDiagnosticSummary()
{
    LicenseUsageSnapshot snapshot =
        licenseUsageSnapshot();

    String text =
        "recordings=" +
        String((unsigned long)snapshot.recordingsTotal) +
        " | recorded_seconds=" +
        String((unsigned long long)snapshot.recordedSecondsTotal) +
        " | recorded_effective_seconds=" +
        String((unsigned long long)snapshot.recordedSecondsEffectiveTotal) +
        " | streamed_seconds=" +
        String((unsigned long long)snapshot.streamedSecondsTotal) +
        " | streamed_effective_seconds=" +
        String((unsigned long long)snapshot.streamedSecondsEffectiveTotal) +
        " | uptime_seconds=" +
        String((unsigned long long)snapshot.systemUptimeSecondsTotal) +
        " | interrupted=" +
        String((unsigned long)snapshot.interruptedRecordingsTotal) +
        " | trusted_epoch=" +
        String((long long)snapshot.lastTrustedEpoch) +
        " | recording_active=" +
        String(snapshot.recordingActive ? 1 : 0) +
        " | streaming_active=" +
        String(snapshot.streamingActive ? 1 : 0) +
        " | nvs=" +
        String(snapshot.storageHealthy ? "ok" : "error") +
        " | integrity=" +
        String(snapshot.integrityError ? "error" : "ok");

    return text;
}
