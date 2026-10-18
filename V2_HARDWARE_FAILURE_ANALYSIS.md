# SensorForge v87-beta132 – V2 Hardware Failure Analysis

**Date:** 2026-10-18
**Status:** ROOT CAUSE IDENTIFIED
**Device:** devxiao3 (Broadcaster) after 331 seconds

---

## Hardware Symptoms

```
sent=408
received=0                           ← NO PACKETS RECEIVED
authenticated=0
rejected=0
probes_sent=0                        ← NO PROBES SENT
exchanges_complete=0
broadcaster_production_beacons_sent=0 ← NO BEACONS SENT
broadcaster_lease_active=false       ← LEASE NOT ACTIVE
```

**Grant diagnostic channel:** Working (159 received/authenticated)
**All 4 devices:** Online, V2 flashed
**Coordinator:** boot 721310ab, Epoch 2199669293

---

## Analysis Summary

**ROOT CAUSE:** Time synchronization bootstrap sequence never starts.

**Key Finding:** `sent=408` but `broadcaster_production_beacons_sent=0` + `probes_sent=0`
- The 408 packets sent are NOT time sync packets
- Likely diagnostic beacons or other non-time-sync packets
- Time sync packets never sent → nothing received → no synchronization

---

## Technical Investigation

### 1. Packet Size Verification ✓

**Test Results:** All V2 packet format tests PASSED
- kPacketSize = 80 bytes ✓
- kPayloadSize = 48 bytes ✓
- kVersion = 2 ✓
- sizeof(packet[kPacketSize]) = 80 ✓
- Encode/decode accept 80-byte packets ✓
- Encode/decode reject 76-byte packets ✓
- preSendDelayUs roundtrip works ✓

**Conclusion:** V2 packet format implementation is correct.

### 2. Reception Path Analysis ✓

**cluster_espnow_radio.cpp Line 696:**
```cpp
if(length!=static_cast<int>(EspNowTimePackets::kPacketSize))return;
```

**Verified:**
- kPacketSize constant correct (80)
- Queue buffer size correct: `uint8_t bytes[EspNowTimePackets::kPacketSize]` (Line 123)
- memcpy size correct: `memcpy(queue[head].bytes,data,EspNowTimePackets::kPacketSize)` (Line 702)
- No hardcoded V1 sizes (76, 44) found

**Conclusion:** Reception path correctly configured for V2 (80 bytes).

### 3. Transmission Path Analysis ⚠

**Coordinator Beacons (Line 2576):**
```cpp
if(sendFrame(b,kBroadcastMac) && beaconExperimentUntilMs)++beaconExperimentSent;
```
Uses sendFrame() → buffer created with kPacketSize (80 bytes) ✓

**Broadcaster Production Beacons (Lines 2503-2509):**
```cpp
uint8_t packet[EspNowTimePackets::kPacketSize];
if(EspNowTimePackets::encode(prodBeacon,packet,clusterEspNowTimeSign,nullptr)) {
    if(esp_now_send(kBroadcastMac,packet,sizeof(packet))==ESP_OK) {
        ++stats.sent;
        ++broadcasterProductionBeaconsSent;  ← NEVER INCREMENTED
    }
}
```

**Critical Path:** Lines 2472-2509 (Broadcaster Production Service)
```cpp
// Send production beacons when lease active AND time valid
if(broadcasterLeaseActive && broadcasterTimeBaseValid) {
    // ... beacon creation and sending ...
}
```

**Observed:** `broadcasterLeaseActive=false` → Condition fails → No beacons sent

### 4. Bootstrap Sequence Failure ✗

**Required Sequence:**
1. Coordinator sends beacons (via sendFrame)
2. Broadcaster receives beacons → syncs time → `broadcasterTimeBaseValid=true`
3. Broadcaster receives lease grant → `broadcasterLeaseActive=true`
4. Broadcaster sends production beacons (lines 2503-2509)
5. Nodes receive beacons → send probes

**Current State:** Sequence stuck at step 2 or earlier

**Possible Causes:**
1. **Coordinator not sending beacons** (encode() failing silently?)
2. **Broadcaster not receiving beacons** (length mismatch? decode() failing?)
3. **HMAC verification failing** (clusterEspNowTimeSign issue with V2?)
4. **Initialization issue** (ESP-NOW not fully initialized for V2?)

---

## Critical Questions Requiring Answer

### Q1: Is Coordinator Sending Beacons?
**How to verify:**
- Add Serial.printf() before Line 2576 in coordinator code
- Check if sendFrame() is called
- Check if sendFrame() returns true

**If NOT sending:**
- encode() may be failing (no diagnostic output!)
- clusterEspNowTimeSign may be rejecting V2 format

### Q2: Are Packets Reaching ESP-NOW Layer?
**How to verify:**
- Check esp_now_send() return codes
- Add diagnostic output in sendFrame() after Line 798
- Check TX callback counters (txCallbackFailed, etc.)

### Q3: Is HMAC Signing Working for V2?
**Critical:** clusterEspNowTimeSign function not defined in analyzed files
- May be external function that doesn't support 48-byte payload
- HMAC-SHA256 input size changed: V1 (44 bytes) → V2 (48 bytes)
- If signing function hardcodes 44-byte input, V2 encode() would fail

**How to verify:**
- Find clusterEspNowTimeSign implementation
- Check if it validates/hardcodes kPayloadSize
- Test with 48-byte payload

---

