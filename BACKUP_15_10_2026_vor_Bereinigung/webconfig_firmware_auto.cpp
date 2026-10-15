#include "webconfig_firmware_auto.h"
#include "config.h"
#include "firmware_manifest_probe.h"
#include <Preferences.h>
#include <stdlib.h>

const char *const WEB_FW_AUTO_DEFAULT_MANIFEST =
    "https://firmware.exapan.at/c1/manifest.json";

namespace {

bool validManifestUrl(const String &url)
{
    if (!url.startsWith("https://") || url.length() < 12 ||
        url.length() > 240) return false;
    for (size_t i = 0; i < url.length(); ++i) {
        const char c = url[i];
        if (c <= ' ' || c == 127) return false;
    }
    return true;
}

bool saveSettings(bool enabled, const String &url)
{
    Preferences prefs;
    if (!prefs.begin("c1-update", false)) return false;
    const bool savedUrl = prefs.putString("manifest", url) == url.length();
    // Preserve the existing storage representation from stage 1.
    const bool savedEnabled = savedUrl && prefs.putBool("enabled", enabled) == 1;
    prefs.end();
    return savedUrl && savedEnabled;
}

} // namespace

bool webFirmwareAutoRead(bool &enabled, String &manifestUrl)
{
    enabled = false;
    manifestUrl = WEB_FW_AUTO_DEFAULT_MANIFEST;
    Preferences prefs;
    if (!prefs.begin("c1-update", true)) return false;
    enabled = prefs.getBool("enabled", false);
    manifestUrl = prefs.getString("manifest", WEB_FW_AUTO_DEFAULT_MANIFEST);
    prefs.end();
    if (!validManifestUrl(manifestUrl)) {
        manifestUrl = WEB_FW_AUTO_DEFAULT_MANIFEST;
        enabled = false;
    }
    return true;
}

void webFirmwareAutoRegisterRoutes(WebServer &server, uint32_t bootSessionToken)
{
    server.on("/firmware_auto_settings", HTTP_POST, [&server, bootSessionToken]() {
        if (!server.hasArg("token")) {
            server.send(403, "text/plain; charset=utf-8", "Invalid action token");
            return;
        }
        const String token = server.arg("token");
        char *end = nullptr;
        const unsigned long value = strtoul(token.c_str(), &end, 10);
        if (!token.length() || !end || *end != '\0' ||
            static_cast<uint32_t>(value) != bootSessionToken) {
            server.send(403, "text/plain; charset=utf-8", "Invalid action token");
            return;
        }
        if (cfg_web_auth_enabled &&
            !server.authenticate(cfg_web_username.c_str(), cfg_web_password.c_str())) {
            server.requestAuthentication();
            return;
        }
        String source = server.arg("manifest");
        source.trim();
        if (!validManifestUrl(source)) {
            server.send(400, "text/plain; charset=utf-8",
                        "Manifest URL must be a valid HTTPS URL (max 240 chars)");
            return;
        }
        if (!saveSettings(server.arg("enabled") == "1", source)) {
            server.send(500, "text/plain; charset=utf-8", "Unable to save settings");
            return;
        }
        server.sendHeader("Location", "/system#firmware");
        server.send(303, "text/plain; charset=utf-8", "");
    });
    // Manually triggered, read-only manifest check; never installs firmware.
    server.on("/firmware_auto_check", HTTP_POST, [&server, bootSessionToken]() {
        if (!server.hasArg("token")) {
            server.send(403, "text/plain; charset=utf-8", "Invalid action token");
            return;
        }
        const String token = server.arg("token");
        char *end = nullptr;
        const unsigned long value = strtoul(token.c_str(), &end, 10);
        if (!token.length() || !end || *end != '\0' ||
            static_cast<uint32_t>(value) != bootSessionToken) {
            server.send(403, "text/plain; charset=utf-8", "Invalid action token");
            return;
        }
        if (cfg_web_auth_enabled &&
            !server.authenticate(cfg_web_username.c_str(), cfg_web_password.c_str())) {
            server.requestAuthentication();
            return;
        }
        bool enabled = false;
        String source;
        if (!webFirmwareAutoRead(enabled, source)) {
            server.send(503, "text/plain; charset=utf-8", "Update settings not readable");
            return;
        }
        String result;
        const bool ok = firmwareManifestProbe(source, result);
        server.send(ok ? 200 : 502, "text/plain; charset=utf-8", result);
    });

}
