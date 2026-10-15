# SensorForge v87-beta105 – ESP-NOW raw exchange export (stage 1)

Baseline: user-uploaded `sensorforge_2026-10-13_22-48-18.zip` with latest late-TX-stamp fix.

## Implemented

- Keep a bounded rolling ring of the last **100 fully completed authenticated normal precision-sync exchanges** on each XIAO node (t1 Node TX, t2 Coordinator RX, t3 Coordinator TX delivered in Final, t4 Node RX of Reply). This records the values already used by the current estimator; no fabricated one-way latency or physical timestamp claim.
- Read-only endpoint `GET /cluster_espnow_benchmark` (JSON) or `GET /cluster_espnow_benchmark?format=csv` (CSV) on the XIAO itself.
- Add CSV/JSON links for every Node under Coordinator's existing **Verbindungsdiagnose und Einzeltests**. The original signed Job Probe remains unchanged.
- Export: sample index, uptime in milliseconds, four monotonic timestamps, computed RTT in microseconds, JSON minimum/maximum RTT and offset range for the retained window, midpoint offset estimate in microseconds (including .5). Negative half-integers are formatted correctly.
- Stream export line-by-line to avoid large temporary response allocation. Fixed-size ring ~4 KB Node RAM; zero SD/NVS writes. Ring clears on radio restart/channel/role change.
- **No new ESP-NOW packets**, slot changes, quality gate changes or capture changes. No browser polling added.

## Scope and limitations – important

This is a **PASSIVE RAW-DATA BENCHMARK**, not the originally proposed complete active bidirectional test-suite. It does **not** transmit 100 dedicated Coordinator->Node and 100 Node->Coordinator data frames, control packet intervals, pause/resume the normal measurement scheduler, or calculate independent one-way delays. Those parts require an authenticated control-plane protocol and coordinated exclusive session lifecycle and remain unimplemented.

The per-exchange *offset* is a four-timestamp estimate under symmetric path-delay assumptions; it is not absolute accuracy or camera exposure skew. Timestamp values t1/t4 belong to the Node monotonic clock, t2/t3 to the Coordinator. To compare samples, normalize their offsets to a reference/median, not across differing boot pairs.

Coordinator's endpoint intentionally returns an empty sample list: the raw completed exchanges are held by the Nodes. Node URLs:
- http://192.168.0.3/cluster_espnow_benchmark?format=csv
- http://192.168.0.206/cluster_espnow_benchmark?format=csv
- http://192.168.0.219/cluster_espnow_benchmark?format=csv

Wait for >=100 completed exchanges on each Node, download 3 CSVs, then compare RTT, offset spread, tails and per-Node success counters. Use snapshot soon after testing; ring holds only last 100.

## Verification

- Source edits anchored to exact symbols in supplied archive.
- ZIP integrity and delta file verification done.
- **No complete ESP32 cross-build and no on-device test were possible. Do not treat this as hardware-validated.**
