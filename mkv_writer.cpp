#include "mkv_writer.h"

#include "board_config.h"
#include "config.h"
#include "audio_capture.h"
#include "logger.h"
#include "image_motion.h"
#include "recording_storage.h"
#include "recording_write_buffer.h"

#include <FS.h>
#include <esp_camera.h>
#include <esp_timer.h>
#include <time.h>
#include <string.h>

// =============================================================
// Minimal standards-based Matroska writer
//
// Video:
//   Track 1
//   CodecID = V_MJPEG
//   JPEG frames are written directly as SimpleBlock frames.
//
// Timestamp subtitles:
//   Track 2
//   CodecID = S_TEXT/UTF8
//   Plain UTF-8 text in BlockGroup/Block.
//   Start time = Block timestamp.
//   Display time = BlockDuration.
//
// Audio:
//   Track 3
//   CodecID = A_PCM/INT/LIT
//   Packed little-endian PCM from the generic audio_capture subsystem.
//
// TimestampScale = 1,000,000 ns => one Matroska tick = 1 ms.
//
// Audio is optional and deliberately non-fatal: capture/backend failure leaves
// the video recording running. Shared storage write failures remain fatal.
//
// No JPEG decoding/re-encoding.
// =============================================================

static RecordingWriteBufferedFile mkvFile;
static RecordingWriteBufferStats lastWriteBufferStats = {};
static String mkvPath;

static uint32_t mkvFps       = 5;
static uint32_t frameCount   = 0;
static uint32_t maxFrameSize = 0;

static uint16_t mkvWidth  = 0;
static uint16_t mkvHeight = 0;

static bool headerWritten = false;
static bool writeFailed   = false;
static bool sizeLimitHit  = false;

static time_t recordingStartEpoch = 0;
static bool recordingStartTimeValid = false;
static bool subtitleTrackEnabled    = false;
static uint64_t nextSubtitleMs      = 0;
static bool mkvSparseMode           = false;
static uint64_t mkvLastFrameTimeMs  = 0;
static uint32_t mkvSparseTailMs     = 1;


// Optional production audio state. Audio is embedded only in normal MKV
// recordings, never in sparse shooter MKVs.
static bool audioRequested = false;
static bool audioTrackEnabled = false;
static bool audioCaptureStarted = false;
static bool audioRuntimeFailed = false;
static AudioFormat mkvAudioFormat = {0, 0, 0};
static uint64_t audioBytesWritten = 0;
static uint64_t audioLastEndTimeMs = 0;
static AudioCaptureStats finalAudioStats = {};
static uint8_t audioMuxBuffer[4080];

// Detailed per-stage timing is enabled only by the explicit Recording Load
// Test. Normal recordings keep the established low-overhead path.
static bool detailedFrameTimingEnabled = false;
static MkvFrameTiming lastFrameTiming = {};

static uint64_t frameTimingNowUs()
{
    return detailedFrameTimingEnabled
        ? (uint64_t)esp_timer_get_time()
        : 0ULL;
}

static uint32_t frameTimingElapsedUs(uint64_t startUs)
{
    if (!detailedFrameTimingEnabled || startUs == 0)
        return 0;

    uint64_t nowUs =
        (uint64_t)esp_timer_get_time();

    uint64_t elapsed =
        nowUs >= startUs
        ? nowUs - startUs
        : 0ULL;

    return elapsed > 0xFFFFFFFFULL
        ? 0xFFFFFFFFUL
        : (uint32_t)elapsed;
}


// Keep files below 4 GiB for broad FAT32/storage compatibility.
static const uint64_t MKV_MAX_FILE_SIZE =
    0xFFF00000ULL;

// Matroska implementation recommendation: keep Clusters short.
static const uint32_t MKV_CLUSTER_MAX_MS =
    5000UL;

static const uint32_t MKV_CLUSTER_MAX_BYTES =
    5UL * 1024UL * 1024UL;


// =============================================================
// EBML ELEMENT IDs
// =============================================================

static const uint32_t ID_EBML               = 0x1A45DFA3;
static const uint32_t ID_EBML_VERSION       = 0x4286;
static const uint32_t ID_EBML_READ_VERSION  = 0x42F7;
static const uint32_t ID_EBML_MAX_ID_LENGTH = 0x42F2;
static const uint32_t ID_EBML_MAX_SIZE_LEN  = 0x42F3;
static const uint32_t ID_DOC_TYPE           = 0x4282;
static const uint32_t ID_DOC_TYPE_VERSION   = 0x4287;
static const uint32_t ID_DOC_TYPE_READ_VER  = 0x4285;

static const uint32_t ID_SEGMENT         = 0x18538067;
static const uint32_t ID_INFO            = 0x1549A966;
static const uint32_t ID_TIMESTAMP_SCALE = 0x2AD7B1;
static const uint32_t ID_DURATION        = 0x4489;
static const uint32_t ID_DATE_UTC        = 0x4461;
static const uint32_t ID_MUXING_APP      = 0x4D80;
static const uint32_t ID_WRITING_APP     = 0x5741;

static const uint32_t ID_TRACKS       = 0x1654AE6B;
static const uint32_t ID_TRACK_ENTRY  = 0xAE;
static const uint32_t ID_TRACK_NUMBER = 0xD7;
static const uint32_t ID_TRACK_UID    = 0x73C5;
static const uint32_t ID_TRACK_TYPE   = 0x83;
static const uint32_t ID_FLAG_ENABLED = 0xB9;
static const uint32_t ID_FLAG_DEFAULT = 0x88;
static const uint32_t ID_FLAG_FORCED  = 0x55AA;
static const uint32_t ID_FLAG_LACING  = 0x9C;
static const uint32_t ID_CODEC_ID     = 0x86;
static const uint32_t ID_NAME         = 0x536E;
static const uint32_t ID_LANGUAGE     = 0x22B59C;

static const uint32_t ID_VIDEO        = 0xE0;
static const uint32_t ID_PIXEL_WIDTH  = 0xB0;
static const uint32_t ID_PIXEL_HEIGHT = 0xBA;

static const uint32_t ID_AUDIO               = 0xE1;
static const uint32_t ID_SAMPLING_FREQUENCY  = 0xB5;
static const uint32_t ID_CHANNELS            = 0x9F;
static const uint32_t ID_BIT_DEPTH           = 0x6264;

static const uint32_t ID_CLUSTER       = 0x1F43B675;
static const uint32_t ID_TIMESTAMP     = 0xE7;
static const uint32_t ID_SIMPLE_BLOCK  = 0xA3;
static const uint32_t ID_BLOCK_GROUP   = 0xA0;
static const uint32_t ID_BLOCK         = 0xA1;
static const uint32_t ID_BLOCK_DURATION = 0x9B;


