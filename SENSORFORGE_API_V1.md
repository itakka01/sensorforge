# SensorForge Integration API 1.0

Status: 2026-10-02

This document defines the stable local integration profile built on the existing
SensorForge `/api/v1` HTTP API. It is intended for Home Assistant and other
local automation clients.

## Compatibility contract

- Existing Sync API routes remain unchanged.
- The existing `/api/v1` namespace is retained.
- Protocol version is 1.12; the stable integration profile is 1.0.
- Changes inside API v1 should be additive. Existing documented fields/routes
  must not be renamed or removed without a new major API version.
- Existing camera configuration and stored media are not migrated by this API.
- API 1.0 does not persistently change shooter or motion-recording settings.
- Authentication remains the established SensorForge API HTTP Basic Auth using
  the configured **administrator** Web credentials. v86 streaming-user accounts
  are intentionally not accepted for `/api/v1` control/API access.
- API responses never expose license hardware IDs, Wi-Fi passwords, web
  passwords, hotspot passwords, encryption keys or other secrets.

Every API response/challenge includes:

- `X-SensorForge-API-Version`
- `X-SensorForge-Device-ID`
- `X-SensorForge-Integration-API-Version: 1.0`

`device_id` remains the user-configurable hostname for backwards compatibility.
For automation identity, clients should prefer `integration_id` from `/device`.
It is a random 128-bit public identifier stored in NVS and is independent of the
license hardware ID and MAC address.

## Integration API 1.0 routes

### GET /api/v1/device

Relatively static device identity and capability discovery.

Important fields:

- `integration_api_version`
- `integration_id`
- `device_id`
- `app`
- `platform`
- `core_version`
- `firmware_build`
- `board`
- `camera_config`
- `camera_initialized`
- `camera_pid`
- `config_source`
- `network_mode`
- `operating_mode`
- `streamer_rtsp_enabled`
- `streamer_http_mjpeg_enabled`
- `capabilities`

Capabilities advertise local snapshot, motion state, image-motion state, motion
recording, continuous shooter, shooter flush, storage/sensor status, media
browsing and RTSP support. `rtsp` describes firmware capability; the additive
`streamer_rtsp_enabled` field describes the current persisted configuration.
`mqtt` remains false in API 1.0.

### GET /api/v1/state

Fast operational state intended for regular polling. It deliberately avoids SD
directory scans and camera capture.

Important fields include:

- uptime
- network mode and active IP
- camera/storage readiness
- recording/recorder state
- recording-start block state
- motion-recording enable/decision mode
- current operational motion result
- image-motion state
- recording safety cooldown
- shooter enable state
- API-exclusive state
- operating mode and streamer ready/client/audio state
- configured sleep mode/delay

### GET /api/v1/shooter

Continuous-shooter configuration summary and runtime telemetry:

- enabled/storage format/interval
- darkness and minimum-change persistence filters
- passive motion-hint threshold plus required-hits/window settings
- force-save and flush intervals
- PSRAM buffer frames/bytes/capacity/fill percentage
- accepted JPEG count/bytes/average size
- rejected-dark and rejected-similar counters
- latest measured mean/peak brightness and age

### POST /api/v1/shooter/flush

Requests immediate persistence of the current shooter RAM buffer through the
existing safe flush path. It does not discard frames on a busy condition.

Typical busy/error conditions use existing JSON error semantics and HTTP 409 or
503, for example active recording, locked storage, API exclusive mode or
unavailable storage.

## Existing routes retained unchanged

- `GET /api/v1/status`
- `GET /api/v1/storage`
- `GET /api/v1/sensors`
- `GET /api/v1/camera/snapshot`
- `GET /api/v1/days`
- `GET /api/v1/files?day=YYYYMMDD`
- `GET /api/v1/file?path=...`
- `GET/POST /api/v1/exclusive`
- existing diagnostic test routes

The current Python sync client continues to use the existing routes and does not
depend on the new Integration API 1.0 fields.

## Current network behaviour

The WebConfig/API service follows the existing SensorForge network selection. It
can run on the configured local hotspot or on the prioritized infrastructure-WiFi
profiles; when enabled, the automatic hotspot fallback remains available if none
of the configured external WLANs can be reached. Integration API 1.0 does not
change that network, sleep or wake policy; it uses whichever supported service
network SensorForge currently provides.

## v86+ media-access note

The v86 role split and the v87 Beta 1 WiFi/UI additions do not change Integration API 1.0. When SensorForge web
access protection is enabled, administrator and streaming-user credentials may
be used for the direct media paths such as `/snapshot`, HTTP-MJPEG on port 81
and RTSP. The `/api/v1` namespace remains administrator-only.

For a simple Home Assistant camera without API control, the documented media
path is the MJPEG stream `http://<host>:81/stream` plus the still-image URL
`http://<host>/snapshot`, with username/password supplied in the Home Assistant
integration fields.

## Intended Home Assistant mapping

The first Home Assistant custom integration can be built read-mostly from:

- `/device` for setup/capabilities and unique identity
- `/state` for fast operational entities
- `/storage` for SD capacity/reserve health
- `/sensors` for presence/radar/RTC/temperature data
- `/shooter` for shooter telemetry
- `/camera/snapshot` for the camera entity
- `/shooter/flush` for an explicit flush button

Persistent control of configuration switches should be added later through
explicit API actions rather than by editing `config.txt` from Home Assistant.

## Planned Android/application API expansion (not part of API 1.0)

The planned Android client needs a substantially broader control surface than the
current read-mostly integration profile. This is a roadmap, not an implemented or
stable API contract. The preferred direction is additive `/api/v1` expansion with
capability discovery and the existing administrator authorization model.

Coverage should be brought close to the meaningful WebConfig/device functions,
including at least:

- read/write configuration in typed groups instead of raw `config.txt` editing
- recording state/control and recording-related policy
- continuous shooter configuration/control in addition to the existing status/flush
- image-motion configuration, ROI/mask/background-learning actions and diagnostics
- camera and advanced-camera settings plus safe preview/snapshot actions
- audio configuration/status where supported
- WiFi profiles, hotspot/fallback, TX power and WiFi-alive schedule management
- streamer mode/configuration/status and stream endpoint discovery
- storage/media maintenance operations with the same safety gates used by WebConfig
- log/diagnostic access with bounded payloads
- reboot/shutdown and other explicit system actions
- user/access management only if a dedicated secure API contract is defined for it
- firmware/update status; firmware upload itself should remain separately gated and
  should not be exposed casually as a generic configuration action

Existing `/api/v1` fields and routes should remain backward compatible. New write
actions should validate exactly the same constraints and safety/ownership rules as
the corresponding WebConfig paths rather than introducing a second independent
configuration logic.
