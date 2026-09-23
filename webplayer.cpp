#include "webplayer.h"

#include "board_config.h"
#include "config.h"
#include "recorder.h"
#include "storage_guard.h"
#include "branding.h"
#include "recording_storage.h"

#include <FS.h>
#include <WiFi.h>
#include <algorithm>
#include <string.h>
#include <vector>


// =============================================================
// WEB RECORDING PLAYER
//
// Supported:
//   AVI / MJPEG
//   MKV / Matroska V_MJPEG
//
// Architecture:
//   - one JPEG frame per HTTP request
//   - no endless HTTP streaming loop
//   - browser controls playback timing
//   - sequential file position is retained between frame requests
//
// Timestamp/subtitle overlay is intentionally the next step.
// =============================================================


static WebServer *playerServer =
    nullptr;


static bool rejectPlayerWhileRecording()
{
    if (
        !playerServer ||
        !recorderIsOpen()
    ) {
        return false;
    }

    playerServer->send(
        409,
        "text/plain; charset=utf-8",
        "Recording active - playback is temporarily unavailable."
    );

    return true;
}


enum PlayerFormat {
    PLAYER_NONE = 0,
    PLAYER_AVI,
    PLAYER_MKV
};


static PlayerFormat playerFormat =
    PLAYER_NONE;


static RecordingStorageFile playerFile;
static File subtitleFile;
static String playerPath;

static bool playerHasSubtitles = false;

static uint32_t playerLastAccessMs = 0;

static const uint32_t PLAYER_IDLE_CLOSE_MS =
    30000UL;


// Deletions requested while a recording is active are queued and
// executed only after recording has been continuously idle for a
// short grace period. This avoids SD directory changes at segment
// boundaries or while a new recording is starting.
static const uint8_t PLAYER_DELETE_QUEUE_MAX = 128;
static const uint8_t PLAYER_DELETE_BATCH_MAX = 16;
static const uint32_t PLAYER_DELETE_SAFE_IDLE_MS = 1500UL;

static String pendingDeletePaths[PLAYER_DELETE_QUEUE_MAX];
static uint8_t pendingDeleteCount = 0;
static uint32_t pendingDeleteSafeSinceMs = 0;


// Shared transfer buffer.
// JPEGs are sent to the browser in small chunks.
static uint8_t transferBuffer[8192];


// Finite batches only. At the project's normal 5 fps this is
// two seconds of video. This reduces HTTP/SD round-trips while keeping
// the synchronous WebServer response bounded.
static const uint8_t PLAYER_BATCH_MAX_FRAMES = 10;


struct BatchFrameRef {
    uint32_t frameNumber;
    uint32_t jpegPosition;
    uint32_t jpegSize;
    uint32_t resumePosition;
    uint32_t timestampMs;
    uint32_t holdMs;
};


// =============================================================
// COMMON META
// =============================================================

struct PlayerMeta {
    uint32_t fps;
    uint32_t frameCount;
    uint32_t width;
    uint32_t height;
    uint64_t durationMs;
    bool variableTiming;
};


static PlayerMeta currentMeta = {
    0, 0, 0, 0, 0, false
};


// =============================================================
// PATH HELPERS
// =============================================================

static bool validRecordingPath(
    const String &path
)
{
    if (!path.length())
        return false;

    if (!path.startsWith("/"))
        return false;

    if (path.indexOf("..") >= 0)
        return false;

    if (
        path.indexOf('\r') >= 0 ||
        path.indexOf('\n') >= 0
    ) {
        return false;
    }

    return true;
}


static bool isAviPath(
    const String &path
)
{
    String lower =
        path;

    lower.toLowerCase();

    return
        lower.endsWith(".avi");
}


static bool isMkvPath(
    const String &path
)
{
    String lower =
        path;

    lower.toLowerCase();

    return
        lower.endsWith(".mkv");
}


static bool isJpegPath(
    const String &path
)
{
    String lower =
        path;

    lower.toLowerCase();

    return
        lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg");
}


static const size_t PLAYER_ANNOTATION_MAX_BYTES = 240U;


static String annotationPathForMedia(
    const String &path
)
{
    return
        path +
        ".note";
}


static String normalizedAnnotationText(
    String text
)
{
    text.replace("\r", " ");
    text.replace("\n", " ");
    text.replace("\t", " ");
    text.trim();

    while (text.indexOf("  ") >= 0)
        text.replace("  ", " ");

    if (text.length() > PLAYER_ANNOTATION_MAX_BYTES)
        text.remove(PLAYER_ANNOTATION_MAX_BYTES);

    return text;
}


static bool loadMediaAnnotation(
    const String &mediaPath,
    String &text
)
{
    text = "";

    String notePath =
        annotationPathForMedia(mediaPath);

    RecordingStorageFile file;

    if (!file.openRead(notePath))
        return !STORAGE.exists(notePath.c_str());

    if (file.isDirectory()) {
        file.close();
        return false;
    }

    while (
        file.available() &&
        text.length() < PLAYER_ANNOTATION_MAX_BYTES
    ) {
        int c = file.read();

        if (c < 0)
            break;

        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';

        text += (char)c;
    }

    file.close();

    text =
        normalizedAnnotationText(text);

    return true;
}


static String playerJsonEscape(
    const String &value
)
{
    String escaped;
    escaped.reserve(
        value.length() + 8
    );

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];

        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"':  escaped += "\\\""; break;
            case '\r': escaped += "\\r"; break;
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if ((uint8_t)c < 0x20U) {
                    char buffer[8];
                    snprintf(
                        buffer,
                        sizeof(buffer),
                        "\\u%04x",
                        (unsigned int)(uint8_t)c
                    );
                    escaped += buffer;
                } else {
                    escaped += c;
                }
                break;
        }
    }

    return escaped;
}


static String normalizedMediaMode(
    String mode
)
{
    mode.toLowerCase();

    if (
        mode != "videos" &&
        mode != "images"
    ) {
        mode = "all";
    }

    return mode;
}


static bool mediaPathMatchesMode(
    const String &path,
    const String &mode
)
{
    if (mode == "videos") {
        return
            isAviPath(path) ||
            isMkvPath(path);
    }

    if (mode == "images")
        return isJpegPath(path);

    return
        isAviPath(path) ||
        isMkvPath(path) ||
        isJpegPath(path);
}


// =============================================================
// GENERIC FILE HELPERS
// =============================================================

static bool readAt(
    RecordingStorageFile &file,
    uint32_t position,
    uint8_t *buffer,
    size_t length
)
{
    if (!file.seek(position))
        return false;

    return
        file.read(
            buffer,
            length
        ) == length;
}


static uint32_t readU32LE(
    const uint8_t *p
)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static bool fourCCEquals(
    const uint8_t *p,
    const char code[5]
)
{
    return
        p[0] == (uint8_t)code[0] &&
        p[1] == (uint8_t)code[1] &&
        p[2] == (uint8_t)code[2] &&
        p[3] == (uint8_t)code[3];
}


// =============================================================
// AVI
// =============================================================

struct AviMeta {
    uint32_t moviDataStart;
};


static AviMeta aviMeta = {
    0
};


// Position of next AVI chunk header.
static uint32_t aviScanPos = 0;

// Number of next AVI video frame.
static uint32_t aviNextFrame = 0;


// -------------------------------------------------------------
// Locate "movi".
// -------------------------------------------------------------

static bool findAviMoviDataStart(
    RecordingStorageFile &file,
    uint32_t &dataStart
)
{
    uint64_t fileSize =
        file.size();

    uint32_t scanLimit =
        fileSize < 65536ULL
        ? (uint32_t)fileSize
        : 65536UL;


    if (scanLimit < 12)
        return false;


    uint8_t window[4] = {
        0, 0, 0, 0
    };


    if (!file.seek(0))
        return false;


    for (
        uint32_t pos = 0;
        pos < scanLimit;
        ++pos
    ) {

        int c =
            file.read();

        if (c < 0)
            break;


        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = (uint8_t)c;


        if (
            pos >= 3 &&
            window[0] == 'm' &&
            window[1] == 'o' &&
            window[2] == 'v' &&
            window[3] == 'i'
        ) {

            dataStart =
                pos + 1U;

            return true;
        }
    }


    return false;
}


// -------------------------------------------------------------
// AVI metadata.
// -------------------------------------------------------------

static bool parseAviMeta(
    RecordingStorageFile &file,
    PlayerMeta &meta,
    AviMeta &containerMeta
)
{
    if (!file)
        return false;


    uint8_t header[72];


    if (!readAt(
            file,
            0,
            header,
            sizeof(header)
        )) {
        return false;
    }


    if (
        !fourCCEquals(
            &header[0],
            "RIFF"
        ) ||
        !fourCCEquals(
            &header[8],
            "AVI "
        )
    ) {
        return false;
    }


    uint32_t microSecPerFrame =
        readU32LE(
            &header[32]
        );

    uint32_t totalFrames =
        readU32LE(
            &header[48]
        );

    uint32_t width =
        readU32LE(
            &header[64]
        );

    uint32_t height =
        readU32LE(
            &header[68]
        );


    uint32_t fps =
        0;


    if (microSecPerFrame > 0) {

        fps =
            (
                1000000UL +
                microSecPerFrame / 2UL
            ) /
            microSecPerFrame;
    }


    if (
        fps < 1 ||
        fps > 60
    ) {
        fps = 5;
    }


    uint32_t moviStart =
        0;


    if (!findAviMoviDataStart(
            file,
            moviStart
        )) {
        return false;
    }


    if (
        width == 0 ||
        height == 0 ||
        totalFrames == 0
    ) {
        return false;
    }


    meta.fps =
        fps;

    meta.frameCount =
        totalFrames;

    meta.width =
        width;

    meta.height =
        height;

    meta.durationMs =
        (
            (uint64_t)totalFrames *
            1000ULL
        ) /
        fps;


    meta.variableTiming = false;


    containerMeta.moviDataStart =
        moviStart;


    return true;
}


static bool resetAviScan()
{
    if (!playerFile)
        return false;

    aviScanPos =
        aviMeta.moviDataStart;

    aviNextFrame =
        0;

    return
        playerFile.seek(
            aviScanPos
        );
}


// -------------------------------------------------------------
// Locate requested AVI JPEG.
// -------------------------------------------------------------

static bool locateAviFrame(
    uint32_t requestedFrame,
    uint32_t &jpegPosition,
    uint32_t &jpegSize,
    uint32_t &resumePosition
)
{
    if (!playerFile)
        return false;


    if (
        requestedFrame >=
        currentMeta.frameCount
    ) {
        return false;
    }


    if (
        requestedFrame <
        aviNextFrame
    ) {

        if (!resetAviScan())
            return false;
    }


    uint32_t fileSize =
        (uint32_t)playerFile.size();


    while (
        aviScanPos + 8U <=
        fileSize
    ) {

        uint8_t chunkHeader[8];


        if (!readAt(
                playerFile,
                aviScanPos,
                chunkHeader,
                sizeof(chunkHeader)
            )) {
            return false;
        }


        uint32_t chunkSize =
            readU32LE(
                &chunkHeader[4]
            );


        uint64_t next64 =
            (uint64_t)aviScanPos +
            8ULL +
            chunkSize +
            (chunkSize & 1U);


        if (
            next64 >
            fileSize
        ) {
            return false;
        }


        uint32_t nextPos =
            (uint32_t)next64;


        bool isVideo =
            fourCCEquals(
                &chunkHeader[0],
                "00dc"
            ) ||
            fourCCEquals(
                &chunkHeader[0],
                "00db"
            );


        if (isVideo) {

            uint32_t thisFrame =
                aviNextFrame;


            aviNextFrame++;


            if (
                thisFrame ==
                requestedFrame
            ) {

                jpegPosition =
                    aviScanPos + 8U;

                jpegSize =
                    chunkSize;

                resumePosition =
                    nextPos;

                aviScanPos =
                    nextPos;


                return true;
            }
        }


        aviScanPos =
            nextPos;
    }


    return false;
}


// =============================================================
// AVI / SRT SUBTITLES
// =============================================================

struct SubtitleCue {
    bool valid;
    uint64_t startMs;
    uint64_t endMs;
    String text;
};


static SubtitleCue currentSubtitleCue = {
    false,
    0,
    0,
    ""
};


static bool subtitleEof =
    false;


// -------------------------------------------------------------
// Parse HH:MM:SS,mmm
// -------------------------------------------------------------

static bool parseSrtTimestamp(
    const String &value,
    uint64_t &ms
)
{
    if (value.length() < 12)
        return false;

    if (
        value[2] != ':' ||
        value[5] != ':' ||
        (
            value[8] != ',' &&
            value[8] != '.'
        )
    ) {
        return false;
    }


    int hours =
        value.substring(0, 2).toInt();

    int minutes =
        value.substring(3, 5).toInt();

    int seconds =
        value.substring(6, 8).toInt();

    int millisPart =
        value.substring(9, 12).toInt();


    if (
        hours < 0 ||
        minutes < 0 ||
        minutes > 59 ||
        seconds < 0 ||
        seconds > 59 ||
        millisPart < 0 ||
        millisPart > 999
    ) {
        return false;
    }


    ms =
        (uint64_t)hours * 3600000ULL +
        (uint64_t)minutes * 60000ULL +
        (uint64_t)seconds * 1000ULL +
        (uint64_t)millisPart;


    return true;
}


// -------------------------------------------------------------
// Read the next standard SRT cue.
//
// We search for a line containing "-->" instead of depending on
// the numeric cue counter. This also tolerates older/missing cue
// numbers.
// -------------------------------------------------------------

static bool readNextSrtCue(
    SubtitleCue &cue
)
{
    if (!subtitleFile)
        return false;


    while (subtitleFile.available()) {

        String timingLine =
            subtitleFile.readStringUntil('\n');

        timingLine.trim();


        int arrow =
            timingLine.indexOf("-->");


        if (arrow < 0)
            continue;


        String startText =
            timingLine.substring(
                0,
                arrow
            );

        String endText =
            timingLine.substring(
                arrow + 3
            );


        startText.trim();
        endText.trim();


        uint64_t startMs =
            0;

        uint64_t endMs =
            0;


        if (
            !parseSrtTimestamp(
                startText,
                startMs
            ) ||
            !parseSrtTimestamp(
                endText,
                endMs
            ) ||
            endMs <= startMs
        ) {
            continue;
        }


        String text;


        while (subtitleFile.available()) {

            String line =
                subtitleFile.readStringUntil('\n');

            line.replace("\r", "");


            if (line.length() == 0)
                break;


            if (text.length())
                text += '\n';

            text +=
                line;
        }


        cue.valid =
            true;

        cue.startMs =
            startMs;

        cue.endMs =
            endMs;

        cue.text =
            text;


        return true;
    }


    subtitleEof =
        true;

    return false;
}


static bool resetSrtScan()
{
    if (!subtitleFile)
        return false;


    if (!subtitleFile.seek(0))
        return false;


    currentSubtitleCue.valid =
        false;

    currentSubtitleCue.startMs =
        0;

    currentSubtitleCue.endMs =
        0;

    currentSubtitleCue.text =
        "";

    subtitleEof =
        false;


    return true;
}


static bool findSrtSubtitle(
    uint64_t requestedMs,
    String &text
)
{
    text =
        "";


    if (!subtitleFile)
        return false;


    if (
        currentSubtitleCue.valid &&
        requestedMs <
            currentSubtitleCue.startMs
    ) {

        if (!resetSrtScan())
            return false;
    }


    if (
        currentSubtitleCue.valid &&
        requestedMs >=
            currentSubtitleCue.startMs &&
        requestedMs <
            currentSubtitleCue.endMs
    ) {

        text =
            currentSubtitleCue.text;

        return
            text.length() > 0;
    }


    while (!subtitleEof) {

        if (!readNextSrtCue(
                currentSubtitleCue
            )) {
            break;
        }


        if (
            requestedMs <
            currentSubtitleCue.startMs
        ) {
            return false;
        }


        if (
            requestedMs <
            currentSubtitleCue.endMs
        ) {

            text =
                currentSubtitleCue.text;

            return
                text.length() > 0;
        }
    }


    return false;
}


// =============================================================
// MATROSKA / EBML
// =============================================================

// EBML IDs used by our Matroska writer.
static const uint32_t MKV_ID_EBML           = 0x1A45DFA3;
static const uint32_t MKV_ID_SEGMENT        = 0x18538067;
static const uint32_t MKV_ID_INFO           = 0x1549A966;
static const uint32_t MKV_ID_TIMESTAMP_SCALE = 0x2AD7B1;
static const uint32_t MKV_ID_DURATION       = 0x4489;
static const uint32_t MKV_ID_DATE_UTC       = 0x4461;

static const uint32_t MKV_ID_TRACKS         = 0x1654AE6B;
static const uint32_t MKV_ID_TRACK_ENTRY    = 0xAE;
static const uint32_t MKV_ID_TRACK_NUMBER   = 0xD7;
static const uint32_t MKV_ID_TRACK_TYPE     = 0x83;
static const uint32_t MKV_ID_CODEC_ID       = 0x86;
static const uint32_t MKV_ID_VIDEO          = 0xE0;
static const uint32_t MKV_ID_PIXEL_WIDTH    = 0xB0;
static const uint32_t MKV_ID_PIXEL_HEIGHT   = 0xBA;

static const uint32_t MKV_ID_CLUSTER        = 0x1F43B675;
static const uint32_t MKV_ID_CLUSTER_TIMESTAMP = 0xE7;
static const uint32_t MKV_ID_SIMPLE_BLOCK   = 0xA3;
static const uint32_t MKV_ID_BLOCK_GROUP    = 0xA0;
static const uint32_t MKV_ID_BLOCK          = 0xA1;
static const uint32_t MKV_ID_BLOCK_DURATION = 0x9B;


struct EbmlElement {
    uint32_t id;
    uint64_t size;
    uint32_t dataStart;
    uint32_t end;
};


