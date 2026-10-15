#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Shared page chrome stays owned by WebConfig. The Log Viewer receives only
// these two render callbacks so its routes, HTML/JavaScript, storage access and
// SFLOG1 handling can live in a separate translation unit without exposing
// WebConfig internals.
struct WebLogUiHooks {
    String (*htmlHeader)();
    String (*htmlFooter)();
};

// Registers /log, /log_raw, /log_chunk and /log_clear.
// Call once while WebConfig registers its other persistent routes.
void webLogRegisterRoutes(
    WebServer &server,
    const WebLogUiHooks &uiHooks
);
