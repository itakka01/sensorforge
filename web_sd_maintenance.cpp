#include "web_sd_maintenance.h"

#include "board_config.h"
#include "config.h"
#include "language.h"
#include "logger.h"
#include "recorder.h"
#include "recording_storage.h"
#include "recording_write_buffer.h"
#include "storage_guard.h"
#include "webplayer.h"

#include <FS.h>
#include <ff.h>
#include <algorithm>
#include <LittleFS.h>
#include <esp_arduino_version.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// High-level state owned by the main firmware. Keep the same distinction used
// by v39: `recording` drives visible action availability, while recorderIsOpen()
// remains the hard safety gate for storage operations.
extern bool recording;
extern bool sdReady;
extern bool recoverSD();

#if defined(STORAGE_SPI)
extern bool sdManualReadOnlyRecovery(
    String &report,
    uint32_t &mountedFrequencyHz,
    bool &rawCardReady,
    bool &sector0Readable
);
#endif

// Raw-sector formatting support differs between Arduino-ESP32 releases.
#if defined(STORAGE_SPI)
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 1
#elif defined(STORAGE_SDMMC) && \
      ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 1
#else
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 0
#endif

namespace {

static WebServer *g_webServer = nullptr;
static WebSdMaintenanceUiHooks g_uiHooks = { nullptr, nullptr, nullptr };

static WebServer &webServer()
{
    return *g_webServer;
}

static String pageHeader()
{
    return g_uiHooks.htmlHeader
        ? g_uiHooks.htmlHeader()
        : String();
}

static String pageFooter()
{
    return g_uiHooks.htmlFooter
        ? g_uiHooks.htmlFooter()
        : String();
}

static String escapeHtml(const String &value)
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

static String translatedHtml(UiTextId id)
{
    return escapeHtml(String(tr(id)));
}

static void serviceLongOperation()
{
    esp_task_wdt_reset();
    yield();
}

static bool rejectWhileRecording(const char *operation)
{
    if (!recorderIsOpen())
        return false;

    webServer().send(
        409,
        "text/plain; charset=utf-8",
        "Recording active - " +
        String(operation) +
        " is temporarily unavailable."
    );

    return true;
}

static void scheduleReboot(uint32_t delayMs)
{
    if (g_uiHooks.scheduleReboot)
        g_uiHooks.scheduleReboot(delayMs);
}

// -------------------------------------------------------------
// SD STATUS / MAINTENANCE
// -------------------------------------------------------------

enum SdBenchmarkRating : uint8_t {
    SD_BENCH_RATING_NONE = 0,
    SD_BENCH_RATING_GREEN,
    SD_BENCH_RATING_ORANGE,
    SD_BENCH_RATING_RED
};

struct SdMaintenanceViewState {
    bool recoveryAttempted;
    bool recoverySuccess;
    bool recoveryNotRequired;
    uint32_t recoveryMountedFrequencyHz;
    bool recoveryRawCardReady;
    bool recoverySector0Readable;
    String recoveryReport;

    bool benchmarkAttempted;
    bool benchmarkSuccess;
    uint32_t benchmarkElapsedMs;
    float benchmarkReferenceWriteMBps;
    float benchmarkReferenceReadMBps;
    uint32_t benchmarkReferenceWriteP95Us;
    uint32_t benchmarkReferenceWriteWorstUs;
    uint32_t benchmarkReferenceFlushUs;
    bool benchmarkReferenceVerifyOk;
    bool benchmarkGeometryAvailable;
    uint32_t benchmarkClusterBytes;
    SdBenchmarkRating benchmarkRating;
    String benchmarkReport;
    String benchmarkError;
};

static const char *sdBenchmarkRatingPillClass(SdBenchmarkRating rating)
{
    switch (rating) {
        case SD_BENCH_RATING_GREEN:
            return "ok";
        case SD_BENCH_RATING_ORANGE:
            return "warn";
        case SD_BENCH_RATING_RED:
            return "danger";
        default:
            return "warn";
    }
}

static String sdBenchmarkRatingTitle(
    SdBenchmarkRating rating,
    bool de
)
{
    switch (rating) {
        case SD_BENCH_RATING_GREEN:
            return de ? "GRÜN – STORAGE GUT" : "GREEN – STORAGE GOOD";
        case SD_BENCH_RATING_ORANGE:
            return de ? "ORANGE – STORAGE PRÜFEN" : "ORANGE – CHECK STORAGE";
        case SD_BENCH_RATING_RED:
            return de ? "ROT – STORAGE NICHT EMPFOHLEN" : "RED – STORAGE NOT RECOMMENDED";
        default:
            return de ? "STORAGE NICHT BEWERTET" : "STORAGE NOT RATED";
    }
}

static SdBenchmarkRating sdBenchmarkEvaluateRating(
    const SdMaintenanceViewState &view
)
{
    if (!view.benchmarkAttempted)
        return SD_BENCH_RATING_NONE;

    if (
        !view.benchmarkSuccess ||
        !view.benchmarkReferenceVerifyOk
    ) {
        return SD_BENCH_RATING_RED;
    }

    // These thresholds are a SensorForge operational assessment of the
    // complete storage path (card + filesystem + bus), not a card certification.
    // The green write threshold is deliberately below the qualified XIAO/Freenove
    // reference results while retaining useful margin for camera recording.
    if (
        view.benchmarkReferenceWriteMBps < 0.50f ||
        view.benchmarkReferenceReadMBps < 0.50f ||
        view.benchmarkReferenceWriteP95Us > 250000UL
    ) {
        return SD_BENCH_RATING_RED;
    }

    bool clusterNeedsAttention =
        view.benchmarkGeometryAvailable &&
        view.benchmarkClusterBytes > 0 &&
        view.benchmarkClusterBytes < 32U * 1024U;

    if (
        clusterNeedsAttention ||
        view.benchmarkReferenceWriteMBps < 1.00f ||
        view.benchmarkReferenceReadMBps < 0.75f ||
        view.benchmarkReferenceWriteP95Us > 100000UL ||
        view.benchmarkReferenceWriteWorstUs > 250000UL
    ) {
        return SD_BENCH_RATING_ORANGE;
    }

    return SD_BENCH_RATING_GREEN;
}

static String sdMaintenancePage(
    const SdMaintenanceViewState &view
);

static void handleSDRecoveryRun()
{
    if (recording) {
        webServer().send(
            409,
            "text/plain; charset=utf-8",
            "Recording active"
        );

        return;
    }


    String report;

    uint32_t mountedFrequencyHz =
        0;

    bool rawCardReady =
        false;

    bool sector0Readable =
        false;


    bool recoveryNotRequired =
        sdReady;

    bool recovered =
        true;

    if (recoveryNotRequired) {
        report =
            cfg_web_language != "en"
            ? "Die SD-Karte ist aktuell gemountet und betriebsbereit. Es wurde keine Remount-Recovery ausgeführt."
            : "The SD card is currently mounted and operational. No remount recovery was performed.";
    } else {
#if defined(STORAGE_SPI)
        recovered =
            sdManualReadOnlyRecovery(
                report,
                mountedFrequencyHz,
                rawCardReady,
                sector0Readable
            );
#else
        recovered =
            recoverSD();

        report =
            recovered
            ? (
                cfg_web_language != "en"
                ? "SD Recovery erfolgreich. Das Dateisystem wurde über den normalen Recovery-Pfad neu eingebunden."
                : "SD recovery successful. The filesystem was remounted through the normal recovery path."
            )
            : (
                cfg_web_language != "en"
                ? "SD Recovery nicht erfolgreich. Die Karte konnte über den normalen Recovery-Pfad nicht wieder eingebunden werden."
                : "SD recovery was not successful. The card could not be remounted through the normal recovery path."
            );
#endif
    }


    SdMaintenanceViewState view = {};
    view.recoveryAttempted = true;
    view.recoverySuccess = recovered;
    view.recoveryNotRequired = recoveryNotRequired;
    view.recoveryMountedFrequencyHz = mountedFrequencyHz;
    view.recoveryRawCardReady = rawCardReady;
    view.recoverySector0Readable = sector0Readable;
    view.recoveryReport = report;

    String html =
        sdMaintenancePage(view);

    webServer().send(
        200,
        "text/html; charset=utf-8",
        html
    );
}



enum SdMaintenanceMode : uint8_t {
    SD_MAINT_WIPE = 0,
    SD_MAINT_FORMAT,
    SD_MAINT_SECURE_ERASE
};


enum SdMaintenanceResult : uint8_t {
    SD_MAINT_RESULT_OK = 0,
    SD_MAINT_RESULT_OPERATION_FAILED,
    SD_MAINT_RESULT_CONFIG_SOURCE_FAILED,
    SD_MAINT_RESULT_CONFIG_RESTORE_FAILED,
    SD_MAINT_RESULT_RECORDING_ACTIVE,
    SD_MAINT_RESULT_STORAGE_LOCKED,
    SD_MAINT_RESULT_UNSUPPORTED
};


enum SdSecureJobStage : uint8_t {
    SD_SECURE_JOB_IDLE = 0,
    SD_SECURE_JOB_OVERWRITE,
    SD_SECURE_JOB_FORMAT,
    SD_SECURE_JOB_RESTORE,
    SD_SECURE_JOB_DONE
};


// Secure Erase is intentionally processed incrementally from webConfigLoop().
// This keeps the synchronous WebServer responsive enough for progress polling
// and an Abort request while the logical overwrite is running.
static SdSecureJobStage sdSecureJobStage = SD_SECURE_JOB_IDLE;
static bool sdSecureJobActive = false;
static bool sdSecureJobDone = false;
static bool sdSecureAbortRequested = false;
static bool sdSecureAborted = false;
static bool sdSecureOperationOk = true;
static bool sdSecureHadSdConfig = false;
static bool sdSecurePreviousRecordingBlock = false;
static bool sdSecurePreviousStorageLock = false;
static uint64_t sdSecureCardTotalBytes = 0;
static uint64_t sdSecureTargetBytes = 0;
static uint64_t sdSecureOverwrittenBytes = 0;
static size_t sdSecureBufferSize = 0;
static uint8_t *sdSecureZeroBuffer = nullptr;
static File sdSecureEraseFile;
static String sdSecureConfigText;
static String sdSecureLastError;
static SdMaintenanceResult sdSecureFinalResult =
    SD_MAINT_RESULT_OK;


static const size_t SD_MAINT_CONFIG_MAX_BYTES =
    32U * 1024U;


static bool readTextFileForMaintenance(
    fs::FS &filesystem,
    const char *path,
    String &text
)
{
    File file =
        filesystem.open(
            path,
            FILE_READ
        );

    if (!file)
        return false;

    size_t size =
        file.size();

    if (
        size == 0 ||
        size > SD_MAINT_CONFIG_MAX_BYTES
    ) {
        file.close();
        return false;
    }

    text = "";
    text.reserve(size + 1U);

    while (file.available()) {
        char buffer[256];

        size_t got =
            file.readBytes(
                buffer,
                sizeof(buffer)
            );

        if (got == 0)
            break;

        text.concat(
            buffer,
            got
        );
    }

    file.close();

    return
        text.length() ==
        size;
}


static bool loadInternalConfigForSdRestore(
    String &configText,
    String &error
)
{
    error = "";

    if (
        !configInternalAvailable() ||
        !configInternalValid() ||
        !LittleFS.exists("/config.txt")
    ) {
        error =
            "internal config shadow unavailable or invalid";
        return false;
    }

    if (!readTextFileForMaintenance(
            LittleFS,
            "/config.txt",
            configText
        )) {
        error =
            "cannot read internal /config.txt";
        return false;
    }

    String validationError;

    if (!configValidateText(
            configText,
            validationError
        )) {
        error =
            "internal /config.txt validation failed: " +
            validationError;
        return false;
    }

    return true;
}


static bool restoreConfigToSd(
    const String &configText,
    String &error
)
{
    error = "";

    STORAGE.remove("/config.tmp");
    STORAGE.remove("/config.bak");

    File file =
        STORAGE.open(
            "/config.tmp",
            FILE_WRITE
        );

    if (!file) {
        error =
            "cannot create SD /config.tmp";
        return false;
    }

    size_t written =
        file.print(
            configText
        );

    file.flush();
    file.close();

    if (
        written !=
        configText.length()
    ) {
        STORAGE.remove("/config.tmp");
        error =
            "incomplete SD config write";
        return false;
    }

    String verifyText;

    if (
        !readTextFileForMaintenance(
            STORAGE,
            "/config.tmp",
            verifyText
        ) ||
        verifyText != configText
    ) {
        STORAGE.remove("/config.tmp");
        error =
            "SD config verification failed";
        return false;
    }

    String validationError;

    if (!configValidateText(
            verifyText,
            validationError
        )) {
        STORAGE.remove("/config.tmp");
        error =
            "restored SD config is invalid: " +
            validationError;
        return false;
    }

    bool hadOld =
        STORAGE.exists(
            "/config.txt"
        );

    if (hadOld) {
        if (!STORAGE.rename(
                "/config.txt",
                "/config.bak"
            )) {
            STORAGE.remove("/config.tmp");
            error =
                "cannot backup old SD config";
            return false;
        }
    }

    if (!STORAGE.rename(
            "/config.tmp",
            "/config.txt"
        )) {

        if (hadOld) {
            STORAGE.rename(
                "/config.bak",
                "/config.txt"
            );
        }

        STORAGE.remove("/config.tmp");
        error =
            "cannot promote restored SD config";
        return false;
    }

    STORAGE.remove("/config.bak");

    String finalText;

    if (
        !readTextFileForMaintenance(
            STORAGE,
            "/config.txt",
            finalText
        ) ||
        finalText != configText
    ) {
        error =
            "final SD config verification failed";
        return false;
    }

    return true;
}


static bool deleteTree(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str()
        );

    if (!root)
        return false;

    if (!root.isDirectory()) {
        root.close();
        return
            STORAGE.remove(
                path.c_str()
            );
    }

    bool ok = true;

    File file =
        root.openNextFile();

    while (file) {

        String name =
            String(file.name());

        String fullPath;

        if (name.startsWith("/")) {
            fullPath = name;
        } else if (path == "/") {
            fullPath =
                "/" + name;
        } else {
            fullPath =
                path + "/" + name;
        }

        bool isDir =
            file.isDirectory();

        file.close();

        if (isDir) {

            if (!deleteTree(fullPath))
                ok = false;

            if (
                fullPath != "/" &&
                STORAGE.exists(
                    fullPath.c_str()
                ) &&
                !STORAGE.rmdir(
                    fullPath.c_str()
                )
            ) {
                ok = false;
            }

        } else {

            // SD maintenance always rebuilds /config.txt afterwards from
            // the validated LittleFS shadow. Therefore the SD copy itself
            // is deliberately deleted like every other file here.
            if (!STORAGE.remove(
                    fullPath.c_str()
                )) {
                ok = false;
            }
        }

        serviceLongOperation();

        file =
            root.openNextFile();
    }

    root.close();

    serviceLongOperation();

    return ok;
}


static uint16_t sdMaintReadU16LE(
    const uint8_t *p
)
{
    return
        (uint16_t)p[0] |
        ((uint16_t)p[1] << 8);
}


static uint32_t sdMaintReadU32LE(
    const uint8_t *p
)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static uint64_t sdMaintReadU64LE(
    const uint8_t *p
)
{
    return
        (uint64_t)sdMaintReadU32LE(p) |
        ((uint64_t)sdMaintReadU32LE(p + 4) << 32);
}


static bool locateFatVolumeStart(
    uint32_t &volumeStart,
    uint16_t &backupBootSector,
    String &error
)
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED

    error = "";
    volumeStart = 0;
    backupBootSector = 0;

    const size_t sectorSize =
        512U;

    uint64_t cardBytes =
        STORAGE.cardSize();

    uint64_t sectorCount64 =
        cardBytes /
        sectorSize;

    if (
        sectorCount64 == 0 ||
        sectorCount64 > 0xFFFFFFFFULL
    ) {
        error =
            "invalid SD raw sector geometry";
        return false;
    }

    uint32_t sectorCount =
        (uint32_t)sectorCount64;

    uint8_t *sector =
        (uint8_t *)malloc(
            sectorSize
        );

    if (!sector) {
        error =
            "cannot allocate SD sector buffer";
        return false;
    }

    if (!STORAGE.readRAW(
            sector,
            0
        )) {
        free(sector);
        error =
            "cannot read SD sector 0";
        return false;
    }

    bool signature =
        sector[510] == 0x55 &&
        sector[511] == 0xAA;

    uint16_t bytesPerSector =
        (uint16_t)sector[11] |
        ((uint16_t)sector[12] << 8);

    uint8_t sectorsPerCluster =
        sector[13];

    uint16_t reservedSectors =
        (uint16_t)sector[14] |
        ((uint16_t)sector[15] << 8);

    uint8_t fatCount =
        sector[16];

    bool plausibleFatBpb =
        signature &&
        (bytesPerSector == 512 ||
         bytesPerSector == 1024 ||
         bytesPerSector == 2048 ||
         bytesPerSector == 4096) &&
        sectorsPerCluster > 0 &&
        (sectorsPerCluster &
         (sectorsPerCluster - 1U)) == 0 &&
        reservedSectors > 0 &&
        (fatCount == 1 ||
         fatCount == 2);

    bool exFatBoot =
        signature &&
        memcmp(
            &sector[3],
            "EXFAT   ",
            8
        ) == 0;

    bool volumeAtSectorZero =
        plausibleFatBpb ||
        exFatBoot;

    bool partitionFound =
        false;

    if (
        signature &&
        !volumeAtSectorZero
    ) {
        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {
            const uint8_t *entry =
                &sector[446U +
                    (uint16_t)i * 16U];

            uint8_t bootFlag =
                entry[0];

            uint8_t type =
                entry[4];

            uint32_t startLba =
                sdMaintReadU32LE(
                    &entry[8]
                );

            uint32_t count =
                sdMaintReadU32LE(
                    &entry[12]
                );

            if (type == 0xEE) {
                free(sector);
                error =
                    "GPT partition layout is not supported by SensorForge SD Format";
                return false;
            }

            bool plausible =
                (bootFlag == 0x00 ||
                 bootFlag == 0x80) &&
                type != 0x00 &&
                startLba > 0 &&
                startLba < sectorCount &&
                count > 0 &&
                count <=
                    sectorCount - startLba;

            if (plausible) {
                volumeStart =
                    startLba;
                partitionFound =
                    true;
                break;
            }
        }
    }

    if (
        partitionFound &&
        !STORAGE.readRAW(
            sector,
            volumeStart
        )
    ) {
        free(sector);
        error =
            "cannot read FAT volume boot sector";
        return false;
    }

    // FAT32 stores the backup boot sector number in BPB_BkBootSec.
    // For FAT12/16 these bytes are not used for this purpose; only
    // accept the value when the FAT32 signature is present.
    bool fat32 =
        sectorSize >= 90 &&
        memcmp(
            &sector[82],
            "FAT32   ",
            8
        ) == 0;

    if (fat32) {
        uint16_t reservedSectors =
            (uint16_t)sector[14] |
            ((uint16_t)sector[15] << 8);

        uint16_t backup =
            (uint16_t)sector[50] |
            ((uint16_t)sector[51] << 8);

        if (
            backup > 0 &&
            backup < reservedSectors &&
            (uint64_t)volumeStart +
                backup <
                sectorCount
        ) {
            backupBootSector =
                backup;
        }
    }

    free(sector);
    return true;

