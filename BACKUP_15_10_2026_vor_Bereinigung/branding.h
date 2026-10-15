#pragma once

// =============================================================
// SENSORFORGE PRODUCT BRANDING
// =============================================================
// Firmware-owned identity. Intentionally not exposed through config.txt.
// Change release identity here so WebConfig and WebPlayer stay in sync.

#define SENSORFORGE_APP_NAME_LITERAL       "SensorForge"
#define SENSORFORGE_PLATFORM_LITERAL       "Fabric Node"
#define SENSORFORGE_FABRIC_LITERAL         "SensorForge Fabric"
#define SENSORFORGE_CORE_VERSION_LITERAL   "7.1.0"

namespace Branding {

static constexpr const char *APP_NAME =
    SENSORFORGE_APP_NAME_LITERAL;

static constexpr const char *PLATFORM =
    SENSORFORGE_PLATFORM_LITERAL;

static constexpr const char *FABRIC =
    SENSORFORGE_FABRIC_LITERAL;

static constexpr const char *CORE_VERSION =
    SENSORFORGE_CORE_VERSION_LITERAL;

} // namespace Branding

