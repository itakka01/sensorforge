#include "onvif.h"

#include <WebServer.h>
#include <ctype.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <time.h>

#include "access_control.h"
#include "board_config.h"
#include "branding.h"
#include "config.h"
#include "logger.h"
#include "sensorforge_version.h"
#include "streamer.h"

namespace {

static const IPAddress ONVIF_DISCOVERY_GROUP(239, 255, 255, 250);
static const uint16_t ONVIF_DISCOVERY_PORT = 3702;
static const size_t ONVIF_DISCOVERY_RX_MAX = 1535;
static const uint32_t ONVIF_DISCOVERY_RETRY_MS = 5000;
static const char *ONVIF_PROFILE_TOKEN = "sensorforge_main";
static const char *ONVIF_VIDEO_SOURCE_TOKEN = "sensorforge_video_source";
static const char *ONVIF_VIDEO_SOURCE_CONFIG_TOKEN = "sensorforge_video_source_cfg";
static const char *ONVIF_VIDEO_ENCODER_CONFIG_TOKEN = "sensorforge_video_encoder_cfg";

static WiFiUDP discoveryUdp;
static bool discoveryActive = false;
static uint32_t nextDiscoveryRetryMs = 0;
static uint32_t discoveryProbeRx = 0;
static uint32_t discoveryProbeMatchTx = 0;
static IPAddress discoveryLastRemoteIp;
static uint16_t discoveryLastRemotePort = 0;
static String lastError;
static String endpointUuid;
static WebServer *webServer = nullptr;

static String xmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static String uriComponent(const String &value)
{
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = (uint8_t)value[i];
        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += HEX_DIGITS[(c >> 4) & 0x0F];
            out += HEX_DIGITS[c & 0x0F];
        }
    }
    return out;
}

static IPAddress activeNetworkIp()
{
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
        const IPAddress apIp = WiFi.softAPIP();
        if (apIp[0] || apIp[1] || apIp[2] || apIp[3])
            return apIp;
    }
    return WiFi.localIP();
}

static bool ipIsUsable(const IPAddress &ip)
{
    return ip[0] || ip[1] || ip[2] || ip[3];
}

static String httpBaseUrl()
{
    const IPAddress ip = activeNetworkIp();
    if (ipIsUsable(ip))
        return "http://" + ip.toString();
    return "http://" + cfg_hostname + ".local";
}

static String rtspUrlForOnvif()
{
    const IPAddress ip = activeNetworkIp();
    if (ipIsUsable(ip))
        return "rtsp://" + ip.toString() + ":554/stream";
    return streamerRtspUrl();
}

static String snapshotUrlForOnvif()
{
    return httpBaseUrl() + "/snapshot";
}

static String boardModel()
{
#if defined(BOARD_XIAO)
    return "XIAO ESP32S3 Sense";
#elif defined(BOARD_FREENOVE)
    return "Freenove ESP32-S3 Camera";
#else
    return "ESP32-S3 Camera";
#endif
}

static String factorySerial()
{
    const uint64_t mac = ESP.getEfuseMac();
    char serial[17];
    snprintf(
        serial,
        sizeof(serial),
        "%012llX",
        (unsigned long long)(mac & 0x0000FFFFFFFFFFFFULL)
    );
    return String(serial);
}

static String buildEndpointUuid()
{
    // Stable, non-secret endpoint identity derived from the globally unique
    // factory MAC. Version/variant bits are fixed to a UUID-style v5 layout;
    // no security property depends on this identifier.
    const uint64_t mac = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
    char uuid[48];
    snprintf(
        uuid,
        sizeof(uuid),
        "urn:uuid:53464f52-4745-5000-8000-%012llx",
        (unsigned long long)mac
    );
    return String(uuid);
}

static const String &stableEndpointUuid()
{
    if (!endpointUuid.length())
        endpointUuid = buildEndpointUuid();
    return endpointUuid;
}

static void configuredResolution(uint16_t &width, uint16_t &height)
{
    width = 1024;
    height = 768;
    const int separator = cfg_resolution.indexOf('x');
    if (separator <= 0)
        return;

    const int w = cfg_resolution.substring(0, separator).toInt();
    const int h = cfg_resolution.substring(separator + 1).toInt();
    if (w > 0 && w <= 4096 && h > 0 && h <= 4096) {
        width = (uint16_t)w;
        height = (uint16_t)h;
    }
}

static int onvifQuality()
{
    // ESP JPEG quality is inverse (lower = better). ONVIF uses a direct
    // quality scale. The exact scale is implementation-defined, so expose a
    // stable human-friendly approximation only.
    int quality = 100 - (cfg_quality * 2);
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    return quality;
}

static String scopesText()
{
    String scopes =
        "onvif://www.onvif.org/type/video_encoder "
        "onvif://www.onvif.org/type/Network_Video_Transmitter "
        "onvif://www.onvif.org/Profile/Streaming "
        "onvif://www.onvif.org/hardware/SensorForge "
        "onvif://www.onvif.org/name/" + uriComponent(cfg_hostname);

    if (cfg_camera_location.length())
        scopes += " onvif://www.onvif.org/location/" + uriComponent(cfg_camera_location);

    return scopes;
}

