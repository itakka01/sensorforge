#include "cluster_hw_time_pulse.h"
#include "board_config.h"
#include "driver/gpio.h"

#if defined(BOARD_XIAO) && CLUSTER_HW_TIME_OUT == 2
#include "config.h"
#include "soc/gpio_reg.h"
namespace {
constexpr gpio_num_t kOutput = static_cast<gpio_num_t>(CLUSTER_HW_TIME_OUT);
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