// =============================================================
// PATCHABLE MASTER ELEMENT
// =============================================================

struct MasterMark {
    size_t sizePosition;
    size_t dataStart;
};

static MasterMark segmentMark = {0, 0};
static MasterMark clusterMark = {0, 0};

static bool clusterOpen = false;
static uint64_t clusterTimestampMs = 0;

static size_t durationPayloadPosition = 0;


// =============================================================
// LOW-LEVEL WRITE HELPERS
// =============================================================

static bool writeRaw(
    const uint8_t *data,
    size_t length
)
{
    if (!mkvFile || writeFailed)
        return false;

    if (length == 0)
        return true;

    size_t written =
        mkvFile.write(
            data,
            length
        );

    if (written != length) {

        Serial.println(
            "MKV: SD write failed"
        );

        writeFailed = true;

        return false;
    }

    return true;
}


static bool writeByte(
    uint8_t value
)
{
    return writeRaw(
        &value,
        1
    );
}


static uint8_t elementIdLength(
    uint32_t id
)
{
    if (id > 0xFFFFFFUL)
        return 4;

    if (id > 0xFFFFUL)
        return 3;

    if (id > 0xFFUL)
        return 2;

    return 1;
}


static bool writeElementId(
    uint32_t id
)
{
    uint8_t length =
        elementIdLength(id);

    uint8_t bytes[4];

    for (uint8_t i = 0; i < length; i++) {

        uint8_t shift =
            (uint8_t)(
                (length - 1U - i) * 8U
            );

        bytes[i] =
            (uint8_t)(
                (id >> shift) & 0xFFU
            );
    }

    return writeRaw(
        bytes,
        length
    );
}


static uint8_t vintSizeLength(
    uint64_t value
)
{
    for (uint8_t length = 1; length <= 8; length++) {

        uint8_t bits =
            (uint8_t)(7U * length);

        uint64_t maximum;

        if (bits == 56) {
            maximum =
                0x00FFFFFFFFFFFFFEULL;
        } else {
            maximum =
                (1ULL << bits) - 2ULL;
        }

        if (value <= maximum)
            return length;
    }

    return 0;
}


static bool writeVintSize(
    uint64_t value
)
{
    uint8_t length =
        vintSizeLength(value);

    if (length == 0) {

        Serial.println(
            "MKV: EBML size too large"
        );

        writeFailed = true;

        return false;
    }

    uint64_t encoded =
        value |
        (1ULL << (7U * length));

    uint8_t bytes[8];

    for (uint8_t i = 0; i < length; i++) {

        uint8_t shift =
            (uint8_t)(
                (length - 1U - i) * 8U
            );

        bytes[i] =
            (uint8_t)(
                (encoded >> shift) & 0xFFU
            );
    }

    return writeRaw(
        bytes,
        length
    );
}


static bool writeUnknownSize8()
{
    const uint8_t unknown[8] = {
        0x01,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF
    };

    return writeRaw(
        unknown,
        sizeof(unknown)
    );
}


static bool patchSize8(
    size_t position,
    uint64_t value
)
{
    if (!mkvFile)
        return false;

    if (value >
        0x00FFFFFFFFFFFFFEULL) {

        Serial.println(
            "MKV: master element too large"
        );

        return false;
    }


    size_t oldPosition =
        mkvFile.position();

    if (!mkvFile.seek(position)) {

        Serial.println(
            "MKV: seek failed while patching size"
        );

        return false;
    }


    uint64_t encoded =
        value |
        0x0100000000000000ULL;

    uint8_t bytes[8];

    for (uint8_t i = 0; i < 8; i++) {

        uint8_t shift =
            (uint8_t)(
                (7U - i) * 8U
            );

        bytes[i] =
            (uint8_t)(
                (encoded >> shift) & 0xFFU
            );
    }


    bool ok =
        mkvFile.write(
            bytes,
            sizeof(bytes)
        ) == sizeof(bytes);


    if (!mkvFile.seek(oldPosition)) {

        Serial.println(
            "MKV: could not restore file position"
        );

        return false;
    }


    if (!ok) {

        Serial.println(
            "MKV: size patch write failed"
        );
    }

    return ok;
}


static bool beginMaster(
    uint32_t id,
    MasterMark &mark
)
{
    if (!writeElementId(id))
        return false;

    mark.sizePosition =
        mkvFile.position();

    if (!writeUnknownSize8())
        return false;

    mark.dataStart =
        mkvFile.position();

    return true;
}


static bool endMaster(
    const MasterMark &mark
)
{
    if (!mkvFile)
        return false;

    size_t endPosition =
        mkvFile.position();

    if (endPosition < mark.dataStart)
        return false;

    uint64_t payloadSize =
        (uint64_t)(
            endPosition -
            mark.dataStart
        );

    return patchSize8(
        mark.sizePosition,
        payloadSize
    );
}


static bool writeMasterHeader(
    uint32_t id,
    uint64_t payloadSize
)
{
    return
        writeElementId(id) &&
        writeVintSize(payloadSize);
}


static uint8_t uintPayloadLength(
    uint64_t value
)
{
    uint8_t length = 1;

    while (
        length < 8 &&
        value >=
            (1ULL << (8U * length))
    ) {
        length++;
    }

    return length;
}


static bool writeUIntPayload(
    uint64_t value,
    uint8_t length
)
{
    uint8_t bytes[8];

    for (uint8_t i = 0; i < length; i++) {

        uint8_t shift =
            (uint8_t)(
                (length - 1U - i) * 8U
            );

        bytes[i] =
            (uint8_t)(
                (value >> shift) & 0xFFU
            );
    }

    return writeRaw(
        bytes,
        length
    );
}


static bool writeUIntElement(
    uint32_t id,
    uint64_t value
)
{
    uint8_t payloadLength =
        uintPayloadLength(value);

    return
        writeElementId(id) &&
        writeVintSize(payloadLength) &&
        writeUIntPayload(
            value,
            payloadLength
        );
}


static bool writeSignedInt64Element(
    uint32_t id,
    int64_t value
)
{
    uint64_t raw;

    memcpy(
        &raw,
        &value,
        sizeof(raw)
    );

    return
        writeElementId(id) &&
        writeVintSize(8) &&
        writeUIntPayload(
            raw,
            8
        );
}


static bool writeStringElement(
    uint32_t id,
    const char *text
)
{
    if (!text)
        text = "";

    size_t length =
        strlen(text);

    return
        writeElementId(id) &&
        writeVintSize(length) &&
        writeRaw(
            (const uint8_t *)text,
            length
        );
}