#else

    (void)volumeStart;
    (void)backupBootSector;
    error =
        "raw SD access is not available on this board";
    return false;

#endif
}


static bool invalidateFatFilesystem(
    String &error
)
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED

    uint32_t volumeStart = 0;
    uint16_t backupBootSector = 0;

    if (!locateFatVolumeStart(
            volumeStart,
            backupBootSector,
            error
        )) {
        return false;
    }

    const size_t sectorSize =
        512U;

    uint8_t *zeroSector =
        (uint8_t *)calloc(
            1,
            sectorSize
        );

    if (!zeroSector) {
        error =
            "cannot allocate format sector buffer";
        return false;
    }

    bool ok =
        STORAGE.writeRAW(
            zeroSector,
            volumeStart
        );

    if (
        ok &&
        backupBootSector > 0
    ) {
        ok =
            STORAGE.writeRAW(
                zeroSector,
                volumeStart +
                    backupBootSector
            );
    }

    free(zeroSector);

    if (!ok) {
        error =
            "cannot invalidate FAT boot sector";
        return false;
    }

    return true;

#else

    error =
        "raw SD access is not available on this board";
    return false;

#endif
}


static const size_t SENSORFORGE_SD_FORMAT_ALLOCATION_UNIT_BYTES =
    32U * 1024U;


static bool findMountedFatDrive(
    char (&drive)[3],
    FATFS *&filesystem,
    String &error
)
{
    error = "";
    filesystem = nullptr;

    uint64_t expectedTotalBytes =
        STORAGE.totalBytes();

    for (uint8_t index = 0; index < FF_VOLUMES; ++index) {
        char candidate[3] = {
            (char)('0' + index),
            ':',
            '\0'
        };

        DWORD freeClusters = 0;
        FATFS *candidateFilesystem = nullptr;

        if (
            f_getfree(
                candidate,
                &freeClusters,
                &candidateFilesystem
            ) != FR_OK ||
            !candidateFilesystem
        ) {
            continue;
        }

        uint64_t candidateTotalBytes =
            (uint64_t)candidateFilesystem->csize *
            (uint64_t)(candidateFilesystem->n_fatent - 2U) *
            512ULL;

        if (
            expectedTotalBytes > 0 &&
            candidateTotalBytes != expectedTotalBytes
        ) {
            continue;
        }

        drive[0] = candidate[0];
        drive[1] = candidate[1];
        drive[2] = '\0';
        filesystem = candidateFilesystem;
        return true;
    }

    error =
        "cannot identify mounted FAT drive";
    return false;
}


static bool formatSdFilesystem(
    String &error
)
{
#if defined(STORAGE_SPI) || defined(STORAGE_SDMMC)

    error = "";

    char drive[3] = { 0, 0, 0 };
    FATFS *filesystem = nullptr;

    if (!findMountedFatDrive(
            drive,
            filesystem,
            error
        )) {
        return false;
    }

    // Keep the Arduino VFS registration and physical SD driver intact. Only
    // detach the FatFs volume, format that already mounted logical drive, and
    // attach the same FATFS object again. This is backend-neutral for both
    // Arduino SD (SPI) and SD_MMC.
    FRESULT unmountResult =
        f_mount(
            nullptr,
            drive,
            0
        );

    if (unmountResult != FR_OK) {
        error =
            "FatFs unmount before format failed: " +
            String((int)unmountResult);
        return false;
    }

    MKFS_PARM formatOptions = {};
    formatOptions.fmt = FM_FAT32;
    formatOptions.n_fat = 2;
    formatOptions.align = 0;
    formatOptions.n_root = 0;
    formatOptions.au_size =
        SENSORFORGE_SD_FORMAT_ALLOCATION_UNIT_BYTES;

    const UINT workBufferBytes =
        4096U;

    uint8_t *workBuffer =
        (uint8_t *)malloc(
            workBufferBytes
        );

    if (!workBuffer) {
        f_mount(
            filesystem,
            drive,
            1
        );
        error =
            "cannot allocate FatFs format work buffer";
        return false;
    }

    FRESULT formatResult =
        f_mkfs(
            drive,
            &formatOptions,
            workBuffer,
            workBufferBytes
        );

    free(workBuffer);

    FRESULT remountResult =
        f_mount(
            filesystem,
            drive,
            1
        );

    if (formatResult != FR_OK) {
        error =
            "FAT32 format failed: " +
            String((int)formatResult);

        if (remountResult != FR_OK) {
            error +=
                "; remount also failed: " +
                String((int)remountResult);
        }

        return false;
    }

    if (remountResult != FR_OK) {
        error =
            "FAT32 formatted but remount failed: " +
            String((int)remountResult);
        return false;
    }

    DWORD freeClusters = 0;
    FATFS *verifiedFilesystem = nullptr;

    FRESULT verifyResult =
        f_getfree(
            drive,
            &freeClusters,
            &verifiedFilesystem
        );

    if (
        verifyResult != FR_OK ||
        !verifiedFilesystem
    ) {
        error =
            "formatted filesystem verification failed: " +
            String((int)verifyResult);
        return false;
    }

    uint64_t clusterBytes =
        (uint64_t)verifiedFilesystem->csize *
        512ULL;

    if (
        clusterBytes !=
        SENSORFORGE_SD_FORMAT_ALLOCATION_UNIT_BYTES
    ) {
        error =
            "formatted cluster size mismatch: " +
            String((unsigned long)clusterBytes) +
            " bytes";
        return false;
    }

    Serial.printf(
        "SensorForge SD format: FAT32 allocation unit %lu bytes verified\n",
        (unsigned long)clusterBytes
    );

    return true;

#else

    error =
        "SD formatting is unsupported by this storage backend";
    return false;

#endif
}


static bool overwriteFreeSpaceWithZeros(
    uint64_t &overwrittenBytes,
    String &error
)
{
    overwrittenBytes = 0;
    error = "";

    uint64_t total =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    if (
        total == 0 ||
        used > total
    ) {
        error =
            "cannot determine SD free space";
        return false;
    }

    uint64_t freeBefore =
        total - used;

    File eraseFile =
        STORAGE.open(
            "/.__sensorforge_secure_erase.bin",
            FILE_WRITE
        );

    if (!eraseFile) {
        error =
            "cannot create secure erase overwrite file";
        return false;
    }

    size_t bufferSize =
        32U * 1024U;

    uint8_t *zeroBuffer =
        (uint8_t *)calloc(
            1,
            bufferSize
        );

    if (!zeroBuffer) {
        bufferSize =
            4096U;

        zeroBuffer =
            (uint8_t *)calloc(
                1,
                bufferSize
            );
    }

    if (!zeroBuffer) {
        eraseFile.close();
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );
        error =
            "cannot allocate secure erase buffer";
        return false;
    }

    while (true) {

        size_t written =
            eraseFile.write(
                zeroBuffer,
                bufferSize
            );

        overwrittenBytes +=
            written;

        if (written < bufferSize)
            break;

        if (
            (overwrittenBytes &
             0x000FFFFFULL) <
            bufferSize
        ) {
            serviceLongOperation();
        }
    }

    eraseFile.flush();
    eraseFile.close();
    free(zeroBuffer);

    serviceLongOperation();

    // FAT bookkeeping needs a small amount of space of its own. Allow a
    // conservative tolerance, but require that practically all free clusters
    // were consumed by the zero-filled file.
    uint64_t tolerance =
        freeBefore / 50ULL;

    const uint64_t minTolerance =
        8ULL * 1024ULL * 1024ULL;

    if (tolerance < minTolerance)
        tolerance = minTolerance;

    if (tolerance > freeBefore)
        tolerance = freeBefore;

    bool sufficientlyCovered =
        overwrittenBytes +
            tolerance >=
        freeBefore;

    if (!sufficientlyCovered) {
        error =
            "secure overwrite stopped before covering the logical free area";
    }

    return sufficientlyCovered;
}


static bool sdFormatBackendSupported();


static void closeSecureEraseOverwriteFile()
{
    if (sdSecureEraseFile) {
        sdSecureEraseFile.flush();
        sdSecureEraseFile.close();
    }

    if (sdSecureZeroBuffer) {
        free(sdSecureZeroBuffer);
        sdSecureZeroBuffer = nullptr;
    }

    sdSecureBufferSize = 0;
}


static void releaseSecureEraseLocks()
{
    // The logger was closed when the job started. Reopen it only after the
    // current filesystem/config state is settled, then restore both gates.
    logInit();

    g_recordingStartBlocked =
        sdSecurePreviousRecordingBlock;

    g_storageLocked =
        sdSecurePreviousStorageLock;
}


static bool secureEraseCoverageSufficient()
{
    uint64_t tolerance =
        sdSecureTargetBytes / 50ULL;

    const uint64_t minTolerance =
        8ULL * 1024ULL * 1024ULL;

    if (tolerance < minTolerance)
        tolerance = minTolerance;

    if (tolerance > sdSecureTargetBytes)
        tolerance = sdSecureTargetBytes;

    return
        sdSecureOverwrittenBytes + tolerance >=
        sdSecureTargetBytes;
}


static SdMaintenanceResult beginSecureEraseJob()
{
    if (sdSecureJobActive) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (g_storageLocked) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (recorderIsOpen()) {
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    if (!sdFormatBackendSupported()) {
        return
            SD_MAINT_RESULT_UNSUPPORTED;
    }

    configRefreshSdStatus();

    bool hadSdConfig =
        configSdAvailable() &&
        configSdPresent();

    String configText;
    String error;

    if (!loadInternalConfigForSdRestore(
            configText,
            error
        )) {
        Serial.println(
            "SD maintenance aborted: " +
            error
        );
        return
            SD_MAINT_RESULT_CONFIG_SOURCE_FAILED;
    }

    sdSecurePreviousRecordingBlock =
        g_recordingStartBlocked;

    sdSecurePreviousStorageLock =
        g_storageLocked;

    g_storageLocked = true;
    g_recordingStartBlocked = true;

    webPlayerStop();

    if (recorderIsOpen()) {
        g_recordingStartBlocked =
            sdSecurePreviousRecordingBlock;
        g_storageLocked =
            sdSecurePreviousStorageLock;
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    logClose();

    sdSecureJobActive = true;
    sdSecureJobDone = false;
    sdSecureAbortRequested = false;
    sdSecureAborted = false;
    sdSecureOperationOk = true;
    sdSecureHadSdConfig =
        hadSdConfig;
    sdSecureCardTotalBytes = 0;
    sdSecureTargetBytes = 0;
    sdSecureOverwrittenBytes = 0;
    sdSecureLastError = "";
    sdSecureFinalResult =
        SD_MAINT_RESULT_OK;
    sdSecureConfigText =
        configText;

    Serial.println(
        "SensorForge SD maintenance: SECURE ERASE start"
    );

    // Deleting the directory tree is normally fast compared with the full
    // overwrite. It remains synchronous, while the long zero-fill itself is
    // chunked from webConfigLoop() so the browser can poll and abort it.
    bool wipeOk =
        deleteTree("/");

    if (!wipeOk) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "SD wipe failed before secure overwrite";
        Serial.println(
            "SensorForge SD secure wipe warning: " +
            sdSecureLastError
        );
    }

    sdSecureCardTotalBytes =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    if (
        sdSecureCardTotalBytes == 0 ||
        used > sdSecureCardTotalBytes
    ) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot determine SD free space";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureTargetBytes =
        sdSecureCardTotalBytes - used;

    sdSecureEraseFile =
        STORAGE.open(
            "/.__sensorforge_secure_erase.bin",
            FILE_WRITE
        );

    if (!sdSecureEraseFile) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot create secure erase overwrite file";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureBufferSize =
        32U * 1024U;

    sdSecureZeroBuffer =
        (uint8_t *)calloc(
            1,
            sdSecureBufferSize
        );

    if (!sdSecureZeroBuffer) {
        sdSecureBufferSize =
            4096U;

        sdSecureZeroBuffer =
            (uint8_t *)calloc(
                1,
                sdSecureBufferSize
            );
    }

    if (!sdSecureZeroBuffer) {
        closeSecureEraseOverwriteFile();
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot allocate secure erase buffer";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureJobStage =
        SD_SECURE_JOB_OVERWRITE;

    return
        SD_MAINT_RESULT_OK;
}


static void processSecureEraseJob()
{
    if (!sdSecureJobActive)
        return;

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_OVERWRITE
    ) {
        if (sdSecureAbortRequested) {
            sdSecureAborted = true;

            Serial.printf(
                "SensorForge SD secure overwrite aborted at %llu bytes\n",
                (unsigned long long)sdSecureOverwrittenBytes
            );

            closeSecureEraseOverwriteFile();

            // Removing the temporary file only frees FAT cluster metadata;
            // the bytes already written remain zeroed. This also leaves room
            // for config recovery if formatting itself should fail.
            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );

            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
            return;
        }

        if (
            !sdSecureEraseFile ||
            !sdSecureZeroBuffer ||
            sdSecureBufferSize == 0
        ) {
            sdSecureOperationOk = false;
            sdSecureLastError =
                "secure overwrite state invalid";
            closeSecureEraseOverwriteFile();
            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );
            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
            return;
        }

        size_t written =
            sdSecureEraseFile.write(
                sdSecureZeroBuffer,
                sdSecureBufferSize
            );

        sdSecureOverwrittenBytes +=
            written;

        if (
            (sdSecureOverwrittenBytes &
             0x000FFFFFULL) <
            sdSecureBufferSize
        ) {
            serviceLongOperation();
        }

        if (written < sdSecureBufferSize) {
            closeSecureEraseOverwriteFile();

            bool covered =
                secureEraseCoverageSufficient();

            if (!covered) {
                sdSecureOperationOk = false;
                sdSecureLastError =
                    "secure overwrite stopped before covering the logical free area";
            }

            Serial.printf(
                "SensorForge SD secure overwrite: %llu / %llu bytes%s\n",
                (unsigned long long)sdSecureOverwrittenBytes,
                (unsigned long long)sdSecureTargetBytes,
                covered ? "" : " (incomplete)"
            );

            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );

            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
        }

        return;
    }

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_FORMAT
    ) {
        closeSecureEraseOverwriteFile();

        // Ensure no giant temporary overwrite file remains if we arrived here
        // through Abort or an overwrite error.
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );

        String formatError;

        bool formatOk =
            formatSdFilesystem(
                formatError
            );

        if (!formatOk) {
            sdSecureOperationOk = false;
            sdSecureLastError =
                formatError;
            Serial.println(
                "SensorForge SD secure format failed: " +
                formatError
            );
        }

        sdSecureJobStage =
            SD_SECURE_JOB_RESTORE;
        return;
    }

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_RESTORE
    ) {
        String restoreError;

        bool restoreOk =
            true;

        if (sdSecureHadSdConfig) {
            restoreOk =
                restoreConfigToSd(
                    sdSecureConfigText,
                    restoreError
                );

            if (!restoreOk) {
                Serial.println(
                    "SensorForge SD config restore FAILED: " +
                    restoreError
                );
            } else {
                Serial.println(
                    "SensorForge SD config restored from internal flash"
                );
            }
        } else {
            Serial.println(
                "SensorForge SD config intentionally not restored | policy=internal-only"
            );
        }

        configRefreshSdStatus();
        releaseSecureEraseLocks();

        if (!restoreOk) {
            sdSecureFinalResult =
                SD_MAINT_RESULT_CONFIG_RESTORE_FAILED;
        } else if (!sdSecureOperationOk) {
            sdSecureFinalResult =
                SD_MAINT_RESULT_OPERATION_FAILED;
        } else {
            sdSecureFinalResult =
                SD_MAINT_RESULT_OK;
        }

        sdSecureConfigText = "";
        sdSecureJobActive = false;
        sdSecureJobDone = true;
        sdSecureJobStage =
            SD_SECURE_JOB_DONE;

        // Abort means "stop overwriting and continue with Format". If Format
        // and config-policy finalization succeeded, it is a successful controlled abort
        // and we still reboot to guarantee a completely fresh SD mount.
        if (
            sdSecureFinalResult ==
            SD_MAINT_RESULT_OK
        ) {
            scheduleReboot(3000UL);
        }
    }
}


static bool sdFormatBackendSupported()
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED
    return true;
#else
    return false;
#endif
}


