#include "recorder.h"

#include "config.h"
#include "board_config.h"
#include "avi_writer.h"
#include "mkv_writer.h"
#include "logger.h"
#include "storage_guard.h"
#include "recording_storage.h"
#include "webconfig.h"

#include <FS.h>
#include <esp_timer.h>


enum RecorderType {
    RECORDER_NONE = 0,
    RECORDER_AVI,
    RECORDER_MKV
};


static RecorderType activeRecorder =
    RECORDER_NONE;

static String activeFinalPath;
static String activeTempPath;

static bool detailedFrameTimingEnabled = false;
static RecorderFrameTiming lastFrameTiming = {};


// Public global start lock declared in recorder.h.
// This protects only NEW recording starts; it never stops an active recording.
volatile bool g_recordingStartBlocked =
    false;


static String tempPathFor(
    const String &finalPath
)
{
    return finalPath + ".part";
}


static String srtPathForAvi(
    const String &aviPath
)
{
    if (aviPath.length() < 4)
        return "";

    return
        aviPath.substring(
            0,
            aviPath.length() - 4
        ) +
        ".srt";
}


static bool renameWithRetries(
    const String &from,
    const String &to
)
{
    if (!STORAGE.exists(from.c_str()))
        return false;

    if (STORAGE.exists(to.c_str())) {

        Serial.println(
            "Recorder: final file already exists: " +
            to
        );

        return false;
    }

    for (
        int attempt = 0;
        attempt < 3;
        ++attempt
    ) {

        if (STORAGE.rename(
                from.c_str(),
                to.c_str()
            )) {
            return true;
        }

        delay(20);
    }

    return false;
}


static void removeStaleTempFiles()
{
    if (
        activeTempPath.length() &&
        STORAGE.exists(
            activeTempPath.c_str()
        )
    ) {
        STORAGE.remove(
            activeTempPath.c_str()
        );
    }

    if (
        cfg_recording_format == "avi" &&
        activeTempPath.length() >= 9
    ) {
        // foo.avi.part -> foo.srt.part
        String tempSrt =
            activeTempPath.substring(
                0,
                activeTempPath.length() - 9
            ) +
            ".srt.part";

        if (STORAGE.exists(tempSrt.c_str())) {
            STORAGE.remove(
                tempSrt.c_str()
            );
        }
    }
}


static bool promoteRecording()
{
    if (
        !activeTempPath.length() ||
        !activeFinalPath.length()
    ) {
        return false;
    }

    if (!renameWithRetries(
            activeTempPath,
            activeFinalPath
        )) {

        Serial.println(
            "Recorder: cannot promote temporary recording"
        );

        return false;
    }

    if (cfg_recording_format == "avi") {

        String finalSrt =
            srtPathForAvi(
                activeFinalPath
            );

        String tempSrt =
            finalSrt +
            ".part";

        if (STORAGE.exists(tempSrt.c_str())) {

            if (!renameWithRetries(
                    tempSrt,
                    finalSrt
                )) {

                // The AVI itself is valid. Subtitle failure is
                // non-fatal; the .part is removed at next boot.
                Serial.println(
                    "Recorder: SRT promotion failed"
                );
            }
        }
    }

    return true;
}


bool recorderStart(
    const String &fullpath,
    int fps
)
{
    // Central start gates. Storage maintenance and the live camera preview
    // may block NEW recording starts. Check before touching recorder state so
    // an already-running recording is never interrupted by a new gate.
    if (
        g_recordingStartBlocked ||
        g_storageLocked ||
        webConfigCameraPreviewActive()
    ) {
        return false;
    }


    if (activeRecorder != RECORDER_NONE) {
        recorderEnd();
    }

    activeFinalPath =
        fullpath;

    activeTempPath =
        tempPathFor(fullpath);

    removeStaleTempFiles();

    if (STORAGE.exists(
            activeFinalPath.c_str()
        )) {

        Serial.println(
            "Recorder: refusing to overwrite existing file: " +
            activeFinalPath
        );

        activeFinalPath = "";
        activeTempPath = "";

        return false;
    }

    if (cfg_recording_format == "avi") {

        if (cfg_audio_enabled) {
            consoleWrite(
                "REC",
                "WARN | audio_enabled=1 but AVI recording is video-only; use MKV for production audio"
            );

            logWrite(
                "Recording WARN | audio_enabled=1 but AVI recording is video-only; use MKV for production audio"
            );
        }

        aviStart(
            activeTempPath,
            fps
        );

        if (!aviIsOpen()) {

            Serial.println(
                "Recorder: AVI start failed"
            );

            activeFinalPath = "";
            activeTempPath = "";

            return false;
        }

        activeRecorder =
            RECORDER_AVI;

        return true;
    }

    if (cfg_recording_format == "mkv") {

        mkvStart(
            activeTempPath,
            fps
        );

        if (!mkvIsOpen()) {

            Serial.println(
                "Recorder: MKV start failed"
            );

            activeFinalPath = "";
            activeTempPath = "";

            return false;
        }

        activeRecorder =
            RECORDER_MKV;

        return true;
    }

    consoleWrite(
        "REC",
        "ERROR | unsupported format | " +
        cfg_recording_format
    );

    activeFinalPath = "";
    activeTempPath = "";

    return false;
}


