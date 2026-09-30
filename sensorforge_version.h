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

#define SENSORFORGE_RELEASE_NUMBER 84
#define SENSORFORGE_RELEASE_TAG "v84"
#define SENSORFORGE_RELEASE_DATE "2026-09-30"
#define SENSORFORGE_RELEASE_SUMMARY \
    "Network streamer, multi-WiFi/AP fallback, unified responsive settings UI and central camera preview/save workflow."

// Current source-tree stage. v84 was promoted to an official release on 2026-09-30.
#define SENSORFORGE_WORKTREE_STAGE "v84-release"
#define SENSORFORGE_WORKTREE_DATE "2026-09-30"
#define SENSORFORGE_WORKTREE_SUMMARY \
    "Official v84 release: streamer, prioritized WiFi profiles/AP fallback, responsive save UI and central camera preview/crop workflow."
