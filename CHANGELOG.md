# SensorForge Changelog

This file tracks the project release number used by `sensorforge_version.h`.
For each release, commit the complete project tree and create a Git tag with the
same name (`v35`, `v36`, ...). The firmware build timestamp remains independent
and identifies the concrete binary compilation time.

> Historical release notes below are reconstructed from the maintained project
> change sequence beginning with v23. Earlier development history remains in the
> repository history/project documentation.

## v63 — 2026-09-26

- Isolated explicit Recording Load Test ownership from WebConfig automatic
  recording-pause recovery. The benchmark intentionally keeps the normal
  high-level `recording` flag false while owning an open recorder; a resumed
  browser session must therefore no longer classify that recorder as stale and
  finalize it. This fixes long tests being silently closed after a WebConfig
  pause lease expired and was later re-established.
- Made long Recording Load Tests fail fast if their recorder disappears for any
  external reason. `RECORDER_NONE` is no longer allowed to look healthy to the
  benchmark while it continues counting empty `recorderAddFrame()` calls.
- Preserve low-frequency last-known frame/media counters plus audio buffer
  capacity/high-water during the benchmark. If an external close ever occurs
  again, the failure report retains the useful pre-failure measurements instead
  of collapsing to zero frames/bytes and a zero-capacity audio buffer.
- No recording format, SFENC1 crypto/storage behavior, write-behind sizing, SD
  clock policy, audio format or benchmark thresholds changed in this release.

## v62 — 2026-09-26

- Kept normal SFENC1 recording physically sequential: when the underlying file
  stream is already positioned at the exact next encrypted-chunk offset, the
  writer no longer issues a redundant `fseek()`. Random-access MKV/AVI patches
  still seek normally, so encrypted container semantics and the SFENC1 format are
  unchanged.
- Extended SFENC1 physical-write failure diagnostics with the C stdio `errno`
  value/text, stream position before/after the failed write and observed physical
  file size. Seek failures now report the same low-level errno context. No retry
  was added: a real filesystem/SD failure remains fatal instead of being masked.
- Corrected Recording Load Test presentation so Video-/Frame-Timing is rated from
  its measured frame delivery/budget data. A separate storage/finalization failure
  no longer forces the timing row red when the measured timing itself is healthy.
- Kept crypto algorithms, SFENC1 on-disk layout, 4 KiB crypto/I/O slice size,
  512 KiB write-behind capacity, board SD clocks and traffic-light thresholds
  unchanged.

## v61 — 2026-09-26

- Fixed failed-MKV cleanup semantics: an underlying storage failure no longer makes
  the still-open writer look closed. `mkvEnd()` now collects write-behind telemetry,
  stops audio, closes storage/crypto resources and removes the incomplete `.part`
  file instead of returning early. Recording-open state and recording-health state
  are now intentionally separate so storage maintenance cannot race a failed but
  not-yet-cleaned-up recorder.
- Preserved useful diagnostics after storage failure. The load test now reports the
  logical media bytes accumulated before failure and propagates the concrete
  `RecordingStorageFile` reason instead of only `recorder/storage write failed`.
  SFENC1 chunk failures include chunk index and physical offset; partial physical
  writes also report written/expected byte counts.
- Reserved the 512 KiB PSRAM write-behind ring and 32 KiB drain scratch before
  opening SFENC1, so large contiguous recording-buffer allocations occur before
  encryption chunk caches. Initialization now reports the exact fallback reason
  (`ring_alloc_failed`, `scratch_alloc_failed`, `mutex_alloc_failed`,
  `task_create_failed`, etc.) together with free/largest internal RAM and PSRAM.
- Kept the encryption format, load-test traffic-light thresholds, recording clocks
  and normal synchronous fallback policy unchanged.

## v60 — 2026-09-25

- Added a 512 KiB PSRAM write-behind layer for MKV recording. Time-critical
  container writes now enqueue logical bytes into PSRAM while a low-priority
  storage task drains 32 KiB chunks through the unchanged `RecordingStorageFile`
  path. Plain and SFENC1-encrypted recordings therefore share the same buffering
  architecture and on-disk formats remain unchanged.