static bool xmlQualifiedNameMatchesLocal(const String &qualifiedName, const char *localName)
{
    int colon = -1;
    for (size_t i = 0; i < qualifiedName.length(); ++i) {
        if (qualifiedName[i] == ':')
            colon = (int)i;
    }
    const String local = colon >= 0
        ? qualifiedName.substring(colon + 1)
        : qualifiedName;
    return local == localName;
}

static String xmlUnescapeText(String value)
{
    // The ONVIF fields parsed here are simple text nodes. Decode the standard
    // XML entities so usernames/profile tokens containing reserved characters
    // compare against their configured SensorForge values correctly.
    value.replace("&quot;", "\"");
    value.replace("&apos;", "'");
    value.replace("&lt;", "<");
    value.replace("&gt;", ">");
    value.replace("&amp;", "&");
    return value;
}

static String extractElementText(const String &xml, const char *localName)
{
    // Match by XML local-name, not by a literal opening tag. WS-Security
    // commonly adds attributes, for example:
    //   <wsse:Password Type="...#PasswordDigest">...</wsse:Password>
    //   <wsse:Nonce EncodingType="...#Base64Binary">...</wsse:Nonce>
    // The previous parser required the opening tag to end immediately after
    // the element name and therefore silently missed these valid ONVIF tags.
    int scan = 0;
    while (true) {
        const int open = xml.indexOf('<', scan);
        if (open < 0)
            return "";

        const int tagEnd = xml.indexOf('>', open + 1);
        if (tagEnd < 0)
            return "";

        int nameStart = open + 1;
        while (nameStart < tagEnd && isspace((unsigned char)xml[nameStart]))
            ++nameStart;

        if (nameStart >= tagEnd ||
            xml[nameStart] == '/' ||
            xml[nameStart] == '!' ||
            xml[nameStart] == '?') {
            scan = tagEnd + 1;
            continue;
        }

        int nameEnd = nameStart;
        while (nameEnd < tagEnd) {
            const char c = xml[nameEnd];
            if (isspace((unsigned char)c) || c == '/' || c == '>')
                break;
            ++nameEnd;
        }

        const String qualifiedName = xml.substring(nameStart, nameEnd);
        if (!xmlQualifiedNameMatchesLocal(qualifiedName, localName)) {
            scan = tagEnd + 1;
            continue;
        }

        int beforeEnd = tagEnd - 1;
        while (beforeEnd > nameEnd && isspace((unsigned char)xml[beforeEnd]))
            --beforeEnd;
        if (beforeEnd >= nameEnd && xml[beforeEnd] == '/')
            return "";

        const int valueStart = tagEnd + 1;
        int closeScan = valueStart;
        while (true) {
            const int close = xml.indexOf("</", closeScan);
            if (close < 0)
                return "";

            const int closeEnd = xml.indexOf('>', close + 2);
            if (closeEnd < 0)
                return "";

            int closeNameStart = close + 2;
            while (closeNameStart < closeEnd &&
                   isspace((unsigned char)xml[closeNameStart])) {
                ++closeNameStart;
            }

            int closeNameEnd = closeNameStart;
            while (closeNameEnd < closeEnd) {
                const char c = xml[closeNameEnd];
                if (isspace((unsigned char)c) || c == '>')
                    break;
                ++closeNameEnd;
            }

            const String closeQualifiedName =
                xml.substring(closeNameStart, closeNameEnd);
            if (xmlQualifiedNameMatchesLocal(closeQualifiedName, localName)) {
                String value = xml.substring(valueStart, close);
                value.trim();
                return xmlUnescapeText(value);
            }

            closeScan = closeEnd + 1;
        }
    }
}

static bool hasOperation(const String &body, const char *operation);

static bool constantTimeStringEquals(const String &a, const String &b)
{
    if (a.length() != b.length())
        return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); ++i)
        diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
    return diff == 0;
}

static const String *passwordForUsername(const String &username)
{
    if (username == cfg_web_username && cfg_web_password.length())
        return &cfg_web_password;

    for (size_t i = 0; i < SENSORFORGE_STREAM_USER_COUNT; ++i) {
        if (
            cfg_stream_usernames[i].length() &&
            cfg_stream_passwords[i].length() &&
            username == cfg_stream_usernames[i]
        ) {
            return &cfg_stream_passwords[i];
        }
    }

    return nullptr;
}

