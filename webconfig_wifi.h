#pragma once

#include <Arduino.h>

class WebServer;

// Dedicated WiFi settings UI and form processing. Web server lifecycle,
// recording gates, logging and redirects remain owned by webconfig.cpp.
String webconfigWifiSettingsHtml(const String &notice);

// Performs an on-demand WiFi scan and returns a compact JSON result.
String webconfigWifiScanJson();

// Parses and validates the dedicated WiFi form and persists only the WiFi
// configuration block. Returns true for a successful internal/SD save.
bool webconfigWifiSaveRequest(
    WebServer &server,
    bool writeToSd,
    String &error
);
