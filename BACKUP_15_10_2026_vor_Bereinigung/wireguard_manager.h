#pragma once

#include <Arduino.h>

// Optional SensorForge WireGuard client integration.
// The current reference build deliberately selects no external WireGuard
// backend. Configuration/UI/API scaffolding remains available for a future
// maintained ESP-NETIF-compatible backend; no VPN interface is created until
// such a backend is explicitly selected and qualified.

void wireguardSetup();
void wireguardService(bool allowBlockingStart);
void wireguardStop(const char *reason = nullptr);

bool wireguardBuildAvailable();
bool wireguardBoardCapable();
bool wireguardConfigured();
bool wireguardActive();
bool wireguardRuntimeConfigMatchesSaved();

const char *wireguardBackendName();
String wireguardStatusText();
String wireguardLastError();
String wireguardRuntimeAddress();
