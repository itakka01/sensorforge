#pragma once
#include <Arduino.h>
// New accepted job clears ring. No rollover: full ring means stop creating frames.
void clusterCaptureBeginJob(uint64_t jobId);
// testPulse: drive the prepared GPIO test pulse around the capturing fb_get().
// Timing of the last shot for diagnostics (µs, monotonic): drain of old
// frames and the moment the capturing fb_get started.
struct ClusterCaptureShotTiming{int64_t drainStartUs;int64_t drainEndUs;uint8_t drained;uint8_t lateDrained;
    int64_t releaseStartUs;int64_t dispatchMonoUs;int64_t frameReturnedUs;int64_t frameTimestampUs;
    uint8_t framesSkipped;
    // Every frame the shot received after the release (skipped ones first):
    // start relative to the release start [us] and JPEG length.
    uint8_t frameCount;int32_t frameRelUs[5];uint32_t frameLen[5];
    // Sensor orientation registers right after the shot (OV3660/OV5640
    // 0x3820 flip, 0x3821 mirror; -1 = not read).
    int16_t reg3820;int16_t reg3821;};
// Sensor frame period (driver frame timestamps, min of consecutive spacings).
// Measure: blocking, ~5 frame periods (camera awake, loopTask). Track:
// non-blocking, one ready frame per call (while the cameras are armed).
// Period: 0 = unknown; ageUs = time since the last measurement.
bool clusterCaptureMeasureFramePeriod();
void clusterCaptureTrackFramePeriod();
int64_t clusterCaptureFramePeriodUs(int64_t *ageUs=nullptr);
// Drops already completed (old) frames without waiting. Called at the start of
// the final approach (~2-3 ms before the deadline): returning a buffer to the
// driver takes ~0.5 ms, too slow for the moment of the shot itself.
void clusterCapturePreDrain();
ClusterCaptureShotTiming clusterCaptureLastShotTiming();
// frameNotBeforeMonoUs (frame-clock sync): frames whose driver timestamp is
// earlier are dropped; the first later frame is the shot and dispatchUtcUs is
// its frame start. 0 = take the next frame (dispatch = software dispatch).
uint16_t clusterCapturePhoto(uint64_t jobId,int64_t targetUtcUs,int64_t &dispatchUtcUs,uint32_t &bytes,int64_t anchorUtcUs,int64_t anchorMonoUs,bool testPulse=false,int64_t frameNotBeforeMonoUs=0);
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
// Timing of the last fresh live frame (cached answers keep it): camera grab,
// thumbnail decode/encode (camera_thumbnail.h), source and output size.
struct ClusterLiveFrameStats{uint32_t grabUs;uint32_t decodeUs;uint32_t encodeUs;uint16_t srcWidth;uint16_t srcHeight;uint16_t width;uint16_t height;uint8_t path;bool cached;};
ClusterLiveFrameStats clusterCaptureLiveStats();
// Drops the shared live frame (camera settings changed).
void clusterCaptureLiveInvalidate();
// Duration of one sensor line (OV3660/OV5640): shortest gap of consecutive
// frames divided by the frame length register (VTS, 0x380E/0x380F). Exposure
// time = aec_value * lineUs (approximate; a manual exposure longer than the
// frame stretches the frame). Same refusals as the live frame; ~6 frames.
bool clusterCaptureMeasureLineTime(float &lineUs,uint32_t &vts,float &frameMs,String &reason);
String clusterCaptureLocalJson();
bool clusterCapturePrepareCamera();
bool clusterCaptureArmed();