static bool writeFloat64Payload(
    double value
)
{
    uint64_t bits = 0;

    static_assert(
        sizeof(double) == sizeof(uint64_t),
        "MKV writer requires 64-bit double"
    );

    memcpy(
        &bits,
        &value,
        sizeof(bits)
    );

    return writeUIntPayload(
        bits,
        8
    );
}


static bool writeFloat64Element(
    uint32_t id,
    double value,
    size_t *payloadPosition = nullptr
)
{
    if (!writeElementId(id))
        return false;

    if (!writeVintSize(8))
        return false;

    if (payloadPosition) {
        *payloadPosition =
            mkvFile.position();
    }

    return writeFloat64Payload(
        value
    );
}


static bool patchFloat64(
    size_t position,
    double value
)
{
    if (!mkvFile)
        return false;


    size_t oldPosition =
        mkvFile.position();

    if (!mkvFile.seek(position)) {

        Serial.println(
            "MKV: seek failed while patching duration"
        );

        return false;
    }


    uint64_t bits = 0;

    memcpy(
        &bits,
        &value,
        sizeof(bits)
    );

    uint8_t bytes[8];

    for (uint8_t i = 0; i < 8; i++) {

        uint8_t shift =
            (uint8_t)(
                (7U - i) * 8U
            );

        bytes[i] =
            (uint8_t)(
                (bits >> shift) & 0xFFU
            );
    }


    bool ok =
        mkvFile.write(
            bytes,
            sizeof(bytes)
        ) == sizeof(bytes);


    if (!mkvFile.seek(oldPosition)) {

        Serial.println(
            "MKV: could not restore file position"
        );

        return false;
    }


    return ok;
}


static bool writeInt16BE(
    int16_t value
)
{
    uint16_t raw =
        (uint16_t)value;

    uint8_t bytes[2];

    bytes[0] =
        (uint8_t)(
            (raw >> 8) & 0xFFU
        );

    bytes[1] =
        (uint8_t)(
            raw & 0xFFU
        );

    return writeRaw(
        bytes,
        sizeof(bytes)
    );
}


// =============================================================
// SIZE CALCULATION HELPERS
// =============================================================

static uint64_t uintElementTotalSize(
    uint32_t id,
    uint64_t value
)
{
    uint8_t payloadLength =
        uintPayloadLength(value);

    return
        elementIdLength(id) +
        vintSizeLength(payloadLength) +
        payloadLength;
}


static uint64_t binaryElementTotalSize(
    uint32_t id,
    uint64_t payloadLength
)
{
    return
        elementIdLength(id) +
        vintSizeLength(payloadLength) +
        payloadLength;
}


// =============================================================
// MATROSKA HEADER / TRACKS
// =============================================================

static bool writeMatroskaHeader(
    uint16_t width,
    uint16_t height
)
{
    // ---------------------------------------------------------
    // EBML Header
    // ---------------------------------------------------------

    MasterMark ebml;

    if (!beginMaster(
            ID_EBML,
            ebml
        )) {
        return false;
    }

    writeUIntElement(
        ID_EBML_VERSION,
        1
    );

    writeUIntElement(
        ID_EBML_READ_VERSION,
        1
    );

    writeUIntElement(
        ID_EBML_MAX_ID_LENGTH,
        4
    );

    writeUIntElement(
        ID_EBML_MAX_SIZE_LEN,
        8
    );

    writeStringElement(
        ID_DOC_TYPE,
        "matroska"
    );

    // Current Matroska version.
    writeUIntElement(
        ID_DOC_TYPE_VERSION,
        4
    );

    // We only require v2 features for playback
    // (notably SimpleBlock).
    writeUIntElement(
        ID_DOC_TYPE_READ_VER,
        2
    );

    if (!endMaster(ebml))
        return false;


    // ---------------------------------------------------------
    // Segment
    // ---------------------------------------------------------

    if (!beginMaster(
            ID_SEGMENT,
            segmentMark
        )) {
        return false;
    }


    // ---------------------------------------------------------
    // Info
    // ---------------------------------------------------------

    MasterMark info;

    if (!beginMaster(
            ID_INFO,
            info
        )) {
        return false;
    }

    // 1 tick = 1 ms.
    writeUIntElement(
        ID_TIMESTAMP_SCALE,
        1000000UL
    );

    writeStringElement(
        ID_MUXING_APP,
        "ESP32-S3 SensorCam"
    );

    writeStringElement(
        ID_WRITING_APP,
        "ESP32-S3 SensorCam"
    );

    // Duration is expressed in Segment Ticks.
    // Patched when the file is finalized.
    writeFloat64Element(
        ID_DURATION,
        0.0,
        &durationPayloadPosition
    );


    if (recordingStartTimeValid) {

        // Matroska DateUTC:
        // signed nanoseconds since 2001-01-01 00:00:00 UTC.
        const int64_t unixToMatroskaEpoch =
            978307200LL;

        int64_t dateUtcNs =
            (
                (int64_t)recordingStartEpoch -
                unixToMatroskaEpoch
            ) *
            1000000000LL;

        writeSignedInt64Element(
            ID_DATE_UTC,
            dateUtcNs
        );
    }


    if (!endMaster(info))
        return false;


    // ---------------------------------------------------------
    // Tracks
    // ---------------------------------------------------------

    MasterMark tracks;

    if (!beginMaster(
            ID_TRACKS,
            tracks
        )) {
        return false;
    }


    // ---------------- Video Track ----------------

    MasterMark videoTrack;

    if (!beginMaster(
            ID_TRACK_ENTRY,
            videoTrack
        )) {
        return false;
    }

    writeUIntElement(
        ID_TRACK_NUMBER,
        1
    );

    writeUIntElement(
        ID_TRACK_UID,
        1
    );

    writeUIntElement(
        ID_TRACK_TYPE,
        1
    );

    writeUIntElement(
        ID_FLAG_ENABLED,
        1
    );

    writeUIntElement(
        ID_FLAG_DEFAULT,
        1
    );

    writeUIntElement(
        ID_FLAG_LACING,
        0
    );

    writeStringElement(
        ID_CODEC_ID,
        "V_MJPEG"
    );


    MasterMark video;

    if (!beginMaster(
            ID_VIDEO,
            video
        )) {
        return false;
    }

    writeUIntElement(
        ID_PIXEL_WIDTH,
        width
    );

    writeUIntElement(
        ID_PIXEL_HEIGHT,
        height
    );

    if (!endMaster(video))
        return false;

    if (!endMaster(videoTrack))
        return false;


    // ---------------- Audio Track ----------------

    if (audioTrackEnabled) {

        MasterMark audioTrack;

        if (!beginMaster(
                ID_TRACK_ENTRY,
                audioTrack
            )) {
            return false;
        }

        writeUIntElement(ID_TRACK_NUMBER, 3);
        writeUIntElement(ID_TRACK_UID, 3);
        writeUIntElement(ID_TRACK_TYPE, 2);
        writeUIntElement(ID_FLAG_ENABLED, 1);
        writeUIntElement(ID_FLAG_DEFAULT, 1);
        writeUIntElement(ID_FLAG_LACING, 0);

        writeStringElement(ID_CODEC_ID, "A_PCM/INT/LIT");
        writeStringElement(ID_NAME, "Audio");
        writeStringElement(ID_LANGUAGE, "und");

        MasterMark audio;

        if (!beginMaster(ID_AUDIO, audio))
            return false;

        writeFloat64Element(
            ID_SAMPLING_FREQUENCY,
            (double)mkvAudioFormat.sampleRate
        );

        writeUIntElement(
            ID_CHANNELS,
            mkvAudioFormat.channels
        );

        writeUIntElement(
            ID_BIT_DEPTH,
            mkvAudioFormat.bitsPerSample
        );

        if (!endMaster(audio))
            return false;

        if (!endMaster(audioTrack))
            return false;
    }


    // ---------------- Subtitle Track ----------------

    if (subtitleTrackEnabled) {

        MasterMark subtitleTrack;

        if (!beginMaster(
                ID_TRACK_ENTRY,
                subtitleTrack
            )) {
            return false;
        }

        writeUIntElement(
            ID_TRACK_NUMBER,
            2
        );

        writeUIntElement(
            ID_TRACK_UID,
            2
        );

        writeUIntElement(
            ID_TRACK_TYPE,
            17
        );

        writeUIntElement(
            ID_FLAG_ENABLED,
            1
        );

        // Request automatic subtitle selection.
        writeUIntElement(
            ID_FLAG_DEFAULT,
            1
        );

        // The timestamp is intended as on-screen information.
        // Players may still apply their own user preferences.
        writeUIntElement(
            ID_FLAG_FORCED,
            1
        );

        writeUIntElement(
            ID_FLAG_LACING,
            0
        );

        writeStringElement(
            ID_CODEC_ID,
            "S_TEXT/UTF8"
        );

        writeStringElement(
            ID_NAME,
            "Timestamp"
        );

        // The timestamp text itself is language-neutral.
        writeStringElement(
            ID_LANGUAGE,
            "und"
        );

        if (!endMaster(subtitleTrack))
            return false;
    }


    if (!endMaster(tracks))
        return false;


    if (writeFailed)
        return false;


    headerWritten = true;

    mkvWidth  = width;
    mkvHeight = height;


    return true;
}