static SdMaintenanceResult performSdMaintenance(
    SdMaintenanceMode mode
)
{
    // Another module may reserve the SD for a future critical operation.
    // Never start nested maintenance while the global storage gate is held.
    if (g_storageLocked) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (recorderIsOpen()) {
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    if (
        mode != SD_MAINT_WIPE &&
        !sdFormatBackendSupported()
    ) {
        return
            SD_MAINT_RESULT_UNSUPPORTED;
    }

    configRefreshSdStatus();

    bool hadSdConfig =
        configSdAvailable() &&
        configSdPresent();

    String configText;
    String error;

    // Absolutely no destructive SD step is started unless a valid copy is
    // already available in internal flash and can be validated in RAM.
    if (!loadInternalConfigForSdRestore(
            configText,
            error
        )) {
        Serial.println(
            "SD maintenance aborted: " +
            error
        );
        return
            SD_MAINT_RESULT_CONFIG_SOURCE_FAILED;
    }

    bool previousRecordingBlock =
        g_recordingStartBlocked;

    bool previousStorageLock =
        g_storageLocked;

    // Raise both gates before closing any SD users. The generic storage gate
    // is intentionally separate from the recording-start gate so future
    // modules can also refuse new SD work during maintenance.
    g_storageLocked =
        true;

    g_recordingStartBlocked =
        true;

    // Close player file handles before touching the filesystem. The WebServer
    // remains registered; webPlayerStop() only closes the playback session.
    webPlayerStop();

    if (recorderIsOpen()) {
        g_recordingStartBlocked =
            previousRecordingBlock;

        g_storageLocked =
            previousStorageLock;

        Serial.println(
            "SD maintenance aborted: recording became active"
        );

        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    // The logger keeps its SD File handle open for normal operation. It MUST
    // be closed before wipe/format/remount, otherwise the stale handle can
    // write into unrelated files after the filesystem has been rebuilt.
    logClose();

    bool operationOk =
        true;

    if (mode == SD_MAINT_WIPE) {

        Serial.println(
            "SensorForge SD maintenance: WIPE start"
        );

        operationOk =
            deleteTree("/");

    } else if (mode == SD_MAINT_FORMAT) {

        Serial.println(
            "SensorForge SD maintenance: FORMAT start"
        );

        operationOk =
            formatSdFilesystem(
                error
            );

    } else {

        Serial.println(
            "SensorForge SD maintenance: SECURE ERASE start"
        );

        bool wipeOk =
            deleteTree("/");

        uint64_t overwrittenBytes =
            0;

        String overwriteError;

        bool overwriteOk =
            overwriteFreeSpaceWithZeros(
                overwrittenBytes,
                overwriteError
            );

        Serial.printf(
            "SensorForge SD secure overwrite: %llu bytes\n",
            (unsigned long long)overwrittenBytes
        );

        if (!overwriteOk) {
            Serial.println(
                "SensorForge SD secure overwrite warning: " +
                overwriteError
            );
        }

        String formatError;

        bool formatOk =
            formatSdFilesystem(
                formatError
            );

        if (!formatOk) {
            Serial.println(
                "SensorForge SD secure format failed: " +
                formatError
            );
        }

        operationOk =
            wipeOk &&
            overwriteOk &&
            formatOk;
    }

    if (
        !operationOk &&
        error.length()
    ) {
        Serial.println(
            "SensorForge SD maintenance warning: " +
            error
        );
    }

    String restoreError;

    bool restoreOk =
        true;

    if (hadSdConfig) {
        restoreOk =
            restoreConfigToSd(
                configText,
                restoreError
            );

        if (!restoreOk) {
            Serial.println(
                "SensorForge SD config restore FAILED: " +
                restoreError
            );
        } else {
            Serial.println(
                "SensorForge SD config restored from internal flash"
            );
        }
    } else {
        Serial.println(
            "SensorForge SD config intentionally not restored | policy=internal-only"
        );
    }

    configRefreshSdStatus();

    // Reopen the logger only after the filesystem and config policy have been
    // settled. This guarantees a fresh File handle on the current mount.
    logInit();

    g_recordingStartBlocked =
        previousRecordingBlock;

    g_storageLocked =
        previousStorageLock;

    if (!restoreOk) {
        return
            SD_MAINT_RESULT_CONFIG_RESTORE_FAILED;
    }

    if (!operationOk) {
        return
            SD_MAINT_RESULT_OPERATION_FAILED;
    }

    return
        SD_MAINT_RESULT_OK;
}


static const char *sdMaintenanceResultLocation(
    SdMaintenanceMode mode,
    SdMaintenanceResult result
)
{
    if (
        result ==
        SD_MAINT_RESULT_CONFIG_SOURCE_FAILED
    ) {
        return
            "/?notice=sd_restore_source_failed";
    }

    if (
        result ==
        SD_MAINT_RESULT_CONFIG_RESTORE_FAILED
    ) {
        return
            "/?notice=sd_restore_failed";
    }

    if (
        result ==
        SD_MAINT_RESULT_RECORDING_ACTIVE
    ) {
        return
            "/?notice=sd_recording_active";
    }

    if (
        result ==
        SD_MAINT_RESULT_STORAGE_LOCKED
    ) {
        return
            "/?notice=sd_storage_locked";
    }

    if (
        result ==
        SD_MAINT_RESULT_UNSUPPORTED
    ) {
        return
            mode == SD_MAINT_SECURE_ERASE
            ? "/?notice=sd_secure_unsupported"
            : "/?notice=sd_format_unsupported";
    }

    if (mode == SD_MAINT_WIPE) {
        return
            result == SD_MAINT_RESULT_OK
            ? "/?notice=sd_wipe_done"
            : "/?notice=sd_wipe_failed";
    }

    if (mode == SD_MAINT_FORMAT) {
        return
            result == SD_MAINT_RESULT_OK
            ? "/?notice=sd_format_done"
            : "/?notice=sd_format_failed";
    }

    return
        result == SD_MAINT_RESULT_OK
        ? "/?notice=sd_secure_done"
        : "/?notice=sd_secure_failed";
}


static void redirectSdMaintenanceResult(
    SdMaintenanceMode mode,
    SdMaintenanceResult result
)
{
    // A real filesystem rebuild changes the SD mount underneath several
    // subsystems. Even though all known SD users are closed/reopened during
    // maintenance, reboot after a successful Format/Secure Erase gives every
    // module a completely fresh mount and avoids stale state in future code.
    //
    // Normal Format returns directly to the dashboard. The dashboard already
    // renders the one-shot success notice and immediately replaces the browser
    // history URL with "/", so a later reload cannot get stuck on a stale
    // "format completed" subpage. Secure Erase keeps its dedicated reboot page
    // because that path has its own asynchronous progress workflow.
    if (
        result == SD_MAINT_RESULT_OK &&
        (
            mode == SD_MAINT_FORMAT ||
            mode == SD_MAINT_SECURE_ERASE
        )
    ) {
        scheduleReboot(3000UL);

        webServer().sendHeader(
            "Location",
            mode == SD_MAINT_FORMAT
            ? "/?notice=sd_format_done"
            : "/rebooting?reason=sd_secure"
        );

        webServer().send(
            303,
            "text/plain; charset=utf-8",
            ""
        );

        return;
    }

    const char *location =
        sdMaintenanceResultLocation(
            mode,
            result
        );

    webServer().sendHeader(
        "Location",
        location
    );

    webServer().send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static String sdMaintenancePage(
    const SdMaintenanceViewState &view
)
{
    bool de =
        cfg_web_language !=
        "en";

    uint64_t total =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    uint64_t freeBytes =
        total > used
        ? total - used
        : 0;

    String html =
        pageHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>" + translatedHtml(UI_NAV_SD_MAINTENANCE) + "</h2>"
        "<p>" +
        String(
            de
            ? "Status, Recovery, Benchmark und Wartung der SD-Karte"
            : "SD card status, recovery, benchmark and maintenance"
        ) +
        "</p></div></div>";

    html +=
        "<section id='status' class='settings-section'>"
        "<h3>SD Status</h3>"
        "<p><span class='status-pill " +
        String(sdReady ? "ok" : "danger") +
        "'>" +
        String(
            sdReady
            ? (de ? "SD BEREIT" : "SD READY")
            : (de ? "SD NICHT VERFÜGBAR" : "SD UNAVAILABLE")
        ) +
        "</span></p>"
        "<div class='dashboard-grid'>"
        "<div class='dash-card'><div class='card-label'>Total</div><div class='card-value'>" +
        String((unsigned long)(total / 1024ULL / 1024ULL)) +
        " MB</div></div>"
        "<div class='dash-card'><div class='card-label'>Used</div><div class='card-value'>" +
        String((unsigned long)(used / 1024ULL / 1024ULL)) +
        " MB</div></div>"
        "<div class='dash-card'><div class='card-label'>Free</div><div class='card-value'>" +
        String((unsigned long)(freeBytes / 1024ULL / 1024ULL)) +
        " MB</div></div>"
        "</div>"
        "<p style='margin-top:14px'><a class='button' href='/sd_maintenance#status'>" +
        String(de ? "SD STATUS AKTUALISIEREN" : "REFRESH SD STATUS") +
        "</a></p></section>";

    html +=
        "<section id='recovery' class='settings-section'>"
        "<h3>" + translatedHtml(UI_NAV_SD_RECOVERY) + "</h3>"
        "<p class='muted'>" +
        String(
#if defined(STORAGE_SPI)
            de
            ? "Nicht-destruktive Wiederbelebung und Diagnose der SPI-SD. Es werden keine RAW-Schreiboperationen ausgeführt."
            : "Non-destructive SPI SD recovery and diagnostics. No raw-sector writes are performed."
#else
            de
            ? "Recovery des SD-Dateisystems über den normalen Storage-Recovery-Pfad. Es wird weder formatiert noch gewiped; nach erfolgreichem Mount kann die bestehende Recovery unvollständige temporäre Aufnahmedateien bereinigen."
            : "SD filesystem recovery through the normal storage recovery path. It does not format or wipe the card; after a successful mount the existing recovery may clean up incomplete temporary recording files."
#endif
        ) +
        "</p>";

    if (view.recoveryAttempted) {
        html +=
            "<p><span class='status-pill " +
            String(
                view.recoveryNotRequired || view.recoverySuccess
                ? "ok"
                : "danger"
            ) +
            "'>" +
            String(
                view.recoveryNotRequired
                ? (de ? "RECOVERY NICHT ERFORDERLICH" : "RECOVERY NOT REQUIRED")
                : (
                    view.recoverySuccess
                    ? (de ? "RECOVERY ERFOLGREICH" : "RECOVERY SUCCESSFUL")
                    : (de ? "RECOVERY NICHT ERFOLGREICH" : "RECOVERY NOT SUCCESSFUL")
                )
            ) +
            "</span></p>";

        if (
            view.recoverySuccess &&
            view.recoveryMountedFrequencyHz > 0
        ) {
            html +=
                "<p class='muted'>" +
                String(de ? "Gemounteter SPI-Takt: " : "Mounted SPI clock: ") +
                String(
                    (double)view.recoveryMountedFrequencyHz /
                    1000000.0,
                    3
                ) +
                " MHz</p>";
        }

#if defined(STORAGE_SPI)
        if (!view.recoveryNotRequired) {
            html +=
                "<p class='muted'>RAW controller: <b>" +
                String(view.recoveryRawCardReady ? "READY" : "NO READY") +
                "</b> &middot; Sector 0: <b>" +
                String(view.recoverySector0Readable ? "READABLE" : "NOT READABLE") +
                "</b></p>";
        }
#endif

        if (view.recoveryReport.length()) {
            html +=
                "<pre style='white-space:pre-wrap;word-break:break-word;max-height:420px;overflow:auto;padding:12px;"
                "border:1px solid var(--border);border-radius:10px;background:var(--surface-2)'>" +
                escapeHtml(view.recoveryReport) +
                "</pre>";
        }
    }

    html +=
        "<p><span class='status-pill " +
        String(sdReady ? "ok" : "danger") +
        "'>" +
        String(
            sdReady
            ? (de ? "SD GEMOUNTET" : "SD MOUNTED")
            : (de ? "SD NICHT VERFÜGBAR" : "SD UNAVAILABLE")
        ) +
        "</span></p>"
        "<p class='muted'>" +
        String(
            sdReady
            ? (
                de
                ? "Der Recovery-Button bleibt bewusst sichtbar. Bei einer betriebsbereiten SD bestätigt er den Zustand, ohne die aktive Karte unnötig neu zu mounten."
                : "The recovery button remains visible. With an operational SD it confirms the state without unnecessarily remounting the active card."
            )
            : (
#if defined(STORAGE_SPI)
                de
                ? "Die Recovery übernimmt exklusiv den SPI-Bus, prüft den Controller und Sektor 0 read-only und versucht anschließend den Filesystem-Mount mit mehreren SPI-Takten."
                : "Recovery takes exclusive ownership of the SPI bus, checks the controller and sector 0 read-only, then retries the filesystem mount at several SPI clocks."
#else
                de
                ? "Die Recovery versucht die SD über den bestehenden robusten Mount-/Retry-Pfad wieder in Betrieb zu nehmen. Format und Wipe werden dabei nicht ausgeführt."
                : "Recovery attempts to restore the SD through the existing robust mount/retry path. Format and wipe are not performed."
#endif
            )
        ) +
        "</p>";

    if (recording) {
        html +=
            "<button class='primary' type='button' disabled>" +
            String(de ? "SD RECOVERY – AUFNAHME AKTIV" : "SD RECOVERY – RECORDING ACTIVE") +
            "</button>";
    } else {
        html +=
            "<form method='POST' action='/sd_recovery_run#recovery' "
            "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('button').textContent='" +
            String(de ? "Recovery wird geprüft..." : "Checking recovery...") +
            "';\">"
            "<button class='primary' type='submit'>" +
            String(
#if defined(STORAGE_SPI)
                "READ-ONLY SD RECOVERY"
#else
                de
                ? "SD RECOVERY STARTEN"
                : "START SD RECOVERY"
#endif
            ) +
            "</button></form>";
    }

    html +=
        "</section>";

    html +=
        "<section id='benchmark' class='settings-section'>"
        "<h3>" + translatedHtml(UI_NAV_SD_BENCHMARK) + "</h3>"
        "<p class='muted'>" +
        String(
            de
            ? "Erweiterte, nicht-destruktive Storage-Diagnose. SensorForge prüft FAT-/Cluster-Geometrie, 4/16/32/64 KiB Blockgrößen und Allokationskosten, führt zwei 64-MiB-Rohdatei-Endurance-Läufe aus und danach einen dritten 64-MiB-Test durch den echten RecordingWriteBufferedFile -> RecordingStorageFile -> SFENC1-Pfad mit logischem Entschlüsselungs-/Readback-Verify. Anschließend folgt der Bus-Takt-Vergleich."
            : "Extended non-destructive storage diagnostics. SensorForge reports FAT/cluster geometry, tests 4/16/32/64 KiB blocks and allocation cost, runs two 64 MiB raw-file endurance passes, then runs a third 64 MiB test through the real RecordingWriteBufferedFile -> RecordingStorageFile -> SFENC1 stack with logical decrypt/read-back verification. All written data is verified before the bus-clock comparison."
        ) +
        "</p>"
        "<p class='muted'>" +
        String(
            de
            ? "Der Lauf kann auf langsamen Karten deutlich länger dauern. Die Endurance-Dateien sind temporär und physisch maximal etwa 65 MiB groß. Ein echtes EIO wird auf demselben Mount nicht wiederholt. Nur wenn der Produktionstakt über 10 MHz liegt und der SFENC1-Stack-Test dort mit EIO scheitert, darf er nach sauberem Remount einmal bei 10 MHz wiederholt werden; danach wird der Produktionstakt wiederhergestellt. Es wird nicht formatiert."
            : "The run can take substantially longer on slow cards. Endurance files are temporary and at most about 65 MiB physical size. A real EIO is never retried on the same mount. When the production clock is above 10 MHz, the SFENC1 stack test may additionally repeat once at 10 MHz only after a clean remount when the production-clock run failed with EIO, then restores the production mount. No formatting is performed."
        ) +
        "</p>";

    if (view.benchmarkAttempted) {
        SdBenchmarkRating rating =
            view.benchmarkRating;

        html +=
            "<div class='flash-notice' style='border-left-color:" +
            String(
                rating == SD_BENCH_RATING_GREEN
                ? "#2e7d32"
                : (
                    rating == SD_BENCH_RATING_ORANGE
                    ? "#d98200"
                    : "#b3261e"
                )
            ) +
            ";background:" +
            String(
                rating == SD_BENCH_RATING_GREEN
                ? "#eef8ef"
                : (
                    rating == SD_BENCH_RATING_ORANGE
                    ? "#fff6e5"
                    : "#fff0ef"
                )
            ) +
            "'>"
            "<strong><span class='status-pill " +
            String(sdBenchmarkRatingPillClass(rating)) +
            "'>" +
            sdBenchmarkRatingTitle(rating, de) +
            "</span></strong>";

        if (rating == SD_BENCH_RATING_GREEN) {
            html +=
                "<span class='muted'>" +
                String(
                    de
                    ? "Die gemessene Storage-Strecke ist für den normalen SensorForge-Betrieb gut geeignet. Keine Maßnahme erforderlich."
                    : "The measured storage path is suitable for normal SensorForge operation. No action is required."
                ) +
                "</span>";
        } else if (rating == SD_BENCH_RATING_ORANGE) {
            html +=
                "<span class='muted'>" +
                String(
                    de
                    ? "Die Storage-Strecke ist nutzbar, sollte aber vor produktivem Einsatz geprüft bzw. optimiert werden."
                    : "The storage path is usable but should be checked or optimized before production use."
                ) +
                "</span>";
        } else {
            html +=
                "<span class='muted'>" +
                String(
                    de
                    ? "Die gemessene Storage-Strecke ist für zuverlässige Aufnahmen derzeit nicht empfohlen. Ursache beheben und Benchmark wiederholen."
                    : "The measured storage path is currently not recommended for reliable recording. Resolve the cause and rerun the benchmark."
                ) +
                "</span>";
        }

        html += "</div>";

        if (
            view.benchmarkGeometryAvailable &&
            view.benchmarkClusterBytes > 0 &&
            view.benchmarkClusterBytes < 32U * 1024U
        ) {
            html +=
                "<div class='flash-notice' style='border-left-color:#d98200;background:#fff6e5'>"
                "<strong>" +
                String(de ? "Clustergröße zu klein" : "Cluster size too small") +
                "</strong><span class='muted'>" +
                String(
                    de
                    ? "Aktuell: "
                    : "Current: "
                ) +
                String((double)view.benchmarkClusterBytes / 1024.0, 1) +
                " KiB. " +
                String(
                    de
                    ? "SensorForge empfiehlt 32 KiB. SD Format erzeugt diese Geometrie; dabei werden alle SD-Daten gelöscht. Danach den Benchmark erneut ausführen."
                    : "SensorForge recommends 32 KiB. SD Format creates this geometry; all SD data will be erased. Rerun the benchmark afterwards."
                ) +
                "</span><a class='button' href='#format' style='margin-top:10px'>" +
                String(de ? "ZU SD FORMAT" : "GO TO SD FORMAT") +
                "</a></div>";
        }

        if (view.benchmarkReferenceWriteMBps > 0.0f) {
            html +=
                "<div class='dashboard-grid'>"
                "<div class='dash-card'><div class='card-label'>32 KiB Write</div><div class='card-value'>" +
                String(view.benchmarkReferenceWriteMBps, 2) +
                " MB/s</div></div>"
                "<div class='dash-card'><div class='card-label'>32 KiB Read</div><div class='card-value'>" +
                String(view.benchmarkReferenceReadMBps, 2) +
                " MB/s</div></div>"
                "<div class='dash-card'><div class='card-label'>Write P95</div><div class='card-value'>" +
                String((double)view.benchmarkReferenceWriteP95Us / 1000.0, 2) +
                " ms</div></div>"
                "<div class='dash-card'><div class='card-label'>Write Worst</div><div class='card-value'>" +
                String((double)view.benchmarkReferenceWriteWorstUs / 1000.0, 2) +
                " ms</div></div>"
                "<div class='dash-card'><div class='card-label'>Flush</div><div class='card-value'>" +
                String((double)view.benchmarkReferenceFlushUs / 1000.0, 2) +
                " ms</div></div>"
                "<div class='dash-card'><div class='card-label'>Verify</div><div class='card-value'>" +
                String(view.benchmarkReferenceVerifyOk ? "OK" : "FAIL") +
                "</div></div>"
                "</div>";
        }

        html +=
            "<p><b>" +
            String(de ? "Was jetzt?" : "What next?") +
            "</b> ";

        if (!view.benchmarkSuccess || !view.benchmarkReferenceVerifyOk) {
            html +=
                String(
                    de
                    ? "Fehler zuerst beheben; die SD nicht für produktive Aufnahmen freigeben."
                    : "Resolve the error first; do not approve the SD for production recording."
                );
        } else if (
            view.benchmarkGeometryAvailable &&
            view.benchmarkClusterBytes > 0 &&
            view.benchmarkClusterBytes < 32U * 1024U
        ) {
            html +=
                String(
                    de
                    ? "SD Format auf dieser Seite ausführen (32-KiB-Cluster, Datenverlust), danach Benchmark erneut starten."
                    : "Run SD Format on this page (32 KiB clusters, data loss), then rerun the benchmark."
                );
        } else if (rating == SD_BENCH_RATING_GREEN) {
            html +=
                String(
                    de
                    ? "Keine weitere Maßnahme. Bei neuer SD-Karte oder neuem Board den Benchmark erneut ausführen."
                    : "No further action. Rerun the benchmark for a new SD card or a new board."
                );
        } else {
            html +=
                String(
                    de
                    ? "Clustergröße, Bus-Takt und SD-Karte prüfen; nach jeder Änderung Benchmark erneut ausführen."
                    : "Check cluster size, bus clock and SD card; rerun the benchmark after each change."
                );
        }

        html +=
            "</p><p class='muted'>" +
            String(de ? "Gesamtdauer: " : "Total duration: ") +
            String(view.benchmarkElapsedMs) +
            " ms</p>";

        if (view.benchmarkError.length()) {
            html +=
                "<p class='muted'><b>" +
                String(de ? "Fehler: " : "Error: ") +
                "</b>" +
                escapeHtml(view.benchmarkError) +
                "</p>";
        }

        if (view.benchmarkReport.length()) {
            html +=
                "<details style='margin-top:14px'>"
                "<summary><b>" +
                String(de ? "Technische Diagnose-Details" : "Technical diagnostic details") +
                "</b></summary>"
                "<pre style='white-space:pre-wrap;word-break:break-word;max-height:620px;overflow:auto;padding:12px;"
                "border:1px solid var(--border);border-radius:10px;background:var(--surface-2);margin-top:10px'>" +
                escapeHtml(view.benchmarkReport) +
                "</pre></details>";
        }
    }

    if (recording) {
        html +=
            "<p><span class='status-pill warn'>" +
            String(
                de
                ? "Während einer Aufnahme nicht verfügbar"
                : "Unavailable while recording"
            ) +
            "</span></p>"
            "<button type='button' disabled>" +
            String(de ? "SD BENCHMARK – AUFNAHME AKTIV" : "SD BENCHMARK – RECORDING ACTIVE") +
            "</button>";
    } else {
        html +=
            "<form id='sdBenchmarkForm' method='POST' action='/sd_benchmark_run#benchmark' "
            "onsubmit=\"this.querySelector('button').disabled=true;this.querySelector('button').textContent='" +
            String(de ? "SD DIAGNOSE LÄUFT..." : "SD DIAGNOSTICS RUNNING...") +
            "';showSdBusy('benchmark');\">"
            "<button id='sdBenchmarkButton' type='submit'>" +
            String(de ? "ERWEITERTEN SD BENCHMARK STARTEN" : "START EXTENDED SD BENCHMARK") +
            "</button></form>";
    }

    html +=
        "</section>"
        "<div class='page-title' style='margin-top:28px'><div>"
        "<h2>" +
        String(de ? "Destruktive SD-Wartung" : "Destructive SD maintenance") +
        "</h2><p>" +
        String(
            de
            ? "Wipe, Format und Secure Erase"
            : "Wipe, format and secure erase"
        ) +
        "</p></div></div>";

    html +=
        "<div class='flash-notice' style='border-left-color:var(--accent);background:#eef4ff'>"
        "<strong style='color:#174ea6'>Config-Schutz</strong>"
        "<span class='muted'>Vor jedem Vorgang wird die gültige interne LittleFS-<code>/config.txt</code> geprüft. "
        "War vor dem Vorgang eine SD-<code>/config.txt</code> vorhanden, wird sie danach aus der internen Kopie wiederhergestellt. "
        "War keine SD-config.txt vorhanden, bleibt SensorForge bewusst im Internal-only-Modus und es wird keine neue Datei erzeugt. "
        "Ist die interne Config nicht gültig, wird die Operation vollständig abgebrochen.</span>"
        "</div>";

    html +=
        "<section class='settings-section'>"
        "<span class='status-pill warn'>WIPE</span>"
        "<h3 style='margin-top:12px'>SD Wipe</h3>"
        "<p class='muted'>Löscht Dateien und Ordner über das vorhandene FAT-Dateisystem. "
        "Das Dateisystem selbst wird nicht neu erzeugt.</p>"
        "<form id='sdWipeForm' method='POST' action='/sdformat_do'>"
        "<button id='sdWipeButton' class='danger' type='submit'>SD WIPE STARTEN</button>"
        "</form>"
        "</section>";

    html +=
        "<section id='format' class='settings-section'>"
        "<span class='status-pill danger'>FORMAT</span>"
        "<h3 style='margin-top:12px'>SD Format</h3>"
        "<p class='muted'>Erzeugt das FAT-Dateisystem als FAT32 mit 32-KiB-Clustern neu. "
        "Alte Daten können trotz Formatierung forensisch teilweise rekonstruierbar bleiben. "
        "Nach erfolgreicher Formatierung wird SensorForge automatisch neu gestartet.</p>";

    if (sdFormatBackendSupported()) {
        html +=
            "<form id='sdRealFormatForm' method='POST' action='/sd_format_do'>"
            "<button id='sdRealFormatButton' class='danger' type='submit'>SD FORMAT STARTEN</button>"
            "</form>";
    } else {
        html +=
            "<p class='status-pill danger'>Auf diesem Storage-Backend nicht unterstützt</p>";
    }

    html +=
        "</section>";

    html +=
        "<section class='settings-section' style='border-color:#e0a8a3'>"
        "<span class='status-pill danger'>SECURE ERASE</span>"
        "<h3 style='margin-top:12px'>Secure Erase &ndash; Logical Overwrite + Format</h3>"
        "<p class='muted'>Löscht zunächst alle Dateien, überschreibt danach den logisch freien "
        "Datenbereich mit Nullen und formatiert anschließend neu. Das kann je nach Kartengröße "
        "sehr lange dauern. Während des Überschreibens werden Datenmenge und Fortschritt live angezeigt. "
        "Ein Abbruch stoppt nur das weitere Überschreiben; die Formatierung wird danach trotzdem ausgeführt. "
        "Nach erfolgreichem Abschluss wird SensorForge automatisch neu gestartet.</p>"
        "<p class='muted'><b>Wichtig:</b> Wegen Wear-Leveling und internen Reserveblöcken einer SD-Karte "
        "ist keine forensische Garantie für physisch nicht mehr auslesbare NAND-Zellen möglich.</p>";

    if (sdFormatBackendSupported()) {
        html +=
            "<form id='sdSecureForm' method='POST' action='/sd_secure_erase_do'>"
            "<button id='sdSecureButton' class='danger' type='submit'>SECURE ERASE STARTEN</button>"
            "</form>";
    } else {
        html +=
            "<p class='status-pill danger'>Auf diesem Storage-Backend nicht unterstützt</p>";
    }

    html +=
        "</section>"
        "<p id='sdOperationProgress' class='muted' style='display:none'>"
        "SD-Wartung läuft. Aufnahme-Starts und normale SD-Zugriffe sind während des Vorgangs gesperrt. "
        "Bitte Stromversorgung und SD-Karte nicht unterbrechen.</p>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "<style>"
        ".sd-busy-card{width:min(580px,100%);text-align:center;}"
        ".sd-busy-icon{font-size:2rem;line-height:1;margin:4px 0 10px;}"
        ".sd-busy-progress{position:relative;height:14px;margin:20px 0 8px;background:#e5e9ef;border-radius:999px;overflow:hidden;}"
        ".sd-busy-progress>span{display:block;height:100%;width:0;background:var(--danger);border-radius:999px;"
            "transition:width .25s linear;}"
        ".sd-busy-progress>span.indeterminate{width:34%;transition:none;animation:sdBusyMove 1.15s ease-in-out infinite;}"
        ".sd-busy-progress>span.benchmark-dot{position:absolute;top:1px;left:0;width:12px;height:12px;"
            "background:var(--danger);border-radius:50%;transition:none;animation:sdBenchmarkDot 1.15s ease-in-out infinite;"
            "box-shadow:0 0 0 3px rgba(179,38,30,.12);}"
        ".sd-busy-note{font-size:.88rem;color:var(--muted);margin-top:10px;}"
        ".sd-secure-metrics{font-size:.9rem;font-variant-numeric:tabular-nums;color:var(--text);margin-top:8px;}"
        ".sd-abort-button{margin-top:18px;width:100%;}"
        "@keyframes sdBusyMove{"
            "0%{transform:translateX(-115%);}"
            "50%{transform:translateX(98%);}"
            "100%{transform:translateX(290%);}"
        "}"
        "@keyframes sdBenchmarkDot{"
            "0%{left:0;opacity:.35;}"
            "12%{opacity:1;}"
            "88%{opacity:1;}"
            "100%{left:calc(100% - 12px);opacity:.35;}"
        "}"
        "</style>"
        "<div id='sdBusyModal' class='modal-backdrop' hidden>"
            "<div class='modal-card sd-busy-card' role='dialog' aria-modal='true' "
                "aria-labelledby='sdBusyTitle' aria-describedby='sdBusyText'>"
                "<span id='sdBusyPill' class='status-pill danger'>SD-WARTUNG</span>"
                "<div class='sd-busy-icon' aria-hidden='true'>&#9888;</div>"
                "<h3 id='sdBusyTitle'>SD-Wartung läuft</h3>"
                "<p id='sdBusyText'>Bitte warten.</p>"
                "<div class='sd-busy-progress' aria-hidden='true'>"
                    "<span id='sdBusyProgressFill' class='indeterminate'></span>"
                "</div>"
                "<div id='sdBusyProgressText' class='sd-secure-metrics'></div>"
                "<div id='sdBusyNote' class='sd-busy-note'>"
                    "Stromversorgung und SD-Karte jetzt nicht unterbrechen."
                "</div>"
                "<button id='sdSecureAbortButton' class='danger sd-abort-button' type='button' hidden>"
                    "ÜBERSCHREIBEN ABBRECHEN &amp; FORMATIEREN"
                "</button>"
            "</div>"
        "</div>"
        "<script>"
        "function sdFmtBytes(v){"
            "v=Number(v||0);"
            "if(v>=1073741824)return (v/1073741824).toFixed(2)+' GB';"
            "return (v/1048576).toFixed(1)+' MB';"
        "}"
        "function sdSetIndeterminate(on){"
            "var f=document.getElementById('sdBusyProgressFill');"
            "if(!f)return;"
            "f.classList.remove('benchmark-dot');"
            "if(on){f.classList.add('indeterminate');f.style.width='';}"
            "else{f.classList.remove('indeterminate');f.style.transform='none';}"
        "}"
        "function sdSetBenchmarkDot(){"
            "var f=document.getElementById('sdBusyProgressFill');"
            "if(!f)return;"
            "f.classList.remove('indeterminate');"
            "f.style.width='';"
            "f.style.transform='none';"
            "f.classList.add('benchmark-dot');"
        "}"
        "function showSdBusy(kind){"
            "var m=document.getElementById('sdBusyModal');"
            "var pill=document.getElementById('sdBusyPill');"
            "var title=document.getElementById('sdBusyTitle');"
            "var text=document.getElementById('sdBusyText');"
            "var note=document.getElementById('sdBusyNote');"
            "var metrics=document.getElementById('sdBusyProgressText');"
            "var abort=document.getElementById('sdSecureAbortButton');"
            "var icon=m?m.querySelector('.sd-busy-icon'):null;"
            "if(!m)return;"
            "if(abort){abort.hidden=true;abort.disabled=false;abort.textContent='ÜBERSCHREIBEN ABBRECHEN & FORMATIEREN';}"
            "if(icon){icon.textContent='⚠';icon.style.color='';}"
            "if(kind==='benchmark'){"
                "pill.textContent='SD DIAGNOSTIC';"
                "if(icon){icon.textContent='●';icon.style.color='var(--danger)';}"
                "title.textContent='" + String(de ? "SD-Diagnose läuft" : "SD diagnostics in progress") + "';"
                "text.textContent='" + String(de ? "SensorForge prüft Schreib- und Leserate, Latenzen, Datenintegrität und Bus-Takte." : "SensorForge is testing write/read throughput, latency, data integrity and bus clocks.") + "';"
                "note.textContent='" + String(de ? "Bitte diese Seite geöffnet lassen und SD-Karte sowie Stromversorgung nicht unterbrechen." : "Keep this page open and do not interrupt the SD card or power supply.") + "';"
                "if(metrics)metrics.textContent='" + String(de ? "Mehrere verifizierte Testläufe werden ausgeführt …" : "Running multiple verified test passes …") + "';"
                "sdSetBenchmarkDot();"
            "}else if(kind==='format'){"
                "pill.textContent='FORMAT';"
                "title.textContent='SD-Karte wird formatiert';"
                "text.textContent='Das FAT-Dateisystem wird jetzt neu aufgebaut. Nach erfolgreichem Abschluss startet SensorForge automatisch neu.';"
                "note.textContent='Bitte warten. Stromversorgung und SD-Karte jetzt nicht unterbrechen.';"
                "if(metrics)metrics.textContent='Formatierung läuft …';"
                "sdSetIndeterminate(true);"
            "}else if(kind==='secure'){"
                "pill.textContent='SECURE ERASE';"
                "title.textContent='Secure Erase wird vorbereitet';"
                "text.textContent='Dateien werden gelöscht, danach wird der logisch freie Datenbereich mit Nullen überschrieben.';"
                "note.textContent='Danach wird die SD-Karte automatisch formatiert und SensorForge neu gestartet.';"
                "if(metrics)metrics.textContent='Löschbereich wird ermittelt …';"
                "sdSetIndeterminate(false);"
                "var fill=document.getElementById('sdBusyProgressFill');if(fill)fill.style.width='0%';"
            "}"
            "m.hidden=false;"
            "document.body.style.overflow='hidden';"
        "}"
        "function updateSecureProgress(s){"
            "var title=document.getElementById('sdBusyTitle');"
            "var text=document.getElementById('sdBusyText');"
            "var note=document.getElementById('sdBusyNote');"
            "var metrics=document.getElementById('sdBusyProgressText');"
            "var fill=document.getElementById('sdBusyProgressFill');"
            "var abort=document.getElementById('sdSecureAbortButton');"
            "var pct=Math.max(0,Math.min(100,Number(s.percentX10||0)/10));"
            "sdSetIndeterminate(false);"
            "if(fill)fill.style.width=pct.toFixed(1)+'%';"
            "var amount=sdFmtBytes(s.overwrittenBytes)+' / '+sdFmtBytes(s.targetBytes)+' Löschbereich ('+pct.toFixed(1)+' %)';"
            "if(Number(s.totalBytes||0)>0)amount+=' · SD gesamt '+sdFmtBytes(s.totalBytes);"
            "if(metrics)metrics.textContent=amount;"
            "if(s.stage==='overwrite'){"
                "title.textContent='Secure Erase – Überschreiben';"
                "text.textContent='Der logisch freie Datenbereich wird mit Nullen überschrieben.';"
                "note.textContent=s.abortRequested?'Abbruch angefordert. Das Überschreiben wird beendet und anschließend formatiert.':'Mit Abbrechen wird nur das weitere Überschreiben gestoppt; anschließend wird trotzdem formatiert.';"
                "if(abort){abort.hidden=false;abort.disabled=!!s.abortRequested;abort.textContent=s.abortRequested?'ABBRUCH ANGEFORDERT …':'ÜBERSCHREIBEN ABBRECHEN & FORMATIEREN';}"
            "}else if(s.stage==='format'){"
                "title.textContent=s.aborted?'Überschreiben abgebrochen – Formatierung läuft':'Überschreiben abgeschlossen – Formatierung läuft';"
                "text.textContent='Das FAT-Dateisystem wird jetzt neu aufgebaut.';"
                "note.textContent='Stromversorgung und SD-Karte jetzt nicht unterbrechen.';"
                "if(abort)abort.hidden=true;"
            "}else if(s.stage==='restore'){"
                "title.textContent=s.hadSdConfig?'Konfiguration wird wiederhergestellt':'Config-Policy wird abgeschlossen';"
                "text.textContent=s.hadSdConfig?'Die vorher vorhandene config.txt wird aus dem internen Flash auf die SD-Karte zurückkopiert und geprüft.':'Vor dem Secure Erase war keine SD-config.txt vorhanden. Internal-only bleibt erhalten; es wird keine config.txt auf SD erzeugt.';"
                "note.textContent='SensorForge startet danach automatisch neu.';"
                "if(abort)abort.hidden=true;"
            "}"
        "}"
        "var sdSecurePollTimer=0;"
        "function pollSecureStatus(){"
            "fetch('/sd_secure_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){return r.json();})"
            ".then(function(s){"
                "updateSecureProgress(s);"
                "if(s.done){if(s.redirect)location.replace(s.redirect);return;}"
                "sdSecurePollTimer=setTimeout(pollSecureStatus,600);"
            "})"
            ".catch(function(){sdSecurePollTimer=setTimeout(pollSecureStatus,1000);});"
        "}"
        "function armSdForm(formId,buttonId,text,question,busyKind){"
            "var f=document.getElementById(formId);"
            "if(!f)return;"
            "f.addEventListener('submit',function(e){"
                "if(!confirm(question)){e.preventDefault();return;}"
                "var b=document.getElementById(buttonId);"
                "var p=document.getElementById('sdOperationProgress');"
                "if(b){b.disabled=true;b.textContent=text;}"
                "if(busyKind){showSdBusy(busyKind);}else if(p){p.style.display='block';}"
            "});"
        "}"
        "function armSecureErase(){"
            "var f=document.getElementById('sdSecureForm');"
            "var b=document.getElementById('sdSecureButton');"
            "if(!f)return;"
            "f.addEventListener('submit',function(e){"
                "e.preventDefault();"
                "if(!confirm('SECURE ERASE wirklich starten? Der Vorgang kann sehr lange dauern und überschreibt den logischen Datenbereich.'))return;"
                "if(b){b.disabled=true;b.textContent='SECURE ERASE LÄUFT...';}"
                "showSdBusy('secure');"
                "fetch('/sd_secure_erase_do',{method:'POST',cache:'no-store'})"
                ".then(function(r){return r.json();})"
                ".then(function(data){"
                    "if(!data.accepted){location.replace(data.redirect||'/sd_maintenance');return;}"
                    "pollSecureStatus();"
                "})"
                ".catch(function(){pollSecureStatus();});"
            "});"
        "}"
        "var sdAbort=document.getElementById('sdSecureAbortButton');"
        "if(sdAbort){sdAbort.addEventListener('click',function(){"
            "if(!confirm('Überschreiben jetzt abbrechen? Bereits überschriebene Daten bleiben überschrieben; anschließend wird die SD-Karte automatisch formatiert.'))return;"
            "sdAbort.disabled=true;sdAbort.textContent='ABBRUCH ANGEFORDERT …';"
            "fetch('/sd_secure_abort',{method:'POST',cache:'no-store'}).catch(function(){});"
        "});}"
        "armSdForm('sdWipeForm','sdWipeButton','WIPE LÄUFT...',"
            "'SD Wipe wirklich starten? Alle SD-Dateien werden gelöscht. Eine vorher vorhandene SD-config.txt wird wiederhergestellt; Internal-only bleibt Internal-only.','');"
        "armSdForm('sdRealFormatForm','sdRealFormatButton','FORMATIERUNG LÄUFT...',"
            "'SD wirklich neu formatieren? Alle SD-Daten gehen verloren. Eine vorher vorhandene SD-config.txt wird wiederhergestellt; Internal-only bleibt Internal-only.','format');"
        "armSecureErase();"
        "</script>";

    html +=
        pageFooter();

    return html;
}


static void handleSDMaintenance()
{
    SdMaintenanceViewState view = {};

    String html =
        sdMaintenancePage(view);

    webServer().send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


// Backward-compatible route name retained for existing bookmarks.
static void handleSDFormat()
{
    handleSDMaintenance();
}


static void handleSDFormatDo()
{
    SdMaintenanceResult result =
        performSdMaintenance(
            SD_MAINT_WIPE
        );

    redirectSdMaintenanceResult(
        SD_MAINT_WIPE,
        result
    );
}


static void handleSDRealFormatDo()
{
    SdMaintenanceResult result =
        performSdMaintenance(
            SD_MAINT_FORMAT
        );

    redirectSdMaintenanceResult(
        SD_MAINT_FORMAT,
        result
    );
}


static const char *secureEraseStageName()
{
    switch (sdSecureJobStage) {
        case SD_SECURE_JOB_OVERWRITE:
            return "overwrite";
        case SD_SECURE_JOB_FORMAT:
            return "format";
        case SD_SECURE_JOB_RESTORE:
            return "restore";
        case SD_SECURE_JOB_DONE:
            return "done";
        default:
            return "idle";
    }
}


static const char *secureEraseRedirectLocation()
{
    if (!sdSecureJobDone)
        return "";

    if (
        sdSecureFinalResult ==
        SD_MAINT_RESULT_OK
    ) {
        return
            sdSecureAborted
            ? "/rebooting?reason=sd_secure_abort"
            : "/rebooting?reason=sd_secure";
    }

    return
        sdMaintenanceResultLocation(
            SD_MAINT_SECURE_ERASE,
            sdSecureFinalResult
        );
}


static void handleSDSecureEraseDo()
{
    SdMaintenanceResult result =
        beginSecureEraseJob();

    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    if (
        result !=
        SD_MAINT_RESULT_OK
    ) {
        String json =
            "{\"accepted\":false,\"redirect\":\"";

        json +=
            sdMaintenanceResultLocation(
                SD_MAINT_SECURE_ERASE,
                result
            );

        json +=
            "\"}";

        webServer().send(
            409,
            "application/json; charset=utf-8",
            json
        );
        return;
    }

    webServer().send(
        202,
        "application/json; charset=utf-8",
        "{\"accepted\":true}"
    );
}


static void handleSDSecureStatus()
{
    uint32_t percentX10 = 0;

    if (sdSecureTargetBytes > 0) {
        uint64_t scaled =
            (
                sdSecureOverwrittenBytes *
                1000ULL
            ) /
            sdSecureTargetBytes;

        if (scaled > 1000ULL)
            scaled = 1000ULL;

        percentX10 =
            (uint32_t)scaled;
    }

    String json;
    json.reserve(320);

    json +=
        "{\"active\":";
    json +=
        sdSecureJobActive ? "true" : "false";
    json +=
        ",\"done\":";
    json +=
        sdSecureJobDone ? "true" : "false";
    json +=
        ",\"stage\":\"";
    json +=
        secureEraseStageName();
    json +=
        "\",\"abortRequested\":";
    json +=
        sdSecureAbortRequested ? "true" : "false";
    json +=
        ",\"aborted\":";
    json +=
        sdSecureAborted ? "true" : "false";
    json +=
        ",\"hadSdConfig\":";
    json +=
        sdSecureHadSdConfig ? "true" : "false";
    char u64Text[32];

    json +=
        ",\"overwrittenBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureOverwrittenBytes
    );
    json +=
        u64Text;

    json +=
        ",\"targetBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureTargetBytes
    );
    json +=
        u64Text;

    json +=
        ",\"totalBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureCardTotalBytes
    );
    json +=
        u64Text;
    json +=
        ",\"percentX10\":";
    json +=
        String(percentX10);
    json +=
        ",\"redirect\":\"";
    json +=
        secureEraseRedirectLocation();
    json +=
        "\"}";

    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    webServer().send(
        200,
        "application/json; charset=utf-8",
        json
    );
}


static void handleSDSecureAbort()
{
    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    if (
        !sdSecureJobActive ||
        sdSecureJobStage !=
            SD_SECURE_JOB_OVERWRITE
    ) {
        webServer().send(
            409,
            "application/json; charset=utf-8",
            "{\"accepted\":false}"
        );
        return;
    }

    sdSecureAbortRequested =
        true;

    webServer().send(
        202,
        "application/json; charset=utf-8",
        "{\"accepted\":true}"
    );
}


// -------------------------------------------------------------
// SD BENCHMARK / STORAGE DIAGNOSTICS
// -------------------------------------------------------------

struct SdBenchmarkLatencyStats {
    uint32_t minUs;
    uint32_t averageUs;
    uint32_t p95Us;
    uint32_t worstUs;
};

struct SdBenchmarkFilesystemGeometry {
    bool available;
    String filesystemType;
    uint32_t volumeStartSector;
    uint32_t bytesPerSector;
    uint32_t sectorsPerCluster;
    uint64_t clusterBytes;
    uint64_t volumeSectors;
    uint64_t clusterCount;
    String error;
};

struct SdBenchmarkCaseResult {
    size_t blockSize;
    size_t bytesWritten;
    size_t bytesRead;
    uint32_t writeOpenUs;
    uint32_t writeDataUs;
    uint32_t writeCallTotalUs;
    uint32_t writeFlushUs;
    uint32_t writeCloseUs;
    uint32_t writeTotalUs;
    uint32_t readOpenUs;
    uint32_t readDataUs;
    uint32_t readCallTotalUs;
    uint32_t readCloseUs;
    uint32_t readTotalUs;
    SdBenchmarkLatencyStats writeLatency;
    SdBenchmarkLatencyStats readLatency;
    bool writeComplete;
    bool readComplete;
    bool verifyOk;
    size_t verifyMismatchOffset;
    uint8_t verifyExpected;
    uint8_t verifyActual;
    String error;
};

static const char *sdBenchmarkCardTypeName(uint8_t cardType)
{
    if (cardType == CARD_MMC)
        return "MMC";
    if (cardType == CARD_SD)
        return "SDSC";
    if (cardType == CARD_SDHC)
        return "SDHC/SDXC";
    if (cardType == CARD_NONE)
        return "NONE";
    return "UNKNOWN";
}

static bool sdBenchmarkReadFilesystemGeometry(
    SdBenchmarkFilesystemGeometry &geometry
)
{
    geometry = {};

#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED
    uint32_t volumeStart = 0;
    uint16_t backupBootSector = 0;
    String locateError;

    if (!locateFatVolumeStart(
            volumeStart,
            backupBootSector,
            locateError
        )) {
        geometry.error = locateError;
        return false;
    }

    (void)backupBootSector;

    uint8_t sector[512] = {};

    if (!STORAGE.readRAW(
            sector,
            volumeStart
        )) {
        geometry.error =
            "cannot read filesystem boot sector";
        return false;
    }

    bool signature =
        sector[510] == 0x55 &&
        sector[511] == 0xAA;

    geometry.volumeStartSector =
        volumeStart;

    if (
        signature &&
        memcmp(
            &sector[3],
            "EXFAT   ",
            8
        ) == 0
    ) {
        uint8_t bytesPerSectorShift =
            sector[108];
        uint8_t sectorsPerClusterShift =
            sector[109];

        if (
            bytesPerSectorShift < 9 ||
            bytesPerSectorShift > 12 ||
            sectorsPerClusterShift > 25
        ) {
            geometry.error =
                "invalid exFAT geometry";
            return false;
        }

        geometry.filesystemType =
            "exFAT";
        geometry.bytesPerSector =
            1UL << bytesPerSectorShift;
        geometry.sectorsPerCluster =
            1UL << sectorsPerClusterShift;
        geometry.clusterBytes =
            (uint64_t)geometry.bytesPerSector *
            geometry.sectorsPerCluster;
        geometry.volumeSectors =
            sdMaintReadU64LE(
                &sector[72]
            );
        geometry.clusterCount =
            sdMaintReadU32LE(
                &sector[92]
            );
        geometry.available = true;
        return true;
    }

    uint32_t bytesPerSector =
        sdMaintReadU16LE(
            &sector[11]
        );
    uint32_t sectorsPerCluster =
        sector[13];
    uint32_t reservedSectors =
        sdMaintReadU16LE(
            &sector[14]
        );
    uint32_t fatCount =
        sector[16];
    uint32_t rootEntryCount =
        sdMaintReadU16LE(
            &sector[17]
        );
    uint32_t totalSectors16 =
        sdMaintReadU16LE(
            &sector[19]
        );
    uint32_t fatSectors16 =
        sdMaintReadU16LE(
            &sector[22]
        );
    uint32_t totalSectors32 =
        sdMaintReadU32LE(
            &sector[32]
        );
    uint32_t fatSectors32 =
        sdMaintReadU32LE(
            &sector[36]
        );

    if (
        !signature ||
        bytesPerSector == 0 ||
        sectorsPerCluster == 0 ||
        (sectorsPerCluster &
         (sectorsPerCluster - 1U)) != 0 ||
        reservedSectors == 0 ||
        fatCount == 0
    ) {
        geometry.error =
            "unrecognized FAT filesystem geometry";
        return false;
    }

    uint64_t totalSectors =
        totalSectors16 > 0
        ? totalSectors16
        : totalSectors32;
    uint64_t fatSectors =
        fatSectors16 > 0
        ? fatSectors16
        : fatSectors32;

    uint64_t rootDirSectors =
        ((uint64_t)rootEntryCount * 32ULL +
         (uint64_t)bytesPerSector - 1ULL) /
        (uint64_t)bytesPerSector;

    uint64_t metadataSectors =
        (uint64_t)reservedSectors +
        (uint64_t)fatCount *
            fatSectors +
        rootDirSectors;

    if (
        totalSectors == 0 ||
        fatSectors == 0 ||
        metadataSectors >= totalSectors
    ) {
        geometry.error =
            "invalid FAT sector counts";
        return false;
    }

    uint64_t dataSectors =
        totalSectors - metadataSectors;
    uint64_t clusterCount =
        dataSectors /
        sectorsPerCluster;

    if (clusterCount < 4085ULL) {
        geometry.filesystemType =
            "FAT12";
    } else if (clusterCount < 65525ULL) {
        geometry.filesystemType =
            "FAT16";
    } else {
        geometry.filesystemType =
            "FAT32";
    }

    geometry.bytesPerSector =
        bytesPerSector;
    geometry.sectorsPerCluster =
        sectorsPerCluster;
    geometry.clusterBytes =
        (uint64_t)bytesPerSector *
        sectorsPerCluster;
    geometry.volumeSectors =
        totalSectors;
    geometry.clusterCount =
        clusterCount;
    geometry.available = true;
    return true;
#else
    geometry.error =
        "raw filesystem geometry unavailable on this backend/core";
    return false;
#endif
}


static const char *sdBenchmarkBoardName()
{
#if defined(BOARD_FREENOVE)
    return "Freenove FNK0085 ESP32-S3 WROOM";
#elif defined(BOARD_XIAO)
    return "Seeed XIAO ESP32S3 Sense";
#else
    return "Unknown SensorForge board";
#endif
}

static String sdBenchmarkBackendDescription()
{
#if defined(STORAGE_SPI)
    return "SPI";
#elif defined(STORAGE_SDMMC)
#if defined(SD_MMC_D1) && defined(SD_MMC_D2) && defined(SD_MMC_D3)
    return "SD_MMC 4-bit";
#else
    return "SD_MMC 1-bit";
#endif
#else
    return "unknown";
#endif
}

static uint32_t sdBenchmarkConfiguredClockHz()
{
#if defined(STORAGE_SPI)
    return SD_SPI_NORMAL_FREQUENCY_HZ;
#elif defined(STORAGE_SDMMC)
    // Arduino/ESP-IDF SDMMC frequency constants are expressed in kHz.
    return (uint32_t)SDMMC_FREQ_DEFAULT * 1000UL;
#else
    return 0;
#endif
}

static uint32_t sdBenchmarkDiagnosticMaxClockHz()
{
#if defined(STORAGE_SPI)
    // Never exceed the board-approved SPI ceiling.
    return SD_SPI_MAX_FREQUENCY_HZ;
#elif defined(STORAGE_SDMMC)
    uint32_t maximumHz =
        sdBenchmarkConfiguredClockHz();

#ifdef SDMMC_FREQ_HIGHSPEED
    // SDMMC high-speed is an interface diagnostic ceiling only.
    maximumHz =
        (uint32_t)SDMMC_FREQ_HIGHSPEED * 1000UL;
#endif

#ifdef BOARD_MAX_SDMMC_FREQ
    // Respect a board-variant ceiling when Arduino declares one.
    uint32_t boardMaximumHz =
        (uint32_t)BOARD_MAX_SDMMC_FREQ * 1000UL;
    if (
        boardMaximumHz > 0 &&
        boardMaximumHz < maximumHz
    ) {
        maximumHz = boardMaximumHz;
    }
#endif

    return maximumHz;
#else
    return 0;
#endif
}

static bool sdBenchmarkSdMmcOneBitMode()
{
#if defined(STORAGE_SDMMC)
#if defined(SD_MMC_D1) && defined(SD_MMC_D2) && defined(SD_MMC_D3)
    return false;
#else
    return true;
#endif
#else
    return false;
#endif
}

static void sdBenchmarkAddClockCandidate(
    uint32_t *frequencies,
    size_t capacity,
    size_t &count,
    uint32_t frequencyHz,
    uint32_t maxFrequencyHz
)
{
    if (
        !frequencies ||
        count >= capacity ||
        frequencyHz == 0 ||
        frequencyHz > maxFrequencyHz
    ) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (frequencies[i] == frequencyHz)
            return;
    }

    frequencies[count++] = frequencyHz;
}

