#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <algorithm>

#include "board_config.h"
#include "config.h"
#include "avi_writer.h"
#include "recording_storage.h"
#include "recording_crypto.h"

// ============================================================================
// SensorForge XIAO ESP32S3 Sense - practical recording benchmark
//
// Purpose:
//   Compare the REAL camera -> production AVI writer -> SD path at 4 MHz and
//   20 MHz SPI. If recording_encryption=1 in /config.txt, the same production
//   SFENC1 storage layer is used as well.
//
// Safety:
//   This benchmark NEVER provisions/burns eFuses. If encryption is requested
//   but no existing SensorForge recording key is present, the benchmark aborts.
//
// Current measurement:
//   During each recording phase the Serial console prints a very large marker
//   telling you exactly when to read and note the current from your meter.
// ============================================================================

namespace {

// Camera pins: current SensorForge XIAO ESP32S3 Sense profile.
static const int CAM_PWDN  = -1;
static const int CAM_RESET = -1;
static const int CAM_XCLK  = 10;
static const int CAM_SIOD  = 40;
static const int CAM_SIOC  = 39;
static const int CAM_D7    = 48;
static const int CAM_D6    = 11;
static const int CAM_D5    = 12;
static const int CAM_D4    = 14;
static const int CAM_D3    = 16;
static const int CAM_D2    = 18;
static const int CAM_D1    = 17;
static const int CAM_D0    = 15;
static const int CAM_VSYNC = 38;
static const int CAM_HREF  = 47;
static const int CAM_PCLK  = 13;

// Recording timing. One run per frequency is enough for the first A/B test.
// Re-run the sketch if you want another independent pair of measurements.
static const uint32_t PRE_MEASUREMENT_SECONDS = 5;
static const uint32_t CURRENT_WINDOW_SECONDS  = 20;
static const uint32_t POST_MEASUREMENT_SECONDS = 5;
static const uint32_t PHASE_SECONDS =
    PRE_MEASUREMENT_SECONDS +
    CURRENT_WINDOW_SECONDS +
    POST_MEASUREMENT_SECONDS;

static const size_t MAX_LATENCY_SAMPLES = 2048;

struct BenchConfig {
    String camera = "OV3660";
    String resolution = "1024x768";
    int fps = 5;
    int quality = 12;
    int xclkMhz = 20;
    int autoExposure = 1;
    int aeLevel = -2;
    int rotation = 0;
    int recordingEncryption = 0;
};

struct Result {
    uint32_t mhz = 0;
    bool ok = false;
    bool verifyOk = false;
    bool encrypted = false;
    uint32_t targetFps = 0;
    uint32_t frames = 0;
    uint32_t failedFrameAttempts = 0;
    uint32_t overBudgetFrames = 0;
    uint64_t logicalBytes = 0;
    uint64_t physicalBytes = 0;
    uint64_t elapsedUs = 0;
    uint64_t activeFrameUs = 0;
    uint32_t p95Us = 0;
    uint32_t p99Us = 0;
    uint32_t worstUs = 0;
    uint32_t finalizeMs = 0;
};

BenchConfig benchCfg;
Result results[2];
uint32_t latenciesUs[MAX_LATENCY_SAMPLES];
size_t latencyCount = 0;

void printRule(char c = '=')
{
    for (int i = 0; i < 78; ++i)
        Serial.print(c);
    Serial.println();
}

void printCurrentBanner(uint32_t mhz)
{
    Serial.println();
    printRule('#');
    Serial.println("###                                                                    ###");
    Serial.printf ("###   JETZT NEUEN STROMVERBRAUCH ABLESEN: SD-SPI %2lu MHz              ###\n",
                   (unsigned long)mhz);
    Serial.println("###                                                                    ###");
    Serial.println("###   Aufnahme laeuft jetzt stabil. Wert am Strommessgeraet notieren.  ###");
    Serial.printf ("###   Messfenster: %lu Sekunden                                         ###\n",
                   (unsigned long)CURRENT_WINDOW_SECONDS);
    Serial.println("###                                                                    ###");
    printRule('#');
    Serial.println();
}

void printCurrentWindowEnd(uint32_t mhz)
{
    Serial.println();
    printRule('#');
    Serial.printf("### STROMMESSFENSTER %lu MHz BEENDET - naechsten Wert noch NICHT lesen. ###\n",
                  (unsigned long)mhz);
    printRule('#');
    Serial.println();
}

bool remountSd(uint32_t frequencyHz)
{
    SD.end();
    SPI.end();
    delay(50);

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

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

    return SD.cardType() != CARD_NONE;
}

bool parseIntStrict(const String &text, int &value)
{
    if (!text.length())
        return false;

    char *end = nullptr;
    long v = strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0')
        return false;

    value = (int)v;
    return true;
}

void applyConfigKey(const String &key, const String &value)
{
    int v = 0;

    if (key == "camera") {
        benchCfg.camera = value;
    } else if (key == "resolution") {
        benchCfg.resolution = value;
    } else if (key == "fps" && parseIntStrict(value, v) && v >= 1 && v <= 30) {
        benchCfg.fps = v;
    } else if (key == "quality" && parseIntStrict(value, v) && v >= 4 && v <= 63) {
        benchCfg.quality = v;
    } else if (key == "camera_xclk_mhz" && parseIntStrict(value, v) &&
               (v == 10 || v == 16 || v == 20)) {
        benchCfg.xclkMhz = v;
    } else if (key == "camera_auto_exposure" && parseIntStrict(value, v) &&
               (v == 0 || v == 1)) {
        benchCfg.autoExposure = v;
    } else if (key == "camera_ae_level" && parseIntStrict(value, v) && v >= -2 && v <= 2) {
        benchCfg.aeLevel = v;
    } else if (key == "rotation" && parseIntStrict(value, v) && (v == 0 || v == 180)) {
        benchCfg.rotation = v;
    } else if (key == "recording_encryption" && parseIntStrict(value, v) &&
               (v == 0 || v == 1)) {
        benchCfg.recordingEncryption = v;
    }
}

bool loadSensorForgeConfig()
{
    File f = SD.open("/config.txt", FILE_READ);
    if (!f) {
        Serial.println("Config: /config.txt not found - using SensorForge XIAO defaults.");
        return false;
    }

    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();

        if (!line.length() || line.startsWith("#") || line.startsWith(";"))
            continue;

        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;

        String key = line.substring(0, eq);
        String value = line.substring(eq + 1);
        key.trim();
        value.trim();
        applyConfigKey(key, value);
    }

