# V2 Protocol Deployment Guide

## Status: READY FOR DEPLOYMENT

**Firmware Version:** v87-beta132
**Protocol Version:** V2 (Software Timestamp Correction)
**Date:** 2026-10-18

---

## CRITICAL: Version Compatibility

**V1 ↔ V2 INCOMPATIBLE**

The V2 protocol is **NOT compatible** with V1:
- V2 packets: kVersion=2, kPacketSize=80 bytes (48+32)
- V1 packets: kVersion=1, kPacketSize=76 bytes (44+32)
- decode() strictly validates version: `if(b[4]!=kVersion) return false;`

**Consequence:** Mixed V1/V2 devices will **reject all packets** from each other.

**Deployment Requirement:** All 4 devices MUST be updated to V2 simultaneously.

---

## Device Inventory

| Device | Role | MAC | Current FW | Target FW |
|--------|------|-----|------------|-----------|
| devfreenove1 | Coordinator | (TBD) | v87-beta131 | v87-beta132 |
| devxiao3 | Broadcaster | (TBD) | v87-beta131 | v87-beta132 |
| devxiao1 | Node | (TBD) | v87-beta131 | v87-beta132 |
| devxiao2 | Node | (TBD) | v87-beta131 | v87-beta132 |

---

## Safe Flash Order

**RECOMMENDED ORDER: Sequential with Minimal Downtime**

### Phase 1: Prepare All Devices (No Downtime)
1. Build firmware v87-beta132 for all devices
2. Verify build output (no compilation errors)
3. Connect all 4 devices to programming workstation
4. Keep current network operational

### Phase 2: Flash Sequence (Minimize Window of Incompatibility)

**Total downtime window: ~2-3 minutes**

#### Step 1: Flash Coordinator (devfreenove1)
```bash
# Flash devfreenove1 with v87-beta132
# Expected: Device boots, no active broadcaster yet
# Serial log: "No valid beacons received" (expected until broadcaster updates)
```
**Duration:** ~30 seconds

#### Step 2: Flash Broadcaster (devxiao3) **IMMEDIATELY**
```bash
# Flash devxiao3 with v87-beta132
# Expected: Receives time base from coordinator (V2 protocol)
# Serial log: "[BROADCASTER] Production ACTIVATED"
```
**Duration:** ~30 seconds
**Network Status:** Coordinator + Broadcaster operational (V2)

#### Step 3: Flash Node 1 (devxiao1)
```bash
# Flash devxiao1 with v87-beta132
# Expected: Syncs with broadcaster, starts V2 exchanges
# Serial log: Valid exchanges, offset convergence
```
**Duration:** ~30 seconds

#### Step 4: Flash Node 2 (devxiao2)
```bash
# Flash devxiao2 with v87-beta132
# Expected: Syncs with broadcaster, starts V2 exchanges
# Serial log: Valid exchanges, offset convergence
```
**Duration:** ~30 seconds

### Phase 3: Verification (Post-Flash)

**Monitor Serial Output for ~5 minutes per device:**

#### Coordinator (devfreenove1):
```
[EXPECTED LOG ENTRIES]
✓ coordinatorGrantedLeaseActive: true
✓ Responding to broadcaster Probes
✓ Sending beacons with kVersion=2
✓ HMAC verification succeeding
```

#### Broadcaster (devxiao3):
```
[EXPECTED LOG ENTRIES]
✓ broadcasterLeaseActive: true
✓ broadcasterTimeBaseValid: true
✓ Responding to node Probes
✓ preSendDelayUs values: 600-850 µs (realistic range)
✓ last_series_spread_us: <30 µs (target: 15-30 µs)
✓ broadcaster_precision_failed: <5
```

#### Nodes (devxiao1, devxiao2):
```
[EXPECTED LOG ENTRIES]
✓ Valid exchanges with broadcaster
✓ offsetUs median: ±25 µs (acceptable with driver asymmetry)
✓ last_series_spread_us: <30 µs
✓ latestQualityEstimate.valid: true
✓ Probe packets carrying preSendDelayUs from previous Probe
✓ Final packets carrying preSendDelayUs from Reply
```

