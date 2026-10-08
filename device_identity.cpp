#include "device_identity.h"

#include <Preferences.h>
#include <esp_system.h>

const String &deviceIntegrationId()
{
    static String id;
    if (id.length())
        return id;

    uint8_t bytes[16] = {};
    bool persistent = false;

    Preferences prefs;
    if (prefs.begin("sfapi", false)) {
        size_t read = prefs.getBytes("iid", bytes, sizeof(bytes));
        if (read != sizeof(bytes)) {
            esp_fill_random(bytes, sizeof(bytes));
            persistent =
                prefs.putBytes("iid", bytes, sizeof(bytes)) == sizeof(bytes);
        } else {
            persistent = true;
        }
        prefs.end();
    }

    // NVS should normally be available. If it is not, still return a valid
    // per-boot identifier rather than leaking a hardware-derived identifier.
    if (!persistent)
        esp_fill_random(bytes, sizeof(bytes));

    static const char HEX_DIGITS[] = "0123456789abcdef";
    char text[3 + 32 + 1];
    text[0] = 's';
    text[1] = 'f';
    text[2] = '-';
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        text[3 + i * 2] = HEX_DIGITS[(bytes[i] >> 4) & 0x0F];
        text[4 + i * 2] = HEX_DIGITS[bytes[i] & 0x0F];
    }
    text[35] = '\0';

    id = text;
    return id;
}
