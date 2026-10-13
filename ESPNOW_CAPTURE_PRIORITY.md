# ESP-NOW capture priority (2026-10-13)

When a signed scheduled capture is accepted by a node, or the coordinator dispatches a scheduled capture, `pauseForCapture()` immediately discards incomplete Probe/Reply state and suppresses ESP-NOW service transmissions until 5 seconds after the target UTC time. Repeated shots extend the hold. Existing calibrated precision-clock values are retained during holdover and subject to the normal freshness gate; Wi-Fi remains fallback. No blocking waits are introduced.

The radio callback can still enqueue frames and Wi-Fi driver-level traffic cannot be silenced by this guard; after the hold the queue is flushed. The guard addresses scheduled cluster jobs on this path, not all other camera/streaming workflows. The coordinator's hold starts on job dispatch; if job submission fails the conservative hold remains until expiry.

Node-specific ESP-NOW timestamps are **not yet reported centrally**. The existing coordinator sync table's 'Letzte Messung' is Wi-Fi synchronization, not an ESP-NOW last-sync report. ESP-NOW local counters, last offset/RTT and precision_sample_age_ms are available in `espnow_time_local` JSON on the local device.

Full ESP32 build and RF hardware tests still required.