static size_t sdBenchmarkBuildClockPlan(
    uint32_t *frequencies,
    size_t capacity
)
{
    if (!frequencies || capacity == 0)
        return 0;

    size_t count = 0;
    uint32_t productionHz =
        sdBenchmarkConfiguredClockHz();
    uint32_t maximumHz =
        sdBenchmarkDiagnosticMaxClockHz();

    if (maximumHz == 0)
        maximumHz = productionHz;

#if defined(STORAGE_SPI)
    // Generic SPI diagnostic points. Lower clocks are conservative and the
    // upper end is capped by the board profile's approved MAX value.
    const uint32_t standardFrequencies[] = {
        4000000UL,
        10000000UL,
        20000000UL,
        25000000UL,
        40000000UL
    };
#elif defined(STORAGE_SDMMC)
    // Generic SDMMC diagnostic points. 20 MHz is the standard SensorForge
    // production/default policy today; 40 MHz is diagnostic high-speed only.
    const uint32_t standardFrequencies[] = {
        10000000UL,
        20000000UL,
        40000000UL
    };
#else
    const uint32_t standardFrequencies[] = {
        0UL
    };
#endif

    for (size_t i = 0; i < sizeof(standardFrequencies) / sizeof(standardFrequencies[0]); ++i) {
        sdBenchmarkAddClockCandidate(
            frequencies,
            capacity,
            count,
            standardFrequencies[i],
            maximumHz
        );
    }

    sdBenchmarkAddClockCandidate(
        frequencies,
        capacity,
        count,
        productionHz,
        maximumHz
    );

    sdBenchmarkAddClockCandidate(
        frequencies,
        capacity,
        count,
        maximumHz,
        maximumHz
    );

    std::sort(
        frequencies,
        frequencies + count
    );

    return count;
}

