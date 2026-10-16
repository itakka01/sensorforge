# Beta 131 Implementation Plan - Production ESP-NOW Time Broadcaster

**Date:** 2026-10-16
**Current Version:** v87-beta130
**Target Version:** v87-beta131
**Status:** PLANNING COMPLETE - READY FOR IMPLEMENTATION

---

## 🎯 PROJECT GOAL

Enable production-ready time broadcaster handover to achieve:
- ~100 synchronized nodes
- ≤250 µs relative deviation between nodes
- Relative synchronization priority over absolute UTC accuracy
- Safe, lease-based exclusive transmission
- Automatic fallback on broadcaster failure

---

## 📊 CURRENT IMPLEMENTATION STATE (Beta 106-130)

### ✅ COMPLETED COMPONENTS

#### 1. **Diagnostic Broadcaster (Beta 119)**
- **File:** `cluster_espnow_radio.cpp:1397` - `serviceDiagnosticBroadcaster()`
- **Format:** 80-byte SFDB packets (ClusterTimeDiagnosticBeacon)
- **Rate:** 1 Hz
- **HMAC:** Separate domain (`clusterEspNowDiagnosticBeaconSign/Verify`)
- **Status:** ✅ Fully functional, tested on hardware
- **Limitation:** Diagnostic only, NOT used for production time

#### 2. **Grant/Ack Protocol Wire Format (Beta 125)**
- **File:** `cluster_time_grant_protocol.h`
- **Format:** 96-byte authenticated messages
- **Actions:** Grant, Ack, Revoke
- **Fields:** coordinator/broadcaster identities, epoch, transaction, sequence, lease
- **Replay protection:** Per authenticated coordinator boot
- **Status:** ✅ Complete codec with validation

#### 3. **Grant/Ack HMAC (Beta 126)**
- **File:** `firmware_security.cpp` (external functions)
- **Functions:** `clusterTimeGrantSign()`, `clusterTimeGrantVerify()`
- **Domain:** Independent HMAC-SHA256 subkey
- **Status:** ✅ Crypto verified, secure wiping implemented

#### 4. **Grant/Ack Transport (Beta 127)**
- **File:** `cluster_espnow_radio.cpp:1164` - `serviceGrantDiagnostic()`
- **Coordinator:** Sends Grant every 5s when broadcaster is fresh
- **Broadcaster:** Responds with Ack
- **Queue:** 4-slot bounded queue for 96-byte frames
- **Stats:** Full diagnostic counters exposed
- **Status:** ✅ Transport works, tested on hardware
- **Limitation:** Line 1233 comment: `"purely descriptive; NEVER activated"`

#### 5. **Broadcaster Freshness Detection (Beta 123)**
- **Logic:** 3 consecutive authenticated SFDB beacons = fresh
- **Timeout:** 3.5s without valid packet = stale
- **Fields:** `candidateFresh`, `candidateConsecutive`, `candidateAgeMs`
- **Status:** ✅ Working detection logic

#### 6. **Shadow Handover Simulation (Beta 124)**
- **File:** `cluster_time_shadow_handover.h`
- **Class:** `ClusterTimeShadowHandover::Monitor`
- **Metrics:** `simulatedActive`, activations, fallbacks, source changes
- **Status:** ✅ Passive observation, no production impact
- **Limitation:** Does NOT modify clocks or TX behavior

#### 7. **Enrollment Structure (Beta 129)**
- **File:** `cluster_grant_pair_config.h`
- **Fields:**
  - `SF_GRANT_PAIR_KEY_HEX` - 256-bit shared secret (currently empty)
  - `SF_GRANT_COORDINATOR_ID` - Pinned coordinator identity
  - `SF_GRANT_BROADCASTER_ID` - Pinned broadcaster identity
- **Status:** ✅ Structure defined, NOT enforced

#### 8. **A/B Prediction Comparison (Beta 130)**
- **Endpoint:** `/cluster_espnow_ab_prediction_json`
- **Purpose:** Compare coordinator vs broadcaster prediction residuals
- **Status:** ✅ Diagnostic visibility, no clock changes

---

### ❌ MISSING COMPONENTS (Beta 131 Requirements)

#### A. **Production Broadcaster Beacons**
**Current:** Broadcaster sends 80-byte SFDB diagnostic frames
**Required:** Send 76-byte production `Type::Beacon` frames
**Impact:** HIGH - This is the core functionality

