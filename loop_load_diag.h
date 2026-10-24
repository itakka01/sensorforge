#pragma once

#include <Arduino.h>
#include <esp_timer.h>

// Where loopTask spends its time, per loop section, readable over the network
// (GET /load_diag). Diagnostics only: it measures with esp_timer_get_time()
// and changes no behaviour. Values cover the last completed 10-second window.
namespace LoopLoadDiag {

enum Section : uint8_t {
    Prelude = 0,       // watchdog, license, LED, heap/WiFi/task diagnostics, crash guard
    ClusterWifiPolicy,
    ClusterEspNow,     // ClusterEspNowRadio::service
    ClusterHwTime,     // hardware time session + GPIO meter
    ClusterCapture,    // scheduled cluster capture
    ClusterUdpGroup,   // multicast receive + packet processing
    ClusterUdpDirect,  // coordinator socket receive + packet processing
    ClusterElection,   // expire peers/resources/jobs, recompute coordinator, probe retries
    ClusterHeartbeat,  // heartbeat + coordination send
    ClusterCameraReport,
    ClusterTimeRequest,  // coordinator heartbeat, cluster time request, capture result retry
    Thermal,
    WebConfig,         // webConfigLoop (HTTP server, player, SD maintenance)
    DroneMisc,         // firmware auto scheduler + WireGuard in Drone Mode
    // Parts of ClusterEspNow (also counted there, not again in measured_pct):
    EspNowRx,          // receive queue: decode/authenticate, answer probes, reports
    EspNowSlots,       // coordinator slot scheduler (serviceSlots)
    SectionCount
};

// First section that is only a part of another one.
constexpr Section kFirstSubSection = EspNowRx;

// Call once at the top of loop(): counts iterations and rolls the window.
void tick();

void add(Section section, uint32_t us);

// Adds the time since markUs to section and moves markUs to now, so
// consecutive sections can be timed with one running timestamp.
inline void lap(Section section, int64_t &markUs) {
    const int64_t now = esp_timer_get_time();
    add(section, (uint32_t)(now - markUs));
    markUs = now;
}

// Times one section from construction to end of scope.
class Scope {
public:
    explicit Scope(Section section) : section_(section), startUs_(esp_timer_get_time()) {}
    ~Scope() { add(section_, (uint32_t)(esp_timer_get_time() - startUs_)); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;

private:
    Section section_;
    int64_t startUs_;
};

// JSON for GET /load_diag: loop sections plus per-task CPU load.
String json();

}  // namespace LoopLoadDiag
