#include "logger.h"

#include "config.h"
#include "board_config.h"
#include "storage_guard.h"
#include "log_storage.h"
#include "sensorforge_version.h"

#include <FS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <esp_heap_caps.h>


static LogStorageWriter logWriter;
static String logCurrentPath;
static uint64_t logBytesWritten = 0;

// Low-power log batching. Normal log lines are accumulated in PSRAM and only
// persisted in batches. This avoids waking the SD card for every informational
// event during continuous-shooter operation. Light sleep retains PSRAM; all
// deliberate deep-sleep/reboot/shutdown paths already call logFlush()/logClose().
static uint8_t *logRamBuffer = nullptr;
static size_t logRamBufferCapacity = 0;
static size_t logRamBufferUsed = 0;
static uint32_t logRamBufferFirstQueuedMs = 0;
static uint32_t logRamBufferLastAutoFlushAttemptMs = 0;
static const size_t LOG_RAM_BUFFER_NORMAL_TARGET_BYTES = 16U * 1024U;
static const size_t LOG_RAM_BUFFER_SHOOTER_TARGET_BYTES = 256U * 1024U;
static const size_t LOG_RAM_BUFFER_MIN_BYTES = 4U * 1024U;
static const uint32_t LOG_RAM_BUFFER_NORMAL_MAX_AGE_MS =
    5UL * 60UL * 1000UL;
static const uint32_t LOG_RAM_BUFFER_AUTO_FLUSH_RETRY_MS =
    10UL * 1000UL;

// While the continuous shooter batches accepted JPEGs in PSRAM, normal log
// traffic should follow the same persistence rhythm: keep it in PSRAM and let
// the existing shooter flush path persist it immediately after the media batch.
// 256 KiB is intentionally small compared with the shooter's reserved PSRAM
// headroom, but large enough that ordinary status/error traffic cannot wake the
// SD card during long (for example 30-minute) shooter batching windows.
static bool logShooterBatchingPreferred()
{
    return
        cfg_shooter_enabled != 0 &&
        cfg_shooter_flush_seconds > 0;
}

static size_t logDesiredRamBufferBytes()
{
    return
        logShooterBatchingPreferred()
        ? LOG_RAM_BUFFER_SHOOTER_TARGET_BYTES
        : LOG_RAM_BUFFER_NORMAL_TARGET_BYTES;
}


static uint32_t logRamBufferMaxAgeMs()
{
    if (logShooterBatchingPreferred()) {
        return
            (uint32_t)cfg_shooter_flush_seconds *
            1000UL;
    }

    return
        LOG_RAM_BUFFER_NORMAL_MAX_AGE_MS;
}

// Keep encrypted SFLOG1 record boundaries on complete text-line boundaries.
// log_storage currently authenticates at most 1024 plaintext bytes per record.
// If a physical record is later lost/torn and the reader resynchronizes, this
// prevents the tail of one log line from being glued to the head of another.
static const size_t LOG_STORAGE_RECORD_PLAINTEXT_MAX = 1024U;

// Keep at most two log generations: current + .1 backup.
// 5 MiB each gives a hard long-term budget of about 10 MiB.
static const uint64_t LOG_ROTATE_BYTES =
    5ULL * 1024ULL * 1024ULL;


// =============================================================
// FIRMWARE BUILD / INSTALLATION METADATA
// =============================================================
//
// Stored in NVS so the information survives OTA updates and does
// not depend on SD config.txt or LittleFS.
//
// epoch   = installation time in Unix seconds, when known
// pending = firmware update succeeded but there was no valid clock yet
// source  = installation source of the current tracked firmware

static const char *FW_PREF_NAMESPACE =
    "fwmeta";

static const char *FW_PREF_EPOCH =
    "epoch";

static const char *FW_PREF_PENDING =
    "pending";

static const char *FW_PREF_SOURCE =
    "source";

static const char *FW_PREF_SKIP_SD_ONCE =
    "skip_sd_once";


static bool firmwareClockIsValid()
{
    time_t now =
        time(
            nullptr
        );

    // 2021-01-01 UTC. Same spirit as timeIsValid(), but this helper
    // does not depend on local timezone configuration.
    return
        now >=
        (time_t)1609459200;
}


