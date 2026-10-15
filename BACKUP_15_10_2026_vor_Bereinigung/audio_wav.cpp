#include "audio_wav.h"

#include "board_config.h"
#include "recording_storage.h"
#include "storage_guard.h"

#include <math.h>
#include <limits.h>
#include <string.h>

namespace {

static void putU16LE(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void putU32LE(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFUL);
    p[1] = (uint8_t)((value >> 8) & 0xFFUL);
    p[2] = (uint8_t)((value >> 16) & 0xFFUL);
    p[3] = (uint8_t)((value >> 24) & 0xFFUL);
}

static bool buildWavHeader(
    uint8_t header[44],
    const AudioFormat &format,
    uint32_t pcmBytes,
    String &error
)
{
    error = "";

    uint32_t bytesPerSample = format.bitsPerSample / 8U;

    if (
        bytesPerSample == 0 ||
        format.channels == 0 ||
        format.sampleRate == 0
    ) {
        error = "invalid WAV audio format";
        return false;
    }

    uint64_t byteRate64 =
        (uint64_t)format.sampleRate *
        (uint64_t)format.channels *
        (uint64_t)bytesPerSample;

    uint64_t riffSize64 =
        36ULL +
        (uint64_t)pcmBytes;

    if (
        byteRate64 > 0xFFFFFFFFULL ||
        riffSize64 > 0xFFFFFFFFULL
    ) {
        error = "WAV size/rate exceeds classic RIFF limits";
        return false;
    }

    memset(header, 0, 44);

    memcpy(header + 0, "RIFF", 4);
    putU32LE(header + 4, (uint32_t)riffSize64);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    putU32LE(header + 16, 16U);
    putU16LE(header + 20, 1U); // PCM
    putU16LE(header + 22, format.channels);
    putU32LE(header + 24, format.sampleRate);
    putU32LE(header + 28, (uint32_t)byteRate64);
    putU16LE(
        header + 32,
        (uint16_t)(format.channels * bytesPerSample)
    );
    putU16LE(header + 34, format.bitsPerSample);
    memcpy(header + 36, "data", 4);
    putU32LE(header + 40, pcmBytes);

    return true;
}

} // namespace


