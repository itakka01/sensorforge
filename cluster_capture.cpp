#include "cluster_capture.h"
#include "cluster.h"
#include "recorder.h"
#include "streamer.h"
#include "sync_api.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

extern bool recording;
extern bool cameraInitialized;
namespace {
struct Frame {
    uint8_t *bytes=nullptr;
    size_t length=0;
    char uuid[37]={};
    uint64_t job=0;
    int64_t target=0, dispatch=0;
};
constexpr size_t kMaxFrames=3;
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
    for(auto &f:slots){if(f.bytes)heap_caps_free(f.bytes);f=Frame{};}
    count=0;activeJob=jobId;
}
uint16_t clusterCapturePhoto(uint64_t jobId,int64_t targetUtcUs,int64_t &dispatchUtcUs,uint32_t &bytes) {
    bytes=0;dispatchUtcUs=0;
    if(jobId!=activeJob)return 5;
    if(count>=kMaxFrames)return 6; // Job full: NEVER overwrite earlier synchronized images.
    if (!cameraInitialized || !esp_camera_sensor_get() || recording || recorderIsOpen() ||
        streamerModeEnabled() || syncApiExclusiveActive()) return 1;
    if(!psramFound())return 2;
    // Software dispatch, NOT sensor exposure-start timestamp.
    clusterTimeNowUs(dispatchUtcUs);
    camera_fb_t *fb=esp_camera_fb_get();
    if(!fb)return 3;
    if(fb->format!=PIXFORMAT_JPEG||!fb->buf||!fb->len){esp_camera_fb_return(fb);return 3;}
    void *copy=heap_caps_malloc(fb->len,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!copy){esp_camera_fb_return(fb);return 6;} // Capacity exhausted; do not evict.
    memcpy(copy,fb->buf,fb->len);
    size_t size=fb->len;
    esp_camera_fb_return(fb);
    Frame &f=slots[count++];
    f.bytes=(uint8_t*)copy;f.length=size;f.job=jobId;
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
