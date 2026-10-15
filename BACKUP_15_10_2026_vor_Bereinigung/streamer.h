#pragma once

// Codec implementation capability. Keep separate from board hardware capability:
// ONVIF may advertise H.264 only when both the board and streamer support it.
#define SENSORFORGE_STREAMER_H264_IMPLEMENTED 0

#include <Arduino.h>
#include <WiFi.h>

class WebServer;

// Runtime mode is derived only from the persistent operating_mode config key.
// Missing/legacy keys default to normal mode in config.cpp.
bool streamerModeEnabled();

// Starts/stops the dedicated RTSP server and shared camera frame pipeline.
// Camera/WiFi must already be initialized by the main firmware.
bool streamerBegin(String &error);
void streamerLoop();
// Main firmware performs the actual AP/WebConfig restart because it owns that
// lifecycle. The streamer only requests recovery after repeated health-check
// failures and receives the result here.
bool streamerTakeNetworkRecoveryRequest(String &reason);
void streamerNoteNetworkRecoveryResult(bool success, const String &error);
void streamerStop();

// Register WebConfig routes owned by the streamer (/stream and status JSON).
// The settings page itself remains in webconfig.cpp so it shares the normal UI.
void streamerRegisterWebRoutes(WebServer &server);

// Optional internal observer for the exact cached JPEG served by
// streamerSendSnapshot(). The pointer is valid only for the duration of the
// callback and must not be retained. This lets WebConfig diagnostics consume
// the shared streamer frame without introducing another camera owner.
using StreamerSnapshotObserver = void (*)(
    const uint8_t *jpeg,
    size_t len,
    uint16_t width,
    uint16_t height
);

// Sends the most recently captured JPEG without taking a second camera frame.
// If supplied, observer is called after a successful send with that same JPEG.
bool streamerSendSnapshot(
    WebServer &server,
    StreamerSnapshotObserver observer = nullptr
);

// WebConfig camera preview is an internal consumer of the existing streamer
// capture pipeline. It never owns the camera itself; these hooks only raise or
// clear capture demand while the preview page is actively requesting frames.
void streamerNotePreviewActivity();
void streamerClearPreviewDemand();

bool streamerRtspClientConnected();
uint8_t streamerRtspClientCount();
bool streamerHttpClientConnected();
uint8_t streamerHttpClientCount();
bool streamerAudioActive();
bool streamerAudioAvailable();
String streamerAudioStatus();
bool streamerReady();
String streamerLastError();
uint32_t streamerFramesCaptured();
uint32_t streamerFramesSentRtsp();
uint32_t streamerFramesSentHttp();
uint64_t streamerBytesSent();
float streamerMeasuredFps();

// URLs shown by WebConfig/API. They intentionally follow the existing hostname.
String streamerRtspUrl();
String streamerHttpUrl();
String streamerHttpViewerUrl();
