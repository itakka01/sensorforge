# SensorForge Cluster Architecture

**Authoritative cluster foundation:** `v87-beta48`  
**Date:** 2026-10-08  
**Status:** foundation frozen for now; next functional stage is time synchronization + scheduled multi-camera capture.

This document is the detailed re-entry reference for the SensorForge local device-cluster foundation. It complements `00_PROJEKTBESCHREIBUNG_WIEDEREINSTIEG.txt`. The source code remains authoritative if documentation and code ever disagree.

## 1. Scope and freeze point

The current cluster layer provides discovery, authenticated peer membership, liveness, automatic Coordinator selection, passive cluster discovery for configuration, encrypted remembered credentials, and generic resource announcements. Beta 48 keeps the Beta-38+ wire model but fixes first-use Known-Cluster profile lookup and makes a saved Cluster configuration enter runtime deterministically through a controlled reboot.

The following are deliberately **not implemented yet**:

- clock correction / distributed time synchronization
- dedicated Time Master / laser synchronization
- remote snapshot / recording commands
- production sessions / synchronized takes
- automatic download of announced resources
- cluster-wide password rotation transaction
- quorum/consensus protocol for hard network partitions

The general cluster infrastructure should therefore remain unchanged until a concrete time-sync / scheduled-capture implementation is designed against the real hardware path.

**Hardware qualification status:** the code has been structurally/syntax checked during development, but a complete multi-device cluster regression on real hardware is still required. Do not treat the current foundation as field-qualified solely because it compiles or because the architecture is documented.

## 2. Non-negotiable ownership and safety rules

These rules are architectural invariants and must not be weakened by future work:

1. `cluster_enabled=0` is the default.
2. Cluster code must never call WiFi start/connect/AP ownership APIs in order to make itself available.
3. Cluster activity is strictly subordinate to the existing SensorForge WiFi lifecycle.
4. Cluster code must never prolong, reopen, or keep WiFi alive.
5. When WiFi is not available, active cluster networking is not available.
6. Ordered WiFi shutdown stops the cluster first; the cluster sends only a best-effort signed leave notification before its sockets are closed.
7. Recording, streaming, sleep/wake, storage, OTA, WebConfig, recovery and network ownership remain with their existing modules.
8. `webconfig.cpp` remains orchestration/navigation only where technically reasonable; cluster UI stays in `webconfig_cluster.cpp/.h`.
9. Existing functionality must not be removed or redesigned as incidental cleanup when extending the cluster.
10. New cluster changes must be based on the complete current source tree; never reconstruct code from memory or old delta ZIPs.

The only intentional exception to “cluster disabled means no cluster runtime” is the page-scoped **passive discovery scanner** described below. It exists only while `/cluster` is actively being polled and never transmits SensorForge cluster packets.

## 3. Identity model

Do not mix the following identities:

### 3.1 Node identity — `integration_id`

- stable random 128-bit ID stored in NVS
- shared by Integration API and cluster
- implemented centrally through `device_identity.cpp/.h`
- not the hostname
- not the MAC address
- not the license hardware ID

The `integration_id` is the persistent logical identity of one SensorForge node inside the cluster.

### 3.2 Cluster identity — `cluster_id`

Format:

`cl-` + 32 lowercase hexadecimal characters

The Cluster ID is public and stable. It identifies the logical cluster independently from its human-readable name and independently from the current password.

New Cluster IDs are random 128-bit values. A Beta-37 configuration that has no `cluster_id` derives a deterministic legacy Cluster ID from the existing Cluster name so jointly upgraded devices converge on the same Beta-38 identity.

### 3.3 Cluster name — `cluster_name`

Human-readable display name. It is not a cryptographic identity and may later be renamed without changing the Cluster ID.

### 3.4 Credential generation — `cluster_credential_epoch`

Public integer generation, starting at 1. It identifies which password generation is currently expected without publishing a password verifier.

A future successful cluster-wide password rotation must keep the same `cluster_id`, change the password, and advance this epoch.

### 3.5 Cluster secret — `cluster_password`

- shared secret, currently 8..63 characters when membership is enabled
- never advertised in mDNS, discovery, status JSON or logs
- stored in config through the existing hardware-bound SFSEC1 secret mechanism
- remembered known-cluster passwords are also stored as SFSEC1 ciphertext in NVS

