#pragma once

#include <Arduino.h>

// User-facing VPN status block embedded in WiFi settings.
// WireGuard runtime configuration remains prepared internally, but until a
// qualified backend exists the Web UI exposes only a concise availability state.
String webconfigWireGuardWifiSectionHtml();