- Container `seek()`, explicit flush and finalization first drain all queued data
  before accessing the underlying file, preserving existing Matroska duration/
  Segment patching and encrypted random-access semantics. Storage errors remain
  fatal and are propagated back to the recorder; allocation/task failure falls
  back to the previous synchronous path instead of preventing recording.
- Added write-behind telemetry: PSRAM capacity/high-water, producer wait count and
  time, committed/queued bytes, drain-write count, slow drain writes and longest
  drain transaction. Normal MKV stop logs expose buffer high-water/waits, while
  the Recording Load Test reports full drain statistics and includes buffer
  reserve in the storage green/orange/red assessment.
- Physical SD I/O is no longer attributed to an individual slow frame while MKV
  write-behind is active because the drain occurs asynchronously. Existing raw
  per-frame storage diagnostics remain available for synchronous paths such as
  AVI or a write-behind fallback.

## v59 — 2026-09-25

- Added Recording Load Test diagnostics for the real Arduino filesystem boundary,
  measuring physical `File::write()`/`seek()` count, bytes, total time, longest
  operation and writes taking at least 20 ms for each captured slow frame.
- Refined the recording timing assessment so isolated rare stalls produce orange
  rather than automatically forcing red; red remains reserved for P99 beyond the
  frame budget, at least 1% budget overruns, frame delivery below 98% or an
  extreme single stall above five frame budgets.
- Added frame-delivery percentage to the technical report. The diagnostics are
  enabled only during the explicit load test and do not change production media
  formatting or recording behavior.

## v58 — 2026-09-25

- Changed normal MKV Cluster finalization to retain the standards-compliant
  unknown-size VINT already written when each Cluster opens. The next Cluster
  therefore terminates the previous one without seeking back to patch its size.
- Removes the two synchronous storage seeks that the v57 slow-frame diagnostics
  identified as the dominant approximately 140–170 ms periodic latency spike at
  each 5-second Cluster rollover. The 5-second Cluster duration/size policy itself
  is unchanged.
- Extended the SensorForge WebPlayer EBML scanner to recognize unknown-size
  Clusters and terminate them at the next Cluster header. Existing known-size MKV
  files remain supported, while new v58 recordings keep video, subtitles and PCM
  audio playable in the built-in player.
- Other Matroska masters, including Info, Tracks and the enclosing Segment, still
  receive their final known sizes. Video, PCM audio, timestamp subtitles,
  encryption/storage handling and load-test thresholds are otherwise unchanged.

## v57 — 2026-09-25

- Extended the Recording Load Test duration choices from 30 s / 60 s to 30 s,
  60 s, 5 min, 15 min, 30 min and 60 min. Tests longer than one hour are not
  exposed as a single file because the MKV writer deliberately stays below the
  FAT32/compatibility 4 GiB limit; multi-hour endurance testing should use
  segmented media instead of one ever-growing container.
- Converted the explicit load test from one long blocking HTTP request to a
  state machine advanced by the normal firmware loop. WebConfig starts the test
  immediately, polls lightweight status every five seconds and can request an
  abort. The test continues even if the browser page is closed, while normal
  motion/shooter/sleep automation remains excluded from the measured recorder
  path. No separate FreeRTOS recorder task was introduced, so scheduling remains
  representative of production recording.
- Added a hard disposable-media byte budget based on the free-space snapshot at
  test start. The benchmark still never invokes rollover and now stops before
  crossing the configured SD reserve, retaining an additional 64 MiB margin for
  filesystem/encryption overhead. Container size-limit hits also end the test
  cleanly instead of consuming storage indefinitely.
- Added detailed diagnostics for the ten slowest frame calls during the explicit
  benchmark. MKV timing is split into camera/JPEG acquisition, first-frame
  header/audio start, audio capture reads, audio/container writes, Cluster
  management, subtitle work, video writes, image-motion analysis and residual
  uninstrumented time. The added high-resolution stage timers are enabled only
  during the benchmark and do not add permanent per-frame production overhead.
- Thermal emergency handling now finalizes and removes an active benchmark
  container before storage is taken offline, while preserving the emergency
  recording-start block.

## v56 — 2026-09-25

