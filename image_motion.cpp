#include "image_motion.h"
#include "config.h"

#include <esp_heap_caps.h>
#include <img_converters.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>

namespace {

static uint16_t backgroundQ8[IMAGE_MOTION_GRID_CELLS] = {};
static uint16_t noiseQ8[IMAGE_MOTION_GRID_CELLS] = {};
static bool backgroundReadyState = false;
static uint8_t confirmCounterState = 0;
static uint8_t releaseCounterState = 0;
static bool motionActiveState = false;
static uint32_t lastRecordingAnalyzeMs = 0;

static uint8_t *rgb565Buffer = nullptr;
static size_t rgb565Capacity = 0;

static String cachedRoiText;
static uint8_t cachedRoiMask[IMAGE_MOTION_ROI_BYTES] = {};
static bool cachedRoiValid = false;

static ImageMotionDiagnostics lastDiagnostics;
static uint32_t lastConfirmedDetectionMs = 0;
static uint32_t lastAnalysisCompletedMs = 0;
static uint32_t backgroundConfigHash = 0;
static bool backgroundConfigHashValid = false;

// Reused analysis work buffers. The analyzer is called synchronously from the
// main loop/WebConfig handler, so keeping these out of the loopTask stack is
// both safe and friendlier to the ESP32-S3's limited internal stack.
static uint32_t workBlockSums[IMAGE_MOTION_GRID_CELLS] = {};
static uint16_t workBlockCounts[IMAGE_MOTION_GRID_CELLS] = {};
static uint8_t workBlockMeans[IMAGE_MOTION_GRID_CELLS] = {};
// Previous analyzed frame, used only to estimate short-term camera/JPEG jitter.
// This is deliberately separate from the slowly adapting scene background so
// exposure drift or a slow real object cannot inflate the noise threshold.
static uint8_t previousBlockMeans[IMAGE_MOTION_GRID_CELLS] = {};
static bool previousFrameReady = false;
static uint8_t workAbsDiff[IMAGE_MOTION_GRID_CELLS] = {};
static bool workChanged[IMAGE_MOTION_GRID_CELLS] = {};
static bool workVisited[IMAGE_MOTION_GRID_CELLS] = {};
static uint16_t workStack[IMAGE_MOTION_GRID_CELLS] = {};

// Separate RAM-only diagnostic ring buffer. The preferred 400 samples live in
// PSRAM so normal internal heap pressure stays low. A smaller internal-RAM
// fallback keeps diagnostics available on unusual builds without PSRAM.
static ImageMotionDiagnosticSample *diagnosticBuffer = nullptr;
static uint16_t diagnosticCapacityState = 0;
static uint16_t diagnosticHead = 0;
static uint16_t diagnosticStored = 0;
static uint32_t diagnosticSequence = 0;

static bool ensureDiagnosticBuffer()
{
    if (diagnosticBuffer && diagnosticCapacityState > 0)
        return true;

    diagnosticCapacityState = IMAGE_MOTION_DIAGNOSTIC_TARGET_CAPACITY;
    diagnosticBuffer = (ImageMotionDiagnosticSample *)heap_caps_calloc(
        diagnosticCapacityState,
        sizeof(ImageMotionDiagnosticSample),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!diagnosticBuffer) {
        diagnosticCapacityState = 128;
        diagnosticBuffer = (ImageMotionDiagnosticSample *)heap_caps_calloc(
            diagnosticCapacityState,
            sizeof(ImageMotionDiagnosticSample),
            MALLOC_CAP_8BIT
        );
    }

    if (!diagnosticBuffer) {
        diagnosticCapacityState = 0;
        return false;
    }

    return true;
}

static uint16_t clampU16(uint32_t value)
{
    return value > 65535UL ? 65535U : (uint16_t)value;
}

static int16_t floatToDeciSigned(float value)
{
    long scaled = lroundf(value * 10.0f);
    if (scaled < -32768L)
        scaled = -32768L;
    if (scaled > 32767L)
        scaled = 32767L;
    return (int16_t)scaled;
}

static void captureDiagnosticSample(
    const ImageMotionDiagnostics &diagnostics,
    uint16_t sourceWidth,
    uint16_t sourceHeight
)
{
    if (!ensureDiagnosticBuffer())
        return;

    ImageMotionDiagnosticSample sample;
    sample.sequence = ++diagnosticSequence;
    sample.uptimeMs = millis();

    struct timeval tv;
    if (gettimeofday(&tv, nullptr) == 0 && tv.tv_sec >= 1577836800LL) {
        sample.epochSec = (uint32_t)tv.tv_sec;
        sample.epochMs = (uint16_t)(tv.tv_usec / 1000L);
    }

    sample.sourceWidth = sourceWidth;
    sample.sourceHeight = sourceHeight;
    sample.analyzeFrameMs = clampU16(diagnostics.analyzeFrameMs);
    sample.decodeMs = clampU16(diagnostics.decodeMs);
    sample.activeRoiBlocks = diagnostics.activeRoiBlocks;
    sample.changedBlocks = diagnostics.changedBlocks;
    sample.largestClusterBlocks = diagnostics.largestClusterBlocks;
    sample.minimumMotionBlocks = diagnostics.minimumMotionBlocks;
    sample.blockThreshold = diagnostics.blockThreshold;
    sample.dynamicThresholdMin = diagnostics.dynamicThresholdMin;
    sample.dynamicThresholdAvgX10 = diagnostics.dynamicThresholdAvgX10;
    sample.dynamicThresholdMax = diagnostics.dynamicThresholdMax;
    sample.meanAbsDiffX10 = diagnostics.meanAbsDiffX10;
    sample.maxAbsDiff = diagnostics.maxAbsDiff;
    sample.diffGe5 = diagnostics.diffGe5;
    sample.diffGe10 = diagnostics.diffGe10;
    sample.diffGe15 = diagnostics.diffGe15;
    sample.diffGe20 = diagnostics.diffGe20;
    sample.diffGe25 = diagnostics.diffGe25;
    sample.diffGe30 = diagnostics.diffGe30;
    sample.diffGe35 = diagnostics.diffGe35;
    sample.diffGe40 = diagnostics.diffGe40;
    sample.clusterGe10 = diagnostics.clusterGe10;
    sample.clusterGe15 = diagnostics.clusterGe15;
    sample.clusterGe20 = diagnostics.clusterGe20;
    sample.clusterGe25 = diagnostics.clusterGe25;
    sample.clusterGe30 = diagnostics.clusterGe30;
    sample.clusterGe35 = diagnostics.clusterGe35;
    sample.globalMeanX10 = floatToDeciSigned(diagnostics.globalMean);
    sample.globalMeanDeltaX10 = floatToDeciSigned(diagnostics.globalMeanDelta);
    sample.confirmCounter = diagnostics.confirmCounter;
    sample.releaseCounter = diagnostics.releaseCounter;
    sample.state = (uint8_t)diagnostics.state;
    sample.rejectReason = (uint8_t)diagnostics.rejectReason;
    sample.flags =
        (diagnostics.backgroundReady ? 0x01U : 0U) |
        (diagnostics.motionActive ? 0x02U : 0U);

    memset(sample.changedMask, 0, sizeof(sample.changedMask));
    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        if (workChanged[i])
            sample.changedMask[i >> 3] |= (uint8_t)(1U << (i & 7U));
    }

