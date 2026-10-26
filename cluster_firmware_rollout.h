#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Coordinator: cluster firmware rollout. Sends the signed update job
// (clusterRequestNodeFirmwareUpdate) to one node at a time, waits until it is
// back with a new boot and the new release, then the next one; stops at the
// first failure. The coordinator itself is updated by a separate action
// (FirmwareClusterUpdate::requestLocal). loopTask only.

// Answer of a node (cluster.cpp, SFJUA1).
void clusterFirmwareRolloutNote(const char *nodeId, uint32_t seq, uint8_t code, const String &version,
                                const String &text);
// Called from loop().
void clusterFirmwareRolloutService();

String clusterFirmwareRolloutJson();
// Shared by /cluster_firmware and /api/v1/cluster/firmware:
//   action=check&url=...                 manifest content per board
//   action=start&url=...&allow_older=0|1 roll out all online nodes
//   action=stop                          no further nodes (the running one finishes)
//   action=local&url=...&allow_older=0|1 update the coordinator itself
//   action=clear                         forget the finished rollout table
// Returns the HTTP status.
int clusterFirmwareRolloutRequestFromArgs(WebServer &srv, String &json);
