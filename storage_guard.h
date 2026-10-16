#pragma once

#include <Arduino.h>
#include <atomic>

// =============================================================
// REFERENCE-COUNTED STORAGE LOCK (Thread-Safe)
// =============================================================

// Internal reference counter for storage lock depth.
// Do NOT access directly - use StorageLockGuard instead.
extern std::atomic<int> g_storageLockDepth;

// Cooperative system-wide SD/storage gate.
// This is now managed by the reference counter and should be read-only
// for most code. Use StorageLockGuard for thread-safe lock/unlock.
//
// SAFETY: Changed from volatile bool to std::atomic<bool>. The lock state
// is now automatically managed by g_storageLockDepth reference counter.
extern std::atomic<bool> g_storageLocked;

// =============================================================
// RAII STORAGE LOCK GUARD (Preferred API)
// =============================================================

// Thread-safe RAII guard for storage locking.
// Automatically increments reference counter on construction,
// decrements on destruction. Storage is unlocked only when
// depth reaches zero (all guards released).
//
// Usage:
//   {
//       StorageLockGuard lock;
//       // ... storage is locked here ...
//   }  // automatic unlock
//
class StorageLockGuard {
public:
    StorageLockGuard();
    ~StorageLockGuard();

    // Non-copyable, non-movable (RAII pattern)
    StorageLockGuard(const StorageLockGuard&) = delete;
    StorageLockGuard& operator=(const StorageLockGuard&) = delete;
    StorageLockGuard(StorageLockGuard&&) = delete;
    StorageLockGuard& operator=(StorageLockGuard&&) = delete;
};

// Initialize RAII guard mutex - call from setup() before any Guard usage
void initStorageLockGuard();

// Legacy direct access (deprecated - use StorageLockGuard instead)
// Only kept for gradual migration. Will be removed in future version.

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