static bool computeWssePasswordDigest(
    const String &nonceBase64,
    const String &created,
    const String &password,
    String &digestBase64
)
{
    digestBase64 = "";
    if (!nonceBase64.length() || !created.length() || !password.length())
        return false;

    uint8_t nonce[128] = {};
    size_t nonceLength = 0;
    if (
        mbedtls_base64_decode(
            nonce,
            sizeof(nonce),
            &nonceLength,
            reinterpret_cast<const unsigned char *>(nonceBase64.c_str()),
            nonceBase64.length()
        ) != 0 ||
        nonceLength == 0
    ) {
        return false;
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        memset(nonce, 0, sizeof(nonce));
        return false;
    }

    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&operation, PSA_ALG_SHA_1) != PSA_SUCCESS) {
        memset(nonce, 0, sizeof(nonce));
        return false;
    }

    psa_status_t status = psa_hash_update(&operation, nonce, nonceLength);
    if (status == PSA_SUCCESS) {
        status = psa_hash_update(
            &operation,
            reinterpret_cast<const uint8_t *>(created.c_str()),
            created.length()
        );
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_update(
            &operation,
            reinterpret_cast<const uint8_t *>(password.c_str()),
            password.length()
        );
    }

    uint8_t digest[20] = {};
    size_t digestLength = 0;
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(
            &operation,
            digest,
            sizeof(digest),
            &digestLength
        );
    }
    psa_hash_abort(&operation);
    memset(nonce, 0, sizeof(nonce));

    if (status != PSA_SUCCESS || digestLength != sizeof(digest)) {
        memset(digest, 0, sizeof(digest));
        return false;
    }

    unsigned char encoded[40] = {};
    size_t encodedLength = 0;
    const int encodeResult = mbedtls_base64_encode(
        encoded,
        sizeof(encoded) - 1,
        &encodedLength,
        digest,
        sizeof(digest)
    );
    memset(digest, 0, sizeof(digest));

    if (encodeResult != 0 || encodedLength == 0 || encodedLength >= sizeof(encoded))
        return false;

    encoded[encodedLength] = 0;
    digestBase64 = String(reinterpret_cast<const char *>(encoded));
    memset(encoded, 0, sizeof(encoded));
    return true;
}

static bool wsseUsernameTokenValid(const String &body)
{
    // ONVIF UsernameToken Profile: PasswordDigest =
    // Base64(SHA1(Base64Decode(Nonce) + Created + Password)).
    // Plaintext SOAP passwords are deliberately not accepted.
    if (body.indexOf("#PasswordDigest") < 0)
        return false;

    const String username = extractElementText(body, "Username");
    const String suppliedDigest = extractElementText(body, "Password");
    const String nonce = extractElementText(body, "Nonce");
    const String created = extractElementText(body, "Created");

    if (
        !username.length() ||
        !suppliedDigest.length() ||
        !nonce.length() ||
        !created.length()
    ) {
        return false;
    }

    const String *password = passwordForUsername(username);
    if (!password)
        return false;

    String expectedDigest;
    if (!computeWssePasswordDigest(nonce, created, *password, expectedDigest))
        return false;

    return constantTimeStringEquals(suppliedDigest, expectedDigest);
}

static bool deviceOperationIsPreAuth(const String &body)
{
    // ONVIF Core marks these Device service reads PRE_AUTH. Clients commonly
    // query time first so they can construct a valid UsernameToken timestamp.
    return
        hasOperation(body, "GetSystemDateAndTime") ||
        hasOperation(body, "GetCapabilities") ||
        hasOperation(body, "GetServices") ||
        hasOperation(body, "GetServiceCapabilities") ||
        hasOperation(body, "GetHostname");
}

static bool onvifRequestAuthenticated(
    WebServer &server,
    const String &body,
    bool preAuthAllowed
)
{
    if (!cfg_web_auth_enabled || preAuthAllowed)
        return true;

    // Keep HTTP Basic/Digest compatibility for clients that use the HTTP
    // authentication layer, and additionally accept ONVIF WS-Security.
    if (accessControlWebRole(server) != SENSORFORGE_ACCESS_NONE)
        return true;

    return wsseUsernameTokenValid(body);
}

static bool requireOnvifAuthentication(
    WebServer &server,
    const String &body,
    bool preAuthAllowed
)
{
    if (onvifRequestAuthenticated(server, body, preAuthAllowed))
        return true;

    server.requestAuthentication(DIGEST_AUTH, "SensorForge ONVIF");
    return false;
}

static bool hasOperation(const String &body, const char *operation)
{
    const String direct = "<" + String(operation);
    if (body.indexOf(direct) >= 0)
        return true;

    const String needle = ":" + String(operation);
    int p = body.indexOf(needle);
    while (p >= 0) {
        const int after = p + needle.length();
        if (after < (int)body.length()) {
            const char c = body[after];
            if (c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n')
                return true;
        }
        p = body.indexOf(needle, p + 1);
    }
    return false;
}

static String soapEnvelope(const String &body)
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
        "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
        "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
        "xmlns:ter=\"http://www.onvif.org/ver10/error\">"
        "<s:Body>" + body + "</s:Body></s:Envelope>";
}

static void sendSoap(WebServer &server, const String &body)
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/soap+xml; charset=utf-8", soapEnvelope(body));
}

