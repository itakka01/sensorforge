#pragma once

#include <WebServer.h>

// Register the recording-player HTTP routes on the existing WebServer.
void webPlayerRegisterRoutes(WebServer &server);

// Housekeeping; call regularly from webConfigLoop().
void webPlayerLoop();

// Close any open playback file/session.
void webPlayerStop();

// Read only the minimal AVI/MKV metadata required for a list time range.
// Sparse MKV probing stops at the Info element and never scans all frames.
bool webPlayerProbeDurationMs(
    const String &path,
    uint64_t &durationMs
);
