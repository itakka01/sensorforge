#include "sd_presence_policy.h"
#include <Preferences.h>

static constexpr const char *kNamespace = "sfsdpolicy";
static constexpr const char *kKey = "optional";

bool sdPresenceExpectNoCard()
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false; // fail safe: normal SD recovery
    bool value = prefs.getBool(kKey, false);
    prefs.end();
    return value;
}

bool sdPresenceSetExpectNoCard(bool enabled)
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return false;
    const size_t written = prefs.putBool(kKey, enabled);
    prefs.end();
    return written == 1;
}
