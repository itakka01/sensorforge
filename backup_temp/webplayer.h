#pragma once

#include <WebServer.h>

// Register the recording-player HTTP routes on the existing WebServer.
void webPlayerRegisterRoutes(WebServer &server);

// Housekeeping; call regularly from webConfigLoop().
void webPlayerLoop();

// Close any open playback file/session.
void webPlayerStop();
