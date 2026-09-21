#include "avi_writer.h"
#include "board_config.h"
#include "config.h"
#include "logger.h"
#include "image_motion.h"
#include "recording_storage.h"

#include <FS.h>
#include <esp_camera.h>
#include <time.h>

// =============================================================
// MJPEG AVI writer
//
// - Classic RIFF AVI
// - One video stream
// - MJPEG ("MJPG")
// - JPEG frames are stored directly as "00dc" chunks
// - No audio
// - No idx1 index (optional in AVI)
// - Works with STORAGE = SD or SD_MMC
// =============================================================

static RecordingStorageFile aviFile;

static String aviPath;

static uint32_t aviFps          = 5;
static uint32_t frameCount      = 0;
static uint32_t maxFrameSize    = 0;
static uint64_t totalFrameBytes = 0;
static uint32_t moviDataBytes   = 0;

static uint16_t aviWidth  = 0;
static uint16_t aviHeight = 0;

static bool headerWritten = false;
static bool writeFailed   = false;
static bool sizeLimitHit  = false;

// Start time for optional standard SubRip (.srt) subtitles.
static time_t recordingStartEpoch = 0;
static bool recordingStartTimeValid = false;


// =============================================================
// AVI HEADER LAYOUT
// =============================================================
//
// Offset  Description
// ------  ---------------------------------------
//   0     RIFF
//   4     RIFF size
//   8     AVI
//  12     LIST hdrl
//  24     avih
//  32     AVIMAINHEADER data
//  88     LIST strl
// 100     strh
// 108     AVISTREAMHEADER data
// 164     strf
// 172     BITMAPINFOHEADER data
// 212     LIST movi
// 216     movi LIST size
// 220     movi
// 224     first frame chunk
//
// =============================================================

static const uint32_t AVI_HEADER_SIZE = 224;

// Fields that must be updated when recording ends
static const uint32_t OFFSET_RIFF_SIZE          = 4;
static const uint32_t OFFSET_MAX_BYTES_PER_SEC  = 36;
static const uint32_t OFFSET_TOTAL_FRAMES       = 48;
static const uint32_t OFFSET_MAIN_BUFFER_SIZE   = 60;

static const uint32_t OFFSET_STREAM_LENGTH      = 140;
static const uint32_t OFFSET_STREAM_BUFFER_SIZE = 144;

static const uint32_t OFFSET_BITMAP_SIZE_IMAGE  = 192;

static const uint32_t OFFSET_MOVI_LIST_SIZE     = 216;


// Classic RIFF AVI uses 32-bit chunk sizes.
// Stay below 4 GiB to avoid an invalid AVI 1.0 file.
static const uint64_t AVI_MAX_FILE_SIZE =
    0xFFF00000ULL;


// =============================================================
// LITTLE-ENDIAN HELPERS
// =============================================================

static void putU16(
    uint8_t *buffer,
    uint32_t offset,
    uint16_t value
)
{
    buffer[offset + 0] =
        (uint8_t)(value & 0xFF);

    buffer[offset + 1] =
        (uint8_t)((value >> 8) & 0xFF);
}


static void putU32(
    uint8_t *buffer,
    uint32_t offset,
    uint32_t value
)
{
    buffer[offset + 0] =
        (uint8_t)(value & 0xFF);

    buffer[offset + 1] =
        (uint8_t)((value >> 8) & 0xFF);

    buffer[offset + 2] =
        (uint8_t)((value >> 16) & 0xFF);

    buffer[offset + 3] =
        (uint8_t)((value >> 24) & 0xFF);
}


static void putFourCC(
    uint8_t *buffer,
    uint32_t offset,
    const char code[5]
)
{
    buffer[offset + 0] = code[0];
    buffer[offset + 1] = code[1];
    buffer[offset + 2] = code[2];
    buffer[offset + 3] = code[3];
}


