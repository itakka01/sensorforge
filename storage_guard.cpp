#include "storage_guard.h"

#include "board_config.h"
#include "config.h"
#include "logger.h"

#include <FS.h>
#include <esp_task_wdt.h>


// Cooperative global storage gate declared in storage_guard.h.
volatile bool g_storageLocked = false;


// =============================================================
// ROLLOVER TUNING
// =============================================================
//
// When the configured reserve is reached, rollover deletes several
// oldest recordings in one batch instead of repeatedly scanning the
// filesystem for a single file.
//
// The extra headroom above min_free_space_mb acts as hysteresis so the
// rollover path is not entered again at the next recording.
static const uint8_t ROLLOVER_BATCH_CANDIDATES =
    12;

static const int ROLLOVER_MAX_DELETE_FILES =
    100;

static const uint64_t ROLLOVER_MIN_HEADROOM_BYTES =
    64ULL * 1024ULL * 1024ULL;

static const uint64_t ROLLOVER_MAX_HEADROOM_BYTES =
    256ULL * 1024ULL * 1024ULL;


static void serviceStorageLongOperation()
{
    esp_task_wdt_reset();
    yield();
}


static bool isDateFolderName(
    const String &name
)
{
    if (name.length() != 8)
        return false;

    for (size_t i = 0; i < 8; ++i) {
        if (!isDigit(name[i]))
            return false;
    }

    return true;
}


static String baseNameOnly(
    const String &name
)
{
    int slash =
        name.lastIndexOf('/');

    if (slash >= 0)
        return name.substring(slash + 1);

    return name;
}


static String makeFullPath(
    const String &parent,
    const String &name
)
{
    if (name.startsWith("/"))
        return name;

    if (parent == "/")
        return "/" + name;

    return parent + "/" + name;
}


static bool isVideoName(
    const String &name
)
{
    String lower = name;
    lower.toLowerCase();

    return
        lower.endsWith(".avi") ||
        lower.endsWith(".mkv") ||
        lower.endsWith(".jpg") ||
        lower.endsWith(".jpeg");
}


static bool isTemporaryRecordingName(
    const String &name
)
{
    String lower = name;
    lower.toLowerCase();

    return
        lower.endsWith(".avi.part") ||
        lower.endsWith(".mkv.part") ||
        lower.endsWith(".srt.part") ||
        lower.endsWith(".jpg.part") ||
        lower.endsWith(".jpeg.part");
}


uint64_t storageFreeBytes()
{
    if (g_storageLocked)
        return 0;

    uint64_t total =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    if (used >= total)
        return 0;

    return total - used;
}


uint64_t storageReserveBytes()
{
    if (cfg_min_free_space_mb <= 0)
        return 0;

    return
        (uint64_t)cfg_min_free_space_mb *
        1024ULL *
        1024ULL;
}


bool storageHasRequiredFreeSpace()
{
    if (g_storageLocked)
        return false;

    uint64_t reserve =
        storageReserveBytes();

    if (reserve == 0)
        return true;

    return
        storageFreeBytes() >=
        reserve;
}


static uint64_t rolloverTargetFreeBytes()
{
    uint64_t reserve =
        storageReserveBytes();

    if (reserve == 0)
        return 0;


    // Prefer approximately one additional reserve-sized block as
    // headroom, bounded so very small/large configurations stay sane.
    uint64_t headroom =
        reserve;

    if (
        headroom <
        ROLLOVER_MIN_HEADROOM_BYTES
    ) {
        headroom =
            ROLLOVER_MIN_HEADROOM_BYTES;
    }

    if (
        headroom >
        ROLLOVER_MAX_HEADROOM_BYTES
    ) {
        headroom =
            ROLLOVER_MAX_HEADROOM_BYTES;
    }


    // Do not reserve an excessive part of unusually small cards.
    uint64_t total =
        STORAGE.totalBytes();

    if (total <= reserve)
        return reserve;

    uint64_t remainingCapacity =
        total -
        reserve;

    uint64_t smallCardCap =
        remainingCapacity /
        4ULL;

    if (
        smallCardCap > 0 &&
        headroom >
        smallCardCap
    ) {
        headroom =
            smallCardCap;
    }


    return
        reserve +
        headroom;
}


