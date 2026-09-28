## v84 candidate - zwei parallele RTSP-Clients

- RTSP-Streamer von einem auf maximal zwei gleichzeitige RTSP/TCP-Clients erweitert.
- Beide RTSP-Sessions verwenden denselben zentral aufgenommenen JPEG-Frame; Kamera-Capture und JPEG-Erzeugung werden nicht dupliziert.
- RTSP-Session, RTP-Sequenznummern, SSRCs und Interleaved-Kanäle werden pro Client getrennt verwaltet.
- Optionaler PCM/L16-Audio-Capture bleibt ein gemeinsamer SensorForge-Capturepfad und wird an beide aktiven RTSP-Sessions verteilt; kein zweiter PDM/I2S-Treiber wird gestartet.
- Status/API-Telemetrie liefert `rtsp_clients`; Übersicht zeigt RTSP-Clients und HTTP-Clients jeweils als `x/2`.
- Ein dritter RTSP-Client wird weiterhin kontrolliert mit `453 Not Enough Bandwidth` abgewiesen.
- Weiterhin Kandidatenstand: kein behaupteter Hardware-/Langzeittest und kein Release-Sprung auf v84.



### v84 candidate – Streamer-Livetelemetrie
- Streamer-Dashboard aktualisiert Frames, FPS, gesendete Bytes und aktive Stream-Clients alle 2 Sekunden über den bereits vorhandenen leichten `/streamer_status`-Endpunkt.
- Polling pausiert bei verborgenem Browser-Tab und startet beim Zurückkehren sofort wieder.
- `/streamer_status` meldet zusätzlich die tatsächliche Anzahl paralleler HTTP-MJPEG-Clients.

## v84 candidate - HTTP-MJPEG Refresh-Reconnect

- HTTP-MJPEG behandelt einen neuen Stream-Connect vom selben Remote-Host als Browser-Refresh und ersetzt die vorherige Verbindung sofort, statt den Reload wegen eines kurzzeitig noch als verbunden geltenden TCP-Sockets mit `503 busy` abzuweisen.
- Das konservative Limit bleibt bestehen: ein HTTP-MJPEG-Client von einem anderen Host wird weiterhin abgewiesen, solange bereits ein Stream aktiv ist.
- Die Dashboard-Anzeige trennt Web-Oberflächen und Stream-Clients jetzt deutlicher und erklärt, dass eine Web-Oberfläche keinen Stream-Client-Platz belegt.

# SensorForge Changelog

## v84 candidate — 2026-09-28 (not yet released)

