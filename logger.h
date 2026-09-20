#pragma once

#include <Arduino.h>

void logInit();
void logClose();
void logWrite(const String &msg);
void logFlush();
void logBlankLine();

// Clears the active log generation and its .1 rotation backup, then reopens
// the logger and writes one audit line marking the manual reset.
// Returns false and provides a human-readable error if storage is busy or the
// log could not be removed/reopened safely.
bool logClear(String &error);

// Timestamped console output for compact subsystem messages.
void consoleWrite(const char *tag, const String &msg);
void consolePrintf(const char *tag, const char *format, ...);

void debugDump();

// Firmware build / installation metadata.
String firmwareBuildTimestamp();
String firmwareInstallTimestamp();
String firmwareInstallSource();
bool firmwareInstallTimePending();
bool firmwareInfoMarkSdUpdate(const String &sourceFilename);
bool firmwareInfoMarkWifiUpdate(const String &sourceFilename);

// Direct WiFi OTA first-boot guard. A successful direct OTA update arms this
// one-shot NVS flag before switching the boot partition. The next firmware boot
// consumes it before the SD auto-updater runs, preventing a stale SD .bin from
// immediately replacing the just-installed WiFi image.
bool firmwareInfoArmDirectOtaBoot();
void firmwareInfoCancelDirectOtaBoot();
bool firmwareInfoConsumeSkipSdUpdateOnce();

bool firmwareInfoFinalizePendingInstallTime();
void firmwareInfoLogStatus();