static bool patchU32(
    uint32_t offset,
    uint32_t value
)
{
    if (!aviFile)
        return false;

    size_t oldPosition =
        aviFile.position();

    if (!aviFile.seek(offset)) {
        Serial.printf(
            "AVI: seek failed at %lu\n",
            (unsigned long)offset
        );
        return false;
    }

    uint8_t bytes[4];

    bytes[0] =
        (uint8_t)(value & 0xFF);

    bytes[1] =
        (uint8_t)((value >> 8) & 0xFF);

    bytes[2] =
        (uint8_t)((value >> 16) & 0xFF);

    bytes[3] =
        (uint8_t)((value >> 24) & 0xFF);

    bool ok =
        aviFile.write(bytes, sizeof(bytes))
        == sizeof(bytes);

    if (!aviFile.seek(oldPosition)) {
        Serial.println(
            "AVI: could not restore file position"
        );
        return false;
    }

    return ok;
}


// =============================================================
// WRITE INITIAL AVI HEADER
// =============================================================

static bool writeAviHeader(
    uint16_t width,
    uint16_t height,
    uint32_t fps
)
{
    if (!aviFile)
        return false;

    if (fps < 1)
        fps = 1;

    uint8_t header[AVI_HEADER_SIZE];

    memset(
        header,
        0,
        sizeof(header)
    );


    // ---------------------------------------------------------
    // RIFF AVI
    // ---------------------------------------------------------

    putFourCC(header, 0, "RIFF");

    // offset 4 = RIFF size, patched on close

    putFourCC(header, 8, "AVI ");


    // ---------------------------------------------------------
    // LIST hdrl
    // ---------------------------------------------------------

    putFourCC(header, 12, "LIST");

    // Includes "hdrl" + avih + LIST strl
    putU32(header, 16, 192);

    putFourCC(header, 20, "hdrl");


    // ---------------------------------------------------------
    // avih - Main AVI Header
    // ---------------------------------------------------------

    putFourCC(header, 24, "avih");
    putU32(header, 28, 56);

    // dwMicroSecPerFrame
    putU32(
        header,
        32,
        1000000UL / fps
    );

    // dwMaxBytesPerSec
    // patched on close

    // dwPaddingGranularity
    putU32(header, 40, 0);

    // dwFlags
    // No AVIF_HASINDEX because we do not write idx1.
    putU32(header, 44, 0);

    // dwTotalFrames
    // patched on close

    // dwInitialFrames
    putU32(header, 52, 0);

    // dwStreams = one video stream
    putU32(header, 56, 1);

    // dwSuggestedBufferSize
    // patched on close

    // dwWidth / dwHeight
    putU32(header, 64, width);
    putU32(header, 68, height);

    // dwReserved[4] already zero


    // ---------------------------------------------------------
    // LIST strl
    // ---------------------------------------------------------

    putFourCC(header, 88, "LIST");

    // "strl" + strh chunk + strf chunk
    putU32(header, 92, 116);

    putFourCC(header, 96, "strl");


    // ---------------------------------------------------------
    // strh - AVI Stream Header
    // ---------------------------------------------------------

    putFourCC(header, 100, "strh");
    putU32(header, 104, 56);

    // fccType = video
    putFourCC(header, 108, "vids");

    // fccHandler = Motion JPEG
    putFourCC(header, 112, "MJPG");

    // dwFlags
    putU32(header, 116, 0);

    // wPriority / wLanguage
    putU16(header, 120, 0);
    putU16(header, 122, 0);

    // dwInitialFrames
    putU32(header, 124, 0);

    // dwScale / dwRate
    // frame rate = dwRate / dwScale
    putU32(header, 128, 1);
    putU32(header, 132, fps);

    // dwStart
    putU32(header, 136, 0);

    // dwLength
    // patched on close

    // dwSuggestedBufferSize
    // patched on close

    // dwQuality = default
    putU32(header, 148, 0xFFFFFFFFUL);

    // dwSampleSize = 0 because JPEG frames vary in size
    putU32(header, 152, 0);

    // rcFrame: left, top, right, bottom
    putU16(header, 156, 0);
    putU16(header, 158, 0);
    putU16(header, 160, width);
    putU16(header, 162, height);


    // ---------------------------------------------------------
    // strf - BITMAPINFOHEADER
    // ---------------------------------------------------------

    putFourCC(header, 164, "strf");
    putU32(header, 168, 40);

    // biSize
    putU32(header, 172, 40);

    // biWidth / biHeight
    putU32(header, 176, width);
    putU32(header, 180, height);

    // biPlanes
    putU16(header, 184, 1);

    // biBitCount
    putU16(header, 186, 24);

    // biCompression = MJPG
    putFourCC(header, 188, "MJPG");

    // biSizeImage
    // patched on close

    // remaining BITMAPINFOHEADER fields stay zero


    // ---------------------------------------------------------
    // LIST movi
    // ---------------------------------------------------------

    putFourCC(header, 212, "LIST");

    // offset 216 = LIST size, patched on close

    putFourCC(header, 220, "movi");


    // ---------------------------------------------------------
    // Write header
    // ---------------------------------------------------------

    size_t written =
        aviFile.write(
            header,
            sizeof(header)
        );

    if (written != sizeof(header)) {

        consoleWrite(
            "REC",
            "ERROR | AVI header write failed"
        );

        return false;
    }

    headerWritten = true;

    aviWidth  = width;
    aviHeight = height;

    return true;
}


