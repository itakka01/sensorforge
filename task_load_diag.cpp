#include "task_load_diag.h"
#include "psram_alloc.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace TaskLoadDiag {
namespace {

constexpr size_t kMaxTasks = 48;
constexpr size_t kTopTasks = 6;
constexpr uint32_t kFastIntervalMs = 20000U;
constexpr uint32_t kSlowIntervalMs = 600000U;
constexpr uint32_t kFastPhaseMs = 120000U;

struct Previous {
    TaskHandle_t handle;
    uint32_t runtime;
};

// Lazily allocated in PSRAM (see psram_alloc.h); ~3 KB in total.
TaskStatus_t *statusBuffer = nullptr;
Previous *previous = nullptr;
size_t previousCount = 0;
uint32_t previousTotal = 0;
uint32_t lastReportMs = 0;
bool started = false;
bool allocFailed = false;

uint32_t previousRuntime(TaskHandle_t handle, bool &found) {
    for (size_t i = 0; i < previousCount; ++i) {
        if (previous[i].handle == handle) {
            found = true;
            return previous[i].runtime;
        }
    }
    found = false;
    return 0;
}

}  // namespace

void service() {
    const uint32_t now = millis();
    const uint32_t interval = now < kFastPhaseMs ? kFastIntervalMs : kSlowIntervalMs;
    if (started && static_cast<uint32_t>(now - lastReportMs) < interval)
        return;
    if (allocFailed)
        return;
    if (!statusBuffer) {
        statusBuffer = static_cast<TaskStatus_t *>(sfPsramCalloc(kMaxTasks, sizeof(TaskStatus_t)));
        previous = static_cast<Previous *>(sfPsramCalloc(kMaxTasks, sizeof(Previous)));
        if (!statusBuffer || !previous) {
            allocFailed = true;
            Serial.println("[TASK-DIAG] no PSRAM buffer, task load report disabled");
            return;
        }
    }
    lastReportMs = now;

    uint32_t total = 0;
    const UBaseType_t count = uxTaskGetSystemState(statusBuffer, kMaxTasks, &total);
    if (count == 0)
        return;

    if (started && total != previousTotal) {
        const uint32_t totalDelta = total - previousTotal;
        // Pick the busiest tasks by run time since the previous report.
        size_t order[kTopTasks];
        uint32_t deltas[kTopTasks];
        size_t picked = 0;
        for (UBaseType_t i = 0; i < count; ++i) {
            bool found = false;
            const uint32_t before = previousRuntime(statusBuffer[i].xHandle, found);
            const uint32_t delta = found ? statusBuffer[i].ulRunTimeCounter - before : 0;
            size_t pos = picked;
            while (pos > 0 && deltas[pos - 1] < delta)
                --pos;
            if (pos >= kTopTasks)
                continue;
            const size_t last = picked < kTopTasks ? picked : kTopTasks - 1;
            for (size_t k = last; k > pos; --k) {
                order[k] = order[k - 1];
                deltas[k] = deltas[k - 1];
            }
            order[pos] = i;
            deltas[pos] = delta;
            if (picked < kTopTasks)
                ++picked;
        }
        String line = "[TASK-DIAG] last " + String((unsigned long)(interval / 1000)) +
                      " s, % of one core:";
        for (size_t k = 0; k < picked; ++k) {
            const TaskStatus_t &t = statusBuffer[order[k]];
            const BaseType_t core = xTaskGetCoreID(t.xHandle);
            const float pct = 100.0f * (float)deltas[k] / (float)totalDelta;
            line += " | " + String(t.pcTaskName) + " " + String(pct, 1) + "% c" +
                    (core == tskNO_AFFINITY ? String("*") : String((int)core)) +
                    " p" + String((unsigned)t.uxCurrentPriority);
        }
        Serial.println(line);
    }

    previousCount = count < kMaxTasks ? count : kMaxTasks;
    for (size_t i = 0; i < previousCount; ++i) {
        previous[i].handle = statusBuffer[i].xHandle;
        previous[i].runtime = statusBuffer[i].ulRunTimeCounter;
    }
    previousTotal = total;
    started = true;
}

}  // namespace TaskLoadDiag