static void sendSoapFault(WebServer &server, const String &reason)
{
    const String body =
        "<s:Fault>"
        "<s:Code><s:Value>s:Sender</s:Value>"
        "<s:Subcode><s:Value>ter:ActionNotSupported</s:Value></s:Subcode>"
        "</s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">" + xmlEscape(reason) + "</s:Text></s:Reason>"
        "</s:Fault>";
    server.sendHeader("Cache-Control", "no-store");
    server.send(400, "application/soap+xml; charset=utf-8", soapEnvelope(body));
}

static String deviceCapabilitiesXml()
{
    return
        "<tt:Device>"
        "<tt:XAddr>" + xmlEscape(onvifDeviceServiceUrl()) + "</tt:XAddr>"
        "<tt:Network><tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
        "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS></tt:Network>"
        "<tt:System><tt:DiscoveryResolve>true</tt:DiscoveryResolve><tt:DiscoveryBye>true</tt:DiscoveryBye>"
        "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup>"
        "<tt:SystemLogging>false</tt:SystemLogging><tt:FirmwareUpgrade>false</tt:FirmwareUpgrade></tt:System>"
        "<tt:IO><tt:InputConnectors>0</tt:InputConnectors><tt:RelayOutputs>0</tt:RelayOutputs></tt:IO>"
        "<tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
        "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
        "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
        "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken></tt:Security>"
        "</tt:Device>";
}

static String mediaCapabilitiesXml()
{
    return
        "<tt:Media>"
        "<tt:XAddr>" + xmlEscape(onvifMediaServiceUrl()) + "</tt:XAddr>"
        "<tt:StreamingCapabilities>"
        "<tt:RTPMulticast>false</tt:RTPMulticast>"
        "<tt:RTP_TCP>true</tt:RTP_TCP>"
        "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
        "</tt:StreamingCapabilities>"
        "</tt:Media>";
}

static String profileXml()
{
    uint16_t width = 0;
    uint16_t height = 0;
    configuredResolution(width, height);

    String xml;
    xml.reserve(2200);
    xml +=
        "<trt:Profiles token=\"" + String(ONVIF_PROFILE_TOKEN) + "\" fixed=\"true\">"
        "<tt:Name>SensorForge Main</tt:Name>"
        "<tt:VideoSourceConfiguration token=\"" + String(ONVIF_VIDEO_SOURCE_CONFIG_TOKEN) + "\">"
        "<tt:Name>SensorForge Camera</tt:Name><tt:UseCount>1</tt:UseCount>"
        "<tt:SourceToken>" + String(ONVIF_VIDEO_SOURCE_TOKEN) + "</tt:SourceToken>"
        "<tt:Bounds x=\"0\" y=\"0\" width=\"" + String(width) + "\" height=\"" + String(height) + "\"/>"
        "</tt:VideoSourceConfiguration>"
        "<tt:VideoEncoderConfiguration token=\"" + String(ONVIF_VIDEO_ENCODER_CONFIG_TOKEN) + "\">"
        "<tt:Name>SensorForge JPEG</tt:Name><tt:UseCount>1</tt:UseCount>"
        "<tt:Encoding>JPEG</tt:Encoding>"
        "<tt:Resolution><tt:Width>" + String(width) + "</tt:Width><tt:Height>" + String(height) + "</tt:Height></tt:Resolution>"
        "<tt:Quality>" + String(onvifQuality()) + "</tt:Quality>"
        "<tt:RateControl><tt:FrameRateLimit>" + String(cfg_fps) + "</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>0</tt:BitrateLimit></tt:RateControl>"
        "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
        "<tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
        "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
        "</tt:VideoEncoderConfiguration>"
        "</trt:Profiles>";
    return xml;
}

static String profileTokenFromRequest(const String &body)
{
    return extractElementText(body, "ProfileToken");
}

static bool profileTokenValid(const String &body)
{
    const String token = profileTokenFromRequest(body);
    return !token.length() || token == ONVIF_PROFILE_TOKEN;
}