#if defined(STORAGE_SPI)
static void sdBenchmarkSpiIdleClocks()
{
    pinMode(
        SD_CS_PIN,
        OUTPUT
    );
    digitalWrite(
        SD_CS_PIN,
        HIGH
    );

    SPI.beginTransaction(
        SPISettings(
            400000UL,
            MSBFIRST,
            SPI_MODE0
        )
    );

    for (uint8_t i = 0; i < 16; ++i)
        SPI.transfer(0xFF);

    SPI.endTransaction();
    delay(2);
}
#endif

static bool sdBenchmarkRemountAtClock(
    uint32_t frequencyHz,
    String &error
)
{
    error = "";
    sdReady = false;

    STORAGE.end();
    delay(30);

#if defined(STORAGE_SPI)
    SPI.end();

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

    sdBenchmarkSpiIdleClocks();

    bool mounted =
        SD.begin(
            SD_CS_PIN,
            SPI,
            frequencyHz,
            "/sd",
            5,
            false
        );

#elif defined(STORAGE_SDMMC)
    bool oneBitMode =
        sdBenchmarkSdMmcOneBitMode();

#if defined(SD_MMC_D1) && defined(SD_MMC_D2) && defined(SD_MMC_D3)
    SD_MMC.setPins(
        SD_MMC_CLK,
        SD_MMC_CMD,
        SD_MMC_D0,
        SD_MMC_D1,
        SD_MMC_D2,
        SD_MMC_D3
    );
#else
    SD_MMC.setPins(
        SD_MMC_CLK,
        SD_MMC_CMD,
        SD_MMC_D0
    );
#endif

    uint32_t frequencyKHz =
        (frequencyHz + 999UL) / 1000UL;

    bool mounted =
        SD_MMC.begin(
            "/sdcard",
            oneBitMode,
            false,
            frequencyKHz,
            5
        );

#else
    bool mounted = false;
    (void)frequencyHz;
#endif

    if (!mounted) {
        error =
            "mount failed at " +
            String((double)frequencyHz / 1000000.0, 3) +
            " MHz";
        return false;
    }

    if (STORAGE.cardType() == CARD_NONE) {
        STORAGE.end();
        error =
            "no card detected after mount at " +
            String((double)frequencyHz / 1000000.0, 3) +
            " MHz";
        return false;
    }

    sdReady = true;
    return true;
}

static String sdBenchmarkPinDescription()
{
#if defined(STORAGE_SPI)
    return
        "CS=" + String((int)SD_CS_PIN) +
        " SCK=" + String((int)SD_SCK_PIN) +
        " MISO=" + String((int)SD_MISO_PIN) +
        " MOSI=" + String((int)SD_MOSI_PIN);
#elif defined(STORAGE_SDMMC)
    String pins =
        "CLK=" + String((int)SD_MMC_CLK) +
        " CMD=" + String((int)SD_MMC_CMD) +
        " D0=" + String((int)SD_MMC_D0);
#if defined(SD_MMC_D1) && defined(SD_MMC_D2) && defined(SD_MMC_D3)
    pins +=
        " D1=" + String((int)SD_MMC_D1) +
        " D2=" + String((int)SD_MMC_D2) +
        " D3=" + String((int)SD_MMC_D3);
#endif
    return pins;
#else
    return "n/a";
#endif
}

static void sdBenchmarkFillPattern(
    uint8_t *buffer,
    size_t length,
    uint32_t seed
)
{
    for (size_t i = 0; i < length; ++i) {
        uint32_t x =
            (uint32_t)i +
            seed * 2654435761UL;
        x ^= x >> 13;
        x *= 2246822519UL;
        x ^= x >> 16;
        buffer[i] = (uint8_t)(x & 0xFFU);
    }
}

static void sdBenchmarkLatencyStats(
    uint32_t *samples,
    size_t count,
    SdBenchmarkLatencyStats &stats
)
{
    stats = {};

    if (!samples || count == 0)
        return;

    std::sort(
        samples,
        samples + count
    );

    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i)
        total += samples[i];

    size_t p95Index =
        ((count * 95U + 99U) / 100U);

    if (p95Index > 0)
        p95Index--;

    if (p95Index >= count)
        p95Index = count - 1;

    stats.minUs = samples[0];
    stats.averageUs = (uint32_t)(total / count);
    stats.p95Us = samples[p95Index];
    stats.worstUs = samples[count - 1];
}