- Added an additive `operating_mode=normal|streamer` configuration with safe legacy defaults (`normal`, RTSP off, HTTP-MJPEG off). Existing v83 configs therefore continue in normal SensorForge mode without migration.
- Added isolated `streamer.cpp/.h` runtime ownership for a shared JPEG camera pipeline. Normal recording, Power Shooter, recording load test and recording/sleep automation are bypassed in streamer mode without overwriting their persisted settings.
- Added RTSP on TCP port 554 with OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN and GET_PARAMETER, one-client limit, RTP interleaving over RTSP/TCP, and RFC 2435 JPEG packetization using parsed frame dimensions, sampling and quantization tables.
- Added optional RTSP L16/PCM audio by reusing the existing SensorForge `audio_capture` backend and the existing global audio configuration. Audio start failure does not prevent video streaming.
- Added a dedicated non-blocking HTTP-MJPEG server on port 81 with a one-client limit. WebConfig `/stream` redirects to the dedicated stream endpoint.
- Integrated Streamer selection into the general Configuration page as the central **Betriebsmodus** alongside the existing recording modes. Streamer-specific RTSP/HTTP addresses and transport switches appear only when that mode is selected; the old `/streamer` page is now a compatibility redirect. Active Streamer mode shows a dedicated dashboard status box with URLs, client state, audio state, FPS/frame/byte telemetry and links back to Configuration/Camera. Shared-frame snapshots, status URLs and additive integration-API fields remain available.
- Clarified the Streamer transport UI: the camera/audio reuse explanation now lives behind the standard info button; RTSP is labelled as the recommended VLC/ffmpeg/NVR output and HTTP-MJPEG as the browser/integration output. Both may run together. Streamer mode now requires at least one output, with client-side guidance plus server-side validation; when a user newly selects Streamer with neither output checked, RTSP is preselected.
- Expanded Streamer transport guidance for normal users: each transport now explains typical software/use cases, its own URL is shown directly below that transport only while selected, and a dedicated **Welche Stream-Art brauche ich?** info dialog explains RTSP versus HTTP-MJPEG, typical VLC/ffmpeg/NVR/Frigate/Shinobi/Home-Assistant usage, simultaneous operation and the current one-client-per-transport qualification limit.
- Changing the central Betriebsmodus or Streamer transport selection now automatically enters the existing delayed reboot chain after a successful config save. The browser is redirected to the normal restart/reconnect page, so the persisted mode takes effect without a separate manual reboot step; config saves remain blocked during an active recording.
- Clarified browser compatibility in the Streamer UI: RTSP is explicitly marked as not directly playable in Chrome/Firefox and as the optional-audio path, while HTTP-MJPEG is marked as directly browser-playable video without audio.
- Added lightweight live dashboard telemetry: visible WebConfig browser sessions are counted from the existing heartbeat (45 s expiry), streamer clients remain a separate count, and current/minimum internal heap plus free PSRAM are shown as system-resource indicators. The common header now also shows a low-overhead **Systemlast** estimate beside time/temperatures. It is derived from per-core FreeRTOS idle-hook ticks, sampled once per second; the displayed value is the mean of Core 0/1 and the tooltip exposes both core values. No extra sampling task or faster WebConfig polling was introduced.
- Existing camera settings remain authoritative; no duplicate resolution/FPS/JPEG/XCLK/exposure/audio backend configuration was introduced. Active streamer mode blocks live crop mutation to avoid changing a sensor underneath connected stream clients.
- Thermal emergency still has priority and explicitly stops the streamer before WiFi/camera shutdown.
- This is an implementation candidate only. No Arduino-ESP32 build or hardware/VLC/ffmpeg endurance test was available in this workspace, so `sensorforge_version.h` intentionally remains at the last confirmed v83 release until those qualification steps are completed.

## v83 — 2026-09-28

- Reordered the general Configuration page so the complete **Audio / Mikrofon** block now sits directly before **Sleep / Stromsparen** instead of between recording/media settings. Audio enable, advanced audio modal and all existing save/validation semantics are unchanged.
- Simplified the Bootloop/undervoltage help text by removing the redundant normal-shutdown sentence.
- Reworded the LED control for non-technical users as **Aktiviert - Status-LED verwenden** / **Deaktiviert - Status-LED bleibt aus** without changing `led_enabled`.
- Replaced the free-text POSIX timezone field with an alphabetically grouped dropdown of 33 common worldwide regions. The stored values remain the existing POSIX TZ strings, including DST rules where applicable. Existing custom/legacy timezone strings that are not in the list are preserved as a selected compatibility option rather than overwritten.
- Reworked WLAN, Hotspot and Web-access fields into clearer labelled grids with user-facing names and password placeholders while retaining the same config keys and password-preservation behavior. The web recording auto-pause wording now uses normal German umlauts and no longer exposes the raw config key as the primary label.
- Reworded Debug as **Debug-Ausgaben** with understandable dropdown choices and an info dialog explaining that extra technical diagnostics/log writes are intended for fault analysis, not normal operation.
- No config schema, runtime network logic, NTP/time handling, audio backend, sleep logic, bootloop protection, logging implementation or password storage behavior was changed.

## v82 — 2026-09-28

- Consolidated the user-facing diagnostics into one **Systemtest** on the **System** page. The existing production-path Recording Load Test remains the underlying engine and still exercises the real camera -> recorder/container -> SD path, including embedded audio and SD encryption when enabled, plus memory and thermal telemetry.
- Removed the separate 10-second WAV audio-test and capture-only Audio load-test buttons from the normal Audio configuration UI. Their historical POST routes remain as compatibility redirects to the Systemtest so stale pages/bookmarks do not create a second test workflow.
- Added diagnostic-only 16-bit microphone signal measurement (peak/RMS) to `audio_capture`. It is enabled only while the explicit Systemtest is active, so normal recordings incur no permanent per-sample analysis overhead. A requested audio test now fails if bytes are flowing but the measured PCM signal is digital silence.
- Moved Systemtest duration/start/status/last-result controls from general Configuration to System. Test status and result pages now return to the System page.
- Kept the underlying recording-load state machine, temporary-media cleanup, SD-reserve protection, thermal emergency handling, MKV/write-behind diagnostics and long-test durations unchanged.