    f.close();
    return true;
}

framesize_t frameSizeFor(const String &r)
{
    if (r == "160x120")   return FRAMESIZE_QQVGA;
    if (r == "320x240")   return FRAMESIZE_QVGA;
    if (r == "640x480")   return FRAMESIZE_VGA;
    if (r == "800x600")   return FRAMESIZE_SVGA;
    if (r == "1024x768")  return FRAMESIZE_XGA;
    if (r == "1280x1024") return FRAMESIZE_SXGA;
    if (r == "1600x1200") return FRAMESIZE_UXGA;
    if (r == "2048x1536") return FRAMESIZE_QXGA;
    return FRAMESIZE_XGA;
}

bool initCamera()
{
    camera_config_t cfg = {};

    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_pwdn = CAM_PWDN;
    cfg.pin_reset = CAM_RESET;
    cfg.pin_xclk = CAM_XCLK;
    cfg.pin_sccb_sda = CAM_SIOD;
    cfg.pin_sccb_scl = CAM_SIOC;
    cfg.pin_d7 = CAM_D7;
    cfg.pin_d6 = CAM_D6;
    cfg.pin_d5 = CAM_D5;
    cfg.pin_d4 = CAM_D4;
    cfg.pin_d3 = CAM_D3;
    cfg.pin_d2 = CAM_D2;
    cfg.pin_d1 = CAM_D1;
    cfg.pin_d0 = CAM_D0;
    cfg.pin_vsync = CAM_VSYNC;
    cfg.pin_href = CAM_HREF;
    cfg.pin_pclk = CAM_PCLK;
    cfg.xclk_freq_hz = (uint32_t)benchCfg.xclkMhz * 1000000UL;
    cfg.frame_size = frameSizeFor(benchCfg.resolution);
    cfg.jpeg_quality = benchCfg.quality;
    cfg.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        cfg.fb_location = CAMERA_FB_IN_PSRAM;
        cfg.fb_count = 2;
    } else {
        Serial.println("WARNING: PSRAM not detected - using one DRAM framebuffer.");
        cfg.fb_location = CAMERA_FB_IN_DRAM;
        cfg.fb_count = 1;
    }

    cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        if (sensor->set_exposure_ctrl)
            sensor->set_exposure_ctrl(sensor, benchCfg.autoExposure);

        if (sensor->set_ae_level)
            sensor->set_ae_level(sensor, benchCfg.aeLevel);

        int effectiveRotation =
            (CAMERA_BASE_ROTATION_DEGREES + benchCfg.rotation) % 360;

        if (effectiveRotation == 180) {
            if (sensor->set_hmirror) sensor->set_hmirror(sensor, 1);
            if (sensor->set_vflip) sensor->set_vflip(sensor, 1);
        } else {
            if (sensor->set_hmirror) sensor->set_hmirror(sensor, 0);
            if (sensor->set_vflip) sensor->set_vflip(sensor, 0);
        }
    }

    // Let automatic exposure/white balance settle before benchmarking.
    for (int i = 0; i < 8; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb)
            esp_camera_fb_return(fb);
        delay(80);
    }

    return true;
}

