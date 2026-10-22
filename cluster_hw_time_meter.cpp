#include "cluster_hw_time_meter.h"
#include "board_config.h"
#include "cluster_hw_time_event.h"
#include "cluster_hw_time_gpio_lease.h"
#include "cluster_hw_time_math.h"
#include "drone_mode.h"
#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace ClusterHwTimeMeter {
namespace {
// Edges of one shot must arrive within this window after the earliest edge.
// Shots are seconds apart; the pulse itself lasts until fb_get() returns.
constexpr int64_t kEventWindowUs = 500000LL;
// Measuring suspends magnet/presence/radar inputs: never leave it on forever.
constexpr uint32_t kMaxRunMs = 2UL * 60UL * 60UL * 1000UL;
constexpr size_t kCapacity = 256;
// Each snapshot briefly masks interrupts on this core (spinlock shared with the
// edge ISR). Poll rarely: an ISR is delayed by it only with tiny probability.
constexpr uint32_t kPollIntervalMs = 20;

struct Row {
    uint32_t sequence;
    ClusterHwTimeEvent::Event event;
};
// ~16 KB, allocated in PSRAM on the first start only (never on unused boards).
Row *rows = nullptr;
size_t rowCount = 0, rowNext = 0;
uint32_t eventCounter = 0;
bool running = false;
uint32_t startedMs = 0;
uint32_t lastPollMs = 0;
String state = "not_started";

void addRow(const ClusterHwTimeEvent::Event &e) {
    Row &r = rows[rowNext];
    r.sequence = ++eventCounter;
    r.event = e;
    rowNext = (rowNext + 1) % kCapacity;
    if (rowCount < kCapacity) ++rowCount;
}
const Row &rowAt(size_t index) {  // 0 = oldest retained
    return rows[(rowNext + kCapacity - rowCount + index) % kCapacity];
}
// qualifiedOnly=false: raw channel offsets for display/CSV.
// qualifiedOnly=true: pair differences that count as a precision result.
ClusterHwTimeMath::Result offsets(const ClusterHwTimeEvent::Event &e, bool qualifiedOnly) {
    ClusterHwTimeMath::Sample s = {};
    s.referenceUs = e.firstEdgeUs;
    for (int i = 0; i < 3; ++i) {
        s.nodeUs[i] = e.edgeUs[i];
        s.present[i] = qualifiedOnly ? ClusterHwTimeEvent::qualified(e, i) : e.present[i];
    }
    return ClusterHwTimeMath::compare(s);
}
bool anyPairValid(const ClusterHwTimeMath::Result &q) {
    return q.pairValid[0] || q.pairValid[1] || q.pairValid[2];
}
ClusterHwTimeEvent::Observed observed(const ClusterHwTimeCapture::Edges &edges) {
    ClusterHwTimeEvent::Observed o = {};
    for (int i = 0; i < 3; ++i) {
        o.firstRiseUs[i] = edges.firstRiseUs[i];
        o.received[i] = edges.received[i];
        o.rises[i] = edges.rises[i];
    }
    return o;
}
String msText(bool valid, double ms) { return valid ? String(ms, 3) : String(); }
}  // namespace

