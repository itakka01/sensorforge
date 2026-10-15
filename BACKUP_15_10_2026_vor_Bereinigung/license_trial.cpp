#include "license_trial.h"

#include "license_usage.h"

#include <Preferences.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

namespace {

static const char TRIAL_NVS_NAMESPACE[] = "sfltrial";
static const char TRIAL_SLOT_A_KEY[] = "a";
static const char TRIAL_SLOT_B_KEY[] = "b";
static const uint32_t TRIAL_MAGIC = 0x31544653UL; // "SFT1" little-endian
static const uint16_t TRIAL_FORMAT_VERSION = 1U;
static const int64_t MIN_TRUSTED_EPOCH = 1609459200LL; // 2021-01-01 UTC

#pragma pack(push, 1)
struct StoredTrialState {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t generation;

    uint64_t trialStartRecordedSeconds;
    uint64_t trialStartStreamedSeconds;
    uint64_t trialStartUptimeSeconds;
    int64_t trialStartTrustedEpoch;

    uint32_t demoDayKey;
    uint32_t demoDayRecordingsBase;
    uint64_t demoDayRecordedSecondsBase;
    uint64_t demoDayStreamedSecondsBase;

    uint8_t initialized;
    uint8_t demoDayInitialized;
    uint8_t reserved[6];
    uint32_t crc32;
};
#pragma pack(pop)

static StoredTrialState state = {};
static bool initialized = false;
static bool storageHealthy = true;
static bool integrityError = false;
static bool nextWriteSlotA = true;

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
            const uint32_t mask = 0U - (crc & 1U);
            crc =
                (crc >> 1) ^
                (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static uint32_t stateCrc(
    const StoredTrialState &value
)
{
    return crc32Update(
        0U,
        reinterpret_cast<const uint8_t *>(&value),
        offsetof(StoredTrialState, crc32)
    );
}

static bool stateValid(
    const StoredTrialState &value
)
{
    if (
        value.magic != TRIAL_MAGIC ||
        value.version != TRIAL_FORMAT_VERSION ||
        value.size != sizeof(StoredTrialState) ||
        value.initialized > 1U ||
        value.demoDayInitialized > 1U
    ) {
        return false;
    }

    for (size_t i = 0; i < sizeof(value.reserved); ++i) {
        if (value.reserved[i] != 0U)
            return false;
    }

    return value.crc32 == stateCrc(value);
}

static void initializeEmptyState()
{
    memset(&state, 0, sizeof(state));
    state.magic = TRIAL_MAGIC;
    state.version = TRIAL_FORMAT_VERSION;
    state.size = (uint16_t)sizeof(StoredTrialState);
    state.crc32 = stateCrc(state);
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
    StoredTrialState &output,
    bool &present
)
{
    present = false;
    memset(&output, 0, sizeof(output));

    const size_t length = prefs.getBytesLength(key);
    if (length == 0)
        return true;

    present = true;
    if (length != sizeof(StoredTrialState))
        return false;

    const size_t read =
        prefs.getBytes(
            key,
            &output,
            sizeof(output)
        );

    return
        read == sizeof(output) &&
        stateValid(output);
}

static bool writeState()
{
    if (!initialized)
        return false;

    StoredTrialState candidate = state;
    candidate.generation++;
    candidate.magic = TRIAL_MAGIC;
    candidate.version = TRIAL_FORMAT_VERSION;
    candidate.size = (uint16_t)sizeof(StoredTrialState);
    memset(candidate.reserved, 0, sizeof(candidate.reserved));
    candidate.crc32 = stateCrc(candidate);

    Preferences prefs;
    if (!prefs.begin(TRIAL_NVS_NAMESPACE, false)) {
        storageHealthy = false;
        return false;
    }

    const char *key =
        nextWriteSlotA
        ? TRIAL_SLOT_A_KEY
        : TRIAL_SLOT_B_KEY;

    const size_t written =
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
    nextWriteSlotA = !nextWriteSlotA;
    storageHealthy = true;
    return true;
}

static uint64_t elapsedSince(
    uint64_t current,
    uint64_t baseline
)
{
    return current >= baseline
        ? current - baseline
        : 0ULL;
}

static uint64_t saturatingAddU64(
    uint64_t a,
    uint64_t b
)
{
    return UINT64_MAX - a < b
        ? UINT64_MAX
        : a + b;
}

static uint64_t remainingFromLimit(
    uint64_t used,
    uint64_t limit
)
{
    return used >= limit
        ? 0ULL
        : limit - used;
}

static uint32_t currentDemoDayKey(
    const LicenseUsageSnapshot &usage
)
{
    // Prefer the live system clock only when it is currently valid. A previous
    // trusted epoch alone is not enough after a later cold boot without RTC.
    const time_t now = time(nullptr);
    if ((int64_t)now >= MIN_TRUSTED_EPOCH) {
        int64_t effectiveEpoch = (int64_t)now;
        if (usage.lastTrustedEpoch > effectiveEpoch)
            effectiveEpoch = usage.lastTrustedEpoch;

        const uint32_t epochDay =
            (uint32_t)((uint64_t)effectiveEpoch / 86400ULL);

        return 0x80000000UL | (epochDay & 0x7FFFFFFFUL);
    }

    // No RTC/NTP: use coarse active-system-time days. This can make a demo day
    // longer in wall-clock terms when the unit sleeps, which is intentional:
    // the policy favors low overhead and user access over exact metering.
    const uint32_t activeDay =
        (uint32_t)(
            usage.systemUptimeSecondsTotal /
            86400ULL
        );

    return activeDay & 0x7FFFFFFFUL;
}

static void initializeTrialBaselinesIfNeeded()
{
    if (state.initialized != 0U)
        return;

    const LicenseUsageSnapshot usage =
        licenseUsageSnapshot();

    StoredTrialState previous = state;

    state.trialStartRecordedSeconds =
        usage.recordedSecondsEffectiveTotal;
    state.trialStartStreamedSeconds =
        usage.streamedSecondsEffectiveTotal;
    state.trialStartUptimeSeconds =
        usage.systemUptimeSecondsTotal;
    state.trialStartTrustedEpoch = 0;
    state.demoDayInitialized = 0U;
    state.demoDayKey = 0;
    state.demoDayRecordingsBase = 0;
    state.demoDayRecordedSecondsBase = 0;
    state.demoDayStreamedSecondsBase = 0;
    state.initialized = 1U;

    if (!writeState()) {
        state = previous;
        storageHealthy = false;
    }
}

} // namespace

void licenseTrialBegin()
{
    if (initialized)
        return;

    initializeEmptyState();
    initialized = true;
    storageHealthy = true;
    integrityError = false;

    Preferences prefs;
    if (!prefs.begin(TRIAL_NVS_NAMESPACE, true)) {
        storageHealthy = false;
        initializeTrialBaselinesIfNeeded();
        return;
    }

    StoredTrialState slotA = {};
    StoredTrialState slotB = {};
    bool slotAPresent = false;
    bool slotBPresent = false;

    const bool slotAValid =
        readSlot(
            prefs,
            TRIAL_SLOT_A_KEY,
            slotA,
            slotAPresent
        );

    const bool slotBValid =
        readSlot(
            prefs,
            TRIAL_SLOT_B_KEY,
            slotB,
            slotBPresent
        );

    prefs.end();

    if (slotAValid && slotAPresent && slotBValid && slotBPresent) {
        if (generationNewer(slotB.generation, slotA.generation)) {
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

    initializeTrialBaselinesIfNeeded();
}

bool licenseTrialObserveTrustedTimeNow()
{
    if (!initialized)
        licenseTrialBegin();

    if (state.initialized == 0U)
        return false;

    if (state.trialStartTrustedEpoch >= MIN_TRUSTED_EPOCH)
        return true;

    const time_t now = time(nullptr);
    if ((int64_t)now < MIN_TRUSTED_EPOCH)
        return false;

    StoredTrialState previous = state;
    state.trialStartTrustedEpoch = (int64_t)now;

    if (!writeState()) {
        state = previous;
        return false;
    }

    return true;
}

LicenseTrialSnapshot licenseTrialSnapshot()
{
    if (!initialized)
        licenseTrialBegin();

    const LicenseUsageSnapshot usage =
        licenseUsageSnapshot();

    LicenseTrialSnapshot snapshot = {};
    snapshot.initialized = state.initialized != 0U;
    snapshot.storageHealthy = storageHealthy;
    snapshot.integrityError = integrityError;
    snapshot.trialStartTrustedEpoch = state.trialStartTrustedEpoch;
    snapshot.lastTrustedEpoch = usage.lastTrustedEpoch;

    if (!snapshot.initialized) {
        // Fail open if the tiny trial-state record cannot be initialized. A
        // storage problem must never unexpectedly disable product functions.
        snapshot.activeSecondsRemaining = SENSORFORGE_TRIAL_ACTIVE_SECONDS;
        snapshot.recordingSecondsRemaining = SENSORFORGE_TRIAL_RECORDING_SECONDS;
        snapshot.streamingSecondsRemaining = SENSORFORGE_TRIAL_STREAMING_SECONDS;
        snapshot.expired = false;
        return snapshot;
    }

    snapshot.activeSecondsUsed =
        elapsedSince(
            usage.systemUptimeSecondsTotal,
            state.trialStartUptimeSeconds
        );

    snapshot.recordingSecondsUsed =
        elapsedSince(
            usage.recordedSecondsEffectiveTotal,
            state.trialStartRecordedSeconds
        );

    snapshot.streamingSecondsUsed =
        elapsedSince(
            usage.streamedSecondsEffectiveTotal,
            state.trialStartStreamedSeconds
        );

    snapshot.mediaSecondsUsed =
        saturatingAddU64(
            snapshot.recordingSecondsUsed,
            snapshot.streamingSecondsUsed
        );

    snapshot.activeSecondsRemaining =
        remainingFromLimit(
            snapshot.activeSecondsUsed,
            SENSORFORGE_TRIAL_ACTIVE_SECONDS
        );

    snapshot.recordingSecondsRemaining =
        remainingFromLimit(
            snapshot.recordingSecondsUsed,
            SENSORFORGE_TRIAL_RECORDING_SECONDS
        );

    snapshot.streamingSecondsRemaining =
        remainingFromLimit(
            snapshot.streamingSecondsUsed,
            SENSORFORGE_TRIAL_STREAMING_SECONDS
        );

    int64_t effectiveTrustedEpoch =
        usage.lastTrustedEpoch;

    const time_t now = time(nullptr);
    if (
        (int64_t)now >= MIN_TRUSTED_EPOCH &&
        (int64_t)now > effectiveTrustedEpoch
    ) {
        effectiveTrustedEpoch = (int64_t)now;
    }

    snapshot.lastTrustedEpoch = effectiveTrustedEpoch;
    snapshot.trustedCalendarAvailable =
        state.trialStartTrustedEpoch >= MIN_TRUSTED_EPOCH &&
        effectiveTrustedEpoch >= state.trialStartTrustedEpoch;

    if (snapshot.trustedCalendarAvailable) {
        snapshot.trustedCalendarSecondsUsed =
            (uint64_t)(
                effectiveTrustedEpoch -
                state.trialStartTrustedEpoch
            );
    }

    snapshot.expired =
        snapshot.activeSecondsUsed >= SENSORFORGE_TRIAL_ACTIVE_SECONDS ||
        snapshot.recordingSecondsUsed >= SENSORFORGE_TRIAL_RECORDING_SECONDS ||
        snapshot.streamingSecondsUsed >= SENSORFORGE_TRIAL_STREAMING_SECONDS ||
        (
            snapshot.trustedCalendarAvailable &&
            snapshot.trustedCalendarSecondsUsed >= SENSORFORGE_TRIAL_ACTIVE_SECONDS
        );

    return snapshot;
}

LicenseDemoDaySnapshot licenseTrialDemoDaySnapshot()
{
    if (!initialized)
        licenseTrialBegin();

    const LicenseUsageSnapshot usage =
        licenseUsageSnapshot();

    LicenseDemoDaySnapshot snapshot = {};
    snapshot.storageHealthy = storageHealthy;

    const uint32_t dayKey =
        currentDemoDayKey(usage);

    if (
        state.demoDayInitialized == 0U ||
        state.demoDayKey != dayKey
    ) {
        StoredTrialState previous = state;

        state.demoDayKey = dayKey;
        state.demoDayRecordingsBase = usage.recordingsTotal;
        state.demoDayRecordedSecondsBase =
            usage.recordedSecondsEffectiveTotal;
        state.demoDayStreamedSecondsBase =
            usage.streamedSecondsEffectiveTotal;
        state.demoDayInitialized = 1U;

        if (!writeState()) {
            state = previous;
            storageHealthy = false;
        }
    }

    snapshot.initialized = state.demoDayInitialized != 0U;
    snapshot.storageHealthy = storageHealthy;
    snapshot.dayKey = state.demoDayKey;

    if (!snapshot.initialized)
        return snapshot;

    snapshot.recordingsUsed =
        usage.recordingsTotal >= state.demoDayRecordingsBase
        ? usage.recordingsTotal - state.demoDayRecordingsBase
        : 0U;

    snapshot.recordingSecondsUsed =
        elapsedSince(
            usage.recordedSecondsEffectiveTotal,
            state.demoDayRecordedSecondsBase
        );

    snapshot.streamingSecondsUsed =
        elapsedSince(
            usage.streamedSecondsEffectiveTotal,
            state.demoDayStreamedSecondsBase
        );

    return snapshot;
}

String licenseTrialDiagnosticSummary()
{
    const LicenseTrialSnapshot trial =
        licenseTrialSnapshot();

    String text =
        "trial=" +
        String(trial.expired ? "expired" : "active") +
        " | trial_active_s=" +
        String((unsigned long long)trial.activeSecondsUsed) +
        " | trial_media_s=" +
        String((unsigned long long)trial.mediaSecondsUsed) +
        " | trial_recorded_s=" +
        String((unsigned long long)trial.recordingSecondsUsed) +
        " | trial_streamed_s=" +
        String((unsigned long long)trial.streamingSecondsUsed) +
        " | trial_start_epoch=" +
        String((long long)trial.trialStartTrustedEpoch) +
        " | trial_last_epoch=" +
        String((long long)trial.lastTrustedEpoch) +
        " | nvs=" +
        String(trial.storageHealthy ? "ok" : "error") +
        " | integrity=" +
        String(trial.integrityError ? "error" : "ok");

    return text;
}