- Added a WebConfig **Recording Load Test** with selectable 30 s / 60 s duration.
  The test uses the currently saved media configuration and the real camera ->
  recorder/container -> `RecordingStorageFile` -> SD path, including embedded MKV
  audio and SFENC1 encryption when enabled. Motion/event logic and automatic segment
  rotation do not terminate the synthetic test container; real segment/event finalize
  latency is now logged passively during normal recordings. Temporary benchmark media
  is removed afterwards and the benchmark never invokes rollover deletion of customer
  files.
- The load report evaluates video/frame timing, audio capture, storage/finalization,
  internal heap/PSRAM and CPU temperature independently. Green/orange/red overall
  status always follows the worst subsystem result rather than averaging away a
  critical bottleneck.
- Added P95 and P99 frame-call latency to the existing passive recording-performance
  monitor using a compact 5 ms histogram. Normal recording behavior, frame pacing
  and storage writes are unchanged; per-frame overhead is one bounded counter update.
- Added low-frequency passive heap/PSRAM telemetry to normal recording events. The
  event-end log now records start/minimum/after values while existing MKV audio-drop
  and thermal summaries continue to provide audio and temperature telemetry. Segment
  and event finalization duration are also logged explicitly.
- Recording-load timing assessment reports achieved FPS, average/P95/P99/worst call
  time, frame-budget use and overruns. Audio assessment uses capture availability,
  dropped bytes and ring-buffer high-water; storage includes clean finalization time.
- The explicit benchmark services the watchdog and existing thermal safety monitor
  while running. A thermal emergency retains the existing firmware safety behavior.

## v55 — 2026-09-25

- Restored the established WebPlayer behavior in which opening a video from the
  recordings list starts video playback automatically, including MKVs with audio.
- Changed v54 audio startup ordering so the first JPEG frame and video playback are
  no longer held behind the complete embedded-PCM WAV download. Audio extraction is
  deferred briefly after video playback starts, allowing the existing finite frame
  batch prefetch to get a head start and substantially improving perceived startup.
- When embedded audio becomes ready during playback, it is synchronized to the
  current video timestamp and started automatically when browser autoplay policy
  permits it. If unmuted autoplay is blocked, video keeps running and the next user
  pointer/key interaction enables audio instead of forcing the whole player to wait.
- While browser policy is blocking audio, the normal Play/Pause control temporarily
  shows `Enable audio`; using it starts the synchronized audio without pausing video.
- No MKV container, recording, encryption, file-list probing or storage behavior was
  changed in this release.

## v54 — 2026-09-25

- Added WebPlayer playback for SensorForge MKV recordings containing the v53
  `A_PCM/INT/LIT` audio track. The ESP32 exposes embedded PCM as a transient WAV
  HTTP response generated directly from the MKV; no sidecar WAV is written to SD.
- Audio playback uses the existing player controls. Recordings with audio remain
  paused after loading so the operator's Play click satisfies browser audio autoplay
  restrictions; image-only recordings retain the existing automatic start behavior.
- Player speed changes are applied to both video timing and browser audio playback.
  Pause, restart, frame stepping and seek operations keep the embedded audio timeline
  aligned with the displayed video, with bounded drift correction during playback.
- Extended lightweight MKV header probing to inspect Info + Tracks only, stopping
  before the first Cluster. The recording-day list can therefore identify audio
  tracks without scanning JPEG/PCM media payloads or counting sparse frames.
- MKV recordings containing audio now show an additional speaker icon in the
  recordings list. AVI and image entries remain unchanged.
- Embedded audio extraction reads through `RecordingStorageFile`, so encrypted MKV
  recordings continue to use the existing transparent SFENC1 read/decrypt path.
- Bumped the recordings-list session cache namespace so browsers do not reuse cached
  pre-v54 day rows that lack the new audio indicator.

## v53 — 2026-09-25

- Added production audio muxing to normal MKV recordings. When `audio_enabled=1`,
  the configured generic PCM capture source is embedded as Matroska Track 3 using
  `A_PCM/INT/LIT`; video remains Track 1 and timestamp subtitles remain Track 2.
- Audio capture starts with the first real camera frame so the PCM and video
  timelines share the same zero point. PCM block timestamps are derived from the
  number of muxed samples; audio is drained around video-frame timestamps to keep
  Matroska Cluster ordering monotonic across Cluster rotations.
