# SensorForge

SensorForge is an ESP32-S3 camera/sensor firmware for autonomous event recording
and optional local network streaming.

**Current development worktree:** `v87-beta50` (2026-10-08)  
**Last stable official release:** `v86` (2026-10-01)  
**Core branding version:** `7.1.0`  
**Local API:** `1.22` / Integration profile `1.1`

## Supported board profiles

- Seeed XIAO ESP32S3 Sense
- Freenove FNK0085 ESP32-S3 WROOM

Board selection is compile-time in `board_config.h`. The checked-in source
currently selects the XIAO profile; Freenove remains an alternative supported
profile.

## Main capabilities

- sensor-triggered/event recording to SD
- MJPEG/AVI and Matroska-related recording paths
- PIR, radar and image-motion detection
- Power Shooter / sparse image workflows
- RTSP/RTP-JPEG network streaming
- HTTP-MJPEG and snapshot output
- optional audio on supported hardware
- ONVIF discovery and read-only Device/Media/Imaging interoperability layer
- WebConfig and WebPlayer
- local administrator API `/api/v1`
- WiFi / hotspot / fallback / multi-WiFi handling
- light/deep-sleep integration and wake scheduling
- SD diagnostics, recovery and maintenance
- optional hardware-backed recording encryption
- signed `.sfw` SD/WiFi firmware updates
- board-bound offline license activation
- local TRIAL / DEMO usage policy
- optional authenticated local device cluster discovery/status and resource announcements

WiFi provisioning safety in Beta 42: infrastructure/STA mode cannot be saved without at least one SSID profile. Historical configs that already contain STA mode with zero SSIDs boot into the local recovery hotspot; an explicit `Offline bleiben` setting remains authoritative when real STA profiles exist.

Beta 44 changes only WiFi WebConfig presentation: the displayed hotspot-name preview follows edits to the device hostname immediately, before Save. The persisted/runtime hotspot behavior remains unchanged.


Beta 46 makes WiFi configuration boot-consistent: after a verified save, SensorForge shows a reconnect/restart notice and performs a controlled reboot so hostname, STA/AP mode, WiFi profiles, fallback policy and TX power are applied together by the existing network startup path. This prevents the previous mixed state where new runtime values coexisted with the old active interface until the inactivity timeout shut it down.

Beta 45 hardens the shared atomic configuration writer/recovery after the no-SD first-run persistence fault. A valid orphaned `/config.tmp` from an interrupted first commit is recovered instead of discarded, and a save is not reported successful until the final `/config.txt` path is reopened, byte-compared and validated.

H.264 is not implemented on the current XIAO/Freenove profiles. WireGuard/VPN is
architecturally prepared but has no qualified runtime backend in the current
reference build.


## Current Cluster join validation

`v87-beta50` validates credentials before joining a Cluster that is currently visible in passive discovery. While `/cluster` is open, SensorForge keeps a small RAM-only copy of recent signed `SFC1` Presence traffic. On Save, the candidate password is used locally to verify that HMAC. A wrong password therefore produces an immediate error, does not alter persistent configuration and does not reboot the device. No password, candidate key or new challenge packet is transmitted. If only the public `SFD1` announcement has arrived but no signed Presence proof is cached yet, Save is blocked until signed traffic arrives. Manual creation of a new/offline Cluster remains allowed.

`v87-beta49` is a compile-only follow-up to Beta 48: `webconfig.cpp` now includes `cluster.h` so the Cluster restart page can resolve `clusterEffectiveId()`. Runtime behavior is unchanged.

`v87-beta48` fixes two first-use Cluster issues. A brand-new device no longer mistakes a not-yet-created Known-Cluster NVS namespace for failed storage when resolving a saved password. Cluster saves are now boot-consistent: after a verified save SensorForge shows a restart notice and performs a controlled reboot so the saved membership/credentials/policy become the active Cluster runtime. Passive discovery still sends nothing from a disabled device; it only listens while `/cluster` is open. A remote Cluster appears in the dropdown only when at least one remote member is actually running the Cluster runtime (Beta 38 or newer) on the same local WiFi.

## Current no-SD boot fix

`v87-beta47` fixes a hardware-observed `loopTask` stack-canary reboot immediately after the expected SD retry failure on a board with no SD card. Missing SD remains supported. Large temporary `ConfigValues` objects used by config validation/default/recovery are no longer nested on the Arduino loop-task stack; they use short-lived heap storage instead. Config schema, SD > LittleFS > defaults priority and WiFi behavior are unchanged.

## Current beta persistence fix

`v87-beta45` hardens the shared atomic config writer/recovery. On a first internal save, a complete validated `/config.tmp` is now recoverable after an interrupted commit instead of being discarded when `/config.txt` does not yet exist. A save is reported successful only after the final `/config.txt` path itself has been reopened, byte-compared and validated. This specifically protects fresh/no-SD provisioning while preserving the existing SD > internal > defaults priority.