### 3.6 Authentication trust domain

Beta 38 derives the cluster HMAC key from:

`SensorForgeClusterKeyV2 | cluster_id | password`

The public runtime tag is derived separately from the same stable Cluster ID + password pair. The display name is intentionally excluded from authentication identity.

Consequence: Beta-37 and Beta-38 devices must not be mixed in one active cluster because Beta 38 changed the trust-domain derivation. Devices upgraded together from Beta 37 derive the same deterministic legacy Cluster ID and converge again after the joint upgrade.

## 4. Persistent configuration

Current configuration fields:

- `cluster_enabled=0|1` — default `0`
- `cluster_id=cl-<32 lowercase hex>`
- `cluster_name=<display name>`
- `cluster_credential_epoch=<integer >= 1>` — default `1`
- `cluster_password=<shared secret>`
- `cluster_coordinator_policy=auto|preferred|node` — default `auto`

Cluster settings are saved through the normal config system. A saved change may require the next normal WiFi/device restart before the runtime is using the new credentials/role policy; the Cluster page does not force an immediate reboot.

## 5. Network topology and ports

### 5.1 Multicast channel

- group: `239.255.83.70`
- UDP port: `39427`
- maximum accepted cluster packet length: 511 bytes

This channel carries public discovery and authenticated best-effort cluster metadata/events.

### 5.2 Coordinator direct channel

- UDP unicast port: `39428`

This channel is separate from multicast and is used for authenticated Node → Coordinator liveness heartbeats and Coordinator ACKs.

### 5.3 mDNS/DNS-SD

SensorForge retains one central mDNS lifecycle. Cluster code does not call a second `MDNS.begin()`.

When active, Cluster adds:

`_sfcluster._udp`

Current mDNS metadata version is `4`. TXT metadata includes node/cluster identifiers and feature information, but membership must never be inferred from unauthenticated mDNS metadata alone.

Current feature advertisement includes:

`lease,leave,resource,coordinator,passive-discovery`

## 6. Runtime lifecycle

### Cluster disabled

Normal runtime has no persistent cluster socket, heartbeat, mDNS cluster service, HMAC work or resource traffic.

### Cluster enabled + WiFi unavailable

Cluster remains inactive. It must not try to make WiFi available.

### Cluster enabled + existing WiFi lifecycle becomes available

The existing WiFi owner starts networking first. Only after that may `clusterNetworkStart()` initialize:

- derived runtime HMAC key
- multicast socket on 39427
- direct Coordinator socket on 39428
- volatile peer/resource state
- boot nonce / sequences
- Coordinator election state

### WiFi shutdown

`clusterNetworkStop()` runs before radio teardown. If the active runtime can still transmit, it sends a signed best-effort `SFL1` leave packet. Failure to send leave never blocks shutdown.

Unexpected power loss/crash/funk loss is handled by lease expiry rather than by relying on leave.

## 7. Passive cluster discovery while membership is disabled

This is a configuration convenience only.

When `/cluster` is actively open, its status polling calls the discovery touch path. If WiFi is already up but active Cluster membership is disabled, SensorForge may temporarily bind only the multicast receive side.

Properties:

- scanner lease: 12 seconds, continuously refreshed while the page polls
- scanner does not start or extend WiFi
- scanner does not open the Coordinator socket
- scanner never joins/authenticates to a cluster
- scanner never sends SensorForge cluster packets
- scanner stops automatically after page polling ceases
- discovered public nodes expire after about 35 seconds
- up to 32 public discovery nodes are tracked temporarily

The UI can therefore offer a drop-down of visible Cluster IDs/names without making the device a cluster member.

## 8. Public discovery versus authenticated membership

A critical distinction:

### `SFD1` — public discovery

Unauthenticated and intentionally public. Current payload concept:

- Cluster ID
- credential epoch
- source `integration_id`
- Cluster display name
- Coordinator policy
- whether the announcing node currently believes it is Coordinator
- firmware release

It contains no password and does not grant membership.

A malicious LAN participant could advertise a fake `SFD1` entry. Therefore `SFD1` is only a user-interface/discovery hint. Election, membership, liveness and future control must use authenticated data.

