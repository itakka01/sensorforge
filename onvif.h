#pragma once

#include <Arduino.h>

class WebServer;

// Lightweight ONVIF interoperability layer for SensorForge's existing
// JPEG/RTSP streamer. This is intentionally a pragmatic discovery/media
// integration and does not claim ONVIF Profile T conformance.
void onvifRegisterWebRoutes(WebServer &server);

// Discovery is active only when streamer mode, RTSP and onvif_enabled are all
// enabled. Failure of ONVIF discovery must never take the core streamer down.
bool onvifBegin(String &error);
void onvifLoop();
void onvifStop();

bool onvifActive();
String onvifLastError();
uint32_t onvifDiscoveryProbeCount();
uint32_t onvifDiscoveryMatchCount();
String onvifDiscoveryLastRemote();
String onvifEndpointUuid();
String onvifDeviceServiceUrl();
String onvifMediaServiceUrl();