// =============================================================
// CLUSTER HANDLING
// =============================================================

static bool closeCluster()
{
    if (!clusterOpen)
        return true;

    // Clusters deliberately keep the 8-byte unknown-size VINT written by
    // beginMaster(). Matroska permits unknown-size Clusters, and the next
    // Cluster element unambiguously terminates the previous one. Avoiding a
    // final size backpatch here removes two synchronous seeks from the
    // time-critical recording path every time a Cluster rolls over.
    //
    // Other masters (Info, Tracks, Segment, etc.) continue to use endMaster()
    // and therefore retain their final known sizes.
    clusterOpen = false;

    return true;
}


static bool openCluster(
    uint64_t timestampMs
)
{
    if (!closeCluster())
        return false;

    if (!beginMaster(
            ID_CLUSTER,
            clusterMark
        )) {
        return false;
    }

    clusterOpen =
        true;

    clusterTimestampMs =
        timestampMs;

    if (!writeUIntElement(
            ID_TIMESTAMP,
            timestampMs
        )) {
        return false;
    }

    return true;
}


static bool ensureClusterForTime(
    uint64_t timestampMs
)
{
    if (!clusterOpen) {
        return openCluster(
            timestampMs
        );
    }


    uint64_t elapsed =
        timestampMs -
        clusterTimestampMs;

    uint64_t clusterBytes =
        (uint64_t)(
            mkvFile.position() -
            clusterMark.dataStart
        );


    if (
        elapsed >= MKV_CLUSTER_MAX_MS ||
        clusterBytes >= MKV_CLUSTER_MAX_BYTES
    ) {
        return openCluster(
            timestampMs
        );
    }


    return true;
}


// =============================================================
// AUDIO PCM BLOCKS
// =============================================================

static uint64_t audioByteRate()
{
    if (
        mkvAudioFormat.sampleRate == 0 ||
        mkvAudioFormat.channels == 0 ||
        mkvAudioFormat.bitsPerSample == 0
    ) {
        return 0;
    }

    return
        (uint64_t)mkvAudioFormat.sampleRate *
        (uint64_t)mkvAudioFormat.channels *
        (uint64_t)(mkvAudioFormat.bitsPerSample / 8U);
}


static uint64_t audioTimestampMsForBytes(
    uint64_t bytes
)
{
    uint64_t rate = audioByteRate();

    if (rate == 0)
        return 0;

    return
        (bytes * 1000ULL) /
        rate;
}


static void collectAndStopAudioCapture()
{
    if (!audioCaptureStarted)
        return;

    finalAudioStats =
        audioCaptureStats();

    audioCaptureStop();
    audioCaptureStarted = false;
}


static void disableAudioAfterRuntimeFailure(
    const String &reason
)
{
    if (!audioRuntimeFailed) {
        audioRuntimeFailed = true;

        consoleWrite(
            "REC",
            "WARN | MKV audio stopped | " + reason
        );

        logWrite(
            "Recording WARN | MKV audio stopped | " + reason
        );
    }

    collectAndStopAudioCapture();
}


