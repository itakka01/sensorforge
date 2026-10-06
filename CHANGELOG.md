## v87-beta24 — 2026-10-03

- Moved the user-facing VPN/WireGuard entry into `WiFi Einstellungen`; the separate navigation item and standalone configuration page are no longer exposed. Existing `/wireguard` bookmarks redirect to the VPN section inside WiFi settings.
- Reworked the parked WireGuard presentation as a compact product-status card with a clear `Derzeit nicht verfügbar` state. Internal backend names, runtime/debug state, tunnel address, peer/key fields and implementation details are intentionally not shown to end users while the feature is unavailable.
- Removed the WebConfig WireGuard save/configuration form and its POST route from the active UI. The prepared config/secret/API scaffolding remains in the source for later continuation, but the current Web UI cannot enable or edit an unqualified VPN backend.
- WireGuard remains deliberately parked and no third-party WireGuard library is linked. Camera, RTSP, ONVIF, audio, recording, storage, WiFi runtime and firmware-update behavior are unchanged. Host API remains 1.22.

## v87-beta23 — 2026-10-03

- Restored a consistent, build-independent SensorForge reference state after the Beta-21 WireGuard backend experiment exposed that `WireGuard-ESP32` 0.1.5 still depends on removed `tcpip_adapter` APIs under Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5.
- Current XIAO and Freenove profiles deliberately set `BOARD_WIREGUARD_BACKEND_ARDUINO=0`. The installed legacy Arduino library is therefore not included or linked and cannot break the SensorForge build.
- WireGuard architecture/configuration/UI/API scaffolding is retained for later continuation: board capability, single-peer settings, SFSEC1 private-key persistence, runtime status fields and the management page remain in the source.
- Runtime activation is intentionally parked until a maintained ESP-NETIF-compatible backend is selected and qualified. The WebConfig activation control is disabled when no backend is present, and server-side save handling also forces `wireguard_enabled=0` so a forged/stale POST cannot activate an unavailable backend.
- WebConfig now reports the backend as deliberately deferred rather than incorrectly suggesting that the user merely forgot to install a library. API capability continues to report `wireguard_client=false` while backend availability is false.
- The local patch-tool approach considered for Beta 22 is explicitly rejected for the authoritative source line. Beta 22 was not applied to the field source and no third-party library source patch is required by Beta 23.
- Future WireGuard work should resume only with a maintained standard/standard-near ESP-NETIF backend (or after the separately qualified Arduino-ESP32 4.x / ESP-IDF 6.x migration). The current Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 reference stack remains unchanged.
- No camera, RTSP, ONVIF, audio, recording, storage, WiFi or firmware-update runtime path was changed. Host API remains 1.22.

## v87-beta21 — 2026-10-03

- Fixed WireGuard backend discovery with Arduino builds. Beta 20 used `__has_include(<WireGuard-ESP32.h>)`; Arduino library dependency discovery can evaluate that before the external library include path has been added, leaving a successfully compiled firmware reporting `Backend: none` even when WireGuard-ESP32 0.1.5 is installed.
- Current XIAO and Freenove board profiles now explicitly select the `WireGuard-ESP32` Arduino backend. This creates a real `#include <WireGuard-ESP32.h>` dependency so the Arduino builder can resolve and link the installed library deterministically.
- WireGuard remains optional at runtime and still defaults to OFF. No tunnel is created unless the existing `wireguard_enabled` configuration is explicitly enabled and all runtime gates pass.
- For the current XIAO/Freenove profiles, WireGuard-ESP32 is therefore a build dependency when using the Beta-21 source. A future board/backend profile may set `BOARD_WIREGUARD_BACKEND_ARDUINO=0` to omit it.
- No WireGuard handshake/routing logic, WebConfig config semantics, camera, RTSP, ONVIF, audio, recording or storage path was changed. Host API remains 1.22.
- This change intentionally exposes the real compatibility test next: the classic WireGuard-ESP32 0.1.5 backend still contains legacy `tcpip_adapter` usage, so Arduino-ESP32 3.3.12 may now reveal a backend compile incompatibility that Beta 20 had hidden by not linking the library. If so, the next fix must address the backend itself rather than masking it again.

## v87-beta20 — 2026-10-03

- Implemented the first deliberately conservative SensorForge WireGuard client foundation. It is optional, single-peer/client-only and defaults to OFF on the current XIAO and Freenove ESP32-S3 profiles; old configurations therefore retain their previous behavior.
- Added additive persistent configuration for tunnel address, private key, peer endpoint/public key and peer port. The private key is handled by the existing SensorForge secret/SFSEC1 persistence path and is never rendered back into the Web UI, GET APIs or logs.
- Added a dedicated administrator-only `System -> WireGuard VPN` page. Changes are persist/reboot-applied rather than hot-swapping a live management tunnel underneath an active session.
- The runtime VPN adapter starts only after infrastructure STA WiFi is connected and SensorForge has valid system time. It is stopped before WiFi teardown/recovery, and retry attempts are suppressed while recording or active streamer clients own latency-sensitive paths.
- The source remains buildable with WireGuard disabled when no external backend library is present. The initial Arduino backend adapter is detected at build time through `WireGuard-ESP32.h`; absent backend means `backend unavailable`, not a firmware failure. Because the classic Arduino library predates Arduino-ESP32 3.x/ESP-IDF 5.x, real 3.3.12 compatibility must be hardware/build-qualified before production use; an ESP-NETIF-compatible maintained backend remains preferred for the later production freeze.
- Host API is additively raised to 1.22. Device/state/capability/network telemetry reports the configured/active/backend state without exposing the private key. Generic API config writes use the same canonical config/secret path and remain reboot-applied.
- Added board capability gates. Current XIAO/Freenove profiles are allowed to test WireGuard but runtime default remains OFF. RTSP/ONVIF/snapshot traffic through the tunnel is not yet qualified and no media/encoder pipeline was modified.
- Fixed the Beta-18 read-only ONVIF Imaging endpoint integration in the common access-control/CSRF middleware: `/onvif/imaging_service` is now treated consistently with the existing Device/Media SOAP endpoints instead of being rejected as a browser POST.
- ESP-IDF 6.x is intentionally NOT part of this change. SensorForge remains on the current Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 reference stack; a later Arduino-4/IDF-6 migration must be a separate branch/release with full camera, SD, I2S/audio, WiFi, sleep, OTA and streamer regression testing.
- Validation in this workspace is limited to static/source-level checks and isolated adapter syntax tests. A real Arduino-ESP32 3.3.12 build plus XIAO/Freenove tunnel, reconnect, heap/thermal and failure-path tests are mandatory before calling WireGuard production-qualified.

## v87-beta19 — 2026-10-03

- Documentation-only roadmap checkpoint; runtime source behavior remains unchanged from v87-beta18.
- Added the planned optional WireGuard client as a future SensorForge remote-management transport. XIAO and Freenove ESP32-S3 profiles must default it to OFF, and firmware updates must never enable it implicitly.
- Planned rollout is deliberately staged: management/WebConfig/API first, then board-specific CPU/heap/thermal/network qualification, and only afterwards optional RTSP/ONVIF/snapshot traffic through the tunnel.
- WireGuard must remain a transport underneath the existing SensorForge services; it must not introduce a second camera/streaming pipeline, transcoding path or new media owner.
- Future implementation must be board-gated and use additive configuration with secure defaults. Private WireGuard keys are secrets and must use the then-current SensorForge secret persistence path and never be exposed by GET APIs, Web UI reads, logs or diagnostics.
- Initial scope is client-only: no generic VPN router, LAN gateway or WireGuard server on the device. The goal is safe remote access without direct Internet port forwarding.
- Required future qualification includes WiFi reconnect, endpoint/DNS failure, bad-key/config recovery, reboot, Sleep/Wake interaction, long-term heap/thermal behavior and concurrent management/streamer load.
- v87-beta19 is the new authoritative re-entry/documentation stand. Host API remains 1.21 and Integration/App profile remains 1.1.

