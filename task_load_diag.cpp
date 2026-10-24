#include "task_load_diag.h"
#include "psram_alloc.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace TaskLoadDiag {
namespace {

constexpr size_t kMaxTasks = 48;
constexpr size_t kTopTasks = 6;
constexpr size_t kApiTopTasks = 10;
constexpr uint32_t kFastIntervalMs = 20000U;
constexpr uint32_t kSlowIntervalMs = 600000U;
constexpr uint32_t kFastPhaseMs = 120000U;
constexpr uint32_t kApiIntervalMs = 10000U;

struct Previous {
    TaskHandle_t handle;
    uint32_t runtime;
};

struct TopEntry {
    char name[configMAX_TASK_NAME_LEN];
    float pct;
    int8_t core;  // -1 = no affinity
    uint8_t priority;
};

// One run-time counter baseline. The serial report and the network snapshot
// use separate baselines so each covers exactly its own interval.
struct Sampler {
    Previous *previous = nullptr;
    size_t previousCount = 0;
    uint32_t previousTotal = 0;
    uint32_t lastMs = 0;
    bool started = false;
};

// Lazily allocated in PSRAM (see psram_alloc.h); ~3.5 KB in total.
TaskStatus_t *statusBuffer = nullptr;
Sampler serialSampler;
Sampler apiSampler;
bool allocFailed = false;

TopEntry apiTop[kApiTopTasks];
size_t apiTopCount = 0;
uint32_t apiWindowMs = 0;
bool apiValid = false;

bool ensureBuffers() {
    if (allocFailed)
        return false;
    if (statusBuffer)
        return true;
    statusBuffer = static_cast<TaskStatus_t *>(sfPsramCalloc(kMaxTasks, sizeof(TaskStatus_t)));
    serialSampler.previous = static_cast<Previous *>(sfPsramCalloc(kMaxTasks, sizeof(Previous)));
    apiSampler.previous = static_cast<Previous *>(sfPsramCalloc(kMaxTasks, sizeof(Previous)));
    if (!statusBuffer || !serialSampler.previous || !apiSampler.previous) {
        allocFailed = true;
        Serial.println("[TASK-DIAG] no PSRAM buffer, task load report disabled");
        return false;
    }
    return true;
}

uint32_t previousRuntime(const Sampler &s, TaskHandle_t handle, bool &found) {
    for (size_t i = 0; i < s.previousCount; ++i) {
        if (s.previous[i].handle == handle) {
            found = true;
            return s.previous[i].runtime;
        }
    }
    found = false;
    return 0;
}

// Picks the busiest tasks since the sampler's previous baseline into
// order/deltas (sorted, at most maxTop) and returns how many were picked.
size_t pickTop(const Sampler &s, UBaseType_t count, size_t *order, uint32_t *deltas,
               size_t maxTop) {
    size_t picked = 0;
    for (UBaseType_t i = 0; i < count; ++i) {
        bool found = false;
        const uint32_t before = previousRuntime(s, statusBuffer[i].xHandle, found);
        const uint32_t delta = found ? statusBuffer[i].ulRunTimeCounter - before : 0;
        size_t pos = picked;
        while (pos > 0 && deltas[pos - 1] < delta)
            --pos;
        if (pos >= maxTop)
            continue;
        const size_t last = picked < maxTop ? picked : maxTop - 1;
        for (size_t k = last; k > pos; --k) {
            order[k] = order[k - 1];
            deltas[k] = deltas[k - 1];
        }
        order[pos] = i;
        deltas[pos] = delta;
        if (picked < maxTop)
            ++picked;
    }
    return picked;
}

void storeBaseline(Sampler &s, UBaseType_t count, uint32_t total, uint32_t now) {
    s.previousCount = count < kMaxTasks ? count : kMaxTasks;
    for (size_t i = 0; i < s.previousCount; ++i) {
        s.previous[i].handle = statusBuffer[i].xHandle;
        s.previous[i].runtime = statusBuffer[i].ulRunTimeCounter;
    }
    s.previousTotal = total;
    s.lastMs = now;
    s.started = true;
}

void serviceSerial(uint32_t now) {
    const uint32_t interval = now < kFastPhaseMs ? kFastIntervalMs : kSlowIntervalMs;
    if (serialSampler.started && static_cast<uint32_t>(now - serialSampler.lastMs) < interval)
        return;

    uint32_t total = 0;
    const UBaseType_t count = uxTaskGetSystemState(statusBuffer, kMaxTasks, &total);
    if (count == 0) {
        serialSampler.lastMs = now;
        return;
    }

    if (serialSampler.started && total != serialSampler.previousTotal) {
        const uint32_t totalDelta = total - serialSampler.previousTotal;
        size_t order[kTopTasks];
        uint32_t deltas[kTopTasks];
        const size_t picked = pickTop(serialSampler, count, order, deltas, kTopTasks);
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
    storeBaseline(serialSampler, count, total, now);
}

void serviceApi(uint32_t now) {
    if (apiSampler.started && static_cast<uint32_t>(now - apiSampler.lastMs) < kApiIntervalMs)
        return;

    uint32_t total = 0;
    const UBaseType_t count = uxTaskGetSystemState(statusBuffer, kMaxTasks, &total);
    if (count == 0) {
        apiSampler.lastMs = now;
        return;
    }

    if (apiSampler.started && total != apiSampler.previousTotal) {
        const uint32_t totalDelta = total - apiSampler.previousTotal;
        size_t order[kApiTopTasks];
        uint32_t deltas[kApiTopTasks];
        const size_t picked = pickTop(apiSampler, count, order, deltas, kApiTopTasks);
        for (size_t k = 0; k < picked; ++k) {
            const TaskStatus_t &t = statusBuffer[order[k]];
            const BaseType_t core = xTaskGetCoreID(t.xHandle);
            strlcpy(apiTop[k].name, t.pcTaskName, sizeof(apiTop[k].name));
            apiTop[k].pct = 100.0f * (float)deltas[k] / (float)totalDelta;
            apiTop[k].core = core == tskNO_AFFINITY ? -1 : (int8_t)core;
            apiTop[k].priority = (uint8_t)t.uxCurrentPriority;
        }
        apiTopCount = picked;
        apiWindowMs = now - apiSampler.lastMs;
        apiValid = true;
    }
    storeBaseline(apiSampler, count, total, now);
}

}  // namespace

void service() {
    if (!ensureBuffers())
        return;
    const uint32_t now = millis();
    serviceSerial(now);
    serviceApi(now);
}

String json() {
    String out = "{\"valid\":" + String(apiValid ? "true" : "false");
    if (apiValid) {
        out += ",\"window_ms\":" + String((unsigned long)apiWindowMs);
        out += ",\"list\":[";
        for (size_t k = 0; k < apiTopCount; ++k) {
            if (k)
                out += ',';
            // Task names are FreeRTOS identifiers (no quotes or secrets).
            out += "{\"name\":\"" + String(apiTop[k].name) + "\"";
            out += ",\"pct\":" + String(apiTop[k].pct, 1);
            out += ",\"core\":" + String((int)apiTop[k].core);
            out += ",\"prio\":" + String((unsigned)apiTop[k].priority) + "}";
        }
        out += "]";
    }
    out += "}";
    return out;
}

}  // namespace TaskLoadDiag
