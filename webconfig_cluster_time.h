#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Coordinator time overview (beta148): one compact time status per node for
// the device table and the "Zeitsynchronisation" section of
// /cluster_coordinate, plus a CSV of the long-term statistics. Data comes
// from the beacon reports the nodes send anyway; no extra radio traffic.
String webconfigClusterTimeSectionHtml();
void webconfigClusterTimeRegisterRoutes(WebServer &server);
