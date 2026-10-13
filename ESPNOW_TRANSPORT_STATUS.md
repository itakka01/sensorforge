# ESP-NOW time packet codec (integration stage)

New `cluster_espnow_packets.*` files serialize fixed 76-byte frames, authenticate
via injected MAC callbacks, decode only after verification, and reject replayed
or non-increasing sequences within one coordinator epoch/boot. Wire timestamps
are *software* monotonic timestamps, not PHY TX/RX timestamps.

IMPORTANT: This is **not yet** an active ESP-NOW radio transport. The caller
must use the existing authenticated cluster identity, derive a domain-separated
HMAC key (e.g. HKDF-SHA256 with an ESP-NOW-specific info string), verify the
expected coordinator identity and boot/epoch, and reset replay state only after
an independently authenticated coordinator transition. Never accept a new
identity merely because a packet says it is new. Receive callbacks must only
queue raw packets; do MAC validation and state updates in a worker context.

A separate phase is required for ESP-NOW radio initialization, Wi-Fi channel
compatibility, per-peer unicast measurements, link status, precision model,
periodic broadcasts, and coordinator controls. Existing Wi-Fi synchronization
is unaffected. Persistent drift estimates are not proof of a valid clock.