## v81 — 2026-09-28

- Simplified the general Config page: config-storage behavior, factory reset details and recording-mode explanations now live behind concise info buttons.
- Removed the obsolete SFSEC1 explanatory paragraph from the visible Config page.
- Promoted the automatic recording trigger into a dedicated highlighted control with a clear info explanation and a direct button to Image Motion settings.
- Moved recording/log SD encryption completely from the general Config page to SD Maintenance.
- Added a dedicated encryption save path that preserves all unrelated config keys and retains the existing one-time ESP32-S3 eFuse provisioning behavior on first activation.
- General Config saves now preserve recording_encryption unchanged so unrelated settings cannot disable encryption accidentally.

## v80 — 2026-09-28

- Simplified **SD Wartung** for normal users without changing the underlying storage/recovery/benchmark/format logic. The page header now carries the existing **SD BEREIT** and **SD GEMOUNTET** status pills so card state is visible immediately.
- Condensed the storage-reserve policy wording to **Aktion bei unterschrittener Speicherreserve** and removed the redundant two-line explanation.
- Reworked **SD Benchmark** as the single canonical benchmark name. The normal page now shows only a short purpose statement; the full test description is behind an info button. Existing benchmark scope remains unchanged, including verified raw endurance, production recording/SFENC1 stack testing and bus-clock checks. Detailed performance measurements are collapsed under **Messwerte anzeigen**.
- Moved **SD Recovery** to the bottom of the page and labelled it explicitly **SD Recovery (Notfall)**. The visible text now states that it should only be used when a card is no longer detected/mounted correctly. Technical read-only/mount-retry details are behind an info button; post-run diagnostics remain available in a collapsed technical-details section.
- Removed the redundant **Destruktive SD-Wartung** banner and permanent **Config-Schutz** explanation from the normal page. The existing internal config-protection implementation is unchanged.
- Moved the explanatory texts for **SD Wipe**, **SD Format** and **Secure Erase** behind inline info buttons. Destructive confirmation dialogs, progress handling, config protection, reboot behavior and backend capability checks are unchanged.
- Simplified destructive-operation confirmation wording so normal users are not exposed to internal config-storage terminology.

## v79 — 2026-09-27

- Removed the complete Power Shooter tuning block from the general Configuration page and added a dedicated **Power Shooter** menu entry under **System**. The new page owns storage format, interval, darkness threshold, minimum-change filter, force-save, flush interval and the existing advanced motion-hint rule.
- Added `configSaveShooterSettings()` as a dedicated narrow persistence path. It patches only Shooter-specific keys in the active `config.txt`, validates the complete resulting config and keeps the in-RAM Shooter values coherent. Shooter enable/disable remains intentionally owned by the normal Recording mode so `Motion`, `Shooter` and combined operation still have one mode selector.
- General Configuration saves no longer read Shooter tuning from the browser form; they preserve the currently active Shooter values verbatim. This removes the duplicate edit path and prevents unrelated Configuration saves from overwriting settings changed on the dedicated Shooter page.
- Moved `min_free_space_mb` and `disk_full_action` from general Configuration to a new **Speicher / SD-Sicherheit** section at the top of **SD Wartung**.
- Added `configSaveStorageSafety()` as the dedicated persistence path for those two storage-policy keys. The SD Maintenance page now saves the reserve and full-card action directly while preserving all unrelated config keys/comments.
- General Configuration saves now preserve the current SD-safety values rather than accepting a second edit path. Existing rollover/stop runtime semantics, storage guard behavior, Shooter capture/flush logic and recording mode behavior are unchanged.

## v78 — 2026-09-27

