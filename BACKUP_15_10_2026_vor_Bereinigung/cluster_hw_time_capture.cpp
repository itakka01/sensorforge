#include "cluster_hw_time_capture.h"
#include <esp_timer.h>

namespace ClusterHwTimeCapture {
namespace {
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
struct Channel {
    int pin;
    volatile int64_t firstRiseUs;
    volatile bool received;
};
Channel channels[3] = {{-1, 0, false}, {-1, 0, false}, {-1, 0, false}};
bool armed = false;

void IRAM_ATTR onRise(void *arg) {
    Channel *c = static_cast<Channel *>(arg);
    // Time stamp as close as possible to the ISR entry. Not hardware capture.
    const int64_t nowUs = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&lock);
    if (!c->received) {
        c->firstRiseUs = nowUs;
        c->received = true;
    }
    portEXIT_CRITICAL_ISR(&lock);
}

bool validPin(int pin) {
    // ESP32-S3 GPIO 0..48; 19/20 are USB, 26..37 are flash/PSRAM,
    // 45/46 are strapping/input-only and unsuitable for a generic module.
    if (pin < 0 || pin > 48) return false;
    if (pin == 19 || pin == 20 || (pin >= 26 && pin <= 37) ||
        pin == 45 || pin == 46) return false;
    return true;
}
} // namespace

bool begin(const int pins[3], bool droneConfirmed, bool exclusivePinsReleased) {
    if (armed || !droneConfirmed || !exclusivePinsReleased || !pins) return false;
    for (int i = 0; i < 3; ++i) {
        if (!validPin(pins[i])) return false;
        for (int j = 0; j < i; ++j) if (pins[i] == pins[j]) return false;
    }
    for (int i = 0; i < 3; ++i) {
        channels[i].pin = pins[i];
        channels[i].firstRiseUs = 0;
        channels[i].received = false;
        pinMode(pins[i], INPUT);
    }
    // Caller guarantees that the input sources are not already driving high
    // and that all other pin owners remain suspended until end().
    armed = true;
    for (int i = 0; i < 3; ++i)
        attachInterruptArg(digitalPinToInterrupt(pins[i]), onRise, &channels[i], RISING);
    return true;
}

void snapshot(Edges &out) {
    portENTER_CRITICAL(&lock);
    for (int i = 0; i < 3; ++i) {
        out.firstRiseUs[i] = channels[i].firstRiseUs;
        out.received[i] = channels[i].received;
    }
    portEXIT_CRITICAL(&lock);
}

void end() {
    if (!armed) return;
    for (int i = 0; i < 3; ++i)
        detachInterrupt(digitalPinToInterrupt(channels[i].pin));
    armed = false;
    // Do not restore shared pins here: their original owner must explicitly
    // restore UART/sensor pin configuration after end() has returned.
}

bool active() { return armed; }
} // namespace ClusterHwTimeCapture
