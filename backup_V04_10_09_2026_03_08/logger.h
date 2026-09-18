#pragma once

#include <Arduino.h>

void logInit();
void logClose();
void logWrite(const String &msg);
void logFlush();
void logBlankLine();

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
bool firmwareInfoFinalizePendingInstallTime();
void firmwareInfoLogStatus();