- Finalization gives the capture task a short bounded tail-drain window and patches
  MKV Duration to cover the longer of video or embedded audio. Segment rotation
  therefore restarts A/V synchronization cleanly for every recording segment.
- Audio backend/start/runtime failure is deliberately non-fatal to video: SensorForge
  logs the audio warning and keeps the MKV video recording running. Shared MKV/SD
  write failures remain fatal because video and audio use the same container file.
- Embedded audio inherits the existing `RecordingStorageFile` path and therefore the
  same `recording_encryption` policy as the MKV video. No plaintext audio sidecar is
  created when recording encryption is enabled.
- Sparse Power-Shooter MKVs remain intentionally image-only. AVI also remains
  video-only; WebConfig automatically selects MKV whenever Audio is switched on,
  and the recorder logs a warning for manually supplied legacy AVI+audio configs.
- Reworked the 10-second capture-only Audio load test into a customer-facing
  GREEN/ORANGE/RED assessment. It rates delivery ratio, dropped bytes, PSRAM-ring
  high-water, maximum drain gap, minimum internal heap and minimum free PSRAM
  independently, then uses the worst criterion as the overall verdict.
- Initial GREEN limits are deliberately conservative: delivery >=98%, zero drops,
  ring high-water <50%, drain gap <=500 ms, internal heap >=64 KiB and free PSRAM
  >=512 KiB. RED begins below 95% delivery, at >=1% dropped audio, >=90% ring use,
  >2 s drain gap, <32 KiB internal heap or <256 KiB free PSRAM; intermediate values
  are ORANGE. Raw engineering measurements remain available under Technical details.
- The capture-only benchmark explicitly remains a subsystem test. Production load
  qualification still requires a real camera + MKV + audio + SD + optional SFENC1
  recording on hardware. The current SensorForge WebPlayer continues to parse the
  MJPEG/subtitle tracks and does not yet play the new PCM track; downloaded MKV files
  can be used for the first audio-container qualification.

## v52 — 2026-09-25

- Simplified the normal WebConfig audio section to the single operator-facing
  Audio on/off control. Source, format and hardware routing now live behind an
  **Advanced audio settings** modal so normal users do not need to understand
  sample rates, bit depth, channels or GPIO routing.
- The advanced modal retains User/Expert mode, board-default/external source,
  sample rate, bit depth, channel count and the v51 external PDM/I2S hardware
  configuration. User mode continues to force the board-default source.
- Increased the WAV diagnostic from 5 seconds to 10 seconds. Starting the WAV
  test now opens an indeterminate activity popup with a moving bar before the
  synchronous capture begins; it deliberately does not display a fabricated
  percentage.
- Added a separate 10-second capture-only **Audio load test**. It drains the
  generic PCM ring without writing a benchmark file to SD and reports nominal
  PCM rate, captured/drained bytes, dropped bytes, PSRAM ring high-water,
  maximum drain-loop gap, empty reads, and internal-heap/PSRAM before/min/after.
- The load-test verdict is intentionally based on real capture margin/drops and
  does not invent a CPU-utilization percentage. It is an early subsystem check;
  full production qualification still requires a simultaneous camera + audio +
  MKV/storage/encryption stress test after container integration.
- Normal AVI/MKV recording remains video-only in this release; `audio_enabled`
  does not yet mux audio into production recordings.

## v51 — 2026-09-25

- Refactored audio hardware ownership: `board_config.h` now describes only
  physically integrated board audio. XIAO keeps its fixed onboard PDM microphone
  and pins there; Freenove declares that no microphone is integrated.
- Added runtime-configurable audio source selection to `config.txt`: normal
  `board_default` mode uses integrated board hardware, while Expert mode can
  select an external `pdm` or standard `i2s` microphone without requiring a
  separate firmware build.
- Added external PDM clock/data GPIOs and standard-I2S BCLK/WS/DATA/optional
  MCLK plus left/right/stereo slot selection. Known SensorForge pin conflicts
  with camera, SD, presence/radar, RTC, status LED, integrated microphone and
  ESP32-S3 flash/PSRAM bus pins are rejected during config validation.
- Extended the generic audio-capture abstraction so backend/pin selection is
  resolved at runtime. The already-qualified XIAO onboard path remains
  16 kHz / 16-bit / mono; external PDM is conservatively limited to 16-bit
  mono and external standard-I2S currently to 16-bit PCM until broader formats
  are hardware-qualified.