    diagnosticBuffer[diagnosticHead] = sample;
    diagnosticHead = (uint16_t)((diagnosticHead + 1U) % diagnosticCapacityState);
    if (diagnosticStored < diagnosticCapacityState)
        ++diagnosticStored;
}

static uint32_t hashByte(uint32_t hash, uint8_t value)
{
    hash ^= value;
    hash *= 16777619UL;
    return hash;
}

static uint32_t hashString(uint32_t hash, const String &value)
{
    for (size_t i = 0; i < value.length(); ++i)
        hash = hashByte(hash, (uint8_t)value[i]);
    return hashByte(hash, 0xFFU);
}

static uint32_t hashInt(uint32_t hash, int value)
{
    uint32_t v = (uint32_t)value;
    for (uint8_t i = 0; i < 4; ++i) {
        hash = hashByte(hash, (uint8_t)(v & 0xFFU));
        v >>= 8;
    }
    return hash;
}

static uint32_t currentBackgroundConfigHash()
{
    // Only parameters that change image geometry, exposure behavior, ROI or
    // the interpretation of block differences belong here. A changed hash
    // invalidates the learned scene before the next verification.
    uint32_t hash = 2166136261UL;
    hash = hashString(hash, cfg_resolution);
    hash = hashInt(hash, cfg_rotation);
    hash = hashInt(hash, cfg_camera_auto_exposure);
    hash = hashInt(hash, cfg_camera_ae_level);
    hash = hashString(hash, cfg_camera_crop_zoom);
    hash = hashInt(hash, cfg_camera_crop_x);
    hash = hashInt(hash, cfg_camera_crop_y);
    hash = hashInt(hash, cfg_image_motion_sensitivity);
    hash = hashString(hash, cfg_image_motion_roi_mask);
    return hash;
}

static void resetLearnedState()
{
    memset(backgroundQ8, 0, sizeof(backgroundQ8));
    memset(noiseQ8, 0, sizeof(noiseQ8));
    backgroundReadyState = false;
    confirmCounterState = 0;
    releaseCounterState = 0;
    motionActiveState = false;
    previousFrameReady = false;
    memset(previousBlockMeans, 0, sizeof(previousBlockMeans));
    lastRecordingAnalyzeMs = 0;
    lastConfirmedDetectionMs = 0;
    lastAnalysisCompletedMs = 0;
}

static uint8_t hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (uint8_t)(c - 'A' + 10);
    return 0xFFU;
}

static bool ensureDecodeBuffer(size_t bytes)
{
    if (bytes <= rgb565Capacity && rgb565Buffer)
        return true;

    if (rgb565Buffer) {
        free(rgb565Buffer);
        rgb565Buffer = nullptr;
        rgb565Capacity = 0;
    }

    rgb565Buffer = (uint8_t *)heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!rgb565Buffer) {
        rgb565Buffer = (uint8_t *)heap_caps_malloc(
            bytes,
            MALLOC_CAP_8BIT
        );
    }

    if (!rgb565Buffer)
        return false;

    rgb565Capacity = bytes;
    return true;
}

static bool currentRoiMask(uint8_t mask[IMAGE_MOTION_ROI_BYTES])
{
    if (
        !cachedRoiValid ||
        cachedRoiText != cfg_image_motion_roi_mask
    ) {
        cachedRoiText = cfg_image_motion_roi_mask;
        cachedRoiValid = imageMotionDecodeRoiMask(
            cachedRoiText,
            cachedRoiMask
        );

        if (!cachedRoiValid) {
            String fallback = imageMotionDefaultRoiMask();
            cachedRoiValid = imageMotionDecodeRoiMask(
                fallback,
                cachedRoiMask
            );
        }
    }

    if (!cachedRoiValid)
        return false;

    memcpy(mask, cachedRoiMask, IMAGE_MOTION_ROI_BYTES);
    return true;
}