static String firmwareFormatEpochLocal(
    uint64_t epoch
)
{
    if (epoch == 0)
        return String("unknown");


    // Apply the configured POSIX timezone even on boots where NTP is not
    // started. This only affects local-time conversion; it does not alter
    // the underlying Unix clock.
    if (cfg_timezone.length()) {

        setenv(
            "TZ",
            cfg_timezone.c_str(),
            1
        );

        tzset();
    }


    time_t value =
        (time_t)epoch;

    struct tm localTime;

    if (!localtime_r(
            &value,
            &localTime
        )) {

        return String("unknown");
    }


    char buffer[32];

    if (!strftime(
            buffer,
            sizeof(buffer),
            "%Y-%m-%d %H:%M:%S",
            &localTime
        )) {

        return String("unknown");
    }


    return
        String(buffer);
}


String firmwareInstallTimestamp()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            true
        )) {

        return
            String("unknown");
    }


    uint64_t epoch =
        preferences.getULong64(
            FW_PREF_EPOCH,
            0
        );

    bool pending =
        preferences.getBool(
            FW_PREF_PENDING,
            false
        );

    preferences.end();


    if (epoch != 0) {
        return
            firmwareFormatEpochLocal(
                epoch
            );
    }


    if (pending) {
        return
            String("pending time sync");
    }


    return
        String("not tracked");
}


String firmwareInstallSource()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            true
        )) {

        return
            String("unknown");
    }


    String source =
        preferences.getString(
            FW_PREF_SOURCE,
            ""
        );

    preferences.end();


    if (!source.length())
        return String("not tracked");


    return source;
}


bool firmwareInstallTimePending()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            true
        )) {

        return false;
    }


    bool pending =
        preferences.getBool(
            FW_PREF_PENDING,
            false
        );

    preferences.end();

    return pending;
}


static bool firmwareInfoMarkUpdateSource(
    const String &source
)
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            false
        )) {

        return false;
    }


    bool ok =
        preferences.putString(
            FW_PREF_SOURCE,
            source
        ) > 0;


    if (firmwareClockIsValid()) {

        time_t now =
            time(
                nullptr
            );

        ok =
            preferences.putULong64(
                FW_PREF_EPOCH,
                (uint64_t)now
            ) ==
            sizeof(uint64_t) &&
            ok;

        preferences.putBool(
            FW_PREF_PENDING,
            false
        );

    } else {

        preferences.remove(
            FW_PREF_EPOCH
        );

        preferences.putBool(
            FW_PREF_PENDING,
            true
        );
    }


    preferences.end();

    return ok;
}


bool firmwareInfoMarkSdUpdate(
    const String &sourceFilename
)
{
    String source =
        "SD auto-update";

    if (sourceFilename.length()) {
        source +=
            ": " +
            sourceFilename;
    }

    return
        firmwareInfoMarkUpdateSource(
            source
        );
}


bool firmwareInfoMarkWifiUpdate(
    const String &sourceFilename
)
{
    String source =
        "WiFi OTA";

    if (sourceFilename.length()) {
        source +=
            ": " +
            sourceFilename;
    }

    return
        firmwareInfoMarkUpdateSource(
            source
        );
}


bool firmwareInfoArmDirectOtaBoot()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            false
        )) {

        return false;
    }

    bool ok =
        preferences.putBool(
            FW_PREF_SKIP_SD_ONCE,
            true
        ) == 1U;

    preferences.end();
    return ok;
}


void firmwareInfoCancelDirectOtaBoot()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            false
        )) {

        return;
    }

    preferences.remove(
        FW_PREF_SKIP_SD_ONCE
    );

    preferences.end();
}


bool firmwareInfoConsumeSkipSdUpdateOnce()
{
    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            false
        )) {

        return false;
    }

    bool skip =
        preferences.getBool(
            FW_PREF_SKIP_SD_ONCE,
            false
        );

    if (skip) {
        preferences.remove(
            FW_PREF_SKIP_SD_ONCE
        );
    }

    preferences.end();
    return skip;
}