static bool writeAudioBlock(
    const uint8_t *pcmData,
    size_t pcmBytes,
    uint64_t timestampMs
)
{
    if (
        !audioTrackEnabled ||
        !pcmData ||
        pcmBytes == 0
    ) {
        return true;
    }

    uint64_t clusterStartUs =
        frameTimingNowUs();

    if (!ensureClusterForTime(timestampMs)) {
        if (detailedFrameTimingEnabled)
            lastFrameTiming.clusterUs +=
                frameTimingElapsedUs(clusterStartUs);
        return false;
    }

    if (detailedFrameTimingEnabled)
        lastFrameTiming.clusterUs +=
            frameTimingElapsedUs(clusterStartUs);

    uint64_t audioWriteStartUs =
        frameTimingNowUs();

    if (timestampMs < clusterTimestampMs)
        return false;

    uint64_t relative64 =
        timestampMs - clusterTimestampMs;

    if (relative64 > 32767ULL) {
        Serial.println("MKV: audio timestamp outside Cluster range");
        writeFailed = true;
        return false;
    }

    uint64_t blockPayloadSize =
        1ULL +
        2ULL +
        1ULL +
        (uint64_t)pcmBytes;

    uint64_t totalWriteSize =
        elementIdLength(ID_SIMPLE_BLOCK) +
        vintSizeLength(blockPayloadSize) +
        blockPayloadSize;

    uint64_t nextFileSize =
        (uint64_t)mkvFile.size() +
        totalWriteSize +
        64ULL;

    if (nextFileSize > MKV_MAX_FILE_SIZE) {
        if (!sizeLimitHit) {
            Serial.println("MKV: file size limit reached by audio");
            sizeLimitHit = true;
        }
        return false;
    }

    if (!writeElementId(ID_SIMPLE_BLOCK))
        return false;

    if (!writeVintSize(blockPayloadSize))
        return false;

    // Track 3 as EBML VINT.
    if (!writeByte(0x83))
        return false;

    if (!writeInt16BE((int16_t)relative64))
        return false;

    // PCM: no lacing, no keyframe semantics required.
    if (!writeByte(0x00))
        return false;

    if (!writeRaw(pcmData, pcmBytes))
        return false;

    audioBytesWritten +=
        (uint64_t)pcmBytes;

    uint64_t rate = audioByteRate();

    if (rate > 0) {
        audioLastEndTimeMs =
            (
                audioBytesWritten * 1000ULL +
                rate - 1ULL
            ) /
            rate;
    }

    if (detailedFrameTimingEnabled)
        lastFrameTiming.audioWriteUs +=
            frameTimingElapsedUs(audioWriteStartUs);

    return true;
}


static bool drainAudioUntil(
    uint64_t timestampLimitMs,
    bool includeEqualStart
)
{
    if (
        !audioTrackEnabled ||
        !audioCaptureStarted ||
        audioRuntimeFailed
    ) {
        return true;
    }

    uint32_t bytesPerSample =
        mkvAudioFormat.bitsPerSample / 8U;

    size_t blockAlign =
        (size_t)mkvAudioFormat.channels *
        (size_t)bytesPerSample;

    if (blockAlign == 0) {
        disableAudioAfterRuntimeFailure("invalid PCM block alignment");
        return true;
    }

    while (true) {
        uint64_t blockTimeMs =
            audioTimestampMsForBytes(audioBytesWritten);

        if (
            includeEqualStart
            ? blockTimeMs > timestampLimitMs
            : blockTimeMs >= timestampLimitMs
        ) {
            break;
        }

        size_t available =
            audioCaptureBufferedBytes();

        size_t toRead =
            available < sizeof(audioMuxBuffer)
            ? available
            : sizeof(audioMuxBuffer);

        toRead -=
            toRead % blockAlign;

        if (toRead == 0)
            break;

        uint64_t audioReadStartUs =
            frameTimingNowUs();

        size_t got =
            audioCaptureRead(
                audioMuxBuffer,
                toRead,
                0
            );

        if (detailedFrameTimingEnabled)
            lastFrameTiming.audioReadUs +=
                frameTimingElapsedUs(audioReadStartUs);

        if (got == 0)
            break;

        if (got % blockAlign) {
            disableAudioAfterRuntimeFailure(
                "unaligned PCM block from capture backend"
            );
            return true;
        }

        if (!writeAudioBlock(
                audioMuxBuffer,
                got,
                blockTimeMs
            )) {
            return false;
        }

        if (writeFailed || sizeLimitHit)
            return false;
    }

    return true;
}


static bool drainAudioToRecordingEnd(
    uint64_t targetDurationMs
)
{
    if (
        !audioTrackEnabled ||
        !audioCaptureStarted ||
        audioRuntimeFailed
    ) {
        return true;
    }

    // Give the capture task a short bounded opportunity to deliver final PCM
    // up to the video endpoint instead of truncating the audio tail merely
    // because the last camera frame was just written.
    uint32_t deadline =
        millis() + 250UL;

    while (
        audioTimestampMsForBytes(audioBytesWritten) <
            targetDurationMs
    ) {
        uint64_t before =
            audioBytesWritten;

        if (!drainAudioUntil(
                targetDurationMs,
                true
            )) {
            return false;
        }

        if (
            audioTimestampMsForBytes(audioBytesWritten) >=
                targetDurationMs
        ) {
            break;
        }

        if ((int32_t)(deadline - millis()) <= 0)
            break;

        if (audioBytesWritten == before)
            delay(2);
    }

    return true;
}


// =============================================================
// SUBTITLE BLOCK
// =============================================================

static bool writeTimestampSubtitle(
    uint64_t subtitleTimeMs,
    uint32_t durationMs
)
{
    if (!subtitleTrackEnabled)
        return true;


    if (subtitleTimeMs < clusterTimestampMs)
        return false;

    uint64_t relative64 =
        subtitleTimeMs -
        clusterTimestampMs;

    if (relative64 > 32767ULL) {

        Serial.println(
            "MKV: subtitle timestamp outside Cluster range"
        );

        writeFailed = true;

        return false;
    }


    time_t absoluteTime =
        recordingStartEpoch +
        (time_t)(
            subtitleTimeMs / 1000ULL
        );

    struct tm localTime;

    if (!localtime_r(
            &absoluteTime,
            &localTime
        )) {
        return false;
    }


    char text[32];

    strftime(
        text,
        sizeof(text),
        "%Y-%m-%d %H:%M:%S",
        &localTime
    );


    size_t textLength =
        strlen(text);

    uint64_t blockPayloadSize =
        1ULL +   // Track number VINT (0x82)
        2ULL +   // signed relative timestamp
        1ULL +   // flags
        textLength;

    uint64_t blockTotalSize =
        binaryElementTotalSize(
            ID_BLOCK,
            blockPayloadSize
        );

    uint64_t durationTotalSize =
        uintElementTotalSize(
            ID_BLOCK_DURATION,
            durationMs
        );

    uint64_t groupPayloadSize =
        blockTotalSize +
        durationTotalSize;


    if (!writeMasterHeader(
            ID_BLOCK_GROUP,
            groupPayloadSize
        )) {
        return false;
    }


    // Block
    if (!writeElementId(
            ID_BLOCK
        )) {
        return false;
    }

    if (!writeVintSize(
            blockPayloadSize
        )) {
        return false;
    }

    // Track 2 as EBML VINT.
    if (!writeByte(0x82))
        return false;

    if (!writeInt16BE(
            (int16_t)relative64
        )) {
        return false;
    }

    // Block flags: no lacing.
    if (!writeByte(0x00))
        return false;

    if (!writeRaw(
            (const uint8_t *)text,
            textLength
        )) {
        return false;
    }


    // S_TEXT/UTF8 duration is represented by BlockDuration.
    if (!writeUIntElement(
            ID_BLOCK_DURATION,
            durationMs
        )) {
        return false;
    }


    return true;
}


