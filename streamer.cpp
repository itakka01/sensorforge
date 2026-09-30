#include "streamer.h"

#include "audio_capture.h"
#include "config.h"
#include "logger.h"
#include "thermal.h"

#include <WebServer.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <sys/time.h>

// Camera lifecycle is owned by the main firmware. The streamer requests a
// controlled reinitialization only while operating_mode=streamer.
extern bool cameraInitialized;
extern bool initCamera(const String &model, const String &resolution, int quality);

namespace {

static const uint16_t RTSP_PORT = 554;
static const uint16_t HTTP_MJPEG_PORT = 81;
static const uint8_t RTP_PT_JPEG = 26;
static const uint8_t RTP_PT_L16 = 96;
static const size_t RTP_PACKET_BYTES = 1400;
static const uint32_t CLIENT_STALL_TIMEOUT_MS = 250;
// Per-client live-latency control. A client that starts consuming most of the
// frame interval is not disconnected. Instead SensorForge gradually thins only
// that client's future video frames so the existing TCP backlog can drain.
// Level 1 sends every second frame, level 2 every third, level 3 every fourth.
// Recovery is intentionally slower than escalation to avoid oscillation.
static const uint8_t RTSP_LAG_MAX_DIVIDER = 4;
static const uint8_t RTSP_LAG_SLOW_SAMPLES_TO_STEP_UP = 2;
static const uint8_t RTSP_LAG_HEALTHY_SAMPLES_TO_STEP_DOWN = 16;
static const uint8_t RTSP_LAG_ENTER_PERCENT = 60;
static const uint8_t RTSP_LAG_RECOVER_PERCENT = 40;
static const uint32_t RTSP_IDLE_TIMEOUT_MS = 30000;
// Application-level self-healing. The existing 30 s task watchdog remains the
// final protection for a hard task/deadlock stall.
static const uint8_t CAMERA_CAPTURE_FAILURES_BEFORE_RECOVERY = 3;
static const uint32_t STREAM_FRAME_STALL_MS = 15000;
static const uint32_t CAMERA_RECOVERY_RETRY_MS = 5000;
static const uint8_t CAMERA_RECOVERY_FAILURES_BEFORE_REBOOT = 3;
// Low-overhead long-run health checks. These are deliberately slow and only
// inspect already available counters/state.
static const uint32_t NETWORK_HEALTH_INTERVAL_MS = 5000;
static const uint8_t NETWORK_BAD_CHECKS_BEFORE_RECOVERY = 3;
static const uint32_t AUDIO_HEALTH_INTERVAL_MS = 2000;
static const uint32_t AUDIO_PROGRESS_STALL_MS = 8000;
static const uint32_t AUDIO_RECOVERY_RETRY_MS = 10000;
// RTSP audio is a live stream, not an archival queue. Capture produces about
// 32 KiB/s at the XIAO default (16 kHz / 16 bit / mono), so serviceAudio()
// must drain multiple short packets per firmware loop. Keep a small jitter
// reserve, but never allow seconds of stale PCM to accumulate in PSRAM.
static const uint32_t AUDIO_RTP_PACKET_TARGET_MS = 20;
static const uint32_t AUDIO_TARGET_BACKLOG_MS = 220;
static const uint32_t AUDIO_HIGH_BACKLOG_MS = 350;
static const uint32_t AUDIO_SOFT_TRIM_BACKLOG_MS = 500;
static const uint32_t AUDIO_STALE_BACKLOG_MS = 1000;
static const uint32_t AUDIO_SOFT_TRIM_STEP_MS = 20;
static const uint32_t AUDIO_STALE_TRIM_STEP_MS = 80;
static const uint32_t AUDIO_START_GRACE_MS = 3000;
static const uint32_t AUDIO_DRAIN_BUDGET_NORMAL_MS = 35;
static const uint32_t AUDIO_DRAIN_BUDGET_HIGH_MS = 80;
static const uint32_t AUDIO_DRAIN_BUDGET_STALE_MS = 120;
static const uint8_t AUDIO_DRAIN_MAX_PACKETS_PER_CALL = 32;
static const uint32_t MEMORY_HEALTH_INTERVAL_MS = 60000;
static const uint8_t MEMORY_CRITICAL_CHECKS_BEFORE_REBOOT = 3;
static const uint32_t HEALTH_PERSIST_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
// Streamer thermal soft-throttling deliberately starts close to the board-specific
// emergency threshold. It only reduces capture/network cadence; the
// firmware thermal guard and its emergency policy remain authoritative.
static constexpr float STREAM_THERMAL_STAGE1_C = 77.0f;
static constexpr float STREAM_THERMAL_STAGE2_C = 78.0f;
static constexpr float STREAM_THERMAL_STAGE1_RELEASE_C = 76.0f;
static constexpr float STREAM_THERMAL_STAGE2_RELEASE_C = 76.5f;
static const uint32_t STREAM_THERMAL_CHECK_INTERVAL_MS = 5000UL;
static const uint32_t STREAM_IDLE_CAPTURE_INTERVAL_MS = 5000UL;
// Diagnostic stress measurements are manual, time-bounded and never weaken
// the thermal guard. Camera-only mode exercises the normal JPEG capture path
// without creating synthetic network traffic.
static const uint32_t STREAM_STRESS_DEFAULT_MS = 60000UL;
static const uint32_t STREAM_STRESS_MIN_MS = 10000UL;
static const uint32_t STREAM_STRESS_MAX_MS = 300000UL;

static WiFiServer rtspServer(RTSP_PORT);
static WiFiServer httpServer(HTTP_MJPEG_PORT);
static const uint8_t RTSP_CLIENT_SLOTS = 2;
struct RtspClientSlot {
    WiFiClient client;
    bool playing = false;
    bool videoSetup = false;
    bool audioSetup = false;
    uint8_t videoChannel = 0;
    uint8_t audioChannel = 2;
    uint32_t session = 0;
    uint32_t lastActivityMs = 0;
    String rx;
    uint16_t videoSequence = 0;
    uint16_t audioSequence = 0;
    uint32_t videoSsrc = 0;
    uint32_t audioSsrc = 0;
    uint32_t audioTimestampBase = 0;
    // RTP A/V session origin. Both tracks are anchored at the same PLAY instant.
    // Audio payload is captured once and fanned out, while each RTSP client keeps
    // its own RTP sequence/SSRC/timestamp base.
    int64_t playStartedUs = 0;
    uint32_t videoPlayTimestamp = 0;
    uint64_t audioSampleOrigin = 0;
    uint64_t audioSamplesSent = 0;
    // RTCP sender-report state is strictly per client and per media track.
    uint32_t videoRtpPacketsSent = 0;
    uint32_t videoRtpOctetsSent = 0;
    uint32_t audioRtpPacketsSent = 0;
    uint32_t audioRtpOctetsSent = 0;
    uint32_t lastVideoRtpTimestamp = 0;
    uint32_t lastAudioRtpTimestamp = 0;
    uint32_t lastRtcpVideoMs = 0;
    uint32_t lastRtcpAudioMs = 0;
    uint32_t videoRtcpReportsSent = 0;
    uint32_t audioRtcpReportsSent = 0;
    // Adaptive live-latency state. Divider 1 = every frame, 2 = every second
    // frame, etc. This state is strictly per client so one slow viewer does not
    // force the other RTSP viewer to lose frames.
    uint8_t videoFrameDivider = 1;
    uint8_t videoFramePhase = 0;
    uint8_t videoSlowSamples = 0;
    uint8_t videoHealthySamples = 0;
    uint32_t videoLastSendUs = 0;
    uint32_t videoFramesSkipped = 0;
    uint32_t videoAdaptiveChanges = 0;
};
static RtspClientSlot rtspClients[RTSP_CLIENT_SLOTS];
static const uint8_t HTTP_CLIENT_SLOTS = 2;
struct HttpClientSlot {
    WiFiClient client;
    bool streaming = false;
    String rx;
    uint32_t lastActivityMs = 0;
};
static HttpClientSlot httpClients[HTTP_CLIENT_SLOTS];
static WebServer *webServer = nullptr;

static bool started = false;
static bool audioActive = false;
static uint32_t audioStartedMs = 0;
static bool audioAdvertised = false;
static String lastError;

static uint32_t videoTimestamp = 0;
static uint32_t videoTimestampBase = 0;
static int64_t videoClockStartUs = 0;
static uint64_t audioBytesSent = 0;
static uint32_t audioPacketsSent = 0;
// Monotonic sample position of PCM blocks consumed from the shared capture ring.
// Every active RTSP client sees the same block/sample position; clients differ
// only in their RTP transport state and per-session origin.
static uint64_t audioSamplesDistributed = 0;
static uint64_t audioLiveDiscardBytes = 0;
static uint32_t audioDrainPacketsLast = 0;
static uint32_t audioDrainPacketsMax = 0;
static uint32_t lastFrameDueMs = 0;
static uint32_t statsStartedMs = 0;
static uint32_t framesCaptured = 0;
static uint32_t framesRtsp = 0;
static uint32_t framesHttp = 0;
static uint64_t bytesSent = 0;
// Passive streaming telemetry. These counters only accumulate values already
// known in the hot path; no frame hashing/decoding or extra network traffic.
static uint64_t jpegBytesCaptured = 0;
static uint32_t jpegFramesCaptured = 0;
static uint32_t jpegMaxBytes = 0;
static uint64_t rtspVideoPackets = 0;
static uint64_t rtspVideoWireBytes = 0;
static uint64_t httpVideoBytes = 0;
static uint64_t rtspFrameSendUsTotal = 0;
static uint32_t rtspFrameSendCount = 0;
static uint32_t rtspFrameSendMaxUs = 0;

enum class StreamerStressMode : uint8_t {
    None = 0,
    CameraOnly,
    HttpMeasure,
    Observe
};
struct StreamerStressState {
    StreamerStressMode mode = StreamerStressMode::None;
    uint32_t startedMs = 0;
    uint32_t durationMs = 0;
    uint32_t framesStart = 0;
    uint64_t jpegBytesStart = 0;
    uint64_t rtspPacketsStart = 0;
    uint64_t rtspBytesStart = 0;
    uint64_t httpBytesStart = 0;
    uint32_t audioPacketsStart = 0;
    uint64_t audioBytesStart = 0;
    uint64_t totalBytesStart = 0;
    uint32_t maxJpegBytes = 0;
    uint32_t maxRtspFrameSendUs = 0;
    float startTempC = NAN;
    float peakTempC = NAN;
};
static StreamerStressState stressState;
static uint32_t snapshotLastMs = 0;
static uint8_t *snapshotBuffer = nullptr;
static size_t snapshotCapacity = 0;
static size_t snapshotBytes = 0;
// Internal WebConfig preview demand expires automatically if a browser tab is
// closed or disappears without sending /preview_stop. Snapshot polling normally
// refreshes this timestamp every ~200 ms.
static uint32_t previewDemandLastMs = 0;
static const uint32_t PREVIEW_DEMAND_TIMEOUT_MS = 2500UL;

static uint32_t lastSuccessfulFrameMs = 0;
static uint32_t nextCameraRecoveryAllowedMs = 0;
static uint32_t cameraRecoveryCount = 0;
static uint32_t cameraRecoveryFailureCount = 0;
static uint32_t consecutiveCaptureFailures = 0;
static String lastRecoveryReason;

static uint8_t thermalThrottleLevel = 0;
static uint32_t lastThermalThrottleCheckMs = 0;
static float lastStreamerCpuTempC = NAN;
static float thermalThrottlePeakC = NAN;
static uint32_t thermalThrottleEpisodeStartedMs = 0;
static bool streamDemandActive = false;

static uint32_t socketStallCount = 0;
static uint32_t audioRecoveryCount = 0;
static uint32_t audioRecoveryFailureCount = 0;
static uint32_t networkRecoveryCount = 0;
static uint32_t networkRecoveryFailureCount = 0;
static uint32_t lastAudioHealthMs = 0;
static uint32_t lastAudioProgressMs = 0;
static uint64_t lastAudioCapturedBytes = 0;
static uint32_t nextAudioRecoveryAllowedMs = 0;
static uint32_t lastNetworkHealthMs = 0;
static uint8_t networkBadChecks = 0;
static bool networkRecoveryRequested = false;
static String networkRecoveryReason;
static uint32_t nextNetworkRecoveryAllowedMs = 0;
static uint32_t lastMemoryHealthMs = 0;
static uint32_t bootInternalHeap = 0;
static uint32_t bootPsramFree = 0;
static uint32_t currentInternalHeap = 0;
static uint32_t currentPsramFree = 0;
static uint8_t memoryCriticalChecks = 0;
static bool memoryTrendWarning = false;
static uint32_t heapHistory[6] = {};
static uint8_t heapHistoryCount = 0;
static uint8_t heapHistoryPos = 0;

static Preferences healthPrefs;
static bool healthPrefsOpen = false;
static bool healthPrefsLoaded = false;
static bool healthPrefsDirty = false;
static uint32_t lastHealthPersistMs = 0;
static uint32_t persistedCameraRecoveries = 0;
static uint32_t persistedAudioRecoveries = 0;
static uint32_t persistedNetworkRecoveries = 0;
static uint32_t persistedSocketStalls = 0;
static String previousControlledResetReason;

static const char *resetReasonName(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt-watchdog";
        case ESP_RST_TASK_WDT: return "task-watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}

static void openHealthPrefsIfNeeded()
{
    if (healthPrefsOpen)
        return;
    healthPrefsOpen = healthPrefs.begin("sfstrhealth", false);
    if (!healthPrefsOpen)
        return;

    if (!healthPrefsLoaded) {
        persistedCameraRecoveries = healthPrefs.getUInt("cam_rec", 0);
        persistedAudioRecoveries = healthPrefs.getUInt("aud_rec", 0);
        persistedNetworkRecoveries = healthPrefs.getUInt("net_rec", 0);
        persistedSocketStalls = healthPrefs.getUInt("sock_stall", 0);
        previousControlledResetReason = healthPrefs.getString("reset_reason", "");
        if (previousControlledResetReason.length())
            healthPrefs.remove("reset_reason");
        healthPrefsLoaded = true;
    }
}

static void persistHealthCounters(bool force)
{
    openHealthPrefsIfNeeded();
    if (!healthPrefsOpen || !healthPrefsDirty)
        return;
    const uint32_t now = millis();
    if (!force && (uint32_t)(now - lastHealthPersistMs) < HEALTH_PERSIST_INTERVAL_MS)
        return;

    healthPrefs.putUInt("cam_rec", persistedCameraRecoveries);
    healthPrefs.putUInt("aud_rec", persistedAudioRecoveries);
    healthPrefs.putUInt("net_rec", persistedNetworkRecoveries);
    healthPrefs.putUInt("sock_stall", persistedSocketStalls);
    lastHealthPersistMs = now;
    healthPrefsDirty = false;
}

static void persistControlledResetReason(const String &reason)
{
    openHealthPrefsIfNeeded();
    if (healthPrefsOpen) {
        healthPrefs.putString("reset_reason", reason);
        healthPrefs.putUInt("cam_rec", persistedCameraRecoveries);
        healthPrefs.putUInt("aud_rec", persistedAudioRecoveries);
        healthPrefs.putUInt("net_rec", persistedNetworkRecoveries);
        healthPrefs.putUInt("sock_stall", persistedSocketStalls);
    }
}

struct ParsedJpeg {
    const uint8_t *scan;
    size_t scanBytes;
    uint16_t width;
    uint16_t height;
    uint8_t type;
    uint8_t quant[128];
    size_t quantBytes;
};

static uint32_t effectiveStreamerFps();
static uint32_t videoTimestampForNow();

static bool rtspSlotConnected(RtspClientSlot &slot)
{
    return slot.client && slot.client.connected();
}

static bool anyRtspAudioConsumer()
{
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (rtspSlotConnected(slot) && slot.playing && slot.audioSetup)
            return true;
    }
    return false;
}

static void closeRtspClientSlot(uint8_t index)
{
    if (index >= RTSP_CLIENT_SLOTS)
        return;

    RtspClientSlot &slot = rtspClients[index];
    if (slot.client)
        slot.client.stop();
    slot = RtspClientSlot();

    if (audioActive && !anyRtspAudioConsumer()) {
        audioCaptureStop();
        audioActive = false;
        audioStartedMs = 0;
    }
}

static void closeAllRtspClients()
{
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i)
        closeRtspClientSlot(i);
}