bool firmwareInfoFinalizePendingInstallTime()
{
    if (!firmwareClockIsValid())
        return false;


    Preferences preferences;

    if (!preferences.begin(
            FW_PREF_NAMESPACE,
            false
        )) {

        return false;
    }


    bool pending =
        preferences.getBool(
            FW_PREF_PENDING,
            false
        );


    if (!pending) {

        preferences.end();

        return false;
    }


    time_t now =
        time(
            nullptr
        );


    bool epochWritten =
        preferences.putULong64(
            FW_PREF_EPOCH,
            (uint64_t)now
        ) ==
        sizeof(uint64_t);


    if (epochWritten) {

        preferences.putBool(
            FW_PREF_PENDING,
            false
        );
    }


    preferences.end();

    return epochWritten;
}


void firmwareInfoLogStatus()
{
    String line =
        "Firmware: release=" +
        String(SENSORFORGE_RELEASE_TAG) +
        " | release_date=" +
        String(SENSORFORGE_RELEASE_DATE) +
        " | build=" +
        firmwareBuildTimestamp() +
        " | installed=" +
        firmwareInstallTimestamp() +
        " | source=" +
        firmwareInstallSource();

    Serial.println(
        line
    );

    logWrite(
        line
    );
}


static String activeLogPath()
{
    if (cfg_log_file.length())
        return cfg_log_file;

    return "/log.txt";
}


static String rotatedLogPath(
    const String &path
)
{
    return logStorageRotatedPath(path);
}


static bool removeLogGeneration(
    const String &path,
    String &error
)
{
    if (!path.length())
        return true;

    if (STORAGE.exists(path.c_str())) {
        if (!STORAGE.remove(path.c_str())) {
            error =
                "cannot remove active log: " +
                path;
            return false;
        }
    }

    String backup =
        rotatedLogPath(path);

    if (STORAGE.exists(backup.c_str())) {
        if (!STORAGE.remove(backup.c_str())) {
            error =
                "cannot remove rotated log: " +
                backup;
            return false;
        }
    }

    return true;
}


bool logClear(String &error)
{
    error = "";

    if (g_storageLocked) {
        error = "storage is locked by maintenance";
        return false;
    }

    // The logger can fall back to /log.txt if the configured path could not
    // be opened. Clear both the configured generation and the actual open
    // generation so a manual reset cannot leave an older hidden log behind.
    String configuredPath =
        activeLogPath();

    String actualPath =
        logCurrentPath.length()
        ? logCurrentPath
        : configuredPath;

    logClose();

    bool ok =
        removeLogGeneration(
            configuredPath,
            error
        );

    if (
        ok &&
        actualPath != configuredPath
    ) {
        ok =
            removeLogGeneration(
                actualPath,
                error
            );
    }

    // Reopen even after a partial failure so normal logging is not left
    // disabled merely because a stale generation could not be removed.
    logCurrentPath = "";
    logInit();

    if (!logWriter.file) {
        if (!error.length()) {
            error =
                "log file could not be reopened after clear";
        }
        return false;
    }

    if (!ok)
        return false;

    logWrite(
        "Log cleared from WebConfig"
    );

    return true;
}


static bool rotateLogFile(
    const String &path
)
{
    String oldPath =
        rotatedLogPath(path);

    if (STORAGE.exists(oldPath.c_str())) {
        STORAGE.remove(oldPath.c_str());
    }

    if (!STORAGE.rename(
            path.c_str(),
            oldPath.c_str()
        )) {
        Serial.println(
            "Logger: rotation rename failed"
        );
        return false;
    }

    return true;
}