static uint8_t grayFromRgb565(const uint8_t *pixel)
{
    // jpg2rgb565() uses esp_jpeg with swap_color_bytes=0. On ESP32-S3 the ROM
    // decoder is RGB888 and the converter writes RGB565 little-endian: low byte
    // first, high byte second. This is different from a native camera RGB565
    // framebuffer, which is why the byte order is handled locally here.
    uint8_t low = pixel[0];
    uint8_t high = pixel[1];

    uint8_t red = high & 0xF8U;
    uint8_t green = (uint8_t)(
        ((high & 0x07U) << 5) |
        ((low & 0xE0U) >> 3)
    );
    uint8_t blue = (uint8_t)((low & 0x1FU) << 3);

    return (uint8_t)(
        ((uint16_t)red * 77U +
         (uint16_t)green * 150U +
         (uint16_t)blue * 29U) >> 8
    );
}

static uint16_t largestConnectedCluster(
    const bool changed[IMAGE_MOTION_GRID_CELLS],
    const uint8_t roiMask[IMAGE_MOTION_ROI_BYTES]
)
{
    memset(workVisited, 0, sizeof(workVisited));
    uint16_t largest = 0;

    for (uint16_t start = 0; start < IMAGE_MOTION_GRID_CELLS; ++start) {
        if (!changed[start] || workVisited[start])
            continue;

        if ((roiMask[start >> 3] & (1U << (start & 7U))) == 0)
            continue;

        uint16_t count = 0;
        uint16_t top = 0;
        workStack[top++] = start;
        workVisited[start] = true;

        while (top > 0) {
            uint16_t index = workStack[--top];
            ++count;

            int x = index % IMAGE_MOTION_GRID_WIDTH;
            int y = index / IMAGE_MOTION_GRID_WIDTH;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;

                    int nx = x + dx;
                    int ny = y + dy;

                    if (
                        nx < 0 || nx >= IMAGE_MOTION_GRID_WIDTH ||
                        ny < 0 || ny >= IMAGE_MOTION_GRID_HEIGHT
                    ) {
                        continue;
                    }

                    uint16_t next =
                        (uint16_t)ny * IMAGE_MOTION_GRID_WIDTH +
                        (uint16_t)nx;

                    if (workVisited[next] || !changed[next])
                        continue;

                    if ((roiMask[next >> 3] & (1U << (next & 7U))) == 0)
                        continue;

                    workVisited[next] = true;
                    workStack[top++] = next;
                }
            }
        }

        if (count > largest)
            largest = count;
    }

    return largest;
}

static uint16_t largestConnectedClusterAtDifference(
    uint8_t minimumDifference,
    const uint8_t roiMask[IMAGE_MOTION_ROI_BYTES]
)
{
    memset(workVisited, 0, sizeof(workVisited));
    uint16_t largest = 0;

    for (uint16_t start = 0; start < IMAGE_MOTION_GRID_CELLS; ++start) {
        if (workVisited[start] || workAbsDiff[start] < minimumDifference)
            continue;
        if ((roiMask[start >> 3] & (1U << (start & 7U))) == 0)
            continue;

        uint16_t count = 0;
        uint16_t top = 0;
        workStack[top++] = start;
        workVisited[start] = true;

        while (top > 0) {
            uint16_t index = workStack[--top];
            ++count;

            int x = index % IMAGE_MOTION_GRID_WIDTH;
            int y = index / IMAGE_MOTION_GRID_WIDTH;

            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;

                    int nx = x + dx;
                    int ny = y + dy;
                    if (
                        nx < 0 || nx >= IMAGE_MOTION_GRID_WIDTH ||
                        ny < 0 || ny >= IMAGE_MOTION_GRID_HEIGHT
                    ) {
                        continue;
                    }

                    uint16_t next =
                        (uint16_t)ny * IMAGE_MOTION_GRID_WIDTH +
                        (uint16_t)nx;

                    if (
                        workVisited[next] ||
                        workAbsDiff[next] < minimumDifference ||
                        (roiMask[next >> 3] & (1U << (next & 7U))) == 0
                    ) {
                        continue;
                    }

                    workVisited[next] = true;
                    workStack[top++] = next;
                }
            }
        }

        if (count > largest)
            largest = count;
    }

    return largest;
}

static uint16_t sensitivityThreshold(uint8_t sensitivity, float globalMean)
{
    if (sensitivity < 1)
        sensitivity = 1;
    if (sensitivity > 10)
        sensitivity = 10;

    // Field-tuned curve from real OV3660 diagnostics. Sensitivity 5 should
    // react around 25 gray levels instead of the previous ~29 before noise
    // compensation. Higher sensitivity still means a lower threshold.
    int threshold = 40 - ((int)sensitivity * 3);

    // Dark scenes need a small extra reserve because JPEG/luma noise rises,
    // but the previous +6 made normal movement too easy to suppress.
    if (globalMean < 45.0f)
        threshold += 3;

    if (threshold < 8)
        threshold = 8;
    if (threshold > 44)
        threshold = 44;

    return (uint16_t)threshold;
}

static uint16_t minimumMotionBlocks(uint16_t activeBlocks)
{
    if (activeBlocks == 0)
        return 0;

    uint32_t blocks =
        ((uint32_t)activeBlocks *
         (uint32_t)cfg_image_motion_min_area_pct +
         99U) /
        100U;

    if (blocks < 1)
        blocks = 1;
    if (blocks > activeBlocks)
        blocks = activeBlocks;

    return (uint16_t)blocks;
}