static int findFreeRtspSlot()
{
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        if (!rtspSlotConnected(rtspClients[i])) {
            closeRtspClientSlot(i);
            return (int)i;
        }
    }
    return -1;
}

static void closeHttpClientSlot(uint8_t index)
{
    if (index >= HTTP_CLIENT_SLOTS)
        return;
    HttpClientSlot &slot = httpClients[index];
    if (slot.client)
        slot.client.stop();
    slot.client = WiFiClient();
    slot.streaming = false;
    slot.rx = "";
    slot.lastActivityMs = 0;
}

static void closeAllHttpClients()
{
    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i)
        closeHttpClientSlot(i);
}

static int findFreeHttpSlot()
{
    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        if (!httpClients[i].client || !httpClients[i].client.connected()) {
            closeHttpClientSlot(i);
            return (int)i;
        }
    }
    return -1;
}

static bool writeAll(WiFiClient &client, const uint8_t *data, size_t len)
{
    size_t offset = 0;
    uint32_t lastProgress = millis();

    while (offset < len && client.connected()) {
        size_t chunk = std::min((size_t)4096, len - offset);
        size_t written = client.write(data + offset, chunk);

        if (written > 0) {
            offset += written;
            bytesSent += written;
            lastProgress = millis();
            continue;
        }

        if ((uint32_t)(millis() - lastProgress) >= CLIENT_STALL_TIMEOUT_MS) {
            ++socketStallCount;
            ++persistedSocketStalls;
            healthPrefsDirty = true;
            return false;
        }

        delay(0);
    }

    return offset == len;
}

static bool writeText(WiFiClient &client, const String &text)
{
    return writeAll(client, (const uint8_t *)text.c_str(), text.length());
}

static bool parseJpeg(const uint8_t *jpeg, size_t len, ParsedJpeg &out)
{
    memset(&out, 0, sizeof(out));

    if (!jpeg || len < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
        return false;

    size_t p = 2;
    bool haveSof = false;
    bool haveQ0 = false;
    bool haveQ1 = false;

    while (p + 4 <= len) {
        if (jpeg[p] != 0xff) {
            ++p;
            continue;
        }

        while (p < len && jpeg[p] == 0xff)
            ++p;
        if (p >= len)
            break;

        uint8_t marker = jpeg[p++];
        if (marker == 0xd9)
            break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7))
            continue;

        if (p + 2 > len)
            return false;

        uint16_t segLen = ((uint16_t)jpeg[p] << 8) | jpeg[p + 1];
        if (segLen < 2 || p + segLen > len)
            return false;

        const uint8_t *seg = jpeg + p + 2;
        size_t dataLen = segLen - 2;

        if (marker == 0xdb) {
            size_t q = 0;
            while (q < dataLen) {
                uint8_t pqTq = seg[q++];
                uint8_t precision = pqTq >> 4;
                uint8_t table = pqTq & 0x0f;
                size_t tableBytes = precision ? 128U : 64U;
                if (q + tableBytes > dataLen)
                    return false;

                if (precision != 0) {
                    // Current ESP camera JPEGs use 8-bit tables. RFC2435 also
                    // allows 16-bit tables, but advertising those here would
                    // require a larger first RTP fragment and is not qualified.
                    return false;
                }

                if (table == 0) {
                    memcpy(out.quant, seg + q, 64);
                    haveQ0 = true;
                } else if (table == 1) {
                    memcpy(out.quant + 64, seg + q, 64);
                    haveQ1 = true;
                }
                q += tableBytes;
            }
        } else if (marker == 0xc0) {
            if (dataLen < 8 || seg[0] != 8)
                return false;

            out.height = ((uint16_t)seg[1] << 8) | seg[2];
            out.width = ((uint16_t)seg[3] << 8) | seg[4];
            uint8_t components = seg[5];
            if (components != 3 || dataLen < 6U + (size_t)components * 3U)
                return false;

            uint8_t ySampling = seg[7];
            if (ySampling == 0x21)
                out.type = 0; // 4:2:2
            else if (ySampling == 0x22)
                out.type = 1; // 4:2:0
            else
                return false;

            haveSof = true;
        } else if (marker == 0xc2) {
            // Progressive JPEG is outside the deliberately small first RTSP release.
            return false;
        } else if (marker == 0xdd) {
            // RFC2435 requires the restart-marker RTP/JPEG extension when a DRI
            // is present. Current SensorForge camera JPEGs are expected without
            // restart intervals; reject an unexpected one rather than emit a
            // non-conformant stream.
            return false;
        } else if (marker == 0xda) {
            size_t scanStart = p + segLen;
            if (!haveSof || !haveQ0 || scanStart >= len)
                return false;

            size_t scanEnd = len;
            if (scanEnd >= 2 && jpeg[scanEnd - 2] == 0xff && jpeg[scanEnd - 1] == 0xd9)
                scanEnd -= 2;

            if (scanEnd <= scanStart)
                return false;

            out.scan = jpeg + scanStart;
            out.scanBytes = scanEnd - scanStart;

            // Type 0/1 expects two tables. Some encoders may reference one
            // chroma table without emitting it separately; duplicate luma only
            // as a defensive fallback rather than sending an invalid length.
            if (!haveQ1)
                memcpy(out.quant + 64, out.quant, 64);
            out.quantBytes = 128;
            return true;
        }

        p += segLen;
    }

    return false;
}

static bool sendInterleaved(RtspClientSlot &slot, uint8_t channel, const uint8_t *packet, size_t len)
{
    if (!rtspSlotConnected(slot) || len > 65535)
        return false;

    uint8_t prefix[4] = {
        '$', channel,
        (uint8_t)((len >> 8) & 0xff),
        (uint8_t)(len & 0xff)
    };

    return writeAll(slot.client, prefix, sizeof(prefix)) &&
           writeAll(slot.client, packet, len);
}


static void currentNtpTimestamp(uint32_t &seconds, uint32_t &fraction)
{
    timeval tv = {};
    gettimeofday(&tv, nullptr);
    // NTP epoch starts 1900-01-01, Unix epoch 1970-01-01.
    const uint64_t ntpSeconds = (uint64_t)tv.tv_sec + 2208988800ULL;
    seconds = (uint32_t)ntpSeconds;
    fraction = (uint32_t)(((uint64_t)tv.tv_usec << 32) / 1000000ULL);
}

static bool sendRtcpSenderReport(RtspClientSlot &slot, bool audio)
{
    if (!rtspSlotConnected(slot) || !slot.playing)
        return false;

    const bool trackReady = audio ? slot.audioSetup : slot.videoSetup;
    if (!trackReady)
        return true;

    // RFC3550 compound RTCP: Sender Report followed by one SDES CNAME chunk.
    // This gives standard clients a common NTP wall-clock reference for the
    // otherwise independent 90-kHz video and audio sample clocks.
    uint8_t packet[128] = {};
    size_t o = 0;

    const uint32_t ssrc = audio ? slot.audioSsrc : slot.videoSsrc;
    const uint32_t rtpTimestamp = audio
        ? (slot.audioTimestampBase + (uint32_t)(((uint64_t)std::max<int64_t>(0, esp_timer_get_time() - slot.playStartedUs) *
                                                (uint64_t)std::max(1, cfg_audio_sample_rate)) / 1000000ULL))
        : videoTimestampForNow();
    const uint32_t packetCount = audio ? slot.audioRtpPacketsSent : slot.videoRtpPacketsSent;
    const uint32_t octetCount = audio ? slot.audioRtpOctetsSent : slot.videoRtpOctetsSent;

    uint32_t ntpSec = 0, ntpFrac = 0;
    currentNtpTimestamp(ntpSec, ntpFrac);

    // Sender Report, 28 bytes.
    packet[o++] = 0x80; // V=2, P=0, RC=0
    packet[o++] = 200;  // SR
    packet[o++] = 0;
    packet[o++] = 6;    // 28 bytes => 7 32-bit words => length field 6
    const uint32_t srWords[6] = {ssrc, ntpSec, ntpFrac, rtpTimestamp, packetCount, octetCount};
    for (uint32_t word : srWords) {
        packet[o++] = (uint8_t)(word >> 24);
        packet[o++] = (uint8_t)(word >> 16);
        packet[o++] = (uint8_t)(word >> 8);
        packet[o++] = (uint8_t)word;
    }

    // SDES with one CNAME item. Keep the name short and deterministic.
    String cname = String("sensorforge-") + String(slot.session, HEX) + "@" + cfg_hostname;
    if (cname.length() > 63)
        cname.remove(63);
    const size_t sdesStart = o;
    packet[o++] = 0x81; // V=2, SC=1
    packet[o++] = 202;  // SDES
    packet[o++] = 0;    // length filled after padding
    packet[o++] = 0;
    packet[o++] = (uint8_t)(ssrc >> 24);
    packet[o++] = (uint8_t)(ssrc >> 16);
    packet[o++] = (uint8_t)(ssrc >> 8);
    packet[o++] = (uint8_t)ssrc;
    packet[o++] = 1; // CNAME
    packet[o++] = (uint8_t)cname.length();
    memcpy(packet + o, cname.c_str(), cname.length());
    o += cname.length();
    packet[o++] = 0; // END item
    while ((o - sdesStart) & 3U)
        packet[o++] = 0;
    const uint16_t sdesWordsMinusOne = (uint16_t)(((o - sdesStart) / 4U) - 1U);
    packet[sdesStart + 2] = (uint8_t)(sdesWordsMinusOne >> 8);
    packet[sdesStart + 3] = (uint8_t)sdesWordsMinusOne;

    const uint8_t rtcpChannel = (uint8_t)((audio ? slot.audioChannel : slot.videoChannel) + 1U);
    return sendInterleaved(slot, rtcpChannel, packet, o);
}