bool audioWavRecordTest(
    const String &path,
    uint32_t durationMs,
    const AudioFormat &format,
    bool encrypt,
    AudioWavResult &result,
    String &error,
    AudioWavServiceCallback serviceCallback
)
{
    result = {};
    error = "";

    if (!path.length() || path[0] != '/') {
        error = "invalid WAV test path";
        return false;
    }

    String formatError;

    if (!audioCaptureFormatSupported(format, formatError)) {
        error = formatError;
        return false;
    }

    uint32_t bytesPerSample = format.bitsPerSample / 8U;

    uint64_t byteRate =
        (uint64_t)format.sampleRate *
        (uint64_t)format.channels *
        (uint64_t)bytesPerSample;

    uint64_t targetBytes64 =
        (byteRate * (uint64_t)durationMs) /
        1000ULL;

    uint32_t blockAlign =
        (uint32_t)format.channels *
        bytesPerSample;

    if (blockAlign == 0 || targetBytes64 == 0) {
        error = "audio test duration/format produces no PCM data";
        return false;
    }

    targetBytes64 -=
        targetBytes64 % blockAlign;

    if (targetBytes64 > 0xFFFFFF00ULL) {
        error = "audio test is too large for classic WAV";
        return false;
    }

    // The WebConfig audio diagnostic deliberately owns g_storageLocked while
    // this function runs. storageFreeBytes() therefore returns 0 by design and
    // cannot be used for this owner-side reserve check. Query the already-mounted
    // backend directly here; the caller has already excluded competing storage
    // maintenance and NEW recording starts before taking the lock.
    uint64_t totalBytes = STORAGE.totalBytes();
    uint64_t usedBytes = STORAGE.usedBytes();
    uint64_t freeBytes =
        usedBytes < totalBytes
        ? totalBytes - usedBytes
        : 0;

    uint64_t reserveBytes = storageReserveBytes();
    uint64_t requiredBytes = targetBytes64 + 128ULL * 1024ULL;

    if (
        freeBytes <= reserveBytes ||
        freeBytes - reserveBytes < requiredBytes
    ) {
        error = "not enough free SD space above SensorForge reserve";
        return false;
    }

    if (STORAGE.exists(path.c_str()))
        STORAGE.remove(path.c_str());

    RecordingStorageFile wavFile;

    if (!wavFile.openWrite(path, encrypt)) {
        error = "cannot open WAV test file";
        return false;
    }

    uint8_t header[44];

    if (!buildWavHeader(header, format, 0, error)) {
        wavFile.close();
        STORAGE.remove(path.c_str());
        return false;
    }

    if (wavFile.write(header, sizeof(header)) != sizeof(header)) {
        error = "cannot write WAV header";
        wavFile.close();
        STORAGE.remove(path.c_str());
        return false;
    }

    String captureError;

    if (!audioCaptureStart(format, captureError)) {
        error = "audio capture start failed: " + captureError;
        wavFile.close();
        STORAGE.remove(path.c_str());
        return false;
    }

    static const size_t IO_BUFFER_BYTES = 8U * 1024U;
    uint8_t *ioBuffer =
        (uint8_t *)malloc(IO_BUFFER_BYTES);

    if (!ioBuffer) {
        error = "audio WAV I/O buffer allocation failed";
        audioCaptureStop();
        wavFile.close();
        STORAGE.remove(path.c_str());
        return false;
    }

    uint64_t writtenPcm = 0;
    uint64_t sumSquares = 0;
    uint64_t rmsSamples = 0;
    int32_t peakAbs = 0;

    uint32_t captureStartedMs = millis();
    uint32_t lastDataMs = captureStartedMs;
    bool ok = true;

    while (writtenPcm < targetBytes64) {
        size_t wanted =
            (targetBytes64 - writtenPcm) < IO_BUFFER_BYTES
            ? (size_t)(targetBytes64 - writtenPcm)
            : IO_BUFFER_BYTES;

        wanted -= wanted % blockAlign;

        size_t got =
            audioCaptureRead(
                ioBuffer,
                wanted,
                250
            );

        if (got == 0) {
            if (serviceCallback)
                serviceCallback();

            if ((uint32_t)(millis() - lastDataMs) > 2000UL) {
                error = "audio capture produced no data for 2 seconds";
                ok = false;
                break;
            }

            continue;
        }

        lastDataMs = millis();

        got -= got % blockAlign;

        if (got == 0)
            continue;

        if (format.bitsPerSample == 16) {
            for (size_t i = 0; i + 1 < got; i += 2) {
                int16_t sample =
                    (int16_t)(
                        (uint16_t)ioBuffer[i] |
                        ((uint16_t)ioBuffer[i + 1] << 8)
                    );

                int32_t magnitude =
                    sample == INT16_MIN
                    ? 32768
                    : abs((int)sample);

                if (magnitude > peakAbs)
                    peakAbs = magnitude;

                int32_t sample32 = sample;
                sumSquares +=
                    (uint64_t)(sample32 * sample32);
                rmsSamples++;
            }
        }

        if (wavFile.write(ioBuffer, got) != got) {
            error = "WAV PCM write failed";
            ok = false;
            break;
        }

        writtenPcm += (uint64_t)got;

        AudioCaptureStats liveStats = audioCaptureStats();

        if (liveStats.bytesDropped != 0) {
            error =
                "audio PSRAM buffer overrun (" +
                String((unsigned long)liveStats.bytesDropped) +
                " bytes dropped)";
            ok = false;
            break;
        }

        if (serviceCallback)
            serviceCallback();
    }

    AudioCaptureStats finalStats = audioCaptureStats();
    audioCaptureStop();

    result.captureMs =
        (uint32_t)(millis() - captureStartedMs);
    result.pcmBytes = writtenPcm;
    result.peakAbs16 = peakAbs;
    result.droppedBytes = finalStats.bytesDropped;
    result.bufferCapacity = finalStats.bufferCapacity;
    result.bufferHighWater = finalStats.bufferHighWater;

    if (rmsSamples) {
        result.rms16 =
            sqrtf(
                (float)(
                    (double)sumSquares /
                    (double)rmsSamples
                )
            );
    }

    free(ioBuffer);

    if (
        ok &&
        writtenPcm == targetBytes64 &&
        finalStats.bytesDropped == 0
    ) {
        if (!buildWavHeader(
                header,
                format,
                (uint32_t)writtenPcm,
                error
            )) {
            ok = false;
        }
    } else if (ok) {
        error = "audio test ended before requested PCM duration";
        ok = false;
    }

    if (ok) {
        if (!wavFile.seek(0)) {
            error = "cannot seek to WAV header";
            ok = false;
        } else if (
            wavFile.write(header, sizeof(header)) !=
            sizeof(header)
        ) {
            error = "cannot finalize WAV header";
            ok = false;
        }
    }

    if (ok)
        wavFile.flush();

    bool closeOk =
        wavFile.closeChecked();

    if (!closeOk && ok) {
        error = "WAV final close failed";
        ok = false;
    }

    if (!ok)
        STORAGE.remove(path.c_str());

    return ok;
}
