#pragma once
#include <Arduino.h>
// New accepted job clears ring. No rollover: full ring means stop creating frames.
void clusterCaptureBeginJob(uint64_t jobId);
uint16_t clusterCapturePhoto(uint64_t jobId,int64_t targetUtcUs,int64_t &dispatchUtcUs,uint32_t &bytes,int64_t anchorUtcUs,int64_t anchorMonoUs);
bool clusterCaptureLatestMedia(uint64_t jobId,char uuid[37],int64_t &dispatchUtcUs);
bool clusterCaptureGetJpeg(const String &uuid,const uint8_t *&bytes,size_t &length);
String clusterCaptureLocalJson();
bool clusterCapturePrepareCamera();
bool clusterCaptureArmed();
