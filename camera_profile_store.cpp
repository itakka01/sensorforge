#include "camera_profile_store.h"

#include <Preferences.h>

namespace {

const char *const kNamespace = "sf_camprof";
const char *const kKey = "p";

bool loaded = false;
CameraProfile::Profile active;
bool activePersistent = false;

void load()
{
    if (loaded)
        return;
    loaded = true;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true))
        return;
    const String text = prefs.getString(kKey, "");
    prefs.end();
    CameraProfile::Profile p;
    if (text.length() && CameraProfile::parse(text.c_str(), p)) {
        active = p;
        activePersistent = true;
    } else if (text.length()) {
        Serial.println("[CAMERA] stored cluster camera profile invalid, ignored");
    }
}

} // namespace

namespace CameraProfileStore {

bool get(CameraProfile::Profile &out, bool &persistent)
{
    load();
    out = active;
    persistent = activePersistent;
    return active.version != 0;
}

void setSession(const CameraProfile::Profile &profile)
{
    load();
    active = profile;
    activePersistent = false;
}

bool setPersistent(const CameraProfile::Profile &profile, String &error)
{
    load();
    char text[160];
    CameraProfile::format(profile, text, sizeof(text));
    if (!text[0]) {
        error = "profile invalid";
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "NVS not available";
        return false;
    }
    const size_t written = prefs.putString(kKey, text);
    prefs.end();
    if (written != strlen(text)) {
        error = "NVS write failed";
        return false;
    }
    active = profile;
    activePersistent = true;
    return true;
}

bool clear(String &error)
{
    load();
    active = CameraProfile::Profile{};
    activePersistent = false;
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "NVS not available";
        return false;
    }
    const bool hadKey = prefs.isKey(kKey);
    const bool ok = !hadKey || prefs.remove(kKey);
    prefs.end();
    if (!ok)
        error = "NVS remove failed";
    return ok;
}

} // namespace CameraProfileStore
