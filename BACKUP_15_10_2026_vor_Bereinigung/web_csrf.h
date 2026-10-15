#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "access_control.h"

// Per-boot CSRF protection for browser-originated mutating requests.
// API clients under /api/v1 are intentionally excluded and keep their
// existing administrator authentication contract.
void webCsrfBegin();

bool webCsrfRequestRequiresProtection(
    HTTPMethod method,
    const String &uri
);

bool webCsrfRequestValid(
    WebServer &server,
    SensorForgeAccessRole role
);

// Common WebConfig pages embed this bootstrap. It automatically adds the
// current role's token to same-origin POST forms and fetch() requests without
// changing GET/stream/download traffic.
String webCsrfBrowserBootstrap(
    SensorForgeAccessRole role
);

// The standalone WebPlayer does not use the common HTML header. Its page gets
// the role token via a SameSite cookie and appends it only to mutating requests.
String webCsrfCookieHeader(
    SensorForgeAccessRole role
);