## Product modes

SensorForge currently distinguishes:

- **TRIAL** — factory/default state, full functionality
- **DEMO** — fallback after trial usage is exhausted
- **FULL** — permanent purchased activation
- **SERVICE** — service edition

Automatic local trial limits:

- 14 days active/trusted-calendar use, or
- 20 h accumulated recording, or
- 100 h accumulated active streaming

DEMO currently permits 10 recording starts/day, 1 h recording/day and 1 h active
streaming/day. Radar configuration/calibration and local management diagnostics
remain available.

## License architecture

Board-bound activation uses ECDSA P-256 / SHA-256.

- `SF2` is the current keyring-capable activation format.
- The firmware stores only public issuer keys.
- Issuer key capabilities are checked in addition to the signature.
- Future online TRIAL signing can therefore be cryptographically restricted to
  EVALUATION licenses.
- FULL/SERVICE authority and recovery private keys are intended to remain offline
  or later inside non-exportable HSM/KMS infrastructure.
- Firmware signing uses a separate keypair/trust domain.

The installed activation code is stored internally in LittleFS (`/license.dat`).
Normal signed OTA updates replace the application partition and do not erase this
license file.

The checked-in beta keyring is development-only. Production keys must be generated
on a trusted/offline system before commercial release.

## Firmware update security

SensorForge SD and WiFi update paths accept signed `.sfw` packages using a
dedicated firmware ECDSA P-256 / SHA-256 trust anchor.

Relevant tool:

- `sensorforge_firmware_sign.py`

Secure Boot / Flash Encryption / anti-rollback remain separate future hardening
projects and are not silently enabled by normal firmware or license work.

## Configuration persistence

Configuration priority remains **valid SD `/config.txt` → valid internal LittleFS shadow → firmware defaults**. A freshly flashed board may therefore boot and expose WebConfig without an SD card. From Beta 40, the first WiFi-settings save on such a board creates the initial complete internal `/config.txt` from the canonical factory-default serializer and then applies the requested WiFi changes through the normal validation/SFSEC1 save path. It does not create an SD config. Recording/storage features still require the corresponding storage hardware.

Beta 43 hardens the internal LittleFS persistence boundary. A failed mount is no longer treated as permission to format arbitrary existing flash data. SensorForge retries a normal mount and may auto-format only when the exact filesystem partition is verifiably blank/erased. If non-blank flash cannot be mounted, its contents are preserved and `/config` reports an exact internal-shadow status/detail for diagnosis instead of silently erasing the previous configuration.

From Beta 41, the factory/default WiFi fallback is enabled: when infrastructure WiFi is selected and none of the configured networks can be reached, SensorForge falls back to its own hotspot. Existing persisted configs keep their explicitly stored fallback value.

## WebConfig modules

Large WebConfig functions are progressively kept in focused modules without
changing their established URLs/config semantics:

- `webconfig.cpp`
- `webconfig_wifi.cpp/.h`
- `webconfig_streamer.cpp/.h`
- `webconfig_audio.cpp/.h`
- `webconfig_transport.cpp/.h`
- `webconfig_cluster.cpp/.h`
- `cluster_profiles.cpp/.h` (board-local encrypted known-Cluster cache)
- `webconfig_wireguard.cpp/.h`
- `web_sd_maintenance.cpp/.h`
- `web_log.cpp/.h`

Transport Security was moved into its own module in `v87-beta33`; this was a
structural split only. Cluster WebConfig is likewise isolated in its own module
from `v87-beta34` onward so `webconfig.cpp` remains orchestration-focused.

## Integration identity

Do not mix the three device identifiers:

- `device_id` — user-configurable network/hostname identity
- `integration_id` — stable random 128-bit NVS ID for automation/integration
- license hardware ID — board-bound ID derived from the factory/eFuse MAC

The existing `integration_id` is also the node identity for the local cluster.
Its implementation lives in `device_identity.cpp/.h` so API and cluster use the
same persisted NVS identity.

## Local device cluster

Detailed re-entry/architecture reference: `CLUSTER_ARCHITECTURE.md`. Beta 38 is
the current Cluster foundation freeze point; further Cluster feature work should
resume only with the concrete time-sync / scheduled-capture use case and from the
then-current complete source tree. A full real multi-device hardware/performance
qualification of the Cluster foundation is still required.

The optional cluster foundation runs strictly inside the existing SensorForge
WiFi lifecycle. It is disabled by default and must never start, prolong or keep
WiFi alive. Recording, streaming, sleep and camera ownership remain authoritative.