static bool writePendingSubtitles(
    uint64_t frameTimeMs
)
{
    if (!subtitleTrackEnabled)
        return true;


    while (
        nextSubtitleMs <= frameTimeMs
    ) {

        if (!writeTimestampSubtitle(
                nextSubtitleMs,
                1000
            )) {
            return false;
        }

        nextSubtitleMs +=
            1000ULL;
    }


    return true;
}


// =============================================================
// VIDEO SIMPLEBLOCK
// =============================================================

static bool writeVideoFrame(
    const uint8_t *jpegData,
    uint32_t jpegSize,
    uint64_t frameTimeMs
)
{
    if (frameTimeMs < clusterTimestampMs)
        return false;


    uint64_t relative64 =
        frameTimeMs -
        clusterTimestampMs;

    if (relative64 > 32767ULL) {

        Serial.println(
            "MKV: video timestamp outside Cluster range"
        );

        writeFailed = true;

        return false;
    }


    uint64_t blockPayloadSize =
        1ULL +       // Track 1 VINT
        2ULL +       // signed relative timestamp
        1ULL +       // flags
        jpegSize;


    uint64_t totalWriteSize =
        elementIdLength(
            ID_SIMPLE_BLOCK
        ) +
        vintSizeLength(
            blockPayloadSize
        ) +
        blockPayloadSize;


    uint64_t nextFileSize =
        (uint64_t)mkvFile.size() +
        totalWriteSize +
        64ULL;


    if (nextFileSize >
        MKV_MAX_FILE_SIZE) {

        if (!sizeLimitHit) {

            Serial.println(
                "MKV: file size limit reached"
            );

            sizeLimitHit = true;
        }

        return false;
    }


    if (!writeElementId(
            ID_SIMPLE_BLOCK
        )) {
        return false;
    }

    if (!writeVintSize(
            blockPayloadSize
        )) {
        return false;
    }

    // Track 1 as EBML VINT.
    if (!writeByte(0x81))
        return false;

    if (!writeInt16BE(
            (int16_t)relative64
        )) {
        return false;
    }

    // 0x80 = keyframe.
    // Every MJPEG frame is independently decodable.
    if (!writeByte(0x80))
        return false;

    if (!writeRaw(
            jpegData,
            jpegSize
        )) {
        return false;
    }


    return true;
}


// =============================================================
// START MKV RECORDING
// =============================================================

void mkvStart(
    const String &path,
    int fps
)
{
    if (mkvFile) {
        mkvEnd();
    }


    mkvPath =
        path;

    mkvFps =
        fps > 0
        ? (uint32_t)fps
        : 1;


    frameCount   = 0;
    maxFrameSize = 0;

    mkvWidth  = 0;
    mkvHeight = 0;

    headerWritten = false;
    writeFailed   = false;
    sizeLimitHit  = false;

    clusterOpen        = false;
    clusterTimestampMs = 0;

    durationPayloadPosition = 0;

    nextSubtitleMs = 0;
    mkvSparseMode = false;
    mkvLastFrameTimeMs = 0;
    mkvSparseTailMs = 1;

    audioRequested =
        cfg_audio_enabled != 0;

    audioTrackEnabled = false;
    audioCaptureStarted = false;
    audioRuntimeFailed = false;
    audioBytesWritten = 0;
    audioLastEndTimeMs = 0;
    finalAudioStats = {};
    lastWriteBufferStats = {};

    mkvAudioFormat.sampleRate =
        (uint32_t)cfg_audio_sample_rate;

    mkvAudioFormat.bitsPerSample =
        (uint16_t)cfg_audio_bits_per_sample;

    mkvAudioFormat.channels =
        (uint8_t)cfg_audio_channels;

    if (audioRequested) {
        String audioError;

        if (!audioCaptureFormatSupported(
                mkvAudioFormat,
                audioError
            )) {

            audioRequested = false;

            consoleWrite(
                "REC",
                "WARN | MKV audio unavailable | " + audioError
            );

            logWrite(
                "Recording WARN | MKV audio unavailable | " + audioError
            );
        }
    }


    recordingStartEpoch =
        time(nullptr);

    struct tm startTm;

    recordingStartTimeValid =
        localtime_r(
            &recordingStartEpoch,
            &startTm
        ) != nullptr &&
        (startTm.tm_year + 1900) > 2020;


    subtitleTrackEnabled =
        cfg_timestamp_enabled &&
        recordingStartTimeValid;


    if (
        cfg_timestamp_enabled &&
        !recordingStartTimeValid
    ) {
        Serial.println(
            "MKV: system time invalid - timestamp subtitle track disabled"
        );
    }


    if (STORAGE.exists(path.c_str())) {
        STORAGE.remove(
            path.c_str()
        );
    }


    if (!mkvFile.openWrite(
            path,
            cfg_recording_encryption != 0
        )) {

        consoleWrite(
            "REC",
            "ERROR | MKV cannot open | " +
            path
        );

        return;
    }


    // Headers are written with the first JPEG frame because
    // camera_fb_t provides the actual width and height.
}


// =============================================================
// ADD ONE JPEG FRAME
// =============================================================