uint32_t percentileUs(float fraction)
{
    if (!latencyCount)
        return 0;

    static uint32_t sorted[MAX_LATENCY_SAMPLES];
    memcpy(sorted, latenciesUs, latencyCount * sizeof(uint32_t));
    std::sort(sorted, sorted + latencyCount);

    size_t index = (size_t)((latencyCount - 1) * fraction);
    if (index >= latencyCount)
        index = latencyCount - 1;

    return sorted[index];
}

bool verifyLogicalAvi(const String &path, uint64_t &logicalBytesRead)
{
    logicalBytesRead = 0;

    RecordingStorageFile f;
    if (!f.openRead(path)) {
        Serial.println("VERIFY: cannot open recording through production storage layer.");
        return false;
    }

    uint8_t header[12] = {};
    size_t got = f.read(header, sizeof(header));
    if (got != sizeof(header) ||
        memcmp(header + 0, "RIFF", 4) != 0 ||
        memcmp(header + 8, "AVI ", 4) != 0) {
        Serial.println("VERIFY: logical stream is not a RIFF/AVI file.");
        f.close();
        return false;
    }

    logicalBytesRead = sizeof(header);

    // Do not put a large read buffer on the Arduino loopTask stack.
    // The previous 16 KiB local array exhausted that stack and triggered
    // the ESP32-S3 stack-canary immediately when VERIFY started.
    // A static 4 KiB buffer lives in global/BSS memory instead and also
    // matches the production crypto slice size used by SensorForge.
    static uint8_t buffer[4 * 1024];

    while (true) {
        size_t n = f.read(buffer, sizeof(buffer));
        if (!n)
            break;
        logicalBytesRead += n;
    }

    bool ok = !f.failed() && logicalBytesRead == f.size();
    f.close();
    return ok;
}

uint64_t physicalFileSize(const String &path)
{
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f)
        return 0;
    uint64_t size = f.size();
    f.close();
    return size;
}