static float sdBenchmarkRateMBps(
    size_t bytes,
    uint32_t elapsedUs
)
{
    if (bytes == 0 || elapsedUs == 0)
        return 0.0f;

    return
        ((float)bytes / (1024.0f * 1024.0f)) *
        (1000000.0f / (float)elapsedUs);
}

static void sdBenchmarkAppendLatency(
    String &report,
    const char *label,
    const SdBenchmarkLatencyStats &stats
)
{
    report +=
        String(label) +
        " latency ms: min=" +
        String((double)stats.minUs / 1000.0, 3) +
        " avg=" +
        String((double)stats.averageUs / 1000.0, 3) +
        " p95=" +
        String((double)stats.p95Us / 1000.0, 3) +
        " worst=" +
        String((double)stats.worstUs / 1000.0, 3) +
        "\n";
}

static bool sdBenchmarkRunCase(
    const char *path,
    size_t blockSize,
    size_t totalSize,
    uint8_t *buffer,
    uint32_t *writeSamples,
    uint32_t *readSamples,
    SdBenchmarkCaseResult &result,
    bool reuseAllocatedFile = false,
    uint32_t patternSeed = 0
)
{
    result = {};
    result.blockSize = blockSize;
    result.verifyOk = true;

    size_t sampleCapacity =
        (totalSize + blockSize - 1U) /
        blockSize;

    uint32_t effectivePatternSeed =
        patternSeed != 0
        ? patternSeed
        : (uint32_t)blockSize;

    sdBenchmarkFillPattern(
        buffer,
        blockSize,
        effectivePatternSeed
    );

    if (!reuseAllocatedFile)
        STORAGE.remove(path);

    uint32_t writeTotalStart = micros();
    uint32_t openStart = micros();
    errno = 0;
    File writeFile =
        reuseAllocatedFile
        ? STORAGE.open(path, "r+")
        : STORAGE.open(path, FILE_WRITE);
    int openErrno = errno;
    result.writeOpenUs =
        (uint32_t)(micros() - openStart);

    if (!writeFile) {
        if (openErrno == EIO)
            storageMarkIoFault();

        result.error =
            String(
                reuseAllocatedFile
                ? "cannot open preallocated benchmark file for overwrite"
                : "cannot create benchmark file"
            ) +
            " | errno=" +
            String(openErrno) +
            ":" +
            String(strerror(openErrno));
        return false;
    }

    if (reuseAllocatedFile) {
        if (writeFile.size() != totalSize) {
            writeFile.close();
            result.error =
                "preallocated benchmark file size mismatch";
            return false;
        }

        if (!writeFile.seek(0)) {
            writeFile.close();
            result.error =
                "cannot seek preallocated benchmark file to start";
            return false;
        }
    }

    size_t writeSampleCount = 0;
    uint32_t dataStart = micros();

    while (result.bytesWritten < totalSize) {
        size_t remaining =
            totalSize - result.bytesWritten;
        size_t chunk =
            remaining < blockSize
            ? remaining
            : blockSize;

        uint32_t callStart = micros();
        errno = 0;
        size_t written =
            writeFile.write(
                buffer,
                chunk
            );
        int writeErrno = errno;
        uint32_t callUs =
            (uint32_t)(micros() - callStart);

        result.writeCallTotalUs +=
            callUs;

        if (writeSampleCount < sampleCapacity)
            writeSamples[writeSampleCount++] = callUs;

        result.bytesWritten += written;

        if (written != chunk) {
            if (writeErrno == EIO)
                storageMarkIoFault();

            result.error =
                "short benchmark write | offset=" +
                String((unsigned long)(result.bytesWritten - written)) +
                " | written=" +
                String((unsigned long)written) +
                "/" +
                String((unsigned long)chunk) +
                " | errno=" +
                String(writeErrno) +
                ":" +
                String(strerror(writeErrno));
            break;
        }

        serviceLongOperation();
    }

    result.writeDataUs =
        (uint32_t)(micros() - dataStart);

    if (!storageIoFaultActive()) {
        uint32_t flushStart = micros();
        writeFile.flush();
        result.writeFlushUs =
            (uint32_t)(micros() - flushStart);
    }

    uint32_t closeStart = micros();
    writeFile.close();
    result.writeCloseUs =
        (uint32_t)(micros() - closeStart);

    result.writeTotalUs =
        (uint32_t)(micros() - writeTotalStart);

    sdBenchmarkLatencyStats(
        writeSamples,
        writeSampleCount,
        result.writeLatency
    );

    result.writeComplete =
        result.bytesWritten == totalSize;

    if (!result.writeComplete) {
        if (!result.error.length())
            result.error = "benchmark write incomplete";
        return false;
    }

    uint32_t readTotalStart = micros();
    openStart = micros();
    errno = 0;
    File readFile = STORAGE.open(
        path,
        FILE_READ
    );
    int readOpenErrno = errno;
    result.readOpenUs =
        (uint32_t)(micros() - openStart);

    if (!readFile) {
        if (readOpenErrno == EIO)
            storageMarkIoFault();

        result.error =
            "cannot reopen benchmark file for read-back | errno=" +
            String(readOpenErrno) +
            ":" +
            String(strerror(readOpenErrno));
        return false;
    }

    size_t readSampleCount = 0;
    uint32_t readDataStart = micros();

    while (result.bytesRead < totalSize) {
        size_t remaining =
            totalSize - result.bytesRead;
        size_t chunk =
            remaining < blockSize
            ? remaining
            : blockSize;

        uint32_t callStart = micros();
        errno = 0;
        size_t readCount =
            readFile.read(
                buffer,
                chunk
            );
        int readErrno = errno;
        uint32_t callUs =
            (uint32_t)(micros() - callStart);

        result.readCallTotalUs +=
            callUs;

        if (readSampleCount < sampleCapacity)
            readSamples[readSampleCount++] = callUs;

        if (readCount == 0) {
            if (readErrno == EIO)
                storageMarkIoFault();

            result.error =
                "benchmark read incomplete | offset=" +
                String((unsigned long)result.bytesRead) +
                " | errno=" +
                String(readErrno) +
                ":" +
                String(strerror(readErrno));
            break;
        }

        size_t got =
            readCount;

        for (size_t i = 0; i < got; ++i) {
            uint32_t x =
                (uint32_t)i +
                effectivePatternSeed * 2654435761UL;
            x ^= x >> 13;
            x *= 2246822519UL;
            x ^= x >> 16;
            uint8_t expected =
                (uint8_t)(x & 0xFFU);

            if (buffer[i] != expected) {
                result.verifyOk = false;
                result.verifyMismatchOffset =
                    result.bytesRead + i;
                result.verifyExpected = expected;
                result.verifyActual = buffer[i];
                break;
            }
        }

        result.bytesRead += got;

        if (!result.verifyOk) {
            result.error = "benchmark read-back verification mismatch";
            break;
        }

        if (got != chunk) {
            if (readErrno == EIO)
                storageMarkIoFault();

            result.error =
                "short benchmark read | offset=" +
                String((unsigned long)(result.bytesRead - got)) +
                " | read=" +
                String((unsigned long)got) +
                "/" +
                String((unsigned long)chunk) +
                " | errno=" +
                String(readErrno) +
                ":" +
                String(strerror(readErrno));
            break;
        }

        serviceLongOperation();
    }

    result.readDataUs =
        (uint32_t)(micros() - readDataStart);

    uint32_t readCloseStart = micros();
    readFile.close();
    result.readCloseUs =
        (uint32_t)(micros() - readCloseStart);
    result.readTotalUs =
        (uint32_t)(micros() - readTotalStart);

    sdBenchmarkLatencyStats(
        readSamples,
        readSampleCount,
        result.readLatency
    );

    result.readComplete =
        result.bytesRead == totalSize;

    if (!result.readComplete && !result.error.length())
        result.error = "benchmark read incomplete";

    return
        result.writeComplete &&
        result.readComplete &&
        result.verifyOk;
}

static String sdBenchmarkUint64Text(uint64_t value)
{
    char buffer[32];
    snprintf(
        buffer,
        sizeof(buffer),
        "%llu",
        (unsigned long long)value
    );
    return String(buffer);
}

struct SdEnduranceCaseResult {
    uint64_t bytesWritten;
    uint64_t bytesRead;
    uint32_t writeElapsedMs;
    uint32_t readElapsedMs;
    uint32_t flushMs;
    uint32_t writeWorstUs;
    uint32_t readWorstUs;
    uint32_t slowWriteCount;
    bool writeComplete;
    bool readComplete;
    bool verifyOk;
    uint64_t failureOffset;
    uint64_t verifyMismatchOffset;
    uint8_t verifyExpected;
    uint8_t verifyActual;
    int errorNumber;
    String error;
};

static uint8_t sdEndurancePatternByte(
    uint64_t absoluteOffset,
    uint32_t seed
)
{
    // Cheap deterministic position-dependent pattern. The endurance test is
    // storage-bound; avoid turning 256 MiB of fill/verify work into a CPU test.
    uint32_t x =
        (uint32_t)absoluteOffset ^
        (uint32_t)(absoluteOffset >> 32) ^
        seed;

    x ^= x >> 11;
    x ^= x >> 19;

    return (uint8_t)(x & 0xFFU);
}

static void sdEnduranceFillPattern(
    uint8_t *buffer,
    size_t length,
    uint64_t absoluteOffset,
    uint32_t seed
)
{
    for (size_t i = 0; i < length; ++i) {
        buffer[i] =
            sdEndurancePatternByte(
                absoluteOffset + (uint64_t)i,
                seed
            );
    }
}

static bool sdBenchmarkRunEnduranceCase(
    const char *path,
    const char *openMode,
    const char *modeLabel,
    size_t totalSize,
    size_t blockSize,
    uint8_t *buffer,
    uint32_t patternSeed,
    SdEnduranceCaseResult &result
)
{
    result = {};
    result.verifyOk = true;

    STORAGE.remove(path);

    errno = 0;
    File writeFile =
        STORAGE.open(path, openMode);

    if (!writeFile) {
        result.errorNumber = errno;
        if (result.errorNumber == EIO)
            storageMarkIoFault();
        result.error =
            "cannot open endurance file | mode=" +
            String(modeLabel) +
            " | errno=" +
            String(result.errorNumber) +
            ":" +
            String(strerror(result.errorNumber));
        return false;
    }

    uint32_t writeStartMs = millis();

    while (result.bytesWritten < totalSize) {
        size_t remaining =
            totalSize - (size_t)result.bytesWritten;
        size_t chunk =
            remaining < blockSize
            ? remaining
            : blockSize;

        sdEnduranceFillPattern(
            buffer,
            chunk,
            result.bytesWritten,
            patternSeed
        );

        errno = 0;
        uint32_t callStartUs = micros();
        size_t written =
            writeFile.write(buffer, chunk);
        uint32_t callUs =
            (uint32_t)(micros() - callStartUs);
        int writeErrno = errno;

        if (callUs > result.writeWorstUs)
            result.writeWorstUs = callUs;
        if (callUs >= 20000UL)
            result.slowWriteCount++;

        uint64_t callOffset =
            result.bytesWritten;
        result.bytesWritten +=
            (uint64_t)written;

        if (written != chunk) {
            result.failureOffset = callOffset;
            result.errorNumber = writeErrno;

            if (writeErrno == EIO)
                storageMarkIoFault();

            result.error =
                "endurance write failed | mode=" +
                String(modeLabel) +
                " | offset=" +
                sdBenchmarkUint64Text(callOffset) +
                " | written=" +
                String((unsigned long)written) +
                "/" +
                String((unsigned long)chunk) +
                " | errno=" +
                String(writeErrno) +
                ":" +
                String(strerror(writeErrno));
            break;
        }

        serviceLongOperation();
    }

    result.writeElapsedMs =
        millis() - writeStartMs;

    if (!storageIoFaultActive()) {
        uint32_t flushStartMs = millis();
        writeFile.flush();
        result.flushMs = millis() - flushStartMs;
    }

    writeFile.close();

    result.writeComplete =
        result.bytesWritten == totalSize;

    if (!result.writeComplete)
        return false;

    errno = 0;
    File readFile =
        STORAGE.open(path, FILE_READ);

    if (!readFile) {
        result.errorNumber = errno;
        if (result.errorNumber == EIO)
            storageMarkIoFault();
        result.error =
            "cannot reopen endurance file for verify | mode=" +
            String(modeLabel) +
            " | errno=" +
            String(result.errorNumber) +
            ":" +
            String(strerror(result.errorNumber));
        return false;
    }

    uint32_t readStartMs = millis();

    while (result.bytesRead < totalSize) {
        size_t remaining =
            totalSize - (size_t)result.bytesRead;
        size_t chunk =
            remaining < blockSize
            ? remaining
            : blockSize;

        errno = 0;
        uint32_t callStartUs = micros();
        size_t got =
            readFile.read(buffer, chunk);
        uint32_t callUs =
            (uint32_t)(micros() - callStartUs);
        int readErrno = errno;

        if (callUs > result.readWorstUs)
            result.readWorstUs = callUs;

        if (got == 0) {
            result.failureOffset =
                result.bytesRead;
            result.errorNumber = readErrno;

            if (readErrno == EIO)
                storageMarkIoFault();

            result.error =
                "endurance read failed | mode=" +
                String(modeLabel) +
                " | offset=" +
                sdBenchmarkUint64Text(result.bytesRead) +
                " | errno=" +
                String(readErrno) +
                ":" +
                String(strerror(readErrno));
            break;
        }

        for (size_t i = 0; i < got; ++i) {
            uint8_t expected =
                sdEndurancePatternByte(
                    result.bytesRead +
                        (uint64_t)i,
                    patternSeed
                );

            if (buffer[i] != expected) {
                result.verifyOk = false;
                result.verifyMismatchOffset =
                    result.bytesRead +
                    (uint64_t)i;
                result.verifyExpected = expected;
                result.verifyActual = buffer[i];
                result.error =
                    "endurance read-back verification mismatch | mode=" +
                    String(modeLabel);
                break;
            }
        }

        result.bytesRead +=
            (uint64_t)got;

        if (!result.verifyOk)
            break;

        if (got != chunk) {
            if (readErrno == EIO)
                storageMarkIoFault();

            result.errorNumber = readErrno;
            result.error =
                "short endurance read | mode=" +
                String(modeLabel) +
                " | offset=" +
                sdBenchmarkUint64Text(result.bytesRead - got) +
                " | read=" +
                String((unsigned long)got) +
                "/" +
                String((unsigned long)chunk) +
                " | errno=" +
                String(readErrno) +
                ":" +
                String(strerror(readErrno));
            break;
        }

        serviceLongOperation();
    }

    result.readElapsedMs =
        millis() - readStartMs;

    readFile.close();

    result.readComplete =
        result.bytesRead == totalSize;

    if (!result.readComplete && !result.error.length())
        result.error = "endurance read incomplete";

    return
        result.writeComplete &&
        result.readComplete &&
        result.verifyOk;
}

static void sdBenchmarkAppendEnduranceReport(
    String &report,
    const SdEnduranceCaseResult &result,
    const char *modeLabel,
    size_t totalSize,
    size_t blockSize
)
{
    report +=
        "\n--- 64 MiB endurance | " +
        String(modeLabel) +
        " ---\n";
    report +=
        "write size/block: " +
        String((unsigned long)(totalSize / (1024U * 1024U))) +
        " MiB / " +
        String((unsigned long)(blockSize / 1024U)) +
        " KiB\n";
    report +=
        "write: " +
        sdBenchmarkUint64Text(result.bytesWritten) +
        " bytes | " +
        String(result.writeElapsedMs) +
        " ms | worst=" +
        String((double)result.writeWorstUs / 1000.0, 3) +
        " ms | >=20ms=" +
        String((unsigned long)result.slowWriteCount) +
        " | flush=" +
        String(result.flushMs) +
        " ms\n";
    report +=
        "read: " +
        sdBenchmarkUint64Text(result.bytesRead) +
        " bytes | " +
        String(result.readElapsedMs) +
        " ms | worst=" +
        String((double)result.readWorstUs / 1000.0, 3) +
        " ms\n";
    report +=
        "verify: " +
        String(result.verifyOk && result.readComplete ? "OK" : "FAIL") +
        "\n";

    if (!result.verifyOk) {
        report +=
            "verify mismatch offset=" +
            sdBenchmarkUint64Text(result.verifyMismatchOffset) +
            " expected=" +
            String((unsigned)result.verifyExpected) +
            " actual=" +
            String((unsigned)result.verifyActual) +
            "\n";
    }

    if (result.error.length()) {
        report +=
            "error: " +
            result.error +
            "\n";
    }
}


struct SdSfenc1EnduranceResult {
    uint64_t logicalBytesWritten;
    uint64_t logicalBytesRead;
    uint64_t physicalBytes;
    uint32_t writeElapsedMs;
    uint32_t closeElapsedMs;
    uint32_t readElapsedMs;
    bool openOk;
    bool closeOk;
    bool readOpenOk;
    bool verifyOk;
    uint64_t failureOffset;
    uint64_t verifyMismatchOffset;
    uint8_t verifyExpected;
    uint8_t verifyActual;
    bool ioFault;
    RecordingWriteBufferStats writeBufferStats;
    String error;
};