## v87-beta18 — 2026-10-03

- Added ONVIF compatibility round 2 without changing the field-qualified camera/RTSP/RTP-JPEG, HTTP-MJPEG, audio, recording, storage or download pipelines. All new ONVIF operations are read-only.
- Device service now answers `GetNetworkInterfaces`, `GetNetworkProtocols`, `GetDNS` and `GetNTP` from the live SensorForge WiFi/network state and the same NTP servers used by the firmware.
- Media service now also supports singular `GetVideoSourceConfiguration` and `GetVideoEncoderConfiguration` requests in addition to the already supported list/profile/URI methods.
- Added a read-only ONVIF Imaging service at `/onvif/imaging_service` with `GetServiceCapabilities`, `GetImagingSettings` and `GetOptions`. It reports the existing SensorForge exposure mode only; it does not add ONVIF camera-setting writes.
- Added lightweight ONVIF SOAP diagnostics to `/streamer_status`: request count, SOAP fault count and maximum generated SOAP response size. These counters are control-plane only and add no per-frame work.
- Added `sensorforge_onvif_test.py`, a dependency-free regression smoke test covering WS-Discovery, WS-Security SOAP reads, Device/Media/Imaging calls, snapshot JPEG retrieval and RTSP DESCRIBE.
- Added separate board/streamer H.264 capability gates. XIAO and Freenove remain explicitly disabled; ONVIF must not advertise H.264 until both a board profile and a qualified SensorForge streamer implementation enable it. No H.264 encoder or transcoder was added in this release.
- Tightened operation-name matching so singular read methods cannot accidentally match plural method names in unprefixed SOAP. Host API remains 1.21 and Integration/App profile remains 1.1.
- Validation in this workspace: `onvif.cpp` passes an isolated C++17 syntax build against Arduino-compatible stubs and `sensorforge_onvif_test.py` passes Python bytecode compilation/help invocation. A full Arduino-ESP32 3.3.12 build and renewed XIAO/Freenove/Onvier/NVR hardware regression remain required.

## v87-beta17 — 2026-10-03

- Product-identity cleanup for the already field-qualified ONVIF integration; no discovery, authentication, snapshot, RTSP/RTP, HTTP-MJPEG, audio, recording or download behavior changed.
- `GetDeviceInformation` no longer hard-codes the SensorForge product identity or exposes the development-board name as the ONVIF model. `Manufacturer` now comes from `Branding::APP_NAME`, and `Model` comes from `Branding::PLATFORM`.
- Added the central `BOARD_DISPLAY_NAME` to each active profile in `board_config.h`. ONVIF `HardwareId` now reports that board-profile value (`Seeed XIAO ESP32S3 Sense` or `Freenove FNK0085 ESP32-S3 WROOM`) instead of the generic `ESP32-S3`.
- With the current branding this yields `Manufacturer=SensorForge`, `Model=Fabric Node`, the actual board in `HardwareId`, and the current release tag in `FirmwareVersion`. Future product/board renames therefore follow the existing central headers instead of requiring ONVIF-specific edits.
- v87-beta16 remains the field-qualification evidence for ONVIF interoperability; v87-beta17 is the new authoritative source/re-entry stand. Host API remains 1.21 and Integration/App profile remains 1.1.

## v87-beta16 — 2026-10-03

- Documentation/version closeout for the ONVIF MVP after successful real Android/Onvier field qualification. Runtime source code is unchanged from v87-beta15.
- Automatic WS-Discovery is again confirmed working on the real XIAO test device. Onvier discovers SensorForge without manual IP entry.
- The same field test confirms ONVIF device/media queries, existing SensorForge credential authentication, snapshot access and RTSP/JPEG video playback. Onvier reports video streaming success with ONVIF service/snapshot on port 80 and RTSP on port 554.
- The implemented scope is intentionally described as a SensorForge ONVIF basic/MVP integration, not as official ONVIF certification or full Profile-T conformance.
- ONVIF is considered complete/frozen for the current product scope. Further changes should only be made for a concrete reproducible interoperability issue with another relevant NVR/client.
- Host API remains 1.21 and Integration/App profile remains 1.1.

## v87-beta15 — 2026-10-03

- Compile fix for the Beta-14 ONVIF discovery rollback. `onvifBegin()` still contained initialization of the removed Beta-13 `discoveryInstanceId` / `discoveryMessageNumber` AppSequence state, although those variables had intentionally been removed when restoring the known-working Beta-9 discovery wire format.
- Removed only those stale references. ProbeMatch/ResolveMatch/Hello/Bye payloads, Beta-10/11 authentication fixes, Beta-12 retry logic and Beta-13 discovery diagnostics remain unchanged.
- This fixes the Arduino-ESP32 3.3.12 errors `discoveryInstanceId was not declared in this scope` and `discoveryMessageNumber was not declared in this scope`. No runtime media or ONVIF behavior is otherwise changed.

## v87-beta14 — 2026-10-03

- Fixed the remaining automatic ONVIF discovery regression using the real Beta-13 field diagnostics. Onvier probes reached SensorForge (`onvif_probe_rx` increased), SensorForge successfully transmitted ProbeMatch replies (`onvif_probe_match_tx` increased), and the remote endpoint was the Android client, proving that multicast reception and UDP reply transport were working while the client rejected the response payload.
- Compared the discovery wire format against v87-beta9, the last field-proven build that Onvier discovered automatically. Beta 13 had changed the ProbeMatch/Hello/Bye headers, XAddr advertisement and `Types` field. In particular, Beta 9 advertised `tds:Device dn:NetworkVideoTransmitter`, while Beta 13 had dropped `tds:Device`.
- ProbeMatch, ResolveMatch, Hello and Bye are therefore restored to the exact Beta-9-compatible envelope/body format, including `tds:Device dn:NetworkVideoTransmitter` and the single established Device-Service XAddr. The later AppSequence/mustUnderstand/multi-XAddr experiment is removed instead of stacking further speculative discovery changes.
- Beta-10/11 WS-Security authentication fixes, Beta-12 startup retry and Beta-13 discovery counters remain intact. Manual ONVIF setup, RTSP video and snapshot behavior are unchanged. No camera, RTSP/RTP, HTTP-MJPEG, audio, recording or download data path changed.
- Static comparison confirms that the current `discoveryEnvelope()`, `discoveryMatchBody()` and `sendHello()` payload generators are byte-for-byte identical in source form to the known-working Beta-9 implementations; only the newer diagnostics/retry remain around them. Real automatic Onvier discovery retest is required.

## v87-beta13 — 2026-10-03

- Fixed the remaining automatic ONVIF discovery interoperability issue after a real Android/Onvier test proved that manual IP setup, WS-Security authentication, snapshot/RTSP ports and video streaming all work while automatic device discovery still fails. The fault domain is therefore limited to WS-Discovery.
- ProbeMatch/Hello/Bye messages now include a WS-Discovery `AppSequence` plus WS-Addressing `mustUnderstand` attributes consistent with the ONVIF reference exchange. The per-boot InstanceId is random/non-zero and MessageNumber advances monotonically for discovery messages.
- Discovery now advertises all usable local Device Service addresses in `XAddrs` when WiFi is in AP+STA mode instead of always preferring the AP address. This avoids returning an unreachable AP address to a client probing from the infrastructure WLAN.
- Discovery advertises the legacy-compatible `dn:NetworkVideoTransmitter` device type used by ONVIF discovery examples and expected by many older clients. SOAP Device/Media functionality is unchanged.
- Added lightweight diagnostics to `/streamer_status`: `onvif_probe_rx`, `onvif_probe_match_tx` and `onvif_last_remote`. They add no media-path processing and make future discovery failures immediately distinguishable between missing incoming Probes and rejected ProbeMatch responses.
- Beta-12 startup retry and Beta-11/10 authentication fixes remain intact. No RTSP, JPEG, HTTP-MJPEG, audio, recording or download path changed. Host API remains 1.21 / Integration profile 1.1.
- The ONVIF module passes the isolated Arduino-ESP32-3.3.12 compatibility syntax build. Real Onvier automatic-discovery retest remains required.