### Authenticated traffic

Authenticated packet families include an HMAC-SHA256 based on the active cluster trust domain. A node with the same Cluster ID but a different password is not an authenticated peer and does not participate in the same election.

## 9. Packet families in Beta 38

These packet names are useful during future debugging/re-entry:

- `SFD1` — public passive-discovery announcement, not authenticated
- `SFC1` — authenticated basic presence/status heartbeat
- `SFM1` — authenticated lease + approximate WiFi remaining metadata
- `SFL1` — authenticated best-effort leave
- `SFR1` — authenticated generic resource announcement
- `SFCO1` — authenticated coordination/election metadata
- `SFNH1` — authenticated Node → Coordinator direct heartbeat
- `SFNA1` — authenticated Coordinator → Node ACK

Packet formats are internal protocol details and must not be silently reinterpreted. Add/version fields or introduce a new packet revision if a future feature requires incompatible semantics.

The main loop processes a bounded amount of cluster traffic per pass: at most four multicast packets plus four direct Coordinator packets. This is deliberate protection against cluster traffic monopolizing the main loop.

## 10. Presence and liveness

### Multicast presence

Normal active members emit presence every 10 seconds.

`SFC1` carries basic node runtime state, including:

- Node ID
- boot nonce + sequence
- uptime
- UTC epoch if local time is valid
- operating mode
- recording/streaming/transport flags
- firmware release
- hostname

`SFM1` follows with:

- 60-second lease
- approximate `wifi_remaining_sec`

### Lease

Default authenticated peer lease: **60 seconds**.

A single lost multicast packet therefore has no effect. Several packets can be missed without causing immediate removal/election changes.

### `wifi_remaining_sec`

Status hint only. `-1` means unknown/unbounded. It may be derived from SensorForge's existing WiFi timeout/schedule logic.

No cluster component may use this value to keep WiFi alive.

### Leave

An ordered WiFi shutdown sends `SFL1` so peers can remove the departing node immediately. Loss of the leave packet is harmless because the 60-second lease remains the authoritative fallback.

## 11. Coordinator policy and automatic election

The user does not force a permanent master. Configuration only defines candidacy policy:

- `auto` — eligible; default
- `preferred` — eligible and preferred over `auto`
- `node` — never eligible to become Coordinator

Election is deterministic:

1. `preferred` candidates outrank `auto`
2. `node` candidates are ignored
3. when candidates have the same policy, stable `integration_id` ordering breaks the tie
4. only currently leased/authenticated peers are candidates

The current role is runtime state, not a persistent “I am master” flag.

A newly started first `auto`/`preferred` node can therefore bootstrap a cluster by itself and become Coordinator. If every online member is `node`, the cluster intentionally has no Coordinator.

## 12. Coordinator epoch

Whenever a local node newly becomes Coordinator it creates a runtime Coordinator epoch/generation.

Future critical commands must be associated with at least:

- Coordinator `integration_id`
- Coordinator epoch
- future production/session ID

This prevents stale commands from a previous Coordinator generation from being treated as current merely because the same physical node later returns.

The current election mechanism is deliberately lighter than a full quorum consensus protocol. A hard WLAN network partition can theoretically produce separate independently operating authenticated subclusters. Critical production-session semantics must address this explicitly when they are implemented.

## 13. Direct Node → Coordinator liveness

Multicast is best-effort, so Beta 37+ adds a second liveness channel.

Every non-Coordinator with a valid selected Coordinator sends an authenticated unicast `SFNH1` heartbeat approximately every 10 seconds to UDP 39428.

The heartbeat includes the intended Coordinator ID and epoch. The Coordinator accepts it only if it is currently that Coordinator and the epoch matches.

The Coordinator replies with authenticated `SFNA1` ACK containing the heartbeat sequence. Replayed/older heartbeat or ACK sequences are ignored.

Direct heartbeat receipt refreshes peer liveness at the Coordinator. Therefore a missed multicast presence packet alone does not make the Coordinator lose the node.

Current direct Coordinator lease is also 60 seconds.

## 14. Known Cluster profiles

Up to six successfully used clusters can be remembered board-locally.

