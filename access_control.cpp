#include "access_control.h"

#include "config.h"

#include <mbedtls/base64.h>
#include <string.h>

namespace {

static bool credentialsMatch(
    const String &username,
    const String &password,
    const String &expectedUsername,
    const String &expectedPassword
)
{
    if (!expectedUsername.length() || !expectedPassword.length())
        return false;

    return username == expectedUsername && password == expectedPassword;
}

static SensorForgeAccessRole credentialsRole(
    const String &username,
    const String &password
)
{
    if (credentialsMatch(username, password, cfg_web_username, cfg_web_password))
        return SENSORFORGE_ACCESS_ADMIN;

    for (size_t i = 0; i < SENSORFORGE_STREAM_USER_COUNT; ++i) {
        if (credentialsMatch(
                username,
                password,
                cfg_stream_usernames[i],
                cfg_stream_passwords[i]
            )) {
            return SENSORFORGE_ACCESS_STREAM;
        }
    }

    return SENSORFORGE_ACCESS_NONE;
}

static bool decodeBasicCredentials(
    const String &authorizationHeader,
    String &username,
    String &password
)
{
    username = "";
    password = "";

    String header = authorizationHeader;
    header.trim();

    if (!header.startsWith("Basic "))
        return false;

    String encoded = header.substring(6);
    encoded.trim();
    if (!encoded.length() || encoded.length() > 256)
        return false;

    uint8_t decoded[192];
    size_t decodedLength = 0;
    int result = mbedtls_base64_decode(
        decoded,
        sizeof(decoded) - 1,
        &decodedLength,
        reinterpret_cast<const unsigned char *>(encoded.c_str()),
        encoded.length()
    );

    if (result != 0 || decodedLength == 0 || decodedLength >= sizeof(decoded))
        return false;

    decoded[decodedLength] = 0;
    String pair(reinterpret_cast<const char *>(decoded));
    memset(decoded, 0, sizeof(decoded));

    int separator = pair.indexOf(':');
    if (separator <= 0)
        return false;

    username = pair.substring(0, separator);
    password = pair.substring(separator + 1);
    return true;
}

static bool streamReadOnlyPath(const String &uri)
{
    // Live camera / viewer.
    if (
        uri == "/viewer" ||
        uri == "/snapshot" ||
        uri == "/stream" ||
        uri == "/stream_view" ||
        uri == "/activity" ||
        uri == "/ui_status"
    ) {
        return true;
    }

    // Read-only ONVIF device/media web-service endpoints. They use POST for
    // SOAP reads, but SensorForge exposes no mutating ONVIF operations.
    if (
        uri == "/onvif/device_service" ||
        uri == "/onvif/media_service"
    ) {
        return true;
    }

    // Read-only recording browser and WebPlayer resources.
    if (
        uri == "/files" ||
        uri == "/files_day" ||
        uri == "/download_day" ||
        uri == "/download_day_status" ||
        uri == "/file" ||
        uri == "/play" ||
        uri == "/player_permissions.css" ||
        uri == "/player_neighbors" ||
        uri == "/player_meta" ||
        uri == "/player_frame" ||
        uri == "/player_batch" ||
        uri == "/player_audio" ||
        uri == "/player_subtitle" ||
        uri == "/player_annotation"
    ) {
        return true;
    }

    return false;
}

} // namespace

SensorForgeAccessRole accessControlWebRole(WebServer &server)
{
    if (!cfg_web_auth_enabled)
        return SENSORFORGE_ACCESS_ADMIN;

    if (server.authenticate(cfg_web_username.c_str(), cfg_web_password.c_str()))
        return SENSORFORGE_ACCESS_ADMIN;

    for (size_t i = 0; i < SENSORFORGE_STREAM_USER_COUNT; ++i) {
        if (!cfg_stream_usernames[i].length() || !cfg_stream_passwords[i].length())
            continue;

        if (server.authenticate(
                cfg_stream_usernames[i].c_str(),
                cfg_stream_passwords[i].c_str()
            )) {
            return SENSORFORGE_ACCESS_STREAM;
        }
    }

    return SENSORFORGE_ACCESS_NONE;
}

SensorForgeAccessRole accessControlAuthorizationRole(const String &authorizationHeader)
{
    if (!cfg_web_auth_enabled)
        return SENSORFORGE_ACCESS_ADMIN;

    String username;
    String password;
    if (!decodeBasicCredentials(authorizationHeader, username, password))
        return SENSORFORGE_ACCESS_NONE;

    return credentialsRole(username, password);
}

bool accessControlWebRequestAllowed(
    SensorForgeAccessRole role,
    HTTPMethod method,
    const String &uri
)
{
    if (role == SENSORFORGE_ACCESS_ADMIN)
        return true;

    if (role != SENSORFORGE_ACCESS_STREAM)
        return false;

    if (method == HTTP_GET) {
        if (uri == "/")
            return true;

        return streamReadOnlyPath(uri);
    }

    if (method == HTTP_POST) {
        if (uri == "/onvif/device_service" || uri == "/onvif/media_service")
            return true;

        if (uri == "/player_annotation")
            return cfg_stream_allow_annotation_edit != 0;

        if (uri == "/player_delete" || uri == "/delete_day")
            return cfg_stream_allow_delete != 0;
    }

    return false;
}

bool accessControlAnyStreamingUserConfigured()
{
    for (size_t i = 0; i < SENSORFORGE_STREAM_USER_COUNT; ++i) {
        if (cfg_stream_usernames[i].length() && cfg_stream_passwords[i].length())
            return true;
    }
    return false;
}

const char *accessControlRoleName(SensorForgeAccessRole role)
{
    switch (role) {
        case SENSORFORGE_ACCESS_ADMIN:
            return "admin";
        case SENSORFORGE_ACCESS_STREAM:
            return "stream";
        default:
            return "none";
    }
}