- WebConfig now separates normal audio controls from Expert hardware settings.
  Hardware routing must be saved before the 5-second WAV diagnostic can use it,
  preventing a test from silently exercising a different microphone.
- Audio continues to use the same `recording_encryption` media policy as video,
  shooter media and the existing WAV diagnostic through `RecordingStorageFile`;
  no separate unencrypted audio storage path was introduced.
- AVI/MKV recorder muxing remains intentionally unchanged in this release. This
  step establishes the hardware/config abstraction before container integration.

## v50 — 2026-09-25

- Fixed the 5-second WebConfig WAV audio diagnostic incorrectly reporting
  `not enough free SD space above SensorForge reserve` on an almost-empty card.
- Root cause: the diagnostic intentionally owns `g_storageLocked` during capture,
  while the normal `storageFreeBytes()` helper deliberately returns zero whenever
  that lock is active. The WAV diagnostic now performs its owner-side reserve
  check from the mounted storage backend's `totalBytes()` / `usedBytes()` values
  while retaining the configured SensorForge reserve and 128 KiB safety margin.
- No changes to the global storage-lock semantics, normal recording reserve logic,
  audio capture backend, WAV format, encryption behavior or AVI/MKV recording.

## v49 — 2026-09-24

- Factory/default hostname and hotspot SSID are now derived from the central
  `Branding::APP_NAME` product identity instead of duplicating a literal in
  `config.cpp`. The branding value is normalized to a lowercase, hostname-safe
  maximum 32-byte identifier; current `SensorForge` therefore becomes
  `sensorforge`. A future product rename in `branding.h` automatically changes
  the factory AP name on newly defaulted devices.
- Added a complete generated factory-config serializer based on the current
  firmware default value set, including audio, recording, motion, transport,
  network, hotspot, web, storage and debug settings. The generated config is
  validated through the normal config parser before persistence.
- Added WebConfig action **Alle Konfigurationswerte auf Werkseinstellungen**.
  It replaces every known config value with current firmware defaults, always
  writes the internal LittleFS config, and also replaces an already-existing
  SD `/config.txt` so an old SD config cannot win again after reboot. A card
  without `/config.txt` remains internal-only and is not given a new config
  implicitly.
- Factory-config reset clears the deferred transport-mode reconciliation marker,
  applies the defaults coherently for the short pre-reboot interval, then
  schedules a controlled reboot. The transition page tells the operator the
  branding-derived open hotspot SSID and that Wi-Fi remains active indefinitely.
- This is a configuration reset only: recordings/media, license state, firmware,
  board/eFuse cryptographic keys and other non-config persistent identities are
  deliberately not erased.

## v48 — 2026-09-24

- Changed firmware defaults used only when no valid SD config and no valid internal
  LittleFS config exist: hostname / hotspot SSID is now `sensorforge`, the hotspot
  password is empty (open AP), and `wifi_timeout_sec=0` keeps WebConfig Wi-Fi on
  indefinitely. Existing persisted configs remain authoritative and unchanged.
- Hotspot validation now accepts either an empty password (open AP) or a normal
  8..63-character WPA passphrase. The AP startup passes a null passphrase explicitly
  for the open-network case.
- `hotspot_enabled=1` remains the first-boot default, so the local SensorForge AP
  starts automatically. Infrastructure Wi-Fi policy remains independent and
  `wifi_on_system_start` remains `off` by default.
- WebConfig now shows whether the currently active hotspot is open or password
  protected without exposing any password value.

## v47 — 2026-09-24

- Added a new independent `audio_capture.cpp/.h` subsystem with a backend-neutral
  packed-PCM interface. Microphone transport, audio format and recorder/container
  integration are deliberately separated so future PDM, standard I2S and codec/ADC
  backends can share the same higher-level API.
- Added the XIAO ESP32S3 Sense onboard PDM microphone as the first backend: PDM CLK
  GPIO42 and DATA GPIO41. The board profile declares backend capabilities rather
  than hard-coding microphone details into the recorder.
- Audio capture runs in its own task and buffers PCM in a PSRAM ring buffer so short
  filesystem/SD latency does not directly stall the microphone input path. Buffer
  overrun and high-water statistics are exposed for diagnostics.