Stored public metadata includes:

- Cluster ID
- Cluster name
- credential epoch
- whether a password is present

The password itself is stored only as board-bound SFSEC1 ciphertext in NVS and is never exposed to the browser/status JSON.

If a previously known Cluster is selected again and the visible/expected credential epoch still matches, firmware may reuse the remembered password internally without the user retyping it.

If the epoch differs, the cached password is considered stale and automatic reuse is blocked. The user must provide the new password once; a later successful join can refresh the known-cluster cache.

Known profiles are convenience state only. Active cluster membership remains defined by the normal configuration.

## 15. Security boundary of the shared-password protocol

The current Cluster authentication model is a local-LAN shared-secret design, not a PAKE or certificate-based protocol. Authenticated packets contain enough public message material for a listener who captures Cluster traffic to test password guesses offline by deriving candidate keys and comparing HMACs. The mDNS runtime tag is also derived deterministically from Cluster ID + password and must be treated as public metadata, not as a secret.

Therefore:

- use a high-entropy Cluster password/secret, not a weak human password
- do not expose the Cluster to an untrusted/shared network and assume the password is protected merely because it is never transmitted in plaintext
- before a commercial security claim is made, re-evaluate whether onboarding should generate a random Cluster secret (for example via QR/copy workflow) or whether a stronger authenticated key-exchange/PAKE design is required
- do not reuse WiFi, WebConfig, license or other product passwords as the Cluster secret

This limitation does not allow a passive listener to join without guessing the secret, but it defines the resistance of the current Beta-38 trust model and must remain visible in future security reviews.

## 16. Password-change / credential rules

Cluster ID and password are independent.

### Current Beta-38 safety behavior

A local password replacement for the same Cluster ID is blocked while authenticated remote peers are currently attached. This prevents one user action on one device from silently splitting a healthy cluster.

An isolated/recovery device with no authenticated peers may be locally re-credentialed; credential epoch advances as needed.

### Future proper cluster-wide rotation

Not implemented yet. The intended model is Coordinator-only and transactional:

1. PREPARE a new secret under the current authenticated trust domain
2. active members ACK receipt/readiness
3. Coordinator COMMITs the new credential generation
4. all participating nodes move to the new password + incremented credential epoch

Offline nodes that miss the rotation will later see the newer public credential epoch, refuse automatic use of their stale cached password, and require re-credentialing.

### Same Cluster ID, different passwords

Such nodes are separate authentication partitions. They do not elect each other and do not automatically select a password winner. Public discovery may reveal that the same Cluster ID is present with incompatible credential generations/configurations, but only explicit recovery/rotation can resolve it.

## 17. Cluster status UI

`/cluster` is implemented in `webconfig_cluster.cpp/.h`. `webconfig.cpp` only wires navigation/routes.

Each node can show the complete currently authenticated visible cluster, including the local node.

Current status includes, where available:

- integration ID
- hostname / IP
- operating mode
- firmware release
- uptime
- recording / streaming / transport state
- local time validity / epoch
- lease and remaining lease
- approximate WiFi remaining time
- configured Coordinator policy
- actual runtime role
- selected Coordinator + epoch
- direct-heartbeat / ACK age

The page is status/configuration only; it does not currently issue remote control commands to other nodes.

## 18. Generic resource announcements

`clusterAnnounceResource()` provides a generic authenticated metadata broadcast foundation.

Current `SFR1` metadata contains:

- owner `integration_id`
- resource ID
- resource type
- UTC timestamp in **microseconds** if known
- size
- TTL
- compact source-relative locator

Limits:

- up to 24 volatile resources in the local table
- resource TTL 1..3600 seconds
- resource ID up to 48 characters
- type up to 24 characters
- locator up to 160 characters

Important:

- JPEG/video/file bytes are never broadcast through the cluster multicast channel
- no camera/recording producer currently calls this automatically
- no automatic peer download is implemented yet
- future pull must reuse an existing, reviewed SensorForge storage/API read path and respect ownership/concurrency rules

A resource announcement is a notification/hint, not the durable source of truth. Because multicast can be missed, future important resource workflows should combine announcements with a recoverable manifest/catalog or sequence-based catch-up mechanism, followed by reliable unicast transfer.

