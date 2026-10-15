#pragma once
#include <Arduino.h>
#include <WebServer.h>

// Configuration only. Never initiates downloads, flashing, or reboot.
extern const char *const WEB_FW_AUTO_DEFAULT_MANIFEST;
bool webFirmwareAutoRead(bool &enabled, String &manifestUrl);
void webFirmwareAutoRegisterRoutes(WebServer &server, uint32_t bootSessionToken);