void mkvAddFrame()
{
    lastFrameTiming = {};
    lastFrameTiming.valid =
        detailedFrameTimingEnabled;

    if (
        !mkvFile ||
        writeFailed ||
        sizeLimitHit
    ) {
        return;
    }


    uint64_t cameraStartUs =
        frameTimingNowUs();

    camera_fb_t *fb =
        esp_camera_fb_get();

    if (detailedFrameTimingEnabled)
        lastFrameTiming.cameraUs =
            frameTimingElapsedUs(cameraStartUs);


    if (!fb) {

        Serial.println(
            "MKV: camera frame failed"
        );

        return;
    }


    if (fb->format != PIXFORMAT_JPEG) {

        Serial.println(
            "MKV: frame is not JPEG"
        );

        esp_camera_fb_return(fb);

        return;
    }


    if (!headerWritten) {

        uint64_t headerStartUs =
            frameTimingNowUs();

        if (audioRequested) {
            String audioError;

            if (audioCaptureStart(
                    mkvAudioFormat,
                    audioError
                )) {

                audioCaptureStarted = true;
                audioTrackEnabled = true;

            } else {

                audioRequested = false;
                audioTrackEnabled = false;

                consoleWrite(
                    "REC",
                    "WARN | MKV audio start failed, continuing video-only | " +
                    audioError
                );

                logWrite(
                    "Recording WARN | MKV audio start failed, continuing video-only | " +
                    audioError
                );
            }
        }

        if (!writeMatroskaHeader(
                (uint16_t)fb->width,
                (uint16_t)fb->height
            )) {

            if (detailedFrameTimingEnabled)
                lastFrameTiming.headerUs =
                    frameTimingElapsedUs(headerStartUs);

            Serial.println(
                "MKV: header write failed"
            );

            writeFailed = true;
            collectAndStopAudioCapture();

            esp_camera_fb_return(fb);

            return;
        }

        if (detailedFrameTimingEnabled)
            lastFrameTiming.headerUs =
                frameTimingElapsedUs(headerStartUs);
    }


    if (
        fb->width != mkvWidth ||
        fb->height != mkvHeight
    ) {

        Serial.println(
            "MKV: frame resolution changed - frame skipped"
        );

        esp_camera_fb_return(fb);

        return;
    }


    uint64_t frameTimeMs =
        (
            (uint64_t)frameCount *
            1000ULL
        ) /
        (uint64_t)mkvFps;


    // Preserve chronological block order around Cluster rotations: drain audio
    // whose block start is strictly before this video timestamp first.
    if (!drainAudioUntil(
            frameTimeMs,
            false
        )) {

        if (!sizeLimitHit)
            writeFailed = true;

        esp_camera_fb_return(fb);
        return;
    }


    uint64_t clusterStartUs =
        frameTimingNowUs();

    if (!ensureClusterForTime(
            frameTimeMs
        )) {

        if (detailedFrameTimingEnabled)
            lastFrameTiming.clusterUs +=
                frameTimingElapsedUs(clusterStartUs);

        writeFailed = true;

        esp_camera_fb_return(fb);

        return;
    }

    if (detailedFrameTimingEnabled)
        lastFrameTiming.clusterUs +=
            frameTimingElapsedUs(clusterStartUs);


    // Subtitle blocks are written before the first video frame
    // whose timestamp reaches that subtitle start time.
    uint64_t subtitleStartUs =
        frameTimingNowUs();

    if (!writePendingSubtitles(
            frameTimeMs
        )) {

        if (detailedFrameTimingEnabled)
            lastFrameTiming.subtitleUs +=
                frameTimingElapsedUs(subtitleStartUs);

        writeFailed = true;

        esp_camera_fb_return(fb);

        return;
    }

    if (detailedFrameTimingEnabled)
        lastFrameTiming.subtitleUs +=
            frameTimingElapsedUs(subtitleStartUs);


    uint32_t jpegSize =
        (uint32_t)fb->len;


    uint64_t videoWriteStartUs =
        frameTimingNowUs();

    if (!writeVideoFrame(
            fb->buf,
            jpegSize,
            frameTimeMs
        )) {

        if (detailedFrameTimingEnabled)
            lastFrameTiming.videoWriteUs +=
                frameTimingElapsedUs(videoWriteStartUs);

        esp_camera_fb_return(fb);

        return;
    }

    if (detailedFrameTimingEnabled)
        lastFrameTiming.videoWriteUs +=
            frameTimingElapsedUs(videoWriteStartUs);


    // Audio starting exactly at this video timestamp can now be emitted
    // without moving Cluster time ahead of a not-yet-written video frame.
    if (!drainAudioUntil(
            frameTimeMs,
            true
        )) {

        if (!sizeLimitHit)
            writeFailed = true;

        esp_camera_fb_return(fb);
        return;
    }


    frameCount++;
    mkvLastFrameTimeMs = frameTimeMs;

    // Same image_only frame tap as AVI. The first written frame is deliberately
    // left untouched so wake/start latency is not inflated by image analysis.
    if (frameCount > 1U) {
        uint64_t imageAnalysisStartUs =
            frameTimingNowUs();

        imageMotionObserveRecordingJpeg(
            fb->buf,
            fb->len,
            (uint16_t)fb->width,
            (uint16_t)fb->height
        );

        if (detailedFrameTimingEnabled)
            lastFrameTiming.imageAnalysisUs +=
                frameTimingElapsedUs(imageAnalysisStartUs);
    }


    if (jpegSize > maxFrameSize) {
        maxFrameSize =
            jpegSize;
    }



    esp_camera_fb_return(fb);
}


// =============================================================
// SPARSE JPEG INPUT (CONTINUOUS SHOOTER)
// =============================================================

bool mkvStartSparseJpeg(
    const String &path,
    uint16_t width,
    uint16_t height,
    time_t startEpoch,
    uint32_t nominalFrameDurationMs
)
{
    if (mkvFile)
        mkvEnd();

    if (width == 0 || height == 0)
        return false;

    mkvPath = path;
    mkvFps = 1;
    frameCount = 0;
    maxFrameSize = 0;
    mkvWidth = 0;
    mkvHeight = 0;
    headerWritten = false;
    writeFailed = false;
    sizeLimitHit = false;
    clusterOpen = false;
    clusterTimestampMs = 0;
    durationPayloadPosition = 0;
    nextSubtitleMs = 0;
    mkvSparseMode = true;
    mkvLastFrameTimeMs = 0;
    mkvSparseTailMs = nominalFrameDurationMs > 0 ? nominalFrameDurationMs : 1U;

    // Sparse shooter MKVs intentionally remain image-only.
    audioRequested = false;
    audioTrackEnabled = false;
    audioCaptureStarted = false;
    audioRuntimeFailed = false;
    audioBytesWritten = 0;
    audioLastEndTimeMs = 0;
    finalAudioStats = {};
    lastWriteBufferStats = {};
    mkvAudioFormat = {0, 0, 0};

    recordingStartEpoch = startEpoch;
    recordingStartTimeValid = startEpoch >= (time_t)1609459200;

    // Sparse shooter timing already lives in the video block timestamps. A
    // per-second subtitle track would defeat sparsity across long rejected gaps.
    subtitleTrackEnabled = false;

    if (STORAGE.exists(path.c_str()))
        STORAGE.remove(path.c_str());

    if (!mkvFile.openWrite(path, cfg_recording_encryption != 0))
        return false;

    if (!writeMatroskaHeader(width, height)) {
        writeFailed = true;
        mkvEnd();
        return false;
    }

    return true;
}

bool mkvAddSparseJpeg(
    const uint8_t *jpegData,
    size_t jpegSize,
    uint16_t width,
    uint16_t height,
    uint64_t relativeTimestampMs
)
{
    if (
        !mkvFile ||
        !mkvSparseMode ||
        writeFailed ||
        sizeLimitHit ||
        !jpegData ||
        jpegSize < 4 ||
        width != mkvWidth ||
        height != mkvHeight
    ) {
        return false;
    }

    if (
        jpegData[0] != 0xFF || jpegData[1] != 0xD8 ||
        jpegData[jpegSize - 2] != 0xFF || jpegData[jpegSize - 1] != 0xD9
    ) {
        return false;
    }

    if (frameCount > 0 && relativeTimestampMs < mkvLastFrameTimeMs)
        return false;

    if (!ensureClusterForTime(relativeTimestampMs)) {
        writeFailed = true;
        return false;
    }

    if (!writeVideoFrame(
            jpegData,
            (uint32_t)jpegSize,
            relativeTimestampMs
        )) {
        return false;
    }

    frameCount++;
    mkvLastFrameTimeMs = relativeTimestampMs;

    if (jpegSize > maxFrameSize)
        maxFrameSize = (uint32_t)jpegSize;

    return true;
}


