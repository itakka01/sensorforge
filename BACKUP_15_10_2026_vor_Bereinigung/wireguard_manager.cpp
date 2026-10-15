#include "wireguard_manager.h"

#include "board_config.h"
#include "config.h"
#include "logger.h"

#include <WiFi.h>

// No WireGuard backend is linked in the current reference build.
// WireGuard-ESP32 0.1.5 still uses removed tcpip_adapter APIs on our
// Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 stack. Deliberately keep the third-party
// header out of this translation unit entirely so Arduino library discovery
// cannot pull that legacy library into the build merely because it is installed.
// A maintained ESP-NETIF-compatible backend will be added here only after its
// own isolated qualification.
#define SENSORFORGE_HAS_WIREGUARD_ARDUINO 0

namespace {

static const uint32_t WIREGUARD_RETRY_INTERVAL_MS = 60000UL;

struct RuntimeConfig {
    bool captured = false;
    int enabled = 0;
    String address;
    String privateKey;
    String peerEndpoint;
    String peerPublicKey;
    uint16_t peerPort = 51820;
};

RuntimeConfig runtimeConfig;
bool active = false;
uint32_t lastStartAttemptMs = 0;
String lastError;
String statusText = "not initialized";

#if SENSORFORGE_HAS_WIREGUARD_ARDUINO
WireGuard wireguard;
#endif

static bool ipv4Usable(const IPAddress &ip)
{
    return ip[0] || ip[1] || ip[2] || ip[3];
}

static bool parseIpv4(const String &text, IPAddress &ip)
{
    String normalized = text;
    normalized.trim();
    return normalized.length() && ip.fromString(normalized) && ipv4Usable(ip);
}

static bool runtimeConfigured()
{
    return
        runtimeConfig.enabled != 0 &&
        runtimeConfig.address.length() &&
        runtimeConfig.privateKey.length() &&
        runtimeConfig.peerEndpoint.length() &&
        runtimeConfig.peerPublicKey.length() &&
        runtimeConfig.peerPort > 0;
}

static void setStatus(const String &text, const String &error = String())
{
    statusText = text;
    lastError = error;
}

static void stopInternal(const char *reason)
{
#if SENSORFORGE_HAS_WIREGUARD_ARDUINO
    if (active || wireguard.is_initialized()) {
        wireguard.end();
    }
#endif
    active = false;

    if (reason && reason[0]) {
        setStatus(String("stopped: ") + reason);
        logWrite(String("WIREGUARD stopped | reason=") + reason);
    } else {
        setStatus("stopped");
    }
}

static bool tryStart()
{
    if (!runtimeConfig.captured) {
        setStatus("not initialized");
        return false;
    }

    if (!runtimeConfig.enabled) {
        setStatus("disabled");
        return false;
    }

#if !defined(BOARD_WIREGUARD_CAPABLE) || BOARD_WIREGUARD_CAPABLE == 0
    setStatus("unsupported on this board", "board capability disabled");
    return false;
#else
    if (!wireguardBuildAvailable()) {
        setStatus(
            "backend unavailable",
            "no qualified WireGuard backend for this build"
        );
        return false;
    }

    if (!runtimeConfigured()) {
        setStatus("configuration incomplete", "required WireGuard settings missing");
        return false;
    }

    const wifi_mode_t wifiMode = WiFi.getMode();
    const bool staMode = wifiMode == WIFI_MODE_STA || wifiMode == WIFI_MODE_APSTA;
    if (!staMode || WiFi.status() != WL_CONNECTED || !ipv4Usable(WiFi.localIP())) {
        setStatus("waiting for infrastructure WiFi");
        return false;
    }

    if (!timeIsValid()) {
        setStatus("waiting for valid system time");
        return false;
    }

    IPAddress localIp;
    if (!parseIpv4(runtimeConfig.address, localIp)) {
        setStatus("invalid VPN address", "wireguard_address is not a usable IPv4 address");
        return false;
    }

#if SENSORFORGE_HAS_WIREGUARD_ARDUINO
    logWrite(
        "WIREGUARD start | address=" + runtimeConfig.address +
        " | endpoint=" + runtimeConfig.peerEndpoint +
        ":" + String(runtimeConfig.peerPort)
    );

    const bool ok = wireguard.begin(
        localIp,
        runtimeConfig.privateKey.c_str(),
        runtimeConfig.peerEndpoint.c_str(),
        runtimeConfig.peerPublicKey.c_str(),
        runtimeConfig.peerPort
    );

    if (!ok) {
        active = false;
        setStatus("start failed", "WireGuard backend begin() failed");
        logWrite("WIREGUARD start FAILED");
        return false;
    }

    active = true;
    setStatus("interface initialized");
    logWrite(
        "WIREGUARD interface initialized | address=" + runtimeConfig.address +
        " | backend=" + String(wireguardBackendName())
    );
    return true;
#else
    setStatus("backend unavailable", "no qualified WireGuard backend for this build");
    return false;
#endif
#endif
}

} // namespace

