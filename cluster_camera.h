#pragma once

#include <Arduino.h>
#include "cluster_camera_report.h"

// Cluster camera transparency (phase 1, read-only).
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