**What needs to change:**
- Broadcaster must send `EspNowTimePackets::Type::Beacon` (not SFDB)
- Must use production HMAC (`clusterEspNowTimeSign()`)
- Must include coordinator-compatible fields (epoch, coordinatorTag, timeUs)
- Nodes must be able to decode and authenticate these beacons

**Files affected:**
- `cluster_espnow_radio.cpp` - `serviceDiagnosticBroadcaster()`
- Need new production beacon send path

#### B. **Broadcaster Authorization & Enrollment**
**Current:** Grant/Ack transport exists but no identity verification
**Required:** Enforce pinned coordinator/broadcaster identities
**Impact:** HIGH - Security critical

**What needs to change:**
- Check `SF_GRANT_PAIR_KEY_HEX` is configured (non-empty)
- Verify Grant sender matches `SF_GRANT_COORDINATOR_ID`
- Verify Grant target matches `SF_GRANT_BROADCASTER_ID`
- Only accept Grant if identities match pinned values
- Fail closed if key is empty

**Code location:**
- `cluster_espnow_radio.cpp:1179` - Currently says "No trusted coordinator identity"
- Need to add identity check before accepting Grant

#### C. **Lease Enforcement on Broadcaster**
**Current:** Broadcaster receives Grant but never activates
**Required:** Track active lease, send production beacons only while valid
**Impact:** HIGH - Core handover mechanism

**What needs to change:**
- Accept Grant with lease duration
- Track lease expiry time (`leaseExpiresMs`)
- Send production beacons ONLY while lease is valid
- Stop sending when lease expires
- Resume SFDB diagnostic mode after expiry

**State needed:**
- `broadcasterLeaseActive` - bool
- `broadcasterLeaseExpiresMs` - uint32_t
- `broadcasterLeaseGrantedAt` - uint32_t

#### D. **Coordinator Beacon Suppression**
**Current:** Coordinator always sends beacons at 10s intervals
**Required:** Suppress coordinator beacons when broadcaster has active lease
**Impact:** HIGH - Prevents dual senders

**What needs to change:**
- Track if broadcaster has active lease
- Skip coordinator beacon transmission if broadcaster is active
- Log reason for suppression
- Resume beacons when lease expires or broadcaster fails

**Code location:**
- `cluster_espnow_radio.cpp:1658` - Coordinator beacon send logic
- Need check: `if (broadcasterHasActiveLease) skip;`

#### E. **Node Source Switching**
**Current:** Nodes only accept beacons from coordinator
**Required:** Accept beacons from EITHER coordinator OR broadcaster
**Impact:** HIGH - Nodes can't sync to broadcaster without this

**What needs to change:**
- Modify beacon acceptance logic in `service()`
- Accept beacon if `coordinatorTag` matches EITHER:
  - Cluster coordinator identity (existing)
  - Authenticated broadcaster identity (new)
- Track which source is currently being used
- Don't mix samples from different sources in one estimation series

**Code location:**
- `cluster_espnow_radio.cpp:1474` - Packet decode and tag check
- Line 1475: `f.coordinatorTag!=tag` - Need to also check broadcaster tag

#### F. **Safe Source Handover**
**Current:** No coordination when switching time sources
**Required:** Clean series boundaries, no mid-series source changes
**Impact:** MEDIUM - Prevents estimation corruption

**What needs to change:**
- When source changes detected, finish current estimation series
- Clear pending samples
- Reset drift estimation
- Start fresh series with new source
- Don't accept samples if source is changing

**Code location:**
- `cluster_espnow_radio.cpp:873` - `resetTiming()`
- Need source change detection before accepting samples

#### G. **Capture Clock Protection**
**Current:** No specific protection during handover
**Required:** Maintain monotonic behavior, no unwanted jumps
**Impact:** MEDIUM - Preserve scheduled captures

**What needs to change:**
- During handover, hold last accepted estimate
- Don't update capture clock until new source is stable (3+ samples)
- Log source change but keep clock stable
- Use existing `pauseForCapture()` mechanism during transition

**Mechanism:**
- When source changes: keep returning old UTC with increasing age
- After 3 good samples from new source: resume normal updates

#### H. **Fallback on Broadcaster Failure**
**Current:** Shadow simulation tracks this but doesn't act
**Required:** Coordinator automatically resumes if broadcaster fails
**Impact:** HIGH - System reliability

**What needs to change:**
- Coordinator tracks last broadcaster beacon time
- If no broadcaster beacon for 3.5s AND lease expired: resume sending
- Nodes automatically fall back to coordinator beacons
- Log fallback event

