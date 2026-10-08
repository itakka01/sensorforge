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
//   5. Create a Git tag with exactly SENSORFORGE_RELEASE_TAG (for example v87-beta38).
//
// The same release tag is written to the firmware boot log and shown in
// WebConfig, so a running device can be mapped back to the exact Git tag.

#define SENSORFORGE_RELEASE_NUMBER 87
#define SENSORFORGE_RELEASE_TAG "v87-beta38"
#define SENSORFORGE_RELEASE_DATE "2026-10-08"
#define SENSORFORGE_RELEASE_SUMMARY \
    "Beta 38: passive Cluster discovery, stable Cluster IDs and remembered encrypted Cluster credentials."

// Current source-tree stage. v87 Beta 38 adds passive discovery/known-cluster convenience without changing WiFi ownership.
#define SENSORFORGE_WORKTREE_STAGE "v87-beta38"
#define SENSORFORGE_WORKTREE_DATE "2026-10-08"
#define SENSORFORGE_WORKTREE_SUMMARY \
    "v87 Beta 38: page-scoped passive discovery, stable cluster_id/credential epoch and SFSEC1-protected known-cluster cache."