- Added user-facing camera-load guidance to the normal Camera page. `1024x768` now shows an amber **Erhöhte Systemlast** warning; `1280x1024`, `1600x1200` and `2048x1536` show a stronger **Hohe Systemlast** warning. The existing backend performance validator remains unchanged and authoritative at save time.
- Simplified the live-camera area: removed the redundant **Live-Vorschau & Sensor-Ausschnitt** heading and the permanent technical Advanced-settings explanation. The label now reads only **Live-Vorschau**, with its safety explanation behind the adjacent info button.
- Moved the display-only preview zoom controls (`- / + / Fit / 100%` and keyboard shortcuts) directly below the live image so they are visually associated with viewing the image rather than persistent camera configuration.
- Expanded beginner help on **Erweiterte Kameraeinstellungen** with info buttons for FPS, JPEG quality, camera XCLK, Auto Exposure and AE level. FPS help explicitly explains frames per second and warns that more than 4 fps causes substantially higher system load and is not recommended on smaller boards.
- Replaced the raw weighted performance-score text on the Advanced page with a simpler qualified maximum-FPS statement plus an amber warning when the selected value exceeds 4 fps.
- Added **Standardwerte einsetzen** on the Advanced Camera page. It only fills the form; nothing is persisted until the user presses the existing save button. Defaults follow the existing firmware policy (quality 12, Auto Exposure on, AE level -1, board-default XCLK, and a performance-safe default FPS for the currently selected resolution).
- No camera save semantics, config validation, Preview gate, OV3660 crop, recording, Shooter, storage, encryption or transport runtime behavior was changed.

## v77 — 2026-09-27

- Simplified the Transport save confirmation to the single operator-facing message **Transportwerte gespeichert.** The implementation/persistence location is no longer exposed in the normal UI.
- Moved the detailed automatic Transport black-reference measurement explanation behind a small info button. The active measurement progress dialog remains visible while the measurement is actually running.
- Made manual Shutdown reload-safe: the shutdown status page resets the browser address to `/` shortly after it loads without issuing a new request. The visible shutdown page remains on screen while the board enters Deep Sleep, but the next user reload after power/reset opens the normal start page instead of `/shutting_down`.
- Simplified the main Camera page. Camera model is now a dropdown (OV2640/OV3660, while preserving an existing legacy value if present), resolution is a dropdown containing exactly the eight resolutions accepted by the existing config validator, and rotation remains in the normal camera settings. Free-text camera model/resolution entry is removed from the normal UI.
- Added a dedicated **Erweiterte Kameraeinstellungen** page for FPS, JPEG quality, camera XCLK, Auto Exposure and AE level. Both normal and advanced pages continue to use the existing `configSaveCameraSettings()` path, complete config validation and internal/SD persistence policy; hidden fields preserve the settings owned by the other page.
- Moved the Live Preview safety explanation behind an info button. The preview gate itself is unchanged: Detection and Recording remain disabled while the Camera preview is active.
- Camera model handling was not functionally reinterpreted: board-specific builds still select their pin map from the board definition, and runtime sensor capabilities/PID checks remain authoritative for OV3660-only features such as raw sensor crop.
- No recording, Shooter, Sparse-MKV, encryption, storage, transport runtime, camera-crop or OTA behavior was removed.

## v76 — 2026-09-27

- Consolidated the complete camera-facing configuration on the canonical **Kamera** page (`/preview`). Camera model, resolution, recording FPS, JPEG quality, XCLK, Auto Exposure, AE level and rotation now live together with the existing Live Preview and OV3660 sensor-crop controls.
- Removed the visible Camera section from the general Configuration page. The general save form still carries the current camera values as hidden fields so saving unrelated settings cannot reset, delete or silently revert any camera setting.
- Added a dedicated camera-settings persistence path that patches only the non-crop camera keys in the existing active `config.txt`, validates the complete resulting configuration through the normal validator and writes it through the existing internal/SD synchronization policy. Unrelated config keys, comments and future keys are preserved.
- The existing OV3660 live Crop Apply/Save path is unchanged and remains separate because it can be tested directly on the active sensor before persistence. Crop/zoom is nevertheless on the same Camera page, so all camera controls now have one UI home.
- Camera-save success keeps the in-RAM camera config coherent to prevent a later general config save before reboot from restoring stale camera values. Sensor settings that require camera reinitialization still become fully active after reboot, matching the previous configuration semantics.
- When a saved non-FPS sensor setting differs from the currently initialized camera, Live Preview/Crop is deliberately paused until reboot and the Camera page shows a restart notice. This prevents old sensor output from being mistaken for the newly saved configuration or being used for a crop test under mismatched geometry.
- The top navigation **Kamera** entry is now a direct link to the consolidated Camera page instead of a one-item dropdown. Existing `/preview`, `/snapshot`, crop, recording-priority and preview-gate behavior remain intact.

