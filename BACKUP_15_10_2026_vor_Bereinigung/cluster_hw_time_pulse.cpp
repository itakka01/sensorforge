#include "cluster_hw_time_pulse.h"
#include "board_config.h"
#include "cluster.h"
#include "drone_mode.h"
#include "driver/gpio.h"
#include "esp_timer.h"

// The caller must establish exclusive ownership of the configured pin before
// arming. In particular, Freenove GPIO48 is also wired to the RGB LED.
// Neither cluster command routing nor exclusive-pin handoff is implemented here.
namespace {
#if CLUSTER_HW_TIME_OUT >= 0
constexpr gpio_num_t kOutput = static_cast<gpio_num_t>(CLUSTER_HW_TIME_OUT);
constexpr int64_t kPulseWidthUs = 2000;
esp_timer_handle_t riseTimer = nullptr;
esp_timer_handle_t fallTimer = nullptr;
volatile bool active = false;
volatile int64_t lastEdgeUs = 0;

void fallCallback(void*) {
    gpio_set_level(kOutput, 0);
    active = false;
    gpio_reset_pin(kOutput);
}
void riseCallback(void*) {
    if (!active || !droneModeEnabled()) {
        gpio_set_level(kOutput, 0);
        active = false;
        return;
    }
    lastEdgeUs = esp_timer_get_time();
    gpio_set_level(kOutput, 1);
    // On timer-dispatch failure do not leave the output asserted.
    if (esp_timer_start_once(fallTimer, kPulseWidthUs) != ESP_OK) {
        gpio_set_level(kOutput, 0);
        active = false;
    }
}
bool ensureTimers() {
    if (riseTimer && fallTimer) return true;
    const esp_timer_create_args_t riseArgs = {
        .callback = riseCallback, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "sf_hw_rise"
    };
    const esp_timer_create_args_t fallArgs = {
        .callback = fallCallback, .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK, .name = "sf_hw_fall"
    };
    if (!riseTimer && esp_timer_create(&riseArgs, &riseTimer) != ESP_OK) return false;
    return fallTimer || esp_timer_create(&fallArgs, &fallTimer) == ESP_OK;
}
#endif
}

bool clusterHwTimePulseArm(int64_t targetUtcUs) {
#if CLUSTER_HW_TIME_OUT < 0
    (void)targetUtcUs;
    return false;
#else
    if (!droneModeEnabled() || active || !ensureTimers()) return false;
    int64_t nowUtc = 0;
    if (!clusterTimeNowUs(nowUtc)) return false;
    const int64_t delayUs = targetUtcUs - nowUtc;
    // Avoid a too-close deadline and bound timer lifetime.
    if (delayUs < 500000 || delayUs > 1800000000LL) return false;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << CLUSTER_HW_TIME_OUT);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) return false;
    gpio_set_level(kOutput, 0);
    lastEdgeUs = 0;
    active = true;
    if (esp_timer_start_once(riseTimer, static_cast<uint64_t>(delayUs)) != ESP_OK) {
        active = false;
        gpio_set_level(kOutput, 0);
        gpio_reset_pin(kOutput);
        return false;
    }
    return true;
#endif
}
void clusterHwTimePulseCancel() {
#if CLUSTER_HW_TIME_OUT >= 0
    if (!active && !lastEdgeUs) return;
    active = false;
    if (riseTimer) esp_timer_stop(riseTimer);
    if (fallTimer) esp_timer_stop(fallTimer);
    gpio_set_level(kOutput, 0);
    // GPIO ownership is returned to the caller, which must restore any
    // previous peripheral configuration (for example Freenove RGB LED).
    gpio_reset_pin(kOutput);
#endif
}
bool clusterHwTimePulseActive() {
#if CLUSTER_HW_TIME_OUT >= 0
    return active;
#else
    return false;
#endif
}
int64_t clusterHwTimePulseLastEdgeUs() {
#if CLUSTER_HW_TIME_OUT >= 0
    return lastEdgeUs;
#else
    return 0;
#endif
}
