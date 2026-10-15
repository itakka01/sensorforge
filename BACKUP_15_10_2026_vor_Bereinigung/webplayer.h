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

// Local-admin API wrappers built on the same recording-player/storage primitives.
// Return an HTTP-like status code (200/202/400/404/409/500/503) and a short
// machine-readable-safe message. They preserve the player storage/recording gates
// without changing the existing browser routes.
int webPlayerApiReadAnnotation(
    const String &path,
    String &text,
    String &message
);

int webPlayerApiWriteAnnotation(
    const String &path,
    const String &requestedText,
    String &savedText,
    String &message
);

int webPlayerApiDeleteMedia(
    const String &path,
    String &message
);