---

## Rollback Plan

**If V2 deployment fails:**

### Symptoms of Failure:
- broadcaster_precision_failed constantly incrementing
- last_series_spread_us > 250 µs consistently
- No valid exchanges (stats.exchangesComplete not incrementing)
- HMAC authentication failures

### Rollback Procedure:
1. Flash all 4 devices back to v87-beta131 (V1)
2. Same flash order: Coordinator → Broadcaster → Nodes
3. Verify beta131 functionality restored
4. Investigate V2 issue before retry

**Rollback Time:** ~2-3 minutes (same as deployment)

---

## Expected V2 Performance

Based on simulation (test_software_correction.cpp.test):

| Metric | V1 (beta131) | V2 (beta132) | Improvement |
|--------|--------------|--------------|-------------|
| **Spread** | 62.5 µs | **15.0 µs** | **-76%** |
| **Stddev** | 15.91 µs | **4.99 µs** | **-69%** |
| **Bias** | -2.50 µs | -2.81 µs | ~0% (maintained) |
| **GPIO Accuracy** | <250 µs ✓ | <250 µs ✓ | Maintained |

**Real Hardware (Expected):**
- Spread: 325 µs → 15-30 µs (91-95% reduction)
- broadcaster_precision_failed: ~30-50 → <5 (only real RF issues)
- Broadcaster lease stability: Improved (fewer precision failures)

---

## Deployment Checklist

### Pre-Deployment
- [ ] Build v87-beta132 for all devices successful
- [ ] All 4 devices physically accessible
- [ ] Serial monitors ready for all devices
- [ ] Rollback firmware (v87-beta131) available
- [ ] Test results reviewed (12/12 V2 tests, 6/6 regression tests)

### During Deployment
- [ ] Flash devfreenove1 (Coordinator)
- [ ] Verify coordinator boot, check serial log
- [ ] Flash devxiao3 (Broadcaster) **immediately**
- [ ] Verify broadcaster activates, receives time base
- [ ] Flash devxiao1 (Node 1)
- [ ] Verify node 1 syncs, valid exchanges
- [ ] Flash devxiao2 (Node 2)
- [ ] Verify node 2 syncs, valid exchanges

### Post-Deployment (5-10 minute observation)
- [ ] All devices show valid V2 exchanges
- [ ] Spread <30 µs on broadcaster
- [ ] Offset median <±50 µs on nodes
- [ ] broadcaster_precision_failed <5
- [ ] No HMAC authentication failures
- [ ] Broadcaster lease remains active

---

## Technical Notes

### Why Simultaneous Flash Required?

V2 decode() strictly validates packet version:
```cpp
// cluster_espnow_packets.cpp:23
if(b[4]!=kVersion) return false;
```

**V1 device receives V2 packet:** Rejects (b[4]=2, expects 1)
**V2 device receives V1 packet:** Rejects (b[4]=1, expects 2)

No fallback logic exists (by design, for security).

### Flash Order Rationale

**Coordinator First:**
- Coordinator is time source for broadcaster
- Broadcaster needs coordinator operational to activate

**Broadcaster Second:**
- Broadcaster is time source for nodes
- Nodes need broadcaster operational to sync

**Nodes Last:**
- Nodes are consumers only
- Can be updated individually without affecting network

**Minimize Window:**
- Flash coordinator and broadcaster back-to-back
- Reduces period where coordinator has no valid broadcaster

---

## Contact & Support

If deployment encounters issues:
1. Capture full serial logs from all devices
2. Document symptoms (spread, precision failures, exchange counts)
3. Check HMAC authentication (no "rejected" spam in logs)
4. Consider rollback if network non-functional after 10 minutes

**Expected Success Indicators:**
- All devices show regular V2 exchanges within 2 minutes
- Spread drops to <30 µs within 5 minutes
- Broadcaster lease remains stable for 10+ minutes

---

## Deployment Sign-Off

**Performed by:** __________________
**Date:** __________________
**Start time:** __________________
**Completion time:** __________________
**Result:** [ ] Success [ ] Rollback Required
**Notes:**

---