struct MkvMeta {
    uint64_t timestampScaleNs;
    uint64_t videoTrackNumber;
    uint64_t subtitleTrackNumber;
    bool hasSubtitleTrack;
    bool hasDateUtc;
    uint32_t startEpochSec;
    uint32_t segmentDataStart;
    uint32_t segmentEnd;
    uint32_t firstClusterPos;
};


static MkvMeta mkvMeta = {
    1000000ULL,
    1,
    0,
    false,
    false,
    0,
    0,
    0,
    0
};


struct MkvScanState {
    uint32_t pos;
    uint32_t clusterEnd;
    uint64_t clusterTimestampTicks;
    uint32_t nextFrame;
};


static MkvScanState mkvScan = {
    0, 0, 0, 0
};


struct MkvSubtitleScanState {
    uint32_t pos;
    uint32_t clusterEnd;
    uint64_t clusterTimestampTicks;
};


static MkvSubtitleScanState mkvSubtitleScan = {
    0, 0, 0
};


struct MkvFrameInfo {
    uint32_t frameNumber;
    uint32_t jpegPosition;
    uint32_t jpegSize;
    uint64_t timestampMs;
    uint32_t resumePosition;
};


// -------------------------------------------------------------
// EBML VINT helpers.
// -------------------------------------------------------------

static uint8_t ebmlVintLength(
    uint8_t firstByte
)
{
    uint8_t mask =
        0x80;


    for (
        uint8_t length = 1;
        length <= 8;
        ++length
    ) {

        if (firstByte & mask)
            return length;

        mask >>= 1;
    }


    return 0;
}


static bool readEbmlIdAt(
    RecordingStorageFile &file,
    uint32_t position,
    uint32_t &id,
    uint8_t &length
)
{
    uint8_t first;


    if (!readAt(
            file,
            position,
            &first,
            1
        )) {
        return false;
    }


    length =
        ebmlVintLength(
            first
        );


    if (
        length < 1 ||
        length > 4
    ) {
        return false;
    }


    uint8_t bytes[4] = {
        0, 0, 0, 0
    };


    if (!readAt(
            file,
            position,
            bytes,
            length
        )) {
        return false;
    }


    id =
        0;


    for (
        uint8_t i = 0;
        i < length;
        ++i
    ) {

        id =
            (id << 8) |
            bytes[i];
    }


    return true;
}


static bool readEbmlVintValueAt(
    RecordingStorageFile &file,
    uint32_t position,
    uint64_t &value,
    uint8_t &length
)
{
    uint8_t first;


    if (!readAt(
            file,
            position,
            &first,
            1
        )) {
        return false;
    }


    length =
        ebmlVintLength(
            first
        );


    if (
        length < 1 ||
        length > 8
    ) {
        return false;
    }


    uint8_t markerMask =
        (uint8_t)(
            0x80U >>
            (length - 1U)
        );


    value =
        (uint64_t)(
            first &
            (markerMask - 1U)
        );


    for (
        uint8_t i = 1;
        i < length;
        ++i
    ) {

        uint8_t byteValue;


        if (!readAt(
                file,
                position + i,
                &byteValue,
                1
            )) {
            return false;
        }


        value =
            (value << 8) |
            byteValue;
    }


    return true;
}


static bool readEbmlElementAt(
    RecordingStorageFile &file,
    uint32_t position,
    EbmlElement &element
)
{
    uint32_t id =
        0;

    uint8_t idLength =
        0;


    if (!readEbmlIdAt(
            file,
            position,
            id,
            idLength
        )) {
        return false;
    }


    uint64_t size =
        0;

    uint8_t sizeLength =
        0;


    if (!readEbmlVintValueAt(
            file,
            position + idLength,
            size,
            sizeLength
        )) {
        return false;
    }


    uint64_t dataStart64 =
        (uint64_t)position +
        idLength +
        sizeLength;

    uint64_t end64 =
        dataStart64 +
        size;


    uint64_t fileSize =
        file.size();


    if (
        dataStart64 >
        0xFFFFFFFFULL ||
        end64 >
        0xFFFFFFFFULL ||
        end64 >
        fileSize
    ) {
        return false;
    }


    element.id =
        id;

    element.size =
        size;

    element.dataStart =
        (uint32_t)dataStart64;

    element.end =
        (uint32_t)end64;


    return true;
}


static bool readEbmlUnsigned(
    RecordingStorageFile &file,
    const EbmlElement &element,
    uint64_t &value
)
{
    if (
        element.size < 1 ||
        element.size > 8
    ) {
        return false;
    }


    uint8_t bytes[8];


    if (!readAt(
            file,
            element.dataStart,
            bytes,
            (size_t)element.size
        )) {
        return false;
    }


    value =
        0;


    for (
        uint8_t i = 0;
        i < (uint8_t)element.size;
        ++i
    ) {

        value =
            (value << 8) |
            bytes[i];
    }


    return true;
}


static bool readEbmlSigned(
    RecordingStorageFile &file,
    const EbmlElement &element,
    int64_t &value
)
{
    uint64_t raw = 0;

    if (!readEbmlUnsigned(
            file,
            element,
            raw
        )) {
        return false;
    }

    const uint8_t bits =
        (uint8_t)element.size * 8U;

    if (bits < 64U) {
        const uint64_t signBit =
            1ULL << (bits - 1U);

        if (raw & signBit) {
            raw |=
                (~0ULL) << bits;
        }
    }

    value =
        (int64_t)raw;

    return true;
}


static bool readEbmlFloat(
    RecordingStorageFile &file,
    const EbmlElement &element,
    double &value
)
{
    if (element.size == 8) {

        uint8_t bytes[8];


        if (!readAt(
                file,
                element.dataStart,
                bytes,
                8
            )) {
            return false;
        }


        uint64_t bits =
            0;


        for (
            uint8_t i = 0;
            i < 8;
            ++i
        ) {

            bits =
                (bits << 8) |
                bytes[i];
        }


        memcpy(
            &value,
            &bits,
            sizeof(value)
        );


        return true;
    }


    if (element.size == 4) {

        uint8_t bytes[4];


        if (!readAt(
                file,
                element.dataStart,
                bytes,
                4
            )) {
            return false;
        }


        uint32_t bits =
            0;


        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {

            bits =
                (bits << 8) |
                bytes[i];
        }


        float floatValue;


        memcpy(
            &floatValue,
            &bits,
            sizeof(floatValue)
        );


        value =
            floatValue;


        return true;
    }


    return false;
}


static bool readEbmlString(
    RecordingStorageFile &file,
    const EbmlElement &element,
    String &value
)
{
    if (element.size > 64)
        return false;


    char buffer[65];


    size_t length =
        (size_t)element.size;


    if (
        length > 0 &&
        !readAt(
            file,
            element.dataStart,
            (uint8_t *)buffer,
            length
        )
    ) {
        return false;
    }


    buffer[length] =
        '\0';

    value =
        String(buffer);


    return true;
}


// -------------------------------------------------------------
// Parse Matroska Info.
// -------------------------------------------------------------

static bool parseMkvInfo(
    RecordingStorageFile &file,
    const EbmlElement &info,
    MkvMeta &containerMeta,
    double &durationTicks
)
{
    uint32_t pos =
        info.dataStart;


    while (
        pos < info.end
    ) {

        EbmlElement child;


        if (!readEbmlElementAt(
                file,
                pos,
                child
            )) {
            return false;
        }


        if (
            child.end <= pos ||
            child.end > info.end
        ) {
            return false;
        }


        if (
            child.id ==
            MKV_ID_TIMESTAMP_SCALE
        ) {

            uint64_t scale;


            if (readEbmlUnsigned(
                    file,
                    child,
                    scale
                ) &&
                scale > 0
            ) {

                containerMeta.timestampScaleNs =
                    scale;
            }

        } else if (
            child.id ==
            MKV_ID_DURATION
        ) {

            double duration;


            if (readEbmlFloat(
                    file,
                    child,
                    duration
                )) {

                durationTicks =
                    duration;
            }

        } else if (
            child.id ==
            MKV_ID_DATE_UTC
        ) {

            int64_t dateUtcNs = 0;

            if (readEbmlSigned(
                    file,
                    child,
                    dateUtcNs
                )) {

                // Matroska DateUTC is signed nanoseconds since
                // 2001-01-01T00:00:00Z. SensorForge writes it at the
                // wall-clock time of the first frame. The frame-relative
                // timestamps then reconstruct each exact capture time.
                const int64_t unixToMatroskaEpoch =
                    978307200LL;

                int64_t unixSeconds =
                    unixToMatroskaEpoch +
                    dateUtcNs / 1000000000LL;

                if (
                    unixSeconds > 0 &&
                    unixSeconds <= 0xFFFFFFFFLL
                ) {
                    containerMeta.hasDateUtc =
                        true;

                    containerMeta.startEpochSec =
                        (uint32_t)unixSeconds;
                }
            }
        }


        pos =
            child.end;
    }


    return true;
}


// -------------------------------------------------------------
// Parse one Video element.
// -------------------------------------------------------------

static bool parseMkvVideoElement(
    RecordingStorageFile &file,
    const EbmlElement &video,
    uint32_t &width,
    uint32_t &height
)
{
    uint32_t pos =
        video.dataStart;


    while (
        pos < video.end
    ) {

        EbmlElement child;


        if (!readEbmlElementAt(
                file,
                pos,
                child
            )) {
            return false;
        }


        if (
            child.end <= pos ||
            child.end > video.end
        ) {
            return false;
        }


        uint64_t value;


        if (
            child.id ==
            MKV_ID_PIXEL_WIDTH
        ) {

            if (readEbmlUnsigned(
                    file,
                    child,
                    value
                )) {

                width =
                    (uint32_t)value;
            }

        } else if (
            child.id ==
            MKV_ID_PIXEL_HEIGHT
        ) {

            if (readEbmlUnsigned(
                    file,
                    child,
                    value
                )) {

                height =
                    (uint32_t)value;
            }
        }


        pos =
            child.end;
    }


    return true;
}


// -------------------------------------------------------------
// Parse Tracks and locate V_MJPEG video track.
// -------------------------------------------------------------

static bool parseMkvTracks(
    RecordingStorageFile &file,
    const EbmlElement &tracks,
    MkvMeta &containerMeta,
    uint32_t &width,
    uint32_t &height
)
{
    uint32_t pos =
        tracks.dataStart;

    bool videoFound =
        false;


    while (
        pos < tracks.end
    ) {

        EbmlElement entry;


        if (!readEbmlElementAt(
                file,
                pos,
                entry
            )) {
            return false;
        }


        if (
            entry.end <= pos ||
            entry.end > tracks.end
        ) {
            return false;
        }


        if (
            entry.id ==
            MKV_ID_TRACK_ENTRY
        ) {

            uint64_t trackNumber =
                0;

            uint64_t trackType =
                0;

            String codecId;

            uint32_t entryWidth =
                0;

            uint32_t entryHeight =
                0;


            uint32_t childPos =
                entry.dataStart;


            while (
                childPos <
                entry.end
            ) {

                EbmlElement child;


                if (!readEbmlElementAt(
                        file,
                        childPos,
                        child
                    )) {
                    return false;
                }


                if (
                    child.end <= childPos ||
                    child.end > entry.end
                ) {
                    return false;
                }


                if (
                    child.id ==
                    MKV_ID_TRACK_NUMBER
                ) {

                    readEbmlUnsigned(
                        file,
                        child,
                        trackNumber
                    );

                } else if (
                    child.id ==
                    MKV_ID_TRACK_TYPE
                ) {

                    readEbmlUnsigned(
                        file,
                        child,
                        trackType
                    );

                } else if (
                    child.id ==
                    MKV_ID_CODEC_ID
                ) {

                    readEbmlString(
                        file,
                        child,
                        codecId
                    );

                } else if (
                    child.id ==
                    MKV_ID_VIDEO
                ) {

                    if (!parseMkvVideoElement(
                            file,
                            child,
                            entryWidth,
                            entryHeight
                        )) {
                        return false;
                    }
                }


                childPos =
                    child.end;
            }


            if (
                trackType == 1 &&
                codecId == "V_MJPEG" &&
                trackNumber > 0
            ) {

                containerMeta.videoTrackNumber =
                    trackNumber;

                width =
                    entryWidth;

                height =
                    entryHeight;

                videoFound =
                    true;
            }


            if (
                trackType == 17 &&
                codecId == "S_TEXT/UTF8" &&
                trackNumber > 0
            ) {

                containerMeta.subtitleTrackNumber =
                    trackNumber;

                containerMeta.hasSubtitleTrack =
                    true;
            }
        }


        pos =
            entry.end;
    }


    return
        videoFound;
}


// -------------------------------------------------------------
// Initialize Matroska scanning.
// -------------------------------------------------------------

static void initMkvScan(
    MkvScanState &state,
    uint32_t firstClusterPos
)
{
    state.pos =
        firstClusterPos;

    state.clusterEnd =
        0;

    state.clusterTimestampTicks =
        0;

    state.nextFrame =
        0;
}


// -------------------------------------------------------------
// Parse next Track-1 MJPEG SimpleBlock.
// -------------------------------------------------------------

static bool nextMkvVideoFrame(
    RecordingStorageFile &file,
    const MkvMeta &containerMeta,
    MkvScanState &state,
    MkvFrameInfo &frame
)
{
    uint32_t fileSize =
        (uint32_t)file.size();


    while (
        state.pos < fileSize &&
        state.pos <
            containerMeta.segmentEnd
    ) {

        // Outside a Cluster: find the next Cluster.
        if (state.clusterEnd == 0) {

            EbmlElement topLevel;


            if (!readEbmlElementAt(
                    file,
                    state.pos,
                    topLevel
                )) {
                return false;
            }


            if (
                topLevel.end <=
                state.pos
            ) {
                return false;
            }


            if (
                topLevel.id ==
                MKV_ID_CLUSTER
            ) {

                state.clusterEnd =
                    topLevel.end;

                state.clusterTimestampTicks =
                    0;

                state.pos =
                    topLevel.dataStart;


                continue;
            }


            state.pos =
                topLevel.end;


            continue;
        }


        // End of current Cluster.
        if (
            state.pos >=
            state.clusterEnd
        ) {

            state.pos =
                state.clusterEnd;

            state.clusterEnd =
                0;


            continue;
        }


        EbmlElement child;


        if (!readEbmlElementAt(
                file,
                state.pos,
                child
            )) {
            return false;
        }


        if (
            child.end <=
            state.pos ||
            child.end >
            state.clusterEnd
        ) {
            return false;
        }


        if (
            child.id ==
            MKV_ID_CLUSTER_TIMESTAMP
        ) {

            uint64_t timestampTicks;


            if (!readEbmlUnsigned(
                    file,
                    child,
                    timestampTicks
                )) {
                return false;
            }


            state.clusterTimestampTicks =
                timestampTicks;

            state.pos =
                child.end;


            continue;
        }


        if (
            child.id ==
            MKV_ID_SIMPLE_BLOCK
        ) {

            uint64_t trackNumber =
                0;

            uint8_t trackVintLength =
                0;


            if (!readEbmlVintValueAt(
                    file,
                    child.dataStart,
                    trackNumber,
                    trackVintLength
                )) {
                return false;
            }


            if (
                child.size <
                (uint64_t)trackVintLength +
                3ULL
            ) {
                return false;
            }


            uint8_t blockHeader[3];


            if (!readAt(
                    file,
                    child.dataStart +
                        trackVintLength,
                    blockHeader,
                    sizeof(blockHeader)
                )) {
                return false;
            }


            int16_t relativeTimestamp =
                (int16_t)(
                    (
                        (uint16_t)blockHeader[0]
                        << 8
                    ) |
                    blockHeader[1]
                );


            uint8_t flags =
                blockHeader[2];


            // Our writer disables lacing.
            bool laced =
                (flags & 0x06U) != 0;


            if (
                trackNumber ==
                    containerMeta.videoTrackNumber &&
                !laced
            ) {

                int64_t absoluteTicks =
                    (int64_t)
                        state.clusterTimestampTicks +
                    relativeTimestamp;


                if (absoluteTicks < 0)
                    absoluteTicks = 0;


                uint64_t timestampNs =
                    (uint64_t)absoluteTicks *
                    containerMeta.timestampScaleNs;


                frame.frameNumber =
                    state.nextFrame;

                frame.jpegPosition =
                    child.dataStart +
                    trackVintLength +
                    3U;

                frame.jpegSize =
                    (uint32_t)(
                        child.size -
                        trackVintLength -
                        3U
                    );

                frame.timestampMs =
                    timestampNs /
                    1000000ULL;

                frame.resumePosition =
                    child.end;


                state.nextFrame++;

                state.pos =
                    child.end;


                return true;
            }
        }


        state.pos =
            child.end;
    }


    return false;
}


// -------------------------------------------------------------
// Initialize Matroska subtitle scanning.
// -------------------------------------------------------------

static void initMkvSubtitleScan(
    MkvSubtitleScanState &state,
    uint32_t firstClusterPos
)
{
    state.pos =
        firstClusterPos;

    state.clusterEnd =
        0;

    state.clusterTimestampTicks =
        0;
}


// -------------------------------------------------------------
// Parse one subtitle BlockGroup.
//
// Our writer stores S_TEXT/UTF8 as:
//   BlockGroup
//     Block          (track 2, timestamp, UTF-8 text)
//     BlockDuration
// -------------------------------------------------------------