## v75 — 2026-09-27

- Moved the orange **Bewegungsverdacht** badge to the right side of each recording row so time range, size and action controls remain visually aligned with rows that do not carry a hint.
- Added a small information button next to the badge. Hover text and the click dialog explain in plain language why the hint was raised, that broad global-light jumps are suppressed, and that the hint is not a confirmed alarm.
- New motion-hint Sparse-MKV filenames persist the actual hint rule used for that file as a compact internal marker (`c` = change threshold in tenths of a percent, `h` = required hits, `w` = analysis window), while keeping the existing `_motionhint.mkv` suffix for compatibility. The recording list therefore continues to show the correct historical rule even if the Shooter settings are changed later.
- Existing v68–v74 `_motionhint.mkv` files remain fully compatible. Because those older filenames did not record their exact hint threshold/window, their information dialog gives a truthful general explanation instead of substituting the current configuration.
- The additional rule metadata lives only in the in-RAM Shooter frame header and final filename. JPEG selection, the configured `shooter_min_change_pct`, Sparse-MKV media contents/timestamps, PSRAM flush behavior, encryption, storage recovery and normal Motion Recording are unchanged.

## v74 — 2026-09-27

- Added a compact **Speicherreserven** status to the consolidated System page so firmware growth and internal-RAM pressure remain visible when builds or boards change.
- Firmware/App usage is calculated at runtime from the current sketch size and the actually running app partition. The UI uses conservative guidance: below 80% = OK, 80–90% = observe, from 90% = tight. This automatically follows a different app-partition size instead of hard-coding the current XIAO maximum.
- Added current internal free heap plus the minimum internal free heap observed since boot using the ESP-IDF heap-capabilities API. The minimum is rated >=50 KiB = OK, 40–50 KiB = observe and <40 KiB = tight.
- The threshold explanation lives behind the existing System-page info button so the main page remains compact. It explicitly notes that Arduino's compile-time `Global variables use ...` linker statistic cannot be reconstructed reliably at runtime and should still be checked briefly after builds.
- No partition layout, memory allocation policy, recording, Shooter, PSRAM, storage, encryption, RTC/NTP or OTA behavior was changed.

## v73 — 2026-09-27

- Simplified the consolidated **System** page so explanatory text no longer dominates the main UI. RTC, PSRAM and firmware safety explanations are now available through small inline information buttons with one shared lightweight dialog.
- Moved Board and Storage identity to the top of **Systeminformationen** and removed the separate Boardinformationen card. The legacy `/board` URL now redirects to `#system-info`.
- The RTC auto-detection/NTP explanation is no longer permanently shown. The info dialog explains that RTC hardware is detected automatically, updated after successful NTP synchronization, and needs no RTC keys in `config.txt`; without RTC, normal system/NTP time is used.
- The PSRAM section now shows only test state and measurements. Its info dialog states only that the quick test runs automatically when the System page opens; the obsolete wording about there being no separate test button was removed.
- Combined firmware status, firmware-file selection, upload/validation and staged-image installation into one **Firmware Update** section. Removed the separate numbered `Firmware auswählen`, `Prüfung & Installation` and `Sicherheitsablauf` cards.
- Shortened the firmware safety explanation and moved it behind the Firmware Update info button: upload and compatibility validation happen first; only explicit installation activates the staged image for the next reboot; an interrupted upload leaves the existing firmware active.
- No RTC/NTP behavior, PSRAM test algorithm, OTA validation/install logic, recording, Shooter, storage, encryption or transport behavior was changed.

## v72 — 2026-09-27

- Consolidated the former **System Info**, **Board Info**, **PSRAM Test** and **Firmware Update** menu entries into one canonical **System** page. The existing SD Maintenance, Log Viewer, License, Transport, Reboot and Shutdown entries remain separate.
- The System page now shows system resources/RTC diagnostics, board/storage information, the PSRAM result and the complete existing WiFi firmware update workflow on one page. The firmware upload/validation/install safety path itself is unchanged.
- The former 1 MiB PSRAM write/read test now runs automatically when the System page is opened. The page displays only the result, elapsed time and free-PSRAM values; no separate start button or menu entry is needed.
- For safety, the automatic PSRAM allocation test is skipped while a recorder is still open; the System page remains available and reports that the test was not executed during the active recording.
- Legacy `/sysinfo`, `/board` and `/psram` URLs redirect to the matching section on `/system`; `/firmware_update` remains accepted as a compatibility alias for the consolidated page.
- No recording, Shooter, Sparse-MKV, storage, encryption, RTC/NTP, transport or firmware OTA validation/install logic was changed.

