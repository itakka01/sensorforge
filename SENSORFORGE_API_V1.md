# SensorForge Integration API 1.0

Status: 2026-09-22

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
  the configured Web credentials.
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
- `capabilities`

Capabilities currently advertise local snapshot, motion state, image-motion
state, motion recording, continuous shooter, shooter flush, storage/sensor
status and media browsing. `rtsp` and `mqtt` are explicitly false in API 1.0.

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
- configured sleep mode/delay

### GET /api/v1/shooter

Continuous-shooter configuration summary and runtime telemetry:

- enabled/storage format/interval
- darkness and minimum-change filters
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

## Current networking limitation

The present SensorForge WebConfig/API server runs in the established local AP
mode. Integration API 1.0 intentionally does not change Wi-Fi, sleep or boot
behaviour. A later Connected profile can add infrastructure-Wi-Fi operation and
Zeroconf discovery without changing this API contract.

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