void recorderAddFrame()
{
    uint64_t callStartUs =
        detailedFrameTimingEnabled
        ? (uint64_t)esp_timer_get_time()
        : 0ULL;

    lastFrameTiming = {};

    if (detailedFrameTimingEnabled)
        recordingStorageBeginFrameDiagnostics();

    switch (activeRecorder) {
        case RECORDER_AVI:
            aviAddFrame();
            break;

        case RECORDER_MKV:
            mkvAddFrame();
            break;

        default:
            break;
    }

    if (!detailedFrameTimingEnabled)
        return;

    uint64_t callDoneUs =
        (uint64_t)esp_timer_get_time();

    lastFrameTiming.valid = true;
    uint64_t totalUs64 =
        callDoneUs >= callStartUs
        ? callDoneUs - callStartUs
        : 0ULL;

    lastFrameTiming.totalUs =
        totalUs64 > 0xFFFFFFFFULL
        ? 0xFFFFFFFFUL
        : (uint32_t)totalUs64;

    RecordingStorageFrameDiagnostics storageTiming = {};
    if (recordingStorageGetFrameDiagnostics(storageTiming)) {
        lastFrameTiming.storageIoValid = true;
        lastFrameTiming.storageWriteCalls = storageTiming.writeCalls;
        lastFrameTiming.storageWriteBytes = storageTiming.writeBytes;
        lastFrameTiming.storageWriteTotalUs = storageTiming.writeTotalUs;
        lastFrameTiming.storageWriteMaxUs = storageTiming.writeMaxUs;
        lastFrameTiming.storageWriteMaxBytes = storageTiming.writeMaxBytes;
        lastFrameTiming.storageSlowWriteCalls = storageTiming.slowWriteCalls;
        lastFrameTiming.storageSeekCalls = storageTiming.seekCalls;
        lastFrameTiming.storageSeekTotalUs = storageTiming.seekTotalUs;
        lastFrameTiming.storageSeekMaxUs = storageTiming.seekMaxUs;
    }

    if (activeRecorder == RECORDER_MKV) {
        MkvFrameTiming mkvTiming = {};

        if (mkvGetLastFrameTiming(mkvTiming)) {
            lastFrameTiming.stageBreakdownValid = true;
            lastFrameTiming.cameraUs = mkvTiming.cameraUs;
            lastFrameTiming.headerUs = mkvTiming.headerUs;
            lastFrameTiming.audioReadUs = mkvTiming.audioReadUs;
            lastFrameTiming.audioWriteUs = mkvTiming.audioWriteUs;
            lastFrameTiming.clusterUs = mkvTiming.clusterUs;
            lastFrameTiming.subtitleUs = mkvTiming.subtitleUs;
            lastFrameTiming.videoWriteUs = mkvTiming.videoWriteUs;
            lastFrameTiming.imageAnalysisUs = mkvTiming.imageAnalysisUs;
        }
    }

    uint64_t accountedUs =
        (uint64_t)lastFrameTiming.cameraUs +
        (uint64_t)lastFrameTiming.headerUs +
        (uint64_t)lastFrameTiming.audioReadUs +
        (uint64_t)lastFrameTiming.audioWriteUs +
        (uint64_t)lastFrameTiming.clusterUs +
        (uint64_t)lastFrameTiming.subtitleUs +
        (uint64_t)lastFrameTiming.videoWriteUs +
        (uint64_t)lastFrameTiming.imageAnalysisUs;

    lastFrameTiming.otherUs =
        accountedUs < (uint64_t)lastFrameTiming.totalUs
        ? lastFrameTiming.totalUs - (uint32_t)accountedUs
        : 0;
}


void recorderSetDetailedFrameTimingEnabled(bool enabled)
{
    detailedFrameTimingEnabled = enabled;
    lastFrameTiming = {};
    recordingStorageSetFrameDiagnosticsEnabled(enabled);
    mkvSetDetailedFrameTimingEnabled(enabled);
}


bool recorderGetLastFrameTiming(RecorderFrameTiming &timing)
{
    timing = lastFrameTiming;
    return
        detailedFrameTimingEnabled &&
        lastFrameTiming.valid;
}


bool recorderEnd()
{
    if (activeRecorder == RECORDER_NONE)
        return true;

    bool finalized = false;

    switch (activeRecorder) {
        case RECORDER_AVI:
            finalized = aviEnd();
            break;

        case RECORDER_MKV:
            finalized = mkvEnd();
            break;

        default:
            finalized = false;
            break;
    }

    bool promoted = false;

    if (finalized) {
        promoted =
            promoteRecording();
    }

    if (
        !finalized &&
        activeTempPath.length() &&
        STORAGE.exists(
            activeTempPath.c_str()
        )
    ) {
        STORAGE.remove(
            activeTempPath.c_str()
        );
    }

    activeRecorder =
        RECORDER_NONE;

    activeFinalPath = "";
    activeTempPath = "";

    return
        finalized &&
        promoted;
}


bool recorderIsOpen()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return aviIsOpen();

        case RECORDER_MKV:
            return mkvIsOpen();

        default:
            return false;
    }
}


bool recorderIsHealthy()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return aviIsHealthy();

        case RECORDER_MKV:
            return mkvIsHealthy();

        default:
            return true;
    }
}


bool recorderHitSizeLimit()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return aviHitSizeLimit();

        case RECORDER_MKV:
            return mkvHitSizeLimit();

        default:
            return false;
    }
}


uint32_t recorderGetFrameCount()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return aviGetFrameCount();

        case RECORDER_MKV:
            return mkvGetFrameCount();

        default:
            return 0;
    }
}


uint64_t recorderGetBytesWritten()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return aviGetBytesWritten();

        case RECORDER_MKV:
            return mkvGetBytesWritten();

        default:
            return 0;
    }
}


String recorderGetFormat()
{
    switch (activeRecorder) {
        case RECORDER_AVI:
            return "avi";

        case RECORDER_MKV:
            return "mkv";

        default:
            return "";
    }
}


String recorderGetFinalPath()
{
    return activeFinalPath;
}