static void updateBackgroundCell(
    uint16_t index,
    uint8_t current,
    uint16_t alphaQ8,
    bool updateNoise
)
{
    uint16_t currentQ8 = (uint16_t)current << 8;
    int32_t delta = (int32_t)currentQ8 - (int32_t)backgroundQ8[index];

    backgroundQ8[index] = (uint16_t)(
        (int32_t)backgroundQ8[index] +
        ((delta * (int32_t)alphaQ8) >> 8)
    );

    if (updateNoise && previousFrameReady) {
        // Estimate only short-term frame-to-frame jitter. Do NOT use the
        // distance from the slowly adapting background here: that caused
        // exposure drift and slow real movement to be learned as "noise" and
        // pushed the effective threshold into the high 30s.
        uint16_t frameJitter = (uint16_t)abs(
            (int32_t)current -
            (int32_t)previousBlockMeans[index]
        );

        // Large inter-frame jumps are likely real movement or a transition,
        // not sensor/JPEG noise. Ignore them for the noise estimator.
        if (frameJitter <= 12U) {
            uint16_t cappedJitter = min((uint16_t)8U, frameJitter);
            uint16_t targetQ8 = cappedJitter << 8;
            int32_t noiseDelta =
                (int32_t)targetQ8 -
                (int32_t)noiseQ8[index];

            noiseQ8[index] = (uint16_t)(
                (int32_t)noiseQ8[index] +
                (noiseDelta >> 4)
            );
        }
    }
}

static void initializeBackground(
    const uint8_t blockMeans[IMAGE_MOTION_GRID_CELLS]
)
{
    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        backgroundQ8[i] = (uint16_t)blockMeans[i] << 8;
        previousBlockMeans[i] = blockMeans[i];
        noiseQ8[i] = 2U << 8;
    }
    previousFrameReady = true;

    backgroundReadyState = true;
    confirmCounterState = 0;
    releaseCounterState = 0;
    motionActiveState = false;
}

static void setError(
    ImageMotionDiagnostics &diagnostics,
    ImageMotionRejectReason reason
)
{
    diagnostics.state = IMAGE_MOTION_STATE_ERROR;
    diagnostics.rejectReason = reason;
    diagnostics.backgroundReady = backgroundReadyState;
    diagnostics.motionActive = motionActiveState;
    lastDiagnostics = diagnostics;
    lastAnalysisCompletedMs = millis();
}

} // namespace

String imageMotionDefaultRoiMask()
{
    uint8_t bytes[IMAGE_MOTION_ROI_BYTES];
    memset(bytes, 0xFF, sizeof(bytes));

    const uint8_t validBitsInLastByte =
        IMAGE_MOTION_GRID_CELLS & 7U;

    if (validBitsInLastByte != 0) {
        bytes[IMAGE_MOTION_ROI_BYTES - 1U] =
            (uint8_t)((1U << validBitsInLastByte) - 1U);
    }

    static const char hex[] = "0123456789abcdef";
    String out;
    out.reserve(IMAGE_MOTION_ROI_HEX_CHARS);

    for (uint8_t value : bytes) {
        out += hex[(value >> 4) & 0x0FU];
        out += hex[value & 0x0FU];
    }

    return out;
}

bool imageMotionValidateRoiMask(const String &text)
{
    if (text.length() != IMAGE_MOTION_ROI_HEX_CHARS)
        return false;

    for (size_t i = 0; i < text.length(); ++i) {
        if (hexValue(text[i]) == 0xFFU)
            return false;
    }

    uint8_t mask[IMAGE_MOTION_ROI_BYTES];
    if (!imageMotionDecodeRoiMask(text, mask))
        return false;

    const uint8_t validBitsInLastByte =
        IMAGE_MOTION_GRID_CELLS & 7U;

    if (validBitsInLastByte != 0) {
        uint8_t invalidMask =
            (uint8_t)~((1U << validBitsInLastByte) - 1U);

        if ((mask[IMAGE_MOTION_ROI_BYTES - 1U] & invalidMask) != 0)
            return false;
    }

    return true;
}

