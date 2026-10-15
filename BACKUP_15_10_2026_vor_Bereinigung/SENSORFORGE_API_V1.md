# SensorForge Local API v1

Status: 2026-10-08  
Firmware worktree: **v87 Beta 34**  
Host protocol: **1.22**  
Integration profile: **1.1**

This document defines the local HTTP API used by SensorForge Sync clients,
Home Assistant-style integrations and the planned Android application.

The API remains deliberately inside the existing `/api/v1` namespace. The
1.22/1.1 expansion is additive: existing v1 routes and previously documented
fields remain valid.

## Security model

- `/api/v1` is administrator-only.
- Authentication uses HTTP Basic Auth with the existing SensorForge administrator
  Web credentials (`web_username` / `web_password`).
- Streaming-user accounts are not accepted for API control.
- API authentication remains required independently of the normal WebConfig
  authentication toggle.
- Transport is plain HTTP/Basic Auth and is therefore intended for a trusted or
  isolated local network.
- Passwords, SFSEC1 ciphertext, license hardware IDs, encryption keys and other
  secrets are never returned by API responses.

Every API response/challenge includes:

- `X-SensorForge-API-Version`
- `X-SensorForge-Device-ID`
- `X-SensorForge-Integration-API-Version: 1.1`

`device_id` remains the user-configurable hostname for compatibility.
`integration_id` from `/api/v1/device` is the preferred stable automation ID.
It is a random public 128-bit ID stored in NVS and is independent from MAC and
license identity.

## Compatibility rules

1. Existing v1 routes/fields are not renamed or removed inside API v1.
2. New fields/routes are additive.
3. The existing high-speed file transfer, HTTP Range/Resume and API-exclusive
   semantics remain unchanged.
4. The API does not introduce a second set of configuration limits. Persistent
   updates are passed through the existing SensorForge `configValidateText()` and
   `configSaveText()` path.
5. Generic configuration writes are **persist-only** and report
   `reboot_required=true`. They intentionally do not partially hot-apply camera,
   WiFi, sleep, streamer or ownership settings in the running process.
6. Runtime actions use the established SensorForge action/safety paths wherever
   available.

## Discovery and identity

### GET /api/v1/device

Existing device identity/capability endpoint. Important fields include:

- API protocol and integration-profile versions
- `integration_id`
- `device_id`
- product/platform/core identity
- release tag/date and firmware build timestamp
- board/camera information
- config source
- network/operating mode
- streamer configuration
- additive capability flags

### GET /api/v1/capabilities

Machine-oriented API feature discovery. It explicitly reports support for:

- persistent config read/partial write/schema
- snapshot/media retrieval
- media annotation read/write and safe media deletion
- chunked log read and controlled log clear
- manual recording start/stop
- shooter status/flush
- image-motion status/background reset
- radar status/config/calibration
- audio/network/streamer status
- network scan
- transport control
- config-storage management
- reboot/shutdown
- basic ONVIF interoperability capability (`onvif_basic`) and `onvif_enabled` state/configuration
- optional WireGuard client capability/backend/configured/active status

ONVIF itself is not transported through `/api/v1`: when enabled in streamer mode,
WS-Discovery and the read-only `/onvif/device_service`, `/onvif/media_service` and
`/onvif/imaging_service` SOAP endpoints expose the existing SensorForge network,
JPEG/RTSP media and a minimal read-only view of the existing camera exposure mode.
Device operations marked PRE_AUTH by the implemented ONVIF subset are available
before authentication; protected SOAP reads accept the existing SensorForge
credentials through HTTP Basic/Digest or WS-Security UsernameToken/PasswordDigest.
This does not change Integration profile 1.1 or the established media-transfer paths.

Field qualification status: v87 Beta 16 verified automatic WS-Discovery, protected
Device/Media SOAP reads, snapshot access and RTSP/JPEG playback on real XIAO
hardware with Android Onvier. Beta 17 centralized the published product identity.
Beta 18 extends only read-only interoperability: Device network/DNS/NTP/protocol
queries, singular source/encoder configuration reads and a minimal Imaging service.
It also adds SOAP diagnostics and `sensorforge_onvif_test.py` for repeatable
Discovery/Auth/Device/Media/Imaging/Snapshot/RTSP regression checks. The media
pipeline remains the field-qualified JPEG/RTSP implementation. This is not a claim
of official ONVIF certification or full Profile-T conformance.

H.264 remains disabled on the current XIAO and Freenove profiles. Separate board
and streamer implementation gates are present so a future board may advertise a
higher codec only after the hardware capability and a real qualified SensorForge
encoder/streamer implementation both exist; setting a board capability alone must
never make ONVIF advertise an unimplemented codec.