static void rotateLogIfNeeded(
    const String &path
)
{
    File existing =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!existing)
        return;

    uint64_t size =
        existing.size();

    existing.close();

    if (size < LOG_ROTATE_BYTES)
        return;

    // Keep the historical oversized-plaintext cleanup behavior, but do not
    // discard a freshly migrated SFLOG1 generation merely because authenticated
    // record overhead pushed a near-5-MiB plaintext log slightly above the
    // physical threshold. In that case retain it as the normal .1 backup.
    if (size > LOG_ROTATE_BYTES) {
        uint64_t infoSize = 0;
        bool encrypted = false;
        String generation;
        String infoError;

        bool infoOk =
            logStorageGetInfo(
                path,
                infoSize,
                encrypted,
                generation,
                infoError
            );

        if (!infoOk || !encrypted) {
            String oldPath =
                rotatedLogPath(path);

            if (STORAGE.exists(oldPath.c_str())) {
                STORAGE.remove(oldPath.c_str());
            }

            if (!STORAGE.remove(path.c_str())) {
                Serial.println(
                    "Logger: oversized log cleanup failed"
                );
            }
            return;
        }
    }

    rotateLogFile(path);
}


static uint64_t estimatedPhysicalAppendBytes(
    size_t plaintextBytes
)
{
    if (!cfg_recording_encryption)
        return plaintextBytes;

    // SFLOG1 stores at most 1024 plaintext bytes per independently
    // authenticated record. Each record adds 32 physical bytes:
    // 16-byte record header + 16-byte GCM tag.
    uint64_t records =
        (plaintextBytes + 1023U) / 1024U;

    return
        (uint64_t)plaintextBytes +
        records * 32ULL;
}


static bool openLogWriter(
    const String &path
)
{
    String error;

    if (!logStorageOpenWriter(
            path,
            cfg_recording_encryption,
            logWriter,
            error
        )) {
        Serial.println(
            "Logger: open failed | " + error
        );
        logBytesWritten = 0;
        return false;
    }

    logBytesWritten =
        logStorageWriterSize(logWriter);

    return true;
}


static bool rotateActiveLogForNextWrite(
    size_t nextPlaintextBytes
)
{
    if (!logWriter.file)
        return false;

    uint64_t nextPhysicalBytes =
        estimatedPhysicalAppendBytes(
            nextPlaintextBytes
        );

    if (
        logBytesWritten +
        nextPhysicalBytes <=
        LOG_ROTATE_BYTES
    ) {
        return true;
    }

    logStorageFlushWriter(logWriter);
    logStorageCloseWriter(logWriter);

    if (!rotateLogFile(logCurrentPath)) {
        openLogWriter(logCurrentPath);
        return false;
    }

    if (!openLogWriter(logCurrentPath)) {
        Serial.println(
            "Logger: cannot reopen log after rotation"
        );
        return false;
    }

    return true;
}