Result runPhase(uint32_t mhz, const String &path)
{
    Result r;
    r.mhz = mhz;
    r.encrypted = cfg_recording_encryption != 0;
    r.targetFps = (uint32_t)benchCfg.fps;

    Serial.println();
    printRule();
    Serial.printf("PREPARING REAL RECORDING TEST: SD SPI %lu MHz\n", (unsigned long)mhz);
    printRule();

    if (!remountSd(mhz * 1000000UL)) {
        Serial.println("ERROR: SD remount failed.");
        return r;
    }

    if (SD.exists(path.c_str()))
        SD.remove(path.c_str());

    Serial.printf("File       : %s\n", path.c_str());
    Serial.printf("Camera     : %s | %s | target %d fps | JPEG quality %d | XCLK %d MHz\n",
                  benchCfg.camera.c_str(), benchCfg.resolution.c_str(), benchCfg.fps,
                  benchCfg.quality, benchCfg.xclkMhz);
    Serial.printf("Encryption : %s\n", r.encrypted ? "SFENC1 ON" : "OFF");
    Serial.printf("SD SPI     : %lu MHz\n", (unsigned long)mhz);
    Serial.println("Do NOT read the current yet. Measurement marker appears after 5 s warm-up.");
    delay(3000);

    latencyCount = 0;
    uint64_t activeFrameUs = 0;
    uint32_t failedAttempts = 0;
    uint32_t overBudget = 0;
    const uint64_t frameBudgetUs = 1000000ULL / (uint64_t)max(1, benchCfg.fps);

    aviStart(path, benchCfg.fps);
    if (!aviIsOpen()) {
        Serial.println("ERROR: production AVI writer could not start.");
        return r;
    }

    const uint64_t phaseStartUs = esp_timer_get_time();
    const uint64_t measureStartUs =
        phaseStartUs + (uint64_t)PRE_MEASUREMENT_SECONDS * 1000000ULL;
    const uint64_t measureEndUs =
        measureStartUs + (uint64_t)CURRENT_WINDOW_SECONDS * 1000000ULL;
    const uint64_t phaseEndUs =
        phaseStartUs + (uint64_t)PHASE_SECONDS * 1000000ULL;

    bool measurementBannerShown = false;
    bool measurementEndShown = false;
    uint64_t nextFrameDueUs = phaseStartUs;

    while ((uint64_t)esp_timer_get_time() < phaseEndUs) {
        uint64_t nowUs = esp_timer_get_time();

        if (!measurementBannerShown && nowUs >= measureStartUs) {
            printCurrentBanner(mhz);
            measurementBannerShown = true;
        }

        if (!measurementEndShown && nowUs >= measureEndUs) {
            printCurrentWindowEnd(mhz);
            measurementEndShown = true;
        }

        if (nowUs < nextFrameDueUs) {
            uint64_t waitUs = nextFrameDueUs - nowUs;
            if (waitUs > 2000ULL)
                delay((uint32_t)(waitUs / 1000ULL) - 1U);
            else
                delayMicroseconds((uint32_t)waitUs);
            continue;
        }

        uint32_t framesBefore = aviGetFrameCount();
        uint64_t callStartUs = esp_timer_get_time();
        aviAddFrame();
        uint64_t callEndUs = esp_timer_get_time();
        uint32_t framesAfter = aviGetFrameCount();

        uint32_t callUs = (uint32_t)(callEndUs - callStartUs);
        activeFrameUs += callUs;

        if (framesAfter > framesBefore) {
            if (latencyCount < MAX_LATENCY_SAMPLES)
                latenciesUs[latencyCount++] = callUs;

            if ((uint64_t)callUs > frameBudgetUs)
                overBudget++;
        } else {
            failedAttempts++;
        }

        // Keep an absolute target cadence, but do not create catch-up bursts.
        nextFrameDueUs += frameBudgetUs;
        if (callEndUs > nextFrameDueUs)
            nextFrameDueUs = callEndUs;

        if (!aviIsHealthy() || aviHitSizeLimit())
            break;
    }

    if (!measurementBannerShown)
        printCurrentBanner(mhz);
    if (!measurementEndShown)
        printCurrentWindowEnd(mhz);

    r.elapsedUs = esp_timer_get_time() - phaseStartUs;
    r.frames = aviGetFrameCount();
    r.logicalBytes = aviGetBytesWritten();
    r.activeFrameUs = activeFrameUs;
    r.failedFrameAttempts = failedAttempts;
    r.overBudgetFrames = overBudget;
    r.p95Us = percentileUs(0.95f);
    r.p99Us = percentileUs(0.99f);
    r.worstUs = percentileUs(1.0f);

    uint64_t finalizeStartUs = esp_timer_get_time();
    bool finalOk = aviEnd();
    r.finalizeMs = (uint32_t)((esp_timer_get_time() - finalizeStartUs) / 1000ULL);

    r.physicalBytes = physicalFileSize(path);

    Serial.println("VERIFY: reading complete logical AVI back through production storage layer ...");
    uint64_t verifiedLogicalBytes = 0;
    r.verifyOk = finalOk && verifyLogicalAvi(path, verifiedLogicalBytes);

    if (r.verifyOk && verifiedLogicalBytes != r.logicalBytes) {
        Serial.printf("VERIFY WARNING: measured logical bytes=%llu, read back=%llu\n",
                      (unsigned long long)r.logicalBytes,
                      (unsigned long long)verifiedLogicalBytes);
    }

    r.ok = finalOk && r.verifyOk;

    double seconds = r.elapsedUs / 1000000.0;
    double actualFps = seconds > 0.0 ? (double)r.frames / seconds : 0.0;
    double videoMiBs = seconds > 0.0
        ? ((double)r.logicalBytes / 1048576.0) / seconds
        : 0.0;
    double activeMiBs = r.activeFrameUs > 0
        ? ((double)r.logicalBytes / 1048576.0) /
          ((double)r.activeFrameUs / 1000000.0)
        : 0.0;

    Serial.println();
    Serial.printf("RESULT %lu MHz\n", (unsigned long)mhz);
    Serial.printf("Status            : %s\n", r.ok ? "OK" : "FAILED");
    Serial.printf("Frames            : %lu\n", (unsigned long)r.frames);
    Serial.printf("Actual FPS        : %.3f (target %lu)\n", actualFps, (unsigned long)r.targetFps);
    Serial.printf("Logical video rate: %.3f MiB/s (paced real recording)\n", videoMiBs);
    Serial.printf("Active path rate  : %.3f MiB/s (camera capture + AVI/storage calls)\n", activeMiBs);
    Serial.printf("P95 frame call    : %.3f ms\n", r.p95Us / 1000.0);
    Serial.printf("P99 frame call    : %.3f ms\n", r.p99Us / 1000.0);
    Serial.printf("Worst frame call  : %.3f ms\n", r.worstUs / 1000.0);
    Serial.printf("Over frame budget : %lu\n", (unsigned long)r.overBudgetFrames);
    Serial.printf("Failed attempts   : %lu\n", (unsigned long)r.failedFrameAttempts);
    Serial.printf("Finalize          : %lu ms\n", (unsigned long)r.finalizeMs);
    Serial.printf("Logical AVI       : %.3f MiB\n", (double)r.logicalBytes / 1048576.0);
    Serial.printf("Physical SD file  : %.3f MiB\n", (double)r.physicalBytes / 1048576.0);
    Serial.printf("VERIFY            : %s\n", r.verifyOk ? "OK" : "FAILED");

    return r;
}

