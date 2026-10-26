#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Coordinator page "Firmware im Cluster" (/cluster_firmware): manifest check,
// one-node-at-a-time rollout, coordinator update (cluster_firmware_rollout.h).
// GET /cluster_firmware_status (JSON), POST /cluster_firmware_action. Admin only.
void webconfigClusterFirmwareRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)());