void wireguardSetup()
{
    runtimeConfig.captured = true;
    runtimeConfig.enabled = cfg_wireguard_enabled;
    runtimeConfig.address = cfg_wireguard_address;
    runtimeConfig.privateKey = cfg_wireguard_private_key;
    runtimeConfig.peerEndpoint = cfg_wireguard_peer_endpoint;
    runtimeConfig.peerPublicKey = cfg_wireguard_peer_public_key;
    runtimeConfig.peerPort = (uint16_t)cfg_wireguard_peer_port;

    active = false;
    lastStartAttemptMs = 0;
    lastError = "";

    if (!wireguardBoardCapable()) {
        statusText = "unsupported on this board";
        lastError = "board capability disabled";
    } else if (!wireguardBuildAvailable()) {
        statusText = "backend unavailable";
        lastError = "no qualified WireGuard backend for this build";
    } else {
        statusText = runtimeConfig.enabled ? "waiting for network" : "disabled";
    }
}

void wireguardService(bool allowBlockingStart)
{
    if (!runtimeConfig.captured)
        return;

    if (!runtimeConfig.enabled) {
        if (active)
            stopInternal("disabled at boot");
        else
            setStatus("disabled");
        return;
    }

    if (active) {
        if (
            (WiFi.getMode() != WIFI_MODE_STA && WiFi.getMode() != WIFI_MODE_APSTA) ||
            WiFi.status() != WL_CONNECTED ||
            !ipv4Usable(WiFi.localIP())
        ) {
            stopInternal("underlying STA WiFi unavailable");
        }
        return;
    }

    if (!allowBlockingStart)
        return;

    const uint32_t now = millis();
    if (
        lastStartAttemptMs != 0 &&
        (uint32_t)(now - lastStartAttemptMs) < WIREGUARD_RETRY_INTERVAL_MS
    ) {
        return;
    }

    lastStartAttemptMs = now ? now : 1;
    tryStart();
}

void wireguardStop(const char *reason)
{
    if (active) {
        stopInternal(reason);
    } else if (reason && reason[0]) {
        setStatus(String("stopped: ") + reason);
    }
}

bool wireguardBuildAvailable()
{
#if SENSORFORGE_HAS_WIREGUARD_ARDUINO
    return true;
#else
    return false;
#endif
}

bool wireguardBoardCapable()
{
#if defined(BOARD_WIREGUARD_CAPABLE) && BOARD_WIREGUARD_CAPABLE
    return true;
#else
    return false;
#endif
}

bool wireguardConfigured()
{
    return runtimeConfigured();
}

bool wireguardActive()
{
    return active;
}

bool wireguardRuntimeConfigMatchesSaved()
{
    if (!runtimeConfig.captured)
        return false;

    return
        runtimeConfig.enabled == cfg_wireguard_enabled &&
        runtimeConfig.address == cfg_wireguard_address &&
        runtimeConfig.privateKey == cfg_wireguard_private_key &&
        runtimeConfig.peerEndpoint == cfg_wireguard_peer_endpoint &&
        runtimeConfig.peerPublicKey == cfg_wireguard_peer_public_key &&
        runtimeConfig.peerPort == (uint16_t)cfg_wireguard_peer_port;
}

const char *wireguardBackendName()
{
#if SENSORFORGE_HAS_WIREGUARD_ARDUINO
    return "WireGuard-ESP32";
#else
    return "none";
#endif
}

String wireguardStatusText()
{
    return statusText;
}

String wireguardLastError()
{
    return lastError;
}

String wireguardRuntimeAddress()
{
    return runtimeConfig.address;
}