void printFinalSummary()
{
    Serial.println();
    printRule();
    Serial.println("FINAL SUMMARY - REAL SENSORFORGE CAMERA/AVI/SD PATH");
    printRule();
    Serial.println("freq_mhz,status,encryption,target_fps,frames,actual_fps,video_MiB_s,active_path_MiB_s,p95_ms,p99_ms,worst_ms,over_budget,failed_attempts,finalize_ms,logical_MiB,physical_MiB,verify");

    for (int i = 0; i < 2; ++i) {
        const Result &r = results[i];
        double seconds = r.elapsedUs / 1000000.0;
        double actualFps = seconds > 0.0 ? (double)r.frames / seconds : 0.0;
        double videoMiBs = seconds > 0.0
            ? ((double)r.logicalBytes / 1048576.0) / seconds
            : 0.0;
        double activeMiBs = r.activeFrameUs > 0
            ? ((double)r.logicalBytes / 1048576.0) /
              ((double)r.activeFrameUs / 1000000.0)
            : 0.0;

        Serial.printf("%lu,%s,%s,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%lu,%lu,%lu,%.3f,%.3f,%s\n",
            (unsigned long)r.mhz,
            r.ok ? "OK" : "FAILED",
            r.encrypted ? "SFENC1" : "OFF",
            (unsigned long)r.targetFps,
            (unsigned long)r.frames,
            actualFps,
            videoMiBs,
            activeMiBs,
            r.p95Us / 1000.0,
            r.p99Us / 1000.0,
            r.worstUs / 1000.0,
            (unsigned long)r.overBudgetFrames,
            (unsigned long)r.failedFrameAttempts,
            (unsigned long)r.finalizeMs,
            (double)r.logicalBytes / 1048576.0,
            (double)r.physicalBytes / 1048576.0,
            r.verifyOk ? "OK" : "FAILED"
        );
    }

    Serial.println();
    Serial.println("Bitte zusammen mit dieser Tabelle auch die zwei am Strommessgeraet");
    Serial.println("abgelesenen Werte fuer 4 MHz und 20 MHz an ChatGPT schicken.");
    printRule();
}

} // namespace

