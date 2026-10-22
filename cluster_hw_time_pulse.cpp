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
    // GPIO2 is owned by a prepared capture test pulse until its shot is done.
    if (!droneModeEnabled() || active || clusterCapturePulsePrepared() || !ensureTimers()) return false;
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

#if defined(BOARD_XIAO) && CLUSTER_HW_TIME_OUT == 2
#include "config.h"
#include "soc/gpio_reg.h"
namespace {
constexpr uint32_t kCaptureMask = 1UL << CLUSTER_HW_TIME_OUT;
volatile bool capturePrepared = false;
volatile bool captureEmitted = false;
bool audioUsesOutput() {
    const int pins[] = {cfg_audio_pdm_clk_pin, cfg_audio_pdm_data_pin, cfg_audio_i2s_bclk_pin,
                        cfg_audio_i2s_ws_pin, cfg_audio_i2s_data_pin, cfg_audio_i2s_mclk_pin};
    for (int pin : pins) if (pin == CLUSTER_HW_TIME_OUT) return true;
    return false;
}
}
bool clusterCapturePulsePrepare(const char *&reason) {
    captureEmitted = false;
    if (capturePrepared) { reason = "already_prepared"; return true; }
    if (active) { reason = "busy_hw_time_pulse"; return false; }  // SFHT1 owns GPIO2
    if (audioUsesOutput()) { reason = "pin_used_by_audio"; return false; }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << CLUSTER_HW_TIME_OUT);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) { reason = "gpio_config_failed"; return false; }
    REG_WRITE(GPIO_OUT_W1TC_REG, kCaptureMask);
    capturePrepared = true;
    reason = "prepared";
    return true;
}
void IRAM_ATTR clusterCapturePulseRise() {
    if (!capturePrepared) return;
    REG_WRITE(GPIO_OUT_W1TS_REG, kCaptureMask);
    captureEmitted = true;
}
void IRAM_ATTR clusterCapturePulseFall() {
    if (capturePrepared) REG_WRITE(GPIO_OUT_W1TC_REG, kCaptureMask);
}
void clusterCapturePulseRelease() {
    if (!capturePrepared) return;
    REG_WRITE(GPIO_OUT_W1TC_REG, kCaptureMask);
    // Input without pulls instead of gpio_reset_pin(): reset would enable the
    // internal pull-up and could raise the measurement line after the shot.
    gpio_set_direction(kOutput, GPIO_MODE_INPUT);
    gpio_set_pull_mode(kOutput, GPIO_FLOATING);
    capturePrepared = false;
}
bool clusterCapturePulsePrepared() { return capturePrepared; }
bool clusterCapturePulseEmitted() { return captureEmitted; }
#else
bool clusterCapturePulsePrepare(const char *&reason) { reason = "unsupported_board"; return false; }
void clusterCapturePulseRise() {}
void clusterCapturePulseFall() {}
void clusterCapturePulseRelease() {}
bool clusterCapturePulsePrepared() { return false; }
bool clusterCapturePulseEmitted() { return false; }
#endif
