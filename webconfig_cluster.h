#pragma once

#include <Arduino.h>
#include <WebServer.h>

struct WebConfigClusterUiHooks {
    String (*htmlHeader)();
    String (*htmlFooter)();
    void (*scheduleReboot)(uint32_t delayMs);
};

// Dedicated cluster configuration/status surface. The module only registers
// its own routes and deliberately leaves the central WebConfig infrastructure
// in webconfig.cpp untouched apart from one registration hook/menu link.
void webconfigClusterRegisterRoutes(
    WebServer &server,
    const WebConfigClusterUiHooks &uiHooks
);