## 19. Performance design constraints

The cluster is intentionally low-rate and bounded:

- Cluster disabled: no permanent cluster traffic/runtime
- Presence cadence: 10 seconds
- direct Node → Coordinator heartbeat: 10 seconds
- small control/status packets only
- packet size capped at 511 bytes
- at most 16 authenticated peers
- at most 24 volatile resource entries
- at most four multicast + four direct packets processed per loop pass
- large resources never multicast

These choices are intended to make the feature negligible compared with MJPEG/RTSP/video traffic. Nevertheless, real hardware regression is still required before calling the performance impact proven negligible.

Recommended regression comparison:

- Cluster OFF vs ON
- recording-only
- RTSP/HTTP-MJPEG streaming
- audio streaming where applicable
- multiple cluster peers
- FPS / frame drops / reconnects
- CPU / heap / PSRAM
- temperature
- WiFi stability
- sleep/wake behavior

## 20. Failure behavior to preserve

### Single multicast packet lost

No immediate effect. Lease/direct heartbeat continues.

### Several multicast packets lost, direct Coordinator heartbeat still works

Coordinator can retain node liveness through the unicast path.

### Ordered WiFi shutdown

Best-effort leave + normal cluster stop. Cluster never blocks shutdown.

### Crash/power loss/radio loss

No leave is expected. Peer disappears after lease expiry.

### Coordinator disappears

After authenticated peer state/lease expiry causes recomputation, eligible nodes deterministically select another Coordinator.

### All eligible Coordinator nodes disappear

Only `node` members remain; no Coordinator exists until an eligible node returns.

### Same Cluster ID with wrong password

No authenticated membership; separate authentication partitions.

### Stored known-cluster password is stale

Credential-epoch mismatch blocks automatic password reuse; user supplies new credential.

## 21. Planned first real application: time synchronization + scheduled capture

This is the next intended cluster feature, but it is not implemented yet.

### 20.1 Keep Coordinator and Time Master conceptually separate

The Coordinator is the organizational/control leader.

The future Time Master is the node providing the best timing reference. In many installations they may be the same device, especially if that device has laser synchronization hardware, but the protocol should not assume they must always be identical.

Future capabilities may include, for example:

- software/NTP-quality time
- WiFi TSF/FTM-derived timing
- laser synchronization transmitter/receiver
- external hardware pulse/PPS/clock input

Do not turn the current `auto/preferred/node` Coordinator policy into a permanent forced Time-Master switch.

### 20.2 Separate timestamp representation from real accuracy

The current resource protocol stores `timestampUs` (UTC microseconds). Do **not** silently reinterpret this existing field as nanoseconds.

When the time-sync layer is implemented, introduce an additive/versioned representation capable of carrying, for example:

- 64-bit time in nanoseconds
- time source/domain
- estimated offset
- measured drift
- uncertainty / quality in nanoseconds
- age of last synchronization

This keeps the protocol open to sub-microsecond hardware methods later without pretending the current system already has that precision.

### 20.3 Scheduled execution, not packet-arrival execution

Critical multi-camera actions should not mean “take a picture when this WiFi packet arrives”.

Intended pattern:

1. Coordinator establishes a production/session context.
2. Nodes have an already synchronized local cluster clock with known uncertainty.
3. Coordinator sends a command sufficiently early with an absolute target execution time.
4. Each Node ACKs that the command is authenticated, current and scheduled.
5. Each Node executes from its own local synchronized timer at the target time.
6. Each Node returns an execution report with actual local execution timestamp and timing quality.

This makes WLAN packet latency/jitter much less important than the quality of the synchronized local clocks.

### 20.4 Production session context

Future critical control should bind commands to at least:

- explicit production/session ID
- Coordinator ID
- Coordinator epoch
- command ID/sequence
- target execute time
- authentication

Stale commands from a previous Coordinator generation/session must be rejected.

During an active take, do not silently perform an automatic Coordinator failover and continue as though nothing happened. A lost Coordinator should initially be treated as a failed/incomplete take; re-election can prepare the next session. Any more advanced behavior must be designed and tested explicitly.

### 20.5 Broadcast versus reliable control

Use multicast/broadcast for discovery and non-critical event/resource notification.