- Added optional, backward-compatible config keys `audio_enabled`,
  `audio_sample_rate`, `audio_bits_per_sample` and `audio_channels`. Existing v46
  configs remain valid; audio defaults to disabled. Unsupported format combinations
  are rejected against the active board backend.
- Added `audio_wav.cpp/.h` and a WebConfig 5-second WAV diagnostic. It records through
  the normal `RecordingStorageFile` abstraction, performs SD reserve checks, reports
  signal/buffer statistics and allows transparent WAV download. Existing recording
  encryption policy is honored by the diagnostic storage path.
- Freenove currently declares no microphone backend; this is intentional and can be
  extended later without changing the generic audio API.
- AVI/MKV writers and the normal recorder lifecycle are intentionally unchanged in
  v47. This release validates capture, buffering and storage first; container audio
  tracks are a later integration stage.

## v46 — 2026-09-24

- Added a dedicated professional in-progress state for the extended SD benchmark.
  Submitting the benchmark now opens a blocking modal immediately, disables the
  start button and clearly states that write/read throughput, latency, integrity
  and bus-clock diagnostics are running.
- The benchmark modal uses a neutral progress track with a red animated point
  moving continuously across it, so long synchronous diagnostic runs visibly
  remain active without pretending to know a percentage that is not available.
- The progress state is bilingual and uses the existing SD Maintenance modal
  visual language. Benchmark logic, rating thresholds, formatting policy and
  production storage clocks are unchanged.

## v45 — 2026-09-24

- Added a customer-facing green/orange/red storage assessment to the extended
  SD benchmark. The rating evaluates the complete SensorForge storage path
  (card + filesystem + bus), not the SD card in isolation.
- The assessment uses the verified 32 KiB reference case and considers write/read
  throughput, write latency and data verification. Green requires at least
  1.0 MB/s verified write throughput, at least 0.75 MB/s read throughput and
  bounded write latency; clearly slow or failed/incorrect storage becomes red.
- Filesystem geometry below the SensorForge 32 KiB cluster recommendation now
  produces a prominent warning and prevents a green result. The UI explicitly
  tells the operator to use SensorForge SD Format and rerun the benchmark.
- The benchmark page now presents verdict and "what next" guidance before the
  engineering data. Full diagnostic output is retained but collapsed under
  Technical diagnostic details by default.
- No production SD clock, recording behavior or formatting policy changed in
  this release.

## v44 — 2026-09-24

- Changed SensorForge SD Format to create FAT32 explicitly with a 32 KiB
  allocation unit instead of relying on Arduino's format-on-mount-failure
  default. The same formatter is used for SPI and SD_MMC storage backends.
- The formatter keeps the existing Arduino VFS/storage driver registration,
  temporarily detaches only the mounted FatFs logical volume, runs `f_mkfs()`
  with FAT32 / two FATs / 32 KiB clusters, and remounts the same FatFs volume.
- Formatting now verifies the effective FatFs cluster size after remount and
  fails closed if it is not exactly 32 KiB. No silent fallback to 512-byte
  clusters is accepted.
- SD Maintenance UI now states that SD Format creates FAT32 with 32 KiB
  clusters. Existing config protection, storage/recording gates, config restore
  and reboot behavior remain unchanged.
- Benchmark, production SD clock policy and normal recording/storage behavior
  are unchanged.

## v43 — 2026-09-24

- Extended the generic SD benchmark with filesystem geometry reporting derived
  read-only from the mounted card boot sector where the backend/core exposes raw
  sector reads. Reports FAT12/16/32 or exFAT, bytes per sector, sectors per
  cluster, cluster size, volume start LBA and cluster count. Geometry reporting
  is diagnostic only and does not fail the benchmark when unavailable.
- Added an 8 MiB / 32 KiB long-file comparison on the normal production mount.
  The first verified pass measures normal file growth/allocation; the second
  verified pass reopens the exact same 8 MiB file with read/write access and
  overwrites its already allocated clusters using a different deterministic
  pattern. This isolates FAT allocation/file-growth cost from steady-state
  sequential writes without formatting or touching user files.
- Reports growing-file and preallocated-overwrite throughput plus their speed
  ratio, while retaining full write/read latency, flush timing and read-back
  verification for both passes.