- membership uses a stable public `cluster_id`, a human-readable cluster name and a shared cluster password
- `cluster_credential_epoch` identifies the credential generation without exposing a password verifier
- the active password is stored through the existing SFSEC1 config-secret protection
- up to six successfully used Cluster credentials are remembered board-locally in NVS; cached passwords are also SFSEC1-encrypted and are never sent to the browser
- the existing central mDNS responder advertises `_sfcluster._udp`
- authenticated HMAC-SHA256 multicast presence provides self-learning peer status
- presence is sent every 10 s; authenticated peers use a 60 s lease
- orderly WiFi shutdown sends a signed best-effort `LEAVE`; unexpected loss falls back to lease expiry
- each node may advertise an approximate `wifi_remaining_sec` as status only
- only peers with the same cluster credentials are accepted into the volatile peer table
- Coordinator policy is `auto` (default), `preferred` or `node` (never Coordinator)
- actual Coordinator selection is automatic: preferred candidates outrank auto candidates; stable `integration_id` ordering breaks ties
- the active Coordinator has a runtime epoch/generation; this is not a user-forced permanent master role
- every non-Coordinator sends a signed unicast liveness heartbeat to the current Coordinator every 10 s on UDP 39428 and receives an authenticated ACK
- the Coordinator uses these direct heartbeats to refresh node liveness even if a multicast presence packet is missed
- WebConfig `/cluster` shows the complete currently visible cluster, selected Coordinator, runtime roles, policy and direct-link age
- while `/cluster` is actively open, a temporary receive-only discovery listener may run even when Cluster membership is disabled; it never starts/extends WiFi and sends no SensorForge Cluster packet
- active members publish a tiny public `SFD1` discovery announcement (Cluster ID/name/credential epoch/node metadata only) so the page can offer a drop-down of visible Clusters
- known offline Clusters remain selectable from the local encrypted credential cache; a credential-epoch mismatch disables automatic password reuse and requires re-entry
- generic resource announcements can advertise resource ID/type, microsecond timestamp, size, TTL and a compact locator
- resource bytes themselves are never multicast
- the current shared-password/HMAC design is a trusted-local-LAN model, not PAKE/PKI; captured authenticated traffic permits offline password guessing, so Cluster secrets should be high-entropy and the trust model must be reviewed before commercial security claims

The existing Beta-34/35 `SFC1` presence format remains compatible; lease/LEAVE,
resource and coordination metadata are additive. Beta 38 additionally separates
Cluster identity from credentials and adds passive discovery/known-Cluster caching.
Cluster-wide password rotation is intentionally not half-implemented: a healthy
multi-node Cluster rejects local same-ID password replacement. Future rotation is
a Coordinator prepare/ACK/commit transaction; offline nodes that miss it must
re-enter the password after seeing the newer credential epoch. Beta 38 also intentionally binds HMAC/tag key derivation to the stable `cluster_id + password` trust domain. All members of a Cluster must therefore run Beta 38 together; mixed Beta-37/Beta-38 membership is not supported. Beta-37 configs upgraded together derive the same deterministic legacy Cluster ID from their existing Cluster name. There is still deliberately **no clock correction,
Time-Master/laser sync, remote command/control, synchronized recording or
automatic peer file download**. A later production-session layer must bind
commands to the selected Coordinator ID plus its epoch and to an explicit session,
rather than accepting unauthenticated or stale broadcast commands.

## Documentation

Authoritative project/re-entry document:

- `00_PROJEKTBESCHREIBUNG_WIEDEREINSTIEG.txt`

Release history:

- `CHANGELOG.md`

Local API contract:

- `SENSORFORGE_API_V1.md`

Specialist documents include:

- `00_STROMVERBRAUCH.txt`
- `00_SYNC_CAMS_BESCHREIBUNG.txt`
- `00_SYNC_SENDER_BESCHREIBUNG.txt`
- `CLUSTER_ARCHITECTURE.md`
- `FIRMWARE_SIGNING.txt`
- `README_Radar_Kalibrierung.txt`

## Development rules

- Always work from the complete current source tree; never reconstruct or guess code from memory or an older delta ZIP.
- Read the actual affected implementation and its ownership/lifecycle paths before changing it.
- Safety first: preserve established behavior unless a requested change explicitly requires otherwise. Never remove existing functionality as incidental cleanup.
- Prefer additive, narrowly scoped changes and keep recording/storage/recovery/security ownership paths authoritative.
- Add configuration keys compatibly with safe defaults so older `config.txt` files remain valid without migration.
- Keep `webconfig.cpp` as slim and orchestration-focused as technically practical. New self-contained WebConfig functionality should normally live in a dedicated `.cpp/.h` module.
- Do not expose secrets through Web/API/log diagnostics.
- Deliver every code change as one delta ZIP containing only changed/new project files under their original filenames and original source-tree placement.
- Do not include renamed copies, patch/diff files, test plans or unrelated helper files in code ZIPs.
