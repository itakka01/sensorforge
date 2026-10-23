#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Coordinator camera overview (cluster phase 1, read-only). Embedded into the
// /cluster_coordinate page by webconfig_cluster.cpp.
String webconfigClusterCameraSectionHtml();
void webconfigClusterCameraRegisterRoutes(WebServer &server);
