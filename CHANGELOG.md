# SensorForge Changelog

This file tracks the project release number used by `sensorforge_version.h`.
For each release, commit the complete project tree and create a Git tag with the
same name (`v35`, `v36`, ...). The firmware build timestamp remains independent
and identifies the concrete binary compilation time.

> Historical release notes below are reconstructed from the maintained project
> change sequence beginning with v23. Earlier development history remains in the
> repository history/project documentation.

## v35 — 2026-09-23

- Added central `sensorforge_version.h` release identity.
- Release tag/date are shown in WebConfig and written to the firmware boot log.
- Added this changelog and documented the Git tag workflow.
- Functional baseline includes v34, v33, v32 and v31 below.

## v34 — 2026-09-23

- Power Shooter logging follows the shooter media batching rhythm.
- Logger uses a larger PSRAM buffer while shooter batching is active so normal
  log traffic does not wake the SD card between long shooter flush intervals.
- Shooter media flush remains the preferred point for persisting buffered logs.
- Safety/reboot/shutdown/manual log-view paths can still force persistence.

## v33 — 2026-09-23

- Made SFLOG1 writer record boundaries line-safe where possible.
- Reader inserts a separator after torn-record recovery/resynchronization so
  surviving fragments are not misleadingly glued into one log line.
- Existing physically lost log bytes cannot be reconstructed.

## v32 — 2026-09-23

- Fixed recording-list start-time parsing for Power Shooter Sparse-MKV names
  such as `083434_500_shooter.mkv`.
- Shooter MKVs can therefore display start/end time ranges using their actual
  container duration.

## v31 — 2026-09-23

- Restored the intended recording-mode UI:
  - Off
  - Normal motion/alarm recording
  - Power Shooter standalone
  - Normal recording + Power Shooter
- Kept the existing persistent keys `motion_recording_enabled` and
  `shooter_enabled`; no incompatible replacement config key was introduced.
- Restored the shooter darkness slider with live grayscale swatch.
- Added FPS examples/live interpretation for shooter interval values.

## v30 — 2026-09-23

- Restored Power Shooter configuration controls in WebConfig.
- Fixed the config-save path so all shooter settings and
  `motion_recording_enabled` are retained instead of silently falling back to
  defaults.
- Removed the obsolete periodic-snapshot configuration block.

## v29 — 2026-09-23

- Fixed filtered selection in the recordings list.
- Select-all/delete-selected no longer includes hidden media from another
  filter (for example hidden videos while the Images filter is active).

## v28 — 2026-09-23

- Day ZIP downloads skip unreadable/corrupt encrypted recordings instead of
  aborting the complete archive.
- Added `_SENSORFORGE_SKIPPED_CORRUPT.txt` report inside affected archives.
- Added single-day-ZIP download guarding/UI serialization.

## v27 — 2026-09-23

- Progressive day-list rendering after directory enumeration/sort.
- Added visible-row range selection and Shift-click selection behavior.

## v26 — 2026-09-23

- Hardened WebConfig automatic recording pause/lease behavior.
- Prevented new continuous-shooter captures while the WebConfig recording pause
  is active, while retaining already buffered shooter frames for later flush.

## v25 — 2026-09-23

- Display annotations directly in recording lists.
- Increased queued-delete capacity and accelerated queued deletion after
  recording activity ends.

## v24 — 2026-09-23

- Added recording selection/delete UI and annotations.
- Added corrupt/non-decryptable video warning indication.
- Increased WebPlayer transfer/batch sizes for better playback throughput.

## v23 — 2026-09-23

- Added media icons and recording start/end time ranges in lists.
- Reworked day-ZIP logical-size/decryption preflight.
