#pragma once

#include <Arduino.h>
#include <stdint.h>

static constexpr uint8_t IMAGE_MOTION_GRID_WIDTH = 20;
static constexpr uint8_t IMAGE_MOTION_GRID_HEIGHT = 15;
static constexpr uint16_t IMAGE_MOTION_GRID_CELLS =
    (uint16_t)IMAGE_MOTION_GRID_WIDTH * (uint16_t)IMAGE_MOTION_GRID_HEIGHT;
static constexpr uint8_t IMAGE_MOTION_ROI_BYTES =
    (IMAGE_MOTION_GRID_CELLS + 7U) / 8U;
static constexpr uint8_t IMAGE_MOTION_ROI_HEX_CHARS =
    IMAGE_MOTION_ROI_BYTES * 2U;
static constexpr uint16_t IMAGE_MOTION_DIAGNOSTIC_TARGET_CAPACITY = 400;

enum ImageMotionState : uint8_t {
    IMAGE_MOTION_STATE_DISABLED = 0,
    IMAGE_MOTION_STATE_BACKGROUND_INIT,
    IMAGE_MOTION_STATE_IDLE,
    IMAGE_MOTION_STATE_CANDIDATE,
    IMAGE_MOTION_STATE_CONFIRMED,
    IMAGE_MOTION_STATE_GLOBAL_CHANGE,
    IMAGE_MOTION_STATE_ERROR
};

enum ImageMotionRejectReason : uint8_t {
    IMAGE_MOTION_REJECT_NONE = 0,
    IMAGE_MOTION_REJECT_DISABLED,
    IMAGE_MOTION_REJECT_BACKGROUND_INIT,
    IMAGE_MOTION_REJECT_BELOW_AREA,
    IMAGE_MOTION_REJECT_CONFIRMING,
    IMAGE_MOTION_REJECT_GLOBAL_LIGHT,
    IMAGE_MOTION_REJECT_DECODE,
    IMAGE_MOTION_REJECT_INVALID_FRAME,
    IMAGE_MOTION_REJECT_NO_ROI
};

struct ImageMotionDiagnostics {
    uint32_t analyzeFrameMs = 0;
    uint32_t decodeMs = 0;
    uint16_t activeRoiBlocks = 0;
    uint16_t changedBlocks = 0;
    uint16_t largestClusterBlocks = 0;
    float changedAreaPct = 0.0f;
    float globalChangePct = 0.0f;
    float globalMean = 0.0f;
    float globalMeanDelta = 0.0f;
    float motionScore = 0.0f;
    uint16_t blockThreshold = 0;
    uint16_t dynamicThresholdMin = 0;
    uint16_t dynamicThresholdAvgX10 = 0;
    uint16_t dynamicThresholdMax = 0;
    uint16_t meanAbsDiffX10 = 0;
    uint16_t maxAbsDiff = 0;

    // Diagnostic-only frame-to-frame motion. These values compare the current
    // analyzed 20x15 block means with the immediately preceding analyzed frame.
    // They do not currently influence motionActive/state.
    uint32_t frameDeltaIntervalMs = 0;
    uint16_t frameDeltaThreshold = 0;
    uint16_t frameChangedBlocks = 0;
    uint16_t frameLargestClusterBlocks = 0;
    uint16_t frameMeanAbsDiffX10 = 0;
    uint16_t frameMaxAbsDiff = 0;
    uint16_t frameDiffGe5 = 0;
    uint16_t frameDiffGe10 = 0;
    uint16_t frameDiffGe15 = 0;
    uint16_t frameDiffGe20 = 0;
    uint16_t frameClusterGe10 = 0;
    uint16_t frameClusterGe15 = 0;
    uint16_t frameClusterGe20 = 0;
    float frameChangedPct = 0.0f;
    float frameClusterPct = 0.0f;
    bool frameDeltaReady = false;

    uint16_t diffGe5 = 0;
    uint16_t diffGe10 = 0;
    uint16_t diffGe15 = 0;
    uint16_t diffGe20 = 0;
    uint16_t diffGe25 = 0;
    uint16_t diffGe30 = 0;
    uint16_t diffGe35 = 0;
    uint16_t diffGe40 = 0;
    uint16_t clusterGe10 = 0;
    uint16_t clusterGe15 = 0;
    uint16_t clusterGe20 = 0;
    uint16_t clusterGe25 = 0;
    uint16_t clusterGe30 = 0;
    uint16_t clusterGe35 = 0;
    uint16_t minimumMotionBlocks = 0;
    uint8_t confirmCounter = 0;
    uint8_t releaseCounter = 0;
    bool backgroundReady = false;
    bool motionActive = false;
    ImageMotionState state = IMAGE_MOTION_STATE_DISABLED;
    ImageMotionRejectReason rejectReason = IMAGE_MOTION_REJECT_DISABLED;
};

