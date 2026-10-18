# SensorForge v87-beta132 – V2 Fix with Diagnostics

**Status:** ROOT CAUSE INDETERMINATE - Diagnostic instrumentation required
**Date:** 2026-10-18

---

## Verified: NOT the Problem

✓ V2 packet format correct (80 bytes)
✓ HMAC function uses kPayloadSize constant (48 bytes) - correctly updated for V2
✓ Reception path configured for 80-byte packets
✓ No hardcoded V1 sizes found
✓ sizeof() calculations correct

---

## Problem: Complete Time Sync Failure

**Symptoms:**
- sent=408 (some packets sent, likely diagnostic beacons)
- broadcaster_production_beacons_sent=0 (NO production beacons)
- probes_sent=0 (NO probes)
- received=0, authenticated=0, rejected=0 (NOTHING received)

**Bootstrap Sequence Stuck:** Coordinator → Broadcaster → Nodes chain broken

---

## Fix: Add Diagnostic Output

Apply these changes to identify exactly where the failure occurs:

### 1. cluster_espnow_packets.cpp - Encode Diagnostics

**Add diagnostic output to encode() function:**

```cpp
bool encode(const Frame &f, uint8_t out[kPacketSize], SignMac sign, void* user) {
  if (!out) {
      Serial.println("[ENCODE-FAIL] out=nullptr");
      return false;
  }
  if (!sign) {
      Serial.println("[ENCODE-FAIL] sign=nullptr");
      return false;
  }
  if (!typeValid(f.type)) {
      Serial.printf("[ENCODE-FAIL] Invalid type: %u\n", static_cast<uint8_t>(f.type));
      return false;
  }
  if (!f.epoch) {
      Serial.println("[ENCODE-FAIL] epoch=0");
      return false;
  }
  if (!f.coordinatorTag) {
      Serial.println("[ENCODE-FAIL] coordinatorTag=0");
      return false;
  }
  if (!f.senderBoot) {
      Serial.println("[ENCODE-FAIL] senderBoot=0");
      return false;
  }
  if (!f.sequence) {
      Serial.println("[ENCODE-FAIL] sequence=0");
      return false;
  }

  memset(out,0,kPacketSize);
  out[0]='S';out[1]='F';out[2]='E';out[3]='N';out[4]=kVersion;out[5]=static_cast<uint8_t>(f.type);
  put(out,8,f.epoch,4);put(out,12,f.sequence,4);put(out,16,f.coordinatorTag,8);
  put(out,24,f.senderBoot,8);put(out,32,static_cast<uint64_t>(f.timeUs),8);put(out,40,f.nonce,4);
  put(out,44,f.preSendDelayUs,4);

  Serial.printf("[ENCODE] Calling sign with len=%zu (kPayloadSize=%zu)\n", kPayloadSize, kPayloadSize);
  bool signOk = sign(out,kPayloadSize,out+kPayloadSize,user);

  if (!signOk) {
      Serial.println("[ENCODE-FAIL] HMAC signing failed");
      Serial.printf("  Type=%u epoch=%u tag=%llx boot=%llx seq=%u\n",
                    static_cast<uint8_t>(f.type), f.epoch, f.coordinatorTag,
                    f.senderBoot, f.sequence);
      return false;
  }

  Serial.printf("[ENCODE-OK] Type=%u epoch=%u seq=%u\n",
                static_cast<uint8_t>(f.type), f.epoch, f.sequence);
  return true;
}
```

### 2. cluster.cpp - HMAC Diagnostics

**Add diagnostic output to espNowTimeMac():**