static void handleDeviceService()
{
    if (!webServer)
        return;

    WebServer &server = *webServer;
    if (!streamerModeEnabled() || !cfg_onvif_enabled || !cfg_streamer_rtsp_enabled) {
        server.send(503, "text/plain; charset=utf-8", "ONVIF is not active");
        return;
    }

    const String body = server.arg("plain");

    if (!requireOnvifAuthentication(server, body, deviceOperationIsPreAuth(body)))
        return;

    if (hasOperation(body, "GetDeviceInformation")) {
        sendSoap(server,
            "<tds:GetDeviceInformationResponse>"
            "<tds:Manufacturer>SensorForge</tds:Manufacturer>"
            "<tds:Model>" + xmlEscape(boardModel()) + "</tds:Model>"
            "<tds:FirmwareVersion>" + xmlEscape(String(SENSORFORGE_RELEASE_TAG)) + "</tds:FirmwareVersion>"
            "<tds:SerialNumber>" + xmlEscape(factorySerial()) + "</tds:SerialNumber>"
            "<tds:HardwareId>ESP32-S3</tds:HardwareId>"
            "</tds:GetDeviceInformationResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetCapabilities")) {
        sendSoap(server,
            "<tds:GetCapabilitiesResponse><tds:Capabilities>" +
            deviceCapabilitiesXml() + mediaCapabilitiesXml() +
            "</tds:Capabilities></tds:GetCapabilitiesResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetServices")) {
        const String deviceUrl = xmlEscape(onvifDeviceServiceUrl());
        const String mediaUrl = xmlEscape(onvifMediaServiceUrl());
        sendSoap(server,
            "<tds:GetServicesResponse>"
            "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
            "<tds:XAddr>" + deviceUrl + "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor></tds:Version></tds:Service>"
            "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
            "<tds:XAddr>" + mediaUrl + "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>3</tt:Minor></tds:Version></tds:Service>"
            "</tds:GetServicesResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetServiceCapabilities")) {
        sendSoap(server,
            "<tds:GetServiceCapabilitiesResponse><tds:Capabilities>"
            "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
            "<tds:Security TLS1.0=\"false\" TLS1.1=\"false\" TLS1.2=\"false\" OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" "
            "DefaultAccessPolicy=\"false\" Dot1X=\"false\" RemoteUserHandling=\"false\" X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" "
            "RELToken=\"false\" UsernameToken=\"true\" HttpDigest=\"true\"/>"
            "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetScopes")) {
        String scopes = scopesText();
        String response = "<tds:GetScopesResponse>";
        int start = 0;
        while (start < (int)scopes.length()) {
            int end = scopes.indexOf(' ', start);
            if (end < 0) end = scopes.length();
            String scope = scopes.substring(start, end);
            if (scope.length()) {
                response +=
                    "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>" +
                    xmlEscape(scope) + "</tt:ScopeItem></tds:Scopes>";
            }
            start = end + 1;
        }
        response += "</tds:GetScopesResponse>";
        sendSoap(server, response);
        return;
    }

    if (hasOperation(body, "GetSystemDateAndTime")) {
        time_t now = time(nullptr);
        if (now < 1577836800)
            now = 1577836800;
        struct tm utcTime;
        gmtime_r(&now, &utcTime);

        String dateTime;
        dateTime.reserve(560);
        dateTime =
            "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
            "<tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
            "<tt:TimeZone><tt:TZ>" + xmlEscape(cfg_timezone) + "</tt:TZ></tt:TimeZone>"
            "<tt:UTCDateTime><tt:Time><tt:Hour>" + String(utcTime.tm_hour) + "</tt:Hour>"
            "<tt:Minute>" + String(utcTime.tm_min) + "</tt:Minute><tt:Second>" + String(utcTime.tm_sec) + "</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>" + String(utcTime.tm_year + 1900) + "</tt:Year>"
            "<tt:Month>" + String(utcTime.tm_mon + 1) + "</tt:Month><tt:Day>" + String(utcTime.tm_mday) + "</tt:Day></tt:Date></tt:UTCDateTime>"
            "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>";
        sendSoap(server, dateTime);
        return;
    }

    if (hasOperation(body, "GetHostname")) {
        sendSoap(server,
            "<tds:GetHostnameResponse><tds:HostnameInformation>"
            "<tt:FromDHCP>false</tt:FromDHCP><tt:Name>" + xmlEscape(cfg_hostname) + "</tt:Name>"
            "</tds:HostnameInformation></tds:GetHostnameResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetDiscoveryMode")) {
        sendSoap(server,
            "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>"
        );
        return;
    }

    sendSoapFault(server, "Unsupported ONVIF device operation");
}