static void serviceRtcpSenderReports()
{
    const uint32_t now = millis();
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (!rtspSlotConnected(slot) || !slot.playing)
            continue;

        if (slot.videoSetup && (slot.lastRtcpVideoMs == 0 || (uint32_t)(now - slot.lastRtcpVideoMs) >= 1000U)) {
            if (sendRtcpSenderReport(slot, false)) {
                slot.lastRtcpVideoMs = now;
                ++slot.videoRtcpReportsSent;
            }
            else {
                closeRtspClientSlot(i);
                continue;
            }
        }
        if (slot.audioSetup && audioActive &&
            (slot.lastRtcpAudioMs == 0 || (uint32_t)(now - slot.lastRtcpAudioMs) >= 1000U)) {
            if (sendRtcpSenderReport(slot, true)) {
                slot.lastRtcpAudioMs = now;
                ++slot.audioRtcpReportsSent;
            }
            else
                closeRtspClientSlot(i);
        }
    }
}

static bool shouldSendRtspVideoFrame(RtspClientSlot &slot)
{
    const uint8_t divider = std::max<uint8_t>(1U, slot.videoFrameDivider);
    const bool sendThisFrame = slot.videoFramePhase == 0;
    slot.videoFramePhase = (uint8_t)((slot.videoFramePhase + 1U) % divider);
    if (!sendThisFrame)
        ++slot.videoFramesSkipped;
    return sendThisFrame;
}

static void setRtspVideoDivider(RtspClientSlot &slot, uint8_t divider, uint32_t sendUs)
{
    divider = std::max<uint8_t>(1U, std::min<uint8_t>(RTSP_LAG_MAX_DIVIDER, divider));
    if (divider == slot.videoFrameDivider)
        return;

    const uint8_t oldDivider = slot.videoFrameDivider;
    slot.videoFrameDivider = divider;
    slot.videoFramePhase = 0;
    slot.videoSlowSamples = 0;
    slot.videoHealthySamples = 0;
    ++slot.videoAdaptiveChanges;

    String msg = "RTSP live-latency adapt | session=" + String(slot.session) +
                 " | divider=" + String((unsigned)oldDivider) + "->" + String((unsigned)divider) +
                 " | send_ms=" + String(sendUs / 1000.0f, 1) +
                 " | skipped=" + String(slot.videoFramesSkipped);
    logWrite(msg);
}

static void updateRtspVideoLagState(RtspClientSlot &slot, uint32_t sendUs)
{
    slot.videoLastSendUs = sendUs;

    const uint32_t fps = std::max<uint32_t>(1U, effectiveStreamerFps());
    const uint32_t frameBudgetUs = 1000000UL / fps;
    const uint32_t enterUs = (frameBudgetUs * RTSP_LAG_ENTER_PERCENT) / 100U;
    const uint32_t recoverUs = (frameBudgetUs * RTSP_LAG_RECOVER_PERCENT) / 100U;

    if (sendUs >= enterUs) {
        slot.videoHealthySamples = 0;
        if (slot.videoSlowSamples < 255)
            ++slot.videoSlowSamples;
        if (slot.videoSlowSamples >= RTSP_LAG_SLOW_SAMPLES_TO_STEP_UP &&
            slot.videoFrameDivider < RTSP_LAG_MAX_DIVIDER) {
            setRtspVideoDivider(slot, (uint8_t)(slot.videoFrameDivider + 1U), sendUs);
        }
        return;
    }

    slot.videoSlowSamples = 0;
    if (sendUs <= recoverUs) {
        if (slot.videoHealthySamples < 255)
            ++slot.videoHealthySamples;
        if (slot.videoHealthySamples >= RTSP_LAG_HEALTHY_SAMPLES_TO_STEP_DOWN &&
            slot.videoFrameDivider > 1U) {
            setRtspVideoDivider(slot, (uint8_t)(slot.videoFrameDivider - 1U), sendUs);
        }
    } else {
        // Neutral zone: keep the current divider and require a fresh healthy
        // run before restoring more frames.
        slot.videoHealthySamples = 0;
    }
}

static uint32_t videoTimestampForNow()
{
    // RTP/JPEG uses a 90 kHz clock. Derive it from the real monotonic clock,
    // not from the configured FPS. This prevents receiver jitter/playback
    // buffers from growing when multi-client TCP delivery temporarily makes
    // the actual frame cadence slower than the configured capture cadence.
    const int64_t nowUs = esp_timer_get_time();
    if (videoClockStartUs <= 0)
        videoClockStartUs = nowUs;
    const uint64_t elapsedUs = (uint64_t)std::max<int64_t>(0, nowUs - videoClockStartUs);
    return videoTimestampBase + (uint32_t)((elapsedUs * 90ULL) / 1000ULL);
}

static bool sendRtpJpeg(RtspClientSlot &slot, const uint8_t *jpeg, size_t jpegBytes)
{
    const uint32_t sendStartedUs = micros();
    ParsedJpeg parsed;
    if (!parseJpeg(jpeg, jpegBytes, parsed)) {
        lastError = "JPEG format is not supported by RFC2435 packetizer";
        return false;
    }

    if (parsed.width == 0 || parsed.height == 0 || parsed.width > 2040 || parsed.height > 2040) {
        lastError = "RTSP/RFC2435 supports dimensions up to 2040 pixels";
        return false;
    }

    size_t offset = 0;
    while (offset < parsed.scanBytes) {
        uint8_t packet[RTP_PACKET_BYTES];
        size_t h = 0;
        const bool first = offset == 0;

        packet[h++] = 0x80;
        packet[h++] = RTP_PT_JPEG;
        packet[h++] = (uint8_t)(slot.videoSequence >> 8);
        packet[h++] = (uint8_t)slot.videoSequence;
        packet[h++] = (uint8_t)(videoTimestamp >> 24);
        packet[h++] = (uint8_t)(videoTimestamp >> 16);
        packet[h++] = (uint8_t)(videoTimestamp >> 8);
        packet[h++] = (uint8_t)videoTimestamp;
        packet[h++] = (uint8_t)(slot.videoSsrc >> 24);
        packet[h++] = (uint8_t)(slot.videoSsrc >> 16);
        packet[h++] = (uint8_t)(slot.videoSsrc >> 8);
        packet[h++] = (uint8_t)slot.videoSsrc;

        packet[h++] = 0; // type-specific: progressive/full frame
        packet[h++] = (uint8_t)((offset >> 16) & 0xff);
        packet[h++] = (uint8_t)((offset >> 8) & 0xff);
        packet[h++] = (uint8_t)(offset & 0xff);
        packet[h++] = parsed.type;
        packet[h++] = 255; // explicit quantization tables
        packet[h++] = (uint8_t)((parsed.width + 7U) / 8U);
        packet[h++] = (uint8_t)((parsed.height + 7U) / 8U);

        if (first) {
            packet[h++] = 0; // MBZ
            packet[h++] = 0; // 8-bit precision for both tables
            packet[h++] = 0;
            packet[h++] = (uint8_t)parsed.quantBytes;
            memcpy(packet + h, parsed.quant, parsed.quantBytes);
            h += parsed.quantBytes;
        }

        size_t payloadRoom = sizeof(packet) - h;
        size_t fragment = std::min(payloadRoom, parsed.scanBytes - offset);
        const bool last = offset + fragment >= parsed.scanBytes;
        if (last)
            packet[1] |= 0x80;

        memcpy(packet + h, parsed.scan + offset, fragment);

        if (!sendInterleaved(slot, slot.videoChannel, packet, h + fragment))
            return false;

        ++rtspVideoPackets;
        rtspVideoWireBytes += (uint64_t)(h + fragment + 4U);
        ++slot.videoRtpPacketsSent;
        slot.videoRtpOctetsSent += (uint32_t)(h + fragment - 12U);
        slot.lastVideoRtpTimestamp = videoTimestamp;
        ++slot.videoSequence;
        offset += fragment;
        delay(0);
    }

    const uint32_t sendElapsedUs = (uint32_t)(micros() - sendStartedUs);
    ++rtspFrameSendCount;
    rtspFrameSendUsTotal += sendElapsedUs;
    if (sendElapsedUs > rtspFrameSendMaxUs)
        rtspFrameSendMaxUs = sendElapsedUs;
    if (stressState.mode != StreamerStressMode::None && sendElapsedUs > stressState.maxRtspFrameSendUs)
        stressState.maxRtspFrameSendUs = sendElapsedUs;

    slot.videoLastSendUs = sendElapsedUs;
    ++framesRtsp;
    return true;
}

static bool configuredAudioFormat(AudioFormat &format, String &error)
{
    if (!cfg_audio_enabled) {
        error = "audio disabled";
        return false;
    }

    AudioInputSettings input;
    if (!audioCaptureConfiguredInput(input, error))
        return false;

    format.sampleRate = (uint32_t)cfg_audio_sample_rate;
    format.bitsPerSample = (uint16_t)cfg_audio_bits_per_sample;
    format.channels = (uint8_t)cfg_audio_channels;

    if (format.bitsPerSample != 16) {
        error = "RTSP L16 currently requires 16-bit PCM";
        return false;
    }

    if (!audioCaptureFormatSupported(input, format, error))
        return false;

    return true;
}

static bool startAudioIfNeeded()
{
    if (!audioAdvertised || !anyRtspAudioConsumer())
        return true;

    if (audioActive)
        return true;

    AudioFormat format;
    String error;
    if (!configuredAudioFormat(format, error)) {
        lastError = "RTSP audio unavailable: " + error;
        return false;
    }

    if (!audioCaptureStart(format, error)) {
        lastError = "RTSP audio start failed: " + error;
        return false;
    }

    audioBytesSent = 0;
    audioPacketsSent = 0;
    audioSamplesDistributed = 0;
    audioLiveDiscardBytes = 0;
    audioDrainPacketsLast = 0;
    audioDrainPacketsMax = 0;
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (rtspSlotConnected(slot) && slot.playing && slot.audioSetup) {
            if (slot.audioTimestampBase == 0) {
                const uint32_t rnd = esp_random();
                slot.audioTimestampBase = rnd ? rnd : 1;
            }
            slot.audioSampleOrigin = audioSamplesDistributed;
            slot.audioSamplesSent = 0;
        }
    }
    audioActive = true;
    audioStartedMs = millis();
    logWrite("RTSP audio capture started | rate=" + String(format.sampleRate) +
             " | bits=" + String(format.bitsPerSample) +
             " | channels=" + String(format.channels));
    return true;
}

