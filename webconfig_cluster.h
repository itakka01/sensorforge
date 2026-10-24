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
// Sends a large JSON body (100-node lists reach ~100 KB) in 4 KB parts and
// keeps ESP-NOW time exchanges answered in between.
void webconfigClusterSendLargeJson(WebServer &server, const String &json);

// Central WiFi TX power request from form/query args (WebConfig and API):
// dbm, mode=session|persistent|config, scope=all|node|local, node_id,
// include_local=0|1 (scope=all). Fills json, returns the HTTP status.
int clusterTxPowerRequestFromArgs(WebServer &server, String &json);

void webconfigClusterRegisterRoutes(
    WebServer &server,
    const WebConfigClusterUiHooks &uiHooks
);
