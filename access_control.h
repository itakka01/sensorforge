#pragma once

#include <Arduino.h>
#include <WebServer.h>

enum SensorForgeAccessRole {
    SENSORFORGE_ACCESS_NONE = 0,
    SENSORFORGE_ACCESS_STREAM = 1,
    SENSORFORGE_ACCESS_ADMIN = 2
};

// When access protection is disabled, web requests are treated as admin so
// legacy open-WebConfig behavior remains unchanged.
SensorForgeAccessRole accessControlWebRole(WebServer &server);

// Validate an HTTP/RTSP Authorization header using Basic authentication.
// Returns ADMIN for the WebConfig administrator and STREAM for one of
// the configured streaming users.
SensorForgeAccessRole accessControlAuthorizationRole(const String &authorizationHeader);

// Streaming users always get live/snapshot/media-view access. Optional
// recording annotation/delete permissions are controlled by config flags.
// Administrator access remains unrestricted.
bool accessControlWebRequestAllowed(
    SensorForgeAccessRole role,
    HTTPMethod method,
    const String &uri
);

bool accessControlAnyStreamingUserConfigured();
const char *accessControlRoleName(SensorForgeAccessRole role);
