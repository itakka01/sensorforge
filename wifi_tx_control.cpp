#include "wifi_tx_control.h"

#include "board_config.h"
#include "config.h"
#include "logger.h"

#include <esp_wifi.h>
#include <math.h>

namespace WifiTxControl {
namespace {
int16_t g_sessionOverrideX10 = 0;
}

int16_t normalizeX10(int16_t requestedX10)
{
    int16_t value = boardWifiTxPowerNormalizeUpX10(requestedX10);
    if (boardWifiTxPowerCappedX10(value))
        value = BOARD_WIFI_TX_POWER_MAX_X10;
    return value;
}

int16_t sessionOverrideX10()
{
    return g_sessionOverrideX10;
}

void setSessionOverrideX10(int16_t valueX10)
{
    g_sessionOverrideX10 = valueX10 > 0 ? normalizeX10(valueX10) : 0;
}

int16_t effectiveX10()
{
    if (g_sessionOverrideX10 > 0)
        return g_sessionOverrideX10;
    return (int16_t)lroundf(cfg_wifi_tx_power_dbm * 10.0f);
}

int16_t driverX10()
{
    int8_t quarterDbm = 0;
    if (esp_wifi_get_max_tx_power(&quarterDbm) != ESP_OK)
        return -1;
    return (int16_t)lroundf(quarterDbm * 2.5f);
}

bool applyX10(int16_t valueX10, const char *context, int16_t &actualX10)
{
    actualX10 = -1;
    const esp_err_t result = esp_wifi_set_max_tx_power(boardWifiTxPowerQuarterDbm(valueX10));
    if (result != ESP_OK) {
        consoleWrite("WIFI", String("TX power apply failed | context=") + (context ? context : "?") +
                     " | requested=" + String(valueX10 / 10.0f, 1) + " dBm | err=" + String((int)result));
        return false;
    }
    actualX10 = driverX10();
    consoleWrite("WIFI", String("TX power | context=") + (context ? context : "?") +
                 " | target=" + String(valueX10 / 10.0f, 1) + " dBm | driver=" +
                 (actualX10 >= 0 ? String(actualX10 / 10.0f, 1) + " dBm" : String("unavailable")));
    return true;
}

}  // namespace WifiTxControl
