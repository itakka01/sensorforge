#pragma once

#include <WebServer.h>

// Register the stateless SensorForge Sync API v1 on the existing WebConfig
// server. Registration has no background runtime cost; work happens only when
// an /api/v1/... request is received.
void syncApiRegisterRoutes(WebServer &server);

// True while an authenticated API client holds the temporary exclusive lease.
// On SPI-SD boards the lease also owns the temporary board-approved high-speed
// SD clock. Expiry/release safely remounts the card at the board NORMAL clock
// before recording is allowed again. A remount failure remains fail-safe and
// requires a reboot instead of allowing recording against an invalid mount.
bool syncApiExclusiveActive();