static void handleMediaService()
{
    if (!webServer)
        return;

    WebServer &server = *webServer;
    if (!streamerModeEnabled() || !cfg_onvif_enabled || !cfg_streamer_rtsp_enabled) {
        server.send(503, "text/plain; charset=utf-8", "ONVIF is not active");
        return;
    }

    const String body = server.arg("plain");

    if (!requireOnvifAuthentication(server, body, false))
        return;

    if (hasOperation(body, "GetServiceCapabilities")) {
        sendSoap(server,
            "<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">"
            "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
            "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" NonAggregateControl=\"false\" NoRTSPStreaming=\"false\"/>"
            "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetProfiles")) {
        sendSoap(server, "<trt:GetProfilesResponse>" + profileXml() + "</trt:GetProfilesResponse>");
        return;
    }

    if (hasOperation(body, "GetProfile")) {
        if (!profileTokenValid(body)) {
            sendSoapFault(server, "Unknown media profile");
            return;
        }
        String one = profileXml();
        one.replace("<trt:Profiles", "<trt:Profile");
        one.replace("</trt:Profiles>", "</trt:Profile>");
        sendSoap(server, "<trt:GetProfileResponse>" + one + "</trt:GetProfileResponse>");
        return;
    }

    if (hasOperation(body, "GetVideoSources")) {
        uint16_t width = 0;
        uint16_t height = 0;
        configuredResolution(width, height);
        sendSoap(server,
            "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"" + String(ONVIF_VIDEO_SOURCE_TOKEN) + "\">"
            "<tt:Framerate>" + String(cfg_fps) + "</tt:Framerate>"
            "<tt:Resolution><tt:Width>" + String(width) + "</tt:Width><tt:Height>" + String(height) + "</tt:Height></tt:Resolution>"
            "</trt:VideoSources></trt:GetVideoSourcesResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetVideoSourceConfigurations")) {
        uint16_t width = 0;
        uint16_t height = 0;
        configuredResolution(width, height);
        sendSoap(server,
            "<trt:GetVideoSourceConfigurationsResponse>"
            "<trt:Configurations token=\"" + String(ONVIF_VIDEO_SOURCE_CONFIG_TOKEN) + "\">"
            "<tt:Name>SensorForge Camera</tt:Name><tt:UseCount>1</tt:UseCount>"
            "<tt:SourceToken>" + String(ONVIF_VIDEO_SOURCE_TOKEN) + "</tt:SourceToken>"
            "<tt:Bounds x=\"0\" y=\"0\" width=\"" + String(width) + "\" height=\"" + String(height) + "\"/>"
            "</trt:Configurations></trt:GetVideoSourceConfigurationsResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetVideoEncoderConfigurations")) {
        uint16_t width = 0;
        uint16_t height = 0;
        configuredResolution(width, height);
        sendSoap(server,
            "<trt:GetVideoEncoderConfigurationsResponse>"
            "<trt:Configurations token=\"" + String(ONVIF_VIDEO_ENCODER_CONFIG_TOKEN) + "\">"
            "<tt:Name>SensorForge JPEG</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>JPEG</tt:Encoding>"
            "<tt:Resolution><tt:Width>" + String(width) + "</tt:Width><tt:Height>" + String(height) + "</tt:Height></tt:Resolution>"
            "<tt:Quality>" + String(onvifQuality()) + "</tt:Quality>"
            "<tt:RateControl><tt:FrameRateLimit>" + String(cfg_fps) + "</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>0</tt:BitrateLimit></tt:RateControl>"
            "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port><tt:TTL>1</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
            "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
            "</trt:Configurations></trt:GetVideoEncoderConfigurationsResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetVideoEncoderConfigurationOptions")) {
        uint16_t width = 0;
        uint16_t height = 0;
        configuredResolution(width, height);
        sendSoap(server,
            "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
            "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>100</tt:Max></tt:QualityRange>"
            "<tt:JPEG><tt:ResolutionsAvailable><tt:Width>" + String(width) + "</tt:Width><tt:Height>" + String(height) + "</tt:Height></tt:ResolutionsAvailable>"
            "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>" + String(cfg_fps > 0 ? cfg_fps : 1) + "</tt:Max></tt:FrameRateRange>"
            "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange></tt:JPEG>"
            "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetStreamUri")) {
        if (!profileTokenValid(body)) {
            sendSoapFault(server, "Unknown media profile");
            return;
        }
        sendSoap(server,
            "<trt:GetStreamUriResponse><trt:MediaUri>"
            "<tt:Uri>" + xmlEscape(rtspUrlForOnvif()) + "</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT0S</tt:Timeout>"
            "</trt:MediaUri></trt:GetStreamUriResponse>"
        );
        return;
    }

    if (hasOperation(body, "GetSnapshotUri")) {
        if (!profileTokenValid(body)) {
            sendSoapFault(server, "Unknown media profile");
            return;
        }
        sendSoap(server,
            "<trt:GetSnapshotUriResponse><trt:MediaUri>"
            "<tt:Uri>" + xmlEscape(snapshotUrlForOnvif()) + "</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT0S</tt:Timeout>"
            "</trt:MediaUri></trt:GetSnapshotUriResponse>"
        );
        return;
    }

    sendSoapFault(server, "Unsupported ONVIF media operation");
}

static String randomMessageUuid()
{
    const uint32_t a = esp_random();
    const uint32_t b = esp_random();
    const uint32_t c = esp_random();
    const uint32_t d = esp_random();
    char buffer[64];
    snprintf(
        buffer,
        sizeof(buffer),
        "urn:uuid:%08lx-%04lx-4%03lx-8%03lx-%08lx%04lx",
        (unsigned long)a,
        (unsigned long)((b >> 16) & 0xFFFFUL),
        (unsigned long)(b & 0x0FFFUL),
        (unsigned long)(c & 0x0FFFUL),
        (unsigned long)d,
        (unsigned long)((c >> 16) & 0xFFFFUL)
    );
    return String(buffer);
}

static bool sendDiscoveryPacket(const IPAddress &ip, uint16_t port, const String &xml)
{
    if (xml.length() > 1450) {
        lastError = "ONVIF discovery response too large";
        return false;
    }
    if (!discoveryUdp.beginPacket(ip, port)) {
        lastError = "ONVIF discovery beginPacket failed";
        return false;
    }
    discoveryUdp.write((const uint8_t *)xml.c_str(), xml.length());
    if (!discoveryUdp.endPacket()) {
        lastError = "ONVIF discovery endPacket failed";
        return false;
    }
    return true;
}

