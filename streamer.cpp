#include "streamer.h"

#include "audio_capture.h"
#include "config.h"
#include "logger.h"

#include <WebServer.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <algorithm>
#include <esp_heap_caps.h>

namespace {

static const uint16_t RTSP_PORT = 554;
static const uint16_t HTTP_MJPEG_PORT = 81;
static const uint8_t RTP_PT_JPEG = 26;
static const uint8_t RTP_PT_L16 = 96;
static const size_t RTP_PACKET_BYTES = 1400;
static const uint32_t CLIENT_STALL_TIMEOUT_MS = 250;
static const uint32_t RTSP_IDLE_TIMEOUT_MS = 30000;

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
static bool audioAdvertised = false;
static String lastError;

static uint32_t videoTimestamp = 0;
static uint64_t audioSamplesSent = 0;
static uint64_t audioBytesSent = 0;
static uint32_t audioPacketsSent = 0;
static uint32_t lastFrameDueMs = 0;
static uint32_t statsStartedMs = 0;
static uint32_t framesCaptured = 0;
static uint32_t framesRtsp = 0;
static uint32_t framesHttp = 0;
static uint64_t bytesSent = 0;
static uint32_t snapshotLastMs = 0;
static uint8_t *snapshotBuffer = nullptr;
static size_t snapshotCapacity = 0;
static size_t snapshotBytes = 0;

struct ParsedJpeg {
    const uint8_t *scan;
    size_t scanBytes;
    uint16_t width;
    uint16_t height;
    uint8_t type;
    uint8_t quant[128];
    size_t quantBytes;
};

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
        audioSamplesSent = 0;
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

        if ((uint32_t)(millis() - lastProgress) >= CLIENT_STALL_TIMEOUT_MS)
            return false;

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

static bool sendRtpJpeg(RtspClientSlot &slot, const uint8_t *jpeg, size_t jpegBytes)
{
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

        ++slot.videoSequence;
        offset += fragment;
        delay(0);
    }

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

    audioSamplesSent = 0;
    audioBytesSent = 0;
    audioPacketsSent = 0;
    for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
        if (rtspSlotConnected(rtspClients[i]) && rtspClients[i].playing && rtspClients[i].audioSetup)
            rtspClients[i].audioTimestampBase = esp_random();
    }
    audioActive = true;
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
    if (format.bitsPerSample != 16 || format.channels == 0)
        return;

    uint8_t pcm[960];
    size_t bytes = audioCaptureRead(pcm, sizeof(pcm), 0);
    size_t frameBytes = 2U * format.channels;
    bytes -= bytes % frameBytes;
    if (!bytes)
        return;

    // L16 uses network byte order. SensorForge's PCM capture is packed little-endian.
    for (size_t i = 0; i + 1 < bytes; i += 2)
        std::swap(pcm[i], pcm[i + 1]);

    size_t offset = 0;
    while (offset < bytes) {
        uint8_t payloadBuffer[1200 - 12];
        size_t payload = std::min(sizeof(payloadBuffer), bytes - offset);
        payload -= payload % frameBytes;
        if (!payload)
            break;
        memcpy(payloadBuffer, pcm + offset, payload);

        for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
            RtspClientSlot &slot = rtspClients[i];
            if (!rtspSlotConnected(slot) || !slot.playing || !slot.audioSetup)
                continue;

            uint8_t packet[1200];
            uint32_t timestamp = slot.audioTimestampBase + (uint32_t)audioSamplesSent;
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
            memcpy(packet + h, payloadBuffer, payload);

            if (!sendInterleaved(slot, slot.audioChannel, packet, h + payload)) {
                lastError = "RTSP audio client disconnected";
                closeRtspClientSlot(i);
                continue;
            }
            ++slot.audioSequence;
            ++audioPacketsSent;
            audioBytesSent += payload;
        }

        size_t samples = payload / frameBytes;
        audioSamplesSent += samples;
        offset += payload;
        delay(0);
    }
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
    sdp += "c=IN IP4 " + WiFi.localIP().toString() + "\r\n";
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
        if (slot.audioSetup) {
            if (slot.audioTimestampBase == 0)
                slot.audioTimestampBase = esp_random() ? esp_random() : 1;
            startAudioIfNeeded();
        }

        logWrite("RTSP PLAY accepted | client=" + String((unsigned)slotIndex + 1U) +
                 " | video=1 | audio=" + String(slot.audioSetup ? 1 : 0));
        String rtpInfo = "RTP-Info: url=" + rtspBaseUrl() + "/trackID=0;seq=" +
                         String(slot.videoSequence) + ";rtptime=" + String(videoTimestamp);
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

    if (slot.rx.length() >= 4096 ||
        (uint32_t)(millis() - slot.lastActivityMs) > RTSP_IDLE_TIMEOUT_MS) {
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
    bool refreshSnapshot = !snapshotBytes || (uint32_t)(millis() - snapshotLastMs) >= 1000UL;
    if (!needRtsp && !needHttp && !refreshSnapshot)
        return;

    uint32_t fps = cfg_fps > 0 ? (uint32_t)cfg_fps : 1U;
    uint32_t intervalMs = (needRtsp || needHttp)
        ? std::max<uint32_t>(1U, (uint32_t)(1000UL / fps))
        : 1000U;
    uint32_t now = millis();
    if (lastFrameDueMs && (int32_t)(now - lastFrameDueMs) < 0)
        return;
    lastFrameDueMs = now + intervalMs;

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        lastError = "camera frame unavailable";
        return;
    }

    ++framesCaptured;

    if (frame->format != PIXFORMAT_JPEG) {
        lastError = "camera frame is not JPEG";
    } else {
        cacheSnapshot(frame->buf, frame->len);
        snapshotLastMs = millis();
        if (needRtsp) {
            for (uint8_t i = 0; i < RTSP_CLIENT_SLOTS; ++i) {
                RtspClientSlot &slot = rtspClients[i];
                if (!rtspSlotConnected(slot) || !slot.playing || !slot.videoSetup)
                    continue;
                if (!sendRtpJpeg(slot, frame->buf, frame->len))
                    closeRtspClientSlot(i);
            }
        }
        if (needHttp)
            sendHttpFrame(frame->buf, frame->len);
    }

    esp_camera_fb_return(frame);
    videoTimestamp += 90000U / fps;
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
        ",\"frames\":" + String(framesCaptured) +
        ",\"fps\":" + String(streamerMeasuredFps(), 2) +
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

    statsStartedMs = millis();
    lastFrameDueMs = 0;
    videoTimestamp = esp_random();
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
    serviceAudio();
    captureAndDistributeFrame();

    for (uint8_t i = 0; i < HTTP_CLIENT_SLOTS; ++i) {
        if (httpClients[i].client && !httpClients[i].client.connected())
            closeHttpClientSlot(i);
    }
}

void streamerStop()
{
    closeAllRtspClients();
    closeAllHttpClients();
    if (started && cfg_streamer_rtsp_enabled)
        rtspServer.end();
    if (started && cfg_streamer_http_mjpeg_enabled)
        httpServer.end();
    started = false;
    snapshotBytes = 0;
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
    server.on("/streamer_status", HTTP_GET, handleStreamerStatus);
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