static bool findOldestDateFolder(
    String &folderName
)
{
    File root =
        STORAGE.open(
            "/",
            FILE_READ
        );

    if (!root || !root.isDirectory()) {
        if (root)
            root.close();
        return false;
    }

    String oldest;

    uint16_t scanned =
        0;

    File file =
        root.openNextFile();

    while (file) {

        if (file.isDirectory()) {

            String name =
                baseNameOnly(
                    String(file.name())
                );

            if (
                isDateFolderName(name) &&
                (
                    !oldest.length() ||
                    name.compareTo(oldest) < 0
                )
            ) {
                oldest = name;
            }
        }

        file.close();

        scanned++;

        if ((scanned & 0x0FU) == 0)
            serviceStorageLongOperation();

        file = root.openNextFile();
    }

    root.close();

    serviceStorageLongOperation();

    if (!oldest.length())
        return false;

    folderName = oldest;
    return true;
}


struct RolloverCandidate {
    String name;
    uint64_t bytes;
};


static uint8_t collectOldestVideosInFolder(
    const String &folderPath,
    RolloverCandidate *candidates,
    uint8_t capacity
)
{
    if (
        candidates == nullptr ||
        capacity == 0
    ) {
        return 0;
    }


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );

    if (!root || !root.isDirectory()) {
        if (root)
            root.close();

        return 0;
    }


    uint8_t count =
        0;

    uint16_t scanned =
        0;

    File file =
        root.openNextFile();


    while (file) {

        if (!file.isDirectory()) {

            String name =
                baseNameOnly(
                    String(file.name())
                );


            if (isVideoName(name)) {

                uint64_t bytes =
                    file.size();


                // Keep only the N lexicographically oldest recording
                // filenames. Timestamp filenames sort chronologically.
                uint8_t insertAt =
                    count;

                bool accept =
                    count <
                    capacity;


                if (!accept) {

                    if (
                        name.compareTo(
                            candidates[
                                count - 1
                            ].name
                        ) < 0
                    ) {

                        insertAt =
                            count - 1;

                        accept =
                            true;
                    }
                }


                if (accept) {

                    if (count < capacity) {
                        count++;
                    }


                    candidates[
                        insertAt
                    ].name =
                        name;

                    candidates[
                        insertAt
                    ].bytes =
                        bytes;


                    while (
                        insertAt > 0 &&
                        candidates[
                            insertAt
                        ].name.compareTo(
                            candidates[
                                insertAt - 1
                            ].name
                        ) < 0
                    ) {

                        RolloverCandidate tmp =
                            candidates[
                                insertAt - 1
                            ];

                        candidates[
                            insertAt - 1
                        ] =
                            candidates[
                                insertAt
                            ];

                        candidates[
                            insertAt
                        ] =
                            tmp;

                        insertAt--;
                    }
                }
            }
        }


        file.close();

        scanned++;

        if ((scanned & 0x0FU) == 0)
            serviceStorageLongOperation();

        file =
            root.openNextFile();
    }


    root.close();

    serviceStorageLongOperation();

    return count;
}



static bool directoryIsEmpty(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!root || !root.isDirectory()) {
        if (root)
            root.close();
        return false;
    }

    File file =
        root.openNextFile();

    bool empty = !file;

    if (file)
        file.close();

    root.close();

    return empty;
}


static bool deleteRecordingPair(
    const String &videoPath
)
{
    String lower = videoPath;
    lower.toLowerCase();

    if (!STORAGE.remove(videoPath.c_str()))
        return false;

    if (lower.endsWith(".avi")) {

        String srtPath =
            videoPath.substring(
                0,
                videoPath.length() - 4
            ) +
            ".srt";

        if (STORAGE.exists(srtPath.c_str())) {
            STORAGE.remove(
                srtPath.c_str()
            );
        }
    }

    Serial.println(
        "Storage rollover deleted: " +
        videoPath
    );

    logWrite(
        "Storage rollover deleted: " +
        videoPath
    );

    serviceStorageLongOperation();

    return true;
}