static String discoveryEnvelope(
    const String &discoveryNs,
    const String &addressingNs,
    const String &action,
    const String &relatesTo,
    const String &body
)
{
    const String anonymous =
        addressingNs == "http://www.w3.org/2005/08/addressing"
        ? addressingNs + "/anonymous"
        : addressingNs + "/role/anonymous";

    String xml;
    xml.reserve(1400);
    xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"" + addressingNs + "\" "
        "xmlns:d=\"" + discoveryNs + "\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" "
        "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<e:Header><w:MessageID>" + randomMessageUuid() + "</w:MessageID>"
        "<w:RelatesTo>" + xmlEscape(relatesTo) + "</w:RelatesTo>"
        "<w:To>" + anonymous + "</w:To><w:Action>" + action + "</w:Action></e:Header>"
        "<e:Body>" + body + "</e:Body></e:Envelope>";
    return xml;
}

static String discoveryMatchBody(bool resolve)
{
    const String wrapperStart = resolve ? "<d:ResolveMatches><d:ResolveMatch>" : "<d:ProbeMatches><d:ProbeMatch>";
    const String wrapperEnd = resolve ? "</d:ResolveMatch></d:ResolveMatches>" : "</d:ProbeMatch></d:ProbeMatches>";
    return
        wrapperStart +
        "<w:EndpointReference><w:Address>" + stableEndpointUuid() + "</w:Address></w:EndpointReference>"
        "<d:Types>tds:Device dn:NetworkVideoTransmitter</d:Types>"
        "<d:Scopes>" + xmlEscape(scopesText()) + "</d:Scopes>"
        "<d:XAddrs>" + xmlEscape(onvifDeviceServiceUrl()) + "</d:XAddrs>"
        "<d:MetadataVersion>1</d:MetadataVersion>" +
        wrapperEnd;
}

static void processDiscoveryPacket(const String &packet, const IPAddress &remoteIp, uint16_t remotePort)
{
    const bool modern = packet.indexOf("http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01") >= 0;
    const String discoveryNs = modern
        ? "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
        : "http://schemas.xmlsoap.org/ws/2005/04/discovery";
    const String addressingNs = modern
        ? "http://www.w3.org/2005/08/addressing"
        : "http://schemas.xmlsoap.org/ws/2004/08/addressing";

    String messageId = extractElementText(packet, "MessageID");
    if (!messageId.length())
        messageId = "urn:uuid:00000000-0000-0000-0000-000000000000";

    if (hasOperation(packet, "Probe")) {
        ++discoveryProbeRx;
        discoveryLastRemoteIp = remoteIp;
        discoveryLastRemotePort = remotePort;
        const String action = discoveryNs + "/ProbeMatches";
        const String body = discoveryMatchBody(false);
        if (sendDiscoveryPacket(
                remoteIp,
                remotePort,
                discoveryEnvelope(discoveryNs, addressingNs, action, messageId, body))) {
            ++discoveryProbeMatchTx;
        }
        return;
    }

    if (hasOperation(packet, "Resolve")) {
        const String address = extractElementText(packet, "Address");
        if (address.length() && address != stableEndpointUuid())
            return;
        const String action = discoveryNs + "/ResolveMatches";
        const String body = discoveryMatchBody(true);
        sendDiscoveryPacket(
            remoteIp,
            remotePort,
            discoveryEnvelope(discoveryNs, addressingNs, action, messageId, body)
        );
    }
}

static void sendHello()
{
    if (!discoveryActive)
        return;

    const String discoveryNs = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
    const String addressingNs = "http://schemas.xmlsoap.org/ws/2004/08/addressing";
    String xml;
    xml.reserve(1300);
    xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"" + addressingNs + "\" xmlns:d=\"" + discoveryNs + "\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<e:Header><w:MessageID>" + randomMessageUuid() + "</w:MessageID>"
        "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action>" + discoveryNs + "/Hello</w:Action></e:Header><e:Body><d:Hello>"
        "<w:EndpointReference><w:Address>" + stableEndpointUuid() + "</w:Address></w:EndpointReference>"
        "<d:Types>tds:Device dn:NetworkVideoTransmitter</d:Types>"
        "<d:Scopes>" + xmlEscape(scopesText()) + "</d:Scopes>"
        "<d:XAddrs>" + xmlEscape(onvifDeviceServiceUrl()) + "</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion>"
        "</d:Hello></e:Body></e:Envelope>";

    if (xml.length() <= 1450 && discoveryUdp.beginMulticastPacket()) {
        discoveryUdp.write((const uint8_t *)xml.c_str(), xml.length());
        discoveryUdp.endPacket();
    }
}

