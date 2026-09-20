#include "logger.h"

#include "config.h"
#include "board_config.h"
#include "storage_guard.h"
#include "log_storage.h"

#include <FS.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>


static LogStorageWriter logWriter;
static String logCurrentPath;
static uint64_t logBytesWritten = 0;

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
        "Firmware: build=" +
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
}


void logClose()
{
    if (!logWriter.file)
        return;

    logStorageFlushWriter(logWriter);
    logStorageCloseWriter(logWriter);
    logBytesWritten = 0;
}


void logFlush()
{
    logStorageFlushWriter(logWriter);
}


void logBlankLine()
{
    if (g_storageLocked)
        return;

    if (!logWriter.file)
        return;

    static const uint8_t blankLine[] = {'\r', '\n'};

    rotateActiveLogForNextWrite(
        sizeof(blankLine)
    );

    if (!logWriter.file)
        return;

    uint64_t written = 0;
    String error;

    if (!logStorageAppend(
            logWriter,
            blankLine,
            sizeof(blankLine),
            written,
            error
        )) {
        Serial.println(
            "Logger: blank-line write failed | " + error
        );
        return;
    }

    logBytesWritten += written;
    logStorageFlushWriter(logWriter);
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

    rotateActiveLogForNextWrite(
        line.length()
    );

    if (!logWriter.file)
        return;

    uint64_t written = 0;
    String error;

    if (!logStorageAppend(
            logWriter,
            reinterpret_cast<const uint8_t *>(line.c_str()),
            line.length(),
            written,
            error
        )) {
        Serial.println(
            "Logger: write failed | " + error
        );
        return;
    }

    logBytesWritten += written;

    // Low event rate; durability is more important than buffering.
    logStorageFlushWriter(logWriter);
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
        "CONFIG | wifi_start=" + cfg_wifi_on_system_start +
        " | wifi_ssid=" + cfg_wifi_ssid +
        " | debug=" + String(cfg_debug_enabled)
    );
}