static void serviceAudio()
{
    if (!audioActive || !anyRtspAudioConsumer())
        return;

    AudioFormat format = audioCaptureActiveFormat();
    if (format.bitsPerSample != 16 || format.channels == 0 || format.sampleRate == 0)
        return;

    const size_t frameBytes = 2U * format.channels;
    const size_t maxPayloadBytes = 1200U - 12U;
    size_t packetBytes =
        ((size_t)format.sampleRate * frameBytes * AUDIO_RTP_PACKET_TARGET_MS) / 1000U;
    packetBytes -= packetBytes % frameBytes;
    if (packetBytes == 0)
        packetBytes = frameBytes;
    if (packetBytes > maxPayloadBytes) {
        packetBytes = maxPayloadBytes - (maxPayloadBytes % frameBytes);
    }

    const size_t bytesPerSecond = (size_t)format.sampleRate * frameBytes;
    const size_t targetBacklogBytes =
        std::max<size_t>(frameBytes, (bytesPerSecond * AUDIO_TARGET_BACKLOG_MS) / 1000U);
    const size_t highBacklogBytes =
        std::max<size_t>(frameBytes, (bytesPerSecond * AUDIO_HIGH_BACKLOG_MS) / 1000U);
    const size_t softTrimBacklogBytes =
        std::max<size_t>(frameBytes, (bytesPerSecond * AUDIO_SOFT_TRIM_BACKLOG_MS) / 1000U);
    const size_t staleBacklogBytes =
        std::max<size_t>(frameBytes, (bytesPerSecond * AUDIO_STALE_BACKLOG_MS) / 1000U);

    size_t buffered = audioCaptureBufferedBytes();

    // Live correction is intentionally gradual. Once the ring exceeds a modest
    // backlog, discard only a small bounded amount of the oldest complete PCM
    // per service pass instead of cutting a large hole down to the target in one
    // step. If a true >1 s backlog exists, the step is larger but remains
    // bounded. This keeps the stream near live while reducing audible clicks.
    const bool audioStartGraceActive =
        audioStartedMs != 0 && (uint32_t)(millis() - audioStartedMs) < AUDIO_START_GRACE_MS;

    if (!audioStartGraceActive && buffered > softTrimBacklogBytes) {
        const uint32_t trimStepMs = buffered > staleBacklogBytes
            ? AUDIO_STALE_TRIM_STEP_MS
            : AUDIO_SOFT_TRIM_STEP_MS;
        size_t maxDiscardBytes = (bytesPerSecond * trimStepMs) / 1000U;
        maxDiscardBytes -= maxDiscardBytes % frameBytes;
        size_t discardBytes = buffered > targetBacklogBytes
            ? (buffered - targetBacklogBytes)
            : 0;
        discardBytes = std::min(discardBytes, maxDiscardBytes);
        discardBytes -= discardBytes % frameBytes;

        uint8_t discardBuffer[1200 - 12];
        while (discardBytes > 0) {
            size_t chunk = std::min(sizeof(discardBuffer), discardBytes);
            chunk -= chunk % frameBytes;
            if (!chunk)
                break;
            const size_t got = audioCaptureRead(discardBuffer, chunk, 0);
            if (!got)
                break;
            const size_t aligned = got - (got % frameBytes);
            if (!aligned)
                break;
            audioSamplesDistributed += aligned / frameBytes;
            audioLiveDiscardBytes += aligned;
            discardBytes -= std::min(discardBytes, aligned);
            delay(0);
        }
        buffered = audioCaptureBufferedBytes();
    }

    uint32_t budgetMs = AUDIO_DRAIN_BUDGET_NORMAL_MS;
    if (buffered > staleBacklogBytes)
        budgetMs = AUDIO_DRAIN_BUDGET_STALE_MS;
    else if (buffered > highBacklogBytes)
        budgetMs = AUDIO_DRAIN_BUDGET_HIGH_MS;

    const uint32_t startedMs = millis();
    uint8_t packetsThisCall = 0;

    while (packetsThisCall < AUDIO_DRAIN_MAX_PACKETS_PER_CALL) {
        buffered = audioCaptureBufferedBytes();
        if (buffered < packetBytes)
            break;
        if (packetsThisCall > 0 && buffered <= targetBacklogBytes)
            break;
        if ((uint32_t)(millis() - startedMs) >= budgetMs)
            break;

        uint8_t pcm[1200 - 12];
        size_t bytes = audioCaptureRead(pcm, packetBytes, 0);
        bytes -= bytes % frameBytes;
        if (!bytes)
            break;

        // L16 uses network byte order. SensorForge's PCM capture is packed
        // little-endian. Convert once, then fan out this exact payload block to
        // every active RTSP client.
        for (size_t i = 0; i + 1 < bytes; i += 2)
            std::swap(pcm[i], pcm[i + 1]);

        const size_t samples = bytes / frameBytes;
        const uint64_t sharedSamplePosition = audioSamplesDistributed;

        for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
            RtspClientSlot &slot = rtspClients[i];
            if (!rtspSlotConnected(slot) || !slot.playing || !slot.audioSetup)
                continue;

            uint8_t packet[1200];
            const uint64_t sessionSamples =
                sharedSamplePosition >= slot.audioSampleOrigin
                    ? (sharedSamplePosition - slot.audioSampleOrigin)
                    : 0;
            const uint32_t timestamp =
                slot.audioTimestampBase + (uint32_t)sessionSamples;
            size_t h = 0;
            packet[h++] = 0x80;
            packet[h++] = RTP_PT_L16;
            packet[h++] = (uint8_t)(slot.audioSequence >> 8);
            packet[h++] = (uint8_t)slot.audioSequence;
            packet[h++] = (uint8_t)(timestamp >> 24);
            packet[h++] = (uint8_t)(timestamp >> 16);
            packet[h++] = (uint8_t)(timestamp >> 8);
            packet[h++] = (uint8_t)timestamp;
            packet[h++] = (uint8_t)(slot.audioSsrc >> 24);
            packet[h++] = (uint8_t)(slot.audioSsrc >> 16);
            packet[h++] = (uint8_t)(slot.audioSsrc >> 8);
            packet[h++] = (uint8_t)slot.audioSsrc;
            memcpy(packet + h, pcm, bytes);

            if (!sendInterleaved(slot, slot.audioChannel, packet, h + bytes)) {
                lastError = "RTSP audio client disconnected";
                closeRtspClientSlot(i);
                continue;
            }
            ++slot.audioSequence;
            ++slot.audioRtpPacketsSent;
            slot.audioRtpOctetsSent += (uint32_t)bytes;
            slot.lastAudioRtpTimestamp = timestamp;
            slot.audioSamplesSent = sessionSamples + samples;
            ++audioPacketsSent;
            audioBytesSent += bytes;
        }

        audioSamplesDistributed += samples;
        ++packetsThisCall;
        delay(0);
    }

    audioDrainPacketsLast += packetsThisCall;
}

static String headerValue(const String &request, const char *name)
{
    String needle = String("\r\n") + name + ":";
    int p = request.indexOf(needle);
    if (p < 0)
        return "";
    p += needle.length();
    int e = request.indexOf("\r\n", p);
    if (e < 0)
        e = request.length();
    String value = request.substring(p, e);
    value.trim();
    return value;
}

static IPAddress activeNetworkIp()
{
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        const IPAddress apIp = WiFi.softAPIP();
        if (apIp[0] || apIp[1] || apIp[2] || apIp[3])
            return apIp;
    }
    return WiFi.localIP();
}

static String rtspBaseUrl()
{
    return "rtsp://" + cfg_hostname + ".local:" + String(RTSP_PORT) + "/stream";
}

static void rtspReply(RtspClientSlot &slot, int code, const char *reason, const String &cseq,
                      const String &extra = "", const String &body = "")
{
    String response = "RTSP/1.0 " + String(code) + " " + reason + "\r\n";
    if (cseq.length())
        response += "CSeq: " + cseq + "\r\n";
    response += "Server: SensorForge/7.1.0\r\n";
    if (slot.session)
        response += "Session: " + String(slot.session, HEX) + "\r\n";
    response += extra;
    if (body.length()) {
        response += "Content-Length: " + String(body.length()) + "\r\n";
        response += "Content-Type: application/sdp\r\n";
    }
    response += "\r\n";
    response += body;
    writeText(slot.client, response);
}

static String makeSdp()
{
    AudioFormat format;
    String audioError;
    audioAdvertised = configuredAudioFormat(format, audioError);

    String sdp;
    sdp.reserve(512);
    sdp += "v=0\r\n";
    sdp += "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp += "s=SensorForge Network Streamer\r\n";
    sdp += "t=0 0\r\n";
    sdp += "c=IN IP4 " + activeNetworkIp().toString() + "\r\n";
    sdp += "a=control:*\r\n";
    sdp += "a=sendonly\r\n";
    sdp += "m=video 0 RTP/AVP 26\r\n";
    sdp += "a=rtpmap:26 JPEG/90000\r\n";
    sdp += "a=control:trackID=0\r\n";

    if (audioAdvertised) {
        sdp += "m=audio 0 RTP/AVP " + String(RTP_PT_L16) + "\r\n";
        sdp += "a=rtpmap:" + String(RTP_PT_L16) + " L16/" +
               String(format.sampleRate) + "/" + String(format.channels) + "\r\n";
        sdp += "a=control:trackID=1\r\n";
    }
    return sdp;
}

static bool parseInterleavedChannels(const String &transport, uint8_t &rtpChannel)
{
    int p = transport.indexOf("interleaved=");
    if (p < 0)
        return false;
    p += 12;
    int dash = transport.indexOf('-', p);
    String first = dash >= 0 ? transport.substring(p, dash) : transport.substring(p);
    int channel = first.toInt();
    if (channel < 0 || channel > 254)
        return false;
    rtpChannel = (uint8_t)channel;
    return true;
}

static void handleRtspRequest(uint8_t slotIndex, const String &request)
{
    if (slotIndex >= RTSP_CLIENT_SLOTS)
        return;
    RtspClientSlot &slot = rtspClients[slotIndex];

    int lineEnd = request.indexOf("\r\n");
    String first = lineEnd >= 0 ? request.substring(0, lineEnd) : request;
    int sp1 = first.indexOf(' ');
    int sp2 = sp1 >= 0 ? first.indexOf(' ', sp1 + 1) : -1;
    if (sp1 <= 0 || sp2 <= sp1) {
        rtspReply(slot, 400, "Bad Request", headerValue(request, "CSeq"));
        return;
    }

    String method = first.substring(0, sp1);
    String uri = first.substring(sp1 + 1, sp2);
    String cseq = headerValue(request, "CSeq");
    slot.lastActivityMs = millis();
    logWrite("RTSP request | client=" + String((unsigned)slotIndex + 1U) +
             " | method=" + method + " | uri=" + uri + " | cseq=" + cseq);

    if (method == "OPTIONS") {
        rtspReply(slot, 200, "OK", cseq,
                  "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n");
        return;
    }

    if (method == "DESCRIBE") {
        String sdp = makeSdp();
        rtspReply(slot, 200, "OK", cseq,
                  "Content-Base: " + rtspBaseUrl() + "/\r\n", sdp);
        return;
    }

    if (method == "SETUP") {
        String transport = headerValue(request, "Transport");
        if (transport.indexOf("RTP/AVP/TCP") < 0) {
            logWrite("RTSP SETUP rejected | client=" + String((unsigned)slotIndex + 1U) +
                     " | transport=" + transport + " | reason=TCP interleaved required");
            rtspReply(slot, 461, "Unsupported Transport", cseq);
            return;
        }

        uint8_t channel = 0;
        if (!parseInterleavedChannels(transport, channel)) {
            logWrite("RTSP SETUP rejected | client=" + String((unsigned)slotIndex + 1U) +
                     " | transport=" + transport + " | reason=missing interleaved channels");
            rtspReply(slot, 461, "Unsupported Transport", cseq);
            return;
        }

        if (!slot.session)
            slot.session = esp_random() ? esp_random() : 1;

        if (uri.indexOf("trackID=1") >= 0) {
            if (!audioAdvertised) {
                rtspReply(slot, 404, "Not Found", cseq);
                return;
            }
            slot.audioChannel = channel;
            slot.audioSetup = true;
            logWrite("RTSP audio SETUP accepted | client=" + String((unsigned)slotIndex + 1U) +
                     " | channel=" + String(channel));
        } else {
            slot.videoChannel = channel;
            slot.videoSetup = true;
        }

        rtspReply(slot, 200, "OK", cseq,
                  "Transport: RTP/AVP/TCP;unicast;interleaved=" +
                  String(channel) + "-" + String(channel + 1) + "\r\n");
        return;
    }

    if (method == "PLAY") {
        if (!slot.videoSetup) {
            rtspReply(slot, 455, "Method Not Valid in This State", cseq);
            return;
        }

        slot.playing = true;
        slot.playStartedUs = esp_timer_get_time();
        slot.lastRtcpVideoMs = 0;
        slot.lastRtcpAudioMs = 0;
        slot.videoRtpPacketsSent = 0;
        slot.videoRtpOctetsSent = 0;
        slot.audioRtpPacketsSent = 0;
        slot.audioRtpOctetsSent = 0;
        slot.videoRtcpReportsSent = 0;
        slot.audioRtcpReportsSent = 0;
        // RTP-Info must describe the timeline that starts now, not the timestamp
        // of the last idle snapshot (which can be several seconds old).
        slot.videoPlayTimestamp = videoTimestampForNow();
        if (slot.audioSetup) {
            startAudioIfNeeded();
            const uint32_t rnd = esp_random();
            slot.audioTimestampBase = rnd ? rnd : 1;
            slot.audioSampleOrigin = audioSamplesDistributed;
            slot.audioSamplesSent = 0;
        }

        logWrite("RTSP PLAY accepted | client=" + String((unsigned)slotIndex + 1U) +
                 " | video=1 | audio=" + String(slot.audioSetup ? 1 : 0));
        String rtpInfo = "RTP-Info: url=" + rtspBaseUrl() + "/trackID=0;seq=" +
                         String(slot.videoSequence) + ";rtptime=" + String(slot.videoPlayTimestamp);
        if (slot.audioSetup) {
            rtpInfo += ",url=" + rtspBaseUrl() + "/trackID=1;seq=" +
                       String(slot.audioSequence) + ";rtptime=" +
                       String(slot.audioTimestampBase);
        }
        rtpInfo += "\r\n";
        rtspReply(slot, 200, "OK", cseq, rtpInfo);
        return;
    }

    if (method == "GET_PARAMETER") {
        rtspReply(slot, 200, "OK", cseq);
        return;
    }

    if (method == "TEARDOWN") {
        rtspReply(slot, 200, "OK", cseq);
        closeRtspClientSlot(slotIndex);
        return;
    }

    rtspReply(slot, 405, "Method Not Allowed", cseq);
}

static void serviceRtspSlot(uint8_t slotIndex)
{
    if (slotIndex >= RTSP_CLIENT_SLOTS)
        return;
    RtspClientSlot &slot = rtspClients[slotIndex];
    if (!rtspSlotConnected(slot)) {
        closeRtspClientSlot(slotIndex);
        return;
    }

    while (slot.client.available() > 0 && slot.rx.length() < 4096) {
        // Ignore interleaved RTCP/receiver-report frames sent by the client.
        if (slot.rx.length() == 0 && slot.client.peek() == '$') {
            if (slot.client.available() < 4)
                break;
            uint8_t header[4];
            if (slot.client.read(header, sizeof(header)) != sizeof(header))
                break;
            size_t body = ((size_t)header[2] << 8) | header[3];
            uint32_t startedWait = millis();
            while (body && slot.client.connected()) {
                int available = slot.client.available();
                if (available <= 0) {
                    if ((uint32_t)(millis() - startedWait) > 50UL)
                        break;
                    delay(0);
                    continue;
                }
                uint8_t sink[64];
                size_t chunk = std::min((size_t)available, std::min(body, sizeof(sink)));
                size_t got = slot.client.read(sink, chunk);
                if (!got)
                    break;
                body -= got;
            }
            slot.lastActivityMs = millis();
            continue;
        }

        char c = (char)slot.client.read();
        slot.rx += c;
        slot.lastActivityMs = millis();
        if (slot.rx.endsWith("\r\n\r\n")) {
            String request = slot.rx;
            slot.rx = "";
            handleRtspRequest(slotIndex, request);
            if (!rtspSlotConnected(slot))
                break;
        }
    }

    if (slot.rx.length() >= 4096) {
        logWrite("RTSP client closed | client=" + String((unsigned)slotIndex + 1U) +
                 " | reason=request buffer overflow");
        closeRtspClientSlot(slotIndex);
        return;
    }

    // The 30 s timeout is only a handshake/control timeout. Once PLAY is active,
    // long periods without inbound RTSP commands are normal: VLC/ffplay may simply
    // receive interleaved RTP without sending GET_PARAMETER keepalives. The live
    // TCP socket and bounded writeAll() stall detection are the authoritative
    // liveness checks for an active stream.
    if (!slot.playing &&
        (uint32_t)(millis() - slot.lastActivityMs) > RTSP_IDLE_TIMEOUT_MS) {
        logWrite("RTSP client closed | client=" + String((unsigned)slotIndex + 1U) +
                 " | reason=setup/control timeout");
        closeRtspClientSlot(slotIndex);
    }
}