// =============================================================
// STANDARD SUBRIP (.SRT) HELPERS
// =============================================================

static String makeSrtPath(const String &videoPath)
{
    String path = videoPath;

    String lower = path;
    lower.toLowerCase();

    if (lower.endsWith(".avi.part")) {
        path.remove(path.length() - 9);
        path += ".srt.part";
        return path;
    }

    if (lower.endsWith(".avi")) {
        path.remove(path.length() - 4);
    }

    path += ".srt";

    return path;
}


static void formatSrtTime(
    uint64_t milliseconds,
    char *buffer,
    size_t bufferSize
)
{
    uint64_t hours =
        milliseconds / 3600000ULL;

    milliseconds %=
        3600000ULL;

    uint32_t minutes =
        milliseconds / 60000ULL;

    milliseconds %=
        60000ULL;

    uint32_t seconds =
        milliseconds / 1000ULL;

    uint32_t millis =
        milliseconds % 1000ULL;

    snprintf(
        buffer,
        bufferSize,
        "%02llu:%02lu:%02lu,%03lu",
        (unsigned long long)hours,
        (unsigned long)minutes,
        (unsigned long)seconds,
        (unsigned long)millis
    );
}


static bool writeSrtFile()
{
    if (!cfg_timestamp_enabled)
        return true;

    if (!recordingStartTimeValid) {

        consoleWrite(
            "REC",
            "SRT skipped | system time invalid"
        );

        return false;
    }

    if (!aviPath.length() ||
        frameCount == 0 ||
        aviFps == 0) {

        return false;
    }


    String srtPath =
        makeSrtPath(aviPath);

    if (STORAGE.exists(srtPath.c_str())) {
        STORAGE.remove(srtPath.c_str());
    }


    File srtFile =
        STORAGE.open(
            srtPath.c_str(),
            FILE_WRITE
        );

    if (!srtFile) {

        consoleWrite(
            "REC",
            "SRT error | cannot open " + srtPath
        );

        return false;
    }


    // AVI playback duration is derived from frame count / FPS.
    // This keeps subtitle timing synchronized with the video,
    // even if real capture timing had small variations.
    uint64_t totalDurationMs =
        ((uint64_t)frameCount * 1000ULL) /
        (uint64_t)aviFps;

    if (totalDurationMs == 0)
        totalDurationMs = 1;


    uint32_t cueNumber = 1;

    for (
        uint64_t cueStartMs = 0;
        cueStartMs < totalDurationMs;
        cueStartMs += 1000ULL
    ) {

        uint64_t cueEndMs =
            cueStartMs + 1000ULL;

        if (cueEndMs > totalDurationMs)
            cueEndMs = totalDurationMs;


        char startCode[32];
        char endCode[32];

        formatSrtTime(
            cueStartMs,
            startCode,
            sizeof(startCode)
        );

        formatSrtTime(
            cueEndMs,
            endCode,
            sizeof(endCode)
        );


        time_t absoluteTime =
            recordingStartEpoch +
            (time_t)(cueStartMs / 1000ULL);

        struct tm localTime;

        localtime_r(
            &absoluteTime,
            &localTime
        );


        char dateTime[32];

        strftime(
            dateTime,
            sizeof(dateTime),
            "%Y-%m-%d %H:%M:%S",
            &localTime
        );


        // Standard SubRip format:
        //
        // 1
        // 00:00:00,000 --> 00:00:01,000
        // 2026-09-07 09:25:31
        //
        srtFile.printf(
            "%lu\n",
            (unsigned long)cueNumber
        );

        srtFile.printf(
            "%s --> %s\n",
            startCode,
            endCode
        );

        srtFile.printf(
            "%s\n\n",
            dateTime
        );


        cueNumber++;
    }


    srtFile.flush();
    srtFile.close();


    return true;
}