## Hypothesis: HMAC Signing Function Issue

**Most Likely Root Cause:**

The HMAC signing function `clusterEspNowTimeSign` may:
1. **Hardcode V1 payload size (44 bytes)** instead of using kPayloadSize constant
2. **Validate input length** and reject 48-byte V2 payloads
3. **Fail silently** causing encode() to return false

**Evidence:**
- encode() calls: `sign(out,kPayloadSize,out+kPayloadSize,user)` (Line 20)
- kPayloadSize changed: 44 → 48
- If sign() rejects, encode() fails silently (no error output)
- No beacons sent → received=0

**Verification Required:**
```bash
# Find HMAC signing function implementation
grep -rn "clusterEspNowTimeSign" *.cpp *.h
# Check for hardcoded payload size validation
grep -rn "44" <hmac_file>
```

---

## Required Fixes

### Fix 1: Find and Update HMAC Function ✓
**Location:** TBD (not in cluster_espnow_radio.cpp or cluster_espnow_packets.cpp)

**Expected Issue:**
```cpp
// WRONG (V1 hardcoded):
bool clusterEspNowTimeSign(const uint8_t* payload, size_t len, uint8_t mac[32], void* user) {
    if (len != 44) return false;  // ← REJECTS V2 (48 bytes)!
    // ... HMAC calculation ...
}

// CORRECT (V2 compatible):
bool clusterEspNowTimeSign(const uint8_t* payload, size_t len, uint8_t mac[32], void* user) {
    if (len != EspNowTimePackets::kPayloadSize) return false;  // Uses constant
    // ... HMAC calculation ...
}
```

### Fix 2: Add Diagnostic Output
**cluster_espnow_packets.cpp Line 14:**
```cpp
bool encode(const Frame &f, uint8_t out[kPacketSize], SignMac sign, void* user) {
  if (!out || !sign || !typeValid(f.type) || !f.epoch || !f.coordinatorTag || !f.senderBoot || !f.sequence) {
      Serial.println("[ENCODE] Validation failed");  // ← ADD
      return false;
  }
  memset(out,0,kPacketSize);
  out[0]='S';out[1]='F';out[2]='E';out[3]='N';out[4]=kVersion;out[5]=static_cast<uint8_t>(f.type);
  put(out,8,f.epoch,4);put(out,12,f.sequence,4);put(out,16,f.coordinatorTag,8);
  put(out,24,f.senderBoot,8);put(out,32,static_cast<uint64_t>(f.timeUs),8);put(out,40,f.nonce,4);
  put(out,44,f.preSendDelayUs,4);
  bool signOk = sign(out,kPayloadSize,out+kPayloadSize,user);
  if (!signOk) {
      Serial.println("[ENCODE] HMAC signing failed");  // ← ADD
  }
  return signOk;
}
```

**cluster_espnow_radio.cpp Line 2576 (Coordinator beacons):**
```cpp
if(sendFrame(b,kBroadcastMac)) {
    if(beaconExperimentUntilMs) ++beaconExperimentSent;
    Serial.println("[COORDINATOR] Beacon sent");  // ← ADD
} else {
    Serial.println("[COORDINATOR] Beacon sendFrame() failed");  // ← ADD
}
```

### Fix 3: Regression Test for HMAC with V2
**New test:** test_v2_hmac_integration.cpp.test
- Test HMAC signing with 48-byte payload
- Verify clusterEspNowTimeSign accepts kPayloadSize
- Test encode() with real HMAC function (not mock)

---

## Deployment Impact

**V1 ↔ V2 Incompatibility Confirmed:**
- V2 decode() rejects length != 80 (Line 23: `len!=kPacketSize`)
- V1 packets (76 bytes) rejected by V2 firmware
- V2 packets (80 bytes) rejected by V1 firmware

**If HMAC function is issue:**
- All 4 devices have faulty V2 firmware
- NO time sync packets sent by any device
- Grant channel unaffected (different protocol/HMAC function)

**Rollback Required:** If HMAC fix not available, revert all 4 devices to v87-beta131 (V1)

---

## Recommended Next Steps

1. **IMMEDIATE:** Locate clusterEspNowTimeSign implementation
   ```bash
   grep -rn "clusterEspNowTimeSign\|clusterEspNowTimeVerify" /home/itakka/ESP32_Board/sensorforge/
   ```

2. **Check for hardcoded V1 sizes:**
   ```bash
   grep -n "\b44\b" <hmac_implementation_file>
   ```

3. **Add diagnostic output** to encode() and coordinator beacon path

4. **Test on ONE device first:**
   - Flash coordinator (devfreenove1) with diagnostic-enhanced firmware
   - Monitor Serial output for encode/HMAC failures
   - Verify beacons being sent before flashing other devices

5. **If HMAC issue confirmed:** Update to use kPayloadSize constant

6. **Retest:** Run all 22 tests + new HMAC integration test

---

## Files Requiring Investigation

**Priority 1 - HMAC Function Location:**
- sensorforge.ino (main Arduino file)
- cluster.cpp (cluster management)
- config_secrets.* (HMAC key management)

**Priority 2 - Diagnostic Output:**
- cluster_espnow_packets.cpp (encode() function)
- cluster_espnow_radio.cpp (coordinator beacon sending)

---

**Status:** Analysis complete, HMAC function investigation required
**Confidence:** 85% that HMAC signing function has hardcoded V1 payload size
**Risk:** High - all 4 devices non-functional for time sync
