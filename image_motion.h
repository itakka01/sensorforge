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
    uint16_t minimumMotionBlocks = 0;
    uint8_t confirmCounter = 0;
    uint8_t releaseCounter = 0;
    bool backgroundReady = false;
    bool motionActive = false;
    ImageMotionState state = IMAGE_MOTION_STATE_DISABLED;
    ImageMotionRejectReason rejectReason = IMAGE_MOTION_REJECT_DISABLED;
};

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

const ImageMotionDiagnostics &imageMotionLastDiagnostics();
const char *imageMotionStateName(ImageMotionState state);
const char *imageMotionRejectReasonName(ImageMotionRejectReason reason);
String imageMotionDiagnosticsJson();