bool imageMotionDecodeRoiMask(
    const String &text,
    uint8_t mask[IMAGE_MOTION_ROI_BYTES]
)
{
    if (!mask || text.length() != IMAGE_MOTION_ROI_HEX_CHARS)
        return false;

    for (uint8_t i = 0; i < IMAGE_MOTION_ROI_BYTES; ++i) {
        uint8_t high = hexValue(text[(size_t)i * 2U]);
        uint8_t low = hexValue(text[(size_t)i * 2U + 1U]);

        if (high == 0xFFU || low == 0xFFU)
            return false;

        mask[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

bool imageMotionRoiCellEnabled(const String &text, uint16_t cellIndex)
{
    if (cellIndex >= IMAGE_MOTION_GRID_CELLS)
        return false;

    uint8_t mask[IMAGE_MOTION_ROI_BYTES];
    if (!imageMotionDecodeRoiMask(text, mask))
        return false;

    return
        (mask[cellIndex >> 3] &
         (1U << (cellIndex & 7U))) != 0;
}

void imageMotionResetBackground()
{
    resetLearnedState();
    backgroundConfigHash = 0;
    backgroundConfigHashValid = false;
    lastDiagnostics = {};
}

void imageMotionBeginVerification()
{
    // A verification event gets a fresh temporal vote while retaining the
    // adaptive background across light sleep and between independent triggers.
    confirmCounterState = 0;
    releaseCounterState = 0;
    motionActiveState = false;
}

bool imageMotionAnalyzeJpeg(
    const uint8_t *jpeg,
    size_t jpegLength,
    uint16_t sourceWidth,
    uint16_t sourceHeight,
    ImageMotionDiagnostics &diagnostics
)
{
    diagnostics = {};

    uint32_t configHash = currentBackgroundConfigHash();
    if (
        backgroundConfigHashValid &&
        configHash != backgroundConfigHash
    ) {
        resetLearnedState();
    }
    backgroundConfigHash = configHash;
    backgroundConfigHashValid = true;

    diagnostics.backgroundReady = backgroundReadyState;

    if (
        !jpeg || jpegLength == 0 ||
        sourceWidth < IMAGE_MOTION_GRID_WIDTH ||
        sourceHeight < IMAGE_MOTION_GRID_HEIGHT
    ) {
        setError(diagnostics, IMAGE_MOTION_REJECT_INVALID_FRAME);
        return false;
    }

    uint8_t roiMask[IMAGE_MOTION_ROI_BYTES];
    if (!currentRoiMask(roiMask)) {
        setError(diagnostics, IMAGE_MOTION_REJECT_NO_ROI);
        return false;
    }

    uint16_t activeBlocks = 0;
    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        if (roiMask[i >> 3] & (1U << (i & 7U)))
            ++activeBlocks;
    }

    diagnostics.activeRoiBlocks = activeBlocks;

    if (activeBlocks == 0) {
        diagnostics.state = IMAGE_MOTION_STATE_IDLE;
        diagnostics.rejectReason = IMAGE_MOTION_REJECT_NO_ROI;
        lastDiagnostics = diagnostics;
        lastAnalysisCompletedMs = millis();
        return true;
    }

    uint32_t startedMs = millis();

    // esp_jpeg/TJpgDec reports scaled dimensions using integer division.
    const uint16_t scaledWidth =
        (uint16_t)(sourceWidth / 8U);
    const uint16_t scaledHeight =
        (uint16_t)(sourceHeight / 8U);

    if (
        scaledWidth < IMAGE_MOTION_GRID_WIDTH ||
        scaledHeight < IMAGE_MOTION_GRID_HEIGHT
    ) {
        setError(diagnostics, IMAGE_MOTION_REJECT_INVALID_FRAME);
        return false;
    }

    size_t needed =
        (size_t)scaledWidth *
        (size_t)scaledHeight *
        2U + 512U;

    if (!ensureDecodeBuffer(needed)) {
        setError(diagnostics, IMAGE_MOTION_REJECT_DECODE);
        return false;
    }

    uint32_t decodeStartedMs = millis();

    if (!jpg2rgb565(
            jpeg,
            jpegLength,
            rgb565Buffer,
            JPG_SCALE_8X
        )) {
        diagnostics.decodeMs = millis() - decodeStartedMs;
        diagnostics.analyzeFrameMs = millis() - startedMs;
        setError(diagnostics, IMAGE_MOTION_REJECT_DECODE);
        return false;
    }

    diagnostics.decodeMs = millis() - decodeStartedMs;

    memset(workBlockSums, 0, sizeof(workBlockSums));
    memset(workBlockCounts, 0, sizeof(workBlockCounts));
    memset(workBlockMeans, 0, sizeof(workBlockMeans));
    memset(workAbsDiff, 0, sizeof(workAbsDiff));
    memset(workChanged, 0, sizeof(workChanged));

    for (uint16_t y = 0; y < scaledHeight; ++y) {
        uint8_t by = (uint8_t)(
            ((uint32_t)y * IMAGE_MOTION_GRID_HEIGHT) /
            scaledHeight
        );
        if (by >= IMAGE_MOTION_GRID_HEIGHT)
            by = IMAGE_MOTION_GRID_HEIGHT - 1U;

        for (uint16_t x = 0; x < scaledWidth; ++x) {
            uint8_t bx = (uint8_t)(
                ((uint32_t)x * IMAGE_MOTION_GRID_WIDTH) /
                scaledWidth
            );
            if (bx >= IMAGE_MOTION_GRID_WIDTH)
                bx = IMAGE_MOTION_GRID_WIDTH - 1U;

            uint16_t block =
                (uint16_t)by * IMAGE_MOTION_GRID_WIDTH + bx;

            const uint8_t *pixel =
                rgb565Buffer +
                (((size_t)y * scaledWidth + x) * 2U);

            workBlockSums[block] += grayFromRgb565(pixel);
            workBlockCounts[block]++;
        }
    }

    uint64_t globalSum = 0;
    uint32_t globalCount = 0;

    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        if (workBlockCounts[i] == 0)
            continue;

        workBlockMeans[i] = (uint8_t)(
            workBlockSums[i] /
            workBlockCounts[i]
        );

        if (roiMask[i >> 3] & (1U << (i & 7U))) {
            globalSum += workBlockMeans[i];
            globalCount++;
        }
    }

    diagnostics.globalMean =
        globalCount > 0
        ? (float)globalSum / (float)globalCount
        : 0.0f;

    if (!backgroundReadyState) {
        initializeBackground(workBlockMeans);
        diagnostics.backgroundReady = true;
        diagnostics.state = IMAGE_MOTION_STATE_BACKGROUND_INIT;
        diagnostics.rejectReason = IMAGE_MOTION_REJECT_BACKGROUND_INIT;
        diagnostics.analyzeFrameMs = millis() - startedMs;
        lastDiagnostics = diagnostics;
        lastAnalysisCompletedMs = millis();
        captureDiagnosticSample(diagnostics, sourceWidth, sourceHeight);
        return true;
    }

    diagnostics.backgroundReady = true;

    uint16_t baseThreshold = sensitivityThreshold(
        (uint8_t)cfg_image_motion_sensitivity,
        diagnostics.globalMean
    );

    diagnostics.blockThreshold = baseThreshold;
    diagnostics.minimumMotionBlocks = minimumMotionBlocks(activeBlocks);

    uint16_t changedBlocks = 0;
    int64_t backgroundGlobalSum = 0;
    uint32_t absDifferenceSum = 0;
    uint32_t dynamicThresholdSum = 0;
    uint16_t dynamicThresholdMin = 0xFFFFU;
    uint16_t dynamicThresholdMax = 0;
    uint16_t maxAbsDifference = 0;
    uint16_t diffGe5 = 0;
    uint16_t diffGe10 = 0;
    uint16_t diffGe15 = 0;
    uint16_t diffGe20 = 0;
    uint16_t diffGe25 = 0;
    uint16_t diffGe30 = 0;
    uint16_t diffGe35 = 0;
    uint16_t diffGe40 = 0;

    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        if ((roiMask[i >> 3] & (1U << (i & 7U))) == 0)
            continue;

        uint8_t background = (uint8_t)(backgroundQ8[i] >> 8);
        uint8_t noise = (uint8_t)(noiseQ8[i] >> 8);
        // Keep adaptive compensation intentionally small. The previous
        // noise*2/+12 reserve dominated Sensitivity 5 in real diagnostics.
        uint16_t noiseReserve = (uint16_t)min(((int)noise + 1) / 2, 4);
        uint16_t dynamicThreshold = baseThreshold + noiseReserve;

        uint16_t difference = (uint16_t)abs(
            (int)workBlockMeans[i] -
            (int)background
        );
        workAbsDiff[i] = (uint8_t)min((uint16_t)255U, difference);

        backgroundGlobalSum += background;
        absDifferenceSum += difference;
        dynamicThresholdSum += dynamicThreshold;
        if (dynamicThreshold < dynamicThresholdMin)
            dynamicThresholdMin = dynamicThreshold;
        if (dynamicThreshold > dynamicThresholdMax)
            dynamicThresholdMax = dynamicThreshold;
        if (difference > maxAbsDifference)
            maxAbsDifference = difference;

        if (difference >= 5U) ++diffGe5;
        if (difference >= 10U) ++diffGe10;
        if (difference >= 15U) ++diffGe15;
        if (difference >= 20U) ++diffGe20;
        if (difference >= 25U) ++diffGe25;
        if (difference >= 30U) ++diffGe30;
        if (difference >= 35U) ++diffGe35;
        if (difference >= 40U) ++diffGe40;

        if (difference >= dynamicThreshold) {
            workChanged[i] = true;
            ++changedBlocks;
        }
    }

    diagnostics.changedBlocks = changedBlocks;
    diagnostics.dynamicThresholdMin =
        activeBlocks > 0 && dynamicThresholdMin != 0xFFFFU
        ? dynamicThresholdMin
        : 0;
    diagnostics.dynamicThresholdAvgX10 =
        activeBlocks > 0
        ? (uint16_t)((dynamicThresholdSum * 10UL + activeBlocks / 2U) / activeBlocks)
        : 0;
    diagnostics.dynamicThresholdMax = dynamicThresholdMax;
    diagnostics.meanAbsDiffX10 =
        activeBlocks > 0
        ? (uint16_t)((absDifferenceSum * 10UL + activeBlocks / 2U) / activeBlocks)
        : 0;
    diagnostics.maxAbsDiff = maxAbsDifference;
    diagnostics.diffGe5 = diffGe5;
    diagnostics.diffGe10 = diffGe10;
    diagnostics.diffGe15 = diffGe15;
    diagnostics.diffGe20 = diffGe20;
    diagnostics.diffGe25 = diffGe25;
    diagnostics.diffGe30 = diffGe30;
    diagnostics.diffGe35 = diffGe35;
    diagnostics.diffGe40 = diffGe40;
    diagnostics.globalChangePct =
        activeBlocks > 0
        ? ((float)changedBlocks * 100.0f) / (float)activeBlocks
        : 0.0f;

    float backgroundGlobalMean =
        activeBlocks > 0
        ? (float)backgroundGlobalSum / (float)activeBlocks
        : diagnostics.globalMean;

    diagnostics.globalMeanDelta =
        diagnostics.globalMean -
        backgroundGlobalMean;

    bool globalLightChange =
        fabsf(diagnostics.globalMeanDelta) >=
            (float)cfg_image_motion_global_mean_delta ||
        diagnostics.globalChangePct >=
            (float)cfg_image_motion_global_change_pct;

    uint16_t learningAlpha = (uint16_t)constrain(
        cfg_image_motion_background_learning,
        1,
        64
    );

    if (globalLightChange) {
        uint16_t fastAlpha = learningAlpha * 8U;
        if (fastAlpha < 64U)
            fastAlpha = 64U;
        if (fastAlpha > 192U)
            fastAlpha = 192U;

        for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
            if (roiMask[i >> 3] & (1U << (i & 7U))) {
                updateBackgroundCell(
                    i,
                    workBlockMeans[i],
                    fastAlpha,
                    false
                );
            }
        }

        confirmCounterState = 0;
        releaseCounterState = 0;
        motionActiveState = false;

        diagnostics.state = IMAGE_MOTION_STATE_GLOBAL_CHANGE;
        diagnostics.rejectReason = IMAGE_MOTION_REJECT_GLOBAL_LIGHT;
        diagnostics.confirmCounter = confirmCounterState;
        diagnostics.releaseCounter = releaseCounterState;
        diagnostics.motionActive = false;
        diagnostics.analyzeFrameMs = millis() - startedMs;
        memcpy(previousBlockMeans, workBlockMeans, sizeof(previousBlockMeans));
        previousFrameReady = true;
        lastDiagnostics = diagnostics;
        lastAnalysisCompletedMs = millis();
        captureDiagnosticSample(diagnostics, sourceWidth, sourceHeight);
        return true;
    }

    uint16_t largestCluster = largestConnectedCluster(
        workChanged,
        roiMask
    );

    diagnostics.clusterGe10 = largestConnectedClusterAtDifference(10U, roiMask);
    diagnostics.clusterGe15 = largestConnectedClusterAtDifference(15U, roiMask);
    diagnostics.clusterGe20 = largestConnectedClusterAtDifference(20U, roiMask);
    diagnostics.clusterGe25 = largestConnectedClusterAtDifference(25U, roiMask);
    diagnostics.clusterGe30 = largestConnectedClusterAtDifference(30U, roiMask);
    diagnostics.clusterGe35 = largestConnectedClusterAtDifference(35U, roiMask);

    diagnostics.largestClusterBlocks = largestCluster;
    diagnostics.changedAreaPct =
        activeBlocks > 0
        ? ((float)largestCluster * 100.0f) / (float)activeBlocks
        : 0.0f;

    diagnostics.motionScore =
        diagnostics.minimumMotionBlocks > 0
        ? diagnostics.changedAreaPct /
            max(1.0f, (float)cfg_image_motion_min_area_pct)
        : 0.0f;

    bool positive =
        largestCluster >= diagnostics.minimumMotionBlocks;

    if (positive) {
        releaseCounterState = 0;
        if (confirmCounterState < 255)
            ++confirmCounterState;

        if (
            confirmCounterState >=
            (uint8_t)cfg_image_motion_confirm_frames
        ) {
            motionActiveState = true;
            diagnostics.state = IMAGE_MOTION_STATE_CONFIRMED;
            diagnostics.rejectReason = IMAGE_MOTION_REJECT_NONE;
        } else {
            diagnostics.state = IMAGE_MOTION_STATE_CANDIDATE;
            diagnostics.rejectReason = IMAGE_MOTION_REJECT_CONFIRMING;
        }
    } else {
        confirmCounterState = 0;

        if (motionActiveState) {
            if (releaseCounterState < 255)
                ++releaseCounterState;

            if (
                releaseCounterState >=
                (uint8_t)cfg_image_motion_release_frames
            ) {
                motionActiveState = false;
                releaseCounterState = 0;
            }
        }

        diagnostics.state = motionActiveState
            ? IMAGE_MOTION_STATE_CONFIRMED
            : IMAGE_MOTION_STATE_IDLE;
        diagnostics.rejectReason = motionActiveState
            ? IMAGE_MOTION_REJECT_NONE
            : IMAGE_MOTION_REJECT_BELOW_AREA;
    }

    // Adaptive background: never learn blocks currently classified as moving.
    // Static/noisy blocks learn slowly; this lets foliage/exposure drift settle
    // without erasing an active local object immediately.
    for (uint16_t i = 0; i < IMAGE_MOTION_GRID_CELLS; ++i) {
        if ((roiMask[i >> 3] & (1U << (i & 7U))) == 0)
            continue;

        if (!workChanged[i]) {
            updateBackgroundCell(
                i,
                workBlockMeans[i],
                learningAlpha,
                true
            );
        }
    }

    memcpy(previousBlockMeans, workBlockMeans, sizeof(previousBlockMeans));
    previousFrameReady = true;

    diagnostics.confirmCounter = confirmCounterState;
    diagnostics.releaseCounter = releaseCounterState;
    diagnostics.motionActive = motionActiveState;
    diagnostics.analyzeFrameMs = millis() - startedMs;

    uint32_t completedMs = millis();
    if (positive && motionActiveState)
        lastConfirmedDetectionMs = completedMs;

    lastDiagnostics = diagnostics;
    lastAnalysisCompletedMs = completedMs;
    captureDiagnosticSample(diagnostics, sourceWidth, sourceHeight);
    return true;
}