void setup()
{
    Serial.begin(115200);
    delay(2500);

    Serial.println();
    printRule();
    Serial.println("SensorForge SD CAMERA RECORDING BENCHMARK");
    Serial.println("XIAO ESP32S3 Sense | real camera + production AVI/storage path");
    printRule();

    Serial.println("Initial SD mount at 4 MHz for reading /config.txt ...");
    if (!remountSd(SD_SPI_NORMAL_FREQUENCY_HZ)) {
        Serial.println("FATAL: SD card could not be mounted at 4 MHz.");
        return;
    }

    loadSensorForgeConfig();
    cfg_recording_encryption = benchCfg.recordingEncryption;

    Serial.println();
    Serial.println("Effective benchmark settings:");
    Serial.printf("  camera               = %s\n", benchCfg.camera.c_str());
    Serial.printf("  resolution           = %s\n", benchCfg.resolution.c_str());
    Serial.printf("  fps                  = %d\n", benchCfg.fps);
    Serial.printf("  quality              = %d\n", benchCfg.quality);
    Serial.printf("  camera_xclk_mhz      = %d\n", benchCfg.xclkMhz);
    Serial.printf("  camera_auto_exposure = %d\n", benchCfg.autoExposure);
    Serial.printf("  camera_ae_level      = %d\n", benchCfg.aeLevel);
    Serial.printf("  rotation             = %d (+ XIAO base 180 deg)\n", benchCfg.rotation);
    Serial.printf("  recording_encryption = %d\n", benchCfg.recordingEncryption);

    if (cfg_recording_encryption) {
        Serial.println();
        Serial.println("Encryption requested by config.txt.");
        Serial.println("Safety check: benchmark will NOT provision/burn an eFuse key.");

        if (!recordingCryptoBegin() || !recordingCryptoReady()) {
            Serial.printf("FATAL: no existing SensorForge recording key is ready | status=%s\n",
                          recordingCryptoKeyStatusName());
            Serial.println("Run normal SensorForge provisioning first or benchmark with recording_encryption=0.");
            return;
        }

        Serial.printf("Existing key ready: eFuse KEY%d | source=%s\n",
                      recordingCryptoKeySlot(), recordingCryptoKeySourceName());
    } else {
        // Read-only discovery only; never provisions anything.
        recordingCryptoBegin();
    }

    Serial.println();
    Serial.println("Initializing camera with SensorForge settings ...");
    if (!initCamera()) {
        Serial.println("FATAL: camera initialization failed.");
        return;
    }

    Serial.println("Camera ready.");
    Serial.println();
    Serial.println("IMPORTANT FOR POWER MEASUREMENT:");
    Serial.println("- Keep camera scene and lighting unchanged for both phases.");
    Serial.println("- Do not compare the idle/setup periods.");
    Serial.println("- Read the meter ONLY when the large 'JETZT ... ABLESEN' banner appears.");
    Serial.println("- There will be one marked measurement window at 4 MHz and one at 20 MHz.");
    Serial.println();
    Serial.println("Starting first phase in 5 seconds ...");
    delay(5000);

    results[0] = runPhase(4, "/SF_BENCH_4MHZ.avi");

    Serial.println();
    Serial.println("4 MHz phase complete. Switching SD clock. Do NOT read the meter now.");
    Serial.println("20 MHz recording begins shortly ...");
    delay(5000);

    results[1] = runPhase(20, "/SF_BENCH_20MHZ.avi");

    printFinalSummary();

    Serial.println();
    Serial.println("Benchmark complete. Files remain on SD for inspection:");
    Serial.println("  /SF_BENCH_4MHZ.avi");
    Serial.println("  /SF_BENCH_20MHZ.avi");
    Serial.println("Reset the board to run the test again.");
}

void loop()
{
    delay(1000);
}