static bool sdBenchmarkRunSfenc1StackEndurance(
    const char *path,
    size_t totalSize,
    size_t logicalBlockSize,
    uint8_t *buffer,
    uint32_t patternSeed,
    SdSfenc1EnduranceResult &result
)
{
    result = {};
    result.verifyOk = true;

    STORAGE.remove(path);

    RecordingWriteBufferedFile writer;

    uint32_t writeStartMs = millis();

    if (!writer.openWrite(path, true)) {
        result.error =
            "SFENC1 stack open failed: " +
            String(writer.lastError());
        result.ioFault = storageIoFaultActive();
        return false;
    }

    result.openOk = true;
    result.writeBufferStats =
        writer.stats();

    if (!writer.writeBehindEnabled()) {
        result.error =
            "SFENC1 stack write-behind unavailable | init=" +
            String(
                recordingWriteBufferInitStatusName(
                    result.writeBufferStats.initStatus
                )
            );
        (void)writer.closeChecked();
        return false;
    }

    while (result.logicalBytesWritten < totalSize) {
        size_t remaining =
            totalSize - (size_t)result.logicalBytesWritten;
        size_t chunk =
            remaining < logicalBlockSize
            ? remaining
            : logicalBlockSize;

        sdEnduranceFillPattern(
            buffer,
            chunk,
            result.logicalBytesWritten,
            patternSeed
        );

        size_t written =
            writer.write(buffer, chunk);

        uint64_t callOffset =
            result.logicalBytesWritten;

        result.logicalBytesWritten +=
            (uint64_t)written;

        if (written != chunk || writer.failed()) {
            result.failureOffset = callOffset;
            result.error =
                "SFENC1 stack write failed | logical_offset=" +
                sdBenchmarkUint64Text(callOffset) +
                " | written=" +
                String((unsigned long)written) +
                "/" +
                String((unsigned long)chunk) +
                " | detail=" +
                String(writer.lastError());
            break;
        }

        serviceLongOperation();
    }

    result.writeElapsedMs =
        millis() - writeStartMs;

    // Snapshot the live queue statistics before close() releases the PSRAM
    // resources. closeChecked() then drains/finalizes the exact production path.
    result.writeBufferStats =
        writer.stats();

    uint32_t closeStartMs = millis();
    result.closeOk = writer.closeChecked();
    result.closeElapsedMs =
        millis() - closeStartMs;

    RecordingWriteBufferStats finalStats =
        writer.stats();

    // releaseWriteBehind() intentionally zeros only current queue/capacity;
    // cumulative counters survive and are more complete after final drain.
    result.writeBufferStats.bytesCommitted =
        finalStats.bytesCommitted;
    result.writeBufferStats.drainWriteCalls =
        finalStats.drainWriteCalls;
    result.writeBufferStats.drainWriteTotalUs =
        finalStats.drainWriteTotalUs;
    result.writeBufferStats.drainWriteMaxUs =
        finalStats.drainWriteMaxUs;
    result.writeBufferStats.drainWriteMaxBytes =
        finalStats.drainWriteMaxBytes;
    result.writeBufferStats.slowDrainWriteCalls =
        finalStats.slowDrainWriteCalls;
    result.writeBufferStats.failed =
        finalStats.failed;

    result.ioFault =
        storageIoFaultActive();

    if (!result.closeOk && !result.error.length()) {
        result.error =
            "SFENC1 stack close/finalize failed: " +
            String(writer.lastError());
    }

    if (
        result.logicalBytesWritten != totalSize ||
        !result.closeOk ||
        result.ioFault
    ) {
        return false;
    }

    File physicalFile =
        STORAGE.open(path, FILE_READ);

    if (physicalFile) {
        result.physicalBytes =
            (uint64_t)physicalFile.size();
        physicalFile.close();
    }

    RecordingStorageFile reader;

    if (!reader.openRead(path)) {
        result.error =
            "SFENC1 logical verify open failed: " +
            String(reader.lastError());
        result.ioFault = storageIoFaultActive();
        return false;
    }

    result.readOpenOk = true;

    uint32_t readStartMs = millis();

    while (result.logicalBytesRead < totalSize) {
        size_t remaining =
            totalSize - (size_t)result.logicalBytesRead;
        size_t chunk =
            remaining < logicalBlockSize
            ? remaining
            : logicalBlockSize;

        size_t got =
            reader.read(buffer, chunk);

        if (got == 0) {
            result.failureOffset =
                result.logicalBytesRead;
            result.error =
                "SFENC1 logical verify read failed | logical_offset=" +
                sdBenchmarkUint64Text(result.logicalBytesRead) +
                " | detail=" +
                String(reader.lastError());
            break;
        }

        for (size_t i = 0; i < got; ++i) {
            uint8_t expected =
                sdEndurancePatternByte(
                    result.logicalBytesRead +
                        (uint64_t)i,
                    patternSeed
                );

            if (buffer[i] != expected) {
                result.verifyOk = false;
                result.verifyMismatchOffset =
                    result.logicalBytesRead +
                    (uint64_t)i;
                result.verifyExpected = expected;
                result.verifyActual = buffer[i];
                result.error =
                    "SFENC1 logical read-back verification mismatch";
                break;
            }
        }

        result.logicalBytesRead +=
            (uint64_t)got;

        if (!result.verifyOk)
            break;

        if (got != chunk) {
            result.failureOffset =
                result.logicalBytesRead - got;
            result.error =
                "SFENC1 logical verify short read | logical_offset=" +
                sdBenchmarkUint64Text(result.failureOffset) +
                " | read=" +
                String((unsigned long)got) +
                "/" +
                String((unsigned long)chunk);
            break;
        }

        serviceLongOperation();
    }

    result.readElapsedMs =
        millis() - readStartMs;

    bool readerOk =
        !reader.failed();

    if (!readerOk && !result.error.length()) {
        result.error =
            "SFENC1 logical verify failed: " +
            String(reader.lastError());
    }

    reader.close();

    result.ioFault =
        storageIoFaultActive();

    return
        result.logicalBytesWritten == totalSize &&
        result.logicalBytesRead == totalSize &&
        result.closeOk &&
        readerOk &&
        result.verifyOk &&
        !result.ioFault;
}

static void sdBenchmarkAppendSfenc1StackReport(
    String &report,
    const SdSfenc1EnduranceResult &result,
    const char *clockLabel,
    size_t totalSize,
    size_t logicalBlockSize
)
{
    const RecordingWriteBufferStats &stats =
        result.writeBufferStats;

    report +=
        "\n--- SFENC1 production stack endurance | " +
        String(clockLabel) +
        " ---\n";
    report +=
        "logical size/block: " +
        String((unsigned long)(totalSize / (1024U * 1024U))) +
        " MiB / " +
        String((unsigned long)(logicalBlockSize / 1024U)) +
        " KiB producer writes\n";
    report +=
        "path: RecordingWriteBufferedFile -> PSRAM write-behind -> RecordingStorageFile(encrypted) -> SFENC1 -> SD\n";
    report +=
        "write: " +
        sdBenchmarkUint64Text(result.logicalBytesWritten) +
        " logical bytes | " +
        String(result.writeElapsedMs) +
        " ms | close/finalize=" +
        String(result.closeElapsedMs) +
        " ms\n";
    report +=
        "physical file: " +
        sdBenchmarkUint64Text(result.physicalBytes) +
        " bytes\n";
    report +=
        "write-behind: init=" +
        String(recordingWriteBufferInitStatusName(stats.initStatus)) +
        " | capacity=" +
        String((unsigned long)stats.capacity) +
        " | high_water=" +
        String((unsigned long)stats.highWater) +
        " | waits=" +
        String((unsigned long)stats.producerWaitCount) +
        "/" +
        String((double)stats.producerWaitUs / 1000.0, 1) +
        " ms | committed=" +
        sdBenchmarkUint64Text(stats.bytesCommitted) +
        " | drain_calls=" +
        String((unsigned long)stats.drainWriteCalls) +
        " | drain_total=" +
        String((double)stats.drainWriteTotalUs / 1000.0, 1) +
        " ms | drain_max=" +
        String((double)stats.drainWriteMaxUs / 1000.0, 1) +
        " ms@" +
        String((unsigned long)stats.drainWriteMaxBytes) +
        " B | >=20ms=" +
        String((unsigned long)stats.slowDrainWriteCalls) +
        "\n";
    report +=
        "logical read-back: " +
        sdBenchmarkUint64Text(result.logicalBytesRead) +
        " bytes | " +
        String(result.readElapsedMs) +
        " ms | verify=" +
        String(
            result.verifyOk &&
            result.logicalBytesRead == totalSize
            ? "OK"
            : "FAIL"
        ) +
        "\n";
    report +=
        "storage EIO latch: " +
        String(result.ioFault ? "ACTIVE" : "clear") +
        "\n";

    if (!result.verifyOk) {
        report +=
            "verify mismatch offset=" +
            sdBenchmarkUint64Text(result.verifyMismatchOffset) +
            " expected=" +
            String((unsigned)result.verifyExpected) +
            " actual=" +
            String((unsigned)result.verifyActual) +
            "\n";
    }

    if (result.error.length()) {
        report +=
            "error: " +
            result.error +
            "\n";
    }
}

static void sdBenchmarkAppendCaseReport(
    String &report,
    const SdBenchmarkCaseResult &result,
    size_t totalSize,
    const char *sectionLabel = nullptr
)
{
    if (sectionLabel && sectionLabel[0]) {
        report +=
            "\n--- " +
            String(sectionLabel) +
            " ---\n";
        report +=
            "block size: " +
            String((unsigned long)(result.blockSize / 1024U)) +
            " KiB\n";
    } else {
        report +=
            "\n--- block " +
            String((unsigned long)(result.blockSize / 1024U)) +
            " KiB ---\n";
    }

    report +=
        "test bytes: " +
        String((unsigned long)totalSize) +
        "\n";

    report +=
        "write: " +
        String((unsigned long)result.bytesWritten) +
        " bytes | io=" +
        String(sdBenchmarkRateMBps(result.bytesWritten, result.writeCallTotalUs), 3) +
        " MB/s | loop=" +
        String(sdBenchmarkRateMBps(result.bytesWritten, result.writeDataUs), 3) +
        " MB/s | total+flush=" +
        String(sdBenchmarkRateMBps(result.bytesWritten, result.writeTotalUs), 3) +
        " MB/s\n";

    report +=
        "write timing ms: open=" +
        String((double)result.writeOpenUs / 1000.0, 3) +
        " data=" +
        String((double)result.writeDataUs / 1000.0, 3) +
        " flush=" +
        String((double)result.writeFlushUs / 1000.0, 3) +
        " close=" +
        String((double)result.writeCloseUs / 1000.0, 3) +
        " total=" +
        String((double)result.writeTotalUs / 1000.0, 3) +
        "\n";

    sdBenchmarkAppendLatency(
        report,
        "write-call",
        result.writeLatency
    );

    report +=
        "read: " +
        String((unsigned long)result.bytesRead) +
        " bytes | io=" +
        String(sdBenchmarkRateMBps(result.bytesRead, result.readCallTotalUs), 3) +
        " MB/s | loop+verify=" +
        String(sdBenchmarkRateMBps(result.bytesRead, result.readDataUs), 3) +
        " MB/s | total=" +
        String(sdBenchmarkRateMBps(result.bytesRead, result.readTotalUs), 3) +
        " MB/s\n";

    report +=
        "read timing ms: open=" +
        String((double)result.readOpenUs / 1000.0, 3) +
        " data=" +
        String((double)result.readDataUs / 1000.0, 3) +
        " close=" +
        String((double)result.readCloseUs / 1000.0, 3) +
        " total=" +
        String((double)result.readTotalUs / 1000.0, 3) +
        "\n";

    sdBenchmarkAppendLatency(
        report,
        "read-call",
        result.readLatency
    );

    report +=
        "verify: " +
        String(result.verifyOk ? "OK" : "FAIL") +
        "\n";

    if (!result.verifyOk) {
        report +=
            "verify mismatch offset=" +
            String((unsigned long)result.verifyMismatchOffset) +
            " expected=" +
            String((unsigned)result.verifyExpected) +
            " actual=" +
            String((unsigned)result.verifyActual) +
            "\n";
    }

    if (result.error.length()) {
        report +=
            "error: " +
            result.error +
            "\n";
    }
}