static bool parseMkvSubtitleGroup(
    RecordingStorageFile &file,
    const MkvMeta &containerMeta,
    const EbmlElement &group,
    uint64_t clusterTimestampTicks,
    SubtitleCue &cue
)
{
    uint32_t pos =
        group.dataStart;


    bool matchingBlock =
        false;

    int16_t relativeTimestamp =
        0;

    uint32_t textPosition =
        0;

    uint32_t textLength =
        0;

    uint64_t durationTicks =
        0;

    bool durationFound =
        false;


    while (pos < group.end) {

        EbmlElement child;


        if (!readEbmlElementAt(
                file,
                pos,
                child
            )) {
            return false;
        }


        if (
            child.end <= pos ||
            child.end > group.end
        ) {
            return false;
        }


        if (
            child.id ==
            MKV_ID_BLOCK
        ) {

            uint64_t trackNumber =
                0;

            uint8_t trackVintLength =
                0;


            if (!readEbmlVintValueAt(
                    file,
                    child.dataStart,
                    trackNumber,
                    trackVintLength
                )) {
                return false;
            }


            if (
                child.size <
                (uint64_t)trackVintLength +
                3ULL
            ) {
                return false;
            }


            uint8_t blockHeader[3];


            if (!readAt(
                    file,
                    child.dataStart +
                        trackVintLength,
                    blockHeader,
                    sizeof(blockHeader)
                )) {
                return false;
            }


            uint8_t flags =
                blockHeader[2];

            bool laced =
                (flags & 0x06U) != 0;


            if (
                trackNumber ==
                    containerMeta.subtitleTrackNumber &&
                !laced
            ) {

                relativeTimestamp =
                    (int16_t)(
                        (
                            (uint16_t)blockHeader[0]
                            << 8
                        ) |
                        blockHeader[1]
                    );


                textPosition =
                    child.dataStart +
                    trackVintLength +
                    3U;


                textLength =
                    (uint32_t)(
                        child.size -
                        trackVintLength -
                        3U
                    );


                matchingBlock =
                    true;
            }

        } else if (
            child.id ==
            MKV_ID_BLOCK_DURATION
        ) {

            if (readEbmlUnsigned(
                    file,
                    child,
                    durationTicks
                )) {

                durationFound =
                    true;
            }
        }


        pos =
            child.end;
    }


    if (
        !matchingBlock ||
        !durationFound
    ) {
        return false;
    }


    // Timestamps and BlockDuration are expressed in Matroska
    // ticks. Our writer uses TimestampScale = 1 ms.
    int64_t absoluteTicks =
        (int64_t)clusterTimestampTicks +
        relativeTimestamp;


    if (absoluteTicks < 0)
        absoluteTicks = 0;


    uint64_t startNs =
        (uint64_t)absoluteTicks *
        containerMeta.timestampScaleNs;


    uint64_t durationNs =
        durationTicks *
        containerMeta.timestampScaleNs;


    cue.valid =
        true;

    cue.startMs =
        startNs /
        1000000ULL;

    cue.endMs =
        cue.startMs +
        durationNs /
        1000000ULL;


    if (
        cue.endMs <=
        cue.startMs
    ) {
        cue.endMs =
            cue.startMs + 1;
    }


    cue.text =
        "";


    // Timestamp strings are tiny, but keep a defensive upper
    // bound in case a different S_TEXT/UTF8 file is opened.
    if (textLength > 1024U)
        return false;


    cue.text.reserve(
        textLength
    );


    uint32_t remaining =
        textLength;

    uint32_t readPos =
        textPosition;

    uint8_t buffer[128];


    while (remaining > 0) {

        size_t chunk =
            remaining <
                sizeof(buffer)
            ? remaining
            : sizeof(buffer);


        if (!readAt(
                file,
                readPos,
                buffer,
                chunk
            )) {
            return false;
        }


        for (
            size_t i = 0;
            i < chunk;
            ++i
        ) {
            cue.text +=
                (char)buffer[i];
        }


        readPos +=
            (uint32_t)chunk;

        remaining -=
            (uint32_t)chunk;
    }


    return true;
}


// -------------------------------------------------------------
// Read next S_TEXT/UTF8 cue from Matroska.
// -------------------------------------------------------------

static bool readNextMkvSubtitle(
    SubtitleCue &cue
)
{
    if (
        !playerFile ||
        !mkvMeta.hasSubtitleTrack
    ) {
        return false;
    }


    uint32_t fileSize =
        (uint32_t)playerFile.size();


    while (
        mkvSubtitleScan.pos < fileSize &&
        mkvSubtitleScan.pos <
            mkvMeta.segmentEnd
    ) {

        if (
            mkvSubtitleScan.clusterEnd ==
            0
        ) {

            EbmlElement topLevel;


            if (!readEbmlElementAt(
                    playerFile,
                    mkvSubtitleScan.pos,
                    topLevel
                )) {
                return false;
            }


            if (
                topLevel.end <=
                mkvSubtitleScan.pos
            ) {
                return false;
            }


            if (
                topLevel.id ==
                MKV_ID_CLUSTER
            ) {

                mkvSubtitleScan.clusterEnd =
                    topLevel.end;

                mkvSubtitleScan.clusterTimestampTicks =
                    0;

                mkvSubtitleScan.pos =
                    topLevel.dataStart;


                continue;
            }


            mkvSubtitleScan.pos =
                topLevel.end;


            continue;
        }


        if (
            mkvSubtitleScan.pos >=
            mkvSubtitleScan.clusterEnd
        ) {

            mkvSubtitleScan.pos =
                mkvSubtitleScan.clusterEnd;

            mkvSubtitleScan.clusterEnd =
                0;


            continue;
        }


        EbmlElement child;


        if (!readEbmlElementAt(
                playerFile,
                mkvSubtitleScan.pos,
                child
            )) {
            return false;
        }


        if (
            child.end <=
                mkvSubtitleScan.pos ||
            child.end >
                mkvSubtitleScan.clusterEnd
        ) {
            return false;
        }


        if (
            child.id ==
            MKV_ID_CLUSTER_TIMESTAMP
        ) {

            uint64_t timestampTicks =
                0;


            if (!readEbmlUnsigned(
                    playerFile,
                    child,
                    timestampTicks
                )) {
                return false;
            }


            mkvSubtitleScan.clusterTimestampTicks =
                timestampTicks;

            mkvSubtitleScan.pos =
                child.end;


            continue;
        }


        if (
            child.id ==
            MKV_ID_BLOCK_GROUP
        ) {

            SubtitleCue candidate;


            bool found =
                parseMkvSubtitleGroup(
                    playerFile,
                    mkvMeta,
                    child,
                    mkvSubtitleScan.clusterTimestampTicks,
                    candidate
                );


            mkvSubtitleScan.pos =
                child.end;


            if (found) {

                cue =
                    candidate;

                return true;
            }


            continue;
        }


        mkvSubtitleScan.pos =
            child.end;
    }


    subtitleEof =
        true;

    return false;
}


static bool resetMkvSubtitleScan()
{
    if (
        !playerFile ||
        !mkvMeta.hasSubtitleTrack
    ) {
        return false;
    }


    initMkvSubtitleScan(
        mkvSubtitleScan,
        mkvMeta.firstClusterPos
    );


    currentSubtitleCue.valid =
        false;

    currentSubtitleCue.startMs =
        0;

    currentSubtitleCue.endMs =
        0;

    currentSubtitleCue.text =
        "";

    subtitleEof =
        false;


    return
        playerFile.seek(
            mkvSubtitleScan.pos
        );
}


static bool findMkvSubtitle(
    uint64_t requestedMs,
    String &text
)
{
    text =
        "";


    if (
        !playerFile ||
        !mkvMeta.hasSubtitleTrack
    ) {
        return false;
    }


    if (
        currentSubtitleCue.valid &&
        requestedMs <
            currentSubtitleCue.startMs
    ) {

        if (!resetMkvSubtitleScan())
            return false;
    }


    if (
        currentSubtitleCue.valid &&
        requestedMs >=
            currentSubtitleCue.startMs &&
        requestedMs <
            currentSubtitleCue.endMs
    ) {

        text =
            currentSubtitleCue.text;

        return
            text.length() > 0;
    }


    while (!subtitleEof) {

        if (!readNextMkvSubtitle(
                currentSubtitleCue
            )) {
            break;
        }


        if (
            requestedMs <
            currentSubtitleCue.startMs
        ) {
            return false;
        }


        if (
            requestedMs <
            currentSubtitleCue.endMs
        ) {

            text =
                currentSubtitleCue.text;

            return
                text.length() > 0;
        }
    }


    return false;
}


// -------------------------------------------------------------
// Parse Matroska metadata and estimate FPS from first 2 frames.
//
// Our writer stores Duration in Segment Ticks and uses exact
// frame timestamps, so frameCount can be recovered from
// Duration * FPS.
// -------------------------------------------------------------

static bool parseMkvMeta(
    RecordingStorageFile &file,
    PlayerMeta &meta,
    MkvMeta &containerMeta
)
{
    containerMeta.timestampScaleNs =
        1000000ULL;

    containerMeta.videoTrackNumber =
        1;

    containerMeta.subtitleTrackNumber =
        0;

    containerMeta.hasSubtitleTrack =
        false;

    containerMeta.hasDateUtc =
        false;

    containerMeta.startEpochSec =
        0;

    containerMeta.segmentDataStart =
        0;

    containerMeta.segmentEnd =
        0;

    containerMeta.firstClusterPos =
        0;


    uint32_t fileSize =
        (uint32_t)file.size();


    if (fileSize < 32)
        return false;


    uint32_t pos =
        0;

    EbmlElement segment;

    bool segmentFound =
        false;


    while (
        pos < fileSize
    ) {

        EbmlElement element;


        if (!readEbmlElementAt(
                file,
                pos,
                element
            )) {
            return false;
        }


        if (
            element.end <= pos
        ) {
            return false;
        }


        if (
            element.id ==
            MKV_ID_SEGMENT
        ) {

            segment =
                element;

            segmentFound =
                true;

            break;
        }


        pos =
            element.end;
    }


    if (!segmentFound)
        return false;


    containerMeta.segmentDataStart =
        segment.dataStart;

    containerMeta.segmentEnd =
        segment.end;


    double durationTicks =
        0.0;

    uint32_t width =
        0;

    uint32_t height =
        0;

    bool infoFound =
        false;

    bool tracksFound =
        false;


    pos =
        segment.dataStart;


    while (
        pos < segment.end
    ) {

        EbmlElement child;


        if (!readEbmlElementAt(
                file,
                pos,
                child
            )) {
            return false;
        }


        if (
            child.end <= pos ||
            child.end > segment.end
        ) {
            return false;
        }


        if (
            child.id ==
            MKV_ID_INFO
        ) {

            if (!parseMkvInfo(
                    file,
                    child,
                    containerMeta,
                    durationTicks
                )) {
                return false;
            }


            infoFound =
                true;

        } else if (
            child.id ==
            MKV_ID_TRACKS
        ) {

            if (!parseMkvTracks(
                    file,
                    child,
                    containerMeta,
                    width,
                    height
                )) {
                return false;
            }


            tracksFound =
                true;

        } else if (
            child.id ==
            MKV_ID_CLUSTER
        ) {

            containerMeta.firstClusterPos =
                pos;

            break;
        }


        pos =
            child.end;
    }


    if (
        !infoFound ||
        !tracksFound ||
        containerMeta.firstClusterPos == 0 ||
        width == 0 ||
        height == 0 ||
        durationTicks <= 0.0
    ) {
        return false;
    }


    MkvScanState probe;

    initMkvScan(
        probe,
        containerMeta.firstClusterPos
    );


    MkvFrameInfo firstFrame;
    MkvFrameInfo secondFrame;


    if (!nextMkvVideoFrame(
            file,
            containerMeta,
            probe,
            firstFrame
        )) {
        return false;
    }


    uint32_t fps = 5;

    if (nextMkvVideoFrame(file, containerMeta, probe, secondFrame)) {
        uint64_t deltaMs = secondFrame.timestampMs - firstFrame.timestampMs;
        if (deltaMs > 0) {
            uint32_t estimatedFps =
                (uint32_t)((1000ULL + deltaMs / 2ULL) / deltaMs);
            if (estimatedFps >= 1 && estimatedFps <= 60)
                fps = estimatedFps;
        }
    }

    double durationNs =
        durationTicks * (double)containerMeta.timestampScaleNs;
    uint64_t durationMs =
        (uint64_t)(durationNs / 1000000.0 + 0.5);

    // Sparse shooter MKVs intentionally use irregular timestamps. Count the
    // actual video blocks rather than inferring frame count from Duration*FPS.
    MkvScanState countScan;
    initMkvScan(countScan, containerMeta.firstClusterPos);
    MkvFrameInfo countedFrame;
    uint32_t actualFrameCount = 0;
    bool variableTiming = false;
    uint64_t previousTimestampMs = 0;
    bool havePreviousTimestamp = false;
    uint64_t nominalStepMs =
        fps > 0
        ? max(1ULL, 1000ULL / (uint64_t)fps)
        : 1ULL;

    while (nextMkvVideoFrame(file, containerMeta, countScan, countedFrame)) {
        if (actualFrameCount == 0xFFFFFFFFUL)
            return false;

        if (havePreviousTimestamp) {
            uint64_t deltaMs =
                countedFrame.timestampMs >= previousTimestampMs
                ? countedFrame.timestampMs - previousTimestampMs
                : 0;
            uint64_t difference =
                deltaMs > nominalStepMs
                ? deltaMs - nominalStepMs
                : nominalStepMs - deltaMs;
            if (difference > 2ULL)
                variableTiming = true;
        }

        previousTimestampMs = countedFrame.timestampMs;
        havePreviousTimestamp = true;
        actualFrameCount++;

        if ((actualFrameCount & 0x3FU) == 0U)
            yield();
    }

    if (actualFrameCount == 0)
        return false;

    meta.fps = fps;
    meta.frameCount = actualFrameCount;
    meta.width = width;
    meta.height = height;
    meta.durationMs = durationMs;
    meta.variableTiming = variableTiming;

    return true;
}



// -------------------------------------------------------------
// Lightweight duration probe used by the recording list.
//
// Unlike parseMkvMeta(), the MKV path deliberately stops after the Info
// element and does NOT count all sparse video blocks. This keeps the day list
// responsive even when a sparse shooter MKV spans many minutes.
// -------------------------------------------------------------

static bool probeAviDurationMs(
    RecordingStorageFile &file,
    uint64_t &durationMs
)
{
    durationMs = 0;

    uint8_t header[72];

    if (!readAt(
            file,
            0,
            header,
            sizeof(header)
        )) {
        return false;
    }

    if (
        !fourCCEquals(&header[0], "RIFF") ||
        !fourCCEquals(&header[8], "AVI ")
    ) {
        return false;
    }

    uint32_t microSecPerFrame =
        readU32LE(&header[32]);

    uint32_t totalFrames =
        readU32LE(&header[48]);

    if (
        microSecPerFrame == 0 ||
        totalFrames == 0
    ) {
        return false;
    }

    durationMs =
        (
            (uint64_t)microSecPerFrame *
            (uint64_t)totalFrames +
            999ULL
        ) /
        1000ULL;

    return durationMs > 0;
}


static bool probeMkvDurationMs(
    RecordingStorageFile &file,
    uint64_t &durationMs
)
{
    durationMs = 0;

    uint32_t fileSize =
        (uint32_t)file.size();

    if (fileSize < 32U)
        return false;

    uint32_t pos = 0;
    EbmlElement segment;
    bool segmentFound = false;

    while (pos < fileSize) {
        EbmlElement element;

        if (!readEbmlElementAt(
                file,
                pos,
                element
            )) {
            return false;
        }

        if (element.end <= pos)
            return false;

        if (element.id == MKV_ID_SEGMENT) {
            segment = element;
            segmentFound = true;
            break;
        }

        pos = element.end;
    }

    if (!segmentFound)
        return false;

    MkvMeta containerMeta;
    containerMeta.timestampScaleNs = 1000000ULL;
    containerMeta.videoTrackNumber = 1;
    containerMeta.subtitleTrackNumber = 0;
    containerMeta.hasSubtitleTrack = false;
    containerMeta.hasDateUtc = false;
    containerMeta.startEpochSec = 0;
    containerMeta.segmentDataStart = segment.dataStart;
    containerMeta.segmentEnd = segment.end;
    containerMeta.firstClusterPos = 0;

    double durationTicks = 0.0;

    pos = segment.dataStart;

    while (pos < segment.end) {
        EbmlElement child;

        if (!readEbmlElementAt(
                file,
                pos,
                child
            )) {
            return false;
        }

        if (
            child.end <= pos ||
            child.end > segment.end
        ) {
            return false;
        }

        if (child.id == MKV_ID_INFO) {
            if (!parseMkvInfo(
                    file,
                    child,
                    containerMeta,
                    durationTicks
                )) {
                return false;
            }

            if (durationTicks <= 0.0)
                return false;

            double durationNs =
                durationTicks *
                (double)containerMeta.timestampScaleNs;

            durationMs =
                (uint64_t)(
                    durationNs /
                    1000000.0 +
                    0.5
                );

            return durationMs > 0;
        }

        // Info is written before the first Cluster by SensorForge. There is no
        // reason to walk JPEG payloads when the metadata is absent/corrupt.
        if (child.id == MKV_ID_CLUSTER)
            break;

        pos = child.end;
    }

    return false;
}


bool webPlayerProbeDurationMs(
    const String &path,
    uint64_t &durationMs
)
{
    durationMs = 0;

    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {
        return false;
    }

    RecordingStorageFile file;

    if (
        !file.openRead(path) ||
        file.isDirectory()
    ) {
        file.close();
        return false;
    }

    bool ok =
        isAviPath(path)
        ? probeAviDurationMs(
            file,
            durationMs
        )
        : probeMkvDurationMs(
            file,
            durationMs
        );

    file.close();

    return ok;
}


static bool resetMkvScan()
{
    if (!playerFile)
        return false;


    initMkvScan(
        mkvScan,
        mkvMeta.firstClusterPos
    );


    return
        playerFile.seek(
            mkvScan.pos
        );
}


// -------------------------------------------------------------
// Locate requested MKV JPEG.
// -------------------------------------------------------------