static int deleteOldestBatchFromFolder(
    const String &folderPath,
    uint64_t targetFreeBytes,
    uint64_t &estimatedFreeBytes,
    int maxDeleteCount
)
{
    if (maxDeleteCount <= 0)
        return 0;


    RolloverCandidate candidates[
        ROLLOVER_BATCH_CANDIDATES
    ];

    uint8_t count =
        collectOldestVideosInFolder(
            folderPath,
            candidates,
            ROLLOVER_BATCH_CANDIDATES
        );


    if (count == 0)
        return 0;


    int deleted =
        0;


    for (
        uint8_t i = 0;
        i < count &&
        deleted < maxDeleteCount;
        ++i
    ) {

        String videoPath =
            makeFullPath(
                folderPath,
                candidates[i].name
            );


        if (!deleteRecordingPair(
                videoPath
            )) {

            return -1;
        }


        // AVI sidecar SRT files are deleted by deleteRecordingPair().
        // Only the known video size is added here, which deliberately
        // underestimates reclaimed space and is therefore safe.
        estimatedFreeBytes +=
            candidates[i].bytes;

        deleted++;


        if (
            estimatedFreeBytes >=
            targetFreeBytes
        ) {
            break;
        }
    }


    return deleted;
}



bool storagePrepareForRecording()
{
    if (g_storageLocked)
        return false;

    uint64_t reserveBytes =
        storageReserveBytes();


    if (reserveBytes == 0)
        return true;


    // One authoritative free-space query on entry.
    uint64_t freeBytes =
        storageFreeBytes();


    if (
        freeBytes >=
        reserveBytes
    ) {
        return true;
    }


    uint64_t freeMb =
        freeBytes /
        (1024ULL * 1024ULL);

    Serial.printf(
        "Storage reserve reached: free=%llu MB, reserve=%d MB, action=%s\n",
        (unsigned long long)freeMb,
        cfg_min_free_space_mb,
        cfg_disk_full_action.c_str()
    );

    logWrite(
        "Storage reserve reached: free=" +
        String((unsigned long)freeMb) +
        " MB, reserve=" +
        String(cfg_min_free_space_mb) +
        " MB, action=" +
        cfg_disk_full_action
    );


    if (
        cfg_disk_full_action ==
        "stop"
    ) {
        return false;
    }


    uint64_t targetFreeBytes =
        rolloverTargetFreeBytes();

    uint64_t targetFreeMb =
        targetFreeBytes /
        (1024ULL * 1024ULL);


    Serial.printf(
        "Storage rollover batch start: target=%llu MB, batch=%u files\n",
        (unsigned long long)targetFreeMb,
        (unsigned)ROLLOVER_BATCH_CANDIDATES
    );


    uint64_t estimatedFreeBytes =
        freeBytes;

    int totalDeleted =
        0;


    // ---------------------------------------------------------
    // Dated recording folders, oldest day first.
    // ---------------------------------------------------------

    while (
        estimatedFreeBytes <
            targetFreeBytes &&
        totalDeleted <
            ROLLOVER_MAX_DELETE_FILES
    ) {

        String folderName;


        if (!findOldestDateFolder(
                folderName
            )) {
            break;
        }


        String folderPath =
            "/" + folderName;

        bool removedFromFolder =
            false;


        // Stay in the same oldest folder and delete bounded batches.
        // This removes the old root/day rescan for every single file.
        while (
            estimatedFreeBytes <
                targetFreeBytes &&
            totalDeleted <
                ROLLOVER_MAX_DELETE_FILES
        ) {

            int remainingDeleteBudget =
                ROLLOVER_MAX_DELETE_FILES -
                totalDeleted;

            int deleted =
                deleteOldestBatchFromFolder(
                    folderPath,
                    targetFreeBytes,
                    estimatedFreeBytes,
                    remainingDeleteBudget
                );


            if (deleted < 0) {

                Serial.println(
                    "Storage rollover: delete failed"
                );

                logWrite(
                    "Storage rollover failed: delete error"
                );

                return false;
            }


            if (deleted == 0)
                break;


            removedFromFolder =
                true;

            totalDeleted +=
                deleted;


            // Expensive totalBytes()/usedBytes() query only once per
            // deletion batch instead of after every deleted file.
            serviceStorageLongOperation();

            freeBytes =
                storageFreeBytes();

            estimatedFreeBytes =
                freeBytes;


            Serial.printf(
                "Storage rollover batch: deleted=%d total=%d free=%llu MB\n",
                deleted,
                totalDeleted,
                (unsigned long long)(
                    freeBytes /
                    (1024ULL * 1024ULL)
                )
            );


            if (
                freeBytes >=
                targetFreeBytes
            ) {
                break;
            }


            delay(1);
        }


        if (directoryIsEmpty(
                folderPath
            )) {

            STORAGE.rmdir(
                folderPath.c_str()
            );
        }


        if (
            freeBytes >=
            targetFreeBytes
        ) {
            break;
        }


        if (!removedFromFolder) {

            // Preserve the original safety rule: unknown/non-recording
            // content is never deleted automatically.
            break;
        }
    }


    // ---------------------------------------------------------
    // Fallback folder, if dated folders were insufficient.
    // ---------------------------------------------------------

    while (
        freeBytes <
            targetFreeBytes &&
        totalDeleted <
            ROLLOVER_MAX_DELETE_FILES
    ) {

        int remainingDeleteBudget =
            ROLLOVER_MAX_DELETE_FILES -
            totalDeleted;

        int deleted =
            deleteOldestBatchFromFolder(
                "/fallback",
                targetFreeBytes,
                estimatedFreeBytes,
                remainingDeleteBudget
            );


        if (deleted < 0) {

            Serial.println(
                "Storage rollover: fallback delete failed"
            );

            logWrite(
                "Storage rollover failed: fallback delete error"
            );

            return false;
        }


        if (deleted == 0)
            break;


        totalDeleted +=
            deleted;


        freeBytes =
            storageFreeBytes();

        estimatedFreeBytes =
            freeBytes;


        Serial.printf(
            "Storage rollover fallback batch: deleted=%d total=%d free=%llu MB\n",
            deleted,
            totalDeleted,
            (unsigned long long)(
                freeBytes /
                (1024ULL * 1024ULL)
            )
        );


        if (
            freeBytes >=
            targetFreeBytes
        ) {
            break;
        }


        delay(1);
    }


    if (directoryIsEmpty(
            "/fallback"
        )) {

        STORAGE.rmdir(
            "/fallback"
        );
    }


    // Final authoritative check. Reaching the larger hysteresis target
    // is preferred; the configured reserve itself remains the hard
    // requirement for allowing the next recording.
    serviceStorageLongOperation();

    freeBytes =
        storageFreeBytes();

    bool reserveAvailable =
        freeBytes >=
        reserveBytes;


    if (reserveAvailable) {

        Serial.printf(
            "Storage rollover finished: deleted=%d free=%llu MB target=%llu MB\n",
            totalDeleted,
            (unsigned long long)(
                freeBytes /
                (1024ULL * 1024ULL)
            ),
            (unsigned long long)targetFreeMb
        );

        logWrite(
            "Storage rollover finished: deleted=" +
            String(totalDeleted) +
            " free=" +
            String(
                (unsigned long)(
                    freeBytes /
                    (1024ULL * 1024ULL)
                )
            ) +
            " MB"
        );

        return true;
    }


    Serial.println(
        "Storage rollover: no deletable recording found / reserve still unavailable"
    );

    logWrite(
        "Storage rollover failed: reserve still unavailable"
    );

    return false;
}



static int recoverDirectory(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!root || !root.isDirectory()) {
        if (root)
            root.close();
        return 0;
    }

    int removed = 0;

    File file =
        root.openNextFile();

    while (file) {

        String name =
            String(file.name());

        String fullPath =
            makeFullPath(
                path,
                name
            );

        bool isDir =
            file.isDirectory();

        file.close();

        if (isDir) {

            removed +=
                recoverDirectory(
                    fullPath
                );

        } else if (
            isTemporaryRecordingName(
                fullPath
            )
        ) {

            if (STORAGE.remove(
                    fullPath.c_str()
                )) {

                removed++;

                Serial.println(
                    "Recovery removed incomplete file: " +
                    fullPath
                );

                logWrite(
                    "Recovery removed incomplete file: " +
                    fullPath
                );
            }
        }

        file =
            root.openNextFile();
    }

    root.close();

    return removed;
}


int storageRecoverIncompleteRecordings()
{
    if (g_storageLocked)
        return 0;

    return recoverDirectory("/");
}
