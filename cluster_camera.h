#pragma once

#include <Arduino.h>
#include "cluster_camera_report.h"

// Cluster camera transparency (phase 1) and profile results (phase 2).
//
// Nodes report sensor type, capabilities and the effective camera settings in
// the authenticated SFK1 UDP side packet (cluster.cpp). This module keeps the
// latest report per node and renders the coordinator overview. It never
// changes a camera setting.

// Implemented in sensorforge.ino (owns the camera state). RAM-only snapshot:
// no I2C access, no setter calls.
void cameraFillClusterReport(ClusterCamera::Report &r);

// Stores a validated report; older/duplicate report sequences are ignored.
void clusterCameraStoreReport(const char *integrationId, uint32_t bootNonce,
                              uint32_t reportSeq, const ClusterCamera::Report &report);

// Overview of the local device and all online peers.
String clusterCameraJson();

// Latest stored report of a peer (not the local device); ageMs since receipt.
bool clusterCameraLatestReport(const char *integrationId, ClusterCamera::Report &out, uint32_t &ageMs);

// Coordinator: a node's answer to a cluster camera profile job.
void clusterCameraNoteProfileResult(const char *integrationId, uint8_t result, uint32_t version);

// Implemented in cluster_camera_profile.cpp: the coordinator's own last
// profile result as JSON fields (",\"profile_result\":...").
void clusterCameraAppendLocalProfileResult(String &json, uint32_t nowMs);