## v71 — 2026-09-27

- Removed the redundant visible `Zeit:` / `Time:` prefix from the common WebConfig header clock. The header now starts directly with `DD.MM.YYYY · HH:MM:SS`.
- The displayed value remains the ESP32-S3 module system clock: `/ui_status` formats `time(nullptr)` on the device with the configured timezone. The browser does not substitute its own wall-clock time; it only advances the last device-supplied timestamp between the existing 20-second status synchronizations.
- Clarified the clock tooltip to `Aktuelle Systemzeit des ESP32-S3-Moduls` / `Current ESP32-S3 module system time`.
- No RTC/NTP synchronization policy, timezone handling, status polling cadence, recording, Shooter, storage, encryption or transport behavior was changed.

## v70 — 2026-09-27

- Removed the duplicate Transport mode parameter block and duplicate activate/cancel controls from the general Configuration page. `transport_check_seconds`, `transport_light_confirm_seconds`, `transport_install_delay_seconds`, `transport_max_duration_seconds` and `transport_black_threshold` are now user-editable only on the dedicated **Transportsicherung** page.
- A normal Configuration save still preserves the currently active transport values in the canonical config text, but it no longer reads transport values from that form and no longer applies them as a second configuration path. The dedicated Transport save continues to persist the values through `configSaveTransportSettings()`.
- Simplified the Overview arming-status presentation. In the normal released state the large green `KAMERA SCHARF / Keine zeitliche Aufnahmesperre / Aufnahmeautomatik ist freigegeben` block is hidden completely; the existing compact `Bereit` status remains.
- Scheduled waiting, invalid RTC and invalid `recording_not_before` states remain visible because they require operator attention, but their arming notice is now rendered as a smaller one-pixel bordered status panel with reduced typography and padding. The recording safety/cooldown warning path is unchanged.
- No Transport runtime logic, timer-only deep-sleep behavior, threshold calculation, activation/cancel persistence, recording logic, Shooter logic, storage path or encryption path was changed.

## v69 — 2026-09-27

- Added a compact **Erweiterte Shooter-Einstellungen** section below the existing Power-Shooter controls.
- The passive `Bewegungsverdacht` classifier is now configurable without changing the ultra-sensitive shooter persistence filter: `shooter_motion_hint_change_pct` defaults to 2.0%, `shooter_motion_hint_required_hits` to 3 and `shooter_motion_hint_window_frames` to 4.
- Confirmation is constrained to 1..8 analyzed frames with `required_hits <= window_frames`; the hint threshold must not be lower than an enabled shooter persistence threshold. Existing v68 behavior remains the default (`2.0% / 3 of 4`).
- Older configs without the new keys remain valid and automatically use the v68 defaults. A normal WebConfig save writes the new canonical keys.
- Changing the hint parameters resets only the in-RAM hint vote window on the next Shooter analysis; frame persistence, Sparse-MKV timing, buffering, flush, encryption and normal Motion Recording remain unchanged.
- `/api/v1/shooter` exposes the three new hint settings additively.

## v68 — 2026-09-27

- Added a passive Power-Shooter **Bewegungsverdacht** classifier for Sparse-MKV periods. The existing shooter persistence filter is unchanged: `shooter_min_change_pct` still decides which JPEGs are retained, so the current very sensitive 0.2% field profile continues to prioritize evidence capture.
- The new hint uses the already computed shooter 20x15 analysis metrics and therefore adds no second JPEG decode and does not invoke the heavier operational Image Motion engine. A hint is confirmed when at least 3 of the latest 4 analyzed, non-dark shooter frames reach 2.0% changed area.
- Added a conservative global-light guard: a frame with at least 70% changed area plus a global mean-luma jump of at least 24 is treated as an illumination transition and resets the hint window instead of contributing to a likely-motion indication. Long capture gaps also reset the temporal window so unrelated activity cannot be combined across pauses.
- Motion-hint state never starts/stops recording, never changes shooter frame acceptance, and never changes Sparse-MKV frame timestamps, flush timing, encryption or PSRAM buffering. The confirmed flag is carried only in the existing RAM shooter queue until finalization.
- A Sparse MKV containing a confirmed hint is finalized as `HHMMSS_mmm_shooter_motionhint.mkv`; normal shooter files retain the existing `HHMMSS_mmm_shooter.mkv` name. No sidecar file and no additional SD write are introduced. Existing WebPlayer, Sync API, ZIP and delete paths remain extension-based and accept both names.
- The recordings list recognizes the filename marker and shows an orange **Bewegungsverdacht** badge with wording that explicitly identifies it as a hint rather than a confirmed alarm. Shooter flush logs now include `motion_hint=0|1` for field verification.