## v87-beta12 — 2026-10-03

- Hardened ONVIF WS-Discovery startup after a real Beta-11 field observation where the camera was no longer discovered after reboot/update. Discovery startup previously ran only once when the streamer started.
- If IPv4/network readiness or the UDP multicast bind is not yet available at that exact moment, ONVIF now retries the discovery join every 5 seconds while streamer mode, RTSP and `onvif_enabled` remain active. A failed multicast socket is stopped before retrying.
- Retry work is performed only while discovery is inactive and consists of a bounded network-state check/multicast bind attempt; it does not touch camera capture, RTSP/RTP, HTTP-MJPEG, audio, recording or download paths.
- Beta-11 UsernameToken/XML parsing remains unchanged. Host API remains 1.21 / Integration profile 1.1.
- The updated module passed the isolated C++ syntax build against the Arduino-ESP32 3.3.12 compatibility stub. Real XIAO/Freenove WS-Discovery retest remains required.

## v87-beta11 — 2026-10-03

- Fixed the remaining ONVIF/Onvier authentication failure after Beta 10. Real WS-Security UsernameToken requests commonly encode `Password` and `Nonce` with attributes such as `Type="...#PasswordDigest"` and `EncodingType="...#Base64Binary"`.
- The previous lightweight XML helper only recognized opening tags that ended immediately after the element name, so it silently failed to extract attributed `Password`/`Nonce` elements and rejected an otherwise valid UsernameToken with HTTP 401.
- ONVIF text-element parsing now matches XML local names independently of namespace prefix and opening-tag attributes, and decodes the standard XML text entities. This also makes profile/address parsing more tolerant without introducing a general-purpose XML dependency.
- The PasswordDigest calculation, configured administrator/streaming-user credentials, PRE_AUTH behavior and HTTP Digest fallback are unchanged. No media, RTSP, HTTP-MJPEG, snapshot, audio, recording or download path changed.
- The parser was exercised with a representative WS-Security UsernameToken containing attributed `Password` and `Nonce` elements; isolated C++ syntax compilation passed. Full Arduino-ESP32 3.3.12 build and renewed Onvier/NVR hardware test remain required.

## v87-beta10 — 2026-10-03

- Fixed ONVIF authentication interoperability found during the first real Android/Onvier test. Beta 9 challenged every ONVIF SOAP request in the central WebConfig middleware, so even ONVIF `PRE_AUTH` operations such as `GetSystemDateAndTime` incorrectly returned HTTP 401.
- ONVIF Device operations defined as pre-authentication reads in the implemented subset (`GetSystemDateAndTime`, `GetCapabilities`, `GetServices`, `GetServiceCapabilities`, `GetHostname`) now reach the Device service without credentials as required by the ONVIF Core access model.
- Added WS-Security UsernameToken `PasswordDigest` verification for authenticated ONVIF SOAP requests. The verifier uses the existing SensorForge administrator and streaming-user credentials and validates the standard `Base64(SHA1(Base64Decode(Nonce) + Created + Password))` form; plaintext SOAP passwords are not accepted.
- Existing HTTP Basic/Digest compatibility remains available for ONVIF clients that use HTTP-layer authentication. `GetServiceCapabilities` now advertises both `UsernameToken=true` and `HttpDigest=true`.
- ONVIF authentication is now owned by the ONVIF SOAP handlers rather than the generic browser access middleware, while the endpoints remain excluded from browser CSRF handling because they expose read-only machine-to-machine operations.
- No RTSP, HTTP-MJPEG, snapshot, audio, recording or download media path changed. Host API remains 1.21 / Integration profile 1.1.
- The PasswordDigest calculation was cross-checked against a known Onvifer/Onvier-compatible request vector. A full Arduino-ESP32 3.3.12 build and renewed real Onvier/NVR test remain required.

## v87-beta9 — 2026-10-02

- Added an optional `onvif_enabled` integration for streamer mode. It defaults to off and requires the existing RTSP output; older configurations without the key remain unchanged.
- Added lightweight WS-Discovery on UDP multicast `239.255.255.250:3702`, including Probe/Resolve responses plus Hello/Bye lifecycle announcements. Discovery work is bounded to at most two datagrams per streamer loop pass.
- Added read-only ONVIF Device and Media SOAP endpoints at `/onvif/device_service` and `/onvif/media_service`. The initial interoperability set covers device information/capabilities/scopes/time/hostname and media profiles, JPEG source/encoder information, RTSP stream URI and snapshot URI.
- ONVIF does not create a second camera or media path. It advertises the existing SensorForge RTSP/RTP-JPEG stream and `/snapshot`; RTSP, HTTP-MJPEG, audio, recording and download data paths are otherwise unchanged.
- When SensorForge access protection is enabled, ONVIF web-service requests use the existing administrator/streaming-user credentials and are challenged with HTTP Digest. The machine-to-machine ONVIF SOAP POSTs are intentionally outside the browser CSRF-token contract and expose no mutating ONVIF actions.
- Added ONVIF configuration/status visibility to WebConfig, streamer diagnostics and API host protocol **1.21**. Integration profile remains **1.1**.
- This is a pragmatic ONVIF interoperability layer, not an ONVIF Profile T conformance claim. SensorForge continues to stream JPEG/MJPEG over RTSP; no H.264/H.265 transcoder or additional media encryption/load was introduced.
- Static/source checks were performed, including an isolated C++ syntax build of the new ONVIF module against Arduino-ESP32-3.3.12 API signatures. A full Arduino build plus real NVR/WS-Discovery tests on XIAO/Freenove remain required.

## v87-beta8 — 2026-10-02

- Documentation-only security clarification; no firmware runtime logic changed from v87 Beta 7.
- Made the current WebConfig/API threat model explicit: HTTP Basic provides access control but not transport encryption, so management access is intended only through the SensorForge hotspot or a trusted/isolated local network, without direct Internet exposure or port forwarding.
- Added HTTPS/TLS for WebConfig and `/api/v1` as a deliberate future hardening item, especially for later boards with more available resources or deployments in untrusted/shared networks.
- Future management TLS must be evaluated separately from RTSP, HTTP-MJPEG, media playback and file-transfer data paths so time-critical media traffic is not automatically burdened with additional encryption/CPU cost.

## v87-beta7 — 2026-10-02

- Fixed an Arduino-ESP32 compile error in `web_csrf.cpp`: Arduino's `Print.h` defines `HEX` as a macro (`#define HEX 16`), which collided with the local hexadecimal lookup-table name used by `randomToken()`.
- Renamed the local table to `HEX_DIGITS`; token generation, token length, role separation and all CSRF runtime behavior are unchanged from v87-beta6.
- No stream, download, API, configuration or authentication behavior changed. This is a compile-only correction discovered by the real Arduino-ESP32 3.3.12 build.
- Hardware/browser regression requirements from v87-beta6 remain open after a successful full build.

## v87-beta6 — 2026-10-02

- Added centralized server-side CSRF protection for state-changing browser `POST` routes. A fresh 128-bit token is generated per boot, with separate trust tokens for administrator and streaming-user roles.
- The common WebConfig shell automatically attaches the role token to same-origin POST forms and `fetch()` calls. Existing page handlers therefore do not each carry independent CSRF logic.
- The standalone WebPlayer receives the streaming/admin token through a SameSite cookie and appends it only to its mutating POST requests; frame, audio, video, snapshot, stream and download GET traffic is unchanged.
- Config and firmware multipart uploads perform an additional early token check before accepting upload content, because their upload callbacks can run before the normal final route handler.
- `/api/v1` is intentionally excluded from the browser CSRF token contract so existing Sync/App clients remain compatible and continue to use their established administrator authentication. Existing firmware/license action tokens remain in place as additional transaction-specific checks.
- No encryption, hashing or additional processing was added to RTSP, HTTP-MJPEG, media playback/download or recording data paths. Runtime overhead is limited to token comparison on mutating browser actions.
- Static/source review performed in this environment; browser/hardware regression remains required for admin forms, streaming-user annotation/delete permissions, multipart uploads and stale-page behavior across reboot.

