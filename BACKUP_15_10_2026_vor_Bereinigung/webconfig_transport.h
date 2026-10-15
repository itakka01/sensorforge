#pragma once

#include <Arduino.h>
#include <WebServer.h>

struct WebConfigTransportUiHooks {
    String (*htmlHeader)();
    String (*htmlFooter)();
    String (*pageInfoButton)(const String &title, const String &info);
    void (*appendPageInfoUi)(String &html);
    bool (*pauseRecordingForOperation)(const char *operation);
    void (*renewRecordingPauseLease)(bool transportHold);
    void (*scheduleReboot)(uint32_t delayMs);
};

// Registers the dedicated transport-preparation/configuration surface.
// Config semantics, URLs and transport runtime ownership remain unchanged.
void webconfigTransportRegisterRoutes(
    WebServer &server,
    const WebConfigTransportUiHooks &uiHooks
);
