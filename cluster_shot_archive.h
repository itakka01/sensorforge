#pragma once
#include <Arduino.h>

// Shot archive on the coordinator's SD card (beta187).
//
// After a planned or instant shot the coordinator page can ask the coordinator
// to collect the photos of that shot from all nodes and store them as one ZIP
// in /cluster_shots/shot_<UTC>.zip (stored entries, <Node>_<frame UTC>.jpg plus
// shot_info.csv - the same layout as the browser ZIP). Only with a mounted SD
// card. The download runs in its own task on core 0, so the loop task (time
// signal, web server) is not blocked; photos are fetched with a cluster media
// token, so it also works with the WebConfig login enabled.

// SD card mounted and not locked by a maintenance operation.
bool clusterShotArchiveSdReady();
// node_ids: comma separated integration ids whose capture jobs for the shot
// at targetUtcMs are already queued (call right after scheduling).
bool clusterShotArchiveRequest(const String &nodeIds, int64_t targetUtcMs, String &message);
// Loop task: watches the capture jobs and starts the collection.
void clusterShotArchiveLoop();
// Collection task running (SD download of an archive is refused meanwhile).
bool clusterShotArchiveBusy();
// {"sd":true,"state":"idle|waiting|collecting|done|failed",...}
String clusterShotArchiveStatusJson();
// Archives on the SD card, newest first: [{"name":...,"bytes":...},...]
String clusterShotArchiveListJson(size_t maxFiles);
// Validated full path of an archive file name (no directories, .zip only).
bool clusterShotArchivePath(const String &name, String &path);
