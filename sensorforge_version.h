#pragma once

// ============================================================================
// SensorForge release identity
// ============================================================================
//
// This is the human/project release number for the complete source tree.
// It is intentionally separate from:
//   - SENSORFORGE_CORE_VERSION in branding.h (product/core compatibility)
//   - __DATE__ / __TIME__ (individual compiler build timestamp)
//   - API protocol versions (sync_api.cpp)
//
// Release workflow:
//   1. Increment SENSORFORGE_RELEASE_NUMBER / SENSORFORGE_RELEASE_TAG.
//   2. Update SENSORFORGE_RELEASE_DATE and SENSORFORGE_RELEASE_SUMMARY.
//   3. Add the release to CHANGELOG.md.
//   4. Commit the complete project to Git.
//   5. Create a Git tag with exactly SENSORFORGE_RELEASE_TAG (for example v35).
//
// The same release tag is written to the firmware boot log and shown in
// WebConfig, so a running device can be mapped back to the exact Git tag.

#define SENSORFORGE_RELEASE_NUMBER 87
#define SENSORFORGE_RELEASE_TAG "v87-beta19"
#define SENSORFORGE_RELEASE_DATE "2026-10-03"
#define SENSORFORGE_RELEASE_SUMMARY \
    "Beta 19: documentation-only roadmap for optional board-gated WireGuard remote management; runtime unchanged from Beta 18."

// Current source-tree stage. v87 Beta 19 documents the optional WireGuard roadmap; runtime remains unchanged from Beta 18.
#define SENSORFORGE_WORKTREE_STAGE "v87-beta19"
#define SENSORFORGE_WORKTREE_DATE "2026-10-03"
#define SENSORFORGE_WORKTREE_SUMMARY \
    "v87 Beta 19: optional WireGuard client roadmap, default off on ESP32-S3 and management-first qualification; no runtime change."