**State needed:**
- `lastBroadcasterBeaconMs` - uint32_t
- Check in coordinator beacon send logic

---

## 🔧 IMPLEMENTATION PLAN

### Phase 1: Enrollment & Authorization (Files: 1, Lines: ~50)

**Goal:** Enable secure broadcaster authorization

**Changes:**

1. **cluster_espnow_radio.cpp** - Modify `serviceGrantDiagnostic()` at line 1179:

```cpp
// BEFORE (beta130):
if(diagnosticMode) {
    // No trusted coordinator identity -> never accept a lease.
    // Only echo a fresh authenticated diagnostic Grant as an ACK.

// AFTER (beta131):
if(diagnosticMode) {
    // Beta131: Check enrollment before accepting Grant
    const bool enrollmentValid = (
        strlen(SF_GRANT_PAIR_KEY_HEX) == 64 &&  // Key configured
        m.coordinatorTag == idTag(SF_GRANT_COORDINATOR_ID) &&  // Coordinator enrolled
        m.broadcasterTag == idTag(SF_GRANT_BROADCASTER_ID)     // Broadcaster enrolled
    );

    if (!enrollmentValid) {
        ++grantDiag.rejected;
        continue;  // Fail closed if enrollment not configured
    }

    // Now accept and process the Grant...
```

**Deliverable:** Broadcaster only accepts Grants from enrolled coordinator

---

### Phase 2: Production Broadcaster Beacons (Files: 1, Lines: ~100)

**Goal:** Send 76-byte production beacons instead of 80-byte diagnostic

**Changes:**

1. **cluster_espnow_radio.cpp** - Modify `serviceDiagnosticBroadcaster()` at line 1397:

```cpp
// After Grant acceptance, if lease is active:
if (broadcasterLeaseActive && millis() < broadcasterLeaseExpiresMs) {
    // Send PRODUCTION beacon (76 bytes)
    if (!nextBroadcastMs) nextBroadcastMs = millis() + 100;
    if (static_cast<int32_t>(millis() - nextBroadcastMs) >= 0) {
        nextBroadcastMs = millis() + 100;  // 10 Hz production beacons

        EspNowTimePackets::Frame beacon{};
        beacon.type = EspNowTimePackets::Type::Beacon;
        beacon.epoch = epoch;
        beacon.sequence = ++productionSequence;
        beacon.coordinatorTag = tag;  // Broadcaster identity as coordinator
        beacon.senderBoot = localBootNonce;
        beacon.timeUs = esp_timer_get_time();

        uint8_t packet[EspNowTimePackets::kPacketSize];
        if (EspNowTimePackets::encode(beacon, packet,
                clusterEspNowTimeSign, nullptr)) {
            esp_now_send(kBroadcastMac, packet, sizeof(packet));
            ++productionBeaconsSent;
        }
    }
} else {
    // Send DIAGNOSTIC beacon (80 bytes) - existing code
    // ... (keep existing SFDB logic)
}
```

**State variables needed:**
```cpp
static bool broadcasterLeaseActive = false;
static uint32_t broadcasterLeaseExpiresMs = 0;
static uint32_t productionSequence = 0;
static uint32_t productionBeaconsSent = 0;
```

**Deliverable:** Broadcaster sends production beacons when lease is active

---

### Phase 3: Lease Acceptance & Expiry (Files: 1, Lines: ~80)

**Goal:** Implement lease lifecycle on broadcaster

**Changes:**

1. **cluster_espnow_radio.cpp** - Modify Grant acceptance logic at line 1199:

```cpp
// After successful Grant authentication:
if (enrollmentValid && m.action == ClusterTimeGrantProtocol::Action::Grant) {
    // Activate production beacon lease
    const uint32_t now = millis();
    broadcasterLeaseActive = true;
    broadcasterLeaseExpiresMs = now + m.leaseMs;
    broadcasterLeaseGrantedAt = now;

    // Respond with Ack
    m.action = ClusterTimeGrantProtocol::Action::Ack;
    // ... (existing Ack send logic)

    // Log lease activation
    Serial.printf("[BROADCASTER] Production lease GRANTED: %u ms\n", m.leaseMs);
}
```

2. **Add lease expiry check** in `serviceDiagnosticBroadcaster()`:

```cpp
// Check lease expiry
if (broadcasterLeaseActive && millis() >= broadcasterLeaseExpiresMs) {
    broadcasterLeaseActive = false;
    Serial.println("[BROADCASTER] Production lease EXPIRED - resuming diagnostic mode");
}
```