## v67 — 2026-09-26

- Restored the Seeed XIAO ESP32S3 Sense SPI-SD production clock from the temporary v66 10 MHz experiment to the previously qualified 20 MHz setting. `NORMAL` and `MAX` are both 20 MHz, so API-exclusive operation still never changes the card clock behind the recorder's back.
- Retained the v66 conservative 4 fps production cap at 1024x768 and the reduced WebConfig/Recording-Load polling cadence. These remain the active system-load controls while Image Motion is qualified separately.
- The 10 MHz experiment did not improve the observed thermal margin: a v66 4 fps load test entered the existing 80 C thermal emergency path before any storage failure occurred. The storage stack had already completed repeated 64 MiB raw, `w+` and full SFENC1 encrypted write-behind verification at 20 MHz.
- No SFENC1 format, write-behind, audio, recovery, thermal thresholds, Image Motion algorithm or recording-load thresholds were changed.

## v66 — 2026-09-26

- Changed the XIAO SPI-SD production policy from 20 MHz to 10 MHz for stability under combined camera, audio, SFENC1 and active WebConfig traffic. `NORMAL` and `MAX` are both 10 MHz in this profile, so API-exclusive operation no longer raises the card clock. Repeated v65 64 MiB raw, `w+` and full SFENC1 write-behind endurance tests had verified the isolated storage stack, while real 5 fps recording could still reproduce lower-layer `EIO` only with an actively polling foreground WebConfig page.
- Added a conservative XIAO production cap of 4 fps at 1024x768. Existing config files containing a higher value remain loadable; the runtime clamps that specific profile to 4 fps instead of invalidating the complete config, and WebConfig now exposes/saves the same cap. Factory defaults use 4 fps on XIAO. Other boards and lower-resolution weighted-performance policy remain unchanged.
- Reduced common WebConfig heartbeat/status/pause-lease polling from 10 s to 20 s and the Recording Load Test status poll from 5 s to 15 s. This preserves operator visibility and the existing pause lease while reducing concurrent WiFi/HTTP activity during recording qualification.
- Updated the SFENC1 endurance diagnostic wording for the new 10 MHz production clock. The existing code already performs its controlled 10 MHz fallback only when the production clock is above 10 MHz, so v66 never retries an `EIO` at the same clock.
- No SFENC1 format, cryptography, write-behind architecture, audio format, SD recovery semantics or recording-load rating thresholds were changed. Image Motion remains a separately selectable feature; the conservative v66 qualification target uses the direct/non-image-analysis path.

This file tracks the project release number used by `sensorforge_version.h`.
For each release, commit the complete project tree and create a Git tag with the
same name (`v35`, `v36`, ...). The firmware build timestamp remains independent
and identifies the concrete binary compilation time.

> Historical release notes below are reconstructed from the maintained project
> change sequence beginning with v23. Earlier development history remains in the
> repository history/project documentation.

## v65 — 2026-09-26

- Extended SD Maintenance with a 64 MiB endurance test through the exact production encrypted-recording storage stack: `RecordingWriteBufferedFile` -> 512 KiB PSRAM write-behind/drain task -> `RecordingStorageFile(encrypt=true)` -> SFENC1 -> SD.
- The new test writes deterministic data in 4 KiB producer calls, closes/finalizes through the normal encrypted path, then reopens the SFENC1 file through `RecordingStorageFile` and performs a complete logical decrypt/read-back verification.
- Reports write-behind initialization, capacity/high-water, producer waits, committed bytes, drain-call timing, physical encrypted size, logical read-back duration and the existing detailed storage error/EIO reason.
- The isolated SFENC1 phase closes the persistent logger and holds the global storage gate, ensuring no unrelated filesystem writer can perturb the diagnostic result. No failed media write is automatically retried on the same mount.
- If and only if the production-clock SFENC1 run fails with a hard EIO, the diagnostic cleanly remounts at 10 MHz and repeats the same 64 MiB encrypted-stack test once. It then restores the board production mount before returning. A production-clock failure remains a benchmark failure even when the 10 MHz diagnostic fallback passes.
- Existing raw FILE_WRITE/w+ endurance cases, FAT geometry checks, block-size tests, allocation comparison, clock sweep, recording format and SFENC1 on-disk format remain unchanged.