Firmware upload, license management and generic factory reset are intentionally
not exposed by this API expansion yet. Those operations remain on their existing
specialized WebConfig paths until their full safety/transaction semantics are
wrapped rather than duplicated.


## WireGuard client (Beta 24)

WireGuard remains an optional transport design below the existing SensorForge
services; it is not a new API namespace and it does not create another media
pipeline. Current XIAO and Freenove board profiles retain the prepared single-peer
configuration, but the authoritative Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 build
deliberately selects **no WireGuard runtime backend**.

Additive canonical config keys are retained for future continuation:

- `wireguard_enabled`
- `wireguard_address`
- `wireguard_private_key` (**secret; write-only through API semantics**)
- `wireguard_peer_endpoint`
- `wireguard_peer_public_key`
- `wireguard_peer_port`

The private key continues to use the existing SensorForge secret/SFSEC1 path. GET
config/status responses expose only configured/backend/runtime state and never key
material.

For Beta 24 the legacy `WireGuard-ESP32` 0.1.5 Arduino backend is explicitly not
linked because it still depends on removed `tcpip_adapter` APIs on the current
reference stack. Installing that library locally must not change or break a
SensorForge build. Runtime activation therefore remains unavailable and persisted
`wireguard_enabled` is forced OFF while no qualified backend is compiled in.

`GET /api/v1/network` continues to include a `wireguard` object with enabled/active
state, board capability, backend availability/name, tunnel address, endpoint/port,
configured-key flags and runtime status. `/device`, `/state` and `/capabilities`
retain their smaller additive WireGuard fields. Because backend availability is
false in the Beta-24 reference build, the effective `wireguard_client` capability
is false even though the current boards are architecturally allowed to support a
future backend. Host API remains 1.22.

Future implementation should resume with a maintained ESP-NETIF-compatible backend
or after a separately qualified Arduino-ESP32 4.x / ESP-IDF 6.x migration. Local
patching of third-party WireGuard source is not part of the supported SensorForge
workflow. In WebConfig the parked feature is now presented only inside `WiFi
Einstellungen` as a concise availability state; backend names, tunnel/key fields
and other implementation details are intentionally not exposed while no qualified
backend exists. RTSP/ONVIF/snapshot traffic over VPN remains unqualified until a
real backend exists and passes separate load, heap, reconnect and thermal testing.

## Fast state and telemetry

### GET /api/v1/state

Existing lightweight poll endpoint. It avoids SD directory scans and camera
capture. It reports recording, motion, storage, network, streamer, sleep and
exclusive state.

### GET /api/v1/status

Existing broader Sync-API status endpoint.

### GET /api/v1/system

Extended system telemetry:

- release/date/build
- uptime and reset reason
- free/minimum heap and PSRAM data
- CPU and RTC/enclosure temperature
- thermal state/source
- system-time validity
- RTC detection/type

### GET /api/v1/storage

Existing storage capacity/reserve/lock status.

### GET /api/v1/sensors

Existing presence/radar/RTC/temperature status.

## Persistent configuration API

### GET /api/v1/config

Returns the complete current persistent canonical SensorForge configuration,
filtered through the supported API field table.

Response layout:

- `values`: non-secret persistent fields as strings
- `secret_configured`: booleans indicating whether each secret is configured
- `write_semantics: "persist_then_reboot"`

Secret values themselves are never returned.

### GET /api/v1/config/schema

Returns every canonical configuration key currently known to the API with:

- key name
- logical group
- whether it is secret
- whether generic partial writes are allowed

The canonical set covers camera, recording, audio, shooter, image motion,
streamer (including additive `onvif_enabled`), sleep/power, transport tuning, storage policy, network/WiFi,
metadata, WebConfig/access and logging.

`transport_mode` is intentionally read-only in the generic config endpoint; use
the dedicated transport action below.

### POST /api/v1/config

Partial persistent configuration update using
`application/x-www-form-urlencoded` parameters.

Only supplied fields are changed. All other fields/comments/future keys are
preserved. The resulting complete config is validated before either persistent
copy is touched.

Examples:

```text
POST /api/v1/config
fps=4&quality=12&sleep_mode=deep_sleep
```

```text
POST /api/v1/config
wifi_alive_schedule_enabled=1&wifi_alive_schedule=12:10/15;16:37/8
```

Special request parameters:

- `_dry_run=1` — validate the requested patch without persisting it
- `_reboot=1` — after a successful save, schedule a delayed reboot

Successful normal writes return:

- `changed_keys`
- storage result (`internal_only` or `internal_and_sd`)
- `runtime_hot_applied=false`
- `reboot_required=true`
- optional `reboot_scheduled=true`

The explicit persist-then-reboot model is intentional. It avoids transient
mixed ownership/configuration states, especially for camera, streamer, WiFi,
audio and sleep settings.

### GET /api/v1/config/storage

Returns the internal/SD config-copy status and active source.

### POST /api/v1/config/storage

Supported actions:

- `action=copy_internal_to_sd`
- `action=delete_sd_copy`

The existing SensorForge config-copy validity rules remain authoritative.
Actions are rejected while recording or storage maintenance is active.

## Network and WiFi

### GET /api/v1/network

Returns:

- active network mode/IP
- STA connection/SSID/RSSI
- AP activity/client count
- hostname
- WiFi inactivity timeout
- daily WiFi-alive schedule
- configured TX power
- board-default TX power
- explicit indication that automatic fallback AP uses full board-default power
- hotspot/fallback settings
- all five prioritized SSIDs and password-configured booleans

No WiFi/hotspot password is returned.

### GET /api/v1/network/scan

Uses the same on-demand scan implementation as WebConfig.

For production safety the API rejects a scan while:

- a recording is active, or
- an RTSP/HTTP streamer client is currently connected.

## Camera and media retrieval

### GET /api/v1/camera/snapshot

Existing JPEG snapshot route using the established camera/streamer-safe capture
path.

### GET /api/v1/days
### GET /api/v1/files?day=YYYYMMDD
### GET /api/v1/file?path=/YYYYMMDD/file.ext

Existing finalized-media browsing/download API. It retains:

- final-file-only path validation
- HTTP Range/Resume
- direct SD-to-TCP streaming
- recording-priority behaviour outside API-exclusive mode
- encryption-transparent reads through the established storage layer

The existing Python sync client remains compatible and does not depend on the
new control endpoints.

### GET /api/v1/media/annotation?path=/YYYYMMDD/file.avi
### POST /api/v1/media/annotation

Reads or writes the existing recording annotation sidecar through the same
normalization, encryption and storage primitives as the Web player. POST uses:

- `path` — AVI/MKV recording path
- `text` — annotation text; empty text clears the annotation

Annotation access is rejected while the relevant storage path is busy.

### POST /api/v1/media/delete

Deletes one finalized AVI/MKV/JPEG recording through the existing player/storage
safety model. Parameter: `path`. If a recording is already active, deletion is
queued through the existing bounded deferred-delete queue rather than changing
the SD directory underneath the recorder. AVI subtitle and annotation sidecars
are removed together with the media.

## Log access

### GET /api/v1/log

Reads the logical plaintext SensorForge log in bounded chunks, including when
the physical log generation is SFLOG1 encrypted. Optional parameters:

- `cursor` — continuation cursor returned by the previous response
- `limit` — requested plaintext bytes; clamped to 256..16384 bytes
- `flush=1` — flush pending logger data before reading

The response includes `next_cursor`, `more`, `generation`, encryption/reset
state and torn-record recovery state. Existing recording/storage-priority gates
remain authoritative.

### POST /api/v1/log/clear

Clears the active log through the existing `logClear()` transaction. It is
rejected while recording/storage priority blocks safe access.

## Recording control

### GET /api/v1/recording

Detailed runtime/config summary including:

- recording/recorder state and health
- active final path, format, frame/byte counts
- last recorder error
- start/storage locks
- motion recording configuration
- current automation-motion result
- safety cooldown
- `recording_not_before` state/deadline/remaining time

### POST /api/v1/recording/start

Manual recording start through the existing `startRecording()` path.
The API does not bypass existing ownership/safety rules. It is rejected when,
for example, streamer mode owns the camera, storage is locked, another recorder
operation owns the writer, recording starts are blocked, or another existing
SensorForge start gate rejects the request.

### POST /api/v1/recording/stop

Stops a normal active recording through the existing `stopRecording()` path.
It will not hijack another recorder owner such as a diagnostic/load-test path.

## Continuous shooter

### GET /api/v1/shooter

Existing shooter configuration and RAM-buffer telemetry.

### POST /api/v1/shooter/flush

Existing explicit safe flush request.

Shooter configuration itself can now be changed persistently through
`POST /api/v1/config` using the normal canonical shooter keys.

## Image Motion

### GET /api/v1/image-motion

Returns the current image-motion configuration plus the existing detailed
runtime diagnostics object and diagnostic-ring occupancy.

### POST /api/v1/image-motion/background/reset