static bool locateMkvFrame(
    uint32_t requestedFrame,
    uint32_t &jpegPosition,
    uint32_t &jpegSize,
    uint32_t &resumePosition,
    uint64_t *timestampMs = nullptr
)
{
    if (!playerFile)
        return false;


    if (
        requestedFrame >=
        currentMeta.frameCount
    ) {
        return false;
    }


    if (
        requestedFrame <
        mkvScan.nextFrame
    ) {

        if (!resetMkvScan())
            return false;
    }


    MkvFrameInfo frame;


    while (
        nextMkvVideoFrame(
            playerFile,
            mkvMeta,
            mkvScan,
            frame
        )
    ) {

        if (
            frame.frameNumber ==
            requestedFrame
        ) {

            jpegPosition =
                frame.jpegPosition;

            jpegSize =
                frame.jpegSize;

            resumePosition =
                frame.resumePosition;

            if (timestampMs)
                *timestampMs = frame.timestampMs;


            return true;
        }


        if (
            frame.frameNumber >
            requestedFrame
        ) {
            return false;
        }
    }


    return false;
}


// =============================================================
// PLAYER SESSION
// =============================================================

static void closePlayerFile()
{
    if (playerFile) {
        playerFile.close();
    }


    if (subtitleFile) {
        subtitleFile.close();
    }


    playerHasSubtitles =
        false;

    currentSubtitleCue.valid =
        false;

    currentSubtitleCue.startMs =
        0;

    currentSubtitleCue.endMs =
        0;

    currentSubtitleCue.text =
        "";

    subtitleEof =
        false;


    playerPath =
        "";

    playerFormat =
        PLAYER_NONE;

    currentMeta = {
        0, 0, 0, 0, 0, false
    };

    aviMeta = {
        0
    };

    aviScanPos =
        0;

    aviNextFrame =
        0;

    mkvMeta = {
        1000000ULL,
        1,
        0,
        false,
        false,
        0,
        0,
        0,
        0
    };

    mkvScan = {
        0, 0, 0, 0
    };

    mkvSubtitleScan = {
        0, 0, 0
    };
}


static bool openAviSession(
    const String &path
)
{
    if (
        playerFile &&
        playerPath == path &&
        playerFormat == PLAYER_AVI
    ) {

        playerLastAccessMs =
            millis();

        return true;
    }


    closePlayerFile();


    if (!playerFile.openRead(path)) {

        Serial.println(
            "WebPlayer: cannot open " +
            path
        );

        return false;
    }


    PlayerMeta meta;
    AviMeta containerMeta;


    if (!parseAviMeta(
            playerFile,
            meta,
            containerMeta
        )) {

        Serial.println(
            "WebPlayer: invalid/unsupported AVI " +
            path
        );

        playerFile.close();

        return false;
    }


    playerPath =
        path;

    playerFormat =
        PLAYER_AVI;

    currentMeta =
        meta;

    aviMeta =
        containerMeta;

    aviScanPos =
        aviMeta.moviDataStart;

    aviNextFrame =
        0;


    String srtPath =
        path.substring(
            0,
            path.length() - 4
        ) +
        ".srt";


    subtitleFile =
        STORAGE.open(
            srtPath.c_str(),
            FILE_READ
        );


    if (subtitleFile) {

        subtitleFile.setTimeout(
            50
        );

        playerHasSubtitles =
            true;

        resetSrtScan();
    }


    playerLastAccessMs =
        millis();


    Serial.printf(
        "WebPlayer: AVI opened %lux%lu @ %lu fps | frames=%lu\n",
        (unsigned long)currentMeta.width,
        (unsigned long)currentMeta.height,
        (unsigned long)currentMeta.fps,
        (unsigned long)currentMeta.frameCount
    );


    return true;
}


static bool openMkvSession(
    const String &path
)
{
    if (
        playerFile &&
        playerPath == path &&
        playerFormat == PLAYER_MKV
    ) {

        playerLastAccessMs =
            millis();

        return true;
    }


    closePlayerFile();


    if (!playerFile.openRead(path)) {

        Serial.println(
            "WebPlayer: cannot open " +
            path
        );

        return false;
    }


    PlayerMeta meta;
    MkvMeta containerMeta;


    if (!parseMkvMeta(
            playerFile,
            meta,
            containerMeta
        )) {

        Serial.println(
            "WebPlayer: invalid/unsupported MKV " +
            path
        );

        playerFile.close();

        return false;
    }


    playerPath =
        path;

    playerFormat =
        PLAYER_MKV;

    currentMeta =
        meta;

    mkvMeta =
        containerMeta;


    initMkvScan(
        mkvScan,
        mkvMeta.firstClusterPos
    );


    if (mkvMeta.hasSubtitleTrack) {

        playerHasSubtitles =
            true;

        resetMkvSubtitleScan();
    }


    playerLastAccessMs =
        millis();


    Serial.printf(
        "WebPlayer: MKV opened %lux%lu @ %lu fps | frames=%lu\n",
        (unsigned long)currentMeta.width,
        (unsigned long)currentMeta.height,
        (unsigned long)currentMeta.fps,
        (unsigned long)currentMeta.frameCount
    );


    return true;
}


static bool openPlayerSession(
    const String &path
)
{
    if (g_storageLocked)
        return false;

    if (isAviPath(path)) {
        return
            openAviSession(
                path
            );
    }


    if (isMkvPath(path)) {
        return
            openMkvSession(
                path
            );
    }


    return false;
}


static const char *currentFormatName()
{
    switch (playerFormat) {

        case PLAYER_AVI:
            return "avi";

        case PLAYER_MKV:
            return "mkv";

        default:
            return "";
    }
}


// =============================================================
// HTTP / TCP WRITE HELPER
// =============================================================

static bool clientWriteAll(
    WiFiClient &client,
    const uint8_t *data,
    size_t length
)
{
    size_t offset = 0;
    uint32_t lastProgressMs = millis();

    while (
        offset < length &&
        client.connected()
    ) {

        size_t sent =
            client.write(
                data + offset,
                length - offset
            );

        if (sent > 0) {
            offset += sent;
            lastProgressMs = millis();
            continue;
        }

        if (
            (uint32_t)(
                millis() -
                lastProgressMs
            ) >
            5000UL
        ) {
            break;
        }

        delay(1);
    }

    return offset == length;
}


static void putU16LE(
    uint8_t *p,
    uint16_t value
)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}


static void putU32LE(
    uint8_t *p,
    uint32_t value
)
{
    p[0] = (uint8_t)(value & 0xFFUL);
    p[1] = (uint8_t)((value >> 8) & 0xFFUL);
    p[2] = (uint8_t)((value >> 16) & 0xFFUL);
    p[3] = (uint8_t)((value >> 24) & 0xFFUL);
}


// =============================================================
// META RESPONSE
// =============================================================

static void handlePlayerMeta()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;


    String path =
        playerServer->arg("path");


    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {

        playerServer->send(
            400,
            "application/json",
            "{\"error\":\"invalid recording path\"}"
        );

        return;
    }


    if (!openPlayerSession(path)) {

        playerServer->send(
            404,
            "application/json",
            "{\"error\":\"cannot open recording\"}"
        );

        return;
    }


    String json;

    json.reserve(224);


    json +=
        "{\"format\":\"";

    json +=
        currentFormatName();

    json +=
        "\",\"fps\":";

    json +=
        String(
            (unsigned long)currentMeta.fps
        );

    json +=
        ",\"frames\":";

    json +=
        String(
            (unsigned long)currentMeta.frameCount
        );

    json +=
        ",\"width\":";

    json +=
        String(
            (unsigned long)currentMeta.width
        );

    json +=
        ",\"height\":";

    json +=
        String(
            (unsigned long)currentMeta.height
        );

    json +=
        ",\"duration_ms\":";

    json +=
        String(
            (unsigned long)currentMeta.durationMs
        );

    json +=
        ",\"variable_timing\":";

    json +=
        currentMeta.variableTiming
        ? "true"
        : "false";

    json +=
        ",\"subtitles\":";

    json +=
        playerHasSubtitles
        ? "true"
        : "false";

    json +=
        ",\"start_epoch_sec\":";

    if (
        playerFormat == PLAYER_MKV &&
        mkvMeta.hasDateUtc
    ) {
        json +=
            String(
                (unsigned long)mkvMeta.startEpochSec
            );
    } else {
        json +=
            "0";
    }

    json +=
        "}";


    playerServer->sendHeader(
        "Cache-Control",
        "no-store"
    );

    playerServer->send(
        200,
        "application/json",
        json
    );
}


// =============================================================
// FRAME BATCH RESPONSE
//
// Binary wire format, little endian:
//
//   4 bytes  ITB2
//   u16      frame count
//   u16      reserved
//
//   repeated:
//     u32    frame number
//     u32    JPEG byte length
//     u32    frame timestamp in milliseconds
//     u32    display/hold duration in milliseconds
//     bytes  JPEG payload
//
// Only frame positions/sizes are kept in RAM. JPEG data is read
// directly from SD and sent to TCP.
// =============================================================

static void handlePlayerBatch()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;

    String path =
        playerServer->arg("path");

    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {
        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording path"
        );
        return;
    }

    if (!openPlayerSession(path)) {
        playerServer->send(
            404,
            "text/plain; charset=utf-8",
            "Cannot open recording"
        );
        return;
    }

    long startArg =
        playerServer->arg("start").toInt();

    if (startArg < 0)
        startArg = 0;

    uint32_t startFrame =
        (uint32_t)startArg;

    if (
        startFrame >=
        currentMeta.frameCount
    ) {
        playerServer->send(
            416,
            "text/plain; charset=utf-8",
            "Frame not available"
        );
        return;
    }

    BatchFrameRef refs[
        PLAYER_BATCH_MAX_FRAMES
    ];

    uint8_t count = 0;

    for (
        uint8_t i = 0;
        i < PLAYER_BATCH_MAX_FRAMES;
        ++i
    ) {
        uint32_t frameNumber =
            startFrame + i;

        if (
            frameNumber >=
            currentMeta.frameCount
        ) {
            break;
        }

        uint32_t jpegPosition = 0;
        uint32_t jpegSize = 0;
        uint32_t resumePosition = 0;
        uint64_t timestampMs64 = 0;
        bool located = false;

        if (playerFormat == PLAYER_AVI) {
            located = locateAviFrame(
                frameNumber,
                jpegPosition,
                jpegSize,
                resumePosition
            );
            if (located && currentMeta.fps > 0) {
                timestampMs64 =
                    ((uint64_t)frameNumber * 1000ULL) /
                    currentMeta.fps;
            }
        } else if (playerFormat == PLAYER_MKV) {
            located = locateMkvFrame(
                frameNumber,
                jpegPosition,
                jpegSize,
                resumePosition,
                &timestampMs64
            );
        }

        if (!located)
            break;

        refs[count].frameNumber =
            frameNumber;

        refs[count].jpegPosition =
            jpegPosition;

        refs[count].jpegSize =
            jpegSize;

        refs[count].resumePosition =
            resumePosition;

        refs[count].timestampMs =
            timestampMs64 > 0xFFFFFFFFULL
            ? 0xFFFFFFFFUL
            : (uint32_t)timestampMs64;

        refs[count].holdMs =
            currentMeta.fps > 0
            ? max(1UL, 1000UL / currentMeta.fps)
            : 1UL;

        count++;
    }

    if (count == 0) {
        playerServer->send(
            416,
            "text/plain; charset=utf-8",
            "Frame not available"
        );
        return;
    }

    for (uint8_t i = 0; i + 1 < count; ++i) {
        if (refs[i + 1].timestampMs > refs[i].timestampMs)
            refs[i].holdMs = refs[i + 1].timestampMs - refs[i].timestampMs;
    }

    if (currentMeta.variableTiming) {
        uint32_t lastIndex = refs[count - 1].frameNumber;
        if (lastIndex + 1U < currentMeta.frameCount) {
            uint32_t p = 0, z = 0, r = 0;
            uint64_t nextTimestampMs = 0;
            if (
                locateMkvFrame(lastIndex + 1U, p, z, r, &nextTimestampMs) &&
                nextTimestampMs > refs[count - 1].timestampMs
            ) {
                uint64_t delta =
                    nextTimestampMs - refs[count - 1].timestampMs;
                refs[count - 1].holdMs =
                    delta > 0xFFFFFFFFULL
                    ? 0xFFFFFFFFUL
                    : (uint32_t)delta;
            }
        } else if (currentMeta.durationMs > refs[count - 1].timestampMs) {
            uint64_t delta =
                currentMeta.durationMs - refs[count - 1].timestampMs;
            refs[count - 1].holdMs =
                delta > 0xFFFFFFFFULL
                ? 0xFFFFFFFFUL
                : (uint32_t)delta;
        }
    }

    uint64_t totalLength64 = 8ULL;

    for (
        uint8_t i = 0;
        i < count;
        ++i
    ) {
        totalLength64 +=
            16ULL +
            refs[i].jpegSize;
    }

    if (
        totalLength64 >
        0xFFFFFFFFULL
    ) {
        playerServer->send(
            500,
            "text/plain; charset=utf-8",
            "Batch too large"
        );
        return;
    }

    size_t totalLength =
        (size_t)totalLength64;

    playerLastAccessMs =
        millis();

    playerServer->sendHeader(
        "Cache-Control",
        "no-store"
    );

    playerServer->sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );

    playerServer->setContentLength(
        totalLength
    );

    playerServer->send(
        200,
        "application/octet-stream",
        ""
    );

    WiFiClient client =
        playerServer->client();

    uint8_t batchHeader[8] = {
        'I', 'T', 'B', '2',
        0, 0, 0, 0
    };

    putU16LE(
        &batchHeader[4],
        count
    );

    if (!clientWriteAll(
            client,
            batchHeader,
            sizeof(batchHeader)
        )) {
        Serial.println(
            "WebPlayer: batch header send failed"
        );
        return;
    }

    for (
        uint8_t i = 0;
        i < count;
        ++i
    ) {
        uint8_t frameHeader[16];

        putU32LE(
            &frameHeader[0],
            refs[i].frameNumber
        );

        putU32LE(
            &frameHeader[4],
            refs[i].jpegSize
        );

        putU32LE(
            &frameHeader[8],
            refs[i].timestampMs
        );

        putU32LE(
            &frameHeader[12],
            refs[i].holdMs
        );

        if (!clientWriteAll(
                client,
                frameHeader,
                sizeof(frameHeader)
            )) {
            Serial.println(
                "WebPlayer: batch frame header send failed"
            );
            return;
        }

        if (!playerFile.seek(
                refs[i].jpegPosition
            )) {
            Serial.println(
                "WebPlayer: batch JPEG seek failed"
            );
            return;
        }

        uint32_t remaining =
            refs[i].jpegSize;

        while (remaining > 0) {
            size_t wanted =
                remaining <
                    sizeof(transferBuffer)
                ? remaining
                : sizeof(transferBuffer);

            size_t got =
                playerFile.read(
                    transferBuffer,
                    wanted
                );

            if (got == 0) {
                Serial.println(
                    "WebPlayer: batch JPEG read failed"
                );
                return;
            }

            if (!clientWriteAll(
                    client,
                    transferBuffer,
                    got
                )) {
                Serial.println(
                    "WebPlayer: batch JPEG send failed"
                );
                return;
            }

            remaining -=
                (uint32_t)got;
        }
    }

    // Logical parser state already points behind the collected
    // batch. Keep the physical File cursor there as well.
    playerFile.seek(
        refs[count - 1].
            resumePosition
    );
}


// =============================================================
// SUBTITLE RESPONSE
// =============================================================

static void handlePlayerSubtitle()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;


    String path =
        playerServer->arg("path");


    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {

        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording path"
        );

        return;
    }


    if (!openPlayerSession(path)) {

        playerServer->send(
            404,
            "text/plain; charset=utf-8",
            "Cannot open recording"
        );

        return;
    }


    if (!playerHasSubtitles) {

        playerServer->send(
            204,
            "text/plain; charset=utf-8",
            ""
        );

        return;
    }


    uint64_t requestedMs =
        (uint64_t)
            playerServer->arg("ms").toInt();


    String text;

    bool found =
        false;


    if (
        playerFormat ==
        PLAYER_AVI
    ) {

        found =
            findSrtSubtitle(
                requestedMs,
                text
            );

    } else if (
        playerFormat ==
        PLAYER_MKV
    ) {

        found =
            findMkvSubtitle(
                requestedMs,
                text
            );
    }


    playerLastAccessMs =
        millis();


    playerServer->sendHeader(
        "Cache-Control",
        "no-store"
    );


    if (!found) {

        playerServer->send(
            204,
            "text/plain; charset=utf-8",
            ""
        );

        return;
    }


    playerServer->send(
        200,
        "text/plain; charset=utf-8",
        text
    );
}


// =============================================================
// JPEG FRAME RESPONSE
// =============================================================