## v87-beta5 — 2026-10-02

- Fixed the factory/recovery hotspot startup mismatch. Configuration and validation already define an empty `hotspot_password` as a valid open AP, but `startConfiguredHotspot()` previously rejected every password shorter than 8 characters and could therefore disable WebConfig after factory reset or full config fallback.
- `startConfiguredHotspot()` now follows the same policy as the config layer: empty password starts an open AP, 8..63 characters start a protected AP, and invalid non-empty 1..7-character values remain rejected.
- The open-AP branch passes a null passphrase explicitly to `WiFi.softAP()`; protected hotspot, fallback hotspot, SSID, hidden-SSID and TX-power behavior are otherwise unchanged.
- Static/source review performed only. Hardware regression still required for factory reset/default boot, open recovery AP, protected AP and fallback AP on XIAO/Freenove.

## v87-beta4 — 2026-10-02

- Consolidated the firmware signing/key-management workflow without changing the Beta-3 device-side verification behavior. Signed `.sfw` remains mandatory for SD and WebConfig/WiFi updates; Secure Boot, eFuse provisioning and anti-rollback remain intentionally out of scope.
- Extended `sensorforge_firmware_sign.py` with `keygen`: it creates the default `sensorforge_firmware_private.pem` / `sensorforge_firmware_public.pem` pair and automatically creates or replaces `firmware_public_key.h` from the new public key. Existing PEM files are never overwritten.
- Added `--header` and `--no-header` controls for key generation plus `update-header` for synchronizing `firmware_public_key.h` from an existing public PEM.
- `sign` now uses `sensorforge_firmware_private.pem` by default; `--key` remains available for an explicitly selected private PEM. Help output documents the complete keygen → rebuild → sign workflow.
- Direct USB/programmer flashing of normal `.bin` images remains unchanged. The private signing key must remain outside source/release archives.
- Release identity and re-entry documentation promoted to **v87 Beta 4**. Hardware qualification of the signed SD/WiFi update paths remains open.

## v87-beta3 — 2026-10-02

- Added application-level firmware authentication for the two remotely/removably supplied update paths: SD auto-update and WebConfig/WiFi upload now require a signed SensorForge `.sfw` package.
- Added a dedicated firmware signing trust anchor, separate from the license key. Verification uses ECDSA P-256 / SHA-256; only the public key is compiled into the device.
- SD update validates the complete package signature before `Update.begin()` / `Update.write()` and flashes only the embedded raw ESP application image. Unsigned legacy `.bin` files are no longer auto-installed from `/firmware`.
- WiFi upload parses the signed package header, streams only the raw application into the inactive OTA partition, hashes while writing, verifies the signature before `esp_ota_end()`, and still requires the existing explicit INSTALL action before changing the boot partition.
- Existing board compatibility markers, ESP image validation, recording gates, first-boot SD guard and update metadata remain in place. No Secure Boot, flash encryption, eFuse consumption or anti-rollback policy is added in this beta.
- Added `sensorforge_firmware_sign.py` for `.bin` -> `.sfw` release packaging and key generation. Source-archive helper now excludes `*.pem`.
- Static/source and signing-tool tests were performed in this environment; an Arduino build and XIAO/Freenove hardware update test remain required.

## v87-beta2 — 2026-10-02

- Expanded the local `/api/v1` protocol additively from host protocol 1.12 / Integration profile 1.0 to **host protocol 1.20 / Integration profile 1.1**. Existing Sync/API routes and fields remain available.
- Added administrator-only capability discovery plus comprehensive persistent configuration read/schema/partial-write endpoints. Partial writes preserve unrelated config text, pass through the existing `configValidateText()` / `configSaveText()` authority, never return secrets and intentionally use a persist-then-reboot model instead of hot-applying mixed camera/network/ownership state.
- Added extended network, streamer, audio, image-motion, recording, radar and system telemetry for application clients. Network scan reuses the existing WebConfig scan path and is blocked during active recording or active streamer clients.
- Added explicit runtime actions for manual recording start/stop, image-motion background reset, radar configuration/calibration, transport-mode transition, delayed reboot and delayed software shutdown. Existing SensorForge ownership/safety functions remain authoritative; the API does not bypass streamer ownership, recording/storage locks or radar validation/verification.
- Added config-copy status/control (`internal` / SD copy) while preserving the established config-source policy.
- Added app-facing media annotation read/write, safe finalized-media deletion using the existing deferred-delete/storage gates, and bounded plaintext log read/clear over the established log-storage layer.
- Firmware upload, license management and generic factory reset remain on their existing specialized WebConfig paths for now; this avoids duplicating high-risk transaction/safety logic merely to increase endpoint count.
- Updated `SENSORFORGE_API_V1.md` as the authoritative application/API contract and promoted the changed worktree from v87 Beta 1 to **v87 Beta 2**.
- Static/source review only in this environment; no Arduino compile or new hardware qualification is claimed by this API expansion.

## v87-beta1 — 2026-10-02

- Promoted the consolidated post-v86 worktree to the first **v87 Beta 1** checkpoint. Core branding remains 7.1.0; **v86 remains the last stable official release** while v87 Beta 1 becomes the authoritative development, qualification and re-entry basis.
- Declared a feature freeze for this beta line: further work should prioritize stability, regression testing, power/sleep behaviour and UI polish instead of adding broad new features.
- Added optional daily WiFi-alive scheduling with up to 16 local-time windows. A scheduled window keeps WiFi/WebConfig available independently of the normal inactivity timeout, integrates its next start into timer wake planning, and never tears down an active Web/API or API-exclusive transfer merely because the window ended. Existing magnet wake and normal WiFi behaviour remain additive and unchanged outside scheduled windows.
- Changed only the **automatic fallback hotspot** to use the board default/full configured board TX level for maximum recovery reachability. Infrastructure WiFi and a deliberately selected normal hotspot continue to use the user-configured TX-power setting; the stored setting is not overwritten.
- Consolidated the general WebConfig layout without removing configuration options: recording parameters are visually grouped, general LED/sleep/bootloop/timezone/web/debug settings share one section, context info buttons were added, and release/build identity is presented under the System firmware section.
- Integration API behaviour remains unchanged for Beta 1 and administrator-only. The next dedicated development track is to expand `/api/v1` additively toward near-complete application control for the planned Android client without breaking existing Sync/API clients.
- Documentation was corrected to reflect the already-supported AP/STA/multi-WiFi/fallback network operation. No new Arduino build or hardware long-run qualification is claimed by this documentation/version promotion.

## v86 — 2026-10-01

