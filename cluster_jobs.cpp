#include "cluster_jobs.h"
#include <string.h>

namespace ClusterJobs {
namespace {
Entry entries[kMaxJobs] = {};
size_t count = 0;

bool validNode(const char *node) {
    if (!node || strlen(node) != kNodeIdLength - 1 ||
        node[0] != 's' || node[1] != 'f' || node[2] != '-') return false;
    for (size_t i = 3; i < kNodeIdLength - 1; ++i) {
        if (!((node[i] >= '0' && node[i] <= '9') ||
              (node[i] >= 'a' && node[i] <= 'f'))) return false;
    }
    return true;
}
bool terminal(State s) {
    return s == State::Succeeded || s == State::Failed ||
           s == State::TimedOut || s == State::Cancelled;
}
bool allowed(State from, State to) {
    if (terminal(from) || from == State::Empty || from == to) return false;
    if (to == State::Failed || to == State::Cancelled || to == State::TimedOut) return true;
    switch (from) {
        case State::Queued: return to == State::Sent;
        case State::Sent: return to == State::Accepted;
        case State::Accepted: return to == State::Running;
        case State::Running: return to == State::Succeeded;
        default: return false;
    }
}
} // namespace
void reset() { memset(entries, 0, sizeof(entries)); count = 0; }
const Entry *find(uint64_t id) {
    if (!id) return nullptr;
    for (size_t i = 0; i < count; ++i) if (entries[i].jobId == id) return &entries[i];
    return nullptr;
}
bool enqueue(uint64_t id, uint32_t epoch, const char *node, Kind kind,
             uint32_t now, uint32_t timeout) {
    if (!id || !epoch || !validNode(node) || kind == Kind::None ||
        count == kMaxJobs || timeout < 1000 || timeout > 3600000 || find(id)) return false;
    Entry &e = entries[count++];
    e = {};
    e.jobId = id; e.coordinatorEpoch = epoch; e.updatedMs = now; e.createdMs = now;
    e.deadlineMs = now + timeout; e.kind = kind; e.state = State::Queued;
    memcpy(e.nodeId, node, kNodeIdLength);
    return true;
}
bool transition(uint64_t id, uint32_t epoch, State next, uint32_t now, uint16_t result) {
    for (size_t i = 0; i < count; ++i) {
        Entry &e = entries[i];
        if (e.jobId != id || e.coordinatorEpoch != epoch ||
            !allowed(e.state, next)) continue;
        if (static_cast<int32_t>(now - e.deadlineMs) >= 0 && next != State::TimedOut) {
            e.state = State::TimedOut; e.updatedMs = now; return false;
        }
        e.state = next; e.updatedMs = now; e.resultCode = result;
        return true;
    }
    return false;
}
bool setCaptureMedia(uint64_t id,uint32_t epoch,const char *uuid,int64_t dispatch,uint32_t bytes){
    if(!uuid||strlen(uuid)!=36||dispatch<=0||!bytes)return false;
    for(size_t i=0;i<36;i++){
        const char c=uuid[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    }
    for(size_t i=0;i<count;i++){
        Entry &e=entries[i];
        if(e.jobId==id&&e.coordinatorEpoch==epoch&&e.kind==Kind::Capture&&
           (e.state==State::Sent||e.state==State::Accepted)&&!e.mediaUuid[0]){
            memcpy(e.mediaUuid,uuid,37);e.dispatchUtcUs=dispatch;e.mediaBytes=bytes;
            return true;
        }
    }
    return false;
}
void expire(uint32_t now) {
    for (size_t i = 0; i < count; ++i) {
        Entry &e = entries[i];
        // A restart ACK proves scheduling; its Accepted state must never
        // be overwritten by the transport delivery timeout.
        if ((e.kind == Kind::Restart || e.kind == Kind::Shutdown) &&
            e.state == State::Accepted) continue;
        if (!terminal(e.state) && static_cast<int32_t>(now - e.deadlineMs) >= 0) {
            e.state = State::TimedOut; e.updatedMs = now;
        }
    }
}
size_t snapshot(Entry *out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = count < cap ? count : cap;
    memcpy(out, entries, n * sizeof(Entry));
    return n;
}
} // namespace ClusterJobs