static void handlePlayerFrame()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;


    String path =
        playerServer->arg("path");


    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {

        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording path"
        );

        return;
    }


    if (!openPlayerSession(path)) {

        playerServer->send(
            404,
            "text/plain; charset=utf-8",
            "Cannot open recording"
        );

        return;
    }


    long frameArg =
        playerServer->arg("frame").toInt();


    if (frameArg < 0)
        frameArg = 0;


    uint32_t requestedFrame =
        (uint32_t)frameArg;


    uint32_t jpegPosition =
        0;

    uint32_t jpegSize =
        0;

    uint32_t resumePosition =
        0;


    bool located =
        false;


    if (
        playerFormat ==
        PLAYER_AVI
    ) {

        located =
            locateAviFrame(
                requestedFrame,
                jpegPosition,
                jpegSize,
                resumePosition
            );

    } else if (
        playerFormat ==
        PLAYER_MKV
    ) {

        uint64_t frameTimestampMs = 0;
        located =
            locateMkvFrame(
                requestedFrame,
                jpegPosition,
                jpegSize,
                resumePosition,
                &frameTimestampMs
            );

        if (located) {
            playerServer->sendHeader(
                "X-Frame-Time-Ms",
                String((unsigned long)min(
                    frameTimestampMs,
                    (uint64_t)0xFFFFFFFFULL
                ))
            );
        }
    }


    if (!located) {

        playerServer->send(
            416,
            "text/plain; charset=utf-8",
            "Frame not available"
        );

        return;
    }


    if (!playerFile.seek(
            jpegPosition
        )) {

        playerServer->send(
            500,
            "text/plain; charset=utf-8",
            "Recording seek failed"
        );

        return;
    }


    playerLastAccessMs =
        millis();


    playerServer->sendHeader(
        "Cache-Control",
        "no-store"
    );

    playerServer->sendHeader(
        "X-Frame-Number",
        String(
            (unsigned long)requestedFrame
        )
    );

    playerServer->setContentLength(
        jpegSize
    );

    playerServer->send(
        200,
        "image/jpeg",
        ""
    );


    WiFiClient client =
        playerServer->client();


    uint32_t remaining =
        jpegSize;


    while (
        remaining > 0 &&
        client.connected()
    ) {

        size_t wanted =
            remaining <
                sizeof(transferBuffer)
            ? remaining
            : sizeof(transferBuffer);


        size_t got =
            playerFile.read(
                transferBuffer,
                wanted
            );


        if (got == 0)
            break;


        if (!clientWriteAll(
                client,
                transferBuffer,
                got
            )) {
            break;
        }


        remaining -=
            (uint32_t)got;
    }


    // Restore parser position after raw JPEG transfer.
    playerFile.seek(
        resumePosition
    );


    if (remaining != 0) {

        Serial.printf(
            "WebPlayer: incomplete JPEG frame %lu | remaining=%lu\n",
            (unsigned long)requestedFrame,
            (unsigned long)remaining
        );
    }
}


// =============================================================
// DELETE CURRENT RECORDING
// =============================================================

// Temporarily block NEW recordings while the SD directory is modified.
// Preserve the previous global state so a lock that was already set by
// another subsystem is not accidentally cleared when this guard ends.
class RecordingStartBlockGuard {
public:
    RecordingStartBlockGuard()
        : previousState(g_recordingStartBlocked)
    {
        g_recordingStartBlocked =
            true;
    }

    ~RecordingStartBlockGuard()
    {
        g_recordingStartBlocked =
            previousState;
    }

private:
    bool previousState;
};


static bool queuePendingDelete(
    const String &path
)
{
    for (
        uint8_t i = 0;
        i < pendingDeleteCount;
        ++i
    ) {
        if (pendingDeletePaths[i] == path)
            return true;
    }


    if (
        pendingDeleteCount >=
        PLAYER_DELETE_QUEUE_MAX
    ) {
        return false;
    }


    pendingDeletePaths[pendingDeleteCount++] =
        path;

    pendingDeleteSafeSinceMs =
        0;

    return true;
}


static void removeFirstPendingDelete()
{
    if (pendingDeleteCount == 0)
        return;


    for (
        uint8_t i = 1;
        i < pendingDeleteCount;
        ++i
    ) {
        pendingDeletePaths[i - 1] =
            pendingDeletePaths[i];
    }


    pendingDeleteCount--;
    pendingDeletePaths[pendingDeleteCount] =
        "";
}


static bool deleteQueuedRecordingNow(
    const String &path
)
{
    // A queued file may still be the old player session. Ensure no
    // playback handle remains open before touching the directory.
    if (
        playerFile &&
        playerPath == path
    ) {
        closePlayerFile();
    }


    File existing =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );


    // Already absent is equivalent to a successful delete request.
    if (!existing)
        return true;


    bool isDirectory =
        existing.isDirectory();

    existing.close();


    if (isDirectory)
        return false;


    if (!STORAGE.remove(path.c_str()))
        return false;


    if (isAviPath(path)) {

        String srtPath =
            path.substring(
                0,
                path.length() - 4
            ) +
            ".srt";


        File srtFile =
            STORAGE.open(
                srtPath.c_str(),
                FILE_READ
            );


        if (srtFile) {
            srtFile.close();
            STORAGE.remove(
                srtPath.c_str()
            );
        }
    }


    String notePath =
        annotationPathForMedia(path);

    STORAGE.remove(
        notePath.c_str()
    );

    STORAGE.remove(
        (notePath + ".tmp").c_str()
    );


    return true;
}


static void processPendingDelete()
{
    if (pendingDeleteCount == 0) {
        pendingDeleteSafeSinceMs = 0;
        return;
    }


    // Once the queue has reached its safe-idle window, keep NEW recordings
    // blocked while draining a small batch. The previous implementation
    // deleted only one file per 1.5 s grace period, so an old queue could stay
    // full for a long time and reject a later multi-select delete request.
    RecordingStartBlockGuard recordingBlock;


    if (recorderIsOpen()) {
        pendingDeleteSafeSinceMs =
            0;

        return;
    }


    uint8_t processed =
        0;


    while (
        pendingDeleteCount > 0 &&
        processed < PLAYER_DELETE_BATCH_MAX
    ) {
        String path =
            pendingDeletePaths[0];


        bool ok =
            deleteQueuedRecordingNow(
                path
            );


        if (ok) {
            Serial.println(
                "WebPlayer: queued delete completed " +
                path
            );
        } else {
            Serial.println(
                "WebPlayer: queued delete failed " +
                path
            );
        }


        removeFirstPendingDelete();
        processed++;

        // Keep the loop cooperative during a larger queued cleanup.
        yield();
    }


    pendingDeleteSafeSinceMs =
        pendingDeleteCount > 0
        ? millis()
        : 0;
}


static void handlePlayerDelete()
{
    if (!playerServer)
        return;


    String path =
        playerServer->arg("path");


    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path) &&
            !isJpegPath(path)
        )
    ) {

        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid media path"
        );

        return;
    }


    if (recorderIsOpen()) {

        // Close the old playback session immediately, but do not modify
        // the SD directory while the recorder is writing.
        closePlayerFile();


        if (!queuePendingDelete(path)) {

            playerServer->send(
                503,
                "text/plain; charset=utf-8",
                "Delete queue is full"
            );

            return;
        }


        Serial.println(
            "WebPlayer: delete queued until recording ends " +
            path
        );


        playerServer->send(
            202,
            "text/plain; charset=utf-8",
            "Queued for deletion after recording"
        );

        return;
    }


    // From here until this request returns, prevent a new recording from
    // starting while we inspect/remove SD directory entries. The guard
    // restores the previous global lock state on every return path.
    RecordingStartBlockGuard recordingBlock;


    // Defensive re-check after the lock is raised. If a recording slipped
    // in between the first check and the lock, defer the delete instead.
    if (recorderIsOpen()) {

        closePlayerFile();


        if (!queuePendingDelete(path)) {
            playerServer->send(
                503,
                "text/plain; charset=utf-8",
                "Delete queue is full"
            );

            return;
        }


        playerServer->send(
            202,
            "text/plain; charset=utf-8",
            "Queued for deletion after recording"
        );

        return;
    }


    // The player may currently have the video and/or its AVI sidecar
    // subtitle open. Close those handles before removing the files.
    closePlayerFile();


    File existing =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );


    if (!existing) {

        playerServer->send(
            404,
            "text/plain; charset=utf-8",
            "Recording not found"
        );

        return;
    }


    bool isDirectory =
        existing.isDirectory();

    existing.close();


    if (isDirectory) {

        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording path"
        );

        return;
    }


    if (!STORAGE.remove(path.c_str())) {

        playerServer->send(
            500,
            "text/plain; charset=utf-8",
            "Delete failed"
        );

        return;
    }


    // AVI subtitles are stored as a sidecar file with the same basename.
    // Remove it together with the video when present. MKV subtitles are
    // embedded and therefore need no additional file removal.
    if (isAviPath(path)) {

        String srtPath =
            path.substring(
                0,
                path.length() - 4
            ) +
            ".srt";


        File srtFile =
            STORAGE.open(
                srtPath.c_str(),
                FILE_READ
            );


        if (srtFile) {
            srtFile.close();
            STORAGE.remove(
                srtPath.c_str()
            );
        }
    }


    String notePath =
        annotationPathForMedia(path);

    STORAGE.remove(
        notePath.c_str()
    );

    STORAGE.remove(
        (notePath + ".tmp").c_str()
    );


    Serial.println(
        "WebPlayer: deleted " +
        path
    );


    playerServer->send(
        200,
        "text/plain; charset=utf-8",
        "Deleted"
    );
}


// =============================================================
// MEDIA ANNOTATION SIDECAR
// =============================================================

static void handlePlayerAnnotationGet()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;

    String path =
        playerServer->arg("path");

    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {
        playerServer->send(400, "text/plain; charset=utf-8", "Invalid recording path");
        return;
    }

    String text;

    if (!loadMediaAnnotation(path, text)) {
        playerServer->send(500, "text/plain; charset=utf-8", "Annotation read failed");
        return;
    }

    playerServer->sendHeader("Cache-Control", "no-store");

    if (!text.length()) {
        playerServer->send(204, "text/plain; charset=utf-8", "");
        return;
    }

    playerServer->send(200, "text/plain; charset=utf-8", text);
}


static void handlePlayerAnnotationPost()
{
    if (!playerServer)
        return;

    String path =
        playerServer->arg("path");

    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path)
        )
    ) {
        playerServer->send(400, "text/plain; charset=utf-8", "Invalid recording path");
        return;
    }

    if (g_storageLocked) {
        playerServer->send(409, "text/plain; charset=utf-8", "Storage locked");
        return;
    }

    if (recorderIsOpen()) {
        playerServer->send(409, "text/plain; charset=utf-8", "Recording active");
        return;
    }

    RecordingStartBlockGuard recordingBlock;

    if (recorderIsOpen()) {
        playerServer->send(409, "text/plain; charset=utf-8", "Recording active");
        return;
    }

    // Release the current media read handle before changing the directory.
    // The next frame/batch request reopens the session transparently.
    closePlayerFile();

    String text =
        normalizedAnnotationText(
            playerServer->arg("text")
        );

    String notePath =
        annotationPathForMedia(path);

    String tempPath =
        notePath +
        ".tmp";

    STORAGE.remove(tempPath.c_str());

    if (!text.length()) {
        STORAGE.remove(notePath.c_str());
        playerServer->send(200, "text/plain; charset=utf-8", "Cleared");
        return;
    }

    RecordingStorageFile file;

    if (!file.openWrite(
            tempPath,
            cfg_recording_encryption != 0
        )) {
        playerServer->send(500, "text/plain; charset=utf-8", "Annotation write failed");
        return;
    }

    size_t written =
        file.write(
            (const uint8_t *)text.c_str(),
            text.length()
        );

    bool finalized =
        file.closeChecked();

    if (
        written != text.length() ||
        !finalized
    ) {
        STORAGE.remove(tempPath.c_str());
        playerServer->send(500, "text/plain; charset=utf-8", "Annotation write failed");
        return;
    }

    STORAGE.remove(notePath.c_str());

    if (!STORAGE.rename(tempPath.c_str(), notePath.c_str())) {
        STORAGE.remove(tempPath.c_str());
        playerServer->send(500, "text/plain; charset=utf-8", "Annotation finalize failed");
        return;
    }

    playerServer->send(200, "text/plain; charset=utf-8", text);
}


// =============================================================
// MEDIA PREVIOUS / NEXT
// =============================================================

static void handlePlayerNeighbors()
{
    if (!playerServer)
        return;

    if (rejectPlayerWhileRecording())
        return;

    String path =
        playerServer->arg("path");

    String mode =
        normalizedMediaMode(
            playerServer->arg("mode")
        );

    if (
        !validRecordingPath(path) ||
        !mediaPathMatchesMode(
            path,
            mode
        )
    ) {
        playerServer->send(
            400,
            "application/json; charset=utf-8",
            "{\"error\":\"invalid media path or filter\"}"
        );
        return;
    }

    int slash =
        path.lastIndexOf('/');

    if (slash <= 0) {
        playerServer->send(
            400,
            "application/json; charset=utf-8",
            "{\"error\":\"invalid media folder\"}"
        );
        return;
    }

    String folderPath =
        path.substring(
            0,
            slash
        );

    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );

    if (
        !root ||
        !root.isDirectory()
    ) {
        if (root)
            root.close();

        playerServer->send(
            404,
            "application/json; charset=utf-8",
            "{\"error\":\"media folder not found\"}"
        );
        return;
    }

    std::vector<String> mediaPaths;

    File file =
        root.openNextFile();

    while (file) {
        if (!file.isDirectory()) {
            String name =
                String(file.name());

            int fileSlash =
                name.lastIndexOf('/');

            if (fileSlash >= 0) {
                name =
                    name.substring(
                        fileSlash + 1
                    );
            }

            String candidate =
                folderPath +
                "/" +
                name;

            if (mediaPathMatchesMode(
                    candidate,
                    mode
                )) {
                mediaPaths.push_back(
                    candidate
                );
            }
        }

        file.close();
        file = root.openNextFile();
    }

    root.close();

    std::sort(
        mediaPaths.begin(),
        mediaPaths.end(),
        [](const String &a, const String &b) {
            return a.compareTo(b) > 0;
        }
    );

    int currentIndex =
        -1;

    for (size_t i = 0; i < mediaPaths.size(); ++i) {
        if (mediaPaths[i] == path) {
            currentIndex =
                (int)i;
            break;
        }
    }

    if (currentIndex < 0) {
        playerServer->send(
            404,
            "application/json; charset=utf-8",
            "{\"error\":\"media file not found in active filter\"}"
        );
        return;
    }

    String previousPath;
    String nextPath;

    // mediaPaths is sorted newest -> oldest. Chronological navigation must
    // therefore move "Previous" toward the older entry and "Next" toward the
    // newer entry. The old mapping was exactly reversed.
    if (
        (size_t)currentIndex + 1U <
        mediaPaths.size()
    ) {
        previousPath =
            mediaPaths[
                (size_t)currentIndex + 1U
            ];
    }

    if (currentIndex > 0) {
        nextPath =
            mediaPaths[
                (size_t)currentIndex - 1U
            ];
    }

    String json;
    json.reserve(
        160 +
        previousPath.length() +
        nextPath.length()
    );

    json +=
        "{\"mode\":\"" +
        playerJsonEscape(mode) +
        "\",\"previous\":";

    if (previousPath.length()) {
        json +=
            "\"" +
            playerJsonEscape(previousPath) +
            "\"";
    } else {
        json += "null";
    }

    json +=
        ",\"next\":";

    if (nextPath.length()) {
        json +=
            "\"" +
            playerJsonEscape(nextPath) +
            "\"";
    } else {
        json += "null";
    }

    json += "}";

    playerServer->sendHeader(
        "Cache-Control",
        "no-store"
    );

    playerServer->send(
        200,
        "application/json; charset=utf-8",
        json
    );
}


// =============================================================
// PLAYER HTML PAGE
// =============================================================