static bool ensureLogRamBuffer()
{
    size_t desired =
        logDesiredRamBufferBytes();

    if (
        logRamBuffer &&
        logRamBufferCapacity >= desired
    ) {
        return true;
    }

    // The logger normally starts with a small buffer. If the shooter is enabled
    // later from WebConfig, grow lazily without discarding already queued log
    // lines. Allocate the replacement first so an allocation failure leaves the
    // existing queue intact.
    size_t attempt = desired;

    while (attempt >= LOG_RAM_BUFFER_MIN_BYTES) {
        // Never replace a working buffer by a smaller one.
        if (
            logRamBuffer &&
            attempt <= logRamBufferCapacity
        ) {
            return true;
        }

        uint8_t *replacement =
            (uint8_t *)heap_caps_malloc(
                attempt,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

        if (replacement) {
            if (
                logRamBuffer &&
                logRamBufferUsed > 0
            ) {
                memcpy(
                    replacement,
                    logRamBuffer,
                    logRamBufferUsed
                );
            }

            if (logRamBuffer)
                heap_caps_free(logRamBuffer);

            logRamBuffer = replacement;
            logRamBufferCapacity = attempt;

            Serial.printf(
                "Logger RAM buffer: %u bytes PSRAM | mode=%s\n",
                (unsigned)attempt,
                logShooterBatchingPreferred()
                    ? "shooter-batched"
                    : "normal"
            );

            return true;
        }

        attempt /= 2U;
    }

    if (logRamBuffer && logRamBufferCapacity > 0) {
        // Keep the existing queue. If it eventually fills, the normal fail-safe
        // path below will persist it rather than losing diagnostic data.
        return true;
    }

    logRamBuffer = nullptr;
    logRamBufferCapacity = 0;
    logRamBufferUsed = 0;
    logRamBufferFirstQueuedMs = 0;
    logRamBufferLastAutoFlushAttemptMs = 0;

    Serial.println(
        "Logger RAM buffer unavailable - direct SD logging fallback"
    );

    return false;
}


static size_t logBufferedFlushChunkLength(
    const uint8_t *data,
    size_t remaining
)
{
    if (!data || remaining == 0)
        return 0;

    if (remaining <= LOG_STORAGE_RECORD_PLAINTEXT_MAX)
        return remaining;

    size_t limit = LOG_STORAGE_RECORD_PLAINTEXT_MAX;

    // The RAM queue contains complete formatted lines. Prefer the last LF that
    // fits in one authenticated SFLOG1 record so every persisted record ends
    // cleanly between log lines.
    for (size_t i = limit; i > 0; --i) {
        if (data[i - 1U] == '\n')
            return i;
    }

    // Defensive fallback for an unexpectedly huge single line. Normal
    // SensorForge log lines are far below 1024 bytes, so this should not occur.
    return limit;
}


static bool flushPendingLogBuffer()
{
    if (logRamBufferUsed == 0)
        return true;

    if (!logWriter.file)
        return false;

    size_t offset = 0;

    // Keep each storage append at or below the native SFLOG1 plaintext-record
    // size AND end it at a complete log-line boundary whenever possible.
    while (offset < logRamBufferUsed) {
        size_t chunk =
            logBufferedFlushChunkLength(
                logRamBuffer + offset,
                logRamBufferUsed - offset
            );

        if (chunk == 0)
            break;

        if (!rotateActiveLogForNextWrite(chunk))
            break;

        uint64_t written = 0;
        String error;

        if (!logStorageAppend(
                logWriter,
                logRamBuffer + offset,
                chunk,
                written,
                error
            )) {
            if (logWriter.file)
                logBytesWritten = logStorageWriterSize(logWriter);

            Serial.println(
                "Logger: buffered write failed | " + error
            );
            break;
        }

        logBytesWritten += written;
        offset += chunk;
    }

    if (offset > 0) {
        size_t remaining =
            logRamBufferUsed - offset;

        if (remaining > 0) {
            memmove(
                logRamBuffer,
                logRamBuffer + offset,
                remaining
            );
        }

        logRamBufferUsed = remaining;
    }

    if (logRamBufferUsed == 0) {
        logRamBufferFirstQueuedMs = 0;
        logRamBufferLastAutoFlushAttemptMs = 0;
    } else if (logRamBufferFirstQueuedMs == 0) {
        // Defensive only. A non-empty queue should always have an age marker,
        // but preserve a bounded persistence window if an older runtime state
        // reaches this point without one.
        logRamBufferFirstQueuedMs = millis();
    }

    if (logWriter.file)
        logStorageFlushWriter(logWriter);

    return logRamBufferUsed == 0;
}


static bool appendBufferedLogBytes(
    const uint8_t *data,
    size_t length
)
{
    if (!data || length == 0)
        return true;

    if (!logWriter.file)
        return false;

    // PSRAM is expected on production boards. If it is unexpectedly unavailable,
    // preserve the historical durability behavior instead of dropping logs.
    if (!ensureLogRamBuffer()) {
        if (!rotateActiveLogForNextWrite(length))
            return false;

        uint64_t written = 0;
        String error;

        if (!logStorageAppend(
                logWriter,
                data,
                length,
                written,
                error
            )) {
            if (logWriter.file)
                logBytesWritten = logStorageWriterSize(logWriter);

            Serial.println(
                "Logger: direct fallback write failed | " + error
            );
            return false;
        }

        logBytesWritten += written;
        logStorageFlushWriter(logWriter);
        return true;
    }

    // A single unusually long line should not force an oversized RAM buffer.
    // Flush the normal queue first, then persist that line directly.
    if (length > logRamBufferCapacity) {
        if (!flushPendingLogBuffer())
            return false;

        if (!rotateActiveLogForNextWrite(length))
            return false;

        uint64_t written = 0;
        String error;

        if (!logStorageAppend(
                logWriter,
                data,
                length,
                written,
                error
            )) {
            if (logWriter.file)
                logBytesWritten = logStorageWriterSize(logWriter);

            Serial.println(
                "Logger: oversized direct write failed | " + error
            );
            return false;
        }

        logBytesWritten += written;
        logStorageFlushWriter(logWriter);
        return true;
    }

    if (logRamBufferUsed + length > logRamBufferCapacity) {
        // In shooter-batched mode ensureLogRamBuffer() has already attempted to
        // expand the queue up to the dedicated 256-KiB target. Reaching this
        // point is therefore an exceptional safety limit (or PSRAM pressure).
        // Persist rather than dropping logs, even though that may cause one
        // additional SD wake before the next media flush.
        if (!flushPendingLogBuffer())
            return false;
    }

    bool queueWasEmpty =
        logRamBufferUsed == 0;

    memcpy(
        logRamBuffer + logRamBufferUsed,
        data,
        length
    );

    logRamBufferUsed += length;

    if (queueWasEmpty) {
        logRamBufferFirstQueuedMs = millis();
        logRamBufferLastAutoFlushAttemptMs = 0;
    }

    return true;
}


void logInit()
{
    logClose();

    String path =
        activeLogPath();

    String prepareError;

    if (!logStoragePrepareGenerations(
            path,
            cfg_recording_encryption,
            prepareError
        )) {
        Serial.println(
            "Logger: storage prepare failed | " +
            prepareError
        );
    }

    rotateLogIfNeeded(path);
    logCurrentPath = path;

    if (!openLogWriter(path) && path != "/log.txt") {
        path = "/log.txt";

        prepareError = "";
        logStoragePrepareGenerations(
            path,
            cfg_recording_encryption,
            prepareError
        );

        rotateLogIfNeeded(path);
        logCurrentPath = path;
        openLogWriter(path);
    }

    if (!logWriter.file) {
        Serial.println(
            "Logger: cannot open log file"
        );
        logBytesWritten = 0;
        return;
    }

    Serial.printf(
        "Logger storage: %s | path=%s\n",
        logWriter.encrypted ? "SFLOG1" : "plaintext",
        logCurrentPath.c_str()
    );

    ensureLogRamBuffer();
}


void logClose(bool persistPending)
{
    if (!logWriter.file)
        return;

    if (storageIoFaultActive())
        persistPending = false;

    // After EIO the mount may already be poisoned. Do not turn a media fault
    // into a torn SFLOG1 tail by pushing the RAM queue through that handle.
    // The RAM queue intentionally remains intact and will be drained after the
    // recovery remount reopens the writer.
    if (persistPending) {
        flushPendingLogBuffer();
        logStorageFlushWriter(logWriter);
    }

    logStorageCloseWriter(
        logWriter,
        persistPending
    );

    logBytesWritten = 0;
}


void logFlush()
{
    if (storageIoFaultActive())
        return;

    flushPendingLogBuffer();
    logStorageFlushWriter(logWriter);
}


void logService()
{
    if (
        g_storageLocked ||
        storageIoFaultActive() ||
        !logWriter.file ||
        logRamBufferUsed == 0
    ) {
        return;
    }

    uint32_t now =
        millis();

    if (logRamBufferFirstQueuedMs == 0) {
        logRamBufferFirstQueuedMs = now;
        return;
    }

    uint32_t maxAgeMs =
        logRamBufferMaxAgeMs();

    if (
        maxAgeMs == 0 ||
        (uint32_t)(
            now -
            logRamBufferFirstQueuedMs
        ) < maxAgeMs
    ) {
        return;
    }

    if (
        logRamBufferLastAutoFlushAttemptMs != 0 &&
        (uint32_t)(
            now -
            logRamBufferLastAutoFlushAttemptMs
        ) <
        LOG_RAM_BUFFER_AUTO_FLUSH_RETRY_MS
    ) {
        return;
    }

    logRamBufferLastAutoFlushAttemptMs = now;

    // This is a durability fallback, not the preferred shooter path. When the
    // Power Shooter is active, its successful media flush still calls
    // logFlush() first. This timer only prevents a paused/idle shooter from
    // leaving rare but important log lines in PSRAM indefinitely.
    flushPendingLogBuffer();
}


void logBlankLine()
{
    if (g_storageLocked)
        return;

    if (!logWriter.file)
        return;

    static const uint8_t blankLine[] = {'\r', '\n'};

    if (!appendBufferedLogBytes(
            blankLine,
            sizeof(blankLine)
        )) {
        Serial.println(
            "Logger: blank-line queue failed"
        );
    }
}


static void formatLogTimestamp(
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
    // timestamp instead of a bogus calendar date.
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


void consoleWrite(
    const char *tag,
    const String &msg
)
{
    char timestamp[40];
    formatLogTimestamp(
        timestamp,
        sizeof(timestamp)
    );

    const char *safeTag =
        (tag && tag[0])
        ? tag
        : "INFO";

    Serial.printf(
        "[%s] [%s] %s\n",
        timestamp,
        safeTag,
        msg.c_str()
    );
}


void consolePrintf(
    const char *tag,
    const char *format,
    ...
)
{
    char message[256];

    va_list args;
    va_start(args, format);

    vsnprintf(
        message,
        sizeof(message),
        format,
        args
    );

    va_end(args);

    consoleWrite(
        tag,
        String(message)
    );
}


static void writeFormattedLogLine(
    const char *level,
    const String &msg
)
{
    // Never write through a persistent logger handle while the global
    // storage gate is held. SD maintenance closes/reopens the handle itself.
    if (g_storageLocked)
        return;
    if (!logWriter.file)
        return;

    char timestamp[40];
    formatLogTimestamp(
        timestamp,
        sizeof(timestamp)
    );

    String line;
    line.reserve(
        strlen(timestamp) +
        strlen(level) +
        msg.length() +
        10U
    );

    line += '[';
    line += timestamp;
    line += "] [";
    line += level;
    line += "] ";
    line += msg;
    line += "\r\n";

    if (!appendBufferedLogBytes(
            reinterpret_cast<const uint8_t *>(line.c_str()),
            line.length()
        )) {
        Serial.println(
            "Logger: queue failed"
        );
    }
}


void logWrite(
    const String &msg
)
{
    writeFormattedLogLine(
        "INFO ",
        msg
    );
}


void debugDump()
{
    if (!logWriter.file)
        return;

    writeFormattedLogLine(
        "DEBUG",
        "CONFIG | camera=" + cfg_camera +
        " | resolution=" + cfg_resolution +
        " | fps=" + String(cfg_fps) +
        " | quality=" + String(cfg_quality) +
        " | rotation=" + String(cfg_rotation)
    );

    writeFormattedLogLine(
        "DEBUG",
        "CONFIG | format=" + cfg_recording_format +
        " | timestamp=" + String(cfg_timestamp_enabled) +
        " | post_ms=" + String(cfg_post_ms) +
        " | min_free_mb=" + String(cfg_min_free_space_mb) +
        " | disk_action=" + cfg_disk_full_action +
        " | led=" + String(cfg_led_enabled)
    );

    writeFormattedLogLine(
        "DEBUG",
        "CONFIG | encryption=" + String(cfg_recording_encryption) +
        " | shooter=" + String(cfg_shooter_enabled) +
        " | shooter_storage=" + cfg_shooter_storage_format +
        " | shooter_interval_ms=" + String(cfg_shooter_interval_ms) +
        " | shooter_flush_s=" + String(cfg_shooter_flush_seconds) +
        " | motion_recording=" + String(cfg_motion_recording_enabled) +
        " | motion_mode=" + cfg_motion_recording_decision
    );

    writeFormattedLogLine(
        "DEBUG",
        "CONFIG | wifi_start=" + cfg_wifi_on_system_start +
        " | wifi_ssid=" + cfg_wifi_ssid +
        " | debug=" + String(cfg_debug_enabled)
    );
}