**Deliverable:** Broadcaster activates/deactivates based on lease

---

### Phase 4: Coordinator Suppression (Files: 1, Lines: ~40)

**Goal:** Prevent coordinator from sending beacons when broadcaster is active

**Changes:**

1. **cluster_espnow_radio.cpp** - Modify coordinator beacon send at line 1658:

```cpp
// BEFORE:
if (static_cast<int32_t>(now - nextBroadcastMs) >= 0) {
    nextBroadcastMs = now + kBroadcastEveryMs;
    // ... send beacon

// AFTER:
const bool broadcasterActive = (
    observedBroadcasterAt &&
    diagnosticConsecutive >= 3 &&
    uint32_t(now - observedBroadcasterAt) <= 3500U
);

if (static_cast<int32_t>(now - nextBroadcastMs) >= 0) {
    nextBroadcastMs = now + kBroadcastEveryMs;

    if (broadcasterActive) {
        // Suppress coordinator beacons while broadcaster is active
        ++coordinatorBeaconsSuppressed;
        return;  // Don't send
    }

    // ... send beacon as normal
```

**State variable:**
```cpp
static uint32_t coordinatorBeaconsSuppressed = 0;
```

**Deliverable:** Coordinator stops sending when broadcaster is active

---

### Phase 5: Node Source Switching (Files: 1, Lines: ~120)

**Goal:** Nodes accept beacons from coordinator OR broadcaster

**Changes:**

1. **cluster_espnow_radio.cpp** - Modify beacon acceptance at line 1474:

```cpp
// BEFORE:
if (!EspNowTimePackets::decode(...) ||
    f.coordinatorTag != tag ||  // Only accept coordinator
    f.epoch != epoch) {
    ++stats.rejected;
    continue;
}

// AFTER:
if (!EspNowTimePackets::decode(...) || f.epoch != epoch) {
    ++stats.rejected;
    continue;
}

// Accept beacons from EITHER coordinator or enrolled broadcaster
const uint64_t broadcasterTag = idTag(SF_GRANT_BROADCASTER_ID);
const bool fromCoordinator = (f.coordinatorTag == tag);
const bool fromBroadcaster = (
    strlen(SF_GRANT_PAIR_KEY_HEX) == 64 &&
    f.coordinatorTag == broadcasterTag
);

if (!fromCoordinator && !fromBroadcaster) {
    ++stats.rejected;
    continue;
}

// Track source change
const uint64_t currentSource = fromBroadcaster ? broadcasterTag : tag;
if (lastAcceptedSource && lastAcceptedSource != currentSource) {
    // Source changed - reset estimation
    resetTiming();
    lastAcceptedSource = currentSource;
    ++sourceChanges;
    Serial.printf("[NODE] Time source changed: %s -> %s\n",
        lastAcceptedSource == tag ? "coordinator" : "broadcaster",
        currentSource == tag ? "coordinator" : "broadcaster");
}
lastAcceptedSource = currentSource;
```

**State variables:**
```cpp
static uint64_t lastAcceptedSource = 0;
static uint32_t sourceChanges = 0;
```

**Deliverable:** Nodes sync to either coordinator or broadcaster

---

### Phase 6: Capture Clock Safety (Files: 1, Lines: ~60)

**Goal:** Protect against time jumps during source changes

**Implementation:**

Use existing `precisionUtcNow()` holdover mechanism:

```cpp
// In precisionUtcNow() at line 1095:
// When source changes detected:
if (sourceChangePending) {
    // Return last accepted estimate with increasing age
    // Don't update until new source is stable (3+ samples)
    if (seriesCount < 3) {
        return false;  // Use WiFi fallback during transition
    }
    // After 3 good samples, clear flag and resume
    sourceChangePending = false;
}
```

**Deliverable:** Smooth transitions, no time jumps

---

### Phase 7: Diagnostics & JSON (Files: 2, Lines: ~100)

**Goal:** Expose handover state in WebUI

**Changes:**

1. **cluster_espnow_radio.h** - Add to Diagnostics struct:

```cpp
struct Diagnostics {
    // ... existing fields ...

    // Beta131: Handover state
    bool broadcasterLeaseActive = false;
    uint32_t broadcasterLeaseRemainingMs = 0;
    uint64_t activeTimeSource = 0;  // 0=none, tag=coordinator/broadcaster
    uint32_t sourceChanges = 0;
    uint32_t coordinatorBeaconsSuppressed = 0;
    uint32_t productionBeaconsSent = 0;  // broadcaster only
};
```

