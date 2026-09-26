#pragma once

#include <Arduino.h>

// Cooperative system-wide SD/storage gate.
// Set to true before operations that temporarily make the filesystem unsafe
// for normal users (format, remount, wipe, future maintenance operations).
// Modules should refuse NEW storage work while this flag is set.
// This is a state gate, not a mutex, and does not forcibly close open handles.
extern volatile bool g_storageLocked;

// Latched when the filesystem/VFS reports a hard physical I/O fault (EIO).
// This is deliberately separate from g_storageLocked: the current recorder is
// allowed to close its own handles before recovery remounts the card.
void storageMarkIoFault();
bool storageIoFaultActive();
void storageClearIoFault();

uint64_t storageFreeBytes();
uint64_t storageReserveBytes();
bool storageHasRequiredFreeSpace();

// "stop": fail below reserve.
// "rollover": delete oldest complete recordings until reserve
// becomes available again.
bool storagePrepareForRecording();

// Removes only SensorForge interrupted temporary media/metadata files:
// *.avi.part, *.mkv.part, *.srt.part, *.jpg.part, *.jpeg.part, *.note.tmp
int storageRecoverIncompleteRecordings();
