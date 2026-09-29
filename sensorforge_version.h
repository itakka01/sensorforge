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

#define SENSORFORGE_RELEASE_NUMBER 83
#define SENSORFORGE_RELEASE_TAG "v83"
#define SENSORFORGE_RELEASE_DATE "2026-09-28"
#define SENSORFORGE_RELEASE_SUMMARY \
    "Simplify general configuration layout with clearer audio, timezone, network, LED and debug controls."

// Current post-v83 development state. This does not claim a released v84;
// promote the release tag only after the required build/hardware qualification.
#define SENSORFORGE_WORKTREE_STAGE "v84-candidate"
#define SENSORFORGE_WORKTREE_DATE "2026-09-30"
#define SENSORFORGE_WORKTREE_SUMMARY \
    "AP/STA streamer networking with generic board-safe WiFi TX-power UI; compile fix removes obsolete board minimum reference."