static void handlePlayerPage()
{
    if (rejectPlayerWhileRecording())
        return;

    if (!playerServer)
        return;


    String path =
        playerServer->arg("path");


    if (
        !validRecordingPath(path) ||
        (
            !isAviPath(path) &&
            !isMkvPath(path) &&
            !isJpegPath(path)
        )
    ) {

        playerServer->send(
            400,
            "text/plain; charset=utf-8",
            "Invalid media path"
        );

        return;
    }


    static const char PLAYER_PAGE[] PROGMEM =
R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>)HTML"
SENSORFORGE_APP_NAME_LITERAL " &middot; "
SENSORFORGE_PLATFORM_LITERAL " &middot; v"
SENSORFORGE_CORE_VERSION_LITERAL
R"HTML(</title>
<style>
:root{
    --bg:#eef1f4;
    --panel:#fff;
    --text:#1f2933;
    --muted:#667085;
    --line:#d8dee6;
    --nav:#1f2937;
    --nav2:#344054;
    --accent:#2563eb;
    --danger:#b42318;
}
*{box-sizing:border-box;}
body{
    font-family:Arial,sans-serif;
    margin:0;
    background:var(--bg);
    color:var(--text);
}
h2{
    margin-top:0;
    color:#27313d;
}
.topbar{
    position:sticky;
    top:0;
    z-index:1000;
    background:var(--nav);
    box-shadow:0 2px 8px rgba(0,0,0,.18);
}
.navwrap{
    max-width:1200px;
    margin:0 auto;
    display:flex;
    align-items:center;
    gap:8px;
    padding:0 14px;
    min-height:54px;
}
.brand{
    color:#fff;
    text-decoration:none;
    font-weight:700;
    padding:8px 10px 8px 0;
    margin-right:8px;
    white-space:nowrap;
    display:flex;
    flex-direction:column;
    line-height:1.05;
}
.brand-main{
    font-size:1.02rem;
    letter-spacing:.08em;
    text-transform:uppercase;
}
.brand-sub{
    font-size:.61rem;
    font-weight:600;
    color:#aeb8c5;
    letter-spacing:.08em;
    margin-top:4px;
    text-transform:uppercase;
}
.navlinks{
    display:flex;
    align-items:center;
    gap:2px;
    flex-wrap:wrap;
}
.navitem,.navdrop>summary{
    display:block;
    color:#e5e7eb;
    text-decoration:none;
    padding:10px 11px;
    border-radius:6px;
    cursor:pointer;
    user-select:none;
    font-size:.95rem;
    white-space:nowrap;
}
.navitem:hover,.navdrop>summary:hover,.navitem.active,.navdrop>summary.active{
    background:var(--nav2);
    color:#fff;
}
.navdrop{position:relative;}
.navdrop>summary{list-style:none;}
.navdrop>summary::-webkit-details-marker{display:none;}
.navdrop>summary:after{content:'  ▾';font-size:.75em;}
.dropdown{
    position:absolute;
    left:0;
    top:calc(100% + 2px);
    min-width:210px;
    background:#fff;
    border:1px solid var(--line);
    border-radius:8px;
    box-shadow:0 10px 28px rgba(0,0,0,.18);
    padding:6px;
}
.navdrop:not([open]) .dropdown{display:none;}
.dropdown a{
    display:block;
    text-decoration:none;
    color:var(--text);
    padding:9px 10px;
    border-radius:6px;
    margin:0;
}
.dropdown a:hover,.dropdown a.active{background:#eef4ff;color:#174ea6;}
.dropdown .sep{height:1px;background:var(--line);margin:6px 4px;}
.dropdown .danger-link{color:var(--danger);}
.page{
    max-width:1200px;
    margin:20px auto;
    padding:0 14px 30px;
}
.box{
    background:var(--panel);
    padding:22px;
    border-radius:10px;
    box-shadow:0 2px 12px rgba(16,24,40,.08);
}
.media-nav-layout{
    display:grid;
    grid-template-columns:minmax(92px,auto) minmax(0,1fr) minmax(92px,auto);
    gap:10px;
    align-items:center;
}
.media-nav-link{
    display:flex;
    align-items:center;
    justify-content:center;
    min-height:54px;
    padding:10px 12px;
    border-radius:8px;
    background:#e9edf2;
    color:#1f2933;
    text-decoration:none;
    font-weight:700;
    text-align:center;
}
.media-nav-link:hover{filter:brightness(.96);}
.media-nav-link.disabled{
    visibility:hidden;
    pointer-events:none;
}
.viewer{
    width:100%;
    max-width:100%;
    overflow:auto;
    background:#111;
    border-radius:6px;
}
.stage-holder{
    width:max-content;
    min-width:100%;
}
.stage{
    position:relative;
    display:block;
    width:auto;
    max-width:none;
    margin:0 auto;
    background:#000;
}
#frame{
    display:block;
    width:auto;
    height:auto;
    max-width:none;
}
.viewer.fit .stage-holder{
    width:100%;
    min-width:0;
}
.viewer.fit .stage{
    width:auto!important;
    max-width:100%;
}
.viewer.fit #frame{
    width:auto!important;
    height:auto!important;
    max-width:100%;
    max-height:70vh;
}
.zoom-controls{
    display:flex;
    align-items:center;
    flex-wrap:wrap;
    gap:6px;
    margin:10px 0;
}
.zoom-controls button{
    margin:0;
    min-width:46px;
}
.zoom-label{
    min-width:64px;
    text-align:center;
    font-weight:700;
    font-variant-numeric:tabular-nums;
}
.zoom-hint{
    color:var(--muted);
    font-size:.85rem;
}
#annotationOverlay{
    position:absolute;
    top:12px;
    left:50%;
    transform:translateX(-50%);
    max-width:90%;
    padding:6px 10px;
    border-radius:6px;
    color:white;
    background:rgba(0,0,0,.48);
    font-size:18px;
    font-weight:700;
    line-height:1.25;
    text-align:center;
    white-space:pre-wrap;
    overflow-wrap:anywhere;
    text-shadow:0 1px 2px rgba(0,0,0,.9);
    pointer-events:none;
    display:none;
}
.annotation-panel{
    margin:12px 0;
    padding:12px;
    border:1px solid var(--line);
    border-radius:8px;
    background:#f8fafc;
}
.annotation-panel label{display:block;font-weight:700;margin-bottom:6px;}
.annotation-panel textarea{width:100%;min-height:64px;resize:vertical;box-sizing:border-box;padding:8px 10px;border:1px solid #cbd5e1;border-radius:6px;font:inherit;color:#1f2933;background:#fff;}
.annotation-actions{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-top:6px;}
.annotation-status{color:var(--muted);font-size:.85rem;}
#timestamp{
    position:absolute;
    right:12px;
    top:auto;
    left:auto;
    bottom:12px;
    padding:0;
    color:white;
    background:transparent;
    font-family:monospace;
    font-size:18px;
    font-weight:700;
    line-height:1.2;
    text-align:right;
    white-space:pre-line;
    text-shadow:0 1px 2px rgba(0,0,0,.95),0 0 3px rgba(0,0,0,.85);
    pointer-events:none;
    display:none;
}
.controls{
    margin:10px 0 12px;
}
.speed-control{
    display:inline-flex;
    align-items:center;
    gap:6px;
    margin:5px 10px 5px 0;
    color:var(--muted);
    font-weight:600;
}
.speed-control select{
    padding:8px 10px;
    border:1px solid #cbd5e1;
    border-radius:6px;
    background:white;
    color:#1f2933;
    font-weight:700;
}
button{
    padding:9px 15px;
    margin:5px 5px 5px 0;
    border:0;
    border-radius:6px;
    background:#e9edf2;
    color:#1f2933;
    cursor:pointer;
    font-weight:600;
}
button:hover{filter:brightness(.96);}
.deleteBtn{
    background:var(--danger);
    color:white;
}
#seek{
    width:min(700px,95%);
    vertical-align:middle;
}
#status{
    margin-top:8px;
    color:var(--muted);
}
@media(max-width:760px){
    .navwrap{align-items:flex-start;flex-direction:column;padding:8px 12px;}
    .brand{padding:6px 2px;margin:0;}
    .brand-sub{font-size:.58rem;}
    .navlinks{width:100%;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px;}
    .navitem,.navdrop>summary{width:100%;padding:9px;}
    .navdrop{width:100%;}
    .dropdown{position:static;min-width:0;margin-top:3px;box-shadow:none;border-color:#566170;}
    .page{margin-top:12px;padding-left:8px;padding-right:8px;}
    .box{padding:15px;border-radius:8px;}
    .media-nav-layout{grid-template-columns:58px minmax(0,1fr) 58px;gap:5px;}
    .media-nav-link{min-height:48px;padding:7px 4px;font-size:.72rem;}
}
</style>
</head>
<body>
<nav class="topbar"><div class="navwrap">
    <a class="brand" href="/"><span class="brand-main">)HTML"
SENSORFORGE_APP_NAME_LITERAL
R"HTML(</span><span class="brand-sub">)HTML"
SENSORFORGE_PLATFORM_LITERAL " &middot; CORE v" SENSORFORGE_CORE_VERSION_LITERAL
R"HTML(</span></a>
    <div class="navlinks">
        <a class="navitem" href="/">Übersicht</a>
        <details class="navdrop">
            <summary class="active">Aufnahmen</summary>
            <div class="dropdown"><a class="active" href="/files">Aufnahmen verwalten</a></div>
        </details>
        <details class="navdrop">
            <summary>Kamera</summary>
            <div class="dropdown"><a href="/preview">Live Preview</a></div>
        </details>
        <details class="navdrop">
            <summary>Sensor</summary>
            <div class="dropdown">
                <a href="/radar_config">Radar Konfiguration</a>
                <a href="/#simulation">Bewegung simulieren</a>
            </div>
        </details>
        <details class="navdrop">
            <summary>System</summary>
            <div class="dropdown">
                <a href="/sdstatus">SD Status</a>
                <a href="/log">Log Viewer</a>
                <a href="/sysinfo">System Info</a>
                <a href="/board">Board Info</a>
                <a href="/psram">PSRAM Test</a>
                <a href="/sdbench">SD Benchmark</a>
                <div class="sep"></div>
                <a class="danger-link" href="/sdformat">SD Wipe</a>
                <a class="danger-link" href="/reboot">Reboot</a>
            </div>
        </details>
        <a class="navitem" href="/config">Konfiguration</a>
    </div>
</div></nav>
<main class="page"><div class="box">

<h2 id="viewerTitle">Recording Player</h2>

<div class="controls">
    <button class="playBtn videoOnly">Play</button>
    <button class="restartBtn videoOnly">Restart</button>
    <button class="prevFrameBtn videoOnly" type="button" title="Previous frame">&#9664; Frame</button>
    <button class="nextFrameBtn videoOnly" type="button" title="Next frame">Frame &#9654;</button>
    <button class="downloadFrameBtn videoOnly" type="button" title="Download currently displayed frame">Download image</button>
    <label class="speed-control videoOnly">Speed
        <select class="speedSelect" aria-label="Playback speed">
            <option value="1">1x</option>
            <option value="2">2x</option>
            <option value="4">4x</option>
        </select>
    </label>
    <button class="downloadBtn">Download</button>
    <button class="deleteBtn">Delete</button>
    <a href="/files"><button type="button">Back</button></a>
</div>

<div class="annotation-panel videoOnly">
    <label for="annotationText">Annotation</label>
    <textarea id="annotationText" maxlength="240" placeholder="Kurze Anmerkung zu dieser Aufnahme ..."></textarea>
    <div class="annotation-actions">
        <button id="saveAnnotationBtn" type="button">Speichern</button>
        <button id="clearAnnotationBtn" type="button">Leeren</button>
        <span id="annotationStatus" class="annotation-status"></span>
    </div>
</div>

<div class="zoom-controls" aria-label="Bildzoom">
    <button id="zoomOutBtn" type="button" title="Zoom out (-)">-</button>
    <span id="zoomLabel" class="zoom-label">Fit</span>
    <button id="zoomInBtn" type="button" title="Zoom in (+)">+</button>
    <button id="zoomFitBtn" type="button" title="Fit to window (F)">Fit</button>
    <button id="zoom100Btn" type="button" title="100% (0)">100%</button>
    <span class="zoom-hint">Tastatur: - / + / F / 0</span>
</div>

<div class="media-nav-layout">
    <a id="prevMediaLink" class="media-nav-link disabled" href="#" aria-disabled="true">&#9664; Previous</a>
    <div id="viewer" class="viewer fit">
        <div class="stage-holder">
            <div class="stage" id="stage">
                <img id="frame" alt="Media">
                <div id="annotationOverlay"></div>
                <div id="timestamp"></div>
            </div>
        </div>
    </div>
    <a id="nextMediaLink" class="media-nav-link disabled" href="#" aria-disabled="true">Next &#9654;</a>
</div>

<div class="controls">
    <button class="playBtn videoOnly">Play</button>
    <button class="restartBtn videoOnly">Restart</button>
    <button class="prevFrameBtn videoOnly" type="button" title="Previous frame">&#9664; Frame</button>
    <button class="nextFrameBtn videoOnly" type="button" title="Next frame">Frame &#9654;</button>
    <button class="downloadFrameBtn videoOnly" type="button" title="Download currently displayed frame">Download image</button>
    <label class="speed-control videoOnly">Speed
        <select class="speedSelect" aria-label="Playback speed">
            <option value="1">1x</option>
            <option value="2">2x</option>
            <option value="4">4x</option>
        </select>
    </label>
    <button class="downloadBtn">Download</button>
    <button class="deleteBtn">Delete</button>
    <a href="/files"><button type="button">Back</button></a>
    <div class="videoOnly">
        <input id="seek" type="range" min="0" max="0" value="0">
        <br>
        <span id="time">00:00 / 00:00</span>
    </div>
</div>

<div id="status">Loading...</div>

</div></main>

<script>
const params = new URLSearchParams(location.search);
const path = params.get('path');
const requestedMediaMode = params.get('mode') || 'all';
const mediaMode =
    requestedMediaMode === 'videos' || requestedMediaMode === 'images'
        ? requestedMediaMode
        : 'all';
const imageMode = /\.jpe?g$/i.test(path || '');

document.addEventListener('click', function(e) {
    const openMenus = document.querySelectorAll('.navdrop[open]');
    for (let i = 0; i < openMenus.length; i++) {
        if (!openMenus[i].contains(e.target))
            openMenus[i].removeAttribute('open');
    }
});

const frameImg = document.getElementById('frame');
const playBtns = document.querySelectorAll('.playBtn');
const restartBtns = document.querySelectorAll('.restartBtn');
const prevFrameBtns = document.querySelectorAll('.prevFrameBtn');
const nextFrameBtns = document.querySelectorAll('.nextFrameBtn');
const downloadFrameBtns = document.querySelectorAll('.downloadFrameBtn');
const speedSelects = document.querySelectorAll('.speedSelect');
const downloadBtns = document.querySelectorAll('.downloadBtn');
const deleteBtns = document.querySelectorAll('.deleteBtn');
const seek = document.getElementById('seek');
const timeLabel = document.getElementById('time');
const statusEl = document.getElementById('status');
const timestampEl = document.getElementById('timestamp');
const annotationOverlayEl = document.getElementById('annotationOverlay');
const annotationTextEl = document.getElementById('annotationText');
const saveAnnotationBtn = document.getElementById('saveAnnotationBtn');
const clearAnnotationBtn = document.getElementById('clearAnnotationBtn');
const annotationStatusEl = document.getElementById('annotationStatus');
const viewerEl = document.getElementById('viewer');
const stageEl = document.getElementById('stage');
const zoomOutBtn = document.getElementById('zoomOutBtn');
const zoomInBtn = document.getElementById('zoomInBtn');
const zoomFitBtn = document.getElementById('zoomFitBtn');
const zoom100Btn = document.getElementById('zoom100Btn');
const zoomLabel = document.getElementById('zoomLabel');
const viewerTitle = document.getElementById('viewerTitle');
const prevMediaLink = document.getElementById('prevMediaLink');
const nextMediaLink = document.getElementById('nextMediaLink');

const zoomLevels = [0.5,0.75,1,1.25,1.5,2,2.5];
const zoomStorageKey = 'sensorforge.viewer.zoom';
let zoomMode = 'fit';

let meta = null;
let currentFrame = 0;
let currentFrameTimeMs = 0;
const requestedPlaybackSpeed = Number(params.get('speed'));
let playbackSpeed =
    requestedPlaybackSpeed === 2 || requestedPlaybackSpeed === 4
        ? requestedPlaybackSpeed
        : 1;
const sparseGapThresholdMs = 3000;
const sparseGapDisplayMs = 500;
let playing = false;
let loading = false;
let currentObjectUrl = null;

let subtitleSecond = -1;
let subtitleRequestSerial = 0;

let playbackGeneration = 0;
let batchController = null;

function viewerUrl(targetPath) {
    return '/play?path=' +
        encodeURIComponent(targetPath) +
        '&mode=' +
        encodeURIComponent(mediaMode) +
        '&speed=' +
        encodeURIComponent(String(playbackSpeed));
}

function snapshotTimestampTextFromPath(targetPath) {
    if (!targetPath)
        return '';

    const match = String(targetPath).match(
        /\/(\d{8})\/(\d{6})\.(?:jpe?g)$/i
    );

    if (!match)
        return '';

    const day = match[1];
    const time = match[2];

    return (
        day.substring(0, 4) + '-' +
        day.substring(4, 6) + '-' +
        day.substring(6, 8) + ' ' +
        time.substring(0, 2) + ':' +
        time.substring(2, 4) + ':' +
        time.substring(4, 6)
    );
}

function setNeighborLink(link, targetPath) {
    if (!link) return;

    if (!targetPath) {
        link.dataset.targetPath = '';
        link.classList.add('disabled');
        link.setAttribute('aria-disabled','true');
        link.href = '#';
        return;
    }

    link.dataset.targetPath = targetPath;
    link.href = viewerUrl(targetPath);
    link.classList.remove('disabled');
    link.removeAttribute('aria-disabled');
}

function refreshMediaNavigationHrefs() {
    [prevMediaLink, nextMediaLink].forEach(function(link) {
        if (!link) return;

        const targetPath =
            String(link.dataset.targetPath || '');

        if (targetPath.length)
            link.href = viewerUrl(targetPath);
    });
}

function updatePlaybackSpeedUrl() {
    try {
        const current = new URL(location.href);
        current.searchParams.set('speed', String(playbackSpeed));
        history.replaceState(null, '', current.pathname + current.search);
    } catch (e) {}
}

async function initMediaNavigation() {
    if (!path) return;

    try {
        const response = await fetch(
            '/player_neighbors?path=' +
            encodeURIComponent(path) +
            '&mode=' +
            encodeURIComponent(mediaMode) +
            '&t=' +
            Date.now(),
            {cache:'no-store'}
        );

        if (!response.ok)
            throw new Error('HTTP ' + response.status);

        const data = await response.json();
        setNeighborLink(prevMediaLink, data.previous || null);
        setNeighborLink(nextMediaLink, data.next || null);
    } catch (e) {
        setNeighborLink(prevMediaLink, null);
        setNeighborLink(nextMediaLink, null);
    }
}

function readStoredZoom() {
    try {
        const stored = localStorage.getItem(zoomStorageKey);

        if (!stored || stored === 'fit')
            return 'fit';

        const value = Number(stored);

        if (zoomLevels.indexOf(value) >= 0)
            return value;
    } catch (e) {}

    return 'fit';
}

function storeZoom() {
    try {
        localStorage.setItem(
            zoomStorageKey,
            zoomMode === 'fit'
                ? 'fit'
                : String(zoomMode)
        );
    } catch (e) {}
}

function applyZoom() {
    if (!viewerEl || !stageEl || !frameImg)
        return;

    if (zoomMode === 'fit') {
        viewerEl.classList.add('fit');
        stageEl.style.width = '';
        frameImg.style.width = '';
        zoomLabel.textContent = 'Fit';
        zoomOutBtn.disabled = false;
        zoomInBtn.disabled = false;
        return;
    }

    viewerEl.classList.remove('fit');

    const baseWidth =
        meta && meta.width
            ? meta.width
            : (
                frameImg.naturalWidth
                    ? frameImg.naturalWidth
                    : 1024
            );

    const width =
        Math.max(
            1,
            Math.round(baseWidth * zoomMode)
        );

    stageEl.style.width = width + 'px';
    frameImg.style.width = '100%';

    zoomLabel.textContent =
        Math.round(zoomMode * 100) + '%';

    zoomOutBtn.disabled =
        zoomMode <= zoomLevels[0];

    zoomInBtn.disabled =
        zoomMode >= zoomLevels[zoomLevels.length - 1];
}

