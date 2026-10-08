#pragma once

#include <Arduino.h>

// Stable, public, non-secret integration identity for local integrations.
// Generated once from cryptographic randomness and persisted in NVS.
// Deliberately independent from hostname, MAC address and license hardware ID.
const String &deviceIntegrationId();
