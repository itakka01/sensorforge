#include "cluster_hw_time_gpio_lease.h"
#include "board_config.h"
#include "drone_mode.h"
#if defined(BOARD_FREENOVE)
#include "motion_diagnostics.h"
#include "radar.h"
#include "driver/rtc_io.h"
#endif

namespace ClusterHwTimeGpioLease {
namespace {
bool held = false;
}

bool acquire(String &reason) {
    reason = "";
#if !defined(BOARD_FREENOVE)
    reason = "Drei Messeingaenge sind nur fuer Freenove vorgesehen.";
    return false;
#else
    if (held || ClusterHwTimeCapture::active()) {
        reason = "Messeingaenge sind bereits reserviert.";
        return false;
    }
    if (!droneModeEnabled()) {
        reason = "Nur im Drone Mode moeglich.";
        return false;
    }
    // Deliberately permit ONLY the documented candidate mapping. Pin 41
    // is the live UART RX; any other board layout needs separate qualification.
    if (CLUSTER_HW_TIME_IN_1 != CLUSTER_HW_TIME_CANDIDATE_IN_1 ||
        CLUSTER_HW_TIME_IN_2 != CLUSTER_HW_TIME_CANDIDATE_IN_2 ||
        CLUSTER_HW_TIME_IN_3 != CLUSTER_HW_TIME_CANDIDATE_IN_3) {
        reason = "Messeingaenge 14/21/41 sind nicht explizit freigegeben.";
        return false;
    }
    // RTC-backed presence sampling has its own pin mux. Never steal that pin.
    if (motionDiagnosticsPresenceRtcSamplingActive()) {
        reason = "RTC-Praesenzabtastung belegt GPIO21.";
        return false;
    }
    if (!radarReleaseUartForTimeMeasurement()) {
        reason = "Radar-UART kann nicht sicher freigegeben werden.";
        return false;
    }
    motionDiagnosticsSuspendPresenceInterrupt();
    const int inputs[3] = {CLUSTER_HW_TIME_IN_1,
                           CLUSTER_HW_TIME_IN_2,
                           CLUSTER_HW_TIME_IN_3};
    if (!ClusterHwTimeCapture::begin(inputs, droneModeEnabled(), true)) {
        motionDiagnosticsResumePresenceInterrupt();
        radarRestoreUartAfterTimeMeasurement();
        reason = "GPIO-Flankenerfassung konnte nicht gestartet werden.";
        return false;
    }
    held = true;
    return true;
#endif
}

void release() {
#if defined(BOARD_FREENOVE)
    if (!held) return;
    // Detach interrupts before restoring each peripheral's pin mux.
    ClusterHwTimeCapture::end();
    pinMode(MAGNET_SWITCH_PIN, INPUT_PULLUP);
    motionDiagnosticsResumePresenceInterrupt();
    radarRestoreUartAfterTimeMeasurement();
    held = false;
#endif
}

bool active() { return held; }

void takeAndRearm(ClusterHwTimeCapture::Edges &out) {
    for (int i = 0; i < 3; ++i) {
        out.firstRiseUs[i] = 0;
        out.received[i] = false;
        out.rises[i] = 0;
    }
    if (held) ClusterHwTimeCapture::takeAndRearm(out);
}

void snapshot(ClusterHwTimeCapture::Edges &out) {
    for (int i = 0; i < 3; ++i) {
        out.firstRiseUs[i] = 0;
        out.received[i] = false;
        out.rises[i] = 0;
    }
    if (held) ClusterHwTimeCapture::snapshot(out);
}
}