static void serviceRtspControl()
{
    if (!cfg_streamer_rtsp_enabled)
        return;

    // Accept at most two independent RTSP control/RTP-over-TCP sessions.
    // Both sessions consume the same captured JPEG frame; only transport state
    // (socket/session/sequence/SSRC/interleaved channels) is duplicated.
    WiFiClient incoming = rtspServer.available();
    if (incoming) {
        int slotIndex = findFreeRtspSlot();
        if (slotIndex < 0) {
            writeText(incoming,
                "RTSP/1.0 453 Not Enough Bandwidth\r\n"
                "Server: SensorForge/7.1.0\r\n"
                "X-SensorForge-Limit: 2 RTSP clients\r\n\r\n");
            incoming.stop();
        } else {
            RtspClientSlot &slot = rtspClients[slotIndex];
            slot.client = incoming;
            slot.client.setNoDelay(true);
            slot.lastActivityMs = millis();
            slot.videoSequence = (uint16_t)esp_random();
            slot.audioSequence = (uint16_t)esp_random();
            slot.videoSsrc = esp_random();
            slot.audioSsrc = esp_random();
            logWrite("RTSP client connected | client=" + String((unsigned)slotIndex + 1U));
        }
    }

    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i)
        serviceRtspSlot(i);
}

static void sendHttpFrame(const uint8_t *jpeg, size_t len)
{
    String header = "--sensorforge\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                    String(len) + "\r\n\r\n";

    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        HttpClientSlot &slot = httpClients[i];
        if (!slot.streaming || !slot.client || !slot.client.connected())
            continue;

        if (!writeText(slot.client, header) || !writeAll(slot.client, jpeg, len) ||
            !writeText(slot.client, "\r\n")) {
            closeHttpClientSlot(i);
            continue;
        }
        ++framesHttp;
        httpVideoBytes += len;
    }
}

static void cacheSnapshot(const uint8_t *jpeg, size_t len)
{
    if (!jpeg || !len)
        return;

    if (len > snapshotCapacity) {
        size_t newCapacity = (len + 4095U) & ~((size_t)4095U);
        uint8_t *replacement = (uint8_t *)heap_caps_malloc(
            newCapacity,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (!replacement)
            return;
        if (snapshotBuffer)
            free(snapshotBuffer);
        snapshotBuffer = replacement;
        snapshotCapacity = newCapacity;
    }

    memcpy(snapshotBuffer, jpeg, len);
    snapshotBytes = len;
}


static bool anyActiveStreamClient()
{
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (rtspSlotConnected(slot) && slot.playing && slot.videoSetup)
            return true;
    }
    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        if (httpClients[i].streaming && httpClients[i].client && httpClients[i].client.connected())
            return true;
    }
    return false;
}

static bool recoverStreamerCamera(const String &reason)
{
    const uint32_t now = millis();
    if ((int32_t)(now - nextCameraRecoveryAllowedMs) < 0)
        return false;

    nextCameraRecoveryAllowedMs = now + CAMERA_RECOVERY_RETRY_MS;
    lastRecoveryReason = reason;

    logWrite(
        "STREAMER recovery | camera reinit requested | reason=" + reason +
        " | capture_failures=" + String(consecutiveCaptureFailures)
    );

    // Stale RTP/HTTP sessions are not kept across a camera-driver reset.
    // Standard clients/NVRs can reconnect after the local recovery.
    closeAllRtspClients();
    closeAllHttpClients();

    if (audioCaptureIsRunning())
        audioCaptureStop();
    audioActive = false;
    audioStartedMs = 0;

    if (cameraInitialized) {
        esp_camera_deinit();
        cameraInitialized = false;
    }

    delay(100);

    const bool ok = initCamera(cfg_camera, cfg_resolution, cfg_quality);
    if (ok) {
        ++cameraRecoveryCount;
        ++persistedCameraRecoveries;
        healthPrefsDirty = true;
        cameraRecoveryFailureCount = 0;
        consecutiveCaptureFailures = 0;
        lastFrameDueMs = 0;
        snapshotBytes = 0;
        snapshotLastMs = 0;
        lastSuccessfulFrameMs = millis();
        lastError = "";
        logWrite(
            "STREAMER recovery successful | camera_recoveries=" +
            String(cameraRecoveryCount)
        );
        return true;
    }

    ++cameraRecoveryFailureCount;
    lastError = "camera recovery failed";
    logWrite(
        "STREAMER recovery failed | attempt=" +
        String(cameraRecoveryFailureCount) + "/" +
        String(CAMERA_RECOVERY_FAILURES_BEFORE_REBOOT)
    );

    if (cameraRecoveryFailureCount >= CAMERA_RECOVERY_FAILURES_BEFORE_REBOOT) {
        logWrite(
            "STREAMER recovery escalation | reboot after repeated camera recovery failures"
        );
        persistControlledResetReason("streamer camera recovery failed repeatedly");
        persistHealthCounters(true);
        delay(100);
        ESP.restart();
    }

    return false;
}

static bool restartStreamerAudioCapture()
{
    const uint32_t now = millis();
    if ((int32_t)(now - nextAudioRecoveryAllowedMs) < 0)
        return false;
    nextAudioRecoveryAllowedMs = now + AUDIO_RECOVERY_RETRY_MS;

    if (!anyRtspAudioConsumer())
        return true;

    AudioFormat format;
    String error;
    if (!configuredAudioFormat(format, error)) {
        lastError = "RTSP audio recovery unavailable: " + error;
        ++audioRecoveryFailureCount;
        return false;
    }

    // Preserve RTP sequence/timestamp continuity as far as possible: only the
    // capture backend/ring is restarted. Video and RTSP sessions stay alive.
    audioCaptureStop();
    audioActive = false;
    audioStartedMs = 0;
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (rtspSlotConnected(slot) && slot.playing && slot.audioSetup) {
            slot.audioTimestampBase += (uint32_t)slot.audioSamplesSent;
            slot.audioSampleOrigin = audioSamplesDistributed;
            slot.audioSamplesSent = 0;
        }
    }

    if (!audioCaptureStart(format, error)) {
        lastError = "RTSP audio recovery failed: " + error;
        ++audioRecoveryFailureCount;
        logWrite("STREAMER audio recovery failed | " + error);
        return false;
    }

    audioActive = true;
    audioStartedMs = millis();
    lastAudioProgressMs = now;
    lastAudioCapturedBytes = audioCaptureStats().bytesCaptured;
    ++audioRecoveryCount;
    audioRecoveryFailureCount = 0;
    ++persistedAudioRecoveries;
    healthPrefsDirty = true;
    lastRecoveryReason = "audio capture restarted after stalled input";
    logWrite("STREAMER audio recovery successful | count=" + String(audioRecoveryCount));
    return true;
}

static void serviceAudioHealth()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - lastAudioHealthMs) < AUDIO_HEALTH_INTERVAL_MS)
        return;
    lastAudioHealthMs = now;

    if (!anyRtspAudioConsumer()) {
        lastAudioProgressMs = now;
        lastAudioCapturedBytes = 0;
        return;
    }

    if (!audioActive) {
        restartStreamerAudioCapture();
        return;
    }

    const AudioCaptureStats stats = audioCaptureStats();
    if (stats.bytesCaptured != lastAudioCapturedBytes) {
        lastAudioCapturedBytes = stats.bytesCaptured;
        lastAudioProgressMs = now;
        return;
    }

    if (lastAudioProgressMs == 0)
        lastAudioProgressMs = now;
    if ((uint32_t)(now - lastAudioProgressMs) >= AUDIO_PROGRESS_STALL_MS)
        restartStreamerAudioCapture();
}

static void serviceNetworkHealth()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - lastNetworkHealthMs) < NETWORK_HEALTH_INTERVAL_MS)
        return;
    lastNetworkHealthMs = now;

    const wifi_mode_t mode = WiFi.getMode();
    const bool expectAp =
        cfg_hotspot_enabled != 0 ||
        mode == WIFI_AP ||
        mode == WIFI_AP_STA;
    bool healthy = false;
    String unhealthyReason;

    if (expectAp) {
        const bool apMode = (mode == WIFI_AP || mode == WIFI_AP_STA);
        const IPAddress apIp = WiFi.softAPIP();
        const bool apIpValid = apIp[0] != 0 || apIp[1] != 0 || apIp[2] != 0 || apIp[3] != 0;
        healthy = apMode && apIpValid;
        if (!healthy)
            unhealthyReason = !apMode ? "WiFi AP mode lost" : "WiFi AP address unavailable";
    } else {
        const bool staMode = (mode == WIFI_STA || mode == WIFI_AP_STA);
        const IPAddress staIp = WiFi.localIP();
        const bool staIpValid = staIp[0] != 0 || staIp[1] != 0 || staIp[2] != 0 || staIp[3] != 0;
        const bool staConnected = WiFi.status() == WL_CONNECTED;
        healthy = staMode && staConnected && staIpValid;
        if (!healthy) {
            if (!staMode)
                unhealthyReason = "WiFi STA mode lost";
            else if (!staConnected)
                unhealthyReason = "Infrastructure WiFi disconnected";
            else
                unhealthyReason = "Infrastructure WiFi address unavailable";
        }
    }

    if (healthy) {
        networkBadChecks = 0;
        return;
    }

    if (networkBadChecks < 255)
        ++networkBadChecks;
    if (networkBadChecks >= NETWORK_BAD_CHECKS_BEFORE_RECOVERY && !networkRecoveryRequested) {
        networkRecoveryRequested = true;
        networkRecoveryReason = unhealthyReason;
        lastRecoveryReason = networkRecoveryReason;
        logWrite("STREAMER network recovery requested | reason=" + networkRecoveryReason);
    }
}

static void serviceMemoryHealth()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - lastMemoryHealthMs) < MEMORY_HEALTH_INTERVAL_MS)
        return;
    lastMemoryHealthMs = now;

    currentInternalHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    currentPsramFree = ESP.getFreePsram();

    heapHistory[heapHistoryPos] = currentInternalHeap;
    heapHistoryPos = (uint8_t)((heapHistoryPos + 1U) % 6U);
    if (heapHistoryCount < 6)
        ++heapHistoryCount;

    memoryTrendWarning = false;
    if (heapHistoryCount == 6 && bootInternalHeap > 0) {
        uint32_t oldest = heapHistory[heapHistoryPos];
        uint32_t newest = currentInternalHeap;
        uint8_t fallingSteps = 0;
        uint32_t prev = oldest;
        for (uint8_t n = 1; n < 6; ++n) {
            uint8_t idx = (uint8_t)((heapHistoryPos + n) % 6U);
            uint32_t v = heapHistory[idx];
            if (v < prev)
                ++fallingSteps;
            prev = v;
        }
        memoryTrendWarning = fallingSteps >= 4 && newest < oldest &&
                             newest < (bootInternalHeap * 60U) / 100U;
    }

    const uint32_t criticalThreshold = std::min<uint32_t>(24000U, bootInternalHeap / 4U);
    if (currentInternalHeap > 0 && currentInternalHeap < criticalThreshold) {
        if (memoryCriticalChecks < 255)
            ++memoryCriticalChecks;
    } else {
        memoryCriticalChecks = 0;
    }

    if (memoryTrendWarning) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            logWrite("STREAMER memory trend warning | heap=" + String(currentInternalHeap) +
                     " | boot_heap=" + String(bootInternalHeap));
        }
    }

    if (memoryCriticalChecks >= MEMORY_CRITICAL_CHECKS_BEFORE_REBOOT) {
        logWrite("STREAMER memory critical | controlled reboot | heap=" + String(currentInternalHeap));
        persistControlledResetReason("streamer critical internal heap");
        persistHealthCounters(true);
        delay(100);
        ESP.restart();
    }
}

static uint32_t configuredStreamerFps()
{
    return cfg_fps > 0 ? (uint32_t)cfg_fps : 1U;
}

static uint32_t thermalThrottlePercent()
{
    if (thermalThrottleLevel >= 2)
        return 50U;
    if (thermalThrottleLevel == 1)
        return 75U;
    return 100U;
}