Forgets the learned background using the existing
`imageMotionResetBackground()` operation.

Image-motion tuning and ROI mask are writable through the persistent config API.

## Radar / LD2410S

### GET /api/v1/radar

Returns:

- detection/tracking state
- current motion state/remaining hold
- last motion gate/energy
- target state/distance
- cached radar configuration where available
- trigger/hold arrays
- current calibration mode/sample counts

During recording the API will not force a new radar configuration-mode UART
read merely because no cached settings are available.

### POST /api/v1/radar/config

Partial update of LD2410S settings. Unspecified fields retain their current
cached/read value. Supported parameters are:

- `min_gate`
- `max_gate`
- `absence_sec`
- `status_rate_x10`
- `distance_rate_x10`
- `response_speed`
- `trigger_0` .. `trigger_15`
- `hold_0` .. `hold_15`

The existing `radarValidateSettings()` and `radarWriteSettings()` verification
path remains authoritative. If a recording was already active, the existing
post-transaction radar motion hold is retained so the UART transaction does not
artificially terminate the event.

### POST /api/v1/radar/calibration

Parameter `action`:

- `start_quiet`
- `start_motion`
- `stop`
- `reset_quiet`
- `reset_motion`
- `reset_all`

The existing radar calibration engine remains the owner of the measurement and
sensor-range restoration.

## Audio

### GET /api/v1/audio

Returns:

- configured enable state
- configured-input validity/error
- hardware/backend availability
- supported sample-rate/bit-depth/channel capabilities
- current capture running state/format
- capture/ring statistics and drop counters

Audio configuration is writable through `POST /api/v1/config`.

## Streamer

### GET /api/v1/streamer

Detailed streamer telemetry:

- mode/ready state
- configured RTSP/HTTP outputs
- RTSP/HTTP client counts
- audio availability/activity/status
- measured FPS
- capture/send counters and total bytes
- RTSP/HTTP/viewer URLs
- last streamer error

Streamer configuration is persisted through `POST /api/v1/config` and takes
effect after reboot, preserving the existing clean ownership transition.

## Transport mode

### POST /api/v1/transport

Parameter:

- `enabled=1` — persist transport mode and reboot
- `enabled=0` — cancel transport mode and reboot

This uses the existing `configSaveTransportMode()` reconciliation semantics and
is rejected during active recording/storage maintenance.

Transport timing/black-threshold tuning remains available through the generic
config endpoint.

## System actions

### POST /api/v1/system/reboot

Schedules the same delayed restart lifecycle used by WebConfig. It is rejected
while a recording is active so a media file is not cut off.

### POST /api/v1/system/shutdown

Schedules the same software-off/deep-sleep shutdown path used by WebConfig and
blocks new recording starts during the short HTTP grace period. Active recording
causes HTTP 409 rather than forced termination.

## API-exclusive transfer mode

### GET /api/v1/exclusive
### POST /api/v1/exclusive

Existing 60-second RAM-only lease behaviour is unchanged. It is intended for
high-speed sync/download activity and prevents a new recording from pre-empting
the protected transfer. Vanished clients cannot leave the device permanently
exclusive.

## Diagnostic transfer tests

Existing diagnostic routes remain unchanged:

- `GET /api/v1/test_sd_read`
- `GET /api/v1/test_sd_spi`
- `GET /api/v1/test_wifi`

## Error model

Control/config routes use JSON errors in the established form:

```json
{"ok":false,"error":"busy","message":"recording_active"}
```

Typical status classes:

- `400` malformed/invalid parameter or failed configuration validation
- `401` authentication required
- `409` valid request currently blocked by an operational/safety condition
- `404` requested media/config object not found
- `500` persistent write/hardware operation failed
- `503` required subsystem/resource currently unavailable

Clients should treat the machine-readable `error` and `message` fields as the
primary diagnostic information and should not depend on translated WebConfig UI
text.

## Android application guidance

The planned Android application should use API discovery rather than hard-code
firmware assumptions:

1. authenticate and read `/api/v1/device`
2. read `/api/v1/capabilities`
3. cache `/api/v1/config/schema` per API version
4. poll `/api/v1/state` for fast UI state
5. request heavier endpoints only on the relevant screen
6. use partial config writes and explicitly reboot when required
7. use API-exclusive mode around long bulk download sessions
8. never assume a password can be read back; only `*_configured` state is
   available

The API intentionally keeps high-risk firmware upload, license provisioning and
other specialized destructive maintenance outside this first broad application
control expansion. Those functions should only be added later by wrapping their
existing transactional/safety implementation, not by duplicating it.
