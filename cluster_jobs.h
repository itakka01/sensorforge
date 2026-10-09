#pragma once
#include <stddef.h>
#include <stdint.h>

// Beta 71: bounded RAM-only state bookkeeping, no wire protocol or execution.
// Caller must authenticate coordinator identity/epoch and node replies BEFORE
// invoking this module. No storage, recording or power operation is performed.
namespace ClusterJobs {
static const size_t kMaxJobs = 16;
static const size_t kNodeIdLength = 36; // "sf-" + 32 hex + NUL

enum class Kind : uint8_t { None, Restart, Shutdown, ClearRecordings, FirmwareUpdate, Probe };
enum class State : uint8_t { Empty, Queued, Sent, Accepted, Running, Succeeded, Failed, TimedOut, Cancelled };

struct Entry {
    uint64_t jobId;
    uint32_t coordinatorEpoch;
    uint32_t updatedMs;
    uint32_t createdMs; // monotonic time of coordinator enqueue
    uint32_t deadlineMs;
    Kind kind;
    State state;
    char nodeId[kNodeIdLength];
    uint16_t resultCode; // numeric only; never holds secrets or filenames
};

// Must be called on cluster runtime stop/start or coordinator epoch change.
void reset();
// Coordinator-side register: for future use only once an authenticated wire
// transport and explicit administrator consent are implemented.
bool enqueue(uint64_t jobId, uint32_t coordinatorEpoch, const char *nodeId,
             Kind kind, uint32_t nowMs, uint32_t timeoutMs);
// Never retries a completed job. Duplicate IDs cannot be enqueued again while
// the current runtime is active, even if an old entry is terminal.
bool transition(uint64_t jobId, uint32_t coordinatorEpoch, State next,
                uint32_t nowMs, uint16_t resultCode = 0);
void expire(uint32_t nowMs);
size_t snapshot(Entry *out, size_t capacity);
const Entry *find(uint64_t jobId);
}