static uint32_t effectiveStreamerFps()
{
    const uint32_t configured = configuredStreamerFps();
    const uint32_t percent = thermalThrottlePercent();
    // Round to the nearest whole fps while never dropping below 1 fps.
    const uint32_t scaled = (configured * percent + 50U) / 100U;
    return std::max<uint32_t>(1U, scaled);
}

static void logThermalThrottleTransition(uint8_t oldLevel, uint8_t newLevel, float tempC)
{
    const uint32_t configured = configuredStreamerFps();
    const uint32_t effective = effectiveStreamerFps();
    const uint32_t factor = thermalThrottlePercent();

    String msg;
    if (newLevel > oldLevel) {
        msg = "WARNING | THERMAL | STREAMER_THROTTLE";
    } else if (newLevel == 0 && oldLevel != 0) {
        msg = "THERMAL | STREAMER_THROTTLE_RECOVERED";
    } else {
        msg = "THERMAL | STREAMER_THROTTLE_RELAX";
    }

    msg += " | CPU=" + String(tempC, 1) + " C" +
           " | level=" + String((unsigned)newLevel) +
           " | factor=" + String(factor) + "%" +
           " | configured_fps=" + String(configured) +
           " | effective_fps=" + String(effective) +
           " | rtsp_clients=" + String((unsigned)streamerRtspClientCount()) +
           " | http_clients=" + String((unsigned)streamerHttpClientCount());

    if (newLevel == 0 && oldLevel != 0 && thermalThrottleEpisodeStartedMs != 0) {
        const uint32_t durationSec = (millis() - thermalThrottleEpisodeStartedMs) / 1000UL;
        msg += " | episode_s=" + String(durationSec);
        if (isfinite(thermalThrottlePeakC))
            msg += " | peak=" + String(thermalThrottlePeakC, 1) + " C";
    }
    logWrite(msg);
}

static void serviceThermalThrottle()
{
    const uint32_t now = millis();
    if ((uint32_t)(now - lastThermalThrottleCheckMs) < STREAM_THERMAL_CHECK_INTERVAL_MS)
        return;
    lastThermalThrottleCheckMs = now;

    const float tempC = thermalCpuTemperatureC();
    if (!isfinite(tempC))
        return;
    lastStreamerCpuTempC = tempC;

    if (thermalThrottleLevel != 0) {
        if (!isfinite(thermalThrottlePeakC) || tempC > thermalThrottlePeakC)
            thermalThrottlePeakC = tempC;
    }

    uint8_t newLevel = thermalThrottleLevel;
    if (thermalThrottleLevel == 0) {
        if (tempC >= STREAM_THERMAL_STAGE2_C)
            newLevel = 2;
        else if (tempC >= STREAM_THERMAL_STAGE1_C)
            newLevel = 1;
    } else if (thermalThrottleLevel == 1) {
        if (tempC >= STREAM_THERMAL_STAGE2_C)
            newLevel = 2;
        else if (tempC <= STREAM_THERMAL_STAGE1_RELEASE_C)
            newLevel = 0;
    } else {
        if (tempC <= STREAM_THERMAL_STAGE1_RELEASE_C)
            newLevel = 0;
        else if (tempC <= STREAM_THERMAL_STAGE2_RELEASE_C)
            newLevel = 1;
    }

    if (newLevel == thermalThrottleLevel)
        return;

    const uint8_t oldLevel = thermalThrottleLevel;
    if (oldLevel == 0 && newLevel != 0) {
        thermalThrottleEpisodeStartedMs = now;
        thermalThrottlePeakC = tempC;
    }
    thermalThrottleLevel = newLevel;
    logThermalThrottleTransition(oldLevel, newLevel, tempC);
    if (newLevel == 0) {
        thermalThrottleEpisodeStartedMs = 0;
        thermalThrottlePeakC = NAN;
    }
}

static const char *stressModeName(StreamerStressMode mode)
{
    switch (mode) {
        case StreamerStressMode::CameraOnly: return "camera";
        case StreamerStressMode::HttpMeasure: return "http";
        case StreamerStressMode::Observe: return "observe";
        default: return "none";
    }
}

static void finishStreamerStress(const char *reason)
{
    if (stressState.mode == StreamerStressMode::None)
        return;

    const uint32_t now = millis();
    const uint32_t elapsedMs = std::max<uint32_t>(1U, now - stressState.startedMs);
    const uint32_t frameDelta = framesCaptured - stressState.framesStart;
    const uint64_t jpegDelta = jpegBytesCaptured - stressState.jpegBytesStart;
    const uint64_t rtspPacketsDelta = rtspVideoPackets - stressState.rtspPacketsStart;
    const uint64_t rtspBytesDelta = rtspVideoWireBytes - stressState.rtspBytesStart;
    const uint64_t httpBytesDelta = httpVideoBytes - stressState.httpBytesStart;
    const uint32_t audioPacketsDelta = audioPacketsSent - stressState.audioPacketsStart;
    const uint64_t audioBytesDelta = audioBytesSent - stressState.audioBytesStart;
    const uint64_t totalBytesDelta = bytesSent - stressState.totalBytesStart;
    const float measuredFps = (float)frameDelta * 1000.0f / (float)elapsedMs;
    const uint32_t avgJpeg = frameDelta ? (uint32_t)(jpegDelta / frameDelta) : 0U;
    const uint32_t videoKbit = (uint32_t)(((rtspBytesDelta + httpBytesDelta) * 8ULL) / elapsedMs);

    String msg = "STREAMER STRESS END";
    msg += " | mode=" + String(stressModeName(stressState.mode));
    msg += " | reason=" + String(reason ? reason : "finished");
    msg += " | duration_ms=" + String(elapsedMs);
    msg += " | frames=" + String(frameDelta);
    msg += " | measured_fps=" + String(measuredFps, 2);
    msg += " | avg_jpeg=" + String(avgJpeg);
    msg += " | max_jpeg=" + String(stressState.maxJpegBytes);
    msg += " | video_kbit_s=" + String(videoKbit);
    msg += " | rtsp_packets=" + String((unsigned long long)rtspPacketsDelta);
    msg += " | rtsp_bytes=" + String((unsigned long long)rtspBytesDelta);
    msg += " | http_bytes=" + String((unsigned long long)httpBytesDelta);
    msg += " | audio_packets=" + String(audioPacketsDelta);
    msg += " | audio_bytes=" + String((unsigned long long)audioBytesDelta);
    msg += " | total_wire_bytes=" + String((unsigned long long)totalBytesDelta);
    msg += " | max_rtsp_frame_send_ms=" + String((float)stressState.maxRtspFrameSendUs / 1000.0f, 2);
    if (isfinite(stressState.startTempC))
        msg += " | start_temp=" + String(stressState.startTempC, 1) + " C";
    if (isfinite(stressState.peakTempC))
        msg += " | peak_temp=" + String(stressState.peakTempC, 1) + " C";
    msg += " | configured_fps=" + String(configuredStreamerFps());
    msg += " | effective_fps=" + String(effectiveStreamerFps());
    msg += " | rtsp_clients=" + String((unsigned)streamerRtspClientCount());
    msg += " | http_clients=" + String((unsigned)streamerHttpClientCount());
    msg += " | audio_active=" + String(audioActive ? 1 : 0);
    logWrite(msg);

    stressState = StreamerStressState();
}

static bool startStreamerStress(StreamerStressMode mode, uint32_t durationMs, String &error)
{
    error = "";
    if (!started || !streamerModeEnabled()) {
        error = "streamer is not active";
        return false;
    }
    if (stressState.mode != StreamerStressMode::None) {
        error = "another streamer stress measurement is already active";
        return false;
    }
    if (mode == StreamerStressMode::None) {
        error = "invalid stress mode";
        return false;
    }
    if (mode == StreamerStressMode::CameraOnly && anyActiveStreamClient()) {
        error = "camera-only test requires zero RTSP/HTTP stream clients";
        return false;
    }
    if (mode == StreamerStressMode::HttpMeasure) {
        if (!cfg_streamer_http_mjpeg_enabled) {
            error = "HTTP-MJPEG is disabled";
            return false;
        }
        if (anyActiveStreamClient()) {
            error = "HTTP test requires zero existing RTSP/HTTP stream clients";
            return false;
        }
    }

    durationMs = std::max<uint32_t>(STREAM_STRESS_MIN_MS,
                 std::min<uint32_t>(STREAM_STRESS_MAX_MS, durationMs));
    stressState.mode = mode;
    stressState.startedMs = millis();
    stressState.durationMs = durationMs;
    stressState.framesStart = framesCaptured;
    stressState.jpegBytesStart = jpegBytesCaptured;
    stressState.rtspPacketsStart = rtspVideoPackets;
    stressState.rtspBytesStart = rtspVideoWireBytes;
    stressState.httpBytesStart = httpVideoBytes;
    stressState.audioPacketsStart = audioPacketsSent;
    stressState.audioBytesStart = audioBytesSent;
    stressState.totalBytesStart = bytesSent;
    stressState.startTempC = thermalCpuTemperatureC();
    stressState.peakTempC = stressState.startTempC;

    logWrite("STREAMER STRESS START | mode=" + String(stressModeName(mode)) +
             " | duration_ms=" + String(durationMs) +
             " | configured_fps=" + String(configuredStreamerFps()) +
             " | effective_fps=" + String(effectiveStreamerFps()) +
             " | rtsp_clients=" + String((unsigned)streamerRtspClientCount()) +
             " | http_clients=" + String((unsigned)streamerHttpClientCount()) +
             " | audio_active=" + String(audioActive ? 1 : 0) +
             (isfinite(stressState.startTempC) ? " | start_temp=" + String(stressState.startTempC, 1) + " C" : ""));
    return true;
}

static void serviceStreamerStress()
{
    if (stressState.mode == StreamerStressMode::None)
        return;

    const float tempC = thermalCpuTemperatureC();
    if (isfinite(tempC) && (!isfinite(stressState.peakTempC) || tempC > stressState.peakTempC))
        stressState.peakTempC = tempC;

    if ((uint32_t)(millis() - stressState.startedMs) >= stressState.durationMs)
        finishStreamerStress("timer");
}

static void handleStreamerStressStart()
{
    if (!webServer)
        return;

    String modeArg = webServer->arg("mode");
    modeArg.toLowerCase();
    StreamerStressMode mode = StreamerStressMode::Observe;
    if (modeArg == "camera") mode = StreamerStressMode::CameraOnly;
    else if (modeArg == "http") mode = StreamerStressMode::HttpMeasure;
    else if (modeArg == "observe" || modeArg == "rtsp") mode = StreamerStressMode::Observe;
    else {
        webServer->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid mode\"}");
        return;
    }

    uint32_t durationMs = STREAM_STRESS_DEFAULT_MS;
    if (webServer->hasArg("seconds")) {
        const long seconds = webServer->arg("seconds").toInt();
        if (seconds > 0)
            durationMs = (uint32_t)seconds * 1000UL;
    }

    String error;
    if (!startStreamerStress(mode, durationMs, error)) {
        webServer->send(409, "application/json", "{\"ok\":false,\"error\":\"" + error + "\"}");
        return;
    }
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "application/json", "{\"ok\":true}");
}

static void handleStreamerStressStop()
{
    if (!webServer)
        return;
    if (stressState.mode != StreamerStressMode::None)
        finishStreamerStress("manual");
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "application/json", "{\"ok\":true}");
}

static void serviceStreamerHealth()
{
    if (!started)
        return;

    serviceAudioHealth();
    serviceNetworkHealth();
    serviceMemoryHealth();
    serviceThermalThrottle();
    persistHealthCounters(false);

    if (consecutiveCaptureFailures >= CAMERA_CAPTURE_FAILURES_BEFORE_RECOVERY) {
        recoverStreamerCamera("repeated camera capture failure");
        return;
    }

    if (anyActiveStreamClient() && lastSuccessfulFrameMs != 0) {
        const uint32_t age = millis() - lastSuccessfulFrameMs;
        if (age >= STREAM_FRAME_STALL_MS)
            recoverStreamerCamera("no successful frame for " + String(age) + " ms");
    }
}

