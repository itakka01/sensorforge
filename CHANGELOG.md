# SensorForge Changelog

This file tracks the project release number used by `sensorforge_version.h`.
For each release, commit the complete project tree and create a Git tag with the
same name (`v35`, `v36`, ...). The firmware build timestamp remains independent
and identifies the concrete binary compilation time.

> Historical release notes below are reconstructed from the maintained project
> change sequence beginning with v23. Earlier development history remains in the
> repository history/project documentation.

## v39 — 2026-09-23

- SD Maintenance now exposes fixed, visible controls for status refresh, read-only
  SD recovery and the SD benchmark on the unified page.
- The Recovery action remains visible even while the SD is mounted. In that state
  it reports that recovery is not required instead of silently hiding the action;
  an actual remount recovery still runs only when the SD is unavailable.
- SD Recovery is now represented on all storage backends: XIAO/SPI keeps the
  detailed read-only raw/sector/multi-clock recovery, while SD_MMC uses the
  existing robust mount/retry recovery path without format or wipe.
- Recovery and benchmark actions remain visibly present but are disabled while a
  recording is active.
- Removed the obsolete standalone `/sdstatus`, `/sd_recovery` and `/sdbench` GET
  routes; SD tools are now reached only through the unified SD Maintenance page
  and its dedicated POST action endpoints.

## v38 — 2026-09-23

- Consolidated SD Status, SD Recovery and SD Benchmark into the existing
  SD Maintenance page and reduced the System menu to one SD maintenance entry.
- SD Maintenance now shows capacity/readiness first, SPI read-only recovery when
  applicable, and the existing 1 MiB write benchmark on the same page.
- Existing Wipe, Format and Secure Erase workflows remain on the same page with
  their established safety checks and config-preservation behavior unchanged.
- Legacy `/sdstatus` and `/sd_recovery` URLs remain compatible by redirecting to
  the corresponding section of SD Maintenance; `/sdbench` returns the unified
  page with its benchmark result.

## v37 — 2026-09-23

- Logger RAM batching now has an independent maximum persistence age. Normal
  logging is forced to storage after at most five minutes; while Power Shooter
  batching is configured, the logger uses the configured shooter flush window.
  This prevents WebConfig pause/idle periods from leaving rare log events only
  in PSRAM indefinitely.
- The main loop services the logger deadline only while SD storage is safe to
  use (no active recorder, storage lock or Sync API exclusive operation), while
  successful Power Shooter media flushes remain the preferred co-flush point.
- SFLOG1 now rolls a detected partial physical record write back to the previous
  authenticated file boundary using the existing crash-recoverable temp/backup
  transaction files before a retry can append behind the torn record.
- On encrypted logger open, a bounded tail scan authenticates only the last few
  possible records and removes a torn/authentication-damaged terminal tail
  before new records are appended. Existing mid-file recovery gaps with later
  valid records are preserved for the v36 reader instead of discarding history.
- The persistent boot config snapshot now includes the effective recording
  encryption, Power Shooter and motion-recording settings so config-source
  mismatches can be diagnosed from the downloaded log without exposing secrets.

## v36 — 2026-09-23

- SFLOG1 reader now preserves authenticated log data when the final encrypted
  record is complete-looking but fails authentication; the damaged terminal
  record is skipped and unauthenticated bytes are never exposed.
- Log Viewer surfaces recovery as a customer-facing warning instead of a raw
  system-style failure and retains technical details separately for diagnosis.
- Log Viewer fetch errors now include the server-provided diagnostic detail
  instead of reducing every failure to a bare `HTTP 500`.

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
