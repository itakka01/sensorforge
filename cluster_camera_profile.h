#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Coordinator side of the cluster camera profile (camera_profile.h): one
// reference camera per sensor type; its current settings become a versioned
// profile that is sent to every online node with the same sensor, for the
// session or persistently, or removed again. References and the last profile
// per sensor type are kept in NVS ("sf_camref"). loopTask only.

// {"ok":true,"sensors":[{pid, sensor, reference, profile, nodes counts}]}
String clusterCameraProfileJson();

// Shared by /cluster_camera_profile and /api/v1/cluster/camera_profile:
//   action=reference&node_id=<id>
//   action=distribute&pid=<hex>&mode=session|persistent
//   action=clear&pid=<hex>
// Returns the HTTP status; json is the answer body.
int clusterCameraProfileRequestFromArgs(WebServer &srv, String &json);
