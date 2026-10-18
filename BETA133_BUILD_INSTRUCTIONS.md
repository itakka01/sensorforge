# SensorForge v87-beta133 – Build Instructions

## Fix Summary

**Issue:** 80-byte packet dispatch conflict
- Diagnostic Beacons ('SFDB'): 80 bytes
- Time Packets V2 ('SFEN'): 80 bytes
- **Previous behavior:** ALL 80-byte packets routed to Diagnostic queue
- **Result:** Time-sync channel dead (`received=0`)

**Fix:** Magic-based dispatch in `onReceive()`
- Byte 2 = 'D' → Diagnostic queue
- Byte 2 = 'E' → Time queue
- Grant (96 bytes) → Grant queue

**Changed Files:**
- `cluster_espnow_radio.cpp:671-734` - Fixed dispatch logic
- `sensorforge_version.h:23-33` - Updated to beta133

**Test Results:** 8/8 PASSED (see test_80byte_dispatch.cpp)

---

## Build Process

### 1. FREENOVE Build (Coordinator: devfreenove1)

**Board Configuration (already correct):**
```cpp
// board_config.h:27-28
#define BOARD_FREENOVE    // ← Active
//#define BOARD_XIAO       // ← Commented
```

**Build Command:**
```bash
# Arduino IDE: File → Open → sensorforge.ino
# Tools → Board → ESP32 → "ESP32S3 Dev Module"
# Tools → Upload Speed → 921600
# Sketch → Verify/Compile
```

**Output:** `.pio/build/freenove_esp32s3/firmware.bin`

---

### 2. XIAO Build (Broadcaster + Nodes: devxiao1, devxiao2, devxiao3)

**Board Configuration Change Required:**
```cpp
// board_config.h:27-28
//#define BOARD_FREENOVE   // ← Comment out
#define BOARD_XIAO        // ← Activate
```

**Build Command:**
```bash
# Arduino IDE: Same as above
# Tools → Board → ESP32 → "XIAO_ESP32S3"
# Sketch → Verify/Compile
```

**Output:** `.pio/build/xiao_esp32s3/firmware.bin`

**IMPORTANT:** Remember to switch back to FREENOVE after XIAO builds!

---

## Flash Order (CRITICAL)

**All 4 devices MUST be flashed (V2 protocol incompatible with beta132 dispatch bug):**

1. **devfreenove1** (Coordinator) - FREENOVE build
2. **devxiao3** (Broadcaster) - XIAO build
3. **devxiao1** (Node) - XIAO build
4. **devxiao2** (Node) - XIAO build

---

## Verification

### Check Version:
```bash
curl http://<device-ip>/cluster/espnow | grep release
```

**Expected:** `release=v87-beta133`

### Check Time-Sync Channel:
```bash
curl http://<device-ip>/cluster/espnow | grep -E "received|authenticated"
```

**Expected (after ~30 seconds):**
- Coordinator: `received > 0` (receives Probes from nodes/broadcaster)
- Broadcaster: `received > 0` (receives Beacons from coordinator during bootstrap)
- Nodes: `received > 0` (receives Beacons from coordinator or broadcaster)

### Hardware Acceptance Test:

**Wait 60 seconds after all devices booted, then check:**

**Coordinator (devfreenove1):**
```
espnow_time_local.received > 0          ← Receives Probes
espnow_time_local.probes_answered > 0   ← Answers Probes
espnow_grant_diagnostic.grants_sent > 0 ← Grant channel still works
coordinator_beacons_suppressed > 0      ← Suppressed after broadcaster active
```

**Broadcaster (devxiao3):**
```
espnow_time_local.received > 0                  ← Receives coordinator beacons during bootstrap
espnow_time_local.probes_sent > 0               ← Sends probes to coordinator
espnow_time_local.exchanges_complete > 0        ← Completes time exchanges
broadcaster_production_beacons_sent > 0         ← Sends production beacons
espnow_grant_diagnostic.authenticated > 0       ← Grant channel still works
```

**Nodes (devxiao1, devxiao2):**
```
espnow_time_local.received > 0          ← Receives beacons from broadcaster
espnow_time_local.probes_sent > 0       ← Sends probes
espnow_time_local.exchanges_complete > 0 ← Completes time exchanges
```

**SUCCESS CRITERIA:**
- ✓ All devices report `release=v87-beta133`
- ✓ NO device shows `received=0` (dispatch conflict fixed)
- ✓ Grant channel still works (`grants_authenticated > 0`)
- ✓ Broadcaster activates (`broadcaster_production_beacons_sent > 0`)
- ✓ Coordinator suppresses (`coordinator_beacons_suppressed > 0`)
- ✓ Nodes synchronize with broadcaster

---

## Rollback (if needed)

**If beta133 fails:**
1. Check Serial logs for `[RX-REJECT]` messages
2. Verify all 4 devices show `release=v87-beta133`
3. Check board_config.h matches device (FREENOVE vs XIAO)

**Rollback is NOT possible** - beta132 has the dispatch bug, devices would remain at `received=0`.

---

## Technical Details

**Magic Bytes (HMAC-protected, cannot be spoofed):**
- Diagnostic: 'SFDB' (0x53, 0x46, 0x44, 0x42)
- Time: 'SFEN' (0x53, 0x46, 0x45, 0x4E)
- **Byte 2 distinguishes:** 'D' (0x44) vs 'E' (0x45)

**Dispatch Logic (cluster_espnow_radio.cpp:671-734):**
```cpp
if(length == 96) → GRANT
if(length == 80 && data[2] == 'D') → DIAGNOSTIC
if(length == 80 && data[2] == 'E') → TIME
else → REJECT
```

**Security:**
- Magic is part of HMAC payload (verified in decode())
- Spoofing would fail HMAC check
- Invalid magic rejected before queuing
- No security weakening

**Performance:**
- 3 byte comparisons in RX callback (fast)
- No HMAC in callback (HMAC remains in service loop)
- No Serial spam in callback
