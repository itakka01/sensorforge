#include "heap_monitor.h"
#include "logger.h"

#include <esp_heap_caps.h>


// Monitoring interval in milliseconds (60 seconds)
static const uint32_t HEAP_MONITOR_INTERVAL_MS = 60000;

// Fragmentation warning threshold: largest block < X% of total free
static const uint8_t FRAGMENTATION_WARNING_PERCENT = 25;

// Critical fragmentation threshold: largest block < X KB
static const uint32_t CRITICAL_LARGEST_BLOCK_KB = 32;

static uint32_t lastLogMs = 0;
static uint32_t minFreeEverInternal = UINT32_MAX;
static uint32_t minLargestBlockEverInternal = UINT32_MAX;


void heapMonitorInit()
{
    lastLogMs = millis();

    // Capture initial heap state
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    minFreeEverInternal = info.total_free_bytes;
    minLargestBlockEverInternal = info.largest_free_block;

    Serial.printf(
        "[HEAP] Monitor initialized | Internal: %u KB free, %u KB largest block\n",
        info.total_free_bytes / 1024,
        info.largest_free_block / 1024
    );
}


void heapMonitorService()
{
    uint32_t now = millis();

    // Check interval
    if (now - lastLogMs < HEAP_MONITOR_INTERVAL_MS)
        return;

    lastLogMs = now;
    heapMonitorLogNow();
}


void heapMonitorLogNow()
{
    // Internal heap (critical for system stability)
    multi_heap_info_t internalInfo;
    heap_caps_get_info(&internalInfo, MALLOC_CAP_INTERNAL);

    uint32_t freeKb = internalInfo.total_free_bytes / 1024;
    uint32_t largestBlockKb = internalInfo.largest_free_block / 1024;
    uint32_t allocatedKb = internalInfo.total_allocated_bytes / 1024;

    // Track minimums
    if (internalInfo.total_free_bytes < minFreeEverInternal) {
        minFreeEverInternal = internalInfo.total_free_bytes;
    }

    if (internalInfo.largest_free_block < minLargestBlockEverInternal) {
        minLargestBlockEverInternal = internalInfo.largest_free_block;
    }

    uint32_t minFreeEverKb = minFreeEverInternal / 1024;
    uint32_t minLargestEverKb = minLargestBlockEverInternal / 1024;

    // Calculate fragmentation percentage
    uint8_t fragPercent = heapMonitorGetFragmentationPercent();

    // Log to Serial
    Serial.printf(
        "[HEAP] Internal: %u KB free | Largest block: %u KB | Allocated: %u KB | "
        "Fragmentation: %u%% | Min free ever: %u KB | Min largest ever: %u KB\n",
        freeKb,
        largestBlockKb,
        allocatedKb,
        fragPercent,
        minFreeEverKb,
        minLargestEverKb
    );

    // Log to persistent logger
    String logMsg =
        "Heap: " + String(freeKb) + " KB free, " +
        String(largestBlockKb) + " KB largest, " +
        String(fragPercent) + "% frag";

    logWrite(logMsg);

    // PSRAM heap (if available)
    #if defined(BOARD_HAS_PSRAM)
    multi_heap_info_t psramInfo;
    heap_caps_get_info(&psramInfo, MALLOC_CAP_SPIRAM);

    if (psramInfo.total_free_bytes > 0) {
        uint32_t psramFreeKb = psramInfo.total_free_bytes / 1024;
        uint32_t psramLargestKb = psramInfo.largest_free_block / 1024;

        Serial.printf(
            "[HEAP] PSRAM: %u KB free | Largest block: %u KB\n",
            psramFreeKb,
            psramLargestKb
        );
    }
    #endif

    // Warnings
    if (heapMonitorIsCritical()) {
        Serial.println(
            "[HEAP] ⚠️  CRITICAL FRAGMENTATION DETECTED! Largest block < " +
            String(CRITICAL_LARGEST_BLOCK_KB) + " KB"
        );

        logWrite(
            "WARNING: Heap critically fragmented - largest block " +
            String(largestBlockKb) + " KB"
        );
    }
    else if (fragPercent >= FRAGMENTATION_WARNING_PERCENT * 2) {
        Serial.printf(
            "[HEAP] ⚠️  High fragmentation: %u%%\n",
            fragPercent
        );

        logWrite(
            "WARNING: High heap fragmentation " + String(fragPercent) + "%"
        );
    }

    // Memory pressure warning
    if (freeKb < 64) {
        Serial.printf(
            "[HEAP] ⚠️  Low free memory: %u KB\n",
            freeKb
        );

        logWrite("WARNING: Low free heap " + String(freeKb) + " KB");
    }
}


uint8_t heapMonitorGetFragmentationPercent()
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    if (info.total_free_bytes == 0)
        return 100;  // No free memory = 100% fragmented

    // Fragmentation = (1 - largest_block / total_free) * 100
    uint32_t percent =
        100 -
        ((info.largest_free_block * 100) / info.total_free_bytes);

    return (uint8_t)(percent > 100 ? 100 : percent);
}


bool heapMonitorIsCritical()
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);

    uint32_t largestBlockKb = info.largest_free_block / 1024;

    return largestBlockKb < CRITICAL_LARGEST_BLOCK_KB;
}