// =============================================================
// START AVI RECORDING
// =============================================================

void aviStart(
    const String &path,
    int fps
)
{
    // Finalize an old recording if still open
    if (aviFile) {
        aviEnd();
    }


    // Reset state
    aviPath = path;

    aviFps =
        fps > 0
        ? (uint32_t)fps
        : 1;

    frameCount      = 0;
    writeFailed     = false;
    sizeLimitHit    = false;
    maxFrameSize    = 0;
    totalFrameBytes = 0;
    moviDataBytes   = 0;

    aviWidth        = 0;
    aviHeight       = 0;

    headerWritten   = false;

    recordingStartEpoch =
        time(nullptr);

    struct tm startTm;

    recordingStartTimeValid =
        localtime_r(
            &recordingStartEpoch,
            &startTm
        ) != nullptr &&
        (startTm.tm_year + 1900) > 2020;


    // Remove an existing file with the same name
    if (STORAGE.exists(path.c_str())) {
        STORAGE.remove(path.c_str());
    }


    if (!aviFile.openWrite(
            path,
            cfg_recording_encryption != 0
        )) {

        consoleWrite(
            "REC",
            "ERROR | AVI cannot open | " + path
        );

        return;
    }


    // Header is written with the first frame,
    // because width and height are taken directly
    // from camera_fb_t.
}


// =============================================================
// ADD ONE JPEG FRAME
// =============================================================