function setZoom(mode) {
    if (
        mode !== 'fit' &&
        zoomLevels.indexOf(mode) < 0
    ) {
        return;
    }

    zoomMode = mode;
    storeZoom();
    applyZoom();
}

function zoomIn() {
    if (zoomMode === 'fit') {
        setZoom(1);
        return;
    }

    const index =
        zoomLevels.indexOf(zoomMode);

    if (
        index >= 0 &&
        index < zoomLevels.length - 1
    ) {
        setZoom(
            zoomLevels[index + 1]
        );
    }
}

function zoomOut() {
    if (zoomMode === 'fit') {
        setZoom(0.75);
        return;
    }

    const index =
        zoomLevels.indexOf(zoomMode);

    if (index > 0) {
        setZoom(
            zoomLevels[index - 1]
        );
    }
}

zoomOutBtn.addEventListener('click', zoomOut);
zoomInBtn.addEventListener('click', zoomIn);
zoomFitBtn.addEventListener(
    'click',
    function() {
        setZoom('fit');
    }
);
zoom100Btn.addEventListener(
    'click',
    function() {
        setZoom(1);
    }
);

zoomMode = readStoredZoom();
applyZoom();

window.addEventListener(
    'resize',
    function() {
        if (zoomMode === 'fit')
            applyZoom();
    }
);

document.addEventListener(
    'keydown',
    function(event) {
        const target = event.target;
        const tag = target && target.tagName
            ? target.tagName.toLowerCase()
            : '';

        if (
            tag === 'input' ||
            tag === 'select' ||
            tag === 'textarea'
        ) {
            return;
        }

        if (
            event.key === '+' ||
            event.key === '='
        ) {
            event.preventDefault();
            zoomIn();
        } else if (event.key === '-') {
            event.preventDefault();
            zoomOut();
        } else if (
            event.key === 'f' ||
            event.key === 'F'
        ) {
            event.preventDefault();
            setZoom('fit');
        } else if (event.key === '0') {
            event.preventDefault();
            setZoom(1);
        }
    }
);

function formatTime(ms) {
    let sec = Math.floor(ms / 1000);
    const h = Math.floor(sec / 3600);
    sec %= 3600;
    const m = Math.floor(sec / 60);
    const s = sec % 60;

    if (h > 0) {
        return String(h).padStart(2,'0') + ':' +
               String(m).padStart(2,'0') + ':' +
               String(s).padStart(2,'0');
    }

    return String(m).padStart(2,'0') + ':' +
           String(s).padStart(2,'0');
}

function formatCaptureTimestamp(epochMs) {
    const date =
        new Date(epochMs);

    if (Number.isNaN(date.getTime()))
        return '';

    const pad2 = function(value) {
        return String(value).padStart(2,'0');
    };

    const tenth =
        Math.floor(date.getMilliseconds() / 100);

    return (
        date.getFullYear() + '-' +
        pad2(date.getMonth() + 1) + '-' +
        pad2(date.getDate()) + ' ' +
        pad2(date.getHours()) + ':' +
        pad2(date.getMinutes()) + ':' +
        pad2(date.getSeconds()) + '.' +
        String(tenth)
    );
}


function updateUi() {
    if (!meta) return;

    const currentMs =
        Math.min(
            meta.duration_ms,
            meta.variable_timing
                ? Math.max(0, currentFrameTimeMs)
                : Math.round(currentFrame * 1000 / meta.fps)
        );

    seek.value =
        Math.min(
            currentFrame,
            meta.frames - 1
        );

    timeLabel.textContent =
        formatTime(currentMs) +
        ' / ' +
        formatTime(meta.duration_ms);

    playBtns.forEach(function(button) {
        button.textContent =
            playing ? 'Pause' : 'Play';
    });

    prevFrameBtns.forEach(function(button) {
        button.disabled =
            currentFrame <= 0;
    });

    nextFrameBtns.forEach(function(button) {
        button.disabled =
            currentFrame >= meta.frames - 1;
    });

    downloadFrameBtns.forEach(function(button) {
        button.disabled =
            !currentObjectUrl;
    });
}

function applyAnnotationOverlay(text) {
    const value = String(text || '').trim();
    if (annotationOverlayEl) {
        annotationOverlayEl.textContent = value;
        annotationOverlayEl.style.display = value.length ? 'block' : 'none';
    }
    if (annotationTextEl && annotationTextEl.value !== value)
        annotationTextEl.value = value;
}

async function loadAnnotation() {
    if (imageMode || !path) return;
    try {
        const response = await fetch('/player_annotation?path=' + encodeURIComponent(path) + '&t=' + Date.now(), {cache:'no-store'});
        if (response.status === 204) {
            applyAnnotationOverlay('');
            if (annotationStatusEl) annotationStatusEl.textContent = '';
            return;
        }
        if (!response.ok) throw new Error('HTTP ' + response.status);
        const text = await response.text();
        applyAnnotationOverlay(text);
        if (annotationStatusEl) annotationStatusEl.textContent = '';
    } catch (e) {
        if (annotationStatusEl) annotationStatusEl.textContent = 'Annotation konnte nicht geladen werden.';
    }
}

async function saveAnnotationText(text) {
    if (imageMode || !path) return;
    const value = String(text || '').replace(/[\r\n\t]+/g, ' ').trim().slice(0, 240);
    if (saveAnnotationBtn) saveAnnotationBtn.disabled = true;
    if (clearAnnotationBtn) clearAnnotationBtn.disabled = true;
    if (annotationStatusEl) annotationStatusEl.textContent = 'Speichere ...';
    try {
        const response = await fetch('/player_annotation?path=' + encodeURIComponent(path), {
            method:'POST', cache:'no-store', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'text=' + encodeURIComponent(value)
        });
        const body = await response.text();
        if (!response.ok) throw new Error(body || ('HTTP ' + response.status));
        applyAnnotationOverlay(value);

        // The recordings overview caches one opened day in sessionStorage.
        // Invalidate exactly that day so the annotation shown in the list is
        // refreshed when the user returns from the player.
        try {
            const parts = path.split('/');
            if (parts.length >= 3 && parts[1]) {
                sessionStorage.removeItem('recordings.day.' + parts[1]);
                sessionStorage.setItem('recordings.annotationDirtyDay', parts[1]);
            }
        } catch (e) {}

        if (annotationStatusEl) annotationStatusEl.textContent = value.length ? 'Gespeichert.' : 'Annotation entfernt.';
    } catch (e) {
        if (annotationStatusEl) annotationStatusEl.textContent = 'Speichern fehlgeschlagen: ' + e.message;
    } finally {
        if (saveAnnotationBtn) saveAnnotationBtn.disabled = false;
        if (clearAnnotationBtn) clearAnnotationBtn.disabled = false;
    }
}

if (saveAnnotationBtn) {
    saveAnnotationBtn.addEventListener('click', function() {
        saveAnnotationText(annotationTextEl ? annotationTextEl.value : '');
    });
}

if (clearAnnotationBtn) {
    clearAnnotationBtn.addEventListener('click', function() {
        if (annotationTextEl) annotationTextEl.value = '';
        saveAnnotationText('');
    });
}

function updateStatus() {
    if (!meta)
        return;

    statusEl.textContent =
        meta.width + 'x' +
        meta.height + ' | ' +
        (meta.variable_timing
            ? ('sparse timing | gaps > ' + (sparseGapThresholdMs / 1000) + ' s compressed')
            : (meta.fps + ' fps')) +
        ' | ' + playbackSpeed + 'x' +
        ' | ' + meta.format.toUpperCase() +
        (
            meta.subtitles
            ? ' | Timestamp'
            : ''
        );
}

async function updateSubtitle(frameNumber, force=false) {
    // Sparse shooter MKVs deliberately omit subtitle tracks. Reconstruct the
    // real wall-clock capture time from Matroska DateUTC plus this frame's
    // original relative timestamp. This is display-only; playback gap
    // compression never changes the recorded time.
    if (
        meta &&
        !meta.subtitles &&
        Number(meta.start_epoch_sec || 0) > 0
    ) {
        const captureText =
            formatCaptureTimestamp(
                Number(meta.start_epoch_sec) * 1000 +
                Math.max(0, Number(currentFrameTimeMs || 0))
            );

        if (captureText.length) {
            timestampEl.textContent =
                captureText;
            timestampEl.style.display =
                'block';
        } else {
            timestampEl.style.display =
                'none';
            timestampEl.textContent =
                '';
        }

        return;
    }

    if (!meta || !meta.subtitles) {
        timestampEl.style.display = 'none';
        timestampEl.textContent = '';
        return;
    }

    const ms =
        Math.round(
            frameNumber *
            1000 /
            meta.fps
        );

    const second =
        Math.floor(
            ms / 1000
        );

    if (
        !force &&
        second === subtitleSecond
    ) {
        return;
    }

    subtitleSecond =
        second;

    const requestSerial =
        ++subtitleRequestSerial;

    try {
        const response =
            await fetch(
                '/player_subtitle?path=' +
                encodeURIComponent(path) +
                '&ms=' +
                ms +
                '&t=' +
                Date.now(),
                {cache:'no-store'}
            );

        if (
            requestSerial !==
            subtitleRequestSerial
        ) {
            return;
        }

        if (response.status === 204) {
            timestampEl.style.display = 'none';
            timestampEl.textContent = '';
            return;
        }

        if (!response.ok)
            throw new Error(
                'HTTP ' +
                response.status
            );

        const text =
            await response.text();

        if (
            requestSerial !==
            subtitleRequestSerial
        ) {
            return;
        }

        if (text.length) {
            timestampEl.textContent =
                text;

            timestampEl.style.display =
                'block';
        } else {
            timestampEl.style.display =
                'none';

            timestampEl.textContent =
                '';
        }

    } catch (e) {

        // Subtitle errors must not stop video playback.
        timestampEl.style.display =
            'none';

        timestampEl.textContent =
            '';
    }
}


function sleepMs(ms) {
    return new Promise(function(resolve) {
        setTimeout(resolve, ms);
    });
}


function releaseBatch(batch) {
    if (!batch)
        return;

    for (const item of batch) {
        if (
            item.url &&
            item.url !== currentObjectUrl
        ) {
            URL.revokeObjectURL(
                item.url
            );
        }
    }
}


function parseBatch(buffer) {
    const view =
        new DataView(buffer);

    if (view.byteLength < 8)
        throw new Error('Short batch');

    if (
        view.getUint8(0) !== 73 ||
        view.getUint8(1) !== 84 ||
        view.getUint8(2) !== 66 ||
        view.getUint8(3) !== 50
    ) {
        throw new Error(
            'Invalid batch'
        );
    }

    const count =
        view.getUint16(
            4,
            true
        );

    if (
        count < 1 ||
        count > 10
    ) {
        throw new Error(
            'Invalid frame count'
        );
    }

    let offset = 8;
    const frames = [];

    for (
        let i = 0;
        i < count;
        ++i
    ) {
        if (
            offset + 16 >
            view.byteLength
        ) {
            throw new Error(
                'Truncated batch header'
            );
        }

        const frameNumber =
            view.getUint32(
                offset,
                true
            );

        const jpegSize =
            view.getUint32(
                offset + 4,
                true
            );

        const timestampMs =
            view.getUint32(
                offset + 8,
                true
            );

        const holdMs =
            view.getUint32(
                offset + 12,
                true
            );

        offset += 16;

        if (
            jpegSize < 4 ||
            offset + jpegSize >
            view.byteLength
        ) {
            throw new Error(
                'Truncated JPEG'
            );
        }

        const jpeg =
            buffer.slice(
                offset,
                offset + jpegSize
            );

        const url =
            URL.createObjectURL(
                new Blob(
                    [jpeg],
                    {type:'image/jpeg'}
                )
            );

        frames.push({
            frame: frameNumber,
            timestamp_ms: timestampMs,
            hold_ms: Math.max(1, holdMs),
            url: url
        });

        offset += jpegSize;
    }

    if (
        offset !==
        view.byteLength
    ) {
        throw new Error(
            'Unexpected batch data'
        );
    }

    return frames;
}


async function fetchBatch(
    startFrame,
    generation
) {
    if (!meta)
        return null;

    if (
        startFrame >=
        meta.frames
    ) {
        return [];
    }

    const controller =
        new AbortController();

    batchController =
        controller;

    const response =
        await fetch(
            '/player_batch?path=' +
            encodeURIComponent(path) +
            '&start=' +
            startFrame +
            '&t=' +
            Date.now(),
            {
                cache:'no-store',
                signal:controller.signal
            }
        );

    if (
        generation !==
        playbackGeneration
    ) {
        return null;
    }

    if (!response.ok)
        throw new Error(
            'HTTP ' +
            response.status
        );

    const buffer =
        await response.arrayBuffer();

    if (
        generation !==
        playbackGeneration
    ) {
        return null;
    }

    const batch =
        parseBatch(buffer);

    if (
        generation !==
        playbackGeneration
    ) {
        releaseBatch(batch);
        return null;
    }

    return batch;
}


function showBatchFrame(item) {
    if (
        currentObjectUrl &&
        currentObjectUrl !== item.url
    ) {
        URL.revokeObjectURL(
            currentObjectUrl
        );
    }

    currentObjectUrl =
        item.url;

    frameImg.src =
        item.url;

    currentFrame =
        item.frame;

    currentFrameTimeMs =
        Number(item.timestamp_ms || 0);

    updateUi();

    updateSubtitle(
        currentFrame
    );
}


async function runSingleFramePlayback(
    generation
) {
    // Variable-timing MKV requires the timing metadata carried by ITB2 batches.
    // Never silently replay a sparse file at a fabricated constant FPS.
    if (meta && meta.variable_timing) {
        playing = false;
        statusEl.textContent = 'Variable-timing playback requires batch transfer';
        updateUi();
        return;
    }

    // Compatibility fallback for a broken/interrupted batch transfer.
    // /player_frame uses the same proven single-JPEG path that is also
    // used for the initial preview frame.
    while (
        playing &&
        generation ===
            playbackGeneration &&
        currentFrame <
            meta.frames - 1
    ) {
        const nextFrame =
            currentFrame + 1;

        const started =
            performance.now();

        const loaded =
            await loadFrame(
                nextFrame
            );

        if (
            !loaded ||
            !playing ||
            generation !==
                playbackGeneration
        ) {
            return;
        }

        const frameMs =
            (1000 / meta.fps) / playbackSpeed;

        const elapsed =
            performance.now() -
            started;

        await sleepMs(
            Math.max(
                0,
                frameMs - elapsed
            )
        );
    }

    if (
        generation ===
            playbackGeneration &&
        currentFrame >=
            meta.frames - 1
    ) {
        playing = false;
        updateUi();
    }
}


function effectivePlaybackHoldMs(item) {
    let sourceHoldMs =
        Math.max(1, Number(item && item.hold_ms || 1));

    // Sparse shooter MKVs preserve the original capture timeline in the file,
    // but the viewer must not force the operator to wait through long periods
    // in which no frame was accepted. Keep short runs temporally faithful and
    // collapse larger gaps to a brief, visible jump. Playback speed applies to
    // both normal frame spacing and the compressed transition.
    if (
        meta &&
        meta.variable_timing &&
        sourceHoldMs > sparseGapThresholdMs
    ) {
        sourceHoldMs = sparseGapDisplayMs;
    }

    return Math.max(1, sourceHoldMs / playbackSpeed);
}


async function runBatchPlayback(
    generation
) {
    let batch = null;

    try {
        batch =
            await fetchBatch(
                currentFrame,
                generation
            );

        while (
            playing &&
            generation ===
                playbackGeneration &&
            batch &&
            batch.length > 0
        ) {
            const followingStart =
                batch[
                    batch.length - 1
                ].frame + 1;

            let nextBatchPromise =
                null;

            // Prefetch one finite batch while this one is shown.
            // This is the main latency-hiding step.
            if (
                followingStart <
                meta.frames
            ) {
                nextBatchPromise =
                    fetchBatch(
                        followingStart,
                        generation
                    );
            }

            for (
                let i = 0;
                i < batch.length;
                ++i
            ) {
                if (
                    !playing ||
                    generation !==
                        playbackGeneration
                ) {
                    releaseBatch(batch);
                    return;
                }

                const started =
                    performance.now();

                showBatchFrame(
                    batch[i]
                );

                const frameMs =
                    meta.variable_timing
                    ? effectivePlaybackHoldMs(batch[i])
                    : (1000 / meta.fps) / playbackSpeed;

                const elapsed =
                    performance.now() -
                    started;

                await sleepMs(
                    Math.max(
                        0,
                        frameMs - elapsed
                    )
                );
            }

            if (
                currentFrame >=
                meta.frames - 1
            ) {
                releaseBatch(batch);

                playing = false;
                updateUi();
                return;
            }

            releaseBatch(batch);

            if (!nextBatchPromise) {
                playing = false;
                updateUi();
                return;
            }

            batch =
                await nextBatchPromise;
        }

    } catch (e) {
        releaseBatch(batch);

        if (
            e.name === 'AbortError' ||
            generation !==
                playbackGeneration
        ) {
            return;
        }

        // A batch response can occasionally be interrupted by the TCP/HTTP
        // connection even though ordinary single-frame requests still work.
        // Keep playback alive and transparently fall back to one JPEG request
        // per frame instead of showing "Failed to fetch" and stopping.
        statusEl.textContent =
            'Batch transfer interrupted - continuing in compatibility mode...';

        await runSingleFramePlayback(
            generation
        );

        if (
            generation ===
                playbackGeneration &&
            !playing
        ) {
            updateStatus();
        }
    }
}