// =============================================================
// FINISH MKV FILE
// =============================================================

bool mkvEnd()
{
    if (!mkvFile.isOpen())
        return true;

    const bool storageFailed = mkvFile.failed();

    if (
        storageFailed ||
        writeFailed ||
        !headerWritten ||
        frameCount == 0
    ) {

        collectAndStopAudioCapture();

        if (storageFailed || writeFailed) {
            Serial.println(
                "MKV: recording failed - removing incomplete file"
            );
        } else {
            Serial.println(
                "MKV: no frames - removing empty file"
            );
        }


        lastWriteBufferStats =
            mkvFile.stats();

        (void)mkvFile.closeChecked();


        if (mkvPath.length()) {
            STORAGE.remove(
                mkvPath.c_str()
            );
        }


        mkvPath = "";

        return false;
    }


    bool ok = true;


    // Compute the video duration before stopping audio so final buffered PCM
    // can be drained to the same timeline endpoint.
    double videoDurationMs =
        mkvSparseMode
        ? (double)(mkvLastFrameTimeMs + (uint64_t)mkvSparseTailMs)
        : (
            (double)frameCount *
            1000.0
          ) /
          (double)mkvFps;


    if (
        !mkvSparseMode &&
        audioTrackEnabled &&
        audioCaptureStarted
    ) {
        if (!drainAudioToRecordingEnd(
                (uint64_t)videoDurationMs
            )) {
            if (!sizeLimitHit)
                ok = false;
        }

        collectAndStopAudioCapture();
    }


    ok &= closeCluster();


    double durationMs =
        videoDurationMs;

    if (
        audioTrackEnabled &&
        audioLastEndTimeMs > (uint64_t)durationMs
    ) {
        durationMs =
            (double)audioLastEndTimeMs;
    }


    ok &= patchFloat64(
        durationPayloadPosition,
        durationMs
    );


    // Segment payload now has a final known size.
    ok &= endMaster(
        segmentMark
    );


    mkvFile.flush();

    lastWriteBufferStats =
        mkvFile.stats();

    if (mkvFile.failed())
        ok = false;

    if (!mkvFile.closeChecked())
        ok = false;


    if (ok) {

        if (!mkvSparseMode) {
            char summary[448];

            double storageBufferPct =
                lastWriteBufferStats.capacity > 0
                ? (double)lastWriteBufferStats.highWater * 100.0 /
                    (double)lastWriteBufferStats.capacity
                : 0.0;

            snprintf(
                summary,
                sizeof(summary),
                "STOP | MKV | frames=%lu | duration=%.1f s | maxJPEG=%.1f KB%s%s | audioBytes=%llu | audioDropped=%llu | audioBuffer=%u/%u | writeBehind=%s | storageBuffer=%u/%u(%.1f%%) | storageWaits=%lu | storageDrainMax=%.1fms",
                (unsigned long)frameCount,
                (double)(durationMs / 1000.0),
                (double)maxFrameSize / 1024.0,
                subtitleTrackEnabled ? " | subtitles=yes" : "",
                audioTrackEnabled
                    ? (audioRuntimeFailed ? " | audio=partial" : " | audio=yes")
                    : " | audio=no",
                (unsigned long long)audioBytesWritten,
                (unsigned long long)finalAudioStats.bytesDropped,
                (unsigned)finalAudioStats.bufferHighWater,
                (unsigned)finalAudioStats.bufferCapacity,
                lastWriteBufferStats.enabled ? "yes" : "no",
                (unsigned)lastWriteBufferStats.highWater,
                (unsigned)lastWriteBufferStats.capacity,
                storageBufferPct,
                (unsigned long)lastWriteBufferStats.producerWaitCount,
                (double)lastWriteBufferStats.drainWriteMaxUs / 1000.0
            );

            consoleWrite("REC", String(summary));
            logWrite("Recording " + String(summary));

            if (
                audioTrackEnabled &&
                finalAudioStats.bytesDropped > 0
            ) {
                consoleWrite(
                    "REC",
                    "WARN | MKV audio capture dropped " +
                    String((unsigned long)finalAudioStats.bytesDropped) +
                    " bytes; video recording remained active"
                );
            }
        }

    } else {

        consoleWrite(
            "REC",
            "ERROR | MKV finalization"
        );
    }


    bool finalOk =
        ok;

    if (!finalOk && mkvPath.length()) {
        STORAGE.remove(
            mkvPath.c_str()
        );
    }

    mkvPath = "";

    return finalOk;
}


// =============================================================
// STATUS
// =============================================================

void mkvSetDetailedFrameTimingEnabled(bool enabled)
{
    detailedFrameTimingEnabled = enabled;
    lastFrameTiming = {};
}


bool mkvGetLastFrameTiming(MkvFrameTiming &timing)
{
    timing = lastFrameTiming;
    return
        detailedFrameTimingEnabled &&
        lastFrameTiming.valid;
}


bool mkvGetWriteBufferStats(RecordingWriteBufferStats &stats)
{
    if (mkvFile.isOpen()) {
        stats = mkvFile.stats();
    } else {
        stats = lastWriteBufferStats;
    }

    // A synchronous fallback still has useful initialization diagnostics.
    return
        stats.enabled ||
        stats.initStatus != RECORDING_WRITE_BUFFER_NOT_ATTEMPTED;
}


bool mkvWriteBehindEnabled()
{
    return mkvFile.writeBehindEnabled();
}


bool mkvIsOpen()
{
    // Open-state and health-state are deliberately separate. A failed writer
    // still owns an SD handle/resources until mkvEnd() performs cleanup.
    return mkvFile.isOpen();
}

bool mkvIsHealthy()
{
    return
        mkvFile.isOpen() &&
        !mkvFile.failed() &&
        !writeFailed;
}

const char *mkvGetLastError()
{
    return mkvFile.lastError();
}


bool mkvHitSizeLimit()
{
    return sizeLimitHit;
}


uint32_t mkvGetFrameCount()
{
    return frameCount;
}


uint64_t mkvGetBytesWritten()
{
    if (!mkvFile.isOpen())
        return 0;

    // Writers always restore the append position after header/size patches.
    // position() therefore gives the current segment size without a
    // separate filesystem metadata lookup.
    return
        (uint64_t)mkvFile.position();
}
