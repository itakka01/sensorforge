# ESP-NOW Probe/Reply/Final integration (diagnostic only)

- `Probe` (Node -> broadcast) carries t1 in the node monotonic clock.
- `Reply` (Coordinator -> unicast) carries t2: receive callback timestamp at coordinator.
- `Final` (Coordinator -> unicast) carries t3: timestamp before the radio API call.
- Node records t4 in its receive callback for `Final` and uses `cluster_espnow_twoway` to estimate offset and RTT.
- 16 probes form a series, nominally at least 120 ms apart. Each series restarts after 60 s. Timeouts increment a diagnostic counter.
- All frames use the cluster ESP-NOW HMAC callback; nonce, epoch and coordinator ID tie frames to the session. Replay state is maintained for Coordinator transmissions, and per-source MAC for probes.
- Measurements are **diagnostic only**; no clock is changed, no drift is persisted. In particular do not interpret this as microsecond *accuracy*.
- Coordinator sends two unicast messages per probe. A send return code only means accepted by the local ESP-NOW API. No reliable delivery or MAC-layer timestamp is claimed.
- Both devices must associate with an AP on the same Wi-Fi channel. Callback latency and airtime jitter remain uncorrected.
- No Arduino ESP32 board builds or real radio measurements were run here. Test first on a pair of devices before considering time steering.