static void captureAndDistributeFrame()
{
    bool needRtsp = false;
    if (cfg_streamer_rtsp_enabled) {
        for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
            RtspClientSlot &slot = rtspClients[i];
            if (rtspSlotConnected(slot) && slot.playing && slot.videoSetup) {
                needRtsp = true;
                break;
            }
        }
    }
    bool needHttp = false;
    if (cfg_streamer_http_mjpeg_enabled) {
        for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
            if (httpClients[i].streaming && httpClients[i].client && httpClients[i].client.connected()) {
                needHttp = true;
                break;
            }
        }
    }
    // With no stream clients, keep only a very low-rate snapshot refresh. The
    // camera stays initialized, so a new client can resume immediately without
    // a deinit/init cycle.
    const bool forceCameraStress = stressState.mode == StreamerStressMode::CameraOnly;
    const bool previewDemand =
        previewDemandLastMs != 0 &&
        (uint32_t)(millis() - previewDemandLastMs) <= PREVIEW_DEMAND_TIMEOUT_MS;
    const bool activeDemand = needRtsp || needHttp || forceCameraStress || previewDemand;
    if (activeDemand != streamDemandActive) {
        streamDemandActive = activeDemand;
        if (streamDemandActive) {
            logWrite("STREAMER capture profile | active | configured_fps=" +
                     String(configuredStreamerFps()) +
                     " | effective_fps=" + String(effectiveStreamerFps()));
        } else {
            logWrite("STREAMER capture profile | idle | snapshot_interval_ms=" +
                     String(STREAM_IDLE_CAPTURE_INTERVAL_MS));
        }
    }

    bool refreshSnapshot = !snapshotBytes ||
        (uint32_t)(millis() - snapshotLastMs) >= STREAM_IDLE_CAPTURE_INTERVAL_MS;
    if (!activeDemand && !refreshSnapshot)
        return;

    const uint32_t fps = effectiveStreamerFps();
    uint32_t intervalMs = activeDemand
        ? std::max<uint32_t>(1U, (uint32_t)(1000UL / fps))
        : STREAM_IDLE_CAPTURE_INTERVAL_MS;
    uint32_t now = millis();
    if (lastFrameDueMs && (int32_t)(now - lastFrameDueMs) < 0)
        return;
    lastFrameDueMs = now + intervalMs;

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        ++consecutiveCaptureFailures;
        lastError = "camera frame unavailable";
        return;
    }

    consecutiveCaptureFailures = 0;
    lastSuccessfulFrameMs = millis();
    ++framesCaptured;

    if (frame->format != PIXFORMAT_JPEG) {
        lastError = "camera frame is not JPEG";
    } else {
        ++jpegFramesCaptured;
        jpegBytesCaptured += frame->len;
        if (frame->len > jpegMaxBytes)
            jpegMaxBytes = (uint32_t)frame->len;
        if (stressState.mode != StreamerStressMode::None && frame->len > stressState.maxJpegBytes)
            stressState.maxJpegBytes = (uint32_t)frame->len;
        // Stamp every captured JPEG from the real monotonic 90-kHz RTP clock.
        // All RTSP clients sharing this camera frame receive the same timestamp.
        videoTimestamp = videoTimestampForNow();
        cacheSnapshot(frame->buf, frame->len);
        snapshotLastMs = millis();
        if (needRtsp) {
            for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
                RtspClientSlot &slot = rtspClients[i];
                if (!rtspSlotConnected(slot) || !slot.playing || !slot.videoSetup)
                    continue;
                // Send every captured frame. Adaptive RTSP frame thinning is
                // intentionally disabled while multi-client A/V timing is being
                // qualified; dropping future frames did not address receiver-side
                // latency reliably and could obscure timestamp faults.
                if (!sendRtpJpeg(slot, frame->buf, frame->len))
                    closeRtspClientSlot(i);
            }
        }
        if (needHttp)
            sendHttpFrame(frame->buf, frame->len);
    }

    esp_camera_fb_return(frame);
}

static bool httpRequestIsStream(const String &request)
{
    return request.startsWith("GET /stream ") || request.startsWith("GET /stream?");
}

static bool beginHttpStream(WiFiClient &client)
{
    return writeText(client,
        "HTTP/1.1 200 OK\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=sensorforge\r\n\r\n");
}

static void serviceHttpControl()
{
    if (!cfg_streamer_http_mjpeg_enabled)
        return;

    // Accept up to two HTTP-MJPEG viewers. Both consume the same captured JPEG
    // frame; there is no second camera capture or JPEG encode. A third viewer
    // receives an explicit 503 response instead of silently displacing/frozen
    // an existing stream.
    WiFiClient incoming = httpServer.available();
    if (incoming) {
        int slotIndex = findFreeHttpSlot();
        if (slotIndex < 0) {
            writeText(incoming,
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                "SensorForge HTTP-MJPEG: maximum of two viewers reached.\n");
            incoming.stop();
        } else {
            HttpClientSlot &slot = httpClients[slotIndex];
            slot.client = incoming;
            slot.client.setNoDelay(true);
            slot.lastActivityMs = millis();
        }
    }

    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        HttpClientSlot &slot = httpClients[i];
        if (!slot.client || !slot.client.connected()) {
            closeHttpClientSlot(i);
            continue;
        }

        if (slot.streaming)
            continue;

        while (slot.client.available() > 0 && slot.rx.length() < 2048) {
            slot.rx += (char)slot.client.read();
            slot.lastActivityMs = millis();
            if (slot.rx.endsWith("\r\n\r\n")) {
                bool valid = httpRequestIsStream(slot.rx);
                slot.rx = "";
                if (!valid) {
                    writeText(slot.client,
                        "HTTP/1.1 404 Not Found\r\nConnection: close\r\n"
                        "Content-Length: 0\r\n\r\n");
                    closeHttpClientSlot(i);
                    break;
                }

                if (!beginHttpStream(slot.client)) {
                    closeHttpClientSlot(i);
                    break;
                }
                slot.streaming = true;
                break;
            }
        }

        if (slot.rx.length() >= 2048 ||
            (!slot.streaming && (uint32_t)(millis() - slot.lastActivityMs) > 5000UL)) {
            closeHttpClientSlot(i);
        }
    }
}

static String viewerHtmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '&') out += F("&amp;");
        else if (c == '<') out += F("&lt;");
        else if (c == '>') out += F("&gt;");
        else if (c == '"') out += F("&quot;");
        else if (c == '\'') out += F("&#39;");
        else out += c;
    }
    return out;
}

static String viewerInitialLocalMs()
{
    time_t now = time(nullptr);
    if (now < 1600000000)
        return "0";

    struct tm localTm;
    localtime_r(&now, &localTm);

    // Build a browser-side UTC timestamp from the device's already-localized
    // civil time. JS then formats with getUTC*(), so the viewer follows the
    // SensorForge timezone instead of the browser PC timezone.
    String js = "Date.UTC(";
    js += String(localTm.tm_year + 1900);
    js += ',';
    js += String(localTm.tm_mon);
    js += ',';
    js += String(localTm.tm_mday);
    js += ',';
    js += String(localTm.tm_hour);
    js += ',';
    js += String(localTm.tm_min);
    js += ',';
    js += String(localTm.tm_sec);
    js += ")";
    return js;
}

static void handleHttpViewer()
{
    if (!streamerModeEnabled() || !cfg_streamer_http_mjpeg_enabled) {
        webServer->send(404, "text/plain; charset=utf-8", "HTTP-MJPEG viewer is disabled");
        return;
    }

    const String title = cfg_camera_display_name.length()
        ? cfg_camera_display_name
        : String("SensorForge");
    const String overlay = cfg_camera_overlay_text.length()
        ? cfg_camera_overlay_text
        : title;
    const String streamUrl = streamerHttpUrl();
    const String initialLocalMs = viewerInitialLocalMs();

    String html;
    html.reserve(5200);
    html += F("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
    html += "<title>" + viewerHtmlEscape(title) + "</title>";
    html += F("<style>body{margin:0;background:#101214;color:#eee;font-family:Arial,sans-serif}.wrap{max-width:1200px;margin:auto;padding:14px}.stage{position:relative;background:#000;border-radius:8px;overflow:hidden}.stage img{display:block;width:100%;height:auto}.ov{position:absolute;left:14px;bottom:14px;background:rgba(0,0,0,.58);color:#fff;padding:8px 11px;border-radius:6px;text-shadow:0 1px 2px #000;max-width:85%}.ov .name{font-size:1.08rem;font-weight:700}.ov .time{margin-top:3px;font-family:monospace;font-size:.95rem}.meta{margin-top:12px;background:#1b1f22;border-radius:8px;padding:12px 14px;line-height:1.55}.meta b{display:inline-block;min-width:145px}.muted{color:#b9c0c5}@media(max-width:600px){.wrap{padding:0}.stage{border-radius:0}.meta{margin:8px;border-radius:7px}.ov{left:8px;bottom:8px}}</style></head><body><div class='wrap'><div class='stage'>");
    html += "<img src='" + viewerHtmlEscape(streamUrl) + "' alt='HTTP-MJPEG stream'>";
    html += "<div class='ov'><div class='name'>" + viewerHtmlEscape(overlay) + "</div><div id='sfClock' class='time'>--</div></div></div>";
    html += F("<div class='meta'>");
    if (cfg_camera_display_name.length()) html += "<div><b>Kamera:</b> " + viewerHtmlEscape(cfg_camera_display_name) + "</div>";
    if (cfg_camera_location.length()) html += "<div><b>Ort:</b> " + viewerHtmlEscape(cfg_camera_location) + "</div>";
    if (cfg_camera_gps_lat.length() && cfg_camera_gps_lon.length()) html += "<div><b>GPS:</b> " + viewerHtmlEscape(cfg_camera_gps_lat) + ", " + viewerHtmlEscape(cfg_camera_gps_lon) + "</div>";
    if (cfg_camera_responsible.length()) html += "<div><b>Verantwortlich:</b> " + viewerHtmlEscape(cfg_camera_responsible) + "</div>";
    if (cfg_camera_email.length()) html += "<div><b>E-Mail:</b> " + viewerHtmlEscape(cfg_camera_email) + "</div>";
    if (cfg_camera_description.length()) html += "<div><b>Beschreibung:</b> " + viewerHtmlEscape(cfg_camera_description) + "</div>";
    html += F("</div>");
    html += "<script>(function(){var base=" + initialLocalMs + ",t0=Date.now(),el=document.getElementById('sfClock');function p(n){return n<10?'0'+n:n;}function tick(){if(!base){el.textContent='Zeit nicht verfügbar';return;}var d=new Date(base+(Date.now()-t0));el.textContent=p(d.getUTCDate())+'.'+p(d.getUTCMonth()+1)+'.'+d.getUTCFullYear()+' '+p(d.getUTCHours())+':'+p(d.getUTCMinutes())+':'+p(d.getUTCSeconds());}tick();setInterval(tick,1000);}());</script></div></body></html>";

    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "text/html; charset=utf-8", html);
}

static void handleHttpStreamRedirect()
{
    if (!streamerModeEnabled() || !cfg_streamer_http_mjpeg_enabled) {
        webServer->send(404, "text/plain; charset=utf-8", "HTTP-MJPEG stream is disabled");
        return;
    }
    webServer->sendHeader("Location", streamerHttpUrl());
    webServer->send(307, "text/plain; charset=utf-8", "");
}

