#include "web_csrf.h"

#include <esp_system.h>
#include <string.h>

namespace {

static const char *CSRF_ARG = "_csrf";
static const char *CSRF_COOKIE = "sf_csrf";
static const size_t CSRF_RANDOM_BYTES = 16;

static String adminToken;
static String streamToken;

static String randomToken()
{
    uint8_t randomBytes[CSRF_RANDOM_BYTES];
    esp_fill_random(randomBytes, sizeof(randomBytes));

    static const char HEX_DIGITS[] = "0123456789abcdef";
    char encoded[CSRF_RANDOM_BYTES * 2 + 1];

    for (size_t i = 0; i < sizeof(randomBytes); ++i) {
        encoded[i * 2] = HEX_DIGITS[(randomBytes[i] >> 4) & 0x0F];
        encoded[i * 2 + 1] = HEX_DIGITS[randomBytes[i] & 0x0F];
    }

    encoded[sizeof(encoded) - 1] = '\0';
    memset(randomBytes, 0, sizeof(randomBytes));

    return String(encoded);
}

static const String &tokenForRole(SensorForgeAccessRole role)
{
    if (role == SENSORFORGE_ACCESS_STREAM)
        return streamToken;

    return adminToken;
}

static bool constantTimeEquals(const String &left, const String &right)
{
    if (left.length() != right.length())
        return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < left.length(); ++i) {
        diff |= (uint8_t)(left[i] ^ right[i]);
    }

    return diff == 0;
}

static String jsQuotedToken(const String &token)
{
    // Tokens are generated as lowercase hexadecimal only. Keeping this helper
    // explicit documents that no arbitrary user-controlled string is inserted
    // into the JavaScript literal below.
    return token;
}

} // namespace

void webCsrfBegin()
{
    if (!adminToken.length())
        adminToken = randomToken();

    if (!streamToken.length())
        streamToken = randomToken();

    // Extremely defensive: independent 128-bit random values should never
    // collide, but do not intentionally share a role token if they somehow do.
    if (streamToken == adminToken)
        streamToken = randomToken();
}

bool webCsrfRequestRequiresProtection(
    HTTPMethod method,
    const String &uri
)
{
    if (method != HTTP_POST)
        return false;

    // The local integration/app API is not a browser-form interface. Requiring
    // a per-boot browser token there would break existing Sync/App clients.
    if (uri == "/api/v1" || uri.startsWith("/api/v1/"))
        return false;

    // ONVIF uses machine-to-machine SOAP POSTs and HTTP Digest auth, not
    // browser forms. Applying the per-boot browser CSRF token would break NVR
    // clients without adding meaningful protection.
    if (uri == "/onvif/device_service" || uri == "/onvif/media_service")
        return false;

    return true;
}

bool webCsrfRequestValid(
    WebServer &server,
    SensorForgeAccessRole role
)
{
    if (role != SENSORFORGE_ACCESS_ADMIN && role != SENSORFORGE_ACCESS_STREAM)
        return false;

    webCsrfBegin();

    if (!server.hasArg(CSRF_ARG))
        return false;

    const String supplied = server.arg(CSRF_ARG);
    const String &expected = tokenForRole(role);

    return
        supplied.length() == expected.length() &&
        constantTimeEquals(supplied, expected);
}

String webCsrfBrowserBootstrap(
    SensorForgeAccessRole role
)
{
    webCsrfBegin();

    const String token =
        jsQuotedToken(tokenForRole(role));

    String html;
    html.reserve(2600);

    html +=
        "<script>"
        "(function(){"
        "if(window.__sensorForgeCsrfReady)return;"
        "window.__sensorForgeCsrfReady=true;"
        "var T='";
    html += token;
    html +=
        "';"
        "function add(u){"
        "try{var x=new URL(String(u),location.href);if(x.origin!==location.origin)return String(u);"
        "x.searchParams.set('_csrf',T);return x.pathname+x.search+x.hash;}catch(e){return String(u);}}"
        "window.sensorForgeCsrfUrl=add;"
        "var of=window.fetch;"
        "if(of)window.fetch=function(input,init){"
        "var m=(init&&init.method)||(input&&input.method)||'GET';"
        "if(String(m).toUpperCase()==='POST'){"
        "if(typeof input==='string'||(typeof URL!=='undefined'&&input instanceof URL)){input=add(input);}"
        "else if(typeof Request!=='undefined'&&input instanceof Request){try{input=new Request(add(input.url),input);}catch(e){}}"
        "}return of.call(this,input,init);};"
        "function protectForm(f){"
        "if(!f||String(f.method||'GET').toUpperCase()!=='POST')return;"
        "try{var a=f.getAttribute('action')||location.href;f.setAttribute('action',add(a));}catch(e){}}"
        "document.addEventListener('submit',function(e){protectForm(e.target);},true);"
        "if(typeof HTMLFormElement!=='undefined'&&HTMLFormElement.prototype.submit){"
        "var os=HTMLFormElement.prototype.submit;HTMLFormElement.prototype.submit=function(){protectForm(this);return os.call(this);};}"
        "function scan(){try{document.querySelectorAll('form').forEach(protectForm);}catch(e){}}"
        "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',scan);else scan();"
        "})();"
        "</script>";

    return html;
}

String webCsrfCookieHeader(
    SensorForgeAccessRole role
)
{
    webCsrfBegin();

    String value = CSRF_COOKIE;
    value += "=";
    value += tokenForRole(role);
    value += "; Path=/; SameSite=Strict";
    return value;
}