void aviAddFrame()
{
    if (!aviFile)
        return;


    camera_fb_t *fb =
        esp_camera_fb_get();


    if (!fb) {

        Serial.println(
            "AVI: camera frame failed"
        );

        return;
    }


    if (fb->format != PIXFORMAT_JPEG) {

        Serial.println(
            "AVI: frame is not JPEG"
        );

        esp_camera_fb_return(fb);

        return;
    }


    // First frame determines AVI dimensions
    if (!headerWritten) {

        if (!writeAviHeader(
                (uint16_t)fb->width,
                (uint16_t)fb->height,
                aviFps
            )) {

            writeFailed = true;

            esp_camera_fb_return(fb);

            return;
        }
    }


    // Resolution must not change inside one AVI file.
    if (fb->width != aviWidth ||
        fb->height != aviHeight) {

        Serial.println(
            "AVI: frame resolution changed - frame skipped"
        );

        esp_camera_fb_return(fb);

        return;
    }


    uint32_t jpegSize =
        (uint32_t)fb->len;

    uint32_t padding =
        jpegSize & 1U;

    uint64_t nextFileSize =
        (uint64_t)aviFile.size()
        + 8ULL
        + jpegSize
        + padding;


    if (nextFileSize >
        AVI_MAX_FILE_SIZE) {

        Serial.println(
            "AVI: classic AVI size limit reached"
        );

        sizeLimitHit = true;

        esp_camera_fb_return(fb);

        return;
    }


    // ---------------------------------------------------------
    // Frame chunk header
    //
    // 00dc = stream 00, compressed video
    // ---------------------------------------------------------

    uint8_t chunkHeader[8];

    putFourCC(
        chunkHeader,
        0,
        "00dc"
    );

    putU32(
        chunkHeader,
        4,
        jpegSize
    );


    if (aviFile.write(
            chunkHeader,
            sizeof(chunkHeader)
        ) != sizeof(chunkHeader)) {

        Serial.println(
            "AVI: frame header write failed"
        );

        writeFailed = true;

        esp_camera_fb_return(fb);

        return;
    }


    // ---------------------------------------------------------
    // JPEG payload
    // ---------------------------------------------------------

    size_t written =
        aviFile.write(
            fb->buf,
            jpegSize
        );


    if (written != jpegSize) {

        Serial.println(
            "AVI: JPEG write failed"
        );

        writeFailed = true;

        esp_camera_fb_return(fb);

        return;
    }


    // RIFF chunks must be padded to an even byte boundary.
    if (padding) {

        const uint8_t zero = 0;

        if (aviFile.write(
                &zero,
                1
            ) != 1) {

            Serial.println(
                "AVI: padding write failed"
            );

            writeFailed = true;

        esp_camera_fb_return(fb);

            return;
        }
    }


    // ---------------------------------------------------------
    // Update statistics
    // ---------------------------------------------------------

    frameCount++;

    // image_only keeps its motion/release state from the same JPEGs that are
    // already being recorded. Skip the very first frame so the optimized
    // wake->first-frame path and its timing metric remain untouched.
    if (frameCount > 1U) {
        imageMotionObserveRecordingJpeg(
            fb->buf,
            fb->len,
            (uint16_t)fb->width,
            (uint16_t)fb->height
        );
    }

    totalFrameBytes +=
        jpegSize;

    moviDataBytes +=
        8U +
        jpegSize +
        padding;


    if (jpegSize >
        maxFrameSize) {

        maxFrameSize =
            jpegSize;
    }


    esp_camera_fb_return(fb);
}


// =============================================================
// FINISH AVI FILE
// =============================================================