2. **webconfig_cluster.cpp** - Update JSON endpoint:

```json
{
  "handover_enabled": true,
  "broadcaster_lease_active": false,
  "broadcaster_lease_remaining_ms": 0,
  "active_time_source": "coordinator",
  "source_changes": 0,
  "coordinator_beacons_suppressed": 0
}
```

**Deliverable:** Full visibility into handover state

---

## 📋 TESTING PLAN

### Test 1: Enrollment Validation
**Setup:** Empty `SF_GRANT_PAIR_KEY_HEX`
**Expected:** Broadcaster rejects all Grants, logs "enrollment not configured"

### Test 2: Grant/Ack with Enrollment
**Setup:** Configure shared key on coordinator + broadcaster
**Expected:** Coordinator sends Grant → Broadcaster responds with Ack

### Test 3: Production Beacon Activation
**Setup:** Broadcaster receives valid Grant
**Expected:**
- Broadcaster starts sending 76-byte production beacons at 10 Hz
- Nodes receive and authenticate beacons
- Coordinator suppresses own beacons

### Test 4: Lease Expiry
**Setup:** Grant with 3s lease
**Expected:**
- Broadcaster sends production beacons for 3s
- After 3s, broadcaster resumes diagnostic mode
- Coordinator resumes sending beacons

### Test 5: Node Source Switching
**Setup:** Node synced to coordinator, broadcaster activates
**Expected:**
- Node accepts broadcaster beacons
- Node resets estimation series
- Node reports source change in logs

### Test 6: Fallback on Broadcaster Failure
**Setup:** Broadcaster active, then powered off
**Expected:**
- After 3.5s, coordinator resumes beacons
- Nodes fall back to coordinator
- No time jumps on nodes

### Test 7: Capture During Handover
**Setup:** Scheduled capture during source change
**Expected:**
- Capture executes on time
- No unwanted time jumps
- Capture clock remains stable

### Test 8: GPIO Cable Test (Final Validation)
**Setup:** GPIO pulse capture on 2+ nodes
**Expected:** Relative deviation ≤ 250 µs between nodes

---

## 🎯 SUCCESS CRITERIA

1. ✅ Arduino IDE compilation succeeds
2. ✅ Broadcaster accepts Grant from enrolled coordinator
3. ✅ Broadcaster sends production beacons with active lease
4. ✅ Coordinator suppresses beacons when broadcaster is active
5. ✅ Nodes sync to broadcaster beacons
6. ✅ Automatic fallback when broadcaster fails
7. ✅ No time jumps during source changes
8. ✅ Capture clock remains stable during handover
9. ✅ GPIO cable test shows ≤ 250 µs relative deviation
10. ✅ All diagnostics expose handover state correctly

---

## 📦 DELIVERABLE STRUCTURE

```
v87-beta131.zip
├── CHANGELOG.md (updated with beta131 entry)
├── sensorforge_version.h (v87-beta131)
├── cluster_espnow_radio.cpp (core changes)
├── cluster_espnow_radio.h (diagnostics update)
├── webconfig_cluster.cpp (JSON endpoints)
└── BETA131_IMPLEMENTATION_PLAN.md (this file)
```

---

## ⚠️ CONSTRAINTS & RISKS

### Flash: 95% Full (3.18 MB / 3.34 MB)
- **Mitigation:** Reuse existing code paths, no large new buffers
- **Estimate:** +500-800 bytes for new state and logic

### Heap: 10-15 KB Free
- **Mitigation:** All state in static globals, no dynamic allocation
- **Estimate:** +100-200 bytes static data

### Compilation
- **Risk:** Arduino IDE build may fail
- **Mitigation:** Incremental testing after each phase

### Hardware Validation
- **Risk:** RF behavior may differ from simulation
- **Mitigation:** Extensive logging, conservative timeouts

---

## 🚀 NEXT STEPS

1. ✅ Complete this analysis (DONE)
2. → Implement Phase 1 (Enrollment)
3. → Implement Phase 2 (Production beacons)
4. → Implement Phase 3 (Lease logic)
5. → Implement Phase 4 (Coordinator suppression)
6. → Implement Phase 5 (Node switching)
7. → Implement Phase 6 (Capture safety)
8. → Implement Phase 7 (Diagnostics)
9. → Test Arduino compilation
10. → Create deliverable ZIP

---

**Status:** READY FOR IMPLEMENTATION
