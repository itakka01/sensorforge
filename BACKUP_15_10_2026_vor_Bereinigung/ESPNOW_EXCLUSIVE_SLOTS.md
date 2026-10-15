# Exclusive ESP-NOW measurement slots (beta105)

Coordinator learns authenticated Node boot IDs and radio MACs from NodeSyncReport/NodeDriftReport, which remain periodic broadcasts. It grants one boot-ID-specific SyncControl lease at a time (nonce=target boot ID). Nodes ignore controls addressed to another boot ID. Lease is 3 seconds, refreshed every second for a 14-second measurement window; afterward a 3.5-second guard exceeds the previous lease even when revoke is lost. 12 entries maximum; peers are only considered active with reports in the last 30 seconds. Rotation is round robin. Coordinator only answers Probes from active owner; all HMAC/replay protections remain. The Frame layout is unchanged but semantics REQUIRE flashing coordinator AND all nodes.

Failed measurement retries stay at 12-24 seconds. Success retry eligibility is 45 seconds, allowing a 3-node slot rotation of about 52.5 seconds (14+3.5 each). Slot end never resets a calibrated model, but expires the active permission. Capture pause explicitly revokes and suspends slot selection.

CAVEATS: No hardware verification; this is an experimental protocol patch. Slot isolation does not establish actual GPIO or camera exposure skew, nor guarantee every series meets 250us spread. Firmware build and run-on-hardware required. The send callback is not an RF airtime timestamp.