## v64 — 2026-09-26

- Added a hard storage-I/O fault latch for real VFS/SD `EIO` failures.
  `RecordingStorageFile` now marks the mount faulted when a physical
  `File::write()` or `File::seek()` fails with `EIO`; SFLOG1 low-level
  read/write helpers do the same. No failed media write is automatically
  retried or hidden.
- Made SD recovery fault-aware. When an EIO is latched, the persistent logger
  is closed without pushing its pending RAM queue through the already poisoned
  filesystem. The RAM log queue is preserved across the remount and can be
  persisted after the writer is reopened. The EIO latch is cleared only after
  a successful fresh SD mount.
- Recording Load Test now performs the same controlled SD recovery after a
  physical storage EIO that normal recording already uses. Cleanup operations
  are not issued against a mount that remains faulted, and the result reports
  whether SD recovery succeeded.
- Extended the existing non-destructive SD benchmark with two 64 MiB endurance
  passes using real 4 KiB writes plus complete read-back verification. The first
  uses normal `FILE_WRITE`; the second uses `w+`, matching the SFENC1 recording
  file mode. The test stops on the first short/EIO operation, records the
  failing offset/errno, performs no write retry, and remounts storage after a
  latched EIO.
- Kept SFENC1 crypto, on-disk format, 4 KiB crypto slice size, 512 KiB
  write-behind capacity, board SD clocks, recording timing and traffic-light
  thresholds unchanged.

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

### v84 candidate - Systemlastanzeige als Farbbalken
- Die permanente Systemlastanzeige im Header wurde von reinem Prozenttext auf einen kompakten visuellen Balken erweitert.
- Skala: links gruen, mittig orange, rechts rot; eine weisse Marke zeigt die aktuelle gemessene Last, der Prozentwert bleibt daneben sichtbar.
- Die bestehende FreeRTOS-Idle-Messung, Messrate und Status-Polling-Intervalle bleiben unveraendert; die Darstellung erzeugt keine zusaetzliche Firmware-Messlast.

### v84 candidate - system load / multi HTTP / RTSP diagnostics
- Systemlast-Messung korrigiert: ESP-IDF-Idle-Hooks laufen nicht exakt einmal pro Scheduler-Tick. Eine kurze Boot-Referenzkalibrierung ermittelt nun die reale Idle-Callback-Rate pro Core; die Anzeige vergleicht laufende Idle-Rate gegen diese Referenz und kann dadurch unter Last ausschlagen.
- HTTP-MJPEG unterstützt nun zwei echte parallele Viewer, die denselben zentral aufgenommenen JPEG-Frame erhalten. Ein dritter Viewer bekommt sauber HTTP 503 statt einen bestehenden Stream zu verdrängen.
- Dashboard zeigt HTTP-Viewer als 0/2, 1/2 oder 2/2 und zählt Stream-Verbindungen entsprechend.
- RTSP protokolliert OPTIONS/DESCRIBE/SETUP/PLAY sowie abgelehnte SETUP-Transportheader, damit VLC-Probleme eindeutig zwischen UDP-Anforderung, fehlenden interleaved-Kanälen und späterem PLAY-Problem unterschieden werden können.

### v84 candidate - Arduino-ESP32 3.3.12 WiFiClient const compile fix
- RTSP-Slot-Verbindungspruefung an Arduino-ESP32 3.3.12 angepasst: `NetworkClient::connected()` und `operator bool()` sind dort nicht `const`.
- `rtspSlotConnected()` und alle betroffenen lokalen RTSP-Slot-Referenzen verwenden deshalb keine `const`-Qualifizierung mehr.
- Keine Aenderung an Streamer-Limits, RTP/RTSP-Verhalten oder Kamera-/Audio-Ownership.
