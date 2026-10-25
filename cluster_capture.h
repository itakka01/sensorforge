#pragma once
#include <Arduino.h>
// New accepted job clears ring. No rollover: full ring means stop creating frames.
void clusterCaptureBeginJob(uint64_t jobId);
// testPulse: drive the prepared GPIO test pulse around the capturing fb_get().
uint16_t clusterCapturePhoto(uint64_t jobId,int64_t targetUtcUs,int64_t &dispatchUtcUs,uint32_t &bytes,int64_t anchorUtcUs,int64_t anchorMonoUs,bool testPulse=false);
bool clusterCaptureLatestMedia(uint64_t jobId,char uuid[37],int64_t &dispatchUtcUs);
bool clusterCaptureGetJpeg(const String &uuid,const uint8_t *&bytes,size_t &length);
// Downscaled copy (~1/8) of a capture result, made on first request; the
// original when it is already small.
bool clusterCaptureGetThumb(const String &uuid,const uint8_t *&bytes,size_t &length);
// Fresh camera frame for overview/tuning pages, downscaled by 1/8, 1/4 or 1/2
// to at least minWidth pixels (cameraMakeThumbnail); 0 or no fitting factor =
// original. Refused (reason) while a cluster shot is planned or
// recording/streaming/API owns the camera; in Drone Mode the camera is started
// if needed. Bytes stay valid until the next call (loopTask only); calls with
// the same minWidth within 0.7 s share one grab.
constexpr uint16_t kLiveFrameSmall=160;
constexpr uint16_t kLiveFrameMedium=800;
bool clusterCaptureLiveFrame(uint16_t minWidth,const uint8_t *&bytes,size_t &length,String &reason);
String clusterCaptureLocalJson();
bool clusterCapturePrepareCamera();
bool clusterCaptureArmed();
