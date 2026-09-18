#pragma once

#include <Arduino.h>

// Cooperative system-wide SD/storage gate.
// Set to true before operations that temporarily make the filesystem unsafe
// for normal users (format, remount, wipe, future maintenance operations).
// Modules should refuse NEW storage work while this flag is set.
// This is a state gate, not a mutex, and does not forcibly close open handles.
extern volatile bool g_storageLocked;

uint64_t storageFreeBytes();
uint64_t storageReserveBytes();
bool storageHasRequiredFreeSpace();

// "stop": fail below reserve.
// "rollover": delete oldest complete recordings until reserve
// becomes available again.
bool storagePrepareForRecording();

// Removes only our interrupted temporary recording files:
// *.avi.part, *.mkv.part, *.srt.part
int storageRecoverIncompleteRecordings();