static void sendBye()
{
    if (!discoveryActive)
        return;

    const String discoveryNs = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
    const String addressingNs = "http://schemas.xmlsoap.org/ws/2004/08/addressing";
    String xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"" + addressingNs + "\" xmlns:d=\"" + discoveryNs + "\">"
        "<e:Header><w:MessageID>" + randomMessageUuid() + "</w:MessageID>"
        "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action>" + discoveryNs + "/Bye</w:Action></e:Header><e:Body><d:Bye>"
        "<w:EndpointReference><w:Address>" + stableEndpointUuid() + "</w:Address></w:EndpointReference>"
        "</d:Bye></e:Body></e:Envelope>";

    if (xml.length() <= 1450 && discoveryUdp.beginMulticastPacket()) {
        discoveryUdp.write((const uint8_t *)xml.c_str(), xml.length());
        discoveryUdp.endPacket();
    }
}

} // namespace

void onvifRegisterWebRoutes(WebServer &server)
{
    webServer = &server;
    server.on("/onvif/device_service", HTTP_POST, handleDeviceService);
    server.on("/onvif/media_service", HTTP_POST, handleMediaService);
}

bool onvifBegin(String &error)
{
    error = "";
    lastError = "";

    if (!streamerModeEnabled() || !cfg_onvif_enabled || !cfg_streamer_rtsp_enabled) {
        nextDiscoveryRetryMs = 0;
        return true;
    }

    if (discoveryActive)
        return true;

    const IPAddress ip = activeNetworkIp();
    if (!ipIsUsable(ip)) {
        error = "ONVIF discovery cannot start without an active IPv4 address";
        lastError = error;
        nextDiscoveryRetryMs = millis() + ONVIF_DISCOVERY_RETRY_MS;
        return false;
    }

    // A previous failed multicast join must not poison a later retry.
    discoveryUdp.stop();
    if (!discoveryUdp.beginMulticast(ONVIF_DISCOVERY_GROUP, ONVIF_DISCOVERY_PORT)) {
        error = "ONVIF WS-Discovery multicast bind failed";
        lastError = error;
        nextDiscoveryRetryMs = millis() + ONVIF_DISCOVERY_RETRY_MS;
        return false;
    }

    discoveryActive = true;
    nextDiscoveryRetryMs = 0;
    sendHello();
    logWrite(
        "ONVIF discovery started | uuid=" + stableEndpointUuid() +
        " | device=" + onvifDeviceServiceUrl() +
        " | media=" + onvifMediaServiceUrl()
    );
    return true;
}

void onvifLoop()
{
    if (!discoveryActive) {
        if (!streamerModeEnabled() || !cfg_onvif_enabled || !cfg_streamer_rtsp_enabled)
            return;

        const uint32_t now = millis();
        if (nextDiscoveryRetryMs != 0 &&
            (int32_t)(now - nextDiscoveryRetryMs) < 0) {
            return;
        }

        String retryError;
        onvifBegin(retryError);
        return;
    }

    // Process at most two discovery datagrams per main-loop pass so multicast
    // traffic can never monopolize the streamer/video service loop.
    for (uint8_t handled = 0; handled < 2; ++handled) {
        const int packetSize = discoveryUdp.parsePacket();
        if (packetSize <= 0)
            break;

        const IPAddress remoteIp = discoveryUdp.remoteIP();
        const uint16_t remotePort = discoveryUdp.remotePort();
        char buffer[ONVIF_DISCOVERY_RX_MAX + 1];
        const int toRead = packetSize > (int)ONVIF_DISCOVERY_RX_MAX
            ? (int)ONVIF_DISCOVERY_RX_MAX
            : packetSize;
        const int readBytes = discoveryUdp.read((uint8_t *)buffer, toRead);
        if (readBytes <= 0)
            continue;
        buffer[readBytes] = '\0';

        // Drain an oversized datagram. We intentionally do not parse a
        // truncated discovery request.
        if (packetSize > (int)ONVIF_DISCOVERY_RX_MAX) {
            while (discoveryUdp.available() > 0)
                discoveryUdp.read();
            continue;
        }

        processDiscoveryPacket(String(buffer), remoteIp, remotePort);
    }
}

void onvifStop()
{
    if (!discoveryActive)
        return;

    sendBye();
    discoveryUdp.stop();
    discoveryActive = false;
    nextDiscoveryRetryMs = 0;
    logWrite("ONVIF discovery stopped");
}

bool onvifActive()
{
    return discoveryActive;
}

String onvifLastError()
{
    return lastError;
}

uint32_t onvifDiscoveryProbeCount()
{
    return discoveryProbeRx;
}

uint32_t onvifDiscoveryMatchCount()
{
    return discoveryProbeMatchTx;
}

String onvifDiscoveryLastRemote()
{
    if (!ipIsUsable(discoveryLastRemoteIp))
        return "";
    return discoveryLastRemoteIp.toString() + ":" + String(discoveryLastRemotePort);
}

String onvifEndpointUuid()
{
    return stableEndpointUuid();
}

String onvifDeviceServiceUrl()
{
    return httpBaseUrl() + "/onvif/device_service";
}

String onvifMediaServiceUrl()
{
    return httpBaseUrl() + "/onvif/media_service";
}