static void handleSDBench()
{
    if (rejectWhileRecording("SD benchmark"))
        return;

    SdMaintenanceViewState view = {};
    view.benchmarkAttempted = true;

    if (!sdReady) {
        view.benchmarkError =
            "SD is not ready";
    } else if (g_storageLocked) {
        view.benchmarkError =
            "Storage is currently locked by another operation";
    }

    const char *benchmarkPath =
        "/.__sensorforge_sd_benchmark.bin";

    const size_t testSize =
        2U * 1024U * 1024U;

    const size_t longTestSize =
        8U * 1024U * 1024U;

    const size_t longTestBlockSize =
        32U * 1024U;

    const size_t enduranceTestSize =
        64U * 1024U * 1024U;

    const size_t enduranceBlockSize =
        4U * 1024U;

    // Keep 32 KiB first because the historical SensorForge benchmark used
    // that block size. This preserves the cleanest possible A/B comparison.
    const size_t blockSizes[] = {
        32U * 1024U,
        4U * 1024U,
        16U * 1024U,
        64U * 1024U
    };

    const size_t caseCount =
        sizeof(blockSizes) /
        sizeof(blockSizes[0]);

    const size_t maxBlockSize =
        64U * 1024U;

    const size_t maxSamples =
        testSize /
        (4U * 1024U);

    uint8_t *buffer = nullptr;
    uint32_t *writeSamples = nullptr;
    uint32_t *readSamples = nullptr;

    uint32_t benchmarkStartMs =
        millis();

    String report;
    report.reserve(12288);

    report +=
        "SensorForge extended SD diagnostics\n";
    report +=
        "board: " +
        String(sdBenchmarkBoardName()) +
        "\n";
    report +=
        "backend: " +
        sdBenchmarkBackendDescription() +
        "\n";

    uint32_t configuredClockHz =
        sdBenchmarkConfiguredClockHz();

    if (configuredClockHz > 0) {
        report +=
            "configured/requested mount clock: " +
            String((double)configuredClockHz / 1000000.0, 3) +
            " MHz\n";
    } else {
        report +=
            "configured mount clock: unavailable\n";
    }

    report +=
        "pins: " +
        sdBenchmarkPinDescription() +
        "\n";

    report +=
        "arduino-esp32: " +
        String(ESP_ARDUINO_VERSION_MAJOR) +
        "." +
        String(ESP_ARDUINO_VERSION_MINOR) +
        "." +
        String(ESP_ARDUINO_VERSION_PATCH) +
        "\n";

    uint8_t cardType =
        STORAGE.cardType();

    uint64_t cardSize =
        STORAGE.cardSize();
    uint64_t totalBytes =
        STORAGE.totalBytes();
    uint64_t usedBytes =
        STORAGE.usedBytes();
    uint64_t freeBytes =
        totalBytes > usedBytes
        ? totalBytes - usedBytes
        : 0;

    report +=
        "card type: " +
        String(sdBenchmarkCardTypeName(cardType)) +
        "\n";
    report +=
        "card size: " +
        String((double)cardSize / (1024.0 * 1024.0 * 1024.0), 3) +
        " GiB\n";
    report +=
        "filesystem total/used/free: " +
        String((double)totalBytes / (1024.0 * 1024.0 * 1024.0), 3) +
        " / " +
        String((double)usedBytes / (1024.0 * 1024.0 * 1024.0), 3) +
        " / " +
        String((double)freeBytes / (1024.0 * 1024.0 * 1024.0), 3) +
        " GiB\n";

    uint64_t reserveBytes =
        storageReserveBytes();

    report +=
        "configured free-space reserve: " +
        String((double)reserveBytes / (1024.0 * 1024.0), 1) +
        " MiB\n";

    SdBenchmarkFilesystemGeometry geometry;
    bool geometryOk =
        sdBenchmarkReadFilesystemGeometry(
            geometry
        );

    view.benchmarkGeometryAvailable =
        geometryOk;
    view.benchmarkClusterBytes =
        geometryOk
        ? geometry.clusterBytes
        : 0;

    if (geometryOk) {
        report +=
            "filesystem type: " +
            geometry.filesystemType +
            "\n";
        report +=
            "filesystem geometry: bytes/sector=" +
            String((unsigned long)geometry.bytesPerSector) +
            " sectors/cluster=" +
            String((unsigned long)geometry.sectorsPerCluster) +
            " cluster=" +
            String((double)geometry.clusterBytes / 1024.0, 1) +
            " KiB volume-start-LBA=" +
            String((unsigned long)geometry.volumeStartSector) +
            " clusters=" +
            String((double)geometry.clusterCount, 0) +
            "\n";
    } else {
        report +=
            "filesystem geometry: unavailable | " +
            geometry.error +
            "\n";
    }

    // SFENC1 adds a small physical header/record overhead above the 64 MiB
    // logical payload. Keep extra room above the configured reserve.
    size_t requiredBenchmarkBytes =
        enduranceTestSize +
        2U * 1024U * 1024U;

    if (
        !view.benchmarkError.length() &&
        freeBytes < reserveBytes + requiredBenchmarkBytes
    ) {
        view.benchmarkError =
            "Not enough free space above the configured reserve for the benchmark file";
    }
    report +=
        "test file: " +
        String(benchmarkPath) +
        "\n";
    report +=
        "per-case test size: " +
        String((unsigned long)(testSize / (1024U * 1024U))) +
        " MiB\n";
    report +=
        "long allocation comparison: " +
        String((unsigned long)(longTestSize / (1024U * 1024U))) +
        " MiB @ " +
        String((unsigned long)(longTestBlockSize / 1024U)) +
        " KiB blocks\n";
    report +=
        "endurance: two passes of " +
        String((unsigned long)(enduranceTestSize / (1024U * 1024U))) +
        " MiB @ " +
        String((unsigned long)(enduranceBlockSize / 1024U)) +
        " KiB writes | FILE_WRITE + w+ | full read-back verify\n";
    report +=
        "SFENC1 stack endurance: " +
        String((unsigned long)(enduranceTestSize / (1024U * 1024U))) +
        " MiB logical @ " +
        String((unsigned long)(enduranceBlockSize / 1024U)) +
        " KiB producer writes | real write-behind + encrypted storage + logical decrypt/verify\n";
    uint32_t diagnosticMaxClockHz =
        sdBenchmarkDiagnosticMaxClockHz();

    if (diagnosticMaxClockHz > 0) {
        report +=
            "diagnostic clock ceiling: " +
            String((double)diagnosticMaxClockHz / 1000000.0, 3) +
            " MHz\n";
    }

    report +=
        "clock sweep temporarily remounts storage and restores the production mount before returning\n";

    if (!view.benchmarkError.length()) {
        buffer =
            (uint8_t *)malloc(maxBlockSize);
        writeSamples =
            (uint32_t *)malloc(
                maxSamples *
                sizeof(uint32_t)
            );
        readSamples =
            (uint32_t *)malloc(
                maxSamples *
                sizeof(uint32_t)
            );

        if (
            !buffer ||
            !writeSamples ||
            !readSamples
        ) {
            view.benchmarkError =
                "Cannot allocate benchmark buffers";
        }
    }

    bool allCasesOk =
        !view.benchmarkError.length();

    if (allCasesOk) {
        STORAGE.remove(benchmarkPath);

        for (size_t i = 0; i < caseCount; ++i) {
            SdBenchmarkCaseResult caseResult;

            bool caseOk =
                sdBenchmarkRunCase(
                    benchmarkPath,
                    blockSizes[i],
                    testSize,
                    buffer,
                    writeSamples,
                    readSamples,
                    caseResult
                );

            sdBenchmarkAppendCaseReport(
                report,
                caseResult,
                testSize
            );

            if (blockSizes[i] == 32U * 1024U) {
                view.benchmarkReferenceWriteMBps =
                    sdBenchmarkRateMBps(
                        caseResult.bytesWritten,
                        caseResult.writeTotalUs
                    );
                view.benchmarkReferenceReadMBps =
                    sdBenchmarkRateMBps(
                        caseResult.bytesRead,
                        caseResult.readCallTotalUs
                    );
                view.benchmarkReferenceWriteP95Us =
                    caseResult.writeLatency.p95Us;
                view.benchmarkReferenceWriteWorstUs =
                    caseResult.writeLatency.worstUs;
                view.benchmarkReferenceFlushUs =
                    caseResult.writeFlushUs;
                view.benchmarkReferenceVerifyOk =
                    caseResult.verifyOk &&
                    caseResult.readComplete;
            }

            if (!caseOk) {
                allCasesOk = false;
                view.benchmarkError =
                    "Benchmark failed at " +
                    String((unsigned long)(blockSizes[i] / 1024U)) +
                    " KiB blocks: " +
                    caseResult.error;
                break;
            }

            STORAGE.remove(benchmarkPath);
            serviceLongOperation();
        }
    }

    // ---------------------------------------------------------
    // Long-file allocation comparison. The first pass grows a new 8 MiB
    // file normally. The second pass reopens the same file with r+ and
    // overwrites its already allocated clusters using a different pattern.
    // This isolates FAT allocation/file-growth cost from steady-state writes.
    // ---------------------------------------------------------

    if (allCasesOk) {
        report +=
            "\n=== long-file allocation comparison ===\n";
        report +=
            "comparison purpose: normal file growth vs overwrite of already allocated clusters\n";

        STORAGE.remove(benchmarkPath);

        SdBenchmarkCaseResult growingResult;
        bool growingOk =
            sdBenchmarkRunCase(
                benchmarkPath,
                longTestBlockSize,
                longTestSize,
                buffer,
                writeSamples,
                readSamples,
                growingResult,
                false,
                0x13579BDFUL
            );

        sdBenchmarkAppendCaseReport(
            report,
            growingResult,
            longTestSize,
            "long growing file"
        );

        if (!growingOk) {
            allCasesOk = false;
            view.benchmarkError =
                "Long growing-file benchmark failed: " +
                growingResult.error;
        } else {
            SdBenchmarkCaseResult preallocatedResult;
            bool preallocatedOk =
                sdBenchmarkRunCase(
                    benchmarkPath,
                    longTestBlockSize,
                    longTestSize,
                    buffer,
                    writeSamples,
                    readSamples,
                    preallocatedResult,
                    true,
                    0x2468ACE1UL
                );

            sdBenchmarkAppendCaseReport(
                report,
                preallocatedResult,
                longTestSize,
                "preallocated overwrite"
            );

            float growingWriteMBps =
                sdBenchmarkRateMBps(
                    growingResult.bytesWritten,
                    growingResult.writeTotalUs
                );
            float preallocatedWriteMBps =
                sdBenchmarkRateMBps(
                    preallocatedResult.bytesWritten,
                    preallocatedResult.writeTotalUs
                );

            report +=
                "allocation comparison: growing=" +
                String(growingWriteMBps, 3) +
                " MB/s preallocated=" +
                String(preallocatedWriteMBps, 3) +
                " MB/s";

            if (growingWriteMBps > 0.0f) {
                report +=
                    " ratio=" +
                    String(
                        preallocatedWriteMBps /
                        growingWriteMBps,
                        2
                    ) +
                    "x";
            }

            report += "\n";

            if (!preallocatedOk) {
                allCasesOk = false;
                view.benchmarkError =
                    "Preallocated overwrite benchmark failed: " +
                    preallocatedResult.error;
            }
        }

        STORAGE.remove(benchmarkPath);
        serviceLongOperation();
    }

    // ---------------------------------------------------------
    // 64 MiB endurance verification. This intentionally uses the same 4 KiB
    // physical write granularity that exposed the SFENC1 EIO. The second pass
    // opens with w+, matching encrypted recording's read/write file mode.
    // No retry is performed after a short/EIO write.
    // ---------------------------------------------------------

    if (allCasesOk) {
        report +=
            "\n=== 64 MiB / 4 KiB endurance verification ===\n";
        report +=
            "purpose: reproduce intermittent physical/VFS write faults beyond the former 8 MiB benchmark window\n";
        report +=
            "policy: fail on first short write/read; no automatic retry; full read-back verification\n";

        struct EnduranceMode {
            const char *openMode;
            const char *label;
            uint32_t seed;
        };

        const EnduranceMode modes[] = {
            {FILE_WRITE, "FILE_WRITE", 0x64F10001UL},
            {"w+", "w+ (SFENC1 file mode)", 0x64F10002UL}
        };

        for (const EnduranceMode &mode : modes) {
            STORAGE.remove(benchmarkPath);

            SdEnduranceCaseResult enduranceResult;
            bool enduranceOk =
                sdBenchmarkRunEnduranceCase(
                    benchmarkPath,
                    mode.openMode,
                    mode.label,
                    enduranceTestSize,
                    enduranceBlockSize,
                    buffer,
                    mode.seed,
                    enduranceResult
                );

            sdBenchmarkAppendEnduranceReport(
                report,
                enduranceResult,
                mode.label,
                enduranceTestSize,
                enduranceBlockSize
            );

            if (!enduranceOk) {
                allCasesOk = false;
                view.benchmarkError =
                    "64 MiB endurance failed in " +
                    String(mode.label) +
                    ": " +
                    enduranceResult.error;
                break;
            }

            STORAGE.remove(benchmarkPath);
            serviceLongOperation();
        }
    }


    // ---------------------------------------------------------
    // 64 MiB SFENC1 production-stack endurance. Unlike the raw w+ case above,
    // this drives the exact buffered encrypted recording storage architecture.
    // The logger is closed and the global storage gate is held so no unrelated
    // filesystem writer can perturb the result. A production-clock EIO may be
    // repeated once at 10 MHz after a clean remount, then production is restored.
    // ---------------------------------------------------------

    if (allCasesOk) {
        // Release the raw-benchmark working set first. The production encrypted
        // writer itself needs internal crypto/task resources; retaining the
        // 64 KiB raw buffer here would create artificial heap pressure.
        if (buffer) {
            free(buffer);
            buffer = nullptr;
        }
        if (writeSamples) {
            free(writeSamples);
            writeSamples = nullptr;
        }
        if (readSamples) {
            free(readSamples);
            readSamples = nullptr;
        }

        uint8_t *sfenc1Buffer =
            (uint8_t *)heap_caps_malloc(
                enduranceBlockSize,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
            );

        if (!sfenc1Buffer) {
            sfenc1Buffer =
                (uint8_t *)malloc(enduranceBlockSize);
        }

        if (!sfenc1Buffer) {
            allCasesOk = false;
            view.benchmarkError =
                "Cannot allocate 4 KiB SFENC1 endurance pattern buffer";
        }

        const char *sfenc1Path =
            "/.__sensorforge_sfenc1_endurance.bin";
        const uint32_t sfenc1Seed =
            0x65F1C001UL;

        report +=
            "\n=== 64 MiB SFENC1 production-stack endurance ===\n";
        report +=
            "purpose: isolate RecordingWriteBufferedFile + PSRAM drain task + encrypted RecordingStorageFile + SFENC1 from camera/audio/WebConfig load\n";
        report +=
            "policy: no media-write retry; full logical decrypt/read-back verify; 10 MHz fallback only when production clock is above 10 MHz and then EIO occurs\n";

        if (allCasesOk) {
            STORAGE.remove(sfenc1Path);

            logFlush();
            logClose();
            g_storageLocked = true;

        SdSfenc1EnduranceResult productionResult;
        bool productionSfenc1Ok =
            sdBenchmarkRunSfenc1StackEndurance(
                sfenc1Path,
                enduranceTestSize,
                enduranceBlockSize,
                sfenc1Buffer,
                sfenc1Seed,
                productionResult
            );

        String productionClockLabel =
            configuredClockHz > 0
            ? String((double)configuredClockHz / 1000000.0, 3) + " MHz production"
            : String("production mount");

        sdBenchmarkAppendSfenc1StackReport(
            report,
            productionResult,
            productionClockLabel.c_str(),
            enduranceTestSize,
            enduranceBlockSize
        );

        bool ranFallback10Mhz = false;
        bool fallback10MhzOk = false;
        bool productionMountRestored = true;

        if (
            !productionSfenc1Ok &&
            productionResult.ioFault &&
            configuredClockHz > 10000000UL
        ) {
            ranFallback10Mhz = true;
            report +=
                "production-clock SFENC1 EIO: remounting cleanly at 10.000 MHz for one controlled A/B repeat\n";

            String tenMhzMountError;
            bool tenMhzMounted =
                sdBenchmarkRemountAtClock(
                    10000000UL,
                    tenMhzMountError
                );

            if (tenMhzMounted) {
                storageClearIoFault();
                STORAGE.remove(sfenc1Path);

                SdSfenc1EnduranceResult tenMhzResult;
                fallback10MhzOk =
                    sdBenchmarkRunSfenc1StackEndurance(
                        sfenc1Path,
                        enduranceTestSize,
                        enduranceBlockSize,
                        sfenc1Buffer,
                        sfenc1Seed,
                        tenMhzResult
                    );

                sdBenchmarkAppendSfenc1StackReport(
                    report,
                    tenMhzResult,
                    "10.000 MHz fallback",
                    enduranceTestSize,
                    enduranceBlockSize
                );
            } else {
                report +=
                    "10 MHz remount: FAIL | " +
                    tenMhzMountError +
                    "\n";
            }

            // Regardless of the fallback result, return the device to its board
            // production policy before leaving this diagnostic phase.
            String restoreError;
            productionMountRestored =
                sdBenchmarkRemountAtClock(
                    configuredClockHz,
                    restoreError
                );

            if (productionMountRestored) {
                storageClearIoFault();
                report +=
                    "production mount restore after SFENC1 A/B: OK @ " +
                    String((double)configuredClockHz / 1000000.0, 3) +
                    " MHz\n";
            } else {
                report +=
                    "production mount restore after SFENC1 A/B: FAIL | " +
                    restoreError +
                    "\n";
            }
        }

        if (
            sdReady &&
            !storageIoFaultActive()
        ) {
            STORAGE.remove(sfenc1Path);
        }

        g_storageLocked = false;

        if (
            sdReady &&
            !storageIoFaultActive() &&
            productionMountRestored
        ) {
            logInit();
        }

            if (!productionSfenc1Ok) {
                allCasesOk = false;

                view.benchmarkError =
                    "SFENC1 production-stack endurance failed at production clock: " +
                    productionResult.error;

                if (ranFallback10Mhz) {
                    view.benchmarkError +=
                        fallback10MhzOk
                        ? " | 10 MHz fallback=PASS"
                        : " | 10 MHz fallback=FAIL";
                }

                if (!productionMountRestored) {
                    view.benchmarkError +=
                        " | production mount restore failed";
                }
            }
        }

        if (sfenc1Buffer)
            free(sfenc1Buffer);

        serviceLongOperation();
    }

    // ---------------------------------------------------------
    // Generic bus-clock diagnostic sweep. The normal multi-block
    // benchmark above always runs on the production mount. Only after
    // those tests pass do we temporarily close the persistent logger,
    // remount the storage at conservative diagnostic clock points, run
    // one compact verified 32 KiB case per point, and finally restore
    // the exact production clock before reopening the logger.
    // ---------------------------------------------------------

    bool productionRestoreOk = true;

    if (
        allCasesOk &&
        configuredClockHz > 0 &&
        (!buffer || !writeSamples || !readSamples)
    ) {
        buffer =
            (uint8_t *)malloc(maxBlockSize);
        writeSamples =
            (uint32_t *)malloc(
                maxSamples *
                sizeof(uint32_t)
            );
        readSamples =
            (uint32_t *)malloc(
                maxSamples *
                sizeof(uint32_t)
            );

        if (!buffer || !writeSamples || !readSamples) {
            allCasesOk = false;
            view.benchmarkError =
                "Cannot reallocate clock-sweep benchmark buffers after SFENC1 endurance";
        }
    }

    if (
        allCasesOk &&
        configuredClockHz > 0
    ) {
        const size_t clockSweepTestSize =
            512U * 1024U;
        const size_t clockSweepBlockSize =
            32U * 1024U;

        uint32_t clockFrequencies[8] = {};
        size_t clockCount =
            sdBenchmarkBuildClockPlan(
                clockFrequencies,
                sizeof(clockFrequencies) / sizeof(clockFrequencies[0])
            );

        report +=
            "\n=== verified bus-clock sweep ===\n";
        report +=
            "sweep block size: 32 KiB\n";
        report +=
            "sweep test size per clock: " +
            String((unsigned long)(clockSweepTestSize / 1024U)) +
            " KiB\n";
        report +=
            "production clock to restore: " +
            String((double)configuredClockHz / 1000000.0, 3) +
            " MHz\n";

        if (clockCount == 0) {
            report +=
                "clock sweep: no diagnostic frequencies available for this backend\n";
        } else {
            report +=
                "clock plan (requested maxima):";
            for (size_t i = 0; i < clockCount; ++i) {
                report +=
                    " " +
                    String((double)clockFrequencies[i] / 1000000.0, 3) +
                    "MHz";
            }
            report += "\n";

            STORAGE.remove(benchmarkPath);

            // SD.end()/SD_MMC.end() must never invalidate an open logger file.
            // Close it before the first diagnostic remount and reopen only after
            // the production mount is back.
            logFlush();
            logClose();
            g_storageLocked = true;

            size_t clockMountOkCount = 0;
            size_t clockVerifyOkCount = 0;

            for (size_t i = 0; i < clockCount; ++i) {
                uint32_t frequencyHz =
                    clockFrequencies[i];

                report +=
                    "\n--- requested bus clock " +
                    String((double)frequencyHz / 1000000.0, 3) +
                    " MHz ---\n";

                String mountError;
                bool mounted =
                    sdBenchmarkRemountAtClock(
                        frequencyHz,
                        mountError
                    );

                if (!mounted) {
                    report +=
                        "mount: FAIL | " +
                        mountError +
                        "\n";
                    serviceLongOperation();
                    continue;
                }

                clockMountOkCount++;

                report +=
                    "mount: OK | card=" +
                    String(sdBenchmarkCardTypeName(STORAGE.cardType())) +
                    "\n";

                SdBenchmarkCaseResult clockResult;
                bool clockCaseOk =
                    sdBenchmarkRunCase(
                        benchmarkPath,
                        clockSweepBlockSize,
                        clockSweepTestSize,
                        buffer,
                        writeSamples,
                        readSamples,
                        clockResult
                    );

                sdBenchmarkAppendCaseReport(
                    report,
                    clockResult,
                    clockSweepTestSize
                );

                if (clockCaseOk)
                    clockVerifyOkCount++;

                report +=
                    "clock result: " +
                    String(clockCaseOk ? "OK" : "FAIL") +
                    "\n";

                if (storageIoFaultActive()) {
                    allCasesOk = false;
                    if (!view.benchmarkError.length()) {
                        view.benchmarkError =
                            "Hard EIO during diagnostic clock sweep";
                    }
                    report +=
                        "clock sweep stopped: hard EIO latched\n";
                    break;
                }

                STORAGE.remove(benchmarkPath);
                serviceLongOperation();
            }

            report +=
                "\nclock sweep summary: mounted=" +
                String((unsigned long)clockMountOkCount) +
                "/" +
                String((unsigned long)clockCount) +
                " verified=" +
                String((unsigned long)clockVerifyOkCount) +
                "/" +
                String((unsigned long)clockCount) +
                "\n";
            report +=
                "diagnostic clock failures are informational unless the production mount cannot be restored\n";

            String restoreError;
            productionRestoreOk =
                sdBenchmarkRemountAtClock(
                    configuredClockHz,
                    restoreError
                );

            report +=
                "\nproduction mount restore: " +
                String(productionRestoreOk ? "OK" : "FAIL");

            if (productionRestoreOk) {
                report +=
                    " @ " +
                    String((double)configuredClockHz / 1000000.0, 3) +
                    " MHz\n";
            } else {
                report +=
                    " | " +
                    restoreError +
                    "\n";
                allCasesOk = false;
                view.benchmarkError =
                    "Production storage mount could not be restored after the diagnostic clock sweep: " +
                    restoreError;
            }

            g_storageLocked = false;

            if (
                productionRestoreOk &&
                !storageIoFaultActive()
            ) {
                logInit();
            }
        }
    }

    // A hard EIO invalidates assumptions about every open handle on the mount.
    // Recover only after the benchmark file has been closed. recoverSD() knows
    // to close the logger without flushing its RAM queue while this latch is set.
    if (storageIoFaultActive()) {
        report +=
            "\nHARD STORAGE I/O FAULT: EIO latched; performing controlled SD remount\n";

        sdReady = false;
        bool recovered = recoverSD();

        report +=
            "SD recovery after benchmark EIO: " +
            String(recovered ? "OK" : "FAIL") +
            "\n";

        if (!recovered) {
            if (view.benchmarkError.length())
                view.benchmarkError += " | SD recovery failed";
            else
                view.benchmarkError = "SD recovery failed after benchmark EIO";
        }
    }

    if (sdReady && !storageIoFaultActive())
        STORAGE.remove(benchmarkPath);

    bool benchmarkFileStillPresent =
        sdReady && STORAGE.exists(benchmarkPath);

    if (benchmarkFileStillPresent) {
        allCasesOk = false;
        if (!view.benchmarkError.length()) {
            view.benchmarkError =
                "Temporary benchmark file could not be removed";
        }
    }

    if (buffer)
        free(buffer);
    if (writeSamples)
        free(writeSamples);
    if (readSamples)
        free(readSamples);

    view.benchmarkElapsedMs =
        millis() - benchmarkStartMs;
    view.benchmarkSuccess =
        allCasesOk;
    view.benchmarkRating =
        sdBenchmarkEvaluateRating(view);

    uint64_t usedAfter =
        sdReady
        ? STORAGE.usedBytes()
        : 0;
    uint64_t freeAfter =
        (
            sdReady &&
            totalBytes > usedAfter
        )
        ? totalBytes - usedAfter
        : 0;

    report +=
        "\ncleanup: temporary benchmark file " +
        String(
            benchmarkFileStillPresent
            ? "STILL PRESENT"
            : "removed"
        ) +
        "\n";

    if (sdReady) {
        report +=
            "filesystem free after cleanup: " +
            String((double)freeAfter / (1024.0 * 1024.0 * 1024.0), 3) +
            " GiB\n";
    } else {
        report +=
            "filesystem free after cleanup: unavailable (production mount not restored)\n";
    }
    report +=
        "overall result: " +
        String(view.benchmarkSuccess ? "OK" : "FAIL") +
        "\n";
    report +=
        "elapsed: " +
        String(view.benchmarkElapsedMs) +
        " ms\n";

    if (view.benchmarkError.length()) {
        report +=
            "overall error: " +
            view.benchmarkError +
            "\n";
    }

    view.benchmarkReport = report;

    String html =
        sdMaintenancePage(view);

    webServer().send(
        200,
        "text/html; charset=utf-8",
        html
    );
}

} // namespace

void webSdMaintenanceRegisterRoutes(
    WebServer &server,
    const WebSdMaintenanceUiHooks &uiHooks
)
{
    g_webServer = &server;
    g_uiHooks = uiHooks;

    server.on("/sd_recovery_run", HTTP_POST, handleSDRecoveryRun);
    server.on("/sd_maintenance", HTTP_GET, handleSDMaintenance);
    server.on("/sdformat", HTTP_GET, handleSDFormat);
    server.on("/sdformat_do", HTTP_POST, handleSDFormatDo);
    server.on("/sd_format_do", HTTP_POST, handleSDRealFormatDo);
    server.on("/sd_secure_erase_do", HTTP_POST, handleSDSecureEraseDo);
    server.on("/sd_secure_status", HTTP_GET, handleSDSecureStatus);
    server.on("/sd_secure_abort", HTTP_POST, handleSDSecureAbort);
    server.on("/sd_benchmark_run", HTTP_POST, handleSDBench);
}

void webSdMaintenanceLoop()
{
    processSecureEraseJob();
}

bool webSdMaintenanceBusy()
{
    return sdSecureJobActive;
}