- Promoted the consolidated post-v85 worktree to the official **v86** release. Core branding remains 7.1.0; v85 is the preceding Image-Motion/WebConfig checkpoint.
- Added role-aware WebConfig access with the existing Web credentials retained as the **Administrator** account plus up to five additional **Streaming-Benutzer**. Streaming users can access live streams, snapshots and recorded media without receiving general configuration, maintenance, reboot or control privileges.
- Added global optional streaming-user permissions for **recording annotation editing** and **recording deletion**. Both permissions default to disabled; when disabled, the corresponding UI controls are hidden and the server-side POST routes remain denied.
- Protected the RTSP and direct HTTP-MJPEG outputs when SensorForge web access protection is enabled. Both administrator and streaming-user credentials are accepted for stream access; disabling web access protection keeps the streams unauthenticated. RTSP uses Basic authentication and therefore provides access control but not transport encryption.
- Added a dedicated **System > Benutzer & Zugriff** page and removed the redundant access-management link from the general configuration page. The administrator and streaming-user roles are now explained in one place.
- Simplified the streaming-user navigation: the restricted start page remains the normal entry point with only **Viewer** and **Aufnahmen** visible; **Viewer** leads directly to the live view, while recordings remain available only through their dedicated top-level menu item.
- Expanded the Streamer page with authenticated RTSP example URLs (`rtsp://BENUTZER:PASSWORT@<IP>:554/stream`) and clear Home Assistant setup guidance. The documented Home Assistant path uses the **MJPEG IP Camera** integration with `http://<IP>:81/stream` plus `http://<IP>/snapshot` and separate username/password fields. The MJPEG/snapshot integration path was successfully exercised in Home Assistant during this development cycle.
- Kept `/api/v1` authentication intentionally administrator-only. Streaming-user credentials grant media/stream access but do not become API control credentials.
- Fixed post-v85 multi-WiFi secret persistence: `wifi_pass_2..5` are recognized by the secret whitelist and empty unused profile passwords are no longer sent through secret encryption. This prevents failed saves when additional WiFi profiles are configured.
- Corrected the dedicated WiFi save-call signature for `hotspot_fallback_enabled` and restored the missing `webconfigWifiScanJson()` implementation while preserving the latest multi-profile/scan/fallback/TX-power UI.
- Before release promotion, the current v86 source basis was re-audited against the latest delivered deltas. The final streaming-user root-page correction is included: visiting `/` stays on the restricted main page instead of redirecting directly to the live view.
- Release documentation does not claim a fresh Arduino build performed in this environment. The final authenticated RTSP/HTTP access paths should continue to be verified on the target hardware as part of normal release qualification.

## v85 — 2026-10-01

- Promoted the current source tree to the official **v85** checkpoint. v84 remains the preceding streamer/network release baseline; v85 primarily consolidates the Image-Motion/WebConfig work completed afterwards.
- Unified **Image Motion** with the fixed responsive save-bar interaction used by WiFi/camera settings. Drawing the ROI mask, changing parameters or inserting defaults now marks the page as having unsaved changes; persistence remains explicit rather than automatic.
- Simplified the Image-Motion page for normal users. Long explanatory text moved behind info buttons, field labels were shortened, and the obsolete one-shot **Bildanalyse testen / Testergebnis** workflow was removed.
- Reworked the live analysis into a single continuously updated status view showing **Aktuelle Bewegung** (frame-to-frame change), **Abweichung vom Hintergrund**, trigger threshold, confirmation progress and last motion. Technical/raw diagnostics remain available in a collapsed **Technische Details** section.
- Renamed the background/reference reset action to the user-facing **Hintergrund neu lernen** and placed it directly with the live-analysis controls. This resets the learned background only; the recording decision algorithm is otherwise unchanged.
- Fixed Image-Motion live analysis while operating in **streamer mode**: the analyzer now consumes the already captured central streamer JPEG frame instead of attempting a second camera capture. This preserves exclusive camera ownership and allows WebConfig live motion diagnostics in streamer mode.
- The additional frame-to-frame measurement remains diagnostic only and does **not** change the existing Image-Motion recording-trigger decision, which continues to use the established background/connected-area/confirm-release logic.
- The separately tracked Recording/Storage long-term qualification and the existing Image-Motion field-quality qualification remain open where previously documented. This release documentation does not claim an additional Arduino build performed in this environment.

## v84 — 2026-09-30

- Promoted the complete v84 worktree to the official **v84** release. The detailed v84 candidate/development entries below remain as the implementation history for this release.
- Added the optional **Network Streamer** operating mode with RTSP/RTP-JPEG over interleaved TCP, HTTP-MJPEG, optional L16 audio, RTCP sender reports, multi-client timing fixes, bounded real-time audio draining and streamer diagnostics. The tested XIAO streamer path reached the accepted field state with up to 2x RTSP + 2x HTTP-MJPEG and active 16-kHz/16-bit/mono audio.
- Added AP/STA service-network selection, board-safe configurable WiFi TX power, up to five prioritized external WiFi profiles, on-demand WLAN scan and optional hotspot fallback. Legacy `wifi_ssid` / `wifi_pass` remain profile 1 for backward compatibility.
- Moved WiFi configuration into **System > WiFi Einstellungen** and introduced the fixed responsive save bar. The same save interaction is now used on the normal and advanced camera-settings pages.
- Unified camera live preview around the existing `/snapshot` route. In streamer mode the preview reuses the central streamer frame pipeline instead of starting a competing camera capture owner.
- Consolidated OV3660 crop persistence into the central camera save workflow. The separate crop save button is removed; in streamer mode a changed crop is persisted by the normal camera save and becomes active after reboot rather than attempting an unsafe live crop change.
- Preserved the v83 compatibility rule: missing new streamer/WiFi keys still start normal SensorForge behavior, and existing recording/shooter/sleep configuration is not overwritten merely by selecting streamer mode.
- Known accepted streamer residual: an occasional startup click can still occur when opening an RTSP audio stream; the previous continuous ring-overflow crackling is resolved in the qualified streamer path.
- Release decision/status was confirmed on 2026-09-30. The separately tracked long-term Recording/Storage qualification remains independent of the v84 streamer/network/UI release status. This documentation promotion itself does not claim an additional Arduino build performed in this environment.

## v84-candidate - 2026-09-30 - dedicated WiFi configuration, prioritized profiles and optional hotspot fallback

- Moved WiFi/network settings out of the general configuration page into the dedicated **System > WiFi Einstellungen** page. `webconfig_wifi.cpp/.h` now owns rendering, validation, scanning and WiFi-specific persistence; the general config save preserves WiFi values unchanged.
- Added a fixed responsive save bar for configuration pages: compact and always visible on desktop, wide and safe-area aware on small/mobile displays. Saving remains explicit; no automatic persistence on every field change was introduced.
- Expanded infrastructure WiFi from one credential pair to up to five ordered profiles. Profile 1 keeps the legacy `wifi_ssid` / `wifi_pass` keys; profiles 2..5 are additive `wifi_ssid_2..5` / `wifi_pass_2..5`, so old configs remain valid without migration.
- Centralized STA connection attempts: configured profiles are tried in user-defined order for both boot-time time synchronization and the persistent WebConfig/streamer network. The first successful profile becomes active.
- Added optional `hotspot_fallback_enabled`. When external-WLAN mode is selected and no configured profile can connect, an explicitly enabled fallback starts the configured SensorForge AP. Default is off, preserving the previous no-fallback behavior for existing installations.
- Streamer network health now validates the actually active interface, including an AP that was started by fallback, rather than assuming the originally selected STA mode must remain active.
- Reworked the WiFi UI into clearly separated conditional external-WLAN and hotspot blocks. Connection type and failure behavior use dropdowns; external profiles are shown as a responsive priority list with one automatic empty entry row. Reorder/delete controls appear only for configured rows.
- Added an on-demand WiFi scan modal. Scan results show SSID, RSSI and a qualitative signal rating and can populate the next free profile; scanning never runs continuously in the background.
- Simplified user-facing wording for system-time synchronization, WiFi inactivity timeout and TX power. Technical explanations moved behind info buttons; TX power now shows board default and effective board minimum together, and RSSI is presented as a compact current measurement.
- Extended SFSEC1 secret recognition to `wifi_pass_2..5`; all WiFi profile passwords therefore use the same at-rest protection as the legacy profile-1 password.
- Static/source review only in this environment; no new Arduino build or hardware qualification is claimed for these WiFi/UI changes.

## v84-candidate - 2026-09-30 - camera preview reuses the central streamer frame pipeline

- Kept `/snapshot` as the common camera-preview URL. Normal SensorForge mode retains the existing preview/camera path unchanged.
- In streamer mode, `/snapshot` now explicitly raises preview demand and serves the cached JPEG from the existing streamer capture pipeline through `streamerSendSnapshot()`; it does not start a second `esp_camera_fb_get()` owner.
- While the camera settings live preview is visible, a short demand lease raises the streamer from its 5-second idle snapshot refresh to the normal effective streamer frame cadence. `/preview_stop` clears demand, with an expiry timeout as a fail-safe for vanished browser tabs.
- RTSP/RTP, HTTP-MJPEG, audio, recording ownership in normal mode, thermal handling and storage behavior are otherwise unchanged.
- Static/source review only in this environment; no Arduino build or hardware test is claimed for this preview integration.