bool aviEnd()
{
    if (!aviFile)
        return true;


    if (writeFailed) {

        Serial.println(
            "AVI: recording failed - removing incomplete file"
        );

        aviFile.close();

        if (aviPath.length()) {

            STORAGE.remove(
                aviPath.c_str()
            );

            String srtPath =
                makeSrtPath(
                    aviPath
                );

            if (STORAGE.exists(srtPath.c_str())) {
                STORAGE.remove(
                    srtPath.c_str()
                );
            }
        }

        aviPath = "";

        return false;
    }


    // No valid frame was ever written.
    if (!headerWritten ||
        frameCount == 0) {

        Serial.println(
            "AVI: no frames - removing empty file"
        );

        aviFile.close();

        if (aviPath.length()) {

            STORAGE.remove(
                aviPath.c_str()
            );

            String srtPath =
                makeSrtPath(aviPath);

            if (STORAGE.exists(srtPath.c_str())) {
                STORAGE.remove(
                    srtPath.c_str()
                );
            }
        }

        aviPath = "";

        return false;
    }


    // ---------------------------------------------------------
    // Calculate final header values
    // ---------------------------------------------------------

    uint64_t fileSize64 =
        aviFile.size();

    if (fileSize64 >
        0xFFFFFFFFULL) {

        Serial.println(
            "AVI: file too large for classic RIFF AVI"
        );

        aviFile.close();

        if (aviPath.length()) {
            STORAGE.remove(
                aviPath.c_str()
            );
        }

        aviPath = "";

        return false;
    }


    uint32_t fileSize =
        (uint32_t)fileSize64;

    uint32_t riffSize =
        fileSize - 8U;

    uint32_t moviListSize =
        4U + moviDataBytes;


    uint64_t maxBytesPerSec64 =
        (uint64_t)maxFrameSize *
        (uint64_t)aviFps;

    uint32_t maxBytesPerSec =
        maxBytesPerSec64 > 0xFFFFFFFFULL
        ? 0xFFFFFFFFUL
        : (uint32_t)maxBytesPerSec64;


    // ---------------------------------------------------------
    // Patch AVI header
    // ---------------------------------------------------------

    bool ok = true;

    ok &= patchU32(
        OFFSET_RIFF_SIZE,
        riffSize
    );

    ok &= patchU32(
        OFFSET_MAX_BYTES_PER_SEC,
        maxBytesPerSec
    );

    ok &= patchU32(
        OFFSET_TOTAL_FRAMES,
        frameCount
    );

    ok &= patchU32(
        OFFSET_MAIN_BUFFER_SIZE,
        maxFrameSize
    );

    ok &= patchU32(
        OFFSET_STREAM_LENGTH,
        frameCount
    );

    ok &= patchU32(
        OFFSET_STREAM_BUFFER_SIZE,
        maxFrameSize
    );

    ok &= patchU32(
        OFFSET_BITMAP_SIZE_IMAGE,
        maxFrameSize
    );

    ok &= patchU32(
        OFFSET_MOVI_LIST_SIZE,
        moviListSize
    );


    aviFile.flush();

    if (aviFile.failed())
        ok = false;

    if (!aviFile.closeChecked())
        ok = false;


    if (ok) {

        float seconds =
            (float)frameCount /
            (float)aviFps;

        bool srtOk = true;

        if (cfg_timestamp_enabled) {
            srtOk =
                writeSrtFile();
        }

        char summary[192];

        snprintf(
            summary,
            sizeof(summary),
            "STOP | AVI | frames=%lu | duration=%.1f s | maxJPEG=%.1f KB%s",
            (unsigned long)frameCount,
            seconds,
            (double)maxFrameSize / 1024.0,
            cfg_timestamp_enabled
                ? (srtOk ? " | SRT=ok" : " | SRT=failed")
                : ""
        );

        consoleWrite(
            "REC",
            String(summary)
        );

        logWrite(
            "Recording " +
            String(summary)
        );

    } else {

        consoleWrite(
            "REC",
            "ERROR | AVI finalization/header patch"
        );
    }


    bool finalOk =
        ok;

    if (!finalOk && aviPath.length()) {

        STORAGE.remove(
            aviPath.c_str()
        );

        String srtPath =
            makeSrtPath(
                aviPath
            );

        if (STORAGE.exists(srtPath.c_str())) {
            STORAGE.remove(
                srtPath.c_str()
            );
        }
    }

    aviPath = "";

    return finalOk;
}


// =============================================================
// STATUS HELPERS
// =============================================================

bool aviIsOpen()
{
    return
        (bool)aviFile &&
        !writeFailed;
}


bool aviIsHealthy()
{
    return
        (bool)aviFile &&
        !writeFailed;
}


bool aviHitSizeLimit()
{
    return sizeLimitHit;
}


uint32_t aviGetFrameCount()
{
    return frameCount;
}


uint64_t aviGetBytesWritten()
{
    if (!aviFile || !headerWritten)
        return 0;

    // Uses counters already maintained while writing frames.
    // No SD metadata query is required for the segment-size check.
    return
        (uint64_t)AVI_HEADER_SIZE +
        (uint64_t)moviDataBytes;
}