```cpp
static bool espNowTimeMac(const uint8_t *data, size_t len, uint8_t mac[32])
{
    if (!runtimeActive) {
        Serial.println("[HMAC-FAIL] runtimeActive=false");
        return false;
    }
    if (!runtimeHmacKey) {
        Serial.println("[HMAC-FAIL] runtimeHmacKey=nullptr");
        return false;
    }
    if (!data) {
        Serial.println("[HMAC-FAIL] data=nullptr");
        return false;
    }
    if (!mac) {
        Serial.println("[HMAC-FAIL] mac=nullptr");
        return false;
    }
    if (len != EspNowTimePackets::kPayloadSize) {
        Serial.printf("[HMAC-FAIL] len=%zu != kPayloadSize=%zu\n",
                      len, EspNowTimePackets::kPayloadSize);
        return false;
    }

    static const uint8_t context[] = "SensorForge/ESP-NOW/time/HMAC/v1";
    uint8_t subkey[32] = {};
    if (!computePacketMac(context, sizeof(context) - 1, subkey)) {
        Serial.println("[HMAC-FAIL] computePacketMac failed");
        secureWipe(subkey, sizeof(subkey));
        return false;
    }
    const bool ok = computePacketMacWithRawKey(subkey, data, len, mac);
    secureWipe(subkey, sizeof(subkey));

    if (!ok) {
        Serial.println("[HMAC-FAIL] computePacketMacWithRawKey failed");
    } else {
        Serial.println("[HMAC-OK]");
    }
    return ok;
}
```

### 3. cluster_espnow_radio.cpp - Coordinator Beacon Diagnostics

**Add output to coordinator beacon path (Line 2565-2577):**

```cpp
            Serial.println("[COORDINATOR] Attempting to send beacon...");

            EspNowTimePackets::Frame b;
            b.type=EspNowTimePackets::Type::Beacon;b.epoch=epoch;
            b.coordinatorTag=tag;b.senderBoot=localBootNonce;

            Serial.printf("[COORDINATOR] Beacon: epoch=%u tag=%llx boot=%llx\n",
                          epoch, tag, localBootNonce);

            // Software sample pair. This is NOT an RF airtime timestamp.
            const int64_t monoBefore=esp_timer_get_time();
            struct timeval tv={};
            const bool clockOk=gettimeofday(&tv,nullptr)==0 && tv.tv_sec>=1577836800LL;
            const int64_t monoAfter=esp_timer_get_time();

            if (!clockOk) {
                Serial.println("[COORDINATOR] clockOk=false - beacon skipped");
                return;
            }

            if (monoAfter-monoBefore >= 2000) {
                Serial.printf("[COORDINATOR] Sample pair too slow: %lld us - beacon skipped\n",
                              monoAfter-monoBefore);
                return;
            }

            b.timeUs=static_cast<int64_t>(tv.tv_sec)*1000000LL+tv.tv_usec;
            b.nonce=static_cast<uint32_t>(monoBefore+(monoAfter-monoBefore)/2);

            Serial.println("[COORDINATOR] Calling sendFrame()...");
            bool sent = sendFrame(b,kBroadcastMac);

            if (sent) {
                Serial.println("[COORDINATOR] Beacon SENT successfully");
                if(beaconExperimentUntilMs) ++beaconExperimentSent;
            } else {
                Serial.println("[COORDINATOR] Beacon sendFrame() FAILED");
            }
```

### 4. cluster_espnow_radio.cpp - Reception Diagnostics

**Add output to reception callback (Line 696-707):**

```cpp
    Serial.printf("[RX-CB] Received %d bytes from %02x:%02x:%02x:%02x:%02x:%02x\n",
                  length,
                  info->src_addr[0], info->src_addr[1], info->src_addr[2],
                  info->src_addr[3], info->src_addr[4], info->src_addr[5]);

    if(length!=static_cast<int>(EspNowTimePackets::kPacketSize)) {
        Serial.printf("[RX-REJECT] Wrong size: %d != %zu (kPacketSize)\n",
                      length, EspNowTimePackets::kPacketSize);
        return;
    }

    Serial.println("[RX-CB] Size OK, queueing packet");

    const int64_t rx=esp_timer_get_time();
    portENTER_CRITICAL(&queueMux);
    uint8_t next=static_cast<uint8_t>((head+1)%kQueueSize);
    if(next==tail) {
        ++drops;
        Serial.println("[RX-CB] Queue FULL - packet dropped");
    } else {
        memcpy(queue[head].bytes,data,EspNowTimePackets::kPacketSize);
        memcpy(queue[head].mac,info->src_addr,6);
        queue[head].rxUs=rx;head=next;
        Serial.println("[RX-CB] Packet QUEUED successfully");
    }
    portEXIT_CRITICAL(&queueMux);
```

---

## Deployment Strategy

### Step 1: Build Diagnostic Firmware

```bash
cd /home/itakka/ESP32_Board/sensorforge
pio run --environment xiao_esp32s3
```