## v84-candidate - 2026-09-30 - AP/STA compile fix for generic WiFi TX-power minimum

- Fixed `webconfig.cpp` after the AP/STA network-mode merge: the WiFi TX-power warning still referenced the removed legacy `BOARD_WIFI_TX_POWER_MIN_X10` constant.
- The UI now again derives the effective board-safe minimum through `boardWifiTxPowerSafeMinimumX10()` and filters offered TX levels against the global `SENSORFORGE_WIFI_TX_POWER_SAFE_MIN_X10` guard.
- No runtime network, streamer, RTSP, audio, HTTP-MJPEG, thermal or recording behavior is changed by this compile-only correction.
- This correction addresses the Arduino-ESP32 GCC 14 compile error reported at `webconfig.cpp:4649`.

## v84-candidate - 2026-09-30 - AP/STA network selection for streamer and WebConfig

- `hotspot_enabled=1` keeps the existing local SensorForge access-point path; `hotspot_enabled=0` now starts WebConfig and the streamer on the configured infrastructure WLAN (STA) instead of leaving the persistent service network unavailable.
- In normal SensorForge mode, `wifi_timeout_sec=0` keeps the selected network mode online; a positive timeout retains the existing inactivity shutdown behavior.
- In streamer mode, the WiFi inactivity timeout is intentionally ignored so RTSP/HTTP remain continuously reachable. The persisted timeout value is not modified and applies again after returning to normal mode.
- Streamer network health/recovery now validates the configured AP or STA mode instead of assuming AP-only operation. STA loss is handled by the existing bounded network/WebConfig recovery path.
- RTSP SDP now advertises the active interface IP (AP IP in hotspot mode, STA IP in infrastructure-WLAN mode). Hostname-based RTSP/HTTP URLs remain unchanged.
- Network settings UI now describes `hotspot_enabled` as the AP-vs-STA selector and clarifies streamer timeout behavior. Existing credentials, TX-power controls, mDNS, RTSP, HTTP-MJPEG, audio, thermal and recording logic are otherwise unchanged.
- No automatic AP fallback is introduced when STA credentials/connectivity fail; the configured network mode remains authoritative.
- Static/source review only in this environment; no Arduino build or hardware validation is claimed for this change.

## v84-candidate - 2026-09-30 - Audio live correction final tuning / dashboard live counts

- RTSP L16 live reserve raised to about 220 ms; routine live trimming starts only above about 500 ms backlog.
- First 3 s after audio capture start/restart are exempt from live trimming to avoid startup clicks.
- Gentle correction steps reduced; hard stale-backlog protection remains above about 1 s.
- Existing two-pass audio drain, RTCP synchronization, video/HTTP paths and thermal behavior are unchanged.
- Streamer overview RTSP/HTTP/audio summary now follows the same live status polling as the detailed diagnostics instead of remaining a render-time snapshot.

## v84 candidate - 2026-09-30 - smoother multi-client RTSP audio live correction

- Kept the proven 20-ms L16 packetization and bounded real-time drain, but changed live backlog correction from a large one-shot cut to small bounded steps.
- A modest backlog now triggers only about 40 ms of oldest-audio discard per service pass; a true >1 s stale backlog can use a larger but still bounded ~120 ms correction. The RTP sample timeline advances by exactly the discarded sample count.
- The target live reserve is slightly relaxed to about 60 ms to reduce correction oscillation.
- `serviceAudio()` now runs once before and once after the serial video fan-out. This addresses the field case where 2x RTSP + 1x HTTP reduced the main-loop cadence to roughly 2.4 fps and starved a single audio-drain pass even though the audio payload itself was small.
- Drain diagnostics now report the aggregate number of audio packets drained across both passes of the current firmware loop.
- RTCP, RTP A/V timing, video capture/packetization, HTTP-MJPEG, WiFi/TX power and thermal behavior are otherwise unchanged.
- Hardware qualification remains required; key acceptance signals are Capture-Drops=0, substantially reduced Live-Abwurf growth, audio ring well below capacity and no loud/periodic clicking with two RTSP clients.

## v84 candidate - 2026-09-29 - Audio drain compile fix

- `streamer.cpp`: backlog threshold calculations now use explicit `std::max<size_t>` so Arduino-ESP32 GCC 14 does not fail template deduction on mixed `size_t`/`uint32_t` operands.
- Runtime behavior of the real-time audio drain is unchanged.

## v84 candidate - 2026-09-29 - real-time RTSP audio drain / overflow protection

- Fixed a deterministic RTSP audio throughput bug: the streamer previously read at most 960 PCM bytes per firmware loop. At 16 kHz / 16 bit / mono the capture produces about 32 KiB/s, so two RTSP clients plus serial video transmission could fill the 256-KiB PSRAM audio ring and cause hard drops, rhythmic clicks and loud crackling.
- RTSP L16 audio is now packetized in short approximately 20-ms blocks and `serviceAudio()` drains multiple blocks per call within a bounded work budget. Drain effort increases when the live ring backlog rises.
- Normal operation keeps only a small live PCM reserve instead of allowing seconds of queued audio to accumulate.
- If an exceptional stall has already accumulated more than about one second of stale PCM, the oldest complete samples are discarded down to the live reserve and the shared RTP sample timeline advances by exactly the discarded sample count. This favors live continuity over replaying stale audio and avoids repeated ring overflow.
- Streamer diagnostics now distinguish capture-side drops from intentional live stale-audio discard and show the most recent / maximum drain packet count.
- RTCP, RTP A/V timing, video capture, HTTP-MJPEG, WiFi and thermal logic are otherwise unchanged.
- This remains a v84 implementation candidate; no Arduino build or hardware qualification is claimed by this change.


## v84 candidate - 2026-09-29 - RTCP A/V synchronization

- RTSP/TCP now sends RFC3550 compound RTCP Sender Reports plus SDES CNAME for every active client and media track.
- Video RTCP uses the 90-kHz RTP clock; audio RTCP uses the configured sample clock. Both are mapped to the same NTP wall-clock reference so standard clients can synchronize JPEG video and L16 audio.
- RTCP uses the negotiated interleaved channel immediately following each RTP channel (video RTP/RTCP and audio RTP/RTCP).
- RTP packet/octet counters are maintained per client and per track for correct Sender Report statistics.
- Streamer diagnostics now show per-client Video/Audio RTCP Sender Report counters plus live audio-ring occupancy, capacity, high-water and dropped bytes.
- Camera capture, HTTP-MJPEG, recording paths and the current AP/STA behavior are unchanged.
- This remains a v84 implementation candidate; no Arduino build or hardware qualification is claimed by this change.
## v84 candidate - shared PCM fan-out / PLAY A/V timeline alignment

- Multi-client RTSP audio now advances one shared PCM sample position exactly once per captured payload block. The unchanged L16 payload block is then offered to every active RTSP client; only RTP sequence number, SSRC, timestamp base and session origin remain client-local.
- Each RTSP client records the shared sample position present at its own `PLAY` as `audioSampleOrigin`. Audio RTP timestamps are derived from the shared block position relative to that origin, so serial delivery to client 1 and client 2 cannot advance the media clock twice or make their timelines influence each other.
- Fixed a separate A/V start-offset bug in RTSP `RTP-Info`: video `rtptime` previously reported the timestamp of the most recently captured JPEG. In streamer idle mode that frame can be up to five seconds old, matching the observed several-second audio/video offset. `PLAY` now snapshots a fresh monotonic 90-kHz video timestamp and reports that as the session start.
- Audio and video session origins are therefore both established at the client's `PLAY`; the first subsequent media packets advance from those announced origins instead of inheriting stale pre-PLAY video time.
- Existing shared PDM/I2S capture, single-pass L16 byte-order conversion, HTTP-MJPEG, camera settings, WiFi/TX power and thermal logic are unchanged.
- Hardware qualification remains open: verify 1x RTSP audio, then 2x RTSP audio for rhythmic clicks, and measure A/V offset after 1/10/30 minutes.