bool start(String &error) {
    if (running) { error = "Messung laeuft bereits."; return false; }
    if (!rows) {
        rows = static_cast<Row *>(heap_caps_calloc(kCapacity, sizeof(Row), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rows) { error = "Kein PSRAM fuer die Messreihe."; return false; }
    }
    // Same exclusive pin handoff as the coordinator session, but no role check.
    if (!ClusterHwTimeGpioLease::acquire(error)) return false;
    running = true;
    startedMs = millis();
    rowCount = rowNext = 0;
    eventCounter = 0;
    state = "armed";
    return true;
}

void stop(const char *reason) {
    if (!running) return;
    ClusterHwTimeGpioLease::release();
    running = false;
    state = reason ? reason : "stopped";
}

bool active() { return running; }

void service() {
    if (!running) return;
    if (!droneModeEnabled()) { stop("stopped_drone_mode_off"); return; }
    const uint32_t nowMs = millis();
    if ((uint32_t)(nowMs - startedMs) >= kMaxRunMs) { stop("stopped_timeout_2h"); return; }
    if ((uint32_t)(nowMs - lastPollMs) < kPollIntervalMs) return;
    lastPollMs = nowMs;
    ClusterHwTimeCapture::Edges edges = {};
    ClusterHwTimeGpioLease::snapshot(edges);
    if (!ClusterHwTimeEvent::ready(observed(edges), esp_timer_get_time(), kEventWindowUs)) return;
    // Final snapshot and re-arm in one critical section: no edge is lost.
    ClusterHwTimeGpioLease::takeAndRearm(edges);
    ClusterHwTimeEvent::Event e = {};
    if (ClusterHwTimeEvent::close(observed(edges), kEventWindowUs, e)) addRow(e);
}

// Offsets are relative to the earliest edge of each event, differences are
// channel minus channel. One local monotonic clock; not a UTC timestamp.
String csv() {
    // Pair columns are filled ONLY for qualified channels (exactly one edge,
    // no late edge); raw channel offsets stay visible for diagnosis.
    String out = "event,first_edge_mono_us,ch1_ms,ch2_ms,ch3_ms,ch1_minus_ch2_ms,ch1_minus_ch3_ms,ch2_minus_ch3_ms,ch1_rises,ch2_rises,ch3_rises,late_edges,valid\n";
    for (size_t i = 0; i < rowCount; ++i) {
        const Row &r = rowAt(i);
        const ClusterHwTimeMath::Result o = offsets(r.event, false);
        const ClusterHwTimeMath::Result q = offsets(r.event, true);
        out += String(r.sequence) + "," + String((long long)r.event.firstEdgeUs);
        for (int ch = 0; ch < 3; ++ch) out += "," + msText(o.valid[ch], o.offsetMs[ch]);
        for (int p = 0; p < 3; ++p) out += "," + msText(q.pairValid[p], q.pairOffsetMs[p]);
        for (int ch = 0; ch < 3; ++ch) out += "," + String((unsigned long)r.event.rises[ch]);
        out += "," + String((unsigned)r.event.lateEdges) + "," + String(anyPairValid(q) ? 1 : 0) + "\n";
    }
    return out;
}

String statusJson() {
    String json = "{\"active\":";
    json += running ? "true" : "false";
    json += ",\"state\":\"" + state + "\"";
    json += ",\"drone_mode\":";
    json += droneModeEnabled() ? "true" : "false";
    json += ",\"inputs\":[" + String(CLUSTER_HW_TIME_IN_1) + "," + String(CLUSTER_HW_TIME_IN_2) +
            "," + String(CLUSTER_HW_TIME_IN_3) + "]";
    json += ",\"window_ms\":" + String((long)(kEventWindowUs / 1000));
    json += ",\"running_s\":" + String(running ? (unsigned long)((millis() - startedMs) / 1000UL) : 0UL);
    json += ",\"events\":" + String((unsigned long)eventCounter);
    json += ",\"rows\":" + String((unsigned)rowCount);
    // Statistics over qualified pairs of all retained rows only.
    uint32_t validRows = 0;
    uint32_t n[3] = {};
    double minMs[3] = {}, maxMs[3] = {}, sumMs[3] = {};
    for (size_t i = 0; i < rowCount; ++i) {
        const ClusterHwTimeMath::Result q = offsets(rowAt(i).event, true);
        if (anyPairValid(q)) ++validRows;
        for (int p = 0; p < 3; ++p) {
            if (!q.pairValid[p]) continue;
            const double v = q.pairOffsetMs[p];
            if (!n[p] || v < minMs[p]) minMs[p] = v;
            if (!n[p] || v > maxMs[p]) maxMs[p] = v;
            sumMs[p] += v;
            ++n[p];
        }
    }
    json += ",\"valid_rows\":" + String((unsigned long)validRows);
    json += ",\"invalid_rows\":" + String((unsigned long)(rowCount - validRows));
    json += ",\"pairs\":[";
    for (int p = 0; p < 3; ++p) {
        if (p) json += ',';
        json += "{\"n\":" + String((unsigned long)n[p]);
        if (n[p]) json += ",\"min_ms\":" + String(minMs[p], 3) + ",\"max_ms\":" + String(maxMs[p], 3) +
                          ",\"mean_ms\":" + String(sumMs[p] / n[p], 3);
        json += "}";
    }
    json += "]";
    json += ",\"samples\":[";
    // Small bounded history for the browser; full history available as CSV.
    const size_t first = rowCount > 48 ? rowCount - 48 : 0;
    for (size_t i = first; i < rowCount; ++i) {
        const Row &r = rowAt(i);
        const ClusterHwTimeMath::Result o = offsets(r.event, false);
        const ClusterHwTimeMath::Result q = offsets(r.event, true);
        if (i != first) json += ',';
        json += "{\"n\":" + String(r.sequence) + ",\"ms\":[";
        for (int ch = 0; ch < 3; ++ch) {
            if (ch) json += ',';
            json += o.valid[ch] ? String(o.offsetMs[ch], 3) : String("null");
        }
        json += "],\"pair_ms\":[";
        for (int p = 0; p < 3; ++p) {
            if (p) json += ',';
            json += q.pairValid[p] ? String(q.pairOffsetMs[p], 3) : String("null");
        }
        json += "],\"rises\":[" + String((unsigned long)r.event.rises[0]) + "," +
                String((unsigned long)r.event.rises[1]) + "," + String((unsigned long)r.event.rises[2]) +
                "],\"late\":" + String((unsigned)r.event.lateEdges) +
                ",\"valid\":" + String(anyPairValid(q) ? "true" : "false") + "}";
    }
    json += "]}";
    return json;
}
}  // namespace ClusterHwTimeMeter
