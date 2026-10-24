#include "loop_load_diag.h"
#include "task_load_diag.h"

namespace LoopLoadDiag {
namespace {

constexpr uint32_t kWindowMs = 10000U;

const char *const kSectionNames[SectionCount] = {
    "prelude",
    "cluster_wifi_policy",
    "cluster_espnow",
    "cluster_hw_time",
    "cluster_capture",
    "cluster_udp_group",
    "cluster_udp_direct",
    "cluster_election",
    "cluster_heartbeat",
    "cluster_camera_report",
    "cluster_time_request",
    "thermal",
    "webconfig",
    "drone_misc",
    "espnow_rx",
    "espnow_slots",
};

struct Acc {
    uint64_t totalUs;
    uint32_t calls;
    uint32_t maxUs;
};

struct Window {
    Acc sections[SectionCount];
    uint32_t iterations;
    uint32_t durationMs;
};

Window current = {};
Window last = {};
bool haveLast = false;
uint32_t windowStartMs = 0;
bool started = false;

}  // namespace

void tick() {
    const uint32_t now = millis();
    if (!started) {
        started = true;
        windowStartMs = now;
    } else if ((uint32_t)(now - windowStartMs) >= kWindowMs) {
        current.durationMs = now - windowStartMs;
        last = current;
        haveLast = true;
        current = {};
        windowStartMs = now;
    }
    ++current.iterations;
}

void add(Section section, uint32_t us) {
    if (section >= SectionCount)
        return;
    Acc &a = current.sections[section];
    a.totalUs += us;
    ++a.calls;
    if (us > a.maxUs)
        a.maxUs = us;
}

String json() {
    String out;
    out.reserve(1800);
    out += "{\"ok\":true,\"uptime_ms\":" + String((unsigned long)millis());
    out += ",\"loop\":{\"valid\":" + String(haveLast ? "true" : "false");
    if (haveLast) {
        const uint32_t windowMs = last.durationMs ? last.durationMs : 1;
        uint64_t sumUs = 0;
        for (size_t i = 0; i < kFirstSubSection; ++i)
            sumUs += last.sections[i].totalUs;
        out += ",\"window_ms\":" + String((unsigned long)windowMs);
        out += ",\"iterations\":" + String((unsigned long)last.iterations);
        out += ",\"iterations_per_s\":" + String(1000.0f * last.iterations / windowMs, 1);
        out += ",\"measured_pct\":" + String(sumUs / (10.0f * windowMs), 2);
        out += ",\"sections\":[";
        for (size_t i = 0; i < SectionCount; ++i) {
            const Acc &a = last.sections[i];
            if (i)
                out += ',';
            out += "{\"name\":\"" + String(kSectionNames[i]) + "\"";
            // Percent of one core over the window.
            out += ",\"pct\":" + String(a.totalUs / (10.0f * windowMs), 2);
            out += ",\"calls\":" + String((unsigned long)a.calls);
            out += ",\"mean_us\":" + String(a.calls ? (unsigned long)(a.totalUs / a.calls) : 0UL);
            out += ",\"max_us\":" + String((unsigned long)a.maxUs);
            if (i >= kFirstSubSection)
                out += ",\"part_of\":\"cluster_espnow\"";
            out += "}";
        }
        out += "]";
    }
    out += "},\"tasks\":" + TaskLoadDiag::json();
    out += "}";
    return out;
}

}  // namespace LoopLoadDiag