## v84 candidate - fully isolated multi-client RTSP audio clocks

- Fixed the remaining multi-client RTSP audio coupling: RTP audio sample progress is now stored entirely inside each `RtspClientSlot`. The shared PCM capture no longer has a global RTP sample counter.
- Both RTSP viewers still receive the same captured PCM payload, but each session independently owns its sequence number, SSRC, timestamp base and sent-sample count.
- A second RTSP client can therefore no longer alter or inherit the first client's audio clock state.
- Audio capture recovery advances each client's timestamp base by exactly that client's own successfully transmitted sample count before resetting that client's local counter.
- The L16 payload conversion remains single-pass before fan-out; PCM data is not byte-swapped separately per client.
- No changes to camera capture, HTTP-MJPEG, RTSP video timing, WiFi, thermal handling or persisted configuration.
- Hardware qualification remains required, especially 1x RTSP audio versus 2x RTSP audio for noise, intelligibility and A/V drift.

## v84 candidate - per-client RTSP audio timeline / thinning disabled

- Fixed multi-client RTSP audio timing: every RTSP client now receives its own audio sample origin at `PLAY`. A later client no longer inherits the sample count accumulated while an older client was already streaming.
- RTP-Info audio `rtptime` and the first L16 RTP packet of that client now start from the same per-client timeline. This addresses the observed case where a second client could accumulate tens of seconds or minutes of A/V playback delay.
- Audio capture remains shared; only RTP timestamp accounting is per client. No second PDM/I2S capture path is introduced.
- Audio recovery preserves timestamp continuity independently for each active client before the shared capture sample counter is reset.
- Adaptive RTSP video frame thinning is disabled for this qualification step. Every captured video frame is again offered to every playing RTSP client; the real monotonic 90-kHz video clock remains active.
- Existing RTSP diagnostics continue to show connection and per-frame send pressure, but no longer claim that adaptive thinning is catching a client up.
- No changes to camera settings, HTTP-MJPEG, WiFi mode/TX power, thermal guard, recording, storage or persisted configuration.
- Arduino build and long-running 2x RTSP + audio + HTTP-MJPEG hardware qualification remain open.

## v84 candidate - RTSP monotonic clock / visible live-latency diagnostics

- Corrected RTP/JPEG timestamp generation: video timestamps now come from the real monotonic ESP timer at 90 kHz instead of being advanced by a fixed `90000/fps` step. This prevents RTSP receivers from interpreting temporary multi-client send delays as a continuously growing timing/jitter offset.
- The HTTP-MJPEG path is unchanged; this specifically addresses the observed case where HTTP stayed near-live while long-running RTSP sessions accumulated seconds to nearly a minute of delay.
- Per-client adaptive thinning remains connection-preserving and whole-frame-only, but reacts slightly earlier (two sustained slow samples, 60% of the current frame budget). Recovery remains deliberately slower to avoid oscillation.
- The existing dashboard button `Streamer-Status / Diagnose` now shows two visual RTSP client cards with connection state, frame send time, frame-budget load, active thinning divider, skipped frames and number of adaptive adjustments. Green = healthy, orange = currently thinning/catching up, red = elevated transport pressure.
- `/streamer_status` adds per-client connected/adaptive-change fields and reports the video clock mode as `monotonic_90khz`. No new URL has to be remembered by the user; the dashboard continues to poll the existing status endpoint.
- The diagnostic intentionally calls this a transport/backlog indicator rather than claiming exact TCP-buffer byte occupancy; Arduino-ESP32 does not expose a portable per-client TX-queue occupancy API here. Existing socket-stall counters remain the hard overflow/stall signal.
- No RTSP client is intentionally disconnected by the adaptive latency controller. Real socket/write failures still use the existing cleanup path because a partially written interleaved RTP packet cannot safely be abandoned mid-frame.
- Arduino build and long-run multi-client hardware validation remain open.

## v84 candidate - adaptive RTSP live-latency control

- Added per-client adaptive video frame thinning for RTSP/TCP to prevent a slower viewer from accumulating many seconds of live-stream delay.
- A slow client is not disconnected merely because it falls behind. SensorForge first sends every second frame for that client; if sustained send pressure remains, it can step to every third and every fourth frame.
- Frame thinning is strictly per RTSP client, so a healthy second viewer keeps its full frame cadence.
- Recovery is gradual: after a sustained healthy send-time window the divider is reduced one step at a time back toward full frame rate.
- Only complete future JPEG frames are skipped. A frame already being packetized over RTSP/TCP is never intentionally cut mid-packet, preserving interleaved RTP framing.
- Existing cleanup remains authoritative for a real socket/write failure; the adaptive lag mechanism itself does not force client reconnects.
- Divider transitions are logged with session, old/new divider, last frame send time and accumulated skipped-frame count. `/streamer_status` additionally exposes divider, skipped-frame count and last send time for both RTSP client slots.
- No change to camera capture rate, RTP/JPEG format, audio format, thermal guard, WiFi mode or persisted configuration.
- Arduino build and multi-client long-run validation remain open.

## v84 candidate - Webviewer linker/integration fix

- restores the complete `/stream_view` implementation that was accidentally lost when later streamer hardening/thermal changes were based on an older `streamer.cpp`
- restores the public `streamerHttpViewerUrl()` definition referenced by `webconfig_streamer.cpp`
- re-registers `/stream_view` alongside the existing streamer web routes
- keeps the MJPEG stream path unchanged; overlay text/time are still rendered only in the browser
- preserves the later UI cleanup by not restoring the removed explanatory MJPEG text
- no changes to capture, RTSP, HTTP-MJPEG transport, self-healing or thermal-throttle logic

## v84 candidate - dynamic thermal streamer throttling / analyzer integration

- Streamer thermal throttling is now relative to the configured frame rate instead of fixed 3/2 fps caps.
- Stage 1 at 78.0 C uses 75% of configured fps; stage 2 at 79.0 C uses 50%, rounded to the nearest whole fps with a minimum of 1 fps.
- Existing hysteresis and the independent 80 C Thermal Guard remain unchanged.
- Thermal throttle escalation is logged as `WARNING | THERMAL | STREAMER_THROTTLE` with CPU temperature, level, factor, configured/effective fps and client counts.
- Relax/recovery transitions are logged separately, including episode duration and peak temperature when full fps is restored.
- Integrated Web Log Analyzer now counts streamer throttle warnings and completed throttle recoveries explicitly in the thermal statistics.

## v84 candidate - streamer thermal soft-throttling / idle capture reduction

- Added low-overhead streamer thermal soft-throttling without changing the existing 80 C Thermal Guard or emergency/cooldown policy.
- No throttling below 78.0 C. At >=78.0 C the effective streamer cadence is capped at 3 fps; at >=79.0 C it is capped at 2 fps. Hysteresis restores 3 fps below 77.5 C and the configured rate below 77.0 C.
- Thermal checks run only every 5 seconds. Transitions are logged with temperature, configured/effective fps and client counts. When an episode ends the log also records duration and peak temperature.
- With no active RTSP/HTTP stream client the camera remains initialized but the cached snapshot refresh is reduced to one frame every 5 seconds. First/last client transitions log the active/idle capture profile. This avoids camera reinitialization and preserves fast reconnects.
- `/streamer_status` now exposes configured/effective fps, thermal throttle level and the last streamer CPU temperature; the dashboard diagnostic block shows these values live.
- No JPEG decode/re-encode, no additional camera captures, no new background task and no change to RTSP/HTTP/audio payload formats.

## v84 candidate - low-overhead streamer hardening / self-healing

