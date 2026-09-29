#pragma once

#include <Arduino.h>

// Streamer-specific WebConfig rendering only. HTTP handlers, persistence and
// runtime ownership remain in webconfig.cpp / streamer.cpp.
String webconfigStreamerDashboardHtml(uint8_t activeWebUiSessions);

String webconfigStreamerOperatingModeHtml(
    bool configuredStreamerMode,
    int displayedStreamerRtspEnabled,
    int displayedStreamerHttpEnabled
);
