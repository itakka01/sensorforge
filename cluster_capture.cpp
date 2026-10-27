#include "cluster_capture.h"
#include "cluster.h"
#include "cluster_hw_time_pulse.h"
#include "recorder.h"
#include "streamer.h"
#include "drone_mode.h"
#include "sync_api.h"
#include "camera_thumbnail.h"
#include "camera_power.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

extern bool recording;
extern bool cameraInitialized;
// Configured by the existing camera initialization path.
extern uint8_t clusterCaptureFrameBufferCount();
namespace {
struct Frame {
    uint8_t *bytes=nullptr;
    size_t length=0;
    uint16_t width=0, height=0;
    // Downscaled copy for overview pages, made on first request.
    uint8_t *thumb=nullptr;
    size_t thumbLength=0;
    char uuid[37]={};
    uint64_t job=0;
    int64_t target=0, dispatch=0;
};
constexpr size_t kMaxFrames=3;
// Overview thumbnails: at least this wide (1600 px -> 1/8 = 200 px).
constexpr uint16_t kThumbMinWidth=160;
Frame slots[kMaxFrames];
size_t count=0;
uint64_t activeJob=0;
void generateUuid(char *out) {
    uint8_t b[16];
    for (int i=0;i<16;i+=4) {
        uint32_t r=esp_random();
        memcpy(b+i,&r,4);
    }
    b[6]=(b[6]&0x0f)|0x40; b[8]=(b[8]&0x3f)|0x80;
    snprintf(out,37,"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
}
}
void clusterCaptureBeginJob(uint64_t jobId){
    if(!jobId)return;
    // Capture lifecycle is single-threaded in loopTask. Clear at job acceptance,
    // not on read/download, so every run has a defined beginning.
    for(auto &f:slots){if(f.bytes)heap_caps_free(f.bytes);if(f.thumb)heap_caps_free(f.thumb);f=Frame{};}
    count=0;activeJob=jobId;
}
static ClusterCaptureShotTiming lastShot={};
ClusterCaptureShotTiming clusterCaptureLastShotTiming(){return lastShot;}
static inline int64_t frameTsUs(const camera_fb_t *fb){
    return (int64_t)fb->timestamp.tv_sec*1000000LL+fb->timestamp.tv_usec;
}
static int64_t framePeriodUs=0, framePeriodAtUs=0;
static int64_t trackPrevTsUs=0;
static int64_t trackSamples[4]={};
static uint8_t trackCount=0;
// Spacings shorter than 10 ms or longer than 250 ms are not a frame period.
static void notePeriodSample(int64_t d){
    if(d<10000||d>250000)return;
    trackSamples[trackCount++]=d;
    if(trackCount<4)return;
    trackCount=0;
    // Mean of the 4 spacings near the shortest one: a missed frame doubles a
    // spacing and is left out; the timestamp jitter averages out.
    int64_t mn=trackSamples[0];
    for(uint8_t i=1;i<4;++i)if(trackSamples[i]<mn)mn=trackSamples[i];
    int64_t sum=0;uint8_t n=0;
    for(uint8_t i=0;i<4;++i)if(trackSamples[i]<mn+mn/4){sum+=trackSamples[i];++n;}
    const int64_t p=sum/n;
    // A busy loop (e.g. live preview requests) fetches frames too late and
    // the driver skips every other one: all spacings are then 2x (or 3x) the
    // period (measured 104 instead of 52 ms on c003, the shot came one period
    // early). Shorter is trusted at once; a whole multiple of the known
    // period is rejected; any other longer value needs two windows in a row.
    static int64_t pendingUs=0;
    if(framePeriodUs){
        for(int k=2;k<=3;++k)
            if(p>framePeriodUs*k-framePeriodUs/10 && p<framePeriodUs*k+framePeriodUs/10)return;
        if(p>framePeriodUs+framePeriodUs/30){
            if(!pendingUs||llabs(p-pendingUs)>pendingUs/30){pendingUs=p;return;}
        }
    }
    pendingUs=0;
    framePeriodUs=p;framePeriodAtUs=esp_timer_get_time();
}
int64_t clusterCaptureFramePeriodUs(int64_t *ageUs){
    if(ageUs)*ageUs=framePeriodUs?esp_timer_get_time()-framePeriodAtUs:0;
    return framePeriodUs;
}
bool clusterCaptureMeasureFramePeriod(){
    if(!cameraInitialized||!esp_camera_sensor_get()||!cameraWakeForUse())return false;
    for(uint8_t i=0;i<4 && esp_camera_available_frames();++i){
        camera_fb_t *old=esp_camera_fb_get();
        if(!old)break;
        esp_camera_fb_return(old);
    }
    trackPrevTsUs=0;trackCount=0;
    const int64_t before=framePeriodAtUs;
    for(uint8_t i=0;i<5;++i){
        camera_fb_t *fb=esp_camera_fb_get();
        if(!fb)return false;
        const int64_t ts=frameTsUs(fb);
        esp_camera_fb_return(fb);
        if(trackPrevTsUs)notePeriodSample(ts-trackPrevTsUs);
        trackPrevTsUs=ts;
    }
    return framePeriodAtUs!=before;
}
void clusterCaptureTrackFramePeriod(){
    if(!cameraInitialized||cameraFrameSyncHeld()||cameraIdleStandbyActive()||
       !esp_camera_sensor_get()||!esp_camera_available_frames())return;
    camera_fb_t *fb=esp_camera_fb_get();
    if(!fb)return;
    const int64_t ts=frameTsUs(fb);
    esp_camera_fb_return(fb);
    if(trackPrevTsUs)notePeriodSample(ts-trackPrevTsUs);
    trackPrevTsUs=ts;
}
void clusterCapturePreDrain(){
    lastShot={};
    if(!cameraInitialized||!esp_camera_sensor_get())return;
    // A sensor held for frame-clock sync stays held (released at dispatch);
    // drained here are frames (or truncated ones) from before the hold.
    if(!cameraFrameSyncHeld())cameraWakeForUse();
    lastShot.drainStartUs=esp_timer_get_time();
    for(uint8_t i=0;i<4 && esp_camera_available_frames();++i){
        camera_fb_t *old=esp_camera_fb_get();
        if(!old)break;
        esp_camera_fb_return(old);
        ++lastShot.drained;
    }
    lastShot.drainEndUs=esp_timer_get_time();
}
uint16_t clusterCapturePhoto(uint64_t jobId,int64_t targetUtcUs,int64_t &dispatchUtcUs,uint32_t &bytes,int64_t anchorUtcUs,int64_t anchorMonoUs,bool testPulse,int64_t frameNotBeforeMonoUs) {
    bytes=0;dispatchUtcUs=0;
    if(jobId!=activeJob)return 5;
    if(count>=kMaxFrames)return 6; // Job full: NEVER overwrite earlier synchronized images.
    if (!cameraInitialized || !esp_camera_sensor_get() || recording || recorderIsOpen() ||
        (streamerModeEnabled() && !droneModeEnabled()) || streamerReady() || syncApiExclusiveActive()) return 1;
    if(!psramFound())return 2;
    // Normally woken 2 s ahead (clusterCapturePrepareCamera); a sensor held for
    // frame-clock sync is released only at the dispatch below.
    const bool frameSync=cameraFrameSyncHeld();
    if(!frameSync && !cameraWakeForUse())return 1;
    // CAMERA_GRAB_WHEN_EMPTY can leave completed, older frames queued. Drop
    // exactly those that are already complete - without waiting (the old
    // "hold every buffer" flush could block on a frame and made the dispatch
    // moment vary by up to ~1 ms). The get below then waits for a new frame.
    uint8_t n=clusterCaptureFrameBufferCount();
    if(n<1 || n>2)return 3; // Current camera setup uses one or two buffers.
    // Old frames were dropped before the deadline (clusterCapturePreDrain); a
    // frame that completed during the final approach is dropped here.
    for(uint8_t i=0;i<4 && esp_camera_available_frames();++i){
        camera_fb_t *old=esp_camera_fb_get();
        if(!old)break;
        esp_camera_fb_return(old);
        ++lastShot.lateDrained;
    }
    // Optional test pulse (SFJC1 "tp1"): rising edge = start of the capturing
    // fb_get, i.e. the same instant as the dispatch timestamp below. Pin was
    // configured at arming; this is one IRAM register write (no measurable delay).
    if(testPulse)clusterCapturePulseRise();
    // Frame-clock sync: restart the sensor now (one frame period before the
    // deadline). Its first frame started with the release and is under-
    // exposed; the shot is the first frame starting at/after
    // frameNotBeforeMonoUs, on the same frame grid on every node.
    lastShot.releaseStartUs=esp_timer_get_time();
    if(frameSync && !cameraFrameSyncRelease()){if(testPulse)clusterCapturePulseFall();return 3;}
    // Software dispatch, NOT sensor exposure-start timestamp.
    lastShot.dispatchMonoUs=esp_timer_get_time();
    dispatchUtcUs=anchorUtcUs+(lastShot.dispatchMonoUs-anchorMonoUs);
    if(frameSync && frameNotBeforeMonoUs){
        // The restart frame starts ~1.6-2.0 ms after the release: restore
        // the JPEG quality once it is under way (cameraFrameSyncHold).
        while(esp_timer_get_time()-lastShot.releaseStartUs<5000){}
        if(!cameraFrameSyncRestoreQuality()){if(testPulse)clusterCapturePulseFall();return 3;}
    }
    camera_fb_t *fb=esp_camera_fb_get();
    if(testPulse)clusterCapturePulseFall();
    auto noteFrame=[](const camera_fb_t *f){
        if(!f||lastShot.frameCount>=5)return;
        lastShot.frameRelUs[lastShot.frameCount]=(int32_t)(frameTsUs(f)-lastShot.releaseStartUs);
        lastShot.frameLen[lastShot.frameCount++]=f->len;
    };
    noteFrame(fb);
    while(fb && frameSync && frameNotBeforeMonoUs && frameTsUs(fb)<frameNotBeforeMonoUs &&
          lastShot.framesSkipped<4){
        esp_camera_fb_return(fb);
        ++lastShot.framesSkipped;
        fb=esp_camera_fb_get();
        noteFrame(fb);
    }
    if(!fb)return 3;
    // The frames right after the release are consecutive: their spacing caps
    // a period estimate that came out as a multiple (skipped frames).
    for(uint8_t i=1;i<lastShot.frameCount;++i){
        const int64_t d=(int64_t)lastShot.frameRelUs[i]-lastShot.frameRelUs[i-1];
        if(d>10000 && framePeriodUs>d+d/2){framePeriodUs=d;framePeriodAtUs=esp_timer_get_time();}
    }
    lastShot.frameReturnedUs=esp_timer_get_time();
    lastShot.reg3820=lastShot.reg3821=-1;
    if(sensor_t *s=esp_camera_sensor_get())
        if(s->get_reg&&(s->id.PID==0x5640||s->id.PID==0x3660)){
            lastShot.reg3820=(int16_t)s->get_reg(s,0x3820,0xFF);lastShot.reg3821=(int16_t)s->get_reg(s,0x3821,0xFF);}
    lastShot.frameTimestampUs=frameTsUs(fb);
    // With frame-clock sync the frame start is the meaningful shot time.
    if(frameSync && frameNotBeforeMonoUs)
        dispatchUtcUs=anchorUtcUs+(lastShot.frameTimestampUs-anchorMonoUs);
    if(fb->format!=PIXFORMAT_JPEG||!fb->buf||!fb->len){esp_camera_fb_return(fb);return 3;}
    void *copy=heap_caps_malloc(fb->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!copy){esp_camera_fb_return(fb);return 6;} // Capacity exhausted; do not evict.
    memcpy(copy,fb->buf,fb->len);
    size_t size=fb->len;
    const uint16_t width=fb->width, height=fb->height;
    esp_camera_fb_return(fb);

    // SAFETY: Double-check array bounds before access (defense-in-depth).
    // Should be prevented by check at line 50, but adding sanity check here.
    if(count >= kMaxFrames) {
        heap_caps_free(copy);
        return 6;  // Should never happen, but fail-safe
    }

    Frame &f=slots[count++];
    f.bytes=(uint8_t*)copy;f.length=size;f.job=jobId;
    f.width=width;f.height=height;
    f.target=targetUtcUs;f.dispatch=dispatchUtcUs;
    generateUuid(f.uuid);
    bytes=(uint32_t)size;
    return 0;
}
bool clusterCaptureLatestMedia(uint64_t jobId,char uuid[37],int64_t &dispatchUtcUs){
    if(!count||slots[count-1].job!=jobId)return false;
    const Frame &f=slots[count-1];
    memcpy(uuid,f.uuid,37);dispatchUtcUs=f.dispatch;return true;
}
bool clusterCaptureGetJpeg(const String &uuid,const uint8_t *&bytes,size_t &length){
    bytes=nullptr;length=0;
    if(uuid.length()!=36)return false;
    for(size_t i=0;i<count;i++) if(uuid.equals(slots[i].uuid)){
        bytes=slots[i].bytes;length=slots[i].length;return bytes&&length;
    }
    return false;
}
bool clusterCaptureGetThumb(const String &uuid,const uint8_t *&bytes,size_t &length){
    bytes=nullptr;length=0;
    if(uuid.length()!=36)return false;
    for(size_t i=0;i<count;i++) if(uuid.equals(slots[i].uuid)){
        Frame &f=slots[i];
        if(!f.bytes)return false;
        if(!f.thumb && !cameraMakeThumbnail(f.bytes,f.length,f.width,f.height,kThumbMinWidth,f.thumb,f.thumbLength)){
            // Already small or not convertible: the original serves as thumbnail.
            bytes=f.bytes;length=f.length;return true;
        }
        bytes=f.thumb;length=f.thumbLength;return true;
    }
    return false;
}
namespace {
// Live frame cache: polling browsers within kLiveReuseMs share one grab.
uint8_t *liveBytes=nullptr;
size_t liveLength=0;
uint16_t liveMinWidth=0;
uint32_t liveTakenMs=0;
constexpr uint32_t kLiveReuseMs=700;
ClusterLiveFrameStats liveStats={};
void liveClear(){if(liveBytes)heap_caps_free(liveBytes);liveBytes=nullptr;liveLength=0;liveTakenMs=0;}
// Camera free for a page grab (live frame, line-time measurement)?
bool liveCameraReady(String &reason){
    // A planned cluster shot owns the camera from acceptance until after it.
    if(clusterCaptureProtectionActive()){reason="Cluster-Aufnahme geplant";return false;}
    // Armed for the instant shot: no slow side work that could delay a job.
    if(clusterSharpActive()){reason="Kameras scharf geschaltet (Sofortaufnahme) – am Coordinator „Entschärfen“ oder Kamera-Übersicht starten";return false;}
    if(recording || recorderIsOpen()){reason="Aufnahme läuft";return false;}
    if((streamerModeEnabled() && !droneModeEnabled()) || streamerReady()){reason="Kamera vom Streamer belegt";return false;}
    if(syncApiExclusiveActive()){reason="API-Sitzung belegt die Kamera";return false;}
    if(!psramFound()){reason="kein PSRAM";return false;}
    if((!cameraInitialized || !esp_camera_sensor_get()) && !(droneModeEnabled() && clusterCapturePrepareCamera())){
        reason="Kamera ist aus";return false;
    }
    if(!cameraWakeForUse()){reason="Kamera-Standby nicht beendet";return false;}
    return true;
}
}
bool clusterCaptureLiveFrame(uint16_t minWidth,const uint8_t *&bytes,size_t &length,String &reason){
    bytes=nullptr;length=0;
    if(liveBytes && liveMinWidth==minWidth && uint32_t(millis()-liveTakenMs)<kLiveReuseMs){
        bytes=liveBytes;length=liveLength;liveStats.cached=true;return true;
    }
    if(!liveCameraReady(reason))return false;
    const int64_t grabStart=esp_timer_get_time(); // includes returning stale frames
    // Return queued older frames first (see clusterCapturePhoto).
    camera_fb_t *stale[2]={nullptr,nullptr};
    const uint8_t n=clusterCaptureFrameBufferCount();
    for(uint8_t i=0;i<n && i<2;++i) stale[i]=esp_camera_fb_get();
    for(uint8_t i=0;i<n && i<2;++i) if(stale[i]) esp_camera_fb_return(stale[i]);
    camera_fb_t *fb=esp_camera_fb_get();
    const uint32_t grabUs=(uint32_t)(esp_timer_get_time()-grabStart);
    if(!fb){reason="Kamerabild fehlgeschlagen";return false;}
    if(fb->format!=PIXFORMAT_JPEG||!fb->buf||!fb->len){esp_camera_fb_return(fb);reason="Kamerabild fehlgeschlagen";return false;}
    liveClear();
    uint8_t *out=nullptr;size_t outLength=0;
    CameraThumbStats ts;
    liveStats={};
    liveStats.grabUs=grabUs;liveStats.srcWidth=fb->width;liveStats.srcHeight=fb->height;
    if(!minWidth || !cameraMakeThumbnail(fb->buf,fb->len,fb->width,fb->height,minWidth,out,outLength,&ts)){
        out=static_cast<uint8_t*>(heap_caps_malloc(fb->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(out){memcpy(out,fb->buf,fb->len);outLength=fb->len;}
        liveStats.width=fb->width;liveStats.height=fb->height;
    }else{
        liveStats.width=ts.width;liveStats.height=ts.height;
    }
    liveStats.decodeUs=ts.decodeUs;liveStats.encodeUs=ts.encodeUs;liveStats.path=ts.path;
    esp_camera_fb_return(fb);
    if(!out){reason="kein Speicher";return false;}
    liveBytes=out;liveLength=outLength;liveMinWidth=minWidth;liveTakenMs=millis();
    bytes=liveBytes;length=liveLength;
    return true;
}
ClusterLiveFrameStats clusterCaptureLiveStats(){return liveStats;}
void clusterCaptureLiveInvalidate(){liveClear();}
bool clusterCaptureMeasureLineTime(float &lineUs,uint32_t &vts,float &frameMs,String &reason){
    lineUs=0;vts=0;frameMs=0;
    if(!liveCameraReady(reason))return false;
    sensor_t *sensor=esp_camera_sensor_get();
    if(!sensor||!sensor->get_reg||(sensor->id.PID!=0x5640&&sensor->id.PID!=0x3660)){reason="nur OV3660/OV5640";return false;}
    // Return queued frames, then time consecutive frames. The shortest gap is
    // one frame period (a skipped frame only makes a gap longer).
    const uint8_t n=clusterCaptureFrameBufferCount();
    for(uint8_t i=0;i<n && i<2;++i){camera_fb_t *f=esp_camera_fb_get();if(f)esp_camera_fb_return(f);}
    constexpr int kFrames=6;
    int64_t prev=0,best=0;
    for(int i=0;i<kFrames;++i){
        camera_fb_t *f=esp_camera_fb_get();
        const int64_t got=esp_timer_get_time();
        if(!f){reason="Kamerabild fehlgeschlagen";return false;}
        const int64_t ts=(int64_t)f->timestamp.tv_sec*1000000LL+f->timestamp.tv_usec;
        esp_camera_fb_return(f);
        const int64_t t=ts>0?ts:got;
        if(i&&t>prev&&(!best||t-prev<best))best=t-prev;
        prev=t;
    }
    const int hi=sensor->get_reg(sensor,0x380E,0xFF),lo=sensor->get_reg(sensor,0x380F,0xFF);
    if(hi<0||lo<0){reason="Sensorregister nicht lesbar";return false;}
    vts=((uint32_t)hi<<8)|(uint32_t)lo;
    if(best<5000||best>2000000||vts<100){reason="Messung unplausibel";return false;}
    frameMs=best/1000.0f;
    lineUs=(float)best/(float)vts;
    return true;
}
String clusterCaptureLocalJson(){
    String s="[";bool first=true;
    for(size_t i=0;i<count;i++){
        const Frame &f=slots[i];if(!f.bytes)continue;
        if(!first)s+=",";first=false;
        char targetBuf[30],dispatchBuf[30];
        snprintf(targetBuf,sizeof(targetBuf),"%lld",(long long)f.target);
        snprintf(dispatchBuf,sizeof(dispatchBuf),"%lld",(long long)f.dispatch);
        s+="{\"uuid\":\""+String(f.uuid)+"\",\"boot\":"+String((unsigned long)(f.job>>32))+
            ",\"seq\":"+String((unsigned long)f.job)+
            ",\"bytes\":"+String((unsigned long)f.length)+
            ",\"target_utc_us\":\""+String(targetBuf)+
            "\",\"dispatch_utc_us\":\""+String(dispatchBuf)+"\"}";
    }
    return s+"]";
}