void imageMotionObserveRecordingJpeg(
    const uint8_t *jpeg,
    size_t jpegLength,
    uint16_t sourceWidth,
    uint16_t sourceHeight
)
{
    if (cfg_motion_recording_decision != "image_only") {
        return;
    }

    // At high camera FPS there is no benefit in decoding every single JPEG for
    // motion. Five analyses per second keep the configured confirm/release frame
    // semantics responsive while bounding CPU/PSRAM pressure. At the current
    // production 5 fps this still observes every video frame.
    uint32_t now = millis();
    if (
        lastRecordingAnalyzeMs != 0 &&
        (uint32_t)(now - lastRecordingAnalyzeMs) < 180U
    ) {
        return;
    }

    lastRecordingAnalyzeMs = now;

    ImageMotionDiagnostics diagnostics;
    (void)imageMotionAnalyzeJpeg(
        jpeg,
        jpegLength,
        sourceWidth,
        sourceHeight,
        diagnostics
    );
}

bool imageMotionMotionActive()
{
    return motionActiveState;
}

const ImageMotionDiagnostics &imageMotionLastDiagnostics()
{
    return lastDiagnostics;
}

const char *imageMotionStateName(ImageMotionState state)
{
    switch (state) {
        case IMAGE_MOTION_STATE_DISABLED: return "disabled";
        case IMAGE_MOTION_STATE_BACKGROUND_INIT: return "background_init";
        case IMAGE_MOTION_STATE_IDLE: return "idle";
        case IMAGE_MOTION_STATE_CANDIDATE: return "candidate";
        case IMAGE_MOTION_STATE_CONFIRMED: return "confirmed";
        case IMAGE_MOTION_STATE_GLOBAL_CHANGE: return "global_change";
        case IMAGE_MOTION_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

const char *imageMotionRejectReasonName(ImageMotionRejectReason reason)
{
    switch (reason) {
        case IMAGE_MOTION_REJECT_NONE: return "none";
        case IMAGE_MOTION_REJECT_DISABLED: return "disabled";
        case IMAGE_MOTION_REJECT_BACKGROUND_INIT: return "background_init";
        case IMAGE_MOTION_REJECT_BELOW_AREA: return "below_area";
        case IMAGE_MOTION_REJECT_CONFIRMING: return "confirming";
        case IMAGE_MOTION_REJECT_GLOBAL_LIGHT: return "global_light";
        case IMAGE_MOTION_REJECT_DECODE: return "decode";
        case IMAGE_MOTION_REJECT_INVALID_FRAME: return "invalid_frame";
        case IMAGE_MOTION_REJECT_NO_ROI: return "no_roi";
        default: return "unknown";
    }
}

String imageMotionDiagnosticsJson()
{
    const ImageMotionDiagnostics &d = lastDiagnostics;

    String json;
    json.reserve(640);
    json += "{";
    json += "\"analyze_frame_ms\":" + String(d.analyzeFrameMs);
    json += ",\"decode_ms\":" + String(d.decodeMs);
    json += ",\"active_roi_blocks\":" + String(d.activeRoiBlocks);
    json += ",\"changed_blocks\":" + String(d.changedBlocks);
    json += ",\"largest_cluster_blocks\":" + String(d.largestClusterBlocks);
    json += ",\"changed_area_pct\":" + String(d.changedAreaPct, 2);
    json += ",\"global_change_pct\":" + String(d.globalChangePct, 2);
    json += ",\"global_mean\":" + String(d.globalMean, 2);
    json += ",\"global_mean_delta\":" + String(d.globalMeanDelta, 2);
    json += ",\"motion_score\":" + String(d.motionScore, 2);
    json += ",\"block_threshold\":" + String(d.blockThreshold);
    json += ",\"dynamic_threshold_min\":" + String(d.dynamicThresholdMin);
    json += ",\"dynamic_threshold_avg\":" + String((float)d.dynamicThresholdAvgX10 / 10.0f, 1);
    json += ",\"dynamic_threshold_max\":" + String(d.dynamicThresholdMax);
    json += ",\"mean_abs_diff\":" + String((float)d.meanAbsDiffX10 / 10.0f, 1);
    json += ",\"max_abs_diff\":" + String(d.maxAbsDiff);
    json += ",\"diff_ge_5\":" + String(d.diffGe5);
    json += ",\"diff_ge_10\":" + String(d.diffGe10);
    json += ",\"diff_ge_15\":" + String(d.diffGe15);
    json += ",\"diff_ge_20\":" + String(d.diffGe20);
    json += ",\"diff_ge_25\":" + String(d.diffGe25);
    json += ",\"diff_ge_30\":" + String(d.diffGe30);
    json += ",\"diff_ge_35\":" + String(d.diffGe35);
    json += ",\"diff_ge_40\":" + String(d.diffGe40);
    json += ",\"cluster_ge_10\":" + String(d.clusterGe10);
    json += ",\"cluster_ge_15\":" + String(d.clusterGe15);
    json += ",\"cluster_ge_20\":" + String(d.clusterGe20);
    json += ",\"cluster_ge_25\":" + String(d.clusterGe25);
    json += ",\"cluster_ge_30\":" + String(d.clusterGe30);
    json += ",\"cluster_ge_35\":" + String(d.clusterGe35);
    json += ",\"minimum_motion_blocks\":" + String(d.minimumMotionBlocks);
    json += ",\"image_motion_state\":\"" + String(imageMotionStateName(d.state)) + "\"";
    json += ",\"reject_reason\":\"" + String(imageMotionRejectReasonName(d.rejectReason)) + "\"";
    json += ",\"background_state\":\"" + String(d.backgroundReady ? "ready" : "uninitialized") + "\"";
    json += ",\"confirm_counter\":" + String(d.confirmCounter);
    json += ",\"release_counter\":" + String(d.releaseCounter);
    json += ",\"motion_active\":" + String(d.motionActive ? "true" : "false");
    json += ",\"last_detection_valid\":" + String(lastConfirmedDetectionMs != 0 ? "true" : "false");
    json += ",\"last_detection_age_ms\":" + String(
        lastConfirmedDetectionMs != 0
        ? (uint32_t)(millis() - lastConfirmedDetectionMs)
        : 0UL
    );
    json += ",\"last_analysis_valid\":" + String(lastAnalysisCompletedMs != 0 ? "true" : "false");
    json += ",\"last_analysis_age_ms\":" + String(
        lastAnalysisCompletedMs != 0
        ? (uint32_t)(millis() - lastAnalysisCompletedMs)
        : 0UL
    );
    json += "}";
    return json;
}

uint16_t imageMotionDiagnosticCount()
{
    return diagnosticStored;
}

uint16_t imageMotionDiagnosticCapacity()
{
    if (!diagnosticBuffer)
        (void)ensureDiagnosticBuffer();
    return diagnosticCapacityState;
}

void imageMotionDiagnosticClear()
{
    diagnosticHead = 0;
    diagnosticStored = 0;
    diagnosticSequence = 0;

    if (diagnosticBuffer && diagnosticCapacityState > 0) {
        memset(
            diagnosticBuffer,
            0,
            (size_t)diagnosticCapacityState * sizeof(ImageMotionDiagnosticSample)
        );
    }
}

bool imageMotionDiagnosticGet(
    uint16_t index,
    ImageMotionDiagnosticSample &sample
)
{
    if (!diagnosticBuffer || index >= diagnosticStored || diagnosticCapacityState == 0)
        return false;

    uint16_t oldest =
        diagnosticStored < diagnosticCapacityState
        ? 0U
        : diagnosticHead;

    uint16_t position = (uint16_t)(
        (oldest + index) % diagnosticCapacityState
    );

    sample = diagnosticBuffer[position];
    return true;
}