// Compact RAM-only snapshot used for post-test diagnostics. The changed mask
// stores one bit per 20x15 grid cell after the real adaptive threshold has been
// applied. The ring buffer is intentionally independent from SD/main logging.
struct ImageMotionDiagnosticSample {
    uint32_t sequence = 0;
    uint32_t uptimeMs = 0;
    uint32_t epochSec = 0;
    uint16_t epochMs = 0;
    uint16_t sourceWidth = 0;
    uint16_t sourceHeight = 0;
    uint16_t analyzeFrameMs = 0;
    uint16_t decodeMs = 0;
    uint16_t activeRoiBlocks = 0;
    uint16_t changedBlocks = 0;
    uint16_t largestClusterBlocks = 0;
    uint16_t minimumMotionBlocks = 0;
    uint16_t blockThreshold = 0;
    uint16_t dynamicThresholdMin = 0;
    uint16_t dynamicThresholdAvgX10 = 0;
    uint16_t dynamicThresholdMax = 0;
    uint16_t meanAbsDiffX10 = 0;
    uint16_t maxAbsDiff = 0;

    uint16_t frameDeltaIntervalMs = 0;
    uint16_t frameDeltaThreshold = 0;
    uint16_t frameChangedBlocks = 0;
    uint16_t frameLargestClusterBlocks = 0;
    uint16_t frameMeanAbsDiffX10 = 0;
    uint16_t frameMaxAbsDiff = 0;
    uint16_t frameDiffGe5 = 0;
    uint16_t frameDiffGe10 = 0;
    uint16_t frameDiffGe15 = 0;
    uint16_t frameDiffGe20 = 0;
    uint16_t frameClusterGe10 = 0;
    uint16_t frameClusterGe15 = 0;
    uint16_t frameClusterGe20 = 0;

    uint16_t diffGe5 = 0;
    uint16_t diffGe10 = 0;
    uint16_t diffGe15 = 0;
    uint16_t diffGe20 = 0;
    uint16_t diffGe25 = 0;
    uint16_t diffGe30 = 0;
    uint16_t diffGe35 = 0;
    uint16_t diffGe40 = 0;
    uint16_t clusterGe10 = 0;
    uint16_t clusterGe15 = 0;
    uint16_t clusterGe20 = 0;
    uint16_t clusterGe25 = 0;
    uint16_t clusterGe30 = 0;
    uint16_t clusterGe35 = 0;
    int16_t globalMeanX10 = 0;
    int16_t globalMeanDeltaX10 = 0;
    uint8_t confirmCounter = 0;
    uint8_t releaseCounter = 0;
    uint8_t state = 0;
    uint8_t rejectReason = 0;
    uint8_t flags = 0;
    uint8_t changedMask[IMAGE_MOTION_ROI_BYTES] = {};
    uint8_t frameChangedMask[IMAGE_MOTION_ROI_BYTES] = {};
};


// Lightweight metrics for the continuous JPEG shooter. This analyzer shares the
// existing 1/8-scale JPEG decoder but maintains a completely separate reference
// image, so shooter filtering cannot alter the operational image-motion state.
struct ShooterImageMetrics {
    uint32_t analyzeFrameMs = 0;
    uint32_t decodeMs = 0;
    float globalMean = 0.0f;
    uint8_t brightestBlockMean = 0;
    float similarityPct = 0.0f;
    uint16_t changedBlocksGe5 = 0;
    uint16_t meanAbsDiffX10 = 0;
    uint16_t maxAbsDiff = 0;
    bool referenceReady = false;
};

bool imageMotionAnalyzeShooterJpeg(
    const uint8_t *jpeg,
    size_t jpegLength,
    uint16_t sourceWidth,
    uint16_t sourceHeight,
    ShooterImageMetrics &metrics
);

// Commit the most recently measured shooter frame as the similarity reference.
// Call only after that frame has been accepted for persistence.
bool imageMotionCommitShooterReference();
void imageMotionResetShooterReference();

String imageMotionDefaultRoiMask();
bool imageMotionValidateRoiMask(const String &text);
bool imageMotionDecodeRoiMask(const String &text, uint8_t mask[IMAGE_MOTION_ROI_BYTES]);
bool imageMotionRoiCellEnabled(const String &text, uint16_t cellIndex);

void imageMotionResetBackground();
void imageMotionBeginVerification();

bool imageMotionAnalyzeJpeg(
    const uint8_t *jpeg,
    size_t jpegLength,
    uint16_t sourceWidth,
    uint16_t sourceHeight,
    ImageMotionDiagnostics &diagnostics
);

// Called by the recorder with the JPEG frame that was just written. The
// function is a cheap no-op unless image_only is active; in that mode it keeps
// the image-motion state alive during recording so release_frames and the
// normal post_record_ms window can end/restart the event without radar/PIR
// deciding recording duration.
void imageMotionObserveRecordingJpeg(
    const uint8_t *jpeg,
    size_t jpegLength,
    uint16_t sourceWidth,
    uint16_t sourceHeight
);

bool imageMotionMotionActive();
uint32_t imageMotionLastAnalysisCompletedMs();

const ImageMotionDiagnostics &imageMotionLastDiagnostics();
const char *imageMotionStateName(ImageMotionState state);
const char *imageMotionRejectReasonName(ImageMotionRejectReason reason);
String imageMotionDiagnosticsJson();

uint16_t imageMotionDiagnosticCount();
uint16_t imageMotionDiagnosticCapacity();
void imageMotionDiagnosticClear();
bool imageMotionDiagnosticGet(
    uint16_t index,
    ImageMotionDiagnosticSample &sample
);
