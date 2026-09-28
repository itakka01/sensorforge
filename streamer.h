#pragma once

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
void streamerStop();

// Register WebConfig routes owned by the streamer (/stream and status JSON).
// The settings page itself remains in webconfig.cpp so it shares the normal UI.
void streamerRegisterWebRoutes(WebServer &server);

// Sends the most recently captured JPEG without taking a second camera frame.
bool streamerSendSnapshot(WebServer &server);

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
