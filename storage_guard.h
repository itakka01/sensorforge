#pragma once

#include <Arduino.h>
#include <atomic>

// =============================================================
// COOPERATIVE SD GATES (owner bits)
// =============================================================

// Two cooperative gates: g_storageLocked refuses new SD work, and
// g_recordingStartBlocked (recorder.h) refuses new recordings; a running
// recording is never interrupted. Both are read-only outside this module.
//
// Every subsystem sets and clears only its own owner bit; a gate is closed
// while any bit is set. Overlapping operations therefore never reopen each
// other's gate, unlike the former save/restore of the previous value.
enum SdGateOwner : uint32_t {
    SD_GATE_SD_MAINTENANCE    = 1UL << 0,
    SD_GATE_SECURE_ERASE      = 1UL << 1,
    SD_GATE_SD_BENCHMARK      = 1UL << 2,
    SD_GATE_CLUSTER_WIPE      = 1UL << 3,
    SD_GATE_SD_RECOVERY       = 1UL << 4,
    SD_GATE_SYNC_API_REMOUNT  = 1UL << 5,
    SD_GATE_SYNC_API_SPI_TEST = 1UL << 6,
    SD_GATE_PLAYER_DELETE     = 1UL << 7,
    SD_GATE_RECORDING_LOAD    = 1UL << 8,
    SD_GATE_AUDIO_MIC_TEST    = 1UL << 9,
    SD_GATE_FIRMWARE_UPLOAD   = 1UL << 10,
    SD_GATE_FIRMWARE_ACTIVATE = 1UL << 11,
    SD_GATE_FIRMWARE_OTA      = 1UL << 12,
    SD_GATE_SHUTDOWN          = 1UL << 13,
    SD_GATE_THERMAL           = 1UL << 14,
    // A remount failed: keep both gates closed until reboot.
    SD_GATE_REMOUNT_FAULT     = 1UL << 31,
};

extern std::atomic<bool> g_storageLocked;

// Sets the owner's bit on the selected gates (adds to a bit already held).
void sdGateAcquire(uint32_t owner, bool storage, bool recording);
// Clears the owner's bit on both gates.
void sdGateRelease(uint32_t owner);
// Owner bits currently holding each gate (diagnostics).
uint32_t sdGateStorageOwners();
uint32_t sdGateRecordingOwners();

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