- Streamer health supervision remains deliberately low-frequency and local-first: no fast polling loops, no periodic reboot policy.
- Existing 250 ms bounded socket-write protection is retained and now counted diagnostically; a stalled client is closed without disturbing other clients or the board.
- RTSP audio gets an independent 2 s health check. If active capture stops making progress for 8 s, only the audio capture backend/ring is restarted; video and RTSP sessions remain alive. Audio recovery never forces a board reboot.
- Streamer AP health is checked every 5 s. Only three consecutive invalid AP checks (~15 s) request a full AP/mDNS/WebConfig/streamer transport restart. The main firmware owns that lifecycle so the streamer does not duplicate WiFi setup logic.
- Internal heap/PSRAM are sampled only once per minute. A six-minute falling-heap pattern is reported as a warning, not treated as a reset condition. A controlled reboot is reserved for critically low internal heap sustained over three one-minute checks.
- Rare recovery counters are persisted to NVS at most every six hours, plus immediately before a controlled recovery reboot. This avoids continuous flash wear while preserving useful long-term field diagnostics.
- The previous controlled streamer reboot reason is retained for the next boot; the hardware reset reason (including brownout/watchdog/software reset) is exposed in Streamer-Status / Diagnose.
- Streamer diagnosis now shows camera/audio/network recoveries, socket stalls, memory-trend warning, reset reason, and previous controlled reset reason.
- Existing thermal protection and the firmware's 30 s ESP task watchdog remain authoritative final safety layers.

## v84 candidate - Streamer self-healing watchdog

- Added application-level streamer health monitoring for unattended long-term operation.
- Three consecutive camera capture failures or a 15 s frame stall with active stream clients trigger a controlled local camera recovery.
- Recovery closes stale RTSP/HTTP sessions, stops streamer audio capture, reinitializes the camera with the persisted SensorForge camera configuration, and lets clients reconnect cleanly.
- Failed camera recovery is retried after 5 s; after three failed recovery attempts SensorForge escalates to a controlled ESP restart.
- Existing 30 s ESP task watchdog remains the final protection for hard task/deadlock stalls.
- `/streamer_status` and the visible dashboard diagnostics now expose camera recovery count, failed recovery attempts, current capture failures, last-frame age, and the last recovery reason.
- No normal recording-mode behavior or persisted configuration schema changed.

## v84 Streamer UI – RTSP-Aufrufbefehle (2026-09-29)

- RTSP-Konfiguration zeigt jetzt direkt nutzbare Terminal-Befehle für ffplay, VLC und mpv.
- ffplay und mpv erzwingen RTSP/RTP über TCP passend zum SensorForge-Streamer.
- VLC verwendet bewusst den portablen normalen RTSP-Aufruf, da `--rtsp-tcp` nicht in allen VLC-Versionen verfügbar ist.
- Jeder Befehl besitzt einen Kopieren-Button; die URL wird aus dem aktuellen SensorForge-Hostname erzeugt.
- Keine Änderung am Streaming-Backend, an RTSP/RTP, Audio, Kamera oder Persistenz.

## v84 candidate - RTSP audio compatibility/diagnostics

- RTSP SDP now includes a session-level `c=IN IP4 ...` connection line and `a=sendonly`.
- PLAY `RTP-Info` now includes the negotiated audio track when audio was SETUP.
- Added RTSP audio diagnostics for accepted audio SETUP and successful audio-capture start.
- `/streamer_status` now exposes `audio_packets` and `audio_bytes` so RTP audio delivery can be verified independently of player output.
- No change to the existing SensorForge audio source/configuration; RTSP continues to reuse the normal audio-capture path.

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

## v84 candidate — 2026-09-29 — Thermal/WiFi tuning

- Streamer thermal throttling begins 1 °C earlier (77/78 °C stages) while the board thermal emergency limit remains 80 °C.
- Moved the CPU thermal emergency threshold into `board_config.h` for board-specific future qualification.
- Added board-scoped discrete WiFi TX-power levels plus a board default in `board_config.h`. A global SensorForge reachability floor of 8.5 dBm is documented once near the top of `board_config.h`; it is not tied to a specific board capability table.
- WiFi TX-power normalization is now generic for future boards: any finite stored value is lifted to the next higher valid level from the active board table after applying the global 8.5-dBm safety floor. If that exact floor is not supported by a board, its next higher listed level is used automatically; if a stored value exceeds all listed levels, the highest safe board level is used. Lower hardware-valid levels may remain documented in a board table but are not exposed/applied by SensorForge.
- Added additive `wifi_tx_power_dbm` config with a backwards-compatible default of 20.0 dBm; the selected ceiling is applied to both STA and local AP without changing WiFi mode/credentials/fallback behaviour.
- Added WLAN RSSI capture during a real infrastructure-WiFi connection and a green-to-red reception scale in Configuration. The UI provides a conservative TX-power suggestion but never changes TX power automatically.
- No claim of completed Arduino build, RF qualification or thermal hardware validation for this candidate.


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

### v84 Streamer candidate - sichtbarer Streamer-Status auf der Übersicht
- Die Streamer-Übersicht besitzt jetzt einen direkt sichtbaren Button `Streamer-Status / Diagnose`.
- Der aufklappbare Bereich zeigt RTSP-/HTTP-Clientzahlen, Frames, FPS, Gesamtbytes sowie Audio-Status, Audio-RTP-Pakete und Audio-Bytes.
- Die Werte werden über den bestehenden `/streamer_status`-Endpunkt alle 2 Sekunden aktualisiert, solange die Seite sichtbar ist.
- Audio-Diagnose ist damit ohne manuelles Aufrufen eines versteckten API-Endpunkts verfügbar.

### v84 candidate - Mikrofon-Hörtest wiederhergestellt
- Die kurze WAV-Mikrofonprobe ist wieder direkt in der Audio-/Mikrofon-Konfiguration verfügbar.
- `Mikrofon 10 Sekunden testen` verwendet den bestehenden `audioWavRecordTest()`-/Audio-Capture-/PSRAM-Pfad und die aktuell gespeicherte Audiokonfiguration.
- Nach erfolgreicher Aufnahme erscheint direkt ein Browser-Audioplayer; die temporäre WAV-Datei kann zusätzlich heruntergeladen werden und wird beim nächsten Test überschrieben.
- Die Diagnose-WAV ist bewusst temporär und unverschlüsselt; normale Aufnahme-, MKV- und SFENC1-Pfade bleiben unverändert.
- Aufnahme, Storage-Wartung, bereits aktiver Audio-Capture und Netzwerk-Streamer blockieren den Test, um Ownership-Konflikte zu vermeiden.
- Der zentrale Systemtest bleibt erhalten; nur der kurze Hörtest wurde wiederhergestellt. Der historische Audio-Benchmark bleibt auf den Systemtest umgeleitet.
- Neue sichtbare Texte sind in Deutsch und Englisch vorhanden.

## v84 candidate - streamer telemetry + manual stress measurement

- Add passive streamer telemetry without JPEG decode/re-encode or extra network traffic:
  cumulative JPEG frame/byte statistics, JPEG average/max size, RTP/JPEG packet and wire-byte counters,
  HTTP video payload bytes, and RTSP per-frame send-time average/max.
- Expose these counters in `/streamer_status`; the Streamer diagnostics UI derives current aggregate video kbit/s
  from counter deltas in the browser.
- Add a manual, time-bounded Streamer stress/measurement panel in Config (30/60/120 s):
  - Camera/JPEG: normal JPEG capture cadence without network output; requires no active stream clients.
  - HTTP-MJPEG: browser itself opens one real MJPEG client; requires no pre-existing stream clients.
  - Current stream: measures the already-running RTSP/HTTP/audio workload without changing it.
- Stress START/END are logged with mode, duration, configured/effective FPS, frame count, average/max JPEG size,
  video kbit/s, RTP packet count, RTSP/HTTP/audio byte counters, max RTSP frame send time, start/peak CPU temperature,
  and current client/audio state. External current draw remains a manual multimeter measurement.
- Existing 80 C thermal guard and dynamic 78/79 C relative FPS throttling remain authoritative during all tests.
