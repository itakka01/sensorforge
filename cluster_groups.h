#pragma once

#include <Arduino.h>

// Coordinator-local group management on top of the flat cluster.
//
// Groups are recognised from the group_id that online nodes report in their
// mount metadata. Per group, orientation and object distance are stored in
// NVS on this device only (namespace "sfgroups"). Nothing here changes the
// cluster topology: no sub-clusters, radio groups or coordinator hierarchy.
//
// NVS is written only by explicit WebConfig save/delete requests, never from
// the cluster loop, so the radio and time-sync paths see no flash writes.

// RAM-only activity note, called for every authenticated mount report and the
// local heartbeat. Cheap and allocation-free after the first call.
void clusterGroupsNoteActive(const char *groupId);

// Groups (stored and currently reported) with state, nodes and values.
String clusterGroupsJson();

// orientationDeg: "" or 0..359.9; distanceMm: "" or 0..1000000.
// Saving confirms the values for the current use of the group.
bool clusterGroupsSave(const String &groupId, const String &orientationDeg,
                       const String &distanceMm, String &error);

// Only groups without online nodes can be deleted.
bool clusterGroupsDelete(const String &groupId, String &error);