Use authenticated acknowledged unicast for critical commands and readiness/ACK flows. Important commands such as scheduled snapshot, start/stop recording, time-sync state changes or credential changes must not depend on a single best-effort multicast packet.

## 22. Recommended re-entry sequence when time-sync work resumes

1. Start from the complete then-current source tree, not this document or an old Cluster delta ZIP.
2. Re-read `cluster.cpp/.h`, `webconfig_cluster.cpp/.h`, `cluster_profiles.cpp/.h`, WiFi lifecycle ownership, camera capture ownership and the relevant timer APIs.
3. First hardware-test the existing Beta-38 foundation with at least two, preferably three devices:
   - discovery
   - known profile reuse
   - Auto/Preferred/Node election
   - direct heartbeat/ACK
   - LEAVE
   - lease expiry
   - WiFi shutdown behavior
   - Cluster OFF behavior
4. Measure Cluster OFF vs ON performance under real recording/streaming load.
5. Define the actual physical synchronization mechanism and measurable target accuracy.
6. Implement a read-only timing diagnostics stage first: offset/drift/uncertainty measurement without controlling the camera.
7. Only after timing quality is measured, design the scheduled camera-trigger hook against the real existing camera ownership/capture code.
8. Add production-session + authenticated scheduled command + ACK/execution report.
9. Add resource announcements/pull only on top of the real existing storage/API path.
10. Keep all new functionality additive and modular; do not grow `webconfig.cpp` into a feature implementation file.

## 23. Files relevant to future Cluster work

Primary current modules:

- `cluster.cpp` / `cluster.h` — runtime, packets, peer/resource tables, liveness, election
- `cluster_profiles.cpp` / `cluster_profiles.h` — encrypted known-cluster cache
- `webconfig_cluster.cpp` / `webconfig_cluster.h` — Cluster WebConfig/status UI
- `device_identity.cpp` / `device_identity.h` — shared stable node identity
- `config.cpp` / `config.h` — persistent Cluster config
- `config_secrets.cpp` / `config_secrets.h` — active Cluster secret + profile password protection
- `sensorforge.ino` — authoritative WiFi/mDNS lifecycle hooks; Cluster must remain subordinate

Related future timing/capture work must additionally inspect the real camera, recorder, timer and WiFi lifecycle source present at that future revision before changing anything.

## 24. Development / delivery rules for Cluster work

The normal SensorForge rules apply without exception:

- never guess code
- work from the complete latest source tree
- inspect all real callers/owners before changing a lifecycle path
- safety first
- preserve existing functionality unless the requested change explicitly replaces it
- prefer small additive modules
- keep `webconfig.cpp` slim
- secrets never appear in UI/status/logs/release artifacts
- code changes are delivered only as one delta ZIP containing exactly changed/new files under their original project names/paths
- do not add unrelated cleanup, patches, renamed copies or test-plan files to the code ZIP


## Beta 48 activation and first-use clarification

A configured Cluster is not considered discoverable merely because values exist in persistent configuration. Public `SFD1` discovery is emitted only while `clusterNetworkStart()` has successfully activated the Cluster runtime and WiFi is already open under the normal SensorForge lifecycle. Therefore a freshly saved membership/ID/password/policy must be applied before other devices can discover it. Starting with Beta 48, `/cluster_save` schedules a controlled reboot after the persistent commit and shows a dedicated restart page; after reboot the normal WiFi startup owns the subsequent Cluster runtime start.

Known-Cluster credentials are cached in NVS namespace `sfclprof`. On a brand-new board that namespace legitimately does not exist yet. A read-only NVS open cannot distinguish this normal first-use state from a storage error. Beta 48 therefore creates/opens that namespace during an explicit join/save password lookup; an empty cache then means simply “no stored password”, while a real NVS open failure is still surfaced as storage unavailable.

Passive scanner rules are unchanged: while `/cluster` is open, a disabled local node may join the multicast receive group and listen for public `SFD1` announcements, but it does not transmit Cluster packets and never starts or extends WiFi. Only active Beta-38-or-newer members publish `SFD1`; older Beta-37 nodes are not visible to this passive dropdown even though they may implement earlier authenticated Cluster traffic.
