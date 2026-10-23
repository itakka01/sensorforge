#pragma once

#include <Arduino.h>
#include <WebServer.h>

// Node mount metadata form and coordinator group management. Kept separate
// from webconfig_cluster.cpp; that module only embeds the HTML snippets and
// calls the route registration.

// Mount fields for the local /cluster page (every device). Must be placed
// inside the cluster settings form; saved by /cluster_save.
String webconfigClusterMountFormHtml();

// Saves the submitted mount fields (if present) from a /cluster_save request.
// changed=false when the values equal the current ones (nothing written).
// Returns false with error on invalid input or a failed write.
bool webconfigClusterMountSaveFromRequest(
    WebServer &server,
    bool writeToSd,
    bool &changed,
    String &error
);

// Group management section for /cluster_coordinate (coordinator only).
String webconfigClusterGroupsSectionHtml();

// Filter bar placeholder + script for the coordinator device table. The table
// rows must carry data-node-id and renderHealth() must call
// window.sfMountApplyFilter(nodes) after rendering.
String webconfigClusterMountFilterHtml();

void webconfigClusterGroupsRegisterRoutes(WebServer &server);