**Expected:** Compilation success with diagnostic output included

### Step 2: Flash ONE Device (Coordinator)

**Device:** devfreenove1 (Coordinator)

**Monitor Serial Output for:**
1. `[COORDINATOR] Attempting to send beacon...` - Is coordinator trying?
2. `[ENCODE-FAIL]` or `[ENCODE-OK]` - Is encode working?
3. `[HMAC-FAIL]` or `[HMAC-OK]` - Is HMAC working?
4. `[COORDINATOR] Beacon SENT successfully` - Are beacons actually sent?

### Step 3: Flash Broadcaster IF Coordinator Working

**Device:** devxiao3 (Broadcaster)

**Monitor Serial Output for:**
1. `[RX-CB] Received X bytes...` - Are beacons received?
2. `[RX-REJECT] Wrong size` - Size mismatch?
3. `[RX-CB] Packet QUEUED` - Reception working?
4. `[BROADCASTER] Production ACTIVATED` - Did lease activate?
5. `[BROADCASTER] Diagnostic beacon sent` - Is broadcaster active?

### Step 4: Identify Root Cause

**Possible Outcomes:**

**A. Coordinator Not Sending (`[COORDINATOR] Attempting...` never appears):**
- Check coordinator role configuration
- Check coordinatorStartupGuardUntilMs
- Check coordinatorSuppressionStartMs

**B. Encode Failing (`[ENCODE-FAIL]`):**
- Check which validation failed (epoch=0? tag=0? boot=0?)
- Verify coordinator initialization

**C. HMAC Failing (`[HMAC-FAIL]`):**
- Check which condition failed
- Most likely: runtimeActive=false or runtimeHmacKey=nullptr
- Check cluster credential initialization

**D. sendFrame Returning False:**
- Check esp_now_send() return code
- Add more detailed output in sendFrame()

**E. Packets Not Received (`[RX-CB]` never appears):**
- ESP-NOW communication issue
- Wrong channel?
- Peer not registered?

**F. Wrong Packet Size Received:**
- Sender still using V1 (76 bytes)?
- Check sender firmware version

---

## Expected Diagnostic Output (Success Case)

### Coordinator Serial:
```
[COORDINATOR] Attempting to send beacon...
[COORDINATOR] Beacon: epoch=2199669293 tag=... boot=...
[ENCODE] Calling sign with len=48 (kPayloadSize=48)
[HMAC-OK]
[ENCODE-OK] Type=1 epoch=2199669293 seq=1
[COORDINATOR] Calling sendFrame()...
[COORDINATOR] Beacon SENT successfully
```

### Broadcaster Serial:
```
[RX-CB] Received 80 bytes from xx:xx:xx:xx:xx:xx
[RX-CB] Size OK, queueing packet
[RX-CB] Packet QUEUED successfully
... (packet processing) ...
[BROADCASTER] Production ACTIVATED (txn ..., time valid)
```

---

## Cleanup After Root Cause Found

Once root cause identified and fixed:

1. **Remove all diagnostic Serial.printf() calls** - they slow down ESP-NOW
2. **Test without diagnostics** - verify performance
3. **Flash all 4 devices** in correct order (Coordinator → Broadcaster → Nodes)
4. **Verify production operation** - no diagnostic output

---

## Alternative: Minimal Diagnostic

If full diagnostic output too verbose, add ONLY critical checkpoints:

```cpp
// cluster_espnow_packets.cpp - encode()
if (!sign(out,kPayloadSize,out+kPayloadSize,user)) {
    Serial.println("[V2-ENCODE-FAIL]");  // ← Single line
    return false;
}

// cluster.cpp - espNowTimeMac()
if (len != EspNowTimePackets::kPayloadSize) {
    Serial.printf("[V2-HMAC-FAIL] len=%zu\n", len);  // ← Critical failure only
    return false;
}

// cluster_espnow_radio.cpp - coordinator beacons
if (!sendFrame(b,kBroadcastMac)) {
    Serial.println("[V2-BEACON-FAIL]");  // ← Single line
}
```

This gives minimal output while identifying the failure point.

---

**Status:** Diagnostic instrumentation ready for deployment
**Next Step:** User must build, flash coordinator, monitor Serial output
**Expected:** Immediate identification of failure point (encode/HMAC/send/receive)