async function loadFrame(frameNumber) {
    if (!meta || loading)
        return false;

    if (frameNumber < 0)
        frameNumber = 0;

    if (frameNumber >= meta.frames)
        frameNumber = meta.frames - 1;

    loading = true;

    try {
        const url =
            '/player_frame?path=' +
            encodeURIComponent(path) +
            '&frame=' +
            frameNumber +
            '&t=' +
            Date.now();

        const response =
            await fetch(
                url,
                {cache:'no-store'}
            );

        if (!response.ok)
            throw new Error(
                'HTTP ' +
                response.status
            );

        const frameTimeHeader =
            response.headers.get('X-Frame-Time-Ms');

        const blob =
            await response.blob();

        if (currentObjectUrl)
            URL.revokeObjectURL(
                currentObjectUrl
            );

        currentObjectUrl =
            URL.createObjectURL(
                blob
            );

        frameImg.src =
            currentObjectUrl;

        currentFrame =
            frameNumber;

        currentFrameTimeMs =
            frameTimeHeader !== null
            ? Math.max(0, Number(frameTimeHeader) || 0)
            : Math.round(frameNumber * 1000 / Math.max(1, meta.fps));

        updateUi();

        updateSubtitle(
            frameNumber
        );

        return true;

    } catch (e) {

        statusEl.textContent =
            'Frame error: ' +
            e.message;

        playing =
            false;

        updateUi();

        return false;

    } finally {

        loading =
            false;
    }
}

function togglePlayback() {
    if (!meta)
        return;

    if (playing) {
        playing = false;

        playbackGeneration++;

        if (batchController)
            batchController.abort();

        updateUi();
        return;
    }

    if (
        currentFrame >=
        meta.frames - 1
    ) {
        currentFrame = 0;
        currentFrameTimeMs = 0;
        subtitleSecond = -1;
    }

    playing = true;

    playbackGeneration++;

    const generation =
        playbackGeneration;

    updateUi();

    runBatchPlayback(
        generation
    );
}

async function restartPlayback() {
    playing =
        false;

    playbackGeneration++;

    if (batchController)
        batchController.abort();

    currentFrame =
        0;

    subtitleSecond =
        -1;

    updateUi();

    await loadFrame(0);
}

playBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        togglePlayback
    );
});

restartBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        restartPlayback
    );
});


function stopPlaybackForFrameStep() {
    playing = false;
    playbackGeneration++;

    if (batchController) {
        batchController.abort();
        batchController = null;
    }

    updateUi();
}


async function stepFrame(delta) {
    if (!meta || loading)
        return;

    const targetFrame =
        Math.max(
            0,
            Math.min(
                meta.frames - 1,
                currentFrame + delta
            )
        );

    if (targetFrame === currentFrame)
        return;

    stopPlaybackForFrameStep();
    subtitleSecond = -1;

    statusEl.textContent =
        'Loading frame...';

    const loaded =
        await loadFrame(targetFrame);

    if (loaded)
        updateStatus();
}


prevFrameBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        function() {
            stepFrame(-1);
        }
    );
});


nextFrameBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        function() {
            stepFrame(1);
        }
    );
});


function currentFrameDownloadName() {
    let baseName =
        String(path || 'sensorforge')
            .split('/')
            .pop()
            .replace(/\.[^.]+$/, '');

    if (!baseName.length)
        baseName = 'sensorforge';

    let timestampSuffix = '';

    if (
        meta &&
        Number(meta.start_epoch_sec || 0) > 0
    ) {
        const captureText =
            formatCaptureTimestamp(
                Number(meta.start_epoch_sec) * 1000 +
                Math.max(0, Number(currentFrameTimeMs || 0))
            );

        if (captureText.length) {
            timestampSuffix =
                '_' + captureText
                    .replace(/[-:. ]/g, '')
                    .replace(/[^0-9]/g, '');
        }
    }

    return (
        baseName +
        '_frame_' +
        String(currentFrame + 1).padStart(6, '0') +
        timestampSuffix +
        '.jpg'
    );
}


async function downloadCurrentFrame() {
    if (!meta || !currentObjectUrl)
        return;

    // The displayed JPEG is already resident in the browser. Render that frame
    // to a canvas and burn in the currently visible timestamp overlay. This
    // keeps the node/SD completely out of the download path while preserving
    // the pristine JPEG inside the MKV.
    if (!frameImg.complete || !frameImg.naturalWidth || !frameImg.naturalHeight)
        return;

    const canvas =
        document.createElement('canvas');

    canvas.width = frameImg.naturalWidth;
    canvas.height = frameImg.naturalHeight;

    const ctx =
        canvas.getContext('2d');

    if (!ctx)
        return;

    ctx.drawImage(
        frameImg,
        0,
        0,
        canvas.width,
        canvas.height
    );

    const overlayText =
        timestampEl &&
        timestampEl.style.display !== 'none'
            ? String(timestampEl.textContent || '').trim()
            : '';

    if (overlayText.length) {
        const fontSize =
            Math.max(18, Math.round(canvas.width * 0.018));
        const padding =
            Math.max(12, Math.round(fontSize * 0.7));
        const lineHeight =
            Math.round(fontSize * 1.22);
        const lines =
            overlayText.split(/\r?\n/);

        ctx.save();
        ctx.font =
            '700 ' + fontSize + 'px monospace';
        ctx.textAlign = 'right';
        ctx.textBaseline = 'bottom';
        ctx.lineJoin = 'round';
        ctx.strokeStyle = 'rgba(0,0,0,0.9)';
        ctx.lineWidth =
            Math.max(2, Math.round(fontSize * 0.14));
        ctx.fillStyle = 'white';

        for (let i = 0; i < lines.length; i++) {
            const x =
                canvas.width - padding;
            const y =
                canvas.height - padding -
                (lines.length - 1 - i) * lineHeight;

            ctx.strokeText(lines[i], x, y);
            ctx.fillText(lines[i], x, y);
        }

        ctx.restore();
    }

    const blob =
        await new Promise(function(resolve) {
            canvas.toBlob(resolve, 'image/jpeg', 0.95);
        });

    if (!blob)
        return;

    const downloadUrl =
        URL.createObjectURL(blob);
    const link =
        document.createElement('a');

    link.href = downloadUrl;
    link.download = currentFrameDownloadName();
    link.style.display = 'none';

    document.body.appendChild(link);
    link.click();
    link.remove();

    setTimeout(function() {
        URL.revokeObjectURL(downloadUrl);
    }, 1000);
}


downloadFrameBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        downloadCurrentFrame
    );
});


speedSelects.forEach(function(select) {
    select.addEventListener('change', function() {
        const requested = Number(select.value);
        playbackSpeed =
            requested === 2 || requested === 4
            ? requested
            : 1;

        speedSelects.forEach(function(other) {
            other.value = String(playbackSpeed);
        });

        updatePlaybackSpeedUrl();
        refreshMediaNavigationHrefs();
        updateStatus();
    });
});

speedSelects.forEach(function(select) {
    select.value = String(playbackSpeed);
});


downloadBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        function() {
            if (!path)
                return;

            const link =
                document.createElement('a');

            link.href =
                '/file?path=' +
                encodeURIComponent(path);

            link.style.display =
                'none';

            document.body.appendChild(link);
            link.click();
            link.remove();
        }
    );
});


function removeRecordingFromListCache(recordingPath) {
    try {
        const parts =
            recordingPath.split('/');

        if (
            parts.length < 3 ||
            !parts[1]
        ) {
            return;
        }


        const cacheKey =
            'recordings.day.' +
            parts[1];

        const cached =
            sessionStorage.getItem(
                cacheKey
            );

        if (!cached)
            return;


        const holder =
            document.createElement('div');

        holder.innerHTML =
            cached;

        let removed =
            false;


        holder.querySelectorAll('.recording').forEach(
            function(row) {
                const link =
                    row.querySelector(
                        "a[href^='/play?path=']"
                    );

                if (!link)
                    return;

                try {
                    const url =
                        new URL(
                            link.getAttribute('href'),
                            location.origin
                        );

                    if (
                        url.searchParams.get('path') ===
                        recordingPath
                    ) {
                        row.remove();
                        removed = true;
                    }
                } catch (e) {}
            }
        );


        if (!removed)
            return;


        if (
            holder.querySelectorAll('.recording').length ===
            0
        ) {
            holder.innerHTML =
                "<div class='emptyrecording'>Keine Aufnahmen oder Snapshots an diesem Tag.</div>";
        }


        sessionStorage.setItem(
            cacheKey,
            holder.innerHTML
        );

    } catch (e) {}
}


async function deleteRecording() {
    if (!path)
        return;

    if (!confirm(imageMode ? 'Delete this snapshot?' : 'Delete this recording?'))
        return;


    playing =
        false;

    playbackGeneration++;

    if (batchController)
        batchController.abort();

    updateUi();

    statusEl.textContent =
        'Deleting...';


    try {
        const response =
            await fetch(
                '/player_delete?path=' +
                encodeURIComponent(path),
                {
                    method:'POST',
                    cache:'no-store'
                }
            );


        if (!response.ok) {
            const message =
                await response.text();

            throw new Error(
                message.length
                ? message
                : 'HTTP ' + response.status
            );
        }


        // Keep the browser-side recordings cache consistent immediately.
        // This is especially important for deferred deletes because the
        // physical file remains on the SD card until recording has stopped.
        removeRecordingFromListCache(
            path
        );


        if (response.status === 202) {
            statusEl.textContent =
                'Zur Löschung vorgemerkt – wird nach Ende der laufenden Aufnahme gelöscht.';

            deleteBtns.forEach(function(button) {
                button.disabled = true;
                button.textContent = 'Vorgemerkt';
            });

            return;
        }


        location.href =
            '/files';

    } catch (e) {
        statusEl.textContent =
            'Delete failed: ' +
            e.message;
    }
}


deleteBtns.forEach(function(button) {
    button.addEventListener(
        'click',
        deleteRecording
    );
});


seek.addEventListener(
    'change',
    async function() {

        playing =
            false;

        playbackGeneration++;

        if (batchController)
            batchController.abort();

        currentFrame =
            Number(seek.value);

        subtitleSecond =
            -1;

        updateUi();

        statusEl.textContent =
            'Seeking...';

        await loadFrame(
            currentFrame
        );

        updateStatus();
    }
);

// Keep WebConfig/WiFi alive for as long as the player page is open.
// Frame/batch requests already count as activity, but once playback ends or
// is paused there may otherwise be no HTTP traffic for the inactivity timer.
const activityHeartbeatMs = 10000;
let activityHeartbeatTimer = 0;
let lastActivityHeartbeatMs = 0;

async function sendActivityHeartbeat(force = true) {
    const now = Date.now();

    if (!force && now - lastActivityHeartbeatMs < 5000)
        return;

    lastActivityHeartbeatMs = now;

    try {
        await fetch(
            '/activity?t=' + now,
            {
                cache:'no-store',
                credentials:'same-origin',
                keepalive:true
            }
        );
    } catch (e) {
        // A transient heartbeat failure should not interrupt playback.
    }
}

async function sendRecordingPauseKeepalive() {
    // The recording pause belongs to the visible WebConfig session, not to
    // one specific page. The standalone player does not use webconfig.cpp's
    // common HTML header, so it must renew the same pause lease explicitly.
    // Hidden/background tabs intentionally do NOT renew the lease.
    if (document.hidden)
        return;

    try {
        await fetch(
            '/recording_pause_keepalive?t=' + Date.now(),
            {
                method:'POST',
                cache:'no-store',
                credentials:'same-origin',
                keepalive:true
            }
        );
    } catch (e) {
        // A transient keepalive failure should not interrupt playback.
    }
}

function sendPlayerSessionHeartbeat() {
    sendActivityHeartbeat(true);
    sendRecordingPauseKeepalive();
}

function startActivityHeartbeat() {
    sendPlayerSessionHeartbeat();

    activityHeartbeatTimer =
        window.setInterval(
            function() {
                sendPlayerSessionHeartbeat();
            },
            activityHeartbeatMs
        );
}

document.addEventListener(
    'visibilitychange',
    function() {
        if (!document.hidden)
            sendPlayerSessionHeartbeat();
    }
);

window.addEventListener(
    'focus',
    function() {
        sendPlayerSessionHeartbeat();
    }
);

window.addEventListener(
    'pageshow',
    function() {
        sendActivityHeartbeat(true);
    }
);

document.addEventListener(
    'pointerdown',
    function() {
        sendActivityHeartbeat(false);
    },
    { passive:true }
);

document.addEventListener(
    'keydown',
    function() {
        sendActivityHeartbeat(false);
    }
);

async function initPlayer() {
    if (!path) {

        statusEl.textContent =
            'Missing media path';

        return;
    }

    initMediaNavigation();

    if (!imageMode)
        await loadAnnotation();

    if (imageMode) {
        if (viewerTitle)
            viewerTitle.textContent = 'Snapshot Viewer';

        document.querySelectorAll('.videoOnly').forEach(function(element) {
            element.hidden = true;
        });

        timestampEl.style.display = 'none';
        timestampEl.textContent = '';

        try {
            await new Promise(function(resolve, reject) {
                frameImg.onload = function() { resolve(); };
                frameImg.onerror = function() { reject(new Error('Image load failed')); };
                frameImg.src =
                    '/file?inline=1&path=' +
                    encodeURIComponent(path) +
                    '&t=' +
                    Date.now();
            });

            meta = {
                width: frameImg.naturalWidth || 0,
                height: frameImg.naturalHeight || 0,
                fps: 1,
                frames: 1,
                duration_ms: 0,
                format: 'jpg',
                subtitles: false
            };

            const snapshotTimestamp =
                snapshotTimestampTextFromPath(path);

            if (snapshotTimestamp.length) {
                timestampEl.textContent = snapshotTimestamp;
                timestampEl.style.display = 'block';
            }

            applyZoom();

            statusEl.textContent =
                (meta.width && meta.height
                    ? meta.width + 'x' + meta.height + ' | '
                    : '') +
                'JPG snapshot';

        } catch (e) {
            timestampEl.style.display = 'none';
            timestampEl.textContent = '';
            statusEl.textContent =
                'Image error: ' +
                e.message;
        }

        return;
    }

    try {

        const response =
            await fetch(
                '/player_meta?path=' +
                encodeURIComponent(path),
                {cache:'no-store'}
            );

        if (!response.ok)
            throw new Error(
                'HTTP ' +
                response.status
            );

        meta =
            await response.json();

        applyZoom();

        seek.max =
            Math.max(
                0,
                meta.frames - 1
            );

        updateStatus();
        updateUi();

        const firstFrameLoaded =
            await loadFrame(0);

        if (!firstFrameLoaded)
            return;

        // Start playback automatically after metadata and the first
        // frame have loaded. This player uses JPEG requests, so it is
        // not subject to browser audio/video autoplay restrictions.
        playing = true;

        playbackGeneration++;

        const generation =
            playbackGeneration;

        updateUi();

        runBatchPlayback(
            generation
        );

    } catch (e) {

        statusEl.textContent =
            'Player error: ' +
            e.message;
    }
}

window.addEventListener(
    'beforeunload',
    function() {

        if (activityHeartbeatTimer) {
            clearInterval(activityHeartbeatTimer);
            activityHeartbeatTimer = 0;
        }

        playbackGeneration++;

        if (batchController)
            batchController.abort();

        if (currentObjectUrl)
            URL.revokeObjectURL(
                currentObjectUrl
            );
    }
);

startActivityHeartbeat();
initPlayer();
</script>

</body>
</html>
)HTML";


    playerServer->send(
        200,
        "text/html; charset=utf-8",
        FPSTR(PLAYER_PAGE)
    );
}


// =============================================================
// PUBLIC API
// =============================================================

void webPlayerRegisterRoutes(
    WebServer &server
)
{
    playerServer =
        &server;


    server.on(
        "/play",
        HTTP_GET,
        handlePlayerPage
    );


    server.on(
        "/player_neighbors",
        HTTP_GET,
        handlePlayerNeighbors
    );


    server.on(
        "/player_meta",
        HTTP_GET,
        handlePlayerMeta
    );


    server.on(
        "/player_frame",
        HTTP_GET,
        handlePlayerFrame
    );


    server.on(
        "/player_batch",
        HTTP_GET,
        handlePlayerBatch
    );


    server.on(
        "/player_subtitle",
        HTTP_GET,
        handlePlayerSubtitle
    );


    server.on(
        "/player_annotation",
        HTTP_GET,
        handlePlayerAnnotationGet
    );


    server.on(
        "/player_annotation",
        HTTP_POST,
        handlePlayerAnnotationPost
    );


    server.on(
        "/player_delete",
        HTTP_POST,
        handlePlayerDelete
    );
}


void webPlayerLoop()
{
    // Recording always wins. Close any playback file handle as
    // soon as a PIR event starts a recording. Pending deletes are
    // deliberately held until recording has remained idle for the
    // grace period below.
    if (recorderIsOpen()) {

        pendingDeleteSafeSinceMs =
            0;

        if (playerFile)
            closePlayerFile();

        return;
    }


    if (pendingDeleteCount > 0) {

        if (pendingDeleteSafeSinceMs == 0) {
            pendingDeleteSafeSinceMs =
                millis();

        } else if (
            (uint32_t)(
                millis() -
                pendingDeleteSafeSinceMs
            ) >=
            PLAYER_DELETE_SAFE_IDLE_MS
        ) {
            processPendingDelete();
        }
    }


    if (!playerFile)
        return;


    if (
        (uint32_t)(
            millis() -
            playerLastAccessMs
        ) >=
        PLAYER_IDLE_CLOSE_MS
    ) {

        Serial.println(
            "WebPlayer: closing idle session"
        );

        closePlayerFile();
    }
}


void webPlayerStop()
{
    // Only close the active playback session. The WebServer object
    // itself persists across WebConfig stop/start cycles and the
    // registered player routes continue to reference it. Clearing
    // playerServer here would make those handlers return without
    // sending an HTTP response (ERR_EMPTY_RESPONSE).
    closePlayerFile();
}