- The existing 2 MiB multi-block tests and generic verified bus-clock sweep are
  retained unchanged. Free-space safety now reserves enough room for the 8 MiB
  diagnostic file above the configured SensorForge storage reserve.
- No production SD clock, filesystem format policy, recorder behavior or normal
  storage path was changed.

## v42 — 2026-09-24

- Extended the standard SD diagnostic with a generic verified bus-clock sweep in
  `web_sd_maintenance.cpp`; the implementation is shared by SPI and SD_MMC
  backends rather than being specific to the Freenove board.
- The existing production-mount 4/16/32/64 KiB benchmark remains unchanged and
  still provides the primary 32 KiB comparison result.
- SPI diagnostics build a conservative clock plan from standard test points and
  never exceed the board profile's `SD_SPI_MAX_FREQUENCY_HZ`; the current XIAO
  profile therefore tests 4/10/20 MHz and never exceeds its approved 20 MHz.
- SD_MMC diagnostics use standard 10/20/40 MHz points up to the platform's
  high-speed diagnostic ceiling and additionally respect Arduino's
  `BOARD_MAX_SDMMC_FREQ` when the selected board variant declares a lower cap.
  The current Freenove 1-bit profile therefore gets the same generic clock sweep
  without changing its normal 20 MHz policy.
- Every clock point remounts the backend, performs a compact 512 KiB write/read
  test with 32 KiB blocks, records throughput and latency, and verifies the full
  deterministic data pattern. A diagnostic clock that cannot mount or verify is
  reported as such but does not by itself invalidate the production setting.
- Before any diagnostic remount the persistent logger is flushed and closed and
  the storage gate is held. After the sweep SensorForge must restore the exact
  production mount clock before the logger is reopened; restore failure marks
  the overall benchmark as failed and leaves `sdReady` false.
- SD_MMC reporting is width-aware for future board profiles that provide D1-D3;
  current Freenove remains reported and mounted as 1-bit. No format/raw write or
  permanent production clock change is performed by the benchmark.

## v41 — 2026-09-24

- Replaced the former single 1 MiB / 32 KiB write-only SD benchmark with an
  extended non-destructive storage diagnostic in `web_sd_maintenance.cpp`.
- The diagnostic reports board/storage backend, configured mount clock, storage
  pins, Arduino-ESP32 version, card type/capacity and filesystem total/used/free
  space before running the data tests.
- Runs 2 MiB write/read tests with 32 KiB first for direct historical comparison,
  followed by 4 KiB, 16 KiB and 64 KiB block sizes.
- Reports write I/O, loop and total-with-flush throughput; read I/O and verified
  read-path throughput; open/flush/close timings; and per-call min/average/P95/
  worst latency for every tested block size.
- Every test file is read back and verified against a deterministic data pattern;
  integrity failure or an unremovable temporary benchmark file fails the test.
- The benchmark uses only `/.__sensorforge_sd_benchmark.bin`, deletes it after
  each case/final cleanup, respects the configured free-space reserve and remains
  unavailable while recording or another storage operation owns the storage gate.
- The 32 KiB result is summarized directly on the SD Maintenance page; the full
  engineering report is shown in an expandable diagnostics section.
- No production SD/SPI/SD_MMC clock or mount policy was changed in this release.

## v40 — 2026-09-24

- Extracted the complete SD Maintenance implementation from `webconfig.cpp` into
  the new `web_sd_maintenance.cpp` / `web_sd_maintenance.h` module.
- SD status, recovery, benchmark, wipe, format and secure-erase routes keep their
  v39 paths and behavior; the route set is unchanged.
- Long-running Secure Erase remains incrementally serviced from the WebConfig
  firmware loop through the new module loop entry point, including operation
  continuation after WebConfig is stopped.
- Shared WebConfig dependencies are limited to page-header/page-footer callbacks
  and the existing delayed reboot scheduler; storage/config/recorder logic stays
  owned by the SD module.
- This release is a structural refactor only; no intentional SD-maintenance UI or
  storage-policy change was introduced.
- Fixed the module-boundary compile issue in the WebConfig inactivity guard by
  querying SD-maintenance busy state through the module API instead of its private
  Secure Erase state variable.

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
