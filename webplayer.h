#pragma once

#include <WebServer.h>

// Register the recording-player HTTP routes on the existing WebServer.
void webPlayerRegisterRoutes(WebServer &server);

// Housekeeping; call regularly from webConfigLoop().
void webPlayerLoop();

// Close any open playback file/session.
void webPlayerStop();

// Read only the minimal AVI/MKV metadata required by the recording list.
// Sparse MKV probing stops before the first Cluster and never scans media payloads.
bool webPlayerProbeMediaInfo(
    const String &path,
    uint64_t &durationMs,
    bool &hasAudio
);

bool webPlayerProbeDurationMs(
    const String &path,
    uint64_t &durationMs
);