static void handleStreamerStatus()
{
    const AudioCaptureStats audioStats = audioCaptureStats();
    const size_t audioBufferedBytes = audioCaptureBufferedBytes();
    String json = String("{\"ready\":") + (started ? "true" : "false") +
        ",\"mode\":\"" + (streamerModeEnabled() ? "streamer" : "normal") + "\"" +
        ",\"rtsp_enabled\":" + (cfg_streamer_rtsp_enabled ? "true" : "false") +
        ",\"http_mjpeg_enabled\":" + (cfg_streamer_http_mjpeg_enabled ? "true" : "false") +
        ",\"rtsp_client\":" + (streamerRtspClientConnected() ? "true" : "false") +
        ",\"rtsp_clients\":" + String((unsigned)streamerRtspClientCount()) +
        ",\"http_client\":" + (streamerHttpClientConnected() ? "true" : "false") +
        ",\"http_clients\":" + String((unsigned)streamerHttpClientCount()) +
        ",\"audio_available\":" + (streamerAudioAvailable() ? "true" : "false") +
        ",\"audio_active\":" + (audioActive ? "true" : "false") +
        ",\"audio_packets\":" + String(audioPacketsSent) +
        ",\"audio_bytes\":" + String((unsigned long long)audioBytesSent) +
        ",\"audio_buffered_bytes\":" + String((unsigned long long)audioBufferedBytes) +
        ",\"audio_buffer_capacity\":" + String((unsigned long long)audioStats.bufferCapacity) +
        ",\"audio_buffer_high_water\":" + String((unsigned long long)audioStats.bufferHighWater) +
        ",\"audio_dropped_bytes\":" + String((unsigned long long)audioStats.bytesDropped) +
        ",\"audio_live_discard_bytes\":" + String((unsigned long long)audioLiveDiscardBytes) +
        ",\"audio_drain_packets_last\":" + String(audioDrainPacketsLast) +
        ",\"audio_drain_packets_max\":" + String(audioDrainPacketsMax) +
        ",\"audio_recoveries\":" + String(audioRecoveryCount) +
        ",\"audio_recovery_failures\":" + String(audioRecoveryFailureCount) +
        ",\"socket_stalls\":" + String(socketStallCount) +
        ",\"network_recoveries\":" + String(networkRecoveryCount) +
        ",\"network_recovery_failures\":" + String(networkRecoveryFailureCount) +
        ",\"memory_warning\":" + (memoryTrendWarning ? "true" : "false") +
        ",\"heap_free\":" + String(currentInternalHeap) +
        ",\"psram_free\":" + String(currentPsramFree) +
        ",\"reset_reason\":\"" + String(resetReasonName(esp_reset_reason())) + "\"" +
        ",\"previous_controlled_reset\":\"" + previousControlledResetReason + "\"" +
        ",\"persistent_camera_recoveries\":" + String(persistedCameraRecoveries) +
        ",\"persistent_audio_recoveries\":" + String(persistedAudioRecoveries) +
        ",\"persistent_network_recoveries\":" + String(persistedNetworkRecoveries) +
        ",\"persistent_socket_stalls\":" + String(persistedSocketStalls) +
        ",\"camera_recoveries\":" + String(cameraRecoveryCount) +
        ",\"camera_recovery_failures\":" + String(cameraRecoveryFailureCount) +
        ",\"capture_failures\":" + String(consecutiveCaptureFailures) +
        ",\"last_frame_age_ms\":" + String(lastSuccessfulFrameMs ? (uint32_t)(millis() - lastSuccessfulFrameMs) : 0U) +
        ",\"last_recovery_reason\":\"" + lastRecoveryReason + "\"" +
        ",\"frames\":" + String(framesCaptured) +
        ",\"fps\":" + String(streamerMeasuredFps(), 2) +
        ",\"configured_fps\":" + String(configuredStreamerFps()) +
        ",\"effective_fps\":" + String(effectiveStreamerFps()) +
        ",\"thermal_throttle_level\":" + String((unsigned)thermalThrottleLevel) +
        ",\"streamer_cpu_temp_c\":" + (isfinite(lastStreamerCpuTempC) ? String(lastStreamerCpuTempC, 1) : String("null")) +
        ",\"jpeg_frames\":" + String(jpegFramesCaptured) +
        ",\"jpeg_bytes\":" + String((unsigned long long)jpegBytesCaptured) +
        ",\"jpeg_avg_bytes\":" + String(jpegFramesCaptured ? (uint32_t)(jpegBytesCaptured / jpegFramesCaptured) : 0U) +
        ",\"jpeg_max_bytes\":" + String(jpegMaxBytes) +
        ",\"rtsp_video_packets\":" + String((unsigned long long)rtspVideoPackets) +
        ",\"rtsp_video_bytes\":" + String((unsigned long long)rtspVideoWireBytes) +
        ",\"http_video_bytes\":" + String((unsigned long long)httpVideoBytes) +
        ",\"rtsp_send_avg_ms\":" + String(rtspFrameSendCount ? ((float)rtspFrameSendUsTotal / (float)rtspFrameSendCount / 1000.0f) : 0.0f, 2) +
        ",\"rtsp_send_max_ms\":" + String((float)rtspFrameSendMaxUs / 1000.0f, 2) +
        ",\"rtsp_client1_divider\":" + String((unsigned)rtspClients[0].videoFrameDivider) +
        ",\"rtsp_client1_skipped\":" + String(rtspClients[0].videoFramesSkipped) +
        ",\"rtsp_client1_last_send_ms\":" + String((float)rtspClients[0].videoLastSendUs / 1000.0f, 2) +
        ",\"rtsp_client1_adaptive_changes\":" + String(rtspClients[0].videoAdaptiveChanges) +
        ",\"rtsp_client1_connected\":" + (rtspSlotConnected(rtspClients[0]) && rtspClients[0].playing ? "true" : "false") +
        ",\"rtsp_client1_rtcp_video_sr\":" + String(rtspClients[0].videoRtcpReportsSent) +
        ",\"rtsp_client1_rtcp_audio_sr\":" + String(rtspClients[0].audioRtcpReportsSent) +
        ",\"rtsp_client2_divider\":" + String((unsigned)rtspClients[1].videoFrameDivider) +
        ",\"rtsp_client2_skipped\":" + String(rtspClients[1].videoFramesSkipped) +
        ",\"rtsp_client2_last_send_ms\":" + String((float)rtspClients[1].videoLastSendUs / 1000.0f, 2) +
        ",\"rtsp_client2_adaptive_changes\":" + String(rtspClients[1].videoAdaptiveChanges) +
        ",\"rtsp_client2_connected\":" + (rtspSlotConnected(rtspClients[1]) && rtspClients[1].playing ? "true" : "false") +
        ",\"rtsp_client2_rtcp_video_sr\":" + String(rtspClients[1].videoRtcpReportsSent) +
        ",\"rtsp_client2_rtcp_audio_sr\":" + String(rtspClients[1].audioRtcpReportsSent) +
        ",\"rtp_video_clock\":\"monotonic_90khz\"" +
        ",\"stress_active\":" + (stressState.mode != StreamerStressMode::None ? "true" : "false") +
        ",\"stress_mode\":\"" + String(stressModeName(stressState.mode)) + "\"" +
        ",\"stress_remaining_ms\":" + String(stressState.mode != StreamerStressMode::None ? (stressState.durationMs - std::min<uint32_t>(stressState.durationMs, (uint32_t)(millis() - stressState.startedMs))) : 0U) +
        ",\"bytes_sent\":" + String((unsigned long long)bytesSent) + "}";
    webServer->sendHeader("Cache-Control", "no-store");
    webServer->send(200, "application/json", json);
}

} // namespace

bool streamerModeEnabled()
{
    return cfg_operating_mode == "streamer";
}

bool streamerBegin(String &error)
{
    error = "";
    lastError = "";

    if (!streamerModeEnabled()) {
        error = "streamer mode is not active";
        return false;
    }

    if (started)
        return true;

    if (cfg_streamer_rtsp_enabled)
        rtspServer.begin();
    if (cfg_streamer_http_mjpeg_enabled)
        httpServer.begin();

    openHealthPrefsIfNeeded();
    if (statsStartedMs == 0)
        statsStartedMs = millis();
    lastFrameDueMs = 0;
    lastSuccessfulFrameMs = millis();
    nextCameraRecoveryAllowedMs = 0;
    cameraRecoveryFailureCount = 0;
    consecutiveCaptureFailures = 0;
    lastRecoveryReason = "";
    lastAudioHealthMs = millis();
    lastAudioProgressMs = millis();
    lastAudioCapturedBytes = 0;
    nextAudioRecoveryAllowedMs = 0;
    lastNetworkHealthMs = millis();
    networkBadChecks = 0;
    networkRecoveryRequested = false;
    networkRecoveryReason = "";
    nextNetworkRecoveryAllowedMs = 0;
    lastMemoryHealthMs = millis();
    bootInternalHeap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    bootPsramFree = ESP.getFreePsram();
    currentInternalHeap = bootInternalHeap;
    currentPsramFree = bootPsramFree;
    memoryCriticalChecks = 0;
    memoryTrendWarning = false;
    memset(heapHistory, 0, sizeof(heapHistory));
    heapHistoryCount = 0;
    heapHistoryPos = 0;
    videoTimestampBase = esp_random();
    videoTimestamp = videoTimestampBase;
    videoClockStartUs = esp_timer_get_time();
    jpegBytesCaptured = 0;
    jpegFramesCaptured = 0;
    jpegMaxBytes = 0;
    rtspVideoPackets = 0;
    rtspVideoWireBytes = 0;
    httpVideoBytes = 0;
    rtspFrameSendUsTotal = 0;
    rtspFrameSendCount = 0;
    rtspFrameSendMaxUs = 0;
    stressState = StreamerStressState();
    started = true;

    logWrite(
        "STREAMER started | rtsp=" + String(cfg_streamer_rtsp_enabled) +
        " | http_mjpeg=" + String(cfg_streamer_http_mjpeg_enabled) +
        " | fps=" + String(cfg_fps)
    );
    return true;
}

void streamerLoop()
{
    if (!started || !streamerModeEnabled())
        return;

    serviceRtspControl();
    serviceHttpControl();
    audioDrainPacketsLast = 0;
    serviceAudio();
    captureAndDistributeFrame();
    // A second short drain pass prevents the long serial video fan-out from
    // starving 16-kHz PCM when multiple RTSP/HTTP consumers are active.
    serviceAudio();
    if (audioDrainPacketsLast > audioDrainPacketsMax)
        audioDrainPacketsMax = audioDrainPacketsLast;
    serviceRtcpSenderReports();
    serviceStreamerHealth();
    serviceStreamerStress();

    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        if (httpClients[i].client && !httpClients[i].client.connected())
            closeHttpClientSlot(i);
    }
}

bool streamerTakeNetworkRecoveryRequest(String &reason)
{
    if (!networkRecoveryRequested)
        return false;
    if (nextNetworkRecoveryAllowedMs != 0 &&
        (int32_t)(millis() - nextNetworkRecoveryAllowedMs) < 0)
        return false;
    reason = networkRecoveryReason;
    networkRecoveryRequested = false;
    networkBadChecks = 0;
    return true;
}

void streamerNoteNetworkRecoveryResult(bool success, const String &error)
{
    if (success) {
        ++networkRecoveryCount;
        ++persistedNetworkRecoveries;
        healthPrefsDirty = true;
        networkRecoveryFailureCount = 0;
        nextNetworkRecoveryAllowedMs = 0;
        lastRecoveryReason = "network stack restarted";
        logWrite("STREAMER network recovery successful | count=" + String(networkRecoveryCount));
        return;
    }

    ++networkRecoveryFailureCount;
    lastRecoveryReason = "network recovery failed: " + error;
    logWrite("STREAMER network recovery failed | " + error);
    if (networkRecoveryFailureCount < 2) {
        networkRecoveryRequested = true;
        networkRecoveryReason = "retry after failed network recovery";
        nextNetworkRecoveryAllowedMs = millis() + 5000UL;
    }
    // A failed network/WebConfig restart leaves the device unreachable. Reboot is
    // safer than spinning in a fast retry loop, but only after two confirmed
    // failed full-stack recovery attempts.
    if (networkRecoveryFailureCount >= 2) {
        persistControlledResetReason("streamer network recovery failed repeatedly");
        persistHealthCounters(true);
        delay(100);
        ESP.restart();
    }
}

void streamerStop()
{
    if (stressState.mode != StreamerStressMode::None)
        finishStreamerStress("streamer_stop");
    closeAllRtspClients();
    closeAllHttpClients();
    if (started && cfg_streamer_rtsp_enabled)
        rtspServer.end();
    if (started && cfg_streamer_http_mjpeg_enabled)
        httpServer.end();
    started = false;
    persistHealthCounters(false);
    snapshotBytes = 0;
    previewDemandLastMs = 0;
    if (snapshotBuffer) {
        free(snapshotBuffer);
        snapshotBuffer = nullptr;
        snapshotCapacity = 0;
    }
}

void streamerRegisterWebRoutes(WebServer &server)
{
    webServer = &server;
    server.on("/stream", HTTP_GET, handleHttpStreamRedirect);
    server.on("/stream_view", HTTP_GET, handleHttpViewer);
    server.on("/streamer_status", HTTP_GET, handleStreamerStatus);
    server.on("/streamer_stress_start", HTTP_POST, handleStreamerStressStart);
    server.on("/streamer_stress_stop", HTTP_POST, handleStreamerStressStop);
}

bool streamerSendSnapshot(WebServer &server)
{
    if (!streamerModeEnabled() || !started || !snapshotBuffer || !snapshotBytes) {
        server.send(503, "text/plain; charset=utf-8", "No streamer frame is available yet");
        return false;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(snapshotBytes);
    server.send(200, "image/jpeg", "");
    return writeAll(server.client(), snapshotBuffer, snapshotBytes);
}

void streamerNotePreviewActivity()
{
    if (!streamerModeEnabled() || !started)
        return;
    previewDemandLastMs = millis();
}

void streamerClearPreviewDemand()
{
    previewDemandLastMs = 0;
}

bool streamerRtspClientConnected()
{
    return streamerRtspClientCount() > 0;
}

uint8_t streamerRtspClientCount()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        RtspClientSlot &slot = rtspClients[i];
        if (slot.playing && rtspSlotConnected(slot))
            ++count;
    }
    return count;
}

bool streamerHttpClientConnected()
{
    return streamerHttpClientCount() > 0;
}

uint8_t streamerHttpClientCount()
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        if (httpClients[i].streaming && httpClients[i].client && httpClients[i].client.connected())
            ++count;
    }
    return count;
}

bool streamerAudioActive()
{
    return audioActive;
}

bool streamerAudioAvailable()
{
    AudioFormat format;
    String error;
    return configuredAudioFormat(format, error);
}

String streamerAudioStatus()
{
    if (!cfg_audio_enabled)
        return "off - Audio is disabled in Audio settings";

    AudioFormat format;
    String error;
    if (!configuredAudioFormat(format, error))
        return "enabled, but input/format unavailable: " + error;

    return audioActive
        ? "active - included in RTSP stream"
        : "available - starts when requested by RTSP client";
}

bool streamerReady()
{
    return started;
}

String streamerLastError()
{
    return lastError;
}

uint32_t streamerFramesCaptured() { return framesCaptured; }
uint32_t streamerFramesSentRtsp() { return framesRtsp; }
uint32_t streamerFramesSentHttp() { return framesHttp; }
uint64_t streamerBytesSent() { return bytesSent; }

float streamerMeasuredFps()
{
    uint32_t elapsed = millis() - statsStartedMs;
    if (!started || elapsed == 0)
        return 0.0f;
    return (float)framesCaptured * 1000.0f / (float)elapsed;
}

String streamerRtspUrl()
{
    return rtspBaseUrl();
}

String streamerHttpUrl()
{
    return "http://" + cfg_hostname + ".local:" + String(HTTP_MJPEG_PORT) + "/stream";
}

String streamerHttpViewerUrl()
{
    return "http://" + cfg_hostname + ".local/stream_view";
}
