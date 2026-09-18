#include "webconfig.h"
#include "config.h"
#include "board_config.h"

#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_camera.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <vector>
#include <algorithm>
#include <time.h>
#include <math.h>
#include "logger.h"
#include "webplayer.h"
#include "recorder.h"
#include "storage_guard.h"
#include "radar.h"
#include "branding.h"
#include "rtc.h"
#include "thermal.h"


// High-level recording state from the main firmware loop.
// This is the same state used by the periodic STATUS line.
extern bool recording;

// Gracefully finalizes an active recording when the operator explicitly
// pauses the recording automation from WebConfig.
extern void stopRecording();


// Raw-sector formatting support differs between Arduino-ESP32 releases.
// SPI SD has exposed readRAW/writeRAW for many core generations; SD_MMC
// gained the same public API in the 3.1.x line. Older SD_MMC cores keep
// Wipe available but show Format/Secure Erase as unsupported at runtime.
#if defined(STORAGE_SPI)
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 1
#elif defined(STORAGE_SDMMC) && \
      ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 1, 1)
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 1
#else
#define SENSORFORGE_SD_RAW_FORMAT_SUPPORTED 0
#endif


static WebServer server(80);
static bool webActive = false;

// HTTP handlers persist in the WebServer object after server.stop().
// Register them only once so repeated magnet WiFi toggles do not
// append duplicate handlers and slowly consume heap.
static bool webRoutesRegistered = false;

// Last real HTTP/browser activity. All requests pass through the
// WebServer middleware below. Open WebConfig pages also send a
// lightweight heartbeat every 10 seconds, including background tabs,
// so an open browser session is not mistaken for inactivity.
static uint32_t webLastActivityMs = 0;

// Reboot is scheduled instead of executed directly inside the HTTP handler.
// This gives the browser enough time to follow the POST/Redirect/GET flow and
// render the reboot notice before the network connection disappears.
static bool rebootScheduled = false;
static uint32_t rebootAtMs = 0;

static void noteWebActivity()
{
    webLastActivityMs =
        millis();
}

static void serviceWebLongOperation()
{
    // WebServer handlers are synchronous in loopTask.
    // Long SD work must feed the task watchdog itself.
    esp_task_wdt_reset();
    yield();
}

// Random ID that stays constant for one board boot.
// The recordings page uses it to distinguish browser navigation
// within the same boot from an actual ESP32 reboot/reset.
static uint32_t webBootSessionId = 0;

// PIR simulation state.
// The duration is intentionally RAM-only and is not written to config.txt.
static uint32_t simulatedMotionUntilMs = 0;
static uint32_t simulationDurationSeconds = 5;

// Temporary operator-controlled recording pause. This is deliberately RAM-only:
// it must never survive a reboot and is never written to config.txt. While paused,
// the physical sensors may still be read for diagnostics, but the main firmware
// suppresses operational motion detection and NEW recording starts.
//
// The pause is a short lease rather than a sticky switch. Every visible WebConfig
// page renews the lease. Closing/hiding the UI therefore re-enables recording
// automatically after a short grace period, so the system cannot be forgotten in
// maintenance mode.
static bool recordingAutomationPaused = false;
static uint32_t recordingPauseLeaseMs = 0;
static const uint32_t RECORDING_PAUSE_LEASE_TIMEOUT_MS = 35000UL;

static void setRecordingAutomationPaused(
    bool paused,
    const char *reason
)
{
    if (recordingAutomationPaused == paused) {
        if (paused)
            recordingPauseLeaseMs = millis();
        return;
    }

    recordingAutomationPaused = paused;

    if (paused) {
        recordingPauseLeaseMs = millis();
        simulatedMotionUntilMs = 0;

        consoleWrite(
            "REC",
            "Automation paused | WebConfig"
        );

        logWrite(
            "Recording automation paused from WebConfig"
        );

        // Finish the current file cleanly. Once the flag above is set, the
        // next main-loop iteration cannot immediately start a replacement.
        if (recording)
            stopRecording();

    } else {
        recordingPauseLeaseMs = 0;
        simulatedMotionUntilMs = 0;

        String message =
            "Automation resumed";

        if (reason && reason[0]) {
            message +=
                " | " +
                String(reason);
        }

        consoleWrite(
            "REC",
            message
        );

        logWrite(
            "Recording " +
            message
        );
    }
}


bool webConfigRecordingPaused()
{
    return
        recordingAutomationPaused;
}


static void renewRecordingPauseLease()
{
    if (recordingAutomationPaused)
        recordingPauseLeaseMs = millis();
}


static void releaseRecordingPauseIfLeaseExpired()
{
    if (!recordingAutomationPaused)
        return;

    if (!webActive) {
        setRecordingAutomationPaused(
            false,
            "WebConfig stopped"
        );
        return;
    }

    if (
        recordingPauseLeaseMs != 0 &&
        (uint32_t)(
            millis() -
            recordingPauseLeaseMs
        ) >= RECORDING_PAUSE_LEASE_TIMEOUT_MS
    ) {
        setRecordingAutomationPaused(
            false,
            "WebConfig inactive"
        );
    }
}

// Live camera preview gate. Snapshot requests arrive every ~200 ms while the
// preview is visible. The short timeout is a safety net for closed tabs, lost
// WiFi, browser crashes or navigation where the explicit stop request is lost.
static bool cameraPreviewActive = false;
static uint32_t cameraPreviewLastActivityMs = 0;
static const uint32_t CAMERA_PREVIEW_TIMEOUT_MS = 2500UL;


bool webConfigCameraPreviewActive()
{
    if (!cameraPreviewActive)
        return false;

    if (
        (uint32_t)(
            millis() -
            cameraPreviewLastActivityMs
        ) > CAMERA_PREVIEW_TIMEOUT_MS
    ) {
        cameraPreviewActive = false;
        cameraPreviewLastActivityMs = 0;
        return false;
    }

    return true;
}


static void noteCameraPreviewActivity()
{
    cameraPreviewActive = true;
    cameraPreviewLastActivityMs = millis();
}


static void stopCameraPreview()
{
    cameraPreviewActive = false;
    cameraPreviewLastActivityMs = 0;
}


bool webConfigMotionActive()
{
    if (simulatedMotionUntilMs == 0)
        return false;

    int32_t remaining =
        (int32_t)(simulatedMotionUntilMs - millis());

    if (remaining <= 0) {
        simulatedMotionUntilMs = 0;
        return false;
    }

    return true;
}


uint32_t webConfigMotionRemainingMs()
{
    if (!webConfigMotionActive())
        return 0;

    return
        (uint32_t)(
            simulatedMotionUntilMs - millis()
        );
}


// -------------------------------------------------------------
// HTML / URL Helpers
// -------------------------------------------------------------

static String htmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);

    for (size_t i = 0; i < value.length(); ++i) {
        char c = value[i];

        switch (c) {
            case '&':  out += F("&amp;");  break;
            case '<':  out += F("&lt;");   break;
            case '>':  out += F("&gt;");   break;
            case '"':  out += F("&quot;"); break;
            case '\'': out += F("&#39;");  break;
            default:   out += c;            break;
        }
    }

    return out;
}


static String urlEncode(const String &value)
{
    static const char hex[] = "0123456789ABCDEF";

    String out;
    out.reserve(value.length() * 2);

    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t c = (uint8_t)value[i];

        if ((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {

            out += (char)c;

        } else {

            out += '%';
            out += hex[(c >> 4) & 0x0F];
            out += hex[c & 0x0F];
        }
    }

    return out;
}


static String moduleClockText()
{
    time_t now =
        time(nullptr);

    if (now < (time_t)1609459200)
        return String();

    struct tm localTime;

    if (!localtime_r(
            &now,
            &localTime
        )) {
        return String();
    }

    char buffer[32];

    if (!strftime(
            buffer,
            sizeof(buffer),
            "%d.%m.%Y %H:%M:%S",
            &localTime
        )) {
        return String();
    }

    return String(buffer);
}


static String htmlHeader()
{
    return
        "<!doctype html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>" SENSORFORGE_APP_NAME_LITERAL " &middot; "
        SENSORFORGE_PLATFORM_LITERAL " &middot; v"
        SENSORFORGE_CORE_VERSION_LITERAL "</title>"
        "<style>"
        ":root{--bg:#eef1f4;--panel:#fff;--text:#1f2933;--muted:#667085;"
        "--line:#d8dee6;--nav:#1f2937;--nav2:#344054;--accent:#2563eb;"
        "--ok:#087a00;--warn:#9a5a00;--danger:#b42318;}"
        "*{box-sizing:border-box;}"
        "body{font-family:Arial,sans-serif;margin:0;background:var(--bg);color:var(--text);}"
        "h1,h2,h3{margin-top:0;color:#27313d;}"
        "h2{font-size:1.55rem;margin-bottom:10px;}"
        "h3{font-size:1.08rem;}"
        "a{color:var(--accent);}"
        ".topbar{position:sticky;top:0;z-index:1000;background:var(--nav);"
        "box-shadow:0 2px 8px rgba(0,0,0,.18);}"
        ".navwrap{max-width:1200px;margin:0 auto;display:flex;align-items:center;"
        "gap:8px;padding:0 14px;min-height:54px;}"
        ".brand{color:#fff;text-decoration:none;font-weight:700;"
        "padding:8px 10px 8px 0;margin-right:8px;white-space:nowrap;"
        "display:flex;flex-direction:column;line-height:1.05;}"
        ".brand-main{font-size:1.02rem;letter-spacing:.08em;text-transform:uppercase;}"
        ".brand-sub{font-size:.61rem;font-weight:600;color:#aeb8c5;"
        "letter-spacing:.08em;margin-top:4px;text-transform:uppercase;}"
        ".navlinks{display:flex;align-items:center;gap:2px;flex-wrap:wrap;}"
        ".module-meta{margin-left:auto;display:flex;flex-direction:column;align-items:flex-end;"
        "justify-content:center;gap:3px;padding:4px 0 4px 10px;}"
        ".module-clock{color:#f8fafc;font-size:.88rem;font-weight:700;"
        "font-variant-numeric:tabular-nums;white-space:nowrap;text-align:right;letter-spacing:.02em;}"
        ".module-clock.invalid{color:#aeb8c5;font-weight:600;}"
        ".module-clock.thermal-warning{color:#fbbf24;}"
        ".module-clock.thermal-emergency{color:#fca5a5;}"
        ".module-pause-indicator{display:inline-block;padding:3px 8px;border-radius:999px;"
        "background:#f59e0b;color:#241500;font-size:.68rem;font-weight:800;letter-spacing:.05em;"
        "white-space:nowrap;}"
        ".module-pause-indicator[hidden]{display:none!important;}"
        ".navitem,.navdrop>summary{display:block;color:#e5e7eb;text-decoration:none;"
        "padding:10px 11px;border-radius:6px;cursor:pointer;user-select:none;"
        "font-size:.95rem;white-space:nowrap;}"
        ".navitem:hover,.navdrop>summary:hover,.navitem.active,.navdrop>summary.active{"
        "background:var(--nav2);color:#fff;}"
        ".navdrop{position:relative;}"
        ".navdrop>summary{list-style:none;}"
        ".navdrop>summary::-webkit-details-marker{display:none;}"
        ".navdrop>summary:after{content:'  ▾';font-size:.75em;}"
        ".dropdown{position:absolute;left:0;top:calc(100% + 2px);min-width:210px;"
        "background:#fff;border:1px solid var(--line);border-radius:8px;"
        "box-shadow:0 10px 28px rgba(0,0,0,.18);padding:6px;}"
        ".navdrop:not([open]) .dropdown{display:none;}"
        ".dropdown a{display:block;text-decoration:none;color:var(--text);"
        "padding:9px 10px;border-radius:6px;margin:0;}"
        ".dropdown a:hover,.dropdown a.active{background:#eef4ff;color:#174ea6;}"
        ".dropdown .sep{height:1px;background:var(--line);margin:6px 4px;}"
        ".dropdown .danger-link{color:var(--danger);}"
        ".page{max-width:1200px;margin:20px auto;padding:0 14px 30px;}"
        "div.box{background:var(--panel);padding:22px;border-radius:10px;"
        "box-shadow:0 2px 12px rgba(16,24,40,.08);}"
        "input,select{padding:8px 10px;margin:4px 0 8px;width:280px;max-width:100%;"
        "border:1px solid #c7cfd8;border-radius:6px;background:#fff;color:var(--text);}"
        "input:focus,select:focus{outline:2px solid #b9d2ff;border-color:var(--accent);}"
        "button,.button{padding:9px 15px;margin:5px 5px 5px 0;border:0;border-radius:6px;"
        "background:#e9edf2;color:#1f2933;cursor:pointer;font-weight:600;text-decoration:none;"
        "display:inline-block;}"
        "button:hover,.button:hover{filter:brightness(.96);}"
        ".button.primary,button.primary{background:var(--accent);color:#fff;}"
        ".button.danger,button.danger{background:var(--danger);color:#fff;}"
        ".muted{color:var(--muted);}"
        ".page-title{display:flex;justify-content:space-between;gap:14px;align-items:flex-start;"
        "margin-bottom:18px;}"
        ".page-title p{margin:0;color:var(--muted);}"
        ".dashboard-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));"
        "gap:12px;margin:16px 0;}"
        ".dash-card{border:1px solid var(--line);border-radius:9px;padding:15px;background:#fafbfc;}"
        ".card-label{font-size:.82rem;color:var(--muted);text-transform:uppercase;"
        "letter-spacing:.04em;margin-bottom:6px;}"
        ".card-value{font-size:1.15rem;font-weight:700;word-break:break-word;}"
        ".card-note{font-size:.88rem;color:var(--muted);margin-top:5px;}"
        ".status-pill{display:inline-block;padding:5px 9px;border-radius:999px;"
        "font-size:.82rem;font-weight:700;background:#e8edf3;color:#344054;white-space:nowrap;}"
        ".status-pill.ok{background:#e7f6e5;color:var(--ok);}"
        ".status-pill.warn{background:#fff1d6;color:var(--warn);}"
        ".status-pill.danger{background:#fde7e5;color:var(--danger);}"
        ".progress{height:8px;background:#e5e9ef;border-radius:999px;overflow:hidden;margin-top:9px;}"
        ".progress>span{display:block;height:100%;background:var(--accent);}"
        ".quick-actions{display:flex;flex-wrap:wrap;gap:6px;margin:16px 0 6px;}"
        ".settings-section{border:1px solid var(--line);border-radius:9px;padding:16px;"
        "margin:14px 0;background:#fbfcfd;}"
        ".settings-section h3{margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid var(--line);}"
        ".recording-control{display:flex;align-items:center;justify-content:space-between;gap:18px;"
        "border:1px solid var(--line);border-left:5px solid var(--ok);border-radius:9px;"
        "padding:15px 16px;margin:16px 0;background:#fbfcfd;transition:.2s ease;}"
        ".recording-control.paused{border-color:#f0c36a;border-left-color:#d97706;background:#fff7e8;"
        "box-shadow:0 2px 10px rgba(154,90,0,.10);}"
        ".recording-control-copy{min-width:0;}"
        ".recording-control-title{font-weight:800;margin-bottom:5px;}"
        ".recording-control-note{font-size:.9rem;color:var(--muted);line-height:1.45;}"
        ".recording-control-actions{flex:0 0 auto;text-align:right;}"
        ".recording-control-actions button{min-width:220px;margin:0;}"
        ".recording-control.paused .recording-control-actions button{background:#d97706;color:#fff;}"
        ".form-actions{padding:14px 0 2px;}"
        ".form-actions button{background:var(--accent);color:#fff;min-width:120px;}"
        ".flash-notice{position:relative;margin:0 0 16px;padding:14px 16px 14px 18px;"
        "border:1px solid #b8d5bf;border-left:5px solid var(--ok);border-radius:9px;"
        "background:#eef9f0;box-shadow:0 2px 8px rgba(16,24,40,.06);"
        "transition:opacity .35s ease,transform .35s ease;}"
        ".flash-notice.hide{opacity:0;transform:translateY(-6px);}"
        ".flash-notice strong{display:block;margin-bottom:4px;color:#14532d;}"
        ".flash-notice .muted{font-size:.92rem;}"
        ".flash-notice.error{border-color:#efb7b2;border-left-color:var(--danger);background:#fff1f0;}"
        ".flash-notice.error strong{color:#8a1c14;}"
        ".modal-backdrop{position:fixed;inset:0;z-index:2000;display:flex;align-items:center;"
        "justify-content:center;padding:18px;background:rgba(15,23,32,.58);backdrop-filter:blur(2px);}"
        ".modal-backdrop[hidden]{display:none!important;}"
        ".modal-card{width:min(520px,100%);background:#fff;border:1px solid var(--line);border-radius:12px;"
        "box-shadow:0 20px 60px rgba(0,0,0,.28);padding:24px;}"
        ".modal-card h3{margin:12px 0 8px;font-size:1.25rem;}"
        ".modal-card p{margin:8px 0;color:var(--muted);line-height:1.5;}"
        ".modal-actions{display:flex;flex-wrap:wrap;gap:6px;margin-top:18px;}"
        ".modal-state{margin-top:12px;font-size:.88rem;color:var(--muted);}"
        ".operation-card{max-width:620px;margin:26px auto;text-align:center;border:1px solid var(--line);"
        "border-radius:12px;padding:28px 22px;background:#fafbfc;}"
        ".operation-card .countdown{font-size:2.4rem;font-weight:700;margin:14px 0 4px;}"
        ".log-toolbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin:14px 0 10px;}"
        ".log-meta{display:flex;flex-wrap:wrap;gap:8px 16px;margin:0 0 12px;color:var(--muted);font-size:.9rem;}"
        ".log-terminal{height:min(68vh,720px);min-height:360px;overflow:auto;background:#0f1720;"
        "border:1px solid #263445;border-radius:10px;box-shadow:inset 0 1px 0 rgba(255,255,255,.03);}"
        ".log-view{margin:0;padding:16px 18px;color:#d6e2ef;background:transparent;font-family:Consolas,"
        "'Liberation Mono',Menlo,monospace;font-size:.82rem;line-height:1.55;white-space:pre-wrap;"
        "word-break:break-word;tab-size:4;}"
        ".log-status{font-size:.86rem;color:var(--muted);margin-left:auto;}"
        "@media(max-width:760px){.log-terminal{height:62vh;min-height:300px;}"
        ".log-view{padding:12px;font-size:.76rem;}.log-status{width:100%;margin-left:0;}}"
        "table{max-width:100%;}"
        "code{background:#f2f4f7;padding:2px 4px;border-radius:4px;}"
        "@media(max-width:900px){.dashboard-grid{grid-template-columns:repeat(2,minmax(0,1fr));}}"
        "@media(max-width:760px){"
        ".navwrap{align-items:flex-start;flex-direction:column;padding:8px 12px;}"
        ".brand{padding:6px 2px;margin:0;}"
        ".brand-sub{font-size:.58rem;}"
        ".navlinks{width:100%;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px;}"
        ".module-meta{width:100%;margin-left:0;padding:3px 2px 5px;align-items:flex-end;}"
        ".module-clock{width:100%;text-align:right;font-size:.82rem;white-space:normal;line-height:1.25;}"
        ".recording-control{align-items:stretch;flex-direction:column;}"
        ".recording-control-actions{text-align:left;}"
        ".recording-control-actions button{width:100%;min-width:0;}"
        ".navitem,.navdrop>summary{width:100%;padding:9px;}"
        ".navdrop{width:100%;}"
        ".dropdown{position:static;min-width:0;margin-top:3px;box-shadow:none;border-color:#566170;}"
        ".dashboard-grid{grid-template-columns:1fr;}"
        ".page{margin-top:12px;padding-left:8px;padding-right:8px;}"
        "div.box{padding:15px;border-radius:8px;}"
        ".page-title{flex-direction:column;}"
        "}"
        "</style></head><body>"
        "<nav class='topbar'><div class='navwrap'>"
        "<a class='brand' href='/'><span class='brand-main'>"
        SENSORFORGE_APP_NAME_LITERAL
        "</span><span class='brand-sub'>" SENSORFORGE_PLATFORM_LITERAL
        " &middot; CORE v" SENSORFORGE_CORE_VERSION_LITERAL "</span></a>"
        "<div class='navlinks'>"
        "<a class='navitem' data-nav='home' href='/'>Übersicht</a>"
        "<details class='navdrop' id='navRecordings'><summary data-nav='recordings'>Aufnahmen</summary>"
        "<div class='dropdown'><a href='/files'>Aufnahmen verwalten</a></div></details>"
        "<details class='navdrop' id='navCamera'><summary data-nav='camera'>Kamera</summary>"
        "<div class='dropdown'><a href='/preview'>Live Preview</a></div></details>"
        "<details class='navdrop' id='navSensor'><summary data-nav='sensor'>Sensor</summary>"
        "<div class='dropdown'><a href='/radar_config'>Radar Konfiguration</a>"
        "<a href='/#simulation'>Bewegung simulieren</a></div></details>"
        "<details class='navdrop' id='navSystem'><summary data-nav='system'>System</summary>"
        "<div class='dropdown'>"
        "<a href='/sdstatus'>SD Status</a><a href='/log'>Log Viewer</a>"
        "<a href='/sysinfo'>System Info</a><a href='/board'>Board Info</a>"
        "<a href='/psram'>PSRAM Test</a><a href='/sdbench'>SD Benchmark</a>"
        "<div class='sep'></div><a class='danger-link' href='/reboot'>Reboot</a>"
        "</div></details>"
        "<a class='navitem' data-nav='config' href='/config'>Konfiguration</a>"
        "</div>"
        "<div class='module-meta'>"
        "<span id='recordingPauseGlobal' class='module-pause-indicator' hidden>AUFNAHME PAUSIERT</span>"
        "<div id='moduleClock' class='module-clock invalid' title='Aktuelle Systemzeit des Moduls'>"
        "Zeit --.--.---- &middot; --:--:--</div>"
        "</div>"
        "</div></nav>"
        // Central browser-session keepalive. Every normal SensorForge page
        // uses this common header/menu, so an open web UI keeps WiFi alive
        // even if the page itself is otherwise idle. All real HTTP requests
        // are additionally tracked by the WebServer middleware.
        "<script>"
        "(function(){"
        "if(window.__sensorForgeKeepalive)return;"
        "window.__sensorForgeKeepalive=true;"
        "var lastPing=0;"
        "function ping(force){"
        "var now=Date.now();"
        "if(!force&&now-lastPing<5000)return;"
        "lastPing=now;"
        "fetch('/activity?t='+now,{cache:'no-store',credentials:'same-origin',keepalive:true})"
        ".catch(function(){});"
        "}"
        "window.sensorForgeKeepalive=ping;"
        "ping(true);"
        "setInterval(function(){ping(true);},10000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)ping(true);});"
        "window.addEventListener('focus',function(){ping(true);});"
        "window.addEventListener('pageshow',function(){ping(true);});"
        "document.addEventListener('pointerdown',function(){ping(false);},{passive:true});"
        "document.addEventListener('keydown',function(){ping(false);});"
        "})();"
        "</script>"
        // Live module clock. The server supplies its own local wall-clock
        // value; the browser only advances it between lightweight resyncs,
        // so the display does not depend on the browser timezone.
        "<script>"
        "(function(){"
        "var el=document.getElementById('moduleClock');"
        "var pauseEl=document.getElementById('recordingPauseGlobal');"
        "if(!el)return;"
        "var baseMs=0,syncMs=0,pauseActive=false,cpuText='',rtcText='',thermalState='OK';"
        "function pad(v){return String(v).padStart(2,'0');}"
        "function tempSuffix(){"
        "var x='';"
        "if(cpuText)x+=' · CPU '+cpuText+' °C';"
        "if(rtcText)x+=' · RTC '+rtcText+' °C';"
        "return x;"
        "}"
        "function render(){"
        "var suffix=tempSuffix();"
        "if(!baseMs){el.textContent='Zeit --.--.---- · --:--:--'+suffix;el.classList.add('invalid');return;}"
        "var d=new Date(baseMs+(Date.now()-syncMs));"
        "el.textContent='Zeit '+pad(d.getUTCDate())+'.'+pad(d.getUTCMonth()+1)+'.'+d.getUTCFullYear()+"
        "' · '+pad(d.getUTCHours())+':'+pad(d.getUTCMinutes())+':'+pad(d.getUTCSeconds())+suffix;"
        "el.classList.remove('invalid');"
        "}"
        "function renewPauseLease(){"
        "if(!pauseActive||document.hidden)return;"
        "fetch('/recording_pause_keepalive?t='+Date.now(),{method:'POST',cache:'no-store',"
        "credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "function apply(s){"
        "pauseActive=!!(s&&s.recording_paused);"
        "if(pauseEl)pauseEl.hidden=!pauseActive;"
        "if(pauseActive)renewPauseLease();"
        "cpuText=(s&&s.cpu_temp_valid)?Number(s.cpu_temp_c).toFixed(1):'';"
        "rtcText=(s&&s.rtc_temp_valid)?Number(s.rtc_temp_c).toFixed(1):'';"
        "thermalState=(s&&s.thermal_state)||'OK';"
        "el.classList.toggle('thermal-warning',thermalState==='WARNING');"
        "el.classList.toggle('thermal-emergency',thermalState==='EMERGENCY');"
        "if(s){el.title='Temperaturschutz: CPU Warnung ab '+Number(s.thermal_warning_c).toFixed(0)+"
        "' °C, Notprogramm ab '+Number(s.thermal_emergency_c).toFixed(0)+' °C, Wiederanlauf <'+"
        "Number(s.thermal_recovery_c).toFixed(0)+' °C; RTC/Gehäuseindikator Warnung ab '+"
        "Number(s.thermal_rtc_warning_c).toFixed(0)+' °C, Notprogramm ab '+Number(s.thermal_rtc_emergency_c).toFixed(0)+"
        "' °C, Wiederanlauf <'+Number(s.thermal_rtc_recovery_c).toFixed(0)+' °C';}"
        "if(!s||!s.clock_valid||!s.clock){baseMs=0;render();return;}"
        "var m=/^(\\d{2})\\.(\\d{2})\\.(\\d{4}) (\\d{2}):(\\d{2}):(\\d{2})$/.exec(s.clock);"
        "if(!m){baseMs=0;render();return;}"
        "baseMs=Date.UTC(+m[3],+m[2]-1,+m[1],+m[4],+m[5],+m[6]);"
        "syncMs=Date.now();render();"
        "}"
        "function sync(){fetch('/ui_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
        ".then(function(r){if(!r.ok)throw new Error();return r.json();}).then(apply).catch(function(){});}"
        "sync();"
        "setInterval(render,1000);"
        "setInterval(sync,10000);"
        "setInterval(renewPauseLease,10000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)sync();});"
        "window.addEventListener('focus',sync);"
        "})();"
        "</script>"
        "<main class='page'><div class='box'>";
}


static String htmlFooter()
{
    return
        "<script>"
        "(function(){"
        "var p=location.pathname;"
        "var group='';"
        "if(p==='/')group='home';"
        "else if(p==='/config'||p==='/save')group='config';"
        "else if(p.indexOf('/files')===0||p==='/file'||p==='/play')group='recordings';"
        "else if(p==='/preview'||p==='/snapshot')group='camera';"
        "else if(p.indexOf('/radar_')===0)group='sensor';"
        "else group='system';"
        "var active=document.querySelector('[data-nav=\"'+group+'\"]');"
        "if(active)active.classList.add('active');"
        "document.addEventListener('click',function(e){"
        "var open=document.querySelectorAll('.navdrop[open]');"
        "for(var i=0;i<open.length;i++){if(!open[i].contains(e.target))open[i].removeAttribute('open');}"
        "});"
        "})();"
        "</script>"
        "</div></main></body></html>";
}


// -------------------------------------------------------------
// Recording priority
// -------------------------------------------------------------

static bool rejectWhileRecording(
    const char *operation
)
{
    if (!recorderIsOpen())
        return false;

    server.send(
        409,
        "text/plain; charset=utf-8",
        "Recording active - " +
        String(operation) +
        " is temporarily unavailable."
    );

    return true;
}


// Lightweight dynamic UI state. The dashboard uses the high-level
// `recording` flag (same source as the serial STATUS line), while
// `recorder_open` remains available for SD/file-operation safety.
static void handleUiStatus()
{
    String clockText =
        moduleClockText();

    bool clockValid =
        clockText.length() > 0;

    float cpuTempC =
        thermalCpuTemperatureC();

    bool cpuTempValid =
        isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    String json =
        String("{\"recording\":") +
        (recording ? "true" : "false") +
        ",\"recording_paused\":" +
        (recordingAutomationPaused ? "true" : "false") +
        ",\"recorder_open\":" +
        (recorderIsOpen() ? "true" : "false") +
        ",\"clock_valid\":" +
        (clockValid ? "true" : "false") +
        ",\"clock\":\"" +
        clockText +
        "\",\"cpu_temp_valid\":" +
        (cpuTempValid ? "true" : "false") +
        ",\"cpu_temp_c\":" +
        String(cpuTempValid ? cpuTempC : 0.0f, 1) +
        ",\"rtc_temp_valid\":" +
        (rtcTempValid ? "true" : "false") +
        ",\"rtc_temp_c\":" +
        String(rtcTempValid ? rtcTempC : 0.0f, 1) +
        ",\"thermal_state\":\"" +
        String(thermalStateName()) +
        "\",\"thermal_source\":\"" +
        String(thermalSourceName()) +
        "\",\"thermal_warning_c\":" +
        String(SENSORFORGE_THERMAL_WARNING_C, 1) +
        ",\"thermal_emergency_c\":" +
        String(SENSORFORGE_THERMAL_EMERGENCY_C, 1) +
        ",\"thermal_recovery_c\":" +
        String(SENSORFORGE_THERMAL_RECOVERY_C, 1) +
        ",\"thermal_rtc_warning_c\":" +
        String(SENSORFORGE_THERMAL_RTC_WARNING_C, 1) +
        ",\"thermal_rtc_emergency_c\":" +
        String(SENSORFORGE_THERMAL_RTC_EMERGENCY_C, 1) +
        ",\"thermal_rtc_recovery_c\":" +
        String(SENSORFORGE_THERMAL_RTC_RECOVERY_C, 1) +
        "}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static void handleRecordingPause()
{
    String action =
        server.arg("action");

    action.trim();
    action.toLowerCase();

    if (action == "pause") {
        setRecordingAutomationPaused(
            true,
            "operator"
        );

    } else if (
        action == "resume"
    ) {
        setRecordingAutomationPaused(
            false,
            "operator"
        );

    } else {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid recording pause action"
        );
        return;
    }

    handleUiStatus();
}


static void handleRecordingPauseKeepalive()
{
    renewRecordingPauseLease();

    server.send(
        204,
        "text/plain",
        ""
    );
}


static String recordingBrowserModalHtml(
    bool visible
)
{
    String html =
        "<div id='recordingBlockedModal' class='modal-backdrop'";

    if (!visible)
        html += " hidden";

    html +=
        ">"
        "<div class='modal-card' role='dialog' aria-modal='true' "
        "aria-labelledby='recordingBlockedTitle'>"
        "<span class='status-pill danger'>AUFNAHME AKTIV</span>"
        "<h3 id='recordingBlockedTitle'>Aufnahmen momentan nicht verfügbar</h3>"
        "<p>Während einer laufenden Aufnahme wird der Verzeichnisbaum nicht gelesen. "
        "Das vermeidet zusätzliche SD-I/O und schützt die aktive Aufzeichnung.</p>"
        "<p>Die Ansicht wird automatisch wieder freigegeben, sobald der Recorder fertig ist.</p>"
        "<div class='modal-actions'>"
        "<button type='button' class='primary' onclick='location.reload()'>Erneut versuchen</button>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "</div>"
        "<div id='recordingBlockedState' class='modal-state'>Warte auf Aufnahmeende ...</div>"
        "</div></div>";

    return html;
}


// -------------------------------------------------------------
// CONFIG COPY CONSISTENCY
// -------------------------------------------------------------

enum ConfigCopyCompareResult : uint8_t {
    CONFIG_COPY_COMPARE_UNAVAILABLE = 0,
    CONFIG_COPY_COMPARE_IDENTICAL,
    CONFIG_COPY_COMPARE_DIFFERENT,
    CONFIG_COPY_COMPARE_READ_ERROR
};


static ConfigCopyCompareResult compareConfigCopies()
{
    if (
        !configSdAvailable() ||
        !configSdPresent() ||
        !configInternalAvailable()
    ) {
        return
            CONFIG_COPY_COMPARE_UNAVAILABLE;
    }


    if (
        !STORAGE.exists(
            "/config.txt"
        ) ||
        !LittleFS.exists(
            "/config.txt"
        )
    ) {
        return
            CONFIG_COPY_COMPARE_UNAVAILABLE;
    }


    File sdFile =
        STORAGE.open(
            "/config.txt",
            FILE_READ
        );

    File internalFile =
        LittleFS.open(
            "/config.txt",
            FILE_READ
        );


    if (
        !sdFile ||
        !internalFile
    ) {

        if (sdFile)
            sdFile.close();

        if (internalFile)
            internalFile.close();

        return
            CONFIG_COPY_COMPARE_READ_ERROR;
    }


    if (
        sdFile.size() !=
        internalFile.size()
    ) {

        sdFile.close();
        internalFile.close();

        return
            CONFIG_COPY_COMPARE_DIFFERENT;
    }


    uint8_t sdBuffer[256];
    uint8_t internalBuffer[256];


    while (
        sdFile.available() ||
        internalFile.available()
    ) {

        size_t sdRead =
            sdFile.read(
                sdBuffer,
                sizeof(sdBuffer)
            );

        size_t internalRead =
            internalFile.read(
                internalBuffer,
                sizeof(internalBuffer)
            );


        if (
            sdRead !=
            internalRead
        ) {

            sdFile.close();
            internalFile.close();

            return
                CONFIG_COPY_COMPARE_DIFFERENT;
        }


        if (
            sdRead > 0 &&
            memcmp(
                sdBuffer,
                internalBuffer,
                sdRead
            ) != 0
        ) {

            sdFile.close();
            internalFile.close();

            return
                CONFIG_COPY_COMPARE_DIFFERENT;
        }


        serviceWebLongOperation();
    }


    sdFile.close();
    internalFile.close();


    return
        CONFIG_COPY_COMPARE_IDENTICAL;
}


// -------------------------------------------------------------
// ROOT PAGE
// -------------------------------------------------------------

static void handleRoot()
{
    // Keep SD/config state fresh for the dashboard as well.
    configRefreshSdStatus();

    bool recordingActive =
        recording;

    bool recordingPaused =
        recordingAutomationPaused;

    bool simulatedMotion =
        webConfigMotionActive();

    bool radarMotion =
        radarMotionActive();

    bool ot2Active =
        digitalRead(PIR_PIN) == HIGH;

    bool motionActive =
        simulatedMotion ||
        radarMotion ||
        ot2Active;

    float cpuTempC =
        thermalCpuTemperatureC();

    bool cpuTempValid =
        isfinite(cpuTempC);

    float rtcTempC = 0.0f;
    bool rtcTempValid =
        thermalRtcTemperatureC(
            rtcTempC
        );

    String thermalUiState =
        String(thermalStateName());

    if (
        strcmp(
            thermalSourceName(),
            "NONE"
        ) != 0
    ) {
        thermalUiState +=
            " (" +
            String(thermalSourceName()) +
            ")";
    }

    uint64_t totalBytes =
        STORAGE.totalBytes();

    uint64_t usedBytes =
        STORAGE.usedBytes();

    uint64_t freeBytes =
        totalBytes > usedBytes
        ? totalBytes - usedBytes
        : 0;

    uint32_t usedPercent =
        totalBytes > 0
        ? (uint32_t)(
            (usedBytes * 100ULL) /
            totalBytes
        )
        : 0;

    if (usedPercent > 100)
        usedPercent = 100;

    String networkText;

    if (WiFi.status() == WL_CONNECTED) {
        networkText =
            "STA " +
            WiFi.localIP().toString();
    } else {
        networkText =
            "Hotspot " +
            WiFi.softAPIP().toString();
    }

    String html =
        htmlHeader();

    String notice =
        server.arg("notice");

    bool knownNotice =
        notice == "config_saved_both" ||
        notice == "config_saved_internal" ||
        notice == "sd_wipe_done" ||
        notice == "sd_wipe_failed" ||
        notice == "sd_format_done" ||
        notice == "sd_format_failed" ||
        notice == "sd_format_unsupported" ||
        notice == "sd_secure_done" ||
        notice == "sd_secure_failed" ||
        notice == "sd_secure_unsupported" ||
        notice == "sd_recording_active" ||
        notice == "sd_storage_locked" ||
        notice == "sd_restore_source_failed" ||
        notice == "sd_restore_failed";

    if (knownNotice) {

        bool noticeIsError =
            notice == "sd_wipe_failed" ||
            notice == "sd_format_failed" ||
            notice == "sd_format_unsupported" ||
            notice == "sd_secure_failed" ||
            notice == "sd_secure_unsupported" ||
            notice == "sd_recording_active" ||
            notice == "sd_storage_locked" ||
            notice == "sd_restore_source_failed" ||
            notice == "sd_restore_failed";

        html +=
            "<div id='flashNotice' class='flash-notice";

        if (noticeIsError)
            html += " error";

        html += "'>";

        if (notice == "config_saved_both") {
            html +=
                "<strong>Konfiguration gespeichert</strong>"
                "<span class='muted'>Interner Shadow und SD-Karte wurden aktualisiert. "
                "Sleep-Einstellungen sind sofort aktiv; andere geänderte Einstellungen "
                "werden nach einem Neustart vollständig übernommen.</span>";

        } else if (notice == "config_saved_internal") {
            html +=
                "<strong>Konfiguration gespeichert</strong>"
                "<span class='muted'>Der interne Config-Shadow wurde aktualisiert. "
                "Die SD-Karte wurde nicht beschrieben. Sleep-Einstellungen sind sofort aktiv; "
                "andere geänderte Einstellungen werden nach einem Neustart vollständig übernommen.</span>";

        } else if (notice == "sd_wipe_done") {
            html +=
                "<strong>SD Wipe abgeschlossen</strong>"
                "<span class='muted'>Der SD-Inhalt wurde gelöscht. "
                "<code>/config.txt</code> wurde anschließend aus dem internen Flash-Shadow neu auf die SD-Karte kopiert.</span>";

        } else if (notice == "sd_wipe_failed") {
            html +=
                "<strong>SD Wipe nicht vollständig</strong>"
                "<span class='muted'>Mindestens ein SD-Löschvorgang ist fehlgeschlagen. "
                "Die interne Konfiguration wurde anschließend trotzdem wieder auf die SD-Karte geschrieben. "
                "Bitte SD-Status prüfen.</span>";

        } else if (notice == "sd_format_done") {
            html +=
                "<strong>SD Format abgeschlossen</strong>"
                "<span class='muted'>Das FAT-Dateisystem wurde neu erzeugt. "
                "<code>/config.txt</code> wurde aus dem internen Flash-Shadow wiederhergestellt.</span>";

        } else if (notice == "sd_format_failed") {
            html +=
                "<strong>SD Format fehlgeschlagen</strong>"
                "<span class='muted'>Die Formatierung konnte nicht sauber abgeschlossen werden. "
                "Soweit die SD-Karte noch erreichbar war, wurde die interne Konfiguration wiederhergestellt. "
                "Bitte SD-Status prüfen.</span>";

        } else if (notice == "sd_format_unsupported") {
            html +=
                "<strong>SD Format nicht unterstützt</strong>"
                "<span class='muted'>Der aktive Storage-Backend/Core stellt die für SensorForge benötigten "
                "RAW-/Remount-Funktionen nicht bereit. Es wurden keine Daten verändert.</span>";

        } else if (notice == "sd_secure_done") {
            html +=
                "<strong>Secure Erase abgeschlossen</strong>"
                "<span class='muted'>Der adressierbare freie Datenbereich wurde mit Nullen überschrieben, "
                "anschließend wurde FAT neu erzeugt und <code>/config.txt</code> aus dem internen Flash-Shadow wiederhergestellt.</span>";

        } else if (notice == "sd_secure_failed") {
            html +=
                "<strong>Secure Erase nicht vollständig</strong>"
                "<span class='muted'>Überschreiben oder Neuformatierung konnte nicht vollständig abgeschlossen werden. "
                "Soweit möglich wurde die interne Konfiguration wieder auf die SD-Karte kopiert. "
                "Bitte SD-Status prüfen.</span>";

        } else if (notice == "sd_secure_unsupported") {
            html +=
                "<strong>Secure Erase nicht unterstützt</strong>"
                "<span class='muted'>Diese Board-/Storage-Kombination unterstützt den vorgesehenen sicheren Ablauf nicht. "
                "Es wurden keine Daten verändert.</span>";

        } else if (notice == "sd_recording_active") {
            html +=
                "<strong>SD-Wartung nicht gestartet</strong>"
                "<span class='muted'>Eine Aufnahme läuft gerade. Wipe, Format und Secure Erase "
                "werden während einer laufenden Aufnahme grundsätzlich nicht gestartet. "
                "Bitte nach Ende der Aufnahme erneut ausführen.</span>";

        } else if (notice == "sd_storage_locked") {
            html +=
                "<strong>SD-Karte ist momentan gesperrt</strong>"
                "<span class='muted'>Eine andere Systemoperation hat den globalen Storage-Lock gesetzt. "
                "Die SD-Wartung wurde nicht gestartet und es wurden keine Daten verändert.</span>";

        } else if (notice == "sd_restore_source_failed") {
            html +=
                "<strong>SD-Wartung abgebrochen</strong>"
                "<span class='muted'>Der interne Flash-Shadow <code>/config.txt</code> fehlt oder ist ungültig. "
                "Aus Sicherheitsgründen wurde die SD-Karte nicht verändert.</span>";

        } else {
            html +=
                "<strong>Konfiguration konnte nicht auf SD wiederhergestellt werden</strong>"
                "<span class='muted'>Die SD-Wartung wurde ausgeführt, aber das Rückkopieren von "
                "<code>/config.txt</code> aus dem internen Flash ist fehlgeschlagen. "
                "Die laufende RAM-Konfiguration bleibt aktiv; vor einem Neustart bitte SD-Status prüfen.</span>";
        }

        html +=
            "</div>"
            "<script>"
            "history.replaceState(null,'','/');"
            "setTimeout(function(){"
                "var n=document.getElementById('flashNotice');"
                "if(!n)return;"
                "n.classList.add('hide');"
                "setTimeout(function(){if(n.parentNode)n.parentNode.removeChild(n);},400);"
            "},6200);"
            "</script>";
    }

    html +=
        "<div class='page-title'><div>"
        "<h2>Übersicht</h2>"
        "<p>" +
        htmlEscape(cfg_hostname) +
        " &middot; " SENSORFORGE_PLATFORM_LITERAL
        " &middot; Core v" SENSORFORGE_CORE_VERSION_LITERAL "</p>"
        "</div>";

    if (recordingActive) {
        html +=
            "<span id='recordingStatusPill' class='status-pill danger'>AUFNAHME LÄUFT</span>";

    } else if (recordingPaused) {
        html +=
            "<span id='recordingStatusPill' class='status-pill warn'>AUFNAHME PAUSIERT</span>";

    } else {
        html +=
            "<span id='recordingStatusPill' class='status-pill ok'>Bereit</span>";
    }

    html +=
        "</div>";

    html +=
        "<div class='dashboard-grid'>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Aufnahme</div>"
        "<div id='recordingCardValue' class='card-value'>" +
        String(
            recordingActive
            ? "Aktiv"
            : (
                recordingPaused
                ? "Pausiert"
                : "Bereit"
            )
        ) +
        "</div>"
        "<div class='card-note'>Format: " +
        htmlEscape(cfg_recording_format) +
        " &middot; " +
        String(cfg_resolution) +
        " @ " +
        String(cfg_fps) +
        " fps</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Bewegung</div>"
        "<div class='card-value'>" +
        String(
            motionActive
            ? "Erkannt"
            : "Keine"
        ) +
        "</div>"
        "<div class='card-note'>OT2=" +
        String(ot2Active ? "1" : "0") +
        " &middot; Radar=" +
        String(radarMotion ? "1" : "0") +
        " &middot; Simulation=" +
        String(simulatedMotion ? "1" : "0") +
        "</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>SD-Karte</div>"
        "<div class='card-value'>" +
        String(
            (unsigned long)(
                freeBytes /
                1024ULL /
                1024ULL
            )
        ) +
        " MB frei</div>"
        "<div class='card-note'>" +
        String(usedPercent) +
        "% belegt von " +
        String(
            (unsigned long)(
                totalBytes /
                1024ULL /
                1024ULL
            )
        ) +
        " MB</div>"
        "<div class='progress'><span style='width:" +
        String(usedPercent) +
        "%'></span></div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Netzwerk</div>"
        "<div class='card-value'>" +
        htmlEscape(networkText) +
        "</div>"
        "<div class='card-note'>Hostname: " +
        htmlEscape(cfg_hostname) +
        ".local</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Konfiguration</div>"
        "<div class='card-value'>" +
        htmlEscape(String(configSourceName())) +
        "</div>"
        "<div class='card-note'>SD: " +
        htmlEscape(String(configSdStatusName())) +
        " &middot; interner Shadow: " +
        String(
            configInternalValid()
            ? "valid"
            : "nicht valid"
        ) +
        "</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Plattform</div>"
        "<div class='card-value'>" SENSORFORGE_FABRIC_LITERAL "</div>"
        "<div class='card-note'>Node: " +
        htmlEscape(cfg_hostname) +
        "<br>Role: " SENSORFORGE_PLATFORM_LITERAL
        "<br>Core: v" SENSORFORGE_CORE_VERSION_LITERAL
        "<br>Build: " +
        htmlEscape(firmwareBuildTimestamp()) +
        "</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Firmware</div>"
        "<div class='card-value'>" +
        htmlEscape(firmwareBuildTimestamp()) +
        "</div>"
        "<div class='card-note'>Installiert: " +
        htmlEscape(firmwareInstallTimestamp()) +
        "<br>Quelle: " +
        htmlEscape(firmwareInstallSource()) +
        "</div>"
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Echtzeituhr</div>";

    if (rtcDetected()) {

        html +=
            "<div class='card-value'>" +
            htmlEscape(String(rtcTypeName())) +
            "</div>"
            "<div class='card-note'><span class='status-pill " +
            String(rtcClockValid() ? "ok" : "warn") +
            "'>" +
            String(rtcClockValid() ? "RTC OK" : "Zeit prüfen") +
            "</span><br>" +
            htmlEscape(rtcTimeText()) +
            "</div>";

    } else {

        html +=
            "<div class='card-value'>Nicht erkannt</div>"
            "<div class='card-note'><span class='status-pill'>Optional</span><br>"
            "System läuft ohne externe RTC weiter.</div>";
    }

    html +=
        "</section>";

    html +=
        "<section class='dash-card'>"
        "<div class='card-label'>Temperatur</div>"
        "<div id='cpuTempCard' class='card-value'>" +
        String(
            cpuTempValid
            ? String(cpuTempC, 1) + " °C"
            : String("--")
        ) +
        "</div>"
        "<div class='card-note'>ESP32 Chiptemperatur"
        "<br>RTC-Modul (Gehäuseindikator): <span id='rtcTempCard'>" +
        String(
            rtcTempValid
            ? String(rtcTempC, 1) + " °C"
            : String("--")
        ) +
        "</span>"
        "<br>Schutzstatus: <b><span id='thermalStateCard'>" +
        htmlEscape(thermalUiState) +
        "</span></b>"
        "<br><b>CPU:</b> Warnung ab " +
        String(SENSORFORGE_THERMAL_WARNING_C, 0) +
        " °C, Notprogramm ab " +
        String(SENSORFORGE_THERMAL_EMERGENCY_C, 0) +
        " °C nach " +
        String((unsigned)SENSORFORGE_THERMAL_EMERGENCY_CONFIRM_SAMPLES) +
        " Messungen, Wiederanlauf unter " +
        String(SENSORFORGE_THERMAL_RECOVERY_C, 0) +
        " °C."
        "<br><b>RTC/Gehäuse:</b> Warnung ab " +
        String(SENSORFORGE_THERMAL_RTC_WARNING_C, 0) +
        " °C, Notprogramm ab " +
        String(SENSORFORGE_THERMAL_RTC_EMERGENCY_C, 0) +
        " °C nach " +
        String((unsigned)SENSORFORGE_THERMAL_RTC_EMERGENCY_CONFIRM_SAMPLES) +
        " Messungen, Wiederanlauf unter " +
        String(SENSORFORGE_THERMAL_RTC_RECOVERY_C, 0) +
        " °C."
        "<br>Die RTC-Temperatur ist nur ein Gehäuseindikator und keine direkte LiPo-Zelltemperatur."
        "<br>Abkühlpause: " +
        String((unsigned long)(SENSORFORGE_THERMAL_COOLDOWN_SECONDS / 60UL)) +
        " min. Im Notprogramm wird eine laufende Aufnahme sauber beendet, WLAN/Kamera werden abgeschaltet "
        "und das Gerät geht in timer-gesteuerten Deep Sleep. Sicherheitsgrenzen sind fest in der Firmware hinterlegt.</div>"
        "</section>";

    html +=
        "</div>";

    html +=
        "<section id='recordingControl' class='recording-control" +
        String(recordingPaused ? " paused" : "") +
        "'>"
        "<div class='recording-control-copy'>"
        "<div class='recording-control-title'>Aufnahmeautomatik &nbsp;"
        "<span id='recordingAutomationPill' class='status-pill " +
        String(recordingPaused ? "warn" : "ok") +
        "'>" +
        String(recordingPaused ? "PAUSIERT" : "AKTIV") +
        "</span></div>"
        "<div id='recordingAutomationNote' class='recording-control-note'>" +
        String(
            recordingPaused
            ? "Bewegungssensoren bleiben für die Diagnose sichtbar, lösen aber keine Aufnahme aus. "
              "Eine laufende Aufnahme wird sauber beendet."
            : "Für Wartung oder Konfigurationsänderungen kann die automatische Bewegungsauslösung "
              "vorübergehend pausiert werden."
        ) +
        "<br><b>Sicherheitsfunktion:</b> Die Pause wird automatisch aufgehoben, wenn etwa 35 Sekunden "
        "keine sichtbare SensorForge-Webseite mehr aktiv ist.</div>"
        "</div>"
        "<div class='recording-control-actions'>"
        "<button id='recordingPauseButton' type='button'>" +
        String(
            recordingPaused
            ? "Aufnahme wieder aktivieren"
            : "Aufnahmeautomatik pausieren"
        ) +
        "</button>"
        "</div>"
        "</section>";

    html +=
        "<script>"
        "(function(){"
        "var pill=document.getElementById('recordingStatusPill');"
        "var value=document.getElementById('recordingCardValue');"
        "var panel=document.getElementById('recordingControl');"
        "var autoPill=document.getElementById('recordingAutomationPill');"
        "var note=document.getElementById('recordingAutomationNote');"
        "var btn=document.getElementById('recordingPauseButton');"
        "var globalPill=document.getElementById('recordingPauseGlobal');"
        "var cpuCard=document.getElementById('cpuTempCard');"
        "var rtcCard=document.getElementById('rtcTempCard');"
        "var thermalCard=document.getElementById('thermalStateCard');"
        "var paused=false;"
        "function apply(s){"
            "var active=!!s.recording;"
            "paused=!!s.recording_paused;"
            "if(pill){"
                "if(active){pill.textContent='AUFNAHME LÄUFT';pill.className='status-pill danger';}"
                "else if(paused){pill.textContent='AUFNAHME PAUSIERT';pill.className='status-pill warn';}"
                "else{pill.textContent='Bereit';pill.className='status-pill ok';}"
            "}"
            "if(value)value.textContent=active?'Aktiv':(paused?'Pausiert':'Bereit');"
            "if(panel)panel.classList.toggle('paused',paused);"
            "if(autoPill){autoPill.textContent=paused?'PAUSIERT':'AKTIV';"
                "autoPill.className='status-pill '+(paused?'warn':'ok');}"
            "if(note)note.innerHTML=paused"
                "?'Bewegungssensoren bleiben für die Diagnose sichtbar, lösen aber keine Aufnahme aus. "
                  "Eine laufende Aufnahme wird sauber beendet.<br><b>Sicherheitsfunktion:</b> Die Pause wird automatisch aufgehoben, "
                  "wenn etwa 35 Sekunden keine sichtbare SensorForge-Webseite mehr aktiv ist.'"
                ":'Für Wartung oder Konfigurationsänderungen kann die automatische Bewegungsauslösung vorübergehend pausiert werden."
                  "<br><b>Sicherheitsfunktion:</b> Die Pause wird automatisch aufgehoben, wenn etwa 35 Sekunden keine sichtbare "
                  "SensorForge-Webseite mehr aktiv ist.';"
            "if(btn){btn.textContent=paused?'Aufnahme wieder aktivieren':'Aufnahmeautomatik pausieren';btn.disabled=false;}"
            "if(globalPill)globalPill.hidden=!paused;"
            "if(cpuCard)cpuCard.textContent=s.cpu_temp_valid?(Number(s.cpu_temp_c).toFixed(1)+' °C'):'--';"
            "if(rtcCard)rtcCard.textContent=s.rtc_temp_valid?(Number(s.rtc_temp_c).toFixed(1)+' °C'):'--';"
            "if(cpuCard){var ct=Number(s.cpu_temp_c);cpuCard.style.color=s.cpu_temp_valid&&ct>=Number(s.thermal_emergency_c)?'#b42318':"
                "(s.cpu_temp_valid&&ct>=Number(s.thermal_warning_c)?'#9a5a00':'');}"
            "if(rtcCard){var rt=Number(s.rtc_temp_c);rtcCard.style.color=s.rtc_temp_valid&&rt>=Number(s.thermal_rtc_emergency_c)?'#b42318':"
                "(s.rtc_temp_valid&&rt>=Number(s.thermal_rtc_warning_c)?'#9a5a00':'');}"
            "if(thermalCard){var src=(s.thermal_source&&s.thermal_source!=='NONE')?(' ('+s.thermal_source+')'):'';"
                "thermalCard.textContent=(s.thermal_state||'OK')+src;}"
        "}"
        "function poll(){"
            "fetch('/ui_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'})"
            ".then(function(r){if(!r.ok)throw new Error();return r.json();})"
            ".then(apply).catch(function(){});"
        "}"
        "if(btn)btn.addEventListener('click',function(){"
            "btn.disabled=true;btn.textContent=paused?'Aktiviere ...':'Pausiere ...';"
            "fetch('/recording_pause',{method:'POST',cache:'no-store',credentials:'same-origin',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'action='+(paused?'resume':'pause')})"
            ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
            ".then(apply).catch(function(e){alert('Aufnahmeautomatik konnte nicht geändert werden: '+e.message);poll();});"
        "});"
        "poll();"
        "setInterval(poll,2000);"
        "document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});"
        "})();"
        "</script>";

    html +=
        "<div class='quick-actions'>"
        "<a class='button primary' href='/files'>Aufnahmen</a>"
        "<a class='button' href='/preview'>Live Preview</a>"
        "<a class='button' href='/radar_config'>Radar</a>"
        "<a class='button' href='/config'>Konfiguration</a>"
        "</div>";

    html +=
        "<section id='simulation' class='settings-section'>"
        "<h3>Bewegung simulieren</h3>"
        "<p class='muted'>Testet die Aufnahmeauslösung ohne reale Radar-/PIR-Bewegung.</p>"
        "<form method='POST' action='/simulate_motion'>"
        "Dauer: "
        "<input name='seconds' type='number' min='1' max='3600' value='" +
        String(simulationDurationSeconds) +
        "' style='width:90px;'> Sekunden "
        "<button type='submit'>Simulation starten</button>";

    if (webConfigMotionActive()) {

        uint32_t remainingSeconds =
            (
                webConfigMotionRemainingMs() +
                999UL
            ) /
            1000UL;

        html +=
            "<br><span class='status-pill warn'>Simulation aktiv: noch ca. " +
            String(remainingSeconds) +
            " s</span>";
    }

    html +=
        "</form></section>";

    html +=
        htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleConfig()
{
    // SD contents may have changed since boot (wipe/card swap).
    configRefreshSdStatus();

    String html = htmlHeader();

    html += "<div class='page-title'><div><h2>Konfiguration</h2>"
            "<p>Geräteeinstellungen, Aufnahme, WLAN und Speicher.</p></div></div>";

    html += "<p style='padding:10px;background:#f7f7f7;border-radius:6px;'>"
            "<b>Firmware Build:</b> " +
            htmlEscape(firmwareBuildTimestamp()) +
            "<br><b>Installiert:</b> " +
            htmlEscape(firmwareInstallTimestamp()) +
            "<br><b>Quelle:</b> " +
            htmlEscape(firmwareInstallSource()) +
            "</p>";

    html += "<p><b>Config source:</b> " +
            htmlEscape(String(configSourceName())) +
            "<br><b>SD config:</b> " +
            htmlEscape(String(configSdStatusName())) +
            "<br><b>Internal shadow:</b> " +
            String(
                configInternalValid()
                ? "valid"
                : (
                    configInternalAvailable()
                    ? "missing / invalid"
                    : "unavailable"
                )
            ) +
            "</p>";


    ConfigCopyCompareResult copyCompare =
        compareConfigCopies();


    if (
        copyCompare ==
        CONFIG_COPY_COMPARE_DIFFERENT
    ) {

        html +=
            "<p style='padding:10px;border:2px solid #c00;background:#fff3f3;color:#900;'>"
            "<b>WARNUNG:</b> SD <code>/config.txt</code> und interner "
            "LittleFS-Shadow sind nicht identisch. "
            "Beim normalen Neustart hat eine gültige SD-Konfiguration Vorrang."
            "</p>";

    } else if (
        copyCompare ==
        CONFIG_COPY_COMPARE_IDENTICAL
    ) {

        html +=
            "<p style='color:#087a00;'>"
            "<b>Config-Kopien:</b> SD und interner Shadow sind identisch."
            "</p>";

    } else if (
        copyCompare ==
        CONFIG_COPY_COMPARE_READ_ERROR
    ) {

        html +=
            "<p style='color:#9a5a00;'>"
            "<b>Config-Kopien:</b> Vergleich konnte nicht vollständig gelesen werden."
            "</p>";
    }


    html += "<form id='configForm' method='POST' action='/save'>";

    bool sdIsActiveConfig =
        configGetSource() == CONFIG_SOURCE_SD &&
        configSdPresent() &&
        configSdValid();

    // Only an actually active, valid SD config is synchronized automatically.
    // If runtime came from internal/defaults, overwriting SD requires
    // explicit confirmation.
    html += "<input id='writeSd' type='hidden' name='write_sd' value='" +
            String(sdIsActiveConfig ? "1" : "0") +
            "'>";

    html += "<div class='settings-section'><h3>Kamera</h3>";
    html += "camera: <input name='camera' value='" +
            htmlEscape(cfg_camera) + "'><br>";

    html += "resolution: <input name='resolution' value='" +
            htmlEscape(cfg_resolution) + "'><br>";

    html += "fps: <input name='fps' type='number' min='1' max='30' value='" +
            String(cfg_fps) + "'><br>";

    html += "quality: <input name='quality' type='number' min='0' max='63' value='" +
            String(cfg_quality) + "'>"
            " <small>(kleiner = bessere JPEG-Qualität, Testwert: 12)</small><br>";

    html += "camera_auto_exposure: <select name='camera_auto_exposure'>";

    html += "<option value='1'" +
            String(cfg_camera_auto_exposure ? " selected" : "") +
            ">1 - Auto Exposure an</option>";

    html += "<option value='0'" +
            String(!cfg_camera_auto_exposure ? " selected" : "") +
            ">0 - Auto Exposure aus</option>";

    html += "</select><br>";

    html += "camera_ae_level: <input name='camera_ae_level' type='number' "
            "min='-2' max='2' value='" +
            String(cfg_camera_ae_level) +
            "'> <small>(-2..2; negativer = dunkleres AE-Ziel)</small><br>";

    html += "rotation: <select name='rotation'>";

    html += "<option value='0'" +
            String(cfg_rotation == 0 ? " selected" : "") +
            ">0°</option>";

    html += "<option value='180'" +
            String(cfg_rotation == 180 ? " selected" : "") +
            ">180°</option>";

    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>Aufnahme</h3>";

    html += "recording_format: <select name='recording_format'>";

    html += "<option value='avi'" +
            String(cfg_recording_format == "avi" ? " selected" : "") +
            ">AVI (MJPEG + separate SRT)</option>";

    html += "<option value='mkv'" +
            String(cfg_recording_format == "mkv" ? " selected" : "") +
            ">MKV (MJPEG + embedded subtitles)</option>";

    html += "</select><br>";

    html += "timestamp_enabled: <select name='timestamp_enabled'>";

    html += "<option value='1'" +
            String(cfg_timestamp_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_timestamp_enabled ? " selected" : "") +
            ">0 - aus</option>";

    html += "</select><br>";

    html += "post_record_ms: <input name='post_record_ms' type='number' min='0' value='" +
            String(cfg_post_ms) + "'><br>";

    html += "recording_segment_seconds: <input name='recording_segment_seconds' type='number' "
            "min='0' max='86400' value='" +
            String(cfg_recording_segment_seconds) +
            "'> <small>(0 = unbegrenzt)</small><br>";

    html += "recording_segment_max_mb: <input name='recording_segment_max_mb' type='number' "
            "min='0' max='4095' value='" +
            String(cfg_recording_segment_max_mb) +
            "'> <small>(0 = unbegrenzt)</small><br>";


    html += "</div><div class='settings-section'><h3>Sleep / Stromsparen</h3>";

    html += "sleep_mode: <select name='sleep_mode'>";

    html += "<option value='off'" +
            String(cfg_sleep_mode == "off" ? " selected" : "") +
            ">off - kein Sleep</option>";

    html += "<option value='light_sleep'" +
            String(cfg_sleep_mode == "light_sleep" ? " selected" : "") +
            ">light_sleep - schneller Wake</option>";

    html += "<option value='deep_sleep'" +
            String(cfg_sleep_mode == "deep_sleep" ? " selected" : "") +
            ">deep_sleep - maximal stromsparend</option>";

    html += "</select><br>";

    html += "sleep_delay_ms: <input name='sleep_delay_ms' type='number' "
            "min='0' max='60000' value='" +
            String(cfg_sleep_delay_ms) +
            "'> <small>(0..60000 ms)</small><br>";


    html += "</div><div class='settings-section'><h3>Speicher / SD-Sicherheit</h3>";

    html += "min_free_space_mb: <input name='min_free_space_mb' type='number' min='0' value='" +
            String(cfg_min_free_space_mb) + "'><br>";

    html += "disk_full_action: <select name='disk_full_action'>";

    html += "<option value='rollover'" +
            String(cfg_disk_full_action == "rollover" ? " selected" : "") +
            ">rollover - älteste Aufnahmen löschen</option>";

    html += "<option value='stop'" +
            String(cfg_disk_full_action == "stop" ? " selected" : "") +
            ">stop - Aufnahme stoppen</option>";

    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>LED</h3>";

    html += "led_enabled: <select name='led_enabled'>";
    html += "<option value='1'" +
            String(cfg_led_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_led_enabled ? " selected" : "") +
            ">0 - aus</option>";
    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>WLAN / NTP</h3>";

    html += "hostname: <input name='hostname' value='" +
            htmlEscape(cfg_hostname) + "'>"
            " <small>(auch Hotspot-SSID)</small><br>";

    html += "timezone: <input name='timezone' maxlength='127' value='" +
            htmlEscape(cfg_timezone) + "'>"
            " <small>POSIX TZ, z.B. Österreich: "
            "CET-1CEST,M3.5.0,M10.5.0/3; UTC: UTC0</small><br>";

    html += "wifi_on_system_start: <select name='wifi_on_system_start'>";

    html += "<option value='off'" +
            String(cfg_wifi_on_system_start == "off" ? " selected" : "") +
            ">off - WLAN beim Systemstart aus</option>";

    html += "<option value='on'" +
            String(cfg_wifi_on_system_start == "on" ? " selected" : "") +
            ">on - WLAN beim Systemstart ein</option>";

    html += "<option value='on_missing_time'" +
            String(cfg_wifi_on_system_start == "on_missing_time" ? " selected" : "") +
            ">on_missing_time - nur einschalten, wenn Uhrzeit fehlt</option>";

    html += "</select><br>";

    html += "wifi_timeout_sec: <input name='wifi_timeout_sec' type='number' min='0' max='86400' value='" +
            String(cfg_wifi_timeout_sec) + "'>"
            " <small>(0 = automatische Abschaltung aus)</small><br>";

    html += "wifi_ssid: <input name='wifi_ssid' value='" +
            htmlEscape(cfg_wifi_ssid) + "'><br>";

    // Passwort absichtlich nicht im HTML zurücksenden.
    // Leeres Feld bedeutet: bestehendes Passwort behalten.
    html += "wifi_pass: <input type='password' name='wifi_pass' "
            "value='' placeholder='leer = unverändert'><br>";


    html += "</div><div class='settings-section'><h3>Hotspot / Access Point</h3>";

    html += "hotspot_enabled: <select name='hotspot_enabled'>";
    html += "<option value='1'" +
            String(cfg_hotspot_enabled ? " selected" : "") +
            ">1 - beim Systemstart automatisch an</option>";
    html += "<option value='0'" +
            String(!cfg_hotspot_enabled ? " selected" : "") +
            ">0 - beim Systemstart aus</option>";
    html += "</select><br>";

    // Hotspot-Passwort ebenfalls nie an den Browser zurücksenden.
    // Leeres Feld bedeutet: bestehendes Passwort behalten.
    html += "hotspot_password: <input type='password' name='hotspot_password' "
            "minlength='8' maxlength='63' value='' "
            "placeholder='leer = unverändert'>"
            " <small>(8..63 Zeichen)</small><br>";

    html += "hotspot_hidden: <select name='hotspot_hidden'>";
    html += "<option value='0'" +
            String(!cfg_hotspot_hidden ? " selected" : "") +
            ">0 - SSID sichtbar</option>";
    html += "<option value='1'" +
            String(cfg_hotspot_hidden ? " selected" : "") +
            ">1 - SSID versteckt</option>";
    html += "</select><br>";


    html += "</div><div class='settings-section'><h3>Webinterface / Zugriffsschutz</h3>";

    html += "web_auth_enabled: <select name='web_auth_enabled'>";
    html += "<option value='1'" +
            String(cfg_web_auth_enabled ? " selected" : "") +
            ">1 - Login erforderlich</option>";
    html += "<option value='0'" +
            String(!cfg_web_auth_enabled ? " selected" : "") +
            ">0 - ohne Login</option>";
    html += "</select><br>";

    html += "web_username: <input name='web_username' maxlength='32' value='" +
            htmlEscape(cfg_web_username) +
            "'> <small>(1..32 Zeichen, kein Doppelpunkt)</small><br>";

    // Web-Passwort nie an den Browser zuruecksenden.
    // Leeres Feld bedeutet: bestehendes Passwort behalten.
    html += "web_password: <input type='password' name='web_password' "
            "minlength='8' maxlength='63' value='' "
            "placeholder='leer = unverändert'>"
            " <small>(8..63 Zeichen)</small><br>";


    html += "</div><div class='settings-section'><h3>Debug / Log</h3>";

    html += "debug_enabled: <select name='debug_enabled'>";
    html += "<option value='1'" +
            String(cfg_debug_enabled ? " selected" : "") +
            ">1 - an</option>";

    html += "<option value='0'" +
            String(!cfg_debug_enabled ? " selected" : "") +
            ">0 - aus</option>";
    html += "</select><br>";

    html += "log_file: <input name='log_file' value='" +
            htmlEscape(cfg_log_file) + "'><br>";

    html += "</div>";
    html += "<div class='form-actions'><button type='submit'>Speichern</button></div>";
    html += "</form>";


    if (
        configSdAvailable() &&
        !configSdPresent()
    ) {

        String promptText =
            "Auf der SD-Karte befindet sich keine config.txt. "
            "Soll die Konfiguration zusätzlich auf der "
            "SD-Karte gespeichert werden?";


        html +=
            "<script>"
            "document.getElementById('configForm').addEventListener('submit',function(){"
                "var writeSd=confirm('" +
                htmlEscape(promptText) +
                "');"
                "document.getElementById('writeSd').value=writeSd?'1':'0';"
            "});"
            "</script>";

    } else if (
        configSdAvailable() &&
        configSdPresent() &&
        configGetSource() != CONFIG_SOURCE_SD
    ) {

        String promptText =
            "ACHTUNG: Die laufende Konfiguration stammt NICHT von der SD-Karte "
            "(Quelle: " +
            String(configSourceName()) +
            "). Soll die vorhandene SD-config.txt wirklich mit den aktuell "
            "angezeigten Werten überschrieben werden?";

        html +=
            "<p style='color:#b00000;'><b>SD-config.txt wird NICHT automatisch überschrieben.</b> "
            "Die aktive Konfiguration stammt aus " +
            htmlEscape(String(configSourceName())) +
            ". Beim Speichern wirst du ausdrücklich gefragt.</p>";

        html +=
            "<script>"
            "document.getElementById('configForm').addEventListener('submit',function(){"
                "var writeSd=confirm('" +
                htmlEscape(promptText) +
                "');"
                "document.getElementById('writeSd').value=writeSd?'1':'0';"
            "});"
            "</script>";

    } else if (!configSdAvailable()) {

        html +=
            "<p><i>SD-Karte nicht verfügbar: Save aktualisiert "
            "nur den internen Config-Shadow.</i></p>";
    }


    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// SAVE CONFIG
// -------------------------------------------------------------

static void handleSave()
{
    if (rejectWhileRecording("config save"))
        return;


    // Never trust cached boot state when deciding whether the
    // user must be asked to create/replace SD config.txt.
    configRefreshSdStatus();


    int fps =
        constrain(
            server.arg("fps").toInt(),
            1,
            30
        );

    int quality =
        constrain(
            server.arg("quality").toInt(),
            0,
            63
        );


    int cameraAutoExposure =
        server.arg("camera_auto_exposure").toInt()
        ? 1
        : 0;


    int cameraAeLevel =
        constrain(
            server.arg("camera_ae_level").toInt(),
            -2,
            2
        );


    int rotation =
        server.arg("rotation").toInt();

    if (
        rotation != 0 &&
        rotation != 180
    ) {
        rotation = 0;
    }


    String recordingFormat =
        server.arg("recording_format");

    recordingFormat.trim();
    recordingFormat.toLowerCase();

    if (
        recordingFormat != "avi" &&
        recordingFormat != "mkv"
    ) {
        recordingFormat = "avi";
    }


    int timestampEnabled =
        server.arg("timestamp_enabled").toInt()
        ? 1
        : 0;


    int postMs =
        server.arg("post_record_ms").toInt();

    if (postMs < 0)
        postMs = 0;


    int recordingSegmentSeconds =
        constrain(
            server.arg("recording_segment_seconds").toInt(),
            0,
            86400
        );


    int recordingSegmentMaxMb =
        constrain(
            server.arg("recording_segment_max_mb").toInt(),
            0,
            4095
        );


    String sleepMode =
        server.arg("sleep_mode");

    sleepMode.trim();
    sleepMode.toLowerCase();

    if (
        sleepMode != "off" &&
        sleepMode != "light_sleep" &&
        sleepMode != "deep_sleep"
    ) {
        sleepMode = "off";
    }


    int sleepDelayMs =
        server.arg("sleep_delay_ms").toInt();

    sleepDelayMs =
        constrain(
            sleepDelayMs,
            0,
            60000
        );


    int minFreeSpaceMb =
        server.arg("min_free_space_mb").toInt();

    if (minFreeSpaceMb < 0)
        minFreeSpaceMb = 0;


    String diskFullAction =
        server.arg("disk_full_action");

    diskFullAction.trim();
    diskFullAction.toLowerCase();

    if (
        diskFullAction != "rollover" &&
        diskFullAction != "stop"
    ) {
        diskFullAction = "rollover";
    }


    String wifiOnSystemStart =
        server.arg("wifi_on_system_start");

    wifiOnSystemStart.trim();
    wifiOnSystemStart.toLowerCase();

    if (
        wifiOnSystemStart != "off" &&
        wifiOnSystemStart != "on" &&
        wifiOnSystemStart != "on_missing_time"
    ) {
        wifiOnSystemStart = "off";
    }


    int wifiTimeoutSec =
        server.arg("wifi_timeout_sec").toInt();

    wifiTimeoutSec =
        constrain(
            wifiTimeoutSec,
            0,
            86400
        );


    String newPassword =
        server.arg("wifi_pass");

    // Empty password field means: keep current password.
    if (!newPassword.length()) {
        newPassword =
            cfg_wifi_pass;
    }


    int hotspotEnabled =
        server.arg("hotspot_enabled").toInt()
        ? 1
        : 0;


    String newHotspotPassword =
        server.arg("hotspot_password");

    // Empty password field means: keep current hotspot password.
    if (!newHotspotPassword.length()) {
        newHotspotPassword =
            cfg_hotspot_password;
    }


    int hotspotHidden =
        server.arg("hotspot_hidden").toInt()
        ? 1
        : 0;


    int webAuthEnabled =
        server.arg("web_auth_enabled").toInt()
        ? 1
        : 0;


    String webUsername =
        server.arg("web_username");

    webUsername.trim();


    String newWebPassword =
        server.arg("web_password");

    // Empty password field means: keep current web password.
    if (!newWebPassword.length()) {
        newWebPassword =
            cfg_web_password;
    }


    String camera =
        server.arg("camera");

    camera.trim();


    String resolution =
        server.arg("resolution");

    resolution.trim();


    String hostname =
        server.arg("hostname");

    hostname.trim();


    String timezone =
        server.arg("timezone");

    timezone.trim();


    String wifiSsid =
        server.arg("wifi_ssid");

    wifiSsid.trim();


    String logFile =
        server.arg("log_file");

    logFile.trim();


    int ledEnabled =
        server.arg("led_enabled").toInt()
        ? 1
        : 0;


    int debugEnabled =
        server.arg("debug_enabled").toInt()
        ? 1
        : 0;


    // Build one canonical config text. Exactly the same text is
    // written to LittleFS and, when requested, to the SD card.
    String text;

    text.reserve(
        1280
    );


    text += "camera=";
    text += camera;
    text += '\n';

    text += "resolution=";
    text += resolution;
    text += '\n';

    text += "fps=";
    text += String(fps);
    text += '\n';

    text += "quality=";
    text += String(quality);
    text += '\n';

    text += "camera_auto_exposure=";
    text += String(cameraAutoExposure);
    text += '\n';

    text += "camera_ae_level=";
    text += String(cameraAeLevel);
    text += '\n';

    text += "rotation=";
    text += String(rotation);
    text += '\n';

    text += "recording_format=";
    text += recordingFormat;
    text += '\n';

    text += "timestamp_enabled=";
    text += String(timestampEnabled);
    text += '\n';

    text += "post_record_ms=";
    text += String(postMs);
    text += '\n';

    text += "recording_segment_seconds=";
    text += String(recordingSegmentSeconds);
    text += '\n';

    text += "recording_segment_max_mb=";
    text += String(recordingSegmentMaxMb);
    text += '\n';

    text += "sleep_mode=";
    text += sleepMode;
    text += '\n';

    text += "sleep_delay_ms=";
    text += String(sleepDelayMs);
    text += '\n';

    text += "led_enabled=";
    text += String(ledEnabled);
    text += '\n';

    text += "min_free_space_mb=";
    text += String(minFreeSpaceMb);
    text += '\n';

    text += "disk_full_action=";
    text += diskFullAction;
    text += '\n';

    text += "wifi_on_system_start=";
    text += wifiOnSystemStart;
    text += '\n';

    text += "wifi_timeout_sec=";
    text += String(wifiTimeoutSec);
    text += '\n';

    text += "hostname=";
    text += hostname;
    text += '\n';

    text += "timezone=";
    text += timezone;
    text += '\n';

    text += "wifi_ssid=";
    text += wifiSsid;
    text += '\n';

    text += "wifi_pass=";
    text += newPassword;
    text += '\n';

    text += "hotspot_enabled=";
    text += String(hotspotEnabled);
    text += '\n';

    text += "hotspot_password=";
    text += newHotspotPassword;
    text += '\n';

    text += "hotspot_hidden=";
    text += String(hotspotHidden);
    text += '\n';

    text += "web_auth_enabled=";
    text += String(webAuthEnabled);
    text += '\n';

    text += "web_username=";
    text += webUsername;
    text += '\n';

    text += "web_password=";
    text += newWebPassword;
    text += '\n';

    text += "debug_enabled=";
    text += String(debugEnabled);
    text += '\n';

    text += "log_file=";
    text += logFile;
    text += '\n';


    String validationError;


    if (!configValidateText(
            text,
            validationError
        )) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Config validation failed: " +
            validationError
        );

        return;
    }


    // SD writes are explicit. The page sets write_sd=1 automatically
    // only when SD is the current valid config source. If runtime came
    // from internal/defaults, replacing an existing SD config requires
    // an explicit browser confirmation.
    bool writeToSd =
        server.arg("write_sd") == "1";


    String saveError;


    ConfigSaveResult result =
        configSaveText(
            text,
            writeToSd,
            saveError
        );


    switch (result) {

        case CONFIG_SAVE_BOTH:

            // Sleep policy is safe to apply immediately. Other settings
            // may still require a reboot (camera, recording format, etc.).
            cfg_sleep_mode =
                sleepMode;

            cfg_sleep_delay_ms =
                sleepDelayMs;

            server.sendHeader(
                "Location",
                "/?notice=config_saved_both"
            );

            server.send(
                303,
                "text/plain; charset=utf-8",
                ""
            );

            return;


        case CONFIG_SAVE_INTERNAL_ONLY:

            // The running system should honor the just-saved sleep policy
            // immediately even when only the internal fallback was written.
            cfg_sleep_mode =
                sleepMode;

            cfg_sleep_delay_ms =
                sleepDelayMs;

            server.sendHeader(
                "Location",
                "/?notice=config_saved_internal"
            );

            server.send(
                303,
                "text/plain; charset=utf-8",
                ""
            );

            return;


        case CONFIG_SAVE_SD_FAILED:

            server.send(
                500,
                "text/plain; charset=utf-8",
                saveError +
                ". Internal shadow contains the new config."
            );

            return;


        case CONFIG_SAVE_INTERNAL_FAILED:
        default:

            server.send(
                500,
                "text/plain; charset=utf-8",
                saveError.length()
                ? saveError
                : String("Internal config save failed")
            );

            return;
    }
}



// -------------------------------------------------------------
// PIR SIMULATION
// -------------------------------------------------------------

static void handleSimulateMotion()
{
    int seconds =
        server.arg("seconds").toInt();

    if (seconds < 1)
        seconds = 5;

    if (seconds > 3600)
        seconds = 3600;

    simulationDurationSeconds =
        (uint32_t)seconds;

    simulatedMotionUntilMs =
        millis() +
        simulationDurationSeconds * 1000UL;

    Serial.printf(
        "PIR simulation started: %lu seconds\n",
        (unsigned long)simulationDurationSeconds
    );

    logWrite(
        "PIR simulation started: " +
        String(simulationDurationSeconds) +
        " s"
    );

    server.sendHeader(
        "Location",
        "/"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// SD STATUS / MAINTENANCE
// -------------------------------------------------------------

static void handleSDStatus()
{
    uint64_t total = STORAGE.totalBytes();
    uint64_t used  = STORAGE.usedBytes();

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>SD Status</h2>"
        "<p>Storage-Status und Wartungszugriff</p>"
        "</div></div>";

    html +=
        "<section class='settings-section'>"
        "<h3>Kapazität</h3>"
        "Total: ";

    html +=
        String((unsigned long)(total / 1024ULL / 1024ULL));

    html +=
        " MB<br>Used: ";

    html +=
        String((unsigned long)(used / 1024ULL / 1024ULL));

    html +=
        " MB<br>Free: ";

    html +=
        String((unsigned long)((total > used ? total - used : 0) / 1024ULL / 1024ULL));

    html +=
        " MB"
        "</section>"
        "<a class='button danger' href='/sd_maintenance'>SD Maintenance</a>"
        "<a class='button' href='/'>Zur Übersicht</a>";

    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


enum SdMaintenanceMode : uint8_t {
    SD_MAINT_WIPE = 0,
    SD_MAINT_FORMAT,
    SD_MAINT_SECURE_ERASE
};


enum SdMaintenanceResult : uint8_t {
    SD_MAINT_RESULT_OK = 0,
    SD_MAINT_RESULT_OPERATION_FAILED,
    SD_MAINT_RESULT_CONFIG_SOURCE_FAILED,
    SD_MAINT_RESULT_CONFIG_RESTORE_FAILED,
    SD_MAINT_RESULT_RECORDING_ACTIVE,
    SD_MAINT_RESULT_STORAGE_LOCKED,
    SD_MAINT_RESULT_UNSUPPORTED
};


enum SdSecureJobStage : uint8_t {
    SD_SECURE_JOB_IDLE = 0,
    SD_SECURE_JOB_OVERWRITE,
    SD_SECURE_JOB_FORMAT,
    SD_SECURE_JOB_RESTORE,
    SD_SECURE_JOB_DONE
};


// Secure Erase is intentionally processed incrementally from webConfigLoop().
// This keeps the synchronous WebServer responsive enough for progress polling
// and an Abort request while the logical overwrite is running.
static SdSecureJobStage sdSecureJobStage = SD_SECURE_JOB_IDLE;
static bool sdSecureJobActive = false;
static bool sdSecureJobDone = false;
static bool sdSecureAbortRequested = false;
static bool sdSecureAborted = false;
static bool sdSecureOperationOk = true;
static bool sdSecurePreviousRecordingBlock = false;
static bool sdSecurePreviousStorageLock = false;
static uint64_t sdSecureCardTotalBytes = 0;
static uint64_t sdSecureTargetBytes = 0;
static uint64_t sdSecureOverwrittenBytes = 0;
static size_t sdSecureBufferSize = 0;
static uint8_t *sdSecureZeroBuffer = nullptr;
static File sdSecureEraseFile;
static String sdSecureConfigText;
static String sdSecureLastError;
static SdMaintenanceResult sdSecureFinalResult =
    SD_MAINT_RESULT_OK;


static const size_t SD_MAINT_CONFIG_MAX_BYTES =
    32U * 1024U;


static bool readTextFileForMaintenance(
    fs::FS &filesystem,
    const char *path,
    String &text
)
{
    File file =
        filesystem.open(
            path,
            FILE_READ
        );

    if (!file)
        return false;

    size_t size =
        file.size();

    if (
        size == 0 ||
        size > SD_MAINT_CONFIG_MAX_BYTES
    ) {
        file.close();
        return false;
    }

    text = "";
    text.reserve(size + 1U);

    while (file.available()) {
        char buffer[256];

        size_t got =
            file.readBytes(
                buffer,
                sizeof(buffer)
            );

        if (got == 0)
            break;

        text.concat(
            buffer,
            got
        );
    }

    file.close();

    return
        text.length() ==
        size;
}


static bool loadInternalConfigForSdRestore(
    String &configText,
    String &error
)
{
    error = "";

    if (
        !configInternalAvailable() ||
        !configInternalValid() ||
        !LittleFS.exists("/config.txt")
    ) {
        error =
            "internal config shadow unavailable or invalid";
        return false;
    }

    if (!readTextFileForMaintenance(
            LittleFS,
            "/config.txt",
            configText
        )) {
        error =
            "cannot read internal /config.txt";
        return false;
    }

    String validationError;

    if (!configValidateText(
            configText,
            validationError
        )) {
        error =
            "internal /config.txt validation failed: " +
            validationError;
        return false;
    }

    return true;
}


static bool restoreConfigToSd(
    const String &configText,
    String &error
)
{
    error = "";

    STORAGE.remove("/config.tmp");
    STORAGE.remove("/config.bak");

    File file =
        STORAGE.open(
            "/config.tmp",
            FILE_WRITE
        );

    if (!file) {
        error =
            "cannot create SD /config.tmp";
        return false;
    }

    size_t written =
        file.print(
            configText
        );

    file.flush();
    file.close();

    if (
        written !=
        configText.length()
    ) {
        STORAGE.remove("/config.tmp");
        error =
            "incomplete SD config write";
        return false;
    }

    String verifyText;

    if (
        !readTextFileForMaintenance(
            STORAGE,
            "/config.tmp",
            verifyText
        ) ||
        verifyText != configText
    ) {
        STORAGE.remove("/config.tmp");
        error =
            "SD config verification failed";
        return false;
    }

    String validationError;

    if (!configValidateText(
            verifyText,
            validationError
        )) {
        STORAGE.remove("/config.tmp");
        error =
            "restored SD config is invalid: " +
            validationError;
        return false;
    }

    bool hadOld =
        STORAGE.exists(
            "/config.txt"
        );

    if (hadOld) {
        if (!STORAGE.rename(
                "/config.txt",
                "/config.bak"
            )) {
            STORAGE.remove("/config.tmp");
            error =
                "cannot backup old SD config";
            return false;
        }
    }

    if (!STORAGE.rename(
            "/config.tmp",
            "/config.txt"
        )) {

        if (hadOld) {
            STORAGE.rename(
                "/config.bak",
                "/config.txt"
            );
        }

        STORAGE.remove("/config.tmp");
        error =
            "cannot promote restored SD config";
        return false;
    }

    STORAGE.remove("/config.bak");

    String finalText;

    if (
        !readTextFileForMaintenance(
            STORAGE,
            "/config.txt",
            finalText
        ) ||
        finalText != configText
    ) {
        error =
            "final SD config verification failed";
        return false;
    }

    return true;
}


static bool deleteTree(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str()
        );

    if (!root)
        return false;

    if (!root.isDirectory()) {
        root.close();
        return
            STORAGE.remove(
                path.c_str()
            );
    }

    bool ok = true;

    File file =
        root.openNextFile();

    while (file) {

        String name =
            String(file.name());

        String fullPath;

        if (name.startsWith("/")) {
            fullPath = name;
        } else if (path == "/") {
            fullPath =
                "/" + name;
        } else {
            fullPath =
                path + "/" + name;
        }

        bool isDir =
            file.isDirectory();

        file.close();

        if (isDir) {

            if (!deleteTree(fullPath))
                ok = false;

            if (
                fullPath != "/" &&
                STORAGE.exists(
                    fullPath.c_str()
                ) &&
                !STORAGE.rmdir(
                    fullPath.c_str()
                )
            ) {
                ok = false;
            }

        } else {

            // SD maintenance always rebuilds /config.txt afterwards from
            // the validated LittleFS shadow. Therefore the SD copy itself
            // is deliberately deleted like every other file here.
            if (!STORAGE.remove(
                    fullPath.c_str()
                )) {
                ok = false;
            }
        }

        serviceWebLongOperation();

        file =
            root.openNextFile();
    }

    root.close();

    serviceWebLongOperation();

    return ok;
}


static uint32_t sdMaintReadU32LE(
    const uint8_t *p
)
{
    return
        (uint32_t)p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}


static bool locateFatVolumeStart(
    uint32_t &volumeStart,
    uint16_t &backupBootSector,
    String &error
)
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED

    error = "";
    volumeStart = 0;
    backupBootSector = 0;

    const size_t sectorSize =
        512U;

    uint64_t cardBytes =
        STORAGE.cardSize();

    uint64_t sectorCount64 =
        cardBytes /
        sectorSize;

    if (
        sectorCount64 == 0 ||
        sectorCount64 > 0xFFFFFFFFULL
    ) {
        error =
            "invalid SD raw sector geometry";
        return false;
    }

    uint32_t sectorCount =
        (uint32_t)sectorCount64;

    uint8_t *sector =
        (uint8_t *)malloc(
            sectorSize
        );

    if (!sector) {
        error =
            "cannot allocate SD sector buffer";
        return false;
    }

    if (!STORAGE.readRAW(
            sector,
            0
        )) {
        free(sector);
        error =
            "cannot read SD sector 0";
        return false;
    }

    bool signature =
        sector[510] == 0x55 &&
        sector[511] == 0xAA;

    uint16_t bytesPerSector =
        (uint16_t)sector[11] |
        ((uint16_t)sector[12] << 8);

    uint8_t sectorsPerCluster =
        sector[13];

    uint16_t reservedSectors =
        (uint16_t)sector[14] |
        ((uint16_t)sector[15] << 8);

    uint8_t fatCount =
        sector[16];

    bool plausibleFatBpb =
        signature &&
        (bytesPerSector == 512 ||
         bytesPerSector == 1024 ||
         bytesPerSector == 2048 ||
         bytesPerSector == 4096) &&
        sectorsPerCluster > 0 &&
        (sectorsPerCluster &
         (sectorsPerCluster - 1U)) == 0 &&
        reservedSectors > 0 &&
        (fatCount == 1 ||
         fatCount == 2);

    bool exFatBoot =
        signature &&
        memcmp(
            &sector[3],
            "EXFAT   ",
            8
        ) == 0;

    bool volumeAtSectorZero =
        plausibleFatBpb ||
        exFatBoot;

    bool partitionFound =
        false;

    if (
        signature &&
        !volumeAtSectorZero
    ) {
        for (
            uint8_t i = 0;
            i < 4;
            ++i
        ) {
            const uint8_t *entry =
                &sector[446U +
                    (uint16_t)i * 16U];

            uint8_t bootFlag =
                entry[0];

            uint8_t type =
                entry[4];

            uint32_t startLba =
                sdMaintReadU32LE(
                    &entry[8]
                );

            uint32_t count =
                sdMaintReadU32LE(
                    &entry[12]
                );

            if (type == 0xEE) {
                free(sector);
                error =
                    "GPT partition layout is not supported by SensorForge SD Format";
                return false;
            }

            bool plausible =
                (bootFlag == 0x00 ||
                 bootFlag == 0x80) &&
                type != 0x00 &&
                startLba > 0 &&
                startLba < sectorCount &&
                count > 0 &&
                count <=
                    sectorCount - startLba;

            if (plausible) {
                volumeStart =
                    startLba;
                partitionFound =
                    true;
                break;
            }
        }
    }

    if (
        partitionFound &&
        !STORAGE.readRAW(
            sector,
            volumeStart
        )
    ) {
        free(sector);
        error =
            "cannot read FAT volume boot sector";
        return false;
    }

    // FAT32 stores the backup boot sector number in BPB_BkBootSec.
    // For FAT12/16 these bytes are not used for this purpose; only
    // accept the value when the FAT32 signature is present.
    bool fat32 =
        sectorSize >= 90 &&
        memcmp(
            &sector[82],
            "FAT32   ",
            8
        ) == 0;

    if (fat32) {
        uint16_t reservedSectors =
            (uint16_t)sector[14] |
            ((uint16_t)sector[15] << 8);

        uint16_t backup =
            (uint16_t)sector[50] |
            ((uint16_t)sector[51] << 8);

        if (
            backup > 0 &&
            backup < reservedSectors &&
            (uint64_t)volumeStart +
                backup <
                sectorCount
        ) {
            backupBootSector =
                backup;
        }
    }

    free(sector);
    return true;

#else

    (void)volumeStart;
    (void)backupBootSector;
    error =
        "raw SD access is not available on this board";
    return false;

#endif
}


static bool invalidateFatFilesystem(
    String &error
)
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED

    uint32_t volumeStart = 0;
    uint16_t backupBootSector = 0;

    if (!locateFatVolumeStart(
            volumeStart,
            backupBootSector,
            error
        )) {
        return false;
    }

    const size_t sectorSize =
        512U;

    uint8_t *zeroSector =
        (uint8_t *)calloc(
            1,
            sectorSize
        );

    if (!zeroSector) {
        error =
            "cannot allocate format sector buffer";
        return false;
    }

    bool ok =
        STORAGE.writeRAW(
            zeroSector,
            volumeStart
        );

    if (
        ok &&
        backupBootSector > 0
    ) {
        ok =
            STORAGE.writeRAW(
                zeroSector,
                volumeStart +
                    backupBootSector
            );
    }

    free(zeroSector);

    if (!ok) {
        error =
            "cannot invalidate FAT boot sector";
        return false;
    }

    return true;

#else

    error =
        "raw SD access is not available on this board";
    return false;

#endif
}


static bool remountStorageAndFormat(
    String &error
)
{
    error = "";

#if defined(STORAGE_SDMMC)

    STORAGE.end();
    delay(30);

    SD_MMC.setPins(
        SD_MMC_CLK,
        SD_MMC_CMD,
        SD_MMC_D0
    );

    if (!SD_MMC.begin(
            "/sdcard",
            true,
            true
        )) {
        error =
            "SD_MMC remount/format failed";
        return false;
    }

    return true;

#elif defined(STORAGE_SPI)

    STORAGE.end();
    SPI.end();
    delay(30);

    SPI.begin(
        SD_SCK_PIN,
        SD_MISO_PIN,
        SD_MOSI_PIN,
        SD_CS_PIN
    );

    if (!SD.begin(
            SD_CS_PIN,
            SPI,
            4000000UL,
            "/sd",
            5,
            true
        )) {
        error =
            "SPI SD remount/format failed";
        return false;
    }

    return true;

#else

    error =
        "SD formatting is unsupported by this storage backend";
    return false;

#endif
}


static bool formatSdFilesystem(
    String &error
)
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED

    if (!invalidateFatFilesystem(
            error
        )) {
        return false;
    }

    // The Arduino SD/SD_MMC wrappers expose format-on-mount-failure,
    // but not a common explicit format() method. After invalidating only
    // the FAT boot sector(s), remounting with format-on-failure creates a
    // fresh FAT filesystem while preserving the physical card interface.
    return
        remountStorageAndFormat(
            error
        );

#else

    error =
        "SD formatting is unsupported by this storage backend";
    return false;

#endif
}


static bool overwriteFreeSpaceWithZeros(
    uint64_t &overwrittenBytes,
    String &error
)
{
    overwrittenBytes = 0;
    error = "";

    uint64_t total =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    if (
        total == 0 ||
        used > total
    ) {
        error =
            "cannot determine SD free space";
        return false;
    }

    uint64_t freeBefore =
        total - used;

    File eraseFile =
        STORAGE.open(
            "/.__sensorforge_secure_erase.bin",
            FILE_WRITE
        );

    if (!eraseFile) {
        error =
            "cannot create secure erase overwrite file";
        return false;
    }

    size_t bufferSize =
        32U * 1024U;

    uint8_t *zeroBuffer =
        (uint8_t *)calloc(
            1,
            bufferSize
        );

    if (!zeroBuffer) {
        bufferSize =
            4096U;

        zeroBuffer =
            (uint8_t *)calloc(
                1,
                bufferSize
            );
    }

    if (!zeroBuffer) {
        eraseFile.close();
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );
        error =
            "cannot allocate secure erase buffer";
        return false;
    }

    while (true) {

        size_t written =
            eraseFile.write(
                zeroBuffer,
                bufferSize
            );

        overwrittenBytes +=
            written;

        if (written < bufferSize)
            break;

        if (
            (overwrittenBytes &
             0x000FFFFFULL) <
            bufferSize
        ) {
            serviceWebLongOperation();
        }
    }

    eraseFile.flush();
    eraseFile.close();
    free(zeroBuffer);

    serviceWebLongOperation();

    // FAT bookkeeping needs a small amount of space of its own. Allow a
    // conservative tolerance, but require that practically all free clusters
    // were consumed by the zero-filled file.
    uint64_t tolerance =
        freeBefore / 50ULL;

    const uint64_t minTolerance =
        8ULL * 1024ULL * 1024ULL;

    if (tolerance < minTolerance)
        tolerance = minTolerance;

    if (tolerance > freeBefore)
        tolerance = freeBefore;

    bool sufficientlyCovered =
        overwrittenBytes +
            tolerance >=
        freeBefore;

    if (!sufficientlyCovered) {
        error =
            "secure overwrite stopped before covering the logical free area";
    }

    return sufficientlyCovered;
}


static bool sdFormatBackendSupported();


static void closeSecureEraseOverwriteFile()
{
    if (sdSecureEraseFile) {
        sdSecureEraseFile.flush();
        sdSecureEraseFile.close();
    }

    if (sdSecureZeroBuffer) {
        free(sdSecureZeroBuffer);
        sdSecureZeroBuffer = nullptr;
    }

    sdSecureBufferSize = 0;
}


static void releaseSecureEraseLocks()
{
    // The logger was closed when the job started. Reopen it only after the
    // current filesystem/config state is settled, then restore both gates.
    logInit();

    g_recordingStartBlocked =
        sdSecurePreviousRecordingBlock;

    g_storageLocked =
        sdSecurePreviousStorageLock;
}


static bool secureEraseCoverageSufficient()
{
    uint64_t tolerance =
        sdSecureTargetBytes / 50ULL;

    const uint64_t minTolerance =
        8ULL * 1024ULL * 1024ULL;

    if (tolerance < minTolerance)
        tolerance = minTolerance;

    if (tolerance > sdSecureTargetBytes)
        tolerance = sdSecureTargetBytes;

    return
        sdSecureOverwrittenBytes + tolerance >=
        sdSecureTargetBytes;
}


static SdMaintenanceResult beginSecureEraseJob()
{
    if (sdSecureJobActive) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (g_storageLocked) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (recorderIsOpen()) {
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    if (!sdFormatBackendSupported()) {
        return
            SD_MAINT_RESULT_UNSUPPORTED;
    }

    String configText;
    String error;

    if (!loadInternalConfigForSdRestore(
            configText,
            error
        )) {
        Serial.println(
            "SD maintenance aborted: " +
            error
        );
        return
            SD_MAINT_RESULT_CONFIG_SOURCE_FAILED;
    }

    sdSecurePreviousRecordingBlock =
        g_recordingStartBlocked;

    sdSecurePreviousStorageLock =
        g_storageLocked;

    g_storageLocked = true;
    g_recordingStartBlocked = true;

    webPlayerStop();

    if (recorderIsOpen()) {
        g_recordingStartBlocked =
            sdSecurePreviousRecordingBlock;
        g_storageLocked =
            sdSecurePreviousStorageLock;
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    logClose();

    sdSecureJobActive = true;
    sdSecureJobDone = false;
    sdSecureAbortRequested = false;
    sdSecureAborted = false;
    sdSecureOperationOk = true;
    sdSecureCardTotalBytes = 0;
    sdSecureTargetBytes = 0;
    sdSecureOverwrittenBytes = 0;
    sdSecureLastError = "";
    sdSecureFinalResult =
        SD_MAINT_RESULT_OK;
    sdSecureConfigText =
        configText;

    Serial.println(
        "SensorForge SD maintenance: SECURE ERASE start"
    );

    // Deleting the directory tree is normally fast compared with the full
    // overwrite. It remains synchronous, while the long zero-fill itself is
    // chunked from webConfigLoop() so the browser can poll and abort it.
    bool wipeOk =
        deleteTree("/");

    if (!wipeOk) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "SD wipe failed before secure overwrite";
        Serial.println(
            "SensorForge SD secure wipe warning: " +
            sdSecureLastError
        );
    }

    sdSecureCardTotalBytes =
        STORAGE.totalBytes();

    uint64_t used =
        STORAGE.usedBytes();

    if (
        sdSecureCardTotalBytes == 0 ||
        used > sdSecureCardTotalBytes
    ) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot determine SD free space";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureTargetBytes =
        sdSecureCardTotalBytes - used;

    sdSecureEraseFile =
        STORAGE.open(
            "/.__sensorforge_secure_erase.bin",
            FILE_WRITE
        );

    if (!sdSecureEraseFile) {
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot create secure erase overwrite file";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureBufferSize =
        32U * 1024U;

    sdSecureZeroBuffer =
        (uint8_t *)calloc(
            1,
            sdSecureBufferSize
        );

    if (!sdSecureZeroBuffer) {
        sdSecureBufferSize =
            4096U;

        sdSecureZeroBuffer =
            (uint8_t *)calloc(
                1,
                sdSecureBufferSize
            );
    }

    if (!sdSecureZeroBuffer) {
        closeSecureEraseOverwriteFile();
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );
        sdSecureOperationOk = false;
        sdSecureLastError =
            "cannot allocate secure erase buffer";
        sdSecureJobStage =
            SD_SECURE_JOB_FORMAT;
        return
            SD_MAINT_RESULT_OK;
    }

    sdSecureJobStage =
        SD_SECURE_JOB_OVERWRITE;

    return
        SD_MAINT_RESULT_OK;
}


static void processSecureEraseJob()
{
    if (!sdSecureJobActive)
        return;

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_OVERWRITE
    ) {
        if (sdSecureAbortRequested) {
            sdSecureAborted = true;

            Serial.printf(
                "SensorForge SD secure overwrite aborted at %llu bytes\n",
                (unsigned long long)sdSecureOverwrittenBytes
            );

            closeSecureEraseOverwriteFile();

            // Removing the temporary file only frees FAT cluster metadata;
            // the bytes already written remain zeroed. This also leaves room
            // for config recovery if formatting itself should fail.
            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );

            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
            return;
        }

        if (
            !sdSecureEraseFile ||
            !sdSecureZeroBuffer ||
            sdSecureBufferSize == 0
        ) {
            sdSecureOperationOk = false;
            sdSecureLastError =
                "secure overwrite state invalid";
            closeSecureEraseOverwriteFile();
            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );
            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
            return;
        }

        size_t written =
            sdSecureEraseFile.write(
                sdSecureZeroBuffer,
                sdSecureBufferSize
            );

        sdSecureOverwrittenBytes +=
            written;

        if (
            (sdSecureOverwrittenBytes &
             0x000FFFFFULL) <
            sdSecureBufferSize
        ) {
            serviceWebLongOperation();
        }

        if (written < sdSecureBufferSize) {
            closeSecureEraseOverwriteFile();

            bool covered =
                secureEraseCoverageSufficient();

            if (!covered) {
                sdSecureOperationOk = false;
                sdSecureLastError =
                    "secure overwrite stopped before covering the logical free area";
            }

            Serial.printf(
                "SensorForge SD secure overwrite: %llu / %llu bytes%s\n",
                (unsigned long long)sdSecureOverwrittenBytes,
                (unsigned long long)sdSecureTargetBytes,
                covered ? "" : " (incomplete)"
            );

            STORAGE.remove(
                "/.__sensorforge_secure_erase.bin"
            );

            sdSecureJobStage =
                SD_SECURE_JOB_FORMAT;
        }

        return;
    }

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_FORMAT
    ) {
        closeSecureEraseOverwriteFile();

        // Ensure no giant temporary overwrite file remains if we arrived here
        // through Abort or an overwrite error.
        STORAGE.remove(
            "/.__sensorforge_secure_erase.bin"
        );

        String formatError;

        bool formatOk =
            formatSdFilesystem(
                formatError
            );

        if (!formatOk) {
            sdSecureOperationOk = false;
            sdSecureLastError =
                formatError;
            Serial.println(
                "SensorForge SD secure format failed: " +
                formatError
            );
        }

        sdSecureJobStage =
            SD_SECURE_JOB_RESTORE;
        return;
    }

    if (
        sdSecureJobStage ==
        SD_SECURE_JOB_RESTORE
    ) {
        String restoreError;

        bool restoreOk =
            restoreConfigToSd(
                sdSecureConfigText,
                restoreError
            );

        if (!restoreOk) {
            Serial.println(
                "SensorForge SD config restore FAILED: " +
                restoreError
            );
        } else {
            Serial.println(
                "SensorForge SD config restored from internal flash"
            );
        }

        configRefreshSdStatus();
        releaseSecureEraseLocks();

        if (!restoreOk) {
            sdSecureFinalResult =
                SD_MAINT_RESULT_CONFIG_RESTORE_FAILED;
        } else if (!sdSecureOperationOk) {
            sdSecureFinalResult =
                SD_MAINT_RESULT_OPERATION_FAILED;
        } else {
            sdSecureFinalResult =
                SD_MAINT_RESULT_OK;
        }

        sdSecureConfigText = "";
        sdSecureJobActive = false;
        sdSecureJobDone = true;
        sdSecureJobStage =
            SD_SECURE_JOB_DONE;

        // Abort means "stop overwriting and continue with Format". If Format
        // and config restore succeeded, it is a successful controlled abort
        // and we still reboot to guarantee a completely fresh SD mount.
        if (
            sdSecureFinalResult ==
            SD_MAINT_RESULT_OK
        ) {
            rebootScheduled = true;
            rebootAtMs =
                millis() + 3000UL;
        }
    }
}


static bool sdFormatBackendSupported()
{
#if SENSORFORGE_SD_RAW_FORMAT_SUPPORTED
    return true;
#else
    return false;
#endif
}


static SdMaintenanceResult performSdMaintenance(
    SdMaintenanceMode mode
)
{
    // Another module may reserve the SD for a future critical operation.
    // Never start nested maintenance while the global storage gate is held.
    if (g_storageLocked) {
        return
            SD_MAINT_RESULT_STORAGE_LOCKED;
    }

    if (recorderIsOpen()) {
        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    if (
        mode != SD_MAINT_WIPE &&
        !sdFormatBackendSupported()
    ) {
        return
            SD_MAINT_RESULT_UNSUPPORTED;
    }

    String configText;
    String error;

    // Absolutely no destructive SD step is started unless a valid copy is
    // already available in internal flash and can be validated in RAM.
    if (!loadInternalConfigForSdRestore(
            configText,
            error
        )) {
        Serial.println(
            "SD maintenance aborted: " +
            error
        );
        return
            SD_MAINT_RESULT_CONFIG_SOURCE_FAILED;
    }

    bool previousRecordingBlock =
        g_recordingStartBlocked;

    bool previousStorageLock =
        g_storageLocked;

    // Raise both gates before closing any SD users. The generic storage gate
    // is intentionally separate from the recording-start gate so future
    // modules can also refuse new SD work during maintenance.
    g_storageLocked =
        true;

    g_recordingStartBlocked =
        true;

    // Close player file handles before touching the filesystem. The WebServer
    // remains registered; webPlayerStop() only closes the playback session.
    webPlayerStop();

    if (recorderIsOpen()) {
        g_recordingStartBlocked =
            previousRecordingBlock;

        g_storageLocked =
            previousStorageLock;

        Serial.println(
            "SD maintenance aborted: recording became active"
        );

        return
            SD_MAINT_RESULT_RECORDING_ACTIVE;
    }

    // The logger keeps its SD File handle open for normal operation. It MUST
    // be closed before wipe/format/remount, otherwise the stale handle can
    // write into unrelated files after the filesystem has been rebuilt.
    logClose();

    bool operationOk =
        true;

    if (mode == SD_MAINT_WIPE) {

        Serial.println(
            "SensorForge SD maintenance: WIPE start"
        );

        operationOk =
            deleteTree("/");

    } else if (mode == SD_MAINT_FORMAT) {

        Serial.println(
            "SensorForge SD maintenance: FORMAT start"
        );

        operationOk =
            formatSdFilesystem(
                error
            );

    } else {

        Serial.println(
            "SensorForge SD maintenance: SECURE ERASE start"
        );

        bool wipeOk =
            deleteTree("/");

        uint64_t overwrittenBytes =
            0;

        String overwriteError;

        bool overwriteOk =
            overwriteFreeSpaceWithZeros(
                overwrittenBytes,
                overwriteError
            );

        Serial.printf(
            "SensorForge SD secure overwrite: %llu bytes\n",
            (unsigned long long)overwrittenBytes
        );

        if (!overwriteOk) {
            Serial.println(
                "SensorForge SD secure overwrite warning: " +
                overwriteError
            );
        }

        String formatError;

        bool formatOk =
            formatSdFilesystem(
                formatError
            );

        if (!formatOk) {
            Serial.println(
                "SensorForge SD secure format failed: " +
                formatError
            );
        }

        operationOk =
            wipeOk &&
            overwriteOk &&
            formatOk;
    }

    if (
        !operationOk &&
        error.length()
    ) {
        Serial.println(
            "SensorForge SD maintenance warning: " +
            error
        );
    }

    String restoreError;

    bool restoreOk =
        restoreConfigToSd(
            configText,
            restoreError
        );

    if (!restoreOk) {
        Serial.println(
            "SensorForge SD config restore FAILED: " +
            restoreError
        );
    } else {
        Serial.println(
            "SensorForge SD config restored from internal flash"
        );
    }

    configRefreshSdStatus();

    // Reopen the logger only after the filesystem and /config.txt have been
    // restored. This guarantees a fresh File handle on the current mount.
    logInit();

    g_recordingStartBlocked =
        previousRecordingBlock;

    g_storageLocked =
        previousStorageLock;

    if (!restoreOk) {
        return
            SD_MAINT_RESULT_CONFIG_RESTORE_FAILED;
    }

    if (!operationOk) {
        return
            SD_MAINT_RESULT_OPERATION_FAILED;
    }

    return
        SD_MAINT_RESULT_OK;
}


static const char *sdMaintenanceResultLocation(
    SdMaintenanceMode mode,
    SdMaintenanceResult result
)
{
    if (
        result ==
        SD_MAINT_RESULT_CONFIG_SOURCE_FAILED
    ) {
        return
            "/?notice=sd_restore_source_failed";
    }

    if (
        result ==
        SD_MAINT_RESULT_CONFIG_RESTORE_FAILED
    ) {
        return
            "/?notice=sd_restore_failed";
    }

    if (
        result ==
        SD_MAINT_RESULT_RECORDING_ACTIVE
    ) {
        return
            "/?notice=sd_recording_active";
    }

    if (
        result ==
        SD_MAINT_RESULT_STORAGE_LOCKED
    ) {
        return
            "/?notice=sd_storage_locked";
    }

    if (
        result ==
        SD_MAINT_RESULT_UNSUPPORTED
    ) {
        return
            mode == SD_MAINT_SECURE_ERASE
            ? "/?notice=sd_secure_unsupported"
            : "/?notice=sd_format_unsupported";
    }

    if (mode == SD_MAINT_WIPE) {
        return
            result == SD_MAINT_RESULT_OK
            ? "/?notice=sd_wipe_done"
            : "/?notice=sd_wipe_failed";
    }

    if (mode == SD_MAINT_FORMAT) {
        return
            result == SD_MAINT_RESULT_OK
            ? "/?notice=sd_format_done"
            : "/?notice=sd_format_failed";
    }

    return
        result == SD_MAINT_RESULT_OK
        ? "/?notice=sd_secure_done"
        : "/?notice=sd_secure_failed";
}


static void redirectSdMaintenanceResult(
    SdMaintenanceMode mode,
    SdMaintenanceResult result
)
{
    // A real filesystem rebuild changes the SD mount underneath several
    // subsystems. Even though all known SD users are closed/reopened during
    // maintenance, reboot after a successful Format/Secure Erase gives every
    // module a completely fresh mount and avoids stale state in future code.
    if (
        result == SD_MAINT_RESULT_OK &&
        (
            mode == SD_MAINT_FORMAT ||
            mode == SD_MAINT_SECURE_ERASE
        )
    ) {
        rebootScheduled =
            true;

        rebootAtMs =
            millis() + 3000UL;

        server.sendHeader(
            "Location",
            mode == SD_MAINT_FORMAT
            ? "/rebooting?reason=sd_format"
            : "/rebooting?reason=sd_secure"
        );

        server.send(
            303,
            "text/plain; charset=utf-8",
            ""
        );

        return;
    }

    const char *location =
        sdMaintenanceResultLocation(
            mode,
            result
        );

    server.sendHeader(
        "Location",
        location
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleSDMaintenance()
{
    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>SD Maintenance</h2>"
        "<p>Destruktive Storage-Wartung mit automatischer Config-Wiederherstellung</p>"
        "</div></div>";

    html +=
        "<div class='flash-notice' style='border-left-color:var(--accent);background:#eef4ff'>"
        "<strong style='color:#174ea6'>Config-Schutz</strong>"
        "<span class='muted'>Vor jedem Vorgang wird der gültige interne LittleFS-Shadow von "
        "<code>/config.txt</code> in RAM geprüft. Erst danach beginnt die SD-Operation. "
        "Nach Wipe, Format oder Secure Erase wird diese interne Kopie immer wieder auf die SD-Karte geschrieben. "
        "Ist der interne Shadow nicht gültig, wird die Operation vollständig abgebrochen.</span>"
        "</div>";

    html +=
        "<section class='settings-section'>"
        "<span class='status-pill warn'>WIPE</span>"
        "<h3 style='margin-top:12px'>SD Wipe</h3>"
        "<p class='muted'>Löscht Dateien und Ordner über das vorhandene FAT-Dateisystem. "
        "Das Dateisystem selbst wird nicht neu erzeugt.</p>"
        "<form id='sdWipeForm' method='POST' action='/sdformat_do'>"
        "<button id='sdWipeButton' class='danger' type='submit'>SD WIPE STARTEN</button>"
        "</form>"
        "</section>";

    html +=
        "<section class='settings-section'>"
        "<span class='status-pill danger'>FORMAT</span>"
        "<h3 style='margin-top:12px'>SD Format</h3>"
        "<p class='muted'>Erzeugt das FAT-Dateisystem neu. Alte Daten können trotz Formatierung "
        "forensisch teilweise rekonstruierbar bleiben. Nach erfolgreicher Formatierung wird "
        "SensorForge automatisch neu gestartet.</p>";

    if (sdFormatBackendSupported()) {
        html +=
            "<form id='sdRealFormatForm' method='POST' action='/sd_format_do'>"
            "<button id='sdRealFormatButton' class='danger' type='submit'>SD FORMAT STARTEN</button>"
            "</form>";
    } else {
        html +=
            "<p class='status-pill danger'>Auf diesem Storage-Backend nicht unterstützt</p>";
    }

    html +=
        "</section>";

    html +=
        "<section class='settings-section' style='border-color:#e0a8a3'>"
        "<span class='status-pill danger'>SECURE ERASE</span>"
        "<h3 style='margin-top:12px'>Secure Erase &ndash; Logical Overwrite + Format</h3>"
        "<p class='muted'>Löscht zunächst alle Dateien, überschreibt danach den logisch freien "
        "Datenbereich mit Nullen und formatiert anschließend neu. Das kann je nach Kartengröße "
        "sehr lange dauern. Während des Überschreibens werden Datenmenge und Fortschritt live angezeigt. "
        "Ein Abbruch stoppt nur das weitere Überschreiben; die Formatierung wird danach trotzdem ausgeführt. "
        "Nach erfolgreichem Abschluss wird SensorForge automatisch neu gestartet.</p>"
        "<p class='muted'><b>Wichtig:</b> Wegen Wear-Leveling und internen Reserveblöcken einer SD-Karte "
        "ist keine forensische Garantie für physisch nicht mehr auslesbare NAND-Zellen möglich.</p>";

    if (sdFormatBackendSupported()) {
        html +=
            "<form id='sdSecureForm' method='POST' action='/sd_secure_erase_do'>"
            "<button id='sdSecureButton' class='danger' type='submit'>SECURE ERASE STARTEN</button>"
            "</form>";
    } else {
        html +=
            "<p class='status-pill danger'>Auf diesem Storage-Backend nicht unterstützt</p>";
    }

    html +=
        "</section>"
        "<p id='sdOperationProgress' class='muted' style='display:none'>"
        "SD-Wartung läuft. Aufnahme-Starts und normale SD-Zugriffe sind während des Vorgangs gesperrt. "
        "Bitte Stromversorgung und SD-Karte nicht unterbrechen.</p>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "<style>"
        ".sd-busy-card{width:min(580px,100%);text-align:center;}"
        ".sd-busy-icon{font-size:2rem;line-height:1;margin:4px 0 10px;}"
        ".sd-busy-progress{height:14px;margin:20px 0 8px;background:#e5e9ef;border-radius:999px;overflow:hidden;}"
        ".sd-busy-progress>span{display:block;height:100%;width:0;background:var(--danger);border-radius:999px;"
            "transition:width .25s linear;}"
        ".sd-busy-progress>span.indeterminate{width:34%;transition:none;animation:sdBusyMove 1.15s ease-in-out infinite;}"
        ".sd-busy-note{font-size:.88rem;color:var(--muted);margin-top:10px;}"
        ".sd-secure-metrics{font-size:.9rem;font-variant-numeric:tabular-nums;color:var(--text);margin-top:8px;}"
        ".sd-abort-button{margin-top:18px;width:100%;}"
        "@keyframes sdBusyMove{"
            "0%{transform:translateX(-115%);}"
            "50%{transform:translateX(98%);}"
            "100%{transform:translateX(290%);}"
        "}"
        "</style>"
        "<div id='sdBusyModal' class='modal-backdrop' hidden>"
            "<div class='modal-card sd-busy-card' role='dialog' aria-modal='true' "
                "aria-labelledby='sdBusyTitle' aria-describedby='sdBusyText'>"
                "<span id='sdBusyPill' class='status-pill danger'>SD-WARTUNG</span>"
                "<div class='sd-busy-icon' aria-hidden='true'>&#9888;</div>"
                "<h3 id='sdBusyTitle'>SD-Wartung läuft</h3>"
                "<p id='sdBusyText'>Bitte warten.</p>"
                "<div class='sd-busy-progress' aria-hidden='true'>"
                    "<span id='sdBusyProgressFill' class='indeterminate'></span>"
                "</div>"
                "<div id='sdBusyProgressText' class='sd-secure-metrics'></div>"
                "<div id='sdBusyNote' class='sd-busy-note'>"
                    "Stromversorgung und SD-Karte jetzt nicht unterbrechen."
                "</div>"
                "<button id='sdSecureAbortButton' class='danger sd-abort-button' type='button' hidden>"
                    "ÜBERSCHREIBEN ABBRECHEN &amp; FORMATIEREN"
                "</button>"
            "</div>"
        "</div>"
        "<script>"
        "function sdFmtBytes(v){"
            "v=Number(v||0);"
            "if(v>=1073741824)return (v/1073741824).toFixed(2)+' GB';"
            "return (v/1048576).toFixed(1)+' MB';"
        "}"
        "function sdSetIndeterminate(on){"
            "var f=document.getElementById('sdBusyProgressFill');"
            "if(!f)return;"
            "if(on){f.classList.add('indeterminate');f.style.width='';}"
            "else{f.classList.remove('indeterminate');f.style.transform='none';}"
        "}"
        "function showSdBusy(kind){"
            "var m=document.getElementById('sdBusyModal');"
            "var pill=document.getElementById('sdBusyPill');"
            "var title=document.getElementById('sdBusyTitle');"
            "var text=document.getElementById('sdBusyText');"
            "var note=document.getElementById('sdBusyNote');"
            "var metrics=document.getElementById('sdBusyProgressText');"
            "var abort=document.getElementById('sdSecureAbortButton');"
            "if(!m)return;"
            "if(abort){abort.hidden=true;abort.disabled=false;abort.textContent='ÜBERSCHREIBEN ABBRECHEN & FORMATIEREN';}"
            "if(kind==='format'){"
                "pill.textContent='FORMAT';"
                "title.textContent='SD-Karte wird formatiert';"
                "text.textContent='Das FAT-Dateisystem wird jetzt neu aufgebaut. Nach erfolgreichem Abschluss startet SensorForge automatisch neu.';"
                "note.textContent='Bitte warten. Stromversorgung und SD-Karte jetzt nicht unterbrechen.';"
                "if(metrics)metrics.textContent='Formatierung läuft …';"
                "sdSetIndeterminate(true);"
            "}else if(kind==='secure'){"
                "pill.textContent='SECURE ERASE';"
                "title.textContent='Secure Erase wird vorbereitet';"
                "text.textContent='Dateien werden gelöscht, danach wird der logisch freie Datenbereich mit Nullen überschrieben.';"
                "note.textContent='Danach wird die SD-Karte automatisch formatiert und SensorForge neu gestartet.';"
                "if(metrics)metrics.textContent='Löschbereich wird ermittelt …';"
                "sdSetIndeterminate(false);"
                "var fill=document.getElementById('sdBusyProgressFill');if(fill)fill.style.width='0%';"
            "}"
            "m.hidden=false;"
            "document.body.style.overflow='hidden';"
        "}"
        "function updateSecureProgress(s){"
            "var title=document.getElementById('sdBusyTitle');"
            "var text=document.getElementById('sdBusyText');"
            "var note=document.getElementById('sdBusyNote');"
            "var metrics=document.getElementById('sdBusyProgressText');"
            "var fill=document.getElementById('sdBusyProgressFill');"
            "var abort=document.getElementById('sdSecureAbortButton');"
            "var pct=Math.max(0,Math.min(100,Number(s.percentX10||0)/10));"
            "sdSetIndeterminate(false);"
            "if(fill)fill.style.width=pct.toFixed(1)+'%';"
            "var amount=sdFmtBytes(s.overwrittenBytes)+' / '+sdFmtBytes(s.targetBytes)+' Löschbereich ('+pct.toFixed(1)+' %)';"
            "if(Number(s.totalBytes||0)>0)amount+=' · SD gesamt '+sdFmtBytes(s.totalBytes);"
            "if(metrics)metrics.textContent=amount;"
            "if(s.stage==='overwrite'){"
                "title.textContent='Secure Erase – Überschreiben';"
                "text.textContent='Der logisch freie Datenbereich wird mit Nullen überschrieben.';"
                "note.textContent=s.abortRequested?'Abbruch angefordert. Das Überschreiben wird beendet und anschließend formatiert.':'Mit Abbrechen wird nur das weitere Überschreiben gestoppt; anschließend wird trotzdem formatiert.';"
                "if(abort){abort.hidden=false;abort.disabled=!!s.abortRequested;abort.textContent=s.abortRequested?'ABBRUCH ANGEFORDERT …':'ÜBERSCHREIBEN ABBRECHEN & FORMATIEREN';}"
            "}else if(s.stage==='format'){"
                "title.textContent=s.aborted?'Überschreiben abgebrochen – Formatierung läuft':'Überschreiben abgeschlossen – Formatierung läuft';"
                "text.textContent='Das FAT-Dateisystem wird jetzt neu aufgebaut.';"
                "note.textContent='Stromversorgung und SD-Karte jetzt nicht unterbrechen.';"
                "if(abort)abort.hidden=true;"
            "}else if(s.stage==='restore'){"
                "title.textContent='Konfiguration wird wiederhergestellt';"
                "text.textContent='config.txt wird aus dem internen Flash auf die SD-Karte zurückkopiert und geprüft.';"
                "note.textContent='SensorForge startet danach automatisch neu.';"
                "if(abort)abort.hidden=true;"
            "}"
        "}"
        "var sdSecurePollTimer=0;"
        "function pollSecureStatus(){"
            "fetch('/sd_secure_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){return r.json();})"
            ".then(function(s){"
                "updateSecureProgress(s);"
                "if(s.done){if(s.redirect)location.replace(s.redirect);return;}"
                "sdSecurePollTimer=setTimeout(pollSecureStatus,600);"
            "})"
            ".catch(function(){sdSecurePollTimer=setTimeout(pollSecureStatus,1000);});"
        "}"
        "function armSdForm(formId,buttonId,text,question,busyKind){"
            "var f=document.getElementById(formId);"
            "if(!f)return;"
            "f.addEventListener('submit',function(e){"
                "if(!confirm(question)){e.preventDefault();return;}"
                "var b=document.getElementById(buttonId);"
                "var p=document.getElementById('sdOperationProgress');"
                "if(b){b.disabled=true;b.textContent=text;}"
                "if(busyKind){showSdBusy(busyKind);}else if(p){p.style.display='block';}"
            "});"
        "}"
        "function armSecureErase(){"
            "var f=document.getElementById('sdSecureForm');"
            "var b=document.getElementById('sdSecureButton');"
            "if(!f)return;"
            "f.addEventListener('submit',function(e){"
                "e.preventDefault();"
                "if(!confirm('SECURE ERASE wirklich starten? Der Vorgang kann sehr lange dauern und überschreibt den logischen Datenbereich.'))return;"
                "if(b){b.disabled=true;b.textContent='SECURE ERASE LÄUFT...';}"
                "showSdBusy('secure');"
                "fetch('/sd_secure_erase_do',{method:'POST',cache:'no-store'})"
                ".then(function(r){return r.json();})"
                ".then(function(data){"
                    "if(!data.accepted){location.replace(data.redirect||'/sd_maintenance');return;}"
                    "pollSecureStatus();"
                "})"
                ".catch(function(){pollSecureStatus();});"
            "});"
        "}"
        "var sdAbort=document.getElementById('sdSecureAbortButton');"
        "if(sdAbort){sdAbort.addEventListener('click',function(){"
            "if(!confirm('Überschreiben jetzt abbrechen? Bereits überschriebene Daten bleiben überschrieben; anschließend wird die SD-Karte automatisch formatiert.'))return;"
            "sdAbort.disabled=true;sdAbort.textContent='ABBRUCH ANGEFORDERT …';"
            "fetch('/sd_secure_abort',{method:'POST',cache:'no-store'}).catch(function(){});"
        "});}"
        "armSdForm('sdWipeForm','sdWipeButton','WIPE LÄUFT...',"
            "'SD Wipe wirklich starten? Alle SD-Dateien werden gelöscht; config.txt wird danach aus dem internen Flash wiederhergestellt.','');"
        "armSdForm('sdRealFormatForm','sdRealFormatButton','FORMATIERUNG LÄUFT...',"
            "'SD wirklich neu formatieren? Alle SD-Daten gehen verloren; config.txt wird danach aus dem internen Flash wiederhergestellt.','format');"
        "armSecureErase();"
        "</script>";

    html +=
        htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


// Backward-compatible route name retained for existing bookmarks.
static void handleSDFormat()
{
    handleSDMaintenance();
}


static void handleSDFormatDo()
{
    SdMaintenanceResult result =
        performSdMaintenance(
            SD_MAINT_WIPE
        );

    redirectSdMaintenanceResult(
        SD_MAINT_WIPE,
        result
    );
}


static void handleSDRealFormatDo()
{
    SdMaintenanceResult result =
        performSdMaintenance(
            SD_MAINT_FORMAT
        );

    redirectSdMaintenanceResult(
        SD_MAINT_FORMAT,
        result
    );
}


static const char *secureEraseStageName()
{
    switch (sdSecureJobStage) {
        case SD_SECURE_JOB_OVERWRITE:
            return "overwrite";
        case SD_SECURE_JOB_FORMAT:
            return "format";
        case SD_SECURE_JOB_RESTORE:
            return "restore";
        case SD_SECURE_JOB_DONE:
            return "done";
        default:
            return "idle";
    }
}


static const char *secureEraseRedirectLocation()
{
    if (!sdSecureJobDone)
        return "";

    if (
        sdSecureFinalResult ==
        SD_MAINT_RESULT_OK
    ) {
        return
            sdSecureAborted
            ? "/rebooting?reason=sd_secure_abort"
            : "/rebooting?reason=sd_secure";
    }

    return
        sdMaintenanceResultLocation(
            SD_MAINT_SECURE_ERASE,
            sdSecureFinalResult
        );
}


static void handleSDSecureEraseDo()
{
    SdMaintenanceResult result =
        beginSecureEraseJob();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    if (
        result !=
        SD_MAINT_RESULT_OK
    ) {
        String json =
            "{\"accepted\":false,\"redirect\":\"";

        json +=
            sdMaintenanceResultLocation(
                SD_MAINT_SECURE_ERASE,
                result
            );

        json +=
            "\"}";

        server.send(
            409,
            "application/json; charset=utf-8",
            json
        );
        return;
    }

    server.send(
        202,
        "application/json; charset=utf-8",
        "{\"accepted\":true}"
    );
}


static void handleSDSecureStatus()
{
    uint32_t percentX10 = 0;

    if (sdSecureTargetBytes > 0) {
        uint64_t scaled =
            (
                sdSecureOverwrittenBytes *
                1000ULL
            ) /
            sdSecureTargetBytes;

        if (scaled > 1000ULL)
            scaled = 1000ULL;

        percentX10 =
            (uint32_t)scaled;
    }

    String json;
    json.reserve(320);

    json +=
        "{\"active\":";
    json +=
        sdSecureJobActive ? "true" : "false";
    json +=
        ",\"done\":";
    json +=
        sdSecureJobDone ? "true" : "false";
    json +=
        ",\"stage\":\"";
    json +=
        secureEraseStageName();
    json +=
        "\",\"abortRequested\":";
    json +=
        sdSecureAbortRequested ? "true" : "false";
    json +=
        ",\"aborted\":";
    json +=
        sdSecureAborted ? "true" : "false";
    char u64Text[32];

    json +=
        ",\"overwrittenBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureOverwrittenBytes
    );
    json +=
        u64Text;

    json +=
        ",\"targetBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureTargetBytes
    );
    json +=
        u64Text;

    json +=
        ",\"totalBytes\":";
    snprintf(
        u64Text,
        sizeof(u64Text),
        "%llu",
        (unsigned long long)sdSecureCardTotalBytes
    );
    json +=
        u64Text;
    json +=
        ",\"percentX10\":";
    json +=
        String(percentX10);
    json +=
        ",\"redirect\":\"";
    json +=
        secureEraseRedirectLocation();
    json +=
        "\"}";

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json; charset=utf-8",
        json
    );
}


static void handleSDSecureAbort()
{
    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    if (
        !sdSecureJobActive ||
        sdSecureJobStage !=
            SD_SECURE_JOB_OVERWRITE
    ) {
        server.send(
            409,
            "application/json; charset=utf-8",
            "{\"accepted\":false}"
        );
        return;
    }

    sdSecureAbortRequested =
        true;

    server.send(
        202,
        "application/json; charset=utf-8",
        "{\"accepted\":true}"
    );
}


// -------------------------------------------------------------
// LIVE PREVIEW
// -------------------------------------------------------------

static void handlePreview()
{
    String html = htmlHeader();

    html += "<h2>Live Kamera-Vorschau</h2>";

    if (recorderIsOpen()) {

        // Existing recordings keep priority. The preview does not take over
        // the camera in the middle of an active recording.
        stopCameraPreview();

        html +=
            "<div class='flash-notice error'>"
            "<strong>Aufnahme läuft</strong>"
            "<span class='muted'>Die Live-Vorschau ist während einer laufenden "
            "Aufnahme nicht verfügbar.</span>"
            "</div>";

    } else if (!esp_camera_sensor_get()) {

        stopCameraPreview();
        html += "<p>Camera is not initialized.</p>";

    } else {

        // Raise the preview gate before the first snapshot request so there is
        // no race where motion could start a recording while the page loads.
        noteCameraPreviewActivity();

        html +=
            "<div class='flash-notice' style='border-left-color:var(--accent);background:#eef4ff'>"
            "<strong style='color:#1d4ed8'>Live-Vorschau aktiv</strong>"
            "<span class='muted'>Detection und Recording sind deaktiviert, "
            "solange diese Kameraansicht aktiv ist.</span>"
            "</div>"
            "<img id='cam' style='max-width:100%;'>"
            "<script>"
            "const img=document.getElementById('cam');"
            "let previewStopped=false;"
            "let previewPaused=false;"

            "function releasePreview(){"
                "fetch('/preview_stop',{method:'POST',keepalive:true,cache:'no-store'}).catch(function(){});"
            "}"

            "function stopPreview(){"
                "if(previewStopped)return;"
                "previewStopped=true;"
                "releasePreview();"
            "}"

            "function nextFrame(){"
                "if(previewStopped||previewPaused)return;"
                "img.src='/snapshot?t='+Date.now();"
            "}"

            "img.onload=function(){"
                "setTimeout(nextFrame,200);"
            "};"

            "img.onerror=function(){"
                "setTimeout(nextFrame,500);"
            "};"

            "window.addEventListener('pagehide',stopPreview);"
            "document.addEventListener('visibilitychange',function(){"
                "if(document.hidden){"
                    "previewPaused=true;"
                    "releasePreview();"
                "}else if(!previewStopped){"
                    "previewPaused=false;"
                    "nextFrame();"
                "}"
            "});"

            "nextFrame();"
            "</script>";
    }

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleSnapshot()
{
    // An already-running recording always wins. Normally this cannot happen
    // because the preview gate blocks new starts, but keep the guard for races
    // and direct /snapshot requests.
    if (rejectWhileRecording("camera preview"))
        return;

    if (!esp_camera_sensor_get()) {
        stopCameraPreview();
        server.send(
            503,
            "text/plain; charset=utf-8",
            "Camera is not initialized"
        );
        return;
    }

    noteCameraPreviewActivity();

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            "Camera capture failed"
        );
        return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");

    server.client().write(
        fb->buf,
        fb->len
    );

    esp_camera_fb_return(fb);
}


static void handlePreviewStop()
{
    stopCameraPreview();

    server.send(
        204,
        "text/plain",
        ""
    );
}


// -------------------------------------------------------------
// LD2410S RADAR CONFIG
// -------------------------------------------------------------

static String radarRateText(
    uint32_t rateX10
)
{
    return
        String(rateX10 / 10U) +
        "." +
        String(rateX10 % 10U);
}


static void appendRadarRateOptions(
    String &html,
    uint32_t selected
)
{
    for (
        uint32_t rate = 5;
        rate <= 80;
        rate += 5
    ) {

        html +=
            "<option value='" +
            String(rate) +
            "'" +
            String(
                rate == selected
                ? " selected"
                : ""
            ) +
            ">" +
            radarRateText(rate) +
            " Hz</option>";
    }
}


static void handleRadarLive()
{
    // This endpoint only returns values already cached in RAM by radarLoop().
    // It does not enter configuration mode and does not touch the SD card.
    //
    // The Radar Config page refreshes this endpoint very frequently. Use that
    // existing traffic as a robust pause-lease heartbeat while the page is
    // actually visible. This avoids a false automatic resume if the browser
    // delays the separate 10-second keepalive behind the live radar requests.
    if (
        recordingAutomationPaused &&
        server.hasArg("visible") &&
        server.arg("visible") == "1"
    ) {
        renewRecordingPauseLease();
    }

    String json;

    json.reserve(
        512
    );

    json +=
        "{\"recent\":";

    json +=
        radarGateEnergyIsRecent()
        ? "true"
        : "false";

    json +=
        ",\"state\":" +
        String(
            radarLastTargetState()
        );

    json +=
        ",\"distance\":" +
        String(
            radarLastTargetDistanceCm()
        );

    json +=
        ",\"motion\":";

    json +=
        radarMotionActive()
        ? "true"
        : "false";

    json +=
        ",\"remaining_ms\":" +
        String(
            radarMotionRemainingMs()
        );

    json +=
        ",\"last_gate\":" +
        String(
            radarLastMotionGate()
        );

    json +=
        ",\"energy\":[";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        if (gate > 0) {
            json += ',';
        }

        json +=
            String(
                radarGateEnergyDb(
                    gate
                ),
                1
            );
    }


    json +=
        "]}";


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "application/json",
        json
    );
}


static void handleRadarConfig()
{
    // Opening/navigating to Radar Config is explicit operator activity. If the
    // recording automation is paused, refresh its lease immediately before the
    // synchronous UART settings read below.
    if (recordingAutomationPaused)
        renewRecordingPauseLease();

    RadarSettings settings;
    String error;

    bool recordingActive =
        recorderIsOpen();

    bool readOk =
        false;


    // While recording, do not interrupt normal radar reports just
    // to render this page. Use the settings cached at startup or
    // after the most recent successful write.
    if (recordingActive) {

        readOk =
            radarGetCachedSettings(
                settings
            );

        if (!readOk) {
            error =
                "no cached radar settings available while recording";
        }

    } else {

        readOk =
            radarReadSettings(
                settings,
                error
            );
    }


    String html =
        htmlHeader();

    html +=
        "<h2>Radar Config - HLK-LD2410S</h2>";


    if (recordingActive) {
        html +=
            "<p style='color:#9a5a00'><b>Aufnahme laeuft:</b> "
            "Die Seite verwendet gecachte Radar-Einstellungen. "
            "Live-Energien laufen weiter. Speichern ist erlaubt; "
            "waehrend des kurzen UART-Schreibvorgangs kann es zu einer "
            "kleinen Luecke zwischen Videoframes kommen.</p>";
    }


    html +=
        "<p><b>OT2 presence:</b> " +
        String(
            digitalRead(PIR_PIN) == HIGH
            ? "PERSON PRESENT"
            : "clear"
        ) +
        "</p>";


    if (radarReportIsRecent()) {

        html +=
            "<p><b>UART live report:</b> state=" +
            String(
                radarLastTargetState()
            ) +
            ", distance=" +
            String(
                radarLastTargetDistanceCm()
            ) +
            " cm</p>";
    }


    html +=
        "<p><b>ESP motion trigger (2 s):</b> " +
        String(
            radarMotionActive()
            ? "ACTIVE"
            : "clear"
        );

    if (radarLastMotionGate() >= 0) {
        html +=
            " &nbsp; last gate=" +
            String(
                radarLastMotionGate()
            ) +
            ", energy=" +
            String(
                radarLastMotionEnergyDb(),
                1
            ) +
            " dB";
    }

    html +=
        "</p>";


    html +=
        "<p><b>UART wiring:</b> "
        "LD2410S OT1/TX &rarr; ESP RX GPIO " +
        String(radarRxPin()) +
        ", LD2410S RX &larr; ESP TX GPIO " +
        String(radarTxPin()) +
        "</p>";


    if (!readOk) {

        html +=
            "<p style='color:#b00020'><b>Sensor/UART nicht erreichbar:</b> " +
            htmlEscape(error) +
            "</p>";

        html +=
            "<p>Pruefe 3.3V, GND und die gekreuzten TX/RX-Leitungen. "
            "OT2 allein reicht fuer die Aufnahme, aber nicht fuer Radar Config.</p>";

        html +=
            "<br><a href='/radar_config'><button>Retry</button></a>";

        html +=
            "<a href='/'><button>Back</button></a>";

        html +=
            htmlFooter();

        server.send(
            200,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    if (
        server.hasArg("saved") &&
        server.arg("saved") == "1"
    ) {

        html +=
            "<p style='color:#087a00'><b>Gespeichert und erfolgreich zurueckgelesen.</b></p>";
    }


    if (
        server.hasArg("defaults") &&
        server.arg("defaults") == "1"
    ) {

        html +=
            "<div class='flash-notice'>"
            "<strong>Hi-Link Standardwerte wiederhergestellt</strong>"
            "<span class='muted'>Die Radar-Parameter wurden geschrieben und erfolgreich zurueckgelesen.</span>"
            "</div>";
    }


    html +=
        "<h3>Live Gate Energy</h3>"
        "<p>Aktualisierung ca. 4x pro Sekunde. "
        "Energie und Trigger-Schwelle benutzen dieselbe dB-Skala. "
        "Positiver Margin bedeutet: Gate liegt ueber der Trigger-Schwelle.</p>";

    html +=
        "<p><b>Live status:</b> "
        "<span id='radarLiveStatus'>waiting...</span>"
        " &nbsp; <b>Motion:</b> "
        "<span id='radarLiveMotion'>-</span>"
        "</p>";

    html +=
        "<div style='overflow-x:auto'>"
        "<table style='border-collapse:collapse'>"
        "<tr>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>ca. Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Energy dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Margin</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Status</th>"
        "</tr>";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        bool inConfiguredRange =
            gate >= settings.minGate &&
            gate <= settings.maxGate;

        html +=
            "<tr id='liveRow" +
            String(gate) +
            "'" +
            String(
                inConfiguredRange
                ? ""
                : " style='opacity:0.45'"
            ) +
            ">"
            "<td style='padding:4px;text-align:center'>" +
            String(gate) +
            "</td>"
            "<td style='padding:4px;text-align:center'>" +
            String(
                gate * 0.7f,
                1
            ) +
            " m</td>"
            "<td id='liveEnergy" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveThreshold" +
            String(gate) +
            "' style='padding:4px;text-align:right'>" +
            String(
                settings.triggerThreshold[
                    gate
                ]
            ) +
            "</td>"
            "<td id='liveMargin" +
            String(gate) +
            "' style='padding:4px;text-align:right'>-</td>"
            "<td id='liveState" +
            String(gate) +
            "' style='padding:4px'>-</td>"
            "</tr>";
    }


    html +=
        "</table></div>";


    html +=
        "<script>"
        "(function(){"
        "var busy=false;"
        "function updateRadarLive(){"
            "if(busy)return;"
            "busy=true;"
            "fetch('/radar_live?visible='+(document.hidden?'0':'1')+'&t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
            ".then(function(d){"
                "var s=document.getElementById('radarLiveStatus');"
                "var m=document.getElementById('radarLiveMotion');"
                "if(!d.recent){"
                    "s.textContent='no recent standard report';"
                "}else{"
                    "s.textContent='state='+d.state+', distance='+d.distance+' cm';"
                "}"
                "m.textContent=d.motion?'ACTIVE ('+d.remaining_ms+' ms)':'clear';"
                "for(var i=0;i<16;i++){"
                    "var e=document.getElementById('liveEnergy'+i);"
                    "var t=document.getElementById('liveThreshold'+i);"
                    "var g=document.getElementById('liveMargin'+i);"
                    "var q=document.getElementById('liveState'+i);"
                    "if(!e||!t||!g||!q)continue;"
                    "if(!d.recent){"
                        "e.textContent='-';g.textContent='-';q.textContent='-';"
                        "continue;"
                    "}"
                    "var energy=Number(d.energy[i]);"
                    "var threshold=Number(t.textContent);"
                    "var margin=energy-threshold;"
                    "e.textContent=energy.toFixed(1);"
                    "g.textContent=(margin>=0?'+':'')+margin.toFixed(1);"
                    "q.textContent=margin>=0?'TRIGGER':'clear';"
                    "q.style.fontWeight=margin>=0?'bold':'normal';"
                "}"
            "})"
            ".catch(function(){"
                "var s=document.getElementById('radarLiveStatus');"
                "if(s)s.textContent='live read failed';"
            "})"
            ".then(function(){busy=false;});"
        "}"
        "updateRadarLive();"
        "setInterval(updateRadarLive,250);"
        "})();"
        "</script>";


    html +=
        "<form method='POST' action='/radar_config_save'>";

    html +=
        "<h3>Allgemeine Parameter</h3>";

    html +=
        "Min. distance gate: "
        "<input type='number' name='min_gate' min='0' max='16' value='" +
        String(settings.minGate) +
        "' style='width:80px;'> "
        "(ca. " +
        String(
            settings.minGate * 0.7f,
            1
        ) +
        " m)<br>";

    html +=
        "Max. distance gate: "
        "<input type='number' name='max_gate' min='1' max='16' value='" +
        String(settings.maxGate) +
        "' style='width:80px;'> "
        "(ca. " +
        String(
            settings.maxGate * 0.7f,
            1
        ) +
        " m)<br>";

    html +=
        "No-person delay: "
        "<input type='number' name='absence_sec' min='10' max='120' value='" +
        String(settings.absenceSec) +
        "' style='width:80px;'> s<br>";


    html +=
        "Status report rate: "
        "<select name='status_rate'>";
    appendRadarRateOptions(
        html,
        settings.statusRateX10
    );
    html +=
        "</select><br>";


    html +=
        "Distance report rate: "
        "<select name='distance_rate'>";
    appendRadarRateOptions(
        html,
        settings.distanceRateX10
    );
    html +=
        "</select><br>";


    html +=
        "Response speed: "
        "<select name='response_speed'>";

    html +=
        "<option value='5'" +
        String(
            settings.responseSpeed == 5
            ? " selected"
            : ""
        ) +
        ">normal</option>";

    html +=
        "<option value='10'" +
        String(
            settings.responseSpeed == 10
            ? " selected"
            : ""
        ) +
        ">fast</option>";

    html +=
        "</select><br>";


    html +=
        "<h3>Distance Gates / Thresholds</h3>";

    html +=
        "<p>Niedrigerer dB-Wert = empfindlicher. "
        "Die Hi-Link-Protokollbeispiele enthalten auch Werte unter 10; "
        "daher erlaubt diese Seite 0..95 und bewahrt vorhandene Werte.</p>";

    html +=
        "<div style='overflow-x:auto'>"
        "<table style='border-collapse:collapse'>"
        "<tr>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Gate</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>ca. Distanz</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Trigger dB</th>"
        "<th style='padding:5px;border-bottom:1px solid #aaa'>Hold dB</th>"
        "</tr>";


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        html +=
            "<tr>"
            "<td style='padding:4px;text-align:center'>" +
            String(gate) +
            "</td>"
            "<td style='padding:4px;text-align:center'>" +
            String(
                gate * 0.7f,
                1
            ) +
            " m</td>"
            "<td style='padding:4px'>"
            "<input type='number' min='0' max='95' "
            "name='trigger_" +
            String(gate) +
            "' value='" +
            String(
                settings.triggerThreshold[gate]
            ) +
            "' style='width:70px'></td>"
            "<td style='padding:4px'>"
            "<input type='number' min='0' max='95' "
            "name='hold_" +
            String(gate) +
            "' value='" +
            String(
                settings.holdThreshold[gate]
            ) +
            "' style='width:70px'></td>"
            "</tr>";
    }


    html +=
        "</table></div>";

    html +=
        "<br><button type='submit'>Write + Verify</button>";

    html +=
        "</form>";


    html +=
        "<div class='settings-section' style='margin-top:18px'>"
        "<h3>Standardwerte</h3>"
        "<p class='muted'>Falls die Radar-Parameter unbrauchbar verstellt wurden, können hier "
        "die Hi-Link-Standardwerte wiederhergestellt werden. Die Werte werden direkt in den "
        "LD2410S geschrieben und anschließend verifiziert.</p>"
        "<form method='POST' action='/radar_config_defaults' "
        "onsubmit=\"return confirm('Alle aktuellen Radar-Einstellungen werden mit den Hi-Link-Standardwerten überschrieben. Fortfahren?');\">"
        "<button type='submit' style='background:#fff1d6;color:#7a4700;border:1px solid #e3b96a'>"
        "Standardwerte wiederherstellen</button>"
        "</form>"
        "</div>";


    html +=
        "<br><a href='/radar_config'><button>Read again</button></a>";

    html +=
        "<a href='/'><button>Back</button></a>";

    html +=
        htmlFooter();


    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleRadarConfigSave()
{
    bool recordingActive =
        recorderIsOpen();


    RadarSettings settings;

    settings.minGate =
        (uint32_t)(
            server.arg(
                "min_gate"
            ).toInt()
        );

    settings.maxGate =
        (uint32_t)(
            server.arg(
                "max_gate"
            ).toInt()
        );

    settings.absenceSec =
        (uint32_t)(
            server.arg(
                "absence_sec"
            ).toInt()
        );

    settings.statusRateX10 =
        (uint32_t)(
            server.arg(
                "status_rate"
            ).toInt()
        );

    settings.distanceRateX10 =
        (uint32_t)(
            server.arg(
                "distance_rate"
            ).toInt()
        );

    settings.responseSpeed =
        (uint32_t)(
            server.arg(
                "response_speed"
            ).toInt()
        );


    for (
        uint8_t gate = 0;
        gate < 16;
        ++gate
    ) {

        settings.triggerThreshold[gate] =
            (uint32_t)(
                server.arg(
                    "trigger_" +
                    String(gate)
                ).toInt()
            );

        settings.holdThreshold[gate] =
            (uint32_t)(
                server.arg(
                    "hold_" +
                    String(gate)
                ).toInt()
            );
    }


    String error;

    bool writeOk =
        radarWriteSettings(
            settings,
            error
        );


    // The UART configuration transaction is synchronous and temporarily
    // interrupts normal standard reports. If recording was already active,
    // keep the ESP-side motion state alive after the transaction so the
    // normal post-record timer cannot expire merely because of WebConfig.
    if (recordingActive) {
        radarHoldMotion(
            2500UL
        );
    }


    if (!writeOk) {

        String html =
            htmlHeader();

        html +=
            "<h2>Radar Config Error</h2>"
            "<p style='color:#b00020'>" +
            htmlEscape(error) +
            "</p>"
            "<a href='/radar_config'><button>Back</button></a>";

        html +=
            htmlFooter();

        server.send(
            400,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    Serial.println(
        "LD2410S config written and verified"
    );

    logWrite(
        "LD2410S config written and verified"
    );


    server.sendHeader(
        "Location",
        "/radar_config?saved=1"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleRadarConfigDefaults()
{
    bool recordingActive =
        recorderIsOpen();


    RadarSettings settings;

    radarGetHiLinkDefaultSettings(
        settings
    );


    String error;

    bool writeOk =
        radarWriteSettings(
            settings,
            error
        );


    // Match the normal Radar Config save path: if a recording was already
    // running, keep the ESP-side motion state alive across the short UART
    // configuration transaction.
    if (recordingActive) {
        radarHoldMotion(
            2500UL
        );
    }


    if (!writeOk) {

        String html =
            htmlHeader();

        html +=
            "<h2>Radar Standardwerte</h2>"
            "<div class='flash-notice error'>"
            "<strong>Wiederherstellung fehlgeschlagen</strong>"
            "<span class='muted'>" +
            htmlEscape(error) +
            "</span></div>"
            "<a href='/radar_config'><button>Zurück</button></a>";

        html +=
            htmlFooter();

        server.send(
            400,
            "text/html; charset=utf-8",
            html
        );

        return;
    }


    Serial.println(
        "LD2410S Hi-Link defaults written and verified"
    );

    logWrite(
        "LD2410S Hi-Link defaults written and verified"
    );


    server.sendHeader(
        "Location",
        "/radar_config?defaults=1"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// REBOOT
// -------------------------------------------------------------

static String rebootRecordingBlockedModalHtml()
{
    return
        "<div id='rebootBlockedModal' class='modal-backdrop'>"
        "<div class='modal-card' role='dialog' aria-modal='true' "
        "aria-labelledby='rebootBlockedTitle'>"
        "<span class='status-pill danger'>AUFNAHME AKTIV</span>"
        "<h3 id='rebootBlockedTitle'>Neustart momentan gesperrt</h3>"
        "<p>Eine Aufnahme läuft gerade. Zum Schutz der laufenden Videodatei "
        "und der SD-Karte ist ein Neustart vorübergehend deaktiviert.</p>"
        "<p>Dieses Fenster verschwindet automatisch, sobald die Aufnahme beendet ist.</p>"
        "<div class='modal-actions'>"
        "<a class='button' href='/'>Zur Übersicht</a>"
        "</div>"
        "<div id='rebootBlockedState' class='modal-state'>Warte auf Aufnahmeende ...</div>"
        "</div></div>"
        "<script>"
        "(function(){"
            "var modal=document.getElementById('rebootBlockedModal');"
            "var state=document.getElementById('rebootBlockedState');"
            "function poll(){"
                "fetch('/ui_status?ts='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();})"
                ".then(function(s){"
                    "if(!s.recorder_open){"
                        "if(state)state.textContent='Aufnahme beendet - Neustart wieder verfügbar.';"
                        "setTimeout(function(){if(modal)modal.hidden=true;},450);"
                        "return;"
                    "}"
                    "setTimeout(poll,1000);"
                "})"
                ".catch(function(){setTimeout(poll,1500);});"
            "}"
            "setTimeout(poll,800);"
        "})();"
        "</script>";
}


static void sendRebootPage(
    int statusCode
)
{
    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Reboot</h2>"
        "<p>SensorForge kontrolliert neu starten</p>"
        "</div></div>"
        "<div class='settings-section'>"
        "<h3>System neu starten</h3>"
        "<p class='muted'>Ein Neustart beendet die aktuelle Sitzung und startet das Board anschließend neu.</p>"
        "<form method='POST' action='/reboot_do'>"
        "<button class='danger' type='submit'>Jetzt neu starten</button>"
        "<a class='button' href='/'>Abbrechen</a>"
        "</form>"
        "</div>";

    if (recorderIsOpen()) {
        html +=
            rebootRecordingBlockedModalHtml();
    }

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        statusCode,
        "text/html; charset=utf-8",
        html
    );
}


static void handleReboot()
{
    sendRebootPage(
        200
    );
}


static void handleRebooting()
{
    String html =
        htmlHeader();

    String reason =
        server.arg("reason");

    bool afterSdFormat =
        reason == "sd_format";

    bool afterSecureErase =
        reason == "sd_secure";

    bool afterSecureAbort =
        reason == "sd_secure_abort";

    html +=
        "<div class='operation-card'>"
        "<span class='status-pill warn'>SYSTEM RESTART</span>";

    if (afterSdFormat) {
        html +=
            "<h2 style='margin-top:16px'>SD Format abgeschlossen</h2>"
            "<p class='muted'>Das Dateisystem und <code>/config.txt</code> wurden wiederhergestellt. "
            "SensorForge startet jetzt automatisch neu, damit alle Storage-Komponenten mit einem "
            "frischen SD-Mount weiterarbeiten.</p>";

    } else if (afterSecureErase) {
        html +=
            "<h2 style='margin-top:16px'>Secure Erase abgeschlossen</h2>"
            "<p class='muted'>Secure Erase, Neuformatierung und Config-Wiederherstellung sind abgeschlossen. "
            "SensorForge startet jetzt automatisch neu, damit alle Storage-Komponenten mit einem "
            "frischen SD-Mount weiterarbeiten.</p>";

    } else if (afterSecureAbort) {
        html +=
            "<h2 style='margin-top:16px'>Secure Erase abgebrochen</h2>"
            "<p class='muted'>Das weitere Überschreiben wurde auf Wunsch beendet. Die SD-Karte wurde anschließend "
            "neu formatiert und <code>/config.txt</code> wiederhergestellt. SensorForge startet jetzt automatisch neu.</p>";

    } else {
        html +=
            "<h2 style='margin-top:16px'>SensorForge wird neu gestartet</h2>"
            "<p class='muted'>Die Verbindung zum Fabric Node wird kurz unterbrochen. "
            "Danach öffnet sich automatisch wieder die Übersicht.</p>";
    }

    html +=
        "<div id='rebootCountdown' class='countdown'>10</div>"
        "<div class='muted'>Sekunden bis zur Rückkehr</div>"
        "</div>"
        "<script>"
        // Make a manual refresh safe immediately. The current document stays
        // visible, but the browser URL/history already points to the dashboard.
        "history.replaceState(null,'','/');"
        "(function(){"
            "var seconds=10;"
            "var el=document.getElementById('rebootCountdown');"
            "var timer=setInterval(function(){"
                "seconds--;"
                "if(seconds<0)seconds=0;"
                "if(el)el.textContent=seconds;"
                "if(seconds===0){clearInterval(timer);tryHome();}"
            "},1000);"
            "function tryHome(){"
                "fetch('/?reconnect='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);location.replace('/');})"
                ".catch(function(){setTimeout(tryHome,1000);});"
            "}"
        "})();"
        "</script>";

    html +=
        htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleRebootDo()
{
    // Keep the server-side guard even though /reboot already shows the
    // recording modal. The recorder may start in the short interval between
    // opening the page and pressing the reboot button.
    if (recorderIsOpen()) {
        sendRebootPage(
            409
        );
        return;
    }

    // POST/Redirect/GET prevents browser refresh from repeating the reboot POST.
    // Three seconds are intentionally left before restart so the browser can
    // fetch and render /rebooting first, even on a slightly slow WiFi link.
    rebootScheduled =
        true;

    rebootAtMs =
        millis() + 3000UL;

    server.sendHeader(
        "Location",
        "/rebooting"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// LOG VIEWER
// -------------------------------------------------------------
static String activeWebLogPath()
{
    return
        cfg_log_file.length()
        ? cfg_log_file
        : String("/log.txt");
}


static void handleLogRaw()
{
    if (rejectWhileRecording("log download"))
        return;

    logFlush();

    String logPath =
        activeWebLogPath();

    File f = STORAGE.open(
        logPath.c_str(),
        FILE_READ
    );

    if (!f) {
        server.send(
            404,
            "text/plain; charset=utf-8",
            "Logdatei nicht gefunden: " + logPath
        );
        return;
    }

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    if (
        server.hasArg("download") &&
        server.arg("download") == "1"
    ) {
        int slashPos =
            logPath.lastIndexOf('/');

        String downloadName =
            slashPos >= 0
            ? logPath.substring(slashPos + 1)
            : logPath;

        if (!downloadName.length())
            downloadName = "sensorforge.log";

        downloadName.replace("\"", "_");
        downloadName.replace("\r", "_");
        downloadName.replace("\n", "_");

        server.sendHeader(
            "Content-Disposition",
            "attachment; filename=\"" +
            downloadName +
            "\""
        );
    }

    server.streamFile(
        f,
        "text/plain; charset=utf-8"
    );

    f.close();
}


static void handleLog()
{
    if (rejectWhileRecording("log viewer"))
        return;

    // Ensure the viewer sees all messages that have already been logged.
    logFlush();

    String logPath =
        activeWebLogPath();

    File f = STORAGE.open(
        logPath.c_str(),
        FILE_READ
    );

    if (!f) {
        String html = htmlHeader();

        html +=
            "<div class='page-title'><div>"
            "<h2>Log Viewer</h2>"
            "<p>SensorForge System- und Ereignisprotokoll</p>"
            "</div></div>"
            "<div class='flash-notice error'>"
            "<strong>Logdatei nicht gefunden</strong>"
            "<span class='muted'><code>" +
            htmlEscape(logPath) +
            "</code></span></div>";

        html += htmlFooter();

        server.send(
            404,
            "text/html; charset=utf-8",
            html
        );
        return;
    }

    uint64_t logSize =
        f.size();

    f.close();

    String sizeText;

    if (logSize >= 1024ULL * 1024ULL) {
        sizeText =
            String(
                (double)logSize /
                (1024.0 * 1024.0),
                1
            ) +
            " MB";
    } else if (logSize >= 1024ULL) {
        sizeText =
            String(
                (double)logSize / 1024.0,
                1
            ) +
            " KB";
    } else {
        sizeText =
            String((unsigned long)logSize) +
            " B";
    }

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Log Viewer</h2>"
        "<p>SensorForge System- und Ereignisprotokoll</p>"
        "</div></div>"
        "<div class='log-toolbar'>"
        "<button id='logReload' class='primary' type='button'>Neu laden</button>"
        "<button id='logBottom' type='button'>Zum Ende</button>"
        "<a class='button' href='/log_raw?download=1'>Download</a>"
        "<span id='logStatus' class='log-status'>Bereit</span>"
        "</div>"
        "<div class='log-meta'>"
        "<span>Datei: <code>" +
        htmlEscape(logPath) +
        "</code></span>"
        "<span>Größe: " +
        htmlEscape(sizeText) +
        "</span>"
        "</div>"
        "<div id='logTerminal' class='log-terminal'>"
        "<pre id='logOutput' class='log-view'>Log wird geladen...</pre>"
        "</div>"
        "<script>"
        "(function(){"
        "var out=document.getElementById('logOutput');"
        "var term=document.getElementById('logTerminal');"
        "var status=document.getElementById('logStatus');"
        "var reload=document.getElementById('logReload');"
        "var bottom=document.getElementById('logBottom');"
        "function scrollBottom(){term.scrollTop=term.scrollHeight;}"
        "function loadLog(){"
            "reload.disabled=true;"
            "status.textContent='Lade Log...';"
            "fetch('/log_raw?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})"
            ".then(function(text){"
                "out.textContent=text.length?text:'(Log ist leer)';"
                "status.textContent='Aktualisiert';"
                "scrollBottom();"
            "})"
            ".catch(function(err){"
                "out.textContent='Log konnte nicht geladen werden: '+err.message;"
                "status.textContent='Fehler';"
            "})"
            ".then(function(){reload.disabled=false;});"
        "}"
        "reload.addEventListener('click',loadLog);"
        "bottom.addEventListener('click',scrollBottom);"
        "loadLog();"
        "})();"
        "</script>";

    html += htmlFooter();

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


// -------------------------------------------------------------
// RECORDING BROWSER
//
// Important:
// /files only scans the SD root and therefore stays fast.
// A day's recordings are loaded only when that day is opened.
// -------------------------------------------------------------

struct RecordingEntry {
    String name;
    String fullPath;
    uint64_t size;
    bool isMkv;
    bool hasSrt;
};


static bool isDateFolderName(const String &name)
{
    if (name.length() != 8)
        return false;

    for (size_t i = 0; i < 8; ++i) {
        if (!isDigit(name[i]))
            return false;
    }

    return true;
}


static String displayDateFolder(const String &folderName)
{
    if (!isDateFolderName(folderName))
        return folderName;

    return
        folderName.substring(6, 8) + "." +
        folderName.substring(4, 6) + "." +
        folderName.substring(0, 4);
}


static String displayRecordingTime(const String &fileName)
{
    int dotPos =
        fileName.lastIndexOf('.');

    String baseName =
        dotPos >= 0
        ? fileName.substring(0, dotPos)
        : fileName;

    if (baseName.length() == 6) {

        bool numeric = true;

        for (size_t i = 0; i < 6; ++i) {
            if (!isDigit(baseName[i])) {
                numeric = false;
                break;
            }
        }

        if (numeric) {
            return
                baseName.substring(0, 2) + ":" +
                baseName.substring(2, 4) + ":" +
                baseName.substring(4, 6);
        }
    }

    return baseName;
}


static String formatFileSize(uint64_t bytes)
{
    if (bytes >= 1024ULL * 1024ULL) {

        float mb =
            (float)bytes /
            (1024.0f * 1024.0f);

        return String(mb, 1) + " MB";
    }

    if (bytes >= 1024ULL) {

        float kb =
            (float)bytes /
            1024.0f;

        return String(kb, 0) + " KB";
    }

    return
        String((unsigned long)bytes) +
        " B";
}


static bool recordingNameDescending(
    const RecordingEntry &a,
    const RecordingEntry &b
)
{
    return
        a.name.compareTo(b.name) > 0;
}


static bool stringDescending(
    const String &a,
    const String &b
)
{
    return
        a.compareTo(b) > 0;
}


static bool hasMatchingSrt(
    const String &videoName,
    const std::vector<String> &srtBaseNames
)
{
    if (videoName.length() < 5)
        return false;

    String base =
        videoName.substring(
            0,
            videoName.length() - 4
        );

    base.toLowerCase();

    for (
        const String &srtBase :
        srtBaseNames
    ) {
        if (base == srtBase)
            return true;
    }

    return false;
}


// -------------------------------------------------------------
// Return one day's recordings as an HTML fragment.
//
// This handler intentionally streams rows instead of building
// one huge HTML String in RAM.
// -------------------------------------------------------------

static void handleFilesDay()
{
    if (recorderIsOpen()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "recording_active"
        );
        return;
    }

    String day =
        server.arg("day");

    bool validDay =
        isDateFolderName(day) ||
        day == "fallback";

    if (!validDay) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    std::vector<RecordingEntry> recordings;
    std::vector<String> srtBaseNames;

    uint16_t scannedEntries =
        0;

    File file =
        root.openNextFile();


    while (file) {

        if (!file.isDirectory()) {

            String name =
                String(file.name());

            int slashPos =
                name.lastIndexOf('/');

            if (slashPos >= 0) {
                name =
                    name.substring(
                        slashPos + 1
                    );
            }


            String lowerName =
                name;

            lowerName.toLowerCase();


            if (lowerName.endsWith(".srt")) {

                srtBaseNames.push_back(
                    lowerName.substring(
                        0,
                        lowerName.length() - 4
                    )
                );

            } else {

                bool isAvi =
                    lowerName.endsWith(".avi");

                bool isMkv =
                    lowerName.endsWith(".mkv");


                if (isAvi || isMkv) {

                    RecordingEntry entry;

                    entry.name =
                        name;

                    entry.fullPath =
                        folderPath +
                        "/" +
                        name;

                    entry.size =
                        file.size();

                    entry.isMkv =
                        isMkv;

                    entry.hasSrt =
                        false;


                    recordings.push_back(
                        entry
                    );
                }
            }
        }


        file.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        file =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    // Match SRT files without performing another SD open/exists()
    // for every AVI file.
    uint16_t matchedEntries =
        0;

    for (
        RecordingEntry &entry :
        recordings
    ) {

        matchedEntries++;

        if ((matchedEntries & 0x1FU) == 0)
            serviceWebLongOperation();

        if (!entry.isMkv) {
            entry.hasSrt =
                hasMatchingSrt(
                    entry.name,
                    srtBaseNames
                );
        }
    }


    std::sort(
        recordings.begin(),
        recordings.end(),
        recordingNameDescending
    );


    // Stream the response. This keeps heap use low even when
    // a day contains many recordings. Never let the browser HTTP-cache
    // this fragment; the optional session cache is managed explicitly.
    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.setContentLength(
        CONTENT_LENGTH_UNKNOWN
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        ""
    );


    if (recordings.empty()) {

        server.sendContent(
            "<div class='recording'>"
            "Keine Aufnahmen an diesem Tag."
            "</div>"
        );

        return;
    }


    for (
        const RecordingEntry &entry :
        recordings
    ) {

        String row;
        row.reserve(512);


        row +=
            "<div class='recording'>";


        row +=
            "<span class='recname'>";

        row +=
            htmlEscape(
                displayRecordingTime(
                    entry.name
                )
            );

        row +=
            "</span>";


        row +=
            "<span class='recmeta'>";

        row +=
            entry.isMkv
            ? "MKV"
            : "AVI";

        row +=
            " &nbsp; ";

        row +=
            formatFileSize(
                entry.size
            );

        row +=
            "</span>";


        // AVI and MKV use the same WebPlayer UI.
        row +=
            "<a href='/play?path=";

        row +=
            urlEncode(
                entry.fullPath
            );

        row +=
            "'><button>Play</button></a>";


        row +=
            "<a href='/file?path=";

        row +=
            urlEncode(
                entry.fullPath
            );

        row +=
            "'><button>Download</button></a>";


        if (
            !entry.isMkv &&
            entry.hasSrt
        ) {

            String srtPath =
                entry.fullPath.substring(
                    0,
                    entry.fullPath.length() - 4
                ) +
                ".srt";


            row +=
                "<a href='/file?path=";

            row +=
                urlEncode(
                    srtPath
                );

            row +=
                "'><button>SRT</button></a>";
        }


        row +=
            "</div>";


        server.sendContent(
            row
        );
    }
}


// -------------------------------------------------------------
// DOWNLOAD ALL RECORDINGS OF ONE DAY AS STREAMING ZIP
//
// The ZIP is generated directly while sending it to the browser.
// No complete archive is created in RAM or on the SD card.
//
// Compression method: STORE (no compression). AVI/MKV already
// contain JPEG-compressed image data, so recompression would waste
// CPU without meaningfully reducing size.
//
// ZIP32 is intentionally used for simplicity and compatibility.
// A single file / complete archive >= 4 GB is rejected cleanly.
// -------------------------------------------------------------

struct DayZipEntry {
    String name;
    uint32_t size;
    uint32_t crc32;
    uint32_t localHeaderOffset;
};


static bool isRecordingFileForDayDownload(
    const String &name
)
{
    String lower =
        name;

    lower.toLowerCase();

    return
        lower.endsWith(".avi") ||
        lower.endsWith(".mkv") ||
        lower.endsWith(".srt");
}


static bool zipClientWriteAll(
    const uint8_t *data,
    size_t length
)
{
    size_t offset =
        0;

    uint32_t stalledSince =
        millis();


    while (offset < length) {

        if (!server.client().connected())
            return false;


        size_t written =
            server.client().write(
                data + offset,
                length - offset
            );


        if (written > 0) {

            offset +=
                written;

            stalledSince =
                millis();

        } else {

            if (
                millis() -
                stalledSince >
                5000UL
            ) {
                return false;
            }

            delay(1);
        }


        // The main task is subscribed to our task watchdog.
        // A large HTTP download may run much longer than 30 s.
        esp_task_wdt_reset();

        yield();
    }


    return true;
}


static bool zipWriteU16(
    uint16_t value
)
{
    uint8_t data[2] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU)
    };

    return
        zipClientWriteAll(
            data,
            sizeof(data)
        );
}


static bool zipWriteU32(
    uint32_t value
)
{
    uint8_t data[4] = {
        (uint8_t)(value & 0xFFU),
        (uint8_t)((value >> 8) & 0xFFU),
        (uint8_t)((value >> 16) & 0xFFU),
        (uint8_t)((value >> 24) & 0xFFU)
    };

    return
        zipClientWriteAll(
            data,
            sizeof(data)
        );
}


static uint32_t zipCrc32Update(
    uint32_t crc,
    const uint8_t *data,
    size_t length
)
{
    // 16-entry nibble table: much faster than the previous
    // bit-by-bit implementation while using only 64 bytes.
    static const uint32_t table[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
    };


    for (
        size_t i = 0;
        i < length;
        ++i
    ) {

        crc ^=
            data[i];


        crc =
            (crc >> 4) ^
            table[
                crc & 0x0FU
            ];


        crc =
            (crc >> 4) ^
            table[
                crc & 0x0FU
            ];
    }


    return
        crc;
}


static bool zipWriteLocalHeader(
    const DayZipEntry &entry
)
{
    const uint16_t flags =
        0x0808U; // data descriptor + UTF-8 filename

    uint16_t nameLength =
        (uint16_t)entry.name.length();


    return
        zipWriteU32(0x04034B50UL) &&
        zipWriteU16(20) &&
        zipWriteU16(flags) &&
        zipWriteU16(0) &&     // STORE
        zipWriteU16(0) &&     // DOS time
        zipWriteU16(0) &&     // DOS date
        zipWriteU32(0) &&     // CRC follows in descriptor
        zipWriteU32(0) &&
        zipWriteU32(0) &&
        zipWriteU16(nameLength) &&
        zipWriteU16(0) &&
        zipClientWriteAll(
            (const uint8_t *)entry.name.c_str(),
            nameLength
        );
}


static bool zipWriteDataDescriptor(
    const DayZipEntry &entry
)
{
    return
        zipWriteU32(0x08074B50UL) &&
        zipWriteU32(entry.crc32) &&
        zipWriteU32(entry.size) &&
        zipWriteU32(entry.size);
}


static bool zipWriteCentralHeader(
    const DayZipEntry &entry
)
{
    const uint16_t flags =
        0x0808U;

    uint16_t nameLength =
        (uint16_t)entry.name.length();


    return
        zipWriteU32(0x02014B50UL) &&
        zipWriteU16(20) &&     // version made by
        zipWriteU16(20) &&     // version needed
        zipWriteU16(flags) &&
        zipWriteU16(0) &&      // STORE
        zipWriteU16(0) &&
        zipWriteU16(0) &&
        zipWriteU32(entry.crc32) &&
        zipWriteU32(entry.size) &&
        zipWriteU32(entry.size) &&
        zipWriteU16(nameLength) &&
        zipWriteU16(0) &&      // extra
        zipWriteU16(0) &&      // comment
        zipWriteU16(0) &&      // disk
        zipWriteU16(0) &&      // internal attrs
        zipWriteU32(0) &&      // external attrs
        zipWriteU32(
            entry.localHeaderOffset
        ) &&
        zipClientWriteAll(
            (const uint8_t *)entry.name.c_str(),
            nameLength
        );
}


static void handleDownloadDay()
{
    if (rejectWhileRecording("download day archive"))
        return;


    String day =
        server.arg("day");

    day.trim();


    if (
        !isDateFolderName(day) &&
        day != "fallback"
    ) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    // Release any old WebPlayer SD handles before starting a
    // potentially long sequential SD read.
    webPlayerStop();


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    std::vector<DayZipEntry> entries;

    entries.reserve(
        32
    );


    uint64_t totalZipBytes =
        22ULL; // end of central directory

    uint64_t localAreaBytes =
        0;

    uint64_t centralAreaBytes =
        0;


    uint16_t scannedEntries =
        0;

    File file =
        root.openNextFile();


    while (file) {

        if (!file.isDirectory()) {

            String name =
                String(file.name());

            int slashPos =
                name.lastIndexOf('/');


            if (slashPos >= 0) {
                name =
                    name.substring(
                        slashPos + 1
                    );
            }


            if (
                isRecordingFileForDayDownload(
                    name
                )
            ) {

                uint64_t fileSize =
                    file.size();


                if (
                    fileSize >
                    0xFFFFFFFFULL ||
                    name.length() >
                    0xFFFFU
                ) {

                    file.close();
                    root.close();

                    server.send(
                        413,
                        "text/plain; charset=utf-8",
                        "Day archive exceeds ZIP32 limits."
                    );

                    return;
                }


                DayZipEntry entry;

                entry.name =
                    name;

                entry.size =
                    (uint32_t)fileSize;

                entry.crc32 =
                    0;

                entry.localHeaderOffset =
                    0;


                entries.push_back(
                    entry
                );


                localAreaBytes +=
                    30ULL +
                    (uint64_t)name.length() +
                    fileSize +
                    16ULL; // data descriptor


                centralAreaBytes +=
                    46ULL +
                    (uint64_t)name.length();
            }
        }


        file.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        file =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    if (entries.empty()) {

        server.send(
            404,
            "text/plain; charset=utf-8",
            "No recordings found for this day."
        );

        return;
    }


    if (
        entries.size() >
        0xFFFFU
    ) {

        server.send(
            413,
            "text/plain; charset=utf-8",
            "Too many files for ZIP32 archive."
        );

        return;
    }


    totalZipBytes +=
        localAreaBytes +
        centralAreaBytes;


    if (
        totalZipBytes >
        0xFFFFFFFFULL ||
        localAreaBytes >
        0xFFFFFFFFULL ||
        centralAreaBytes >
        0xFFFFFFFFULL
    ) {

        server.send(
            413,
            "text/plain; charset=utf-8",
            "Day archive exceeds ZIP32 4 GB limit."
        );

        return;
    }


    String archiveName =
        "recordings_" +
        day +
        ".zip";


    archiveName.replace(
        "\"",
        "_"
    );

    archiveName.replace(
        "\r",
        "_"
    );

    archiveName.replace(
        "\n",
        "_"
    );


    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" +
        archiveName +
        "\""
    );

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );

    server.setContentLength(
        (size_t)totalZipBytes
    );

    server.send(
        200,
        "application/zip",
        ""
    );


    static const size_t ZIP_BUFFER_SIZE =
        8U * 1024U;

    uint8_t *buffer =
        (uint8_t *)malloc(
            ZIP_BUFFER_SIZE
        );


    if (!buffer) {

        Serial.println(
            "ZIP download: buffer allocation failed"
        );

        logWrite(
            "ZIP download failed: buffer allocation"
        );

        server.client().stop();

        return;
    }


    bool ok =
        true;

    uint32_t outputOffset =
        0;


    for (
        DayZipEntry &entry :
        entries
    ) {

        entry.localHeaderOffset =
            outputOffset;


        if (!zipWriteLocalHeader(
                entry
            )) {

            ok =
                false;

            break;
        }


        outputOffset +=
            30U +
            (uint32_t)entry.name.length();


        String fullPath =
            folderPath +
            "/" +
            entry.name;


        File input =
            STORAGE.open(
                fullPath.c_str(),
                FILE_READ
            );


        if (!input) {

            Serial.println(
                "ZIP download: cannot open " +
                fullPath
            );

            ok =
                false;

            break;
        }


        uint32_t crc =
            0xFFFFFFFFUL;

        uint32_t sentForFile =
            0;


        while (input.available()) {

            size_t got =
                input.read(
                    buffer,
                    ZIP_BUFFER_SIZE
                );


            if (got == 0) {

                ok =
                    false;

                break;
            }


            crc =
                zipCrc32Update(
                    crc,
                    buffer,
                    got
                );


            if (!zipClientWriteAll(
                    buffer,
                    got
                )) {

                ok =
                    false;

                break;
            }


            sentForFile +=
                (uint32_t)got;
        }


        input.close();


        if (
            !ok ||
            sentForFile !=
                entry.size
        ) {

            ok =
                false;

            break;
        }


        entry.crc32 =
            crc ^
            0xFFFFFFFFUL;


        outputOffset +=
            entry.size;


        if (!zipWriteDataDescriptor(
                entry
            )) {

            ok =
                false;

            break;
        }


        outputOffset +=
            16U;
    }


    uint32_t centralOffset =
        outputOffset;

    uint32_t centralSize =
        0;


    if (ok) {

        for (
            const DayZipEntry &entry :
            entries
        ) {

            if (!zipWriteCentralHeader(
                    entry
                )) {

                ok =
                    false;

                break;
            }


            uint32_t headerSize =
                46U +
                (uint32_t)entry.name.length();


            centralSize +=
                headerSize;

            outputOffset +=
                headerSize;
        }
    }


    if (ok) {

        uint16_t entryCount =
            (uint16_t)entries.size();


        ok =
            zipWriteU32(0x06054B50UL) &&
            zipWriteU16(0) &&
            zipWriteU16(0) &&
            zipWriteU16(entryCount) &&
            zipWriteU16(entryCount) &&
            zipWriteU32(centralSize) &&
            zipWriteU32(centralOffset) &&
            zipWriteU16(0);


        outputOffset +=
            22U;
    }


    free(
        buffer
    );


    if (
        !ok ||
        outputOffset !=
            (uint32_t)totalZipBytes
    ) {

        Serial.println(
            "ZIP download interrupted or failed"
        );

        logWrite(
            "ZIP download failed/interrupted: " +
            day
        );

        server.client().stop();

        return;
    }


    Serial.println(
        "ZIP download completed: " +
        archiveName +
        " (" +
        String(entries.size()) +
        " files)"
    );

    logWrite(
        "ZIP download completed: " +
        archiveName +
        " (" +
        String(entries.size()) +
        " files)"
    );
}


// -------------------------------------------------------------
// DELETE ALL RECORDINGS OF ONE DAY
//
// Safety rules:
// - POST only (route registration below)
// - YYYYMMDD or "fallback" only
// - blocked while recording
// - only known recording files are removed
// - unknown files/directories are never touched
// - day directory is removed only when it is really empty
// -------------------------------------------------------------

static bool isRecordingFileForDayDelete(
    const String &name
)
{
    String lower =
        name;

    lower.toLowerCase();

    return
        lower.endsWith(".avi") ||
        lower.endsWith(".mkv") ||
        lower.endsWith(".srt") ||
        lower.endsWith(".avi.part") ||
        lower.endsWith(".mkv.part") ||
        lower.endsWith(".srt.part");
}


static bool directoryReallyEmpty(
    const String &path
)
{
    File root =
        STORAGE.open(
            path.c_str(),
            FILE_READ
        );

    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        return false;
    }


    File entry =
        root.openNextFile();

    bool empty =
        !entry;


    if (entry)
        entry.close();

    root.close();

    return empty;
}


static void handleDeleteDay()
{
    if (rejectWhileRecording("delete recordings"))
        return;


    String day =
        server.arg("day");

    day.trim();


    if (
        !isDateFolderName(day) &&
        day != "fallback"
    ) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid day"
        );

        return;
    }


    String folderPath =
        "/" + day;


    File root =
        STORAGE.open(
            folderPath.c_str(),
            FILE_READ
        );


    if (!root || !root.isDirectory()) {

        if (root)
            root.close();

        server.send(
            404,
            "text/plain; charset=utf-8",
            "Recording folder not found"
        );

        return;
    }


    // First collect filenames and close the directory handle.
    // Deleting files while iterating an open FAT directory can
    // otherwise cause entries to be skipped on some FS versions.
    std::vector<String> filesToDelete;

    filesToDelete.reserve(
        32
    );


    uint16_t scannedEntries =
        0;

    File entry =
        root.openNextFile();


    while (entry) {

        bool isDir =
            entry.isDirectory();

        String name =
            String(entry.name());

        int slashPos =
            name.lastIndexOf('/');


        if (slashPos >= 0) {
            name =
                name.substring(
                    slashPos + 1
                );
        }


        if (
            !isDir &&
            isRecordingFileForDayDelete(
                name
            )
        ) {

            filesToDelete.push_back(
                name
            );
        }


        entry.close();

        scannedEntries++;

        if ((scannedEntries & 0x0FU) == 0)
            serviceWebLongOperation();

        entry =
            root.openNextFile();
    }


    root.close();

    serviceWebLongOperation();


    uint32_t deleted =
        0;

    uint32_t failed =
        0;


    for (
        const String &name :
        filesToDelete
    ) {

        String fullPath =
            folderPath +
            "/" +
            name;


        if (STORAGE.remove(
                fullPath.c_str()
            )) {

            deleted++;

        } else {

            failed++;

            Serial.println(
                "Delete day: failed to remove " +
                fullPath
            );

            logWrite(
                "Delete day failed: " +
                fullPath
            );
        }


        // This handler is synchronous inside loopTask.
        serviceWebLongOperation();

        delay(1);
    }


    // Remove the date directory only if no unknown file or
    // subdirectory remains.
    if (directoryReallyEmpty(
            folderPath
        )) {

        STORAGE.rmdir(
            folderPath.c_str()
        );
    }


    if (failed > 0) {

        server.send(
            500,
            "text/plain; charset=utf-8",
            "Deleted " +
            String(deleted) +
            " files, but " +
            String(failed) +
            " files could not be deleted."
        );

        return;
    }


    Serial.println(
        "Delete day: " +
        day +
        " -> " +
        String(deleted) +
        " recording file(s) deleted"
    );

    logWrite(
        "Delete day: " +
        day +
        " -> " +
        String(deleted) +
        " recording file(s) deleted"
    );


    server.send(
        200,
        "text/plain; charset=utf-8",
        String(deleted) +
        " recording file(s) deleted."
    );
}


// -------------------------------------------------------------
// Main recording page.
//
// Only root directories are scanned here.
// -------------------------------------------------------------

static void handleFiles()
{
    if (recorderIsOpen()) {
        String html =
            htmlHeader();

        html +=
            "<div class='page-title'><div>"
            "<h2>Aufnahmen</h2>"
            "<p>Aufnahmen verwalten und wiedergeben</p>"
            "</div></div>";

        html +=
            recordingBrowserModalHtml(
                true
            );

        html +=
            "<script>"
            "(function(){"
            "var state=document.getElementById('recordingBlockedState');"
            "function check(){"
                "fetch('/ui_status?t='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error();return r.json();})"
                ".then(function(s){"
                    "if(!s.recorder_open){"
                        "if(state)state.textContent='Aufnahme beendet – lade Verzeichnis ...';"
                        "setTimeout(function(){location.reload();},350);"
                    "}"
                "}).catch(function(){});"
            "}"
            "setInterval(check,1500);"
            "check();"
            "})();"
            "</script>";

        html +=
            htmlFooter();

        server.sendHeader(
            "Cache-Control",
            "no-store"
        );

        server.send(
            409,
            "text/html; charset=utf-8",
            html
        );

        return;
    }

    String html =
        htmlHeader();

    html +=
        "<h2>Aufnahmen</h2>";

    html +=
        "<div class='recordingsToolbar'>"
        "<button type='button' id='reloadRecordingsBtn' "
        "onclick='reloadRecordingTree()'>"
        "Verzeichnis neu laden"
        "</button>"
        "</div>";

    html +=
        "<p>Tage anklicken, um die Aufnahmen zu laden.</p>";

    html +=
        recordingBrowserModalHtml(
            false
        );


    std::vector<String> dayFolders;

    bool fallbackExists =
        false;


    File root =
        STORAGE.open(
            "/",
            FILE_READ
        );


    if (root && root.isDirectory()) {

        uint16_t scannedEntries =
            0;

        File file =
            root.openNextFile();


        while (file) {

            if (file.isDirectory()) {

                String name =
                    String(file.name());

                int slashPos =
                    name.lastIndexOf('/');

                if (slashPos >= 0) {
                    name =
                        name.substring(
                            slashPos + 1
                        );
                }


                if (isDateFolderName(name)) {

                    dayFolders.push_back(
                        name
                    );

                } else if (
                    name == "fallback"
                ) {

                    fallbackExists =
                        true;
                }
            }


            file.close();

            scannedEntries++;

            if ((scannedEntries & 0x0FU) == 0)
                serviceWebLongOperation();

            file =
                root.openNextFile();
        }


        root.close();

        serviceWebLongOperation();
    }


    std::sort(
        dayFolders.begin(),
        dayFolders.end(),
        stringDescending
    );


    if (
        dayFolders.empty() &&
        !fallbackExists
    ) {

        html +=
            "<p>Keine Aufnahmen gefunden.</p>";
    }


    for (
        const String &folderName :
        dayFolders
    ) {

        html +=
            "<details class='day' data-day='" +
            htmlEscape(folderName) +
            "'>";

        html +=
            "<summary>"
            "<span class='dayLabel'>" +
            htmlEscape(
                displayDateFolder(
                    folderName
                )
            ) +
            "<span class='count'></span>"
            "</span>"
            "<span class='dayActions'>"
            "<button type='button' class='downloadDayBtn' "
            "onclick=\"downloadDay(event,'" +
            htmlEscape(folderName) +
            "')\">"
            "Download ZIP"
            "</button>"
            "<button type='button' class='deleteDayBtn' "
            "onclick=\"deleteDay(event,'" +
            htmlEscape(folderName) +
            "')\">"
            "Alle löschen"
            "</button>"
            "</span>"
            "</summary>";

        html +=
            "<div class='daycontent' "
            "style='padding:8px 12px;'>"
            "Zum Laden aufklappen..."
            "</div>";

        html +=
            "</details>";
    }


    if (fallbackExists) {

        html +=
            "<details class='day' data-day='fallback'>"
            "<summary>"
            "<span class='dayLabel'>"
            "Zeit unbekannt / fallback"
            "<span class='count'></span>"
            "</span>"
            "<span class='dayActions'>"
            "<button type='button' class='downloadDayBtn' "
            "onclick=\"downloadDay(event,'fallback')\">"
            "Download ZIP"
            "</button>"
            "<button type='button' class='deleteDayBtn' "
            "onclick=\"deleteDay(event,'fallback')\">"
            "Alle löschen"
            "</button>"
            "</span>"
            "</summary>"
            "<div class='daycontent' "
            "style='padding:8px 12px;'>"
            "Zum Laden aufklappen..."
            "</div>"
            "</details>";
    }


    // Preserve the opened day and its already loaded HTML in the
    // browser session. Returning from the player therefore does not
    // require another SD directory scan.
    html +=
        "<style>"
        ".recordingsToolbar{"
            "display:flex;justify-content:flex-end;align-items:center;"
            "margin:0 0 10px 0;"
        "}"
        ".recordingsToolbar button{"
            "width:auto;margin:0;padding:7px 11px;"
            "border:1px solid #98a2b3;background:#f8fafc;color:#344054;"
            "border-radius:5px;cursor:pointer;"
        "}"
        ".recordingsToolbar button:disabled{"
            "opacity:.65;cursor:default;"
        "}"
        ".day{"
            "margin:0;border:0;border-bottom:1px solid #d6d6d6;"
            "background:#ffffff;"
        "}"
        ".day:nth-of-type(even){background:#eef3f7;}"
        ".day summary{"
            "cursor:pointer;display:grid;"
            "grid-template-columns:14px minmax(185px,max-content) auto;"
            "align-items:center;column-gap:10px;"
            "padding:9px 10px;min-height:38px;"
            "list-style:none;"
        "}"
        ".day summary::-webkit-details-marker{display:none;}"
        ".day summary::before{"
            "content:'';display:block;width:0;height:0;"
            "border-top:5px solid transparent;"
            "border-bottom:5px solid transparent;"
            "border-left:7px solid #555;"
            "transform-origin:40% 50%;"
            "transition:transform .12s ease;"
        "}"
        ".day[open]>summary::before{transform:rotate(90deg);}"
        ".dayLabel{"
            "display:inline-flex;align-items:center;gap:6px;"
            "font-weight:600;"
        "}"
        ".count{font-weight:normal;color:#666;}"
        ".dayActions{"
            "display:inline-flex;align-items:center;gap:6px;"
            "flex-wrap:wrap;"
        "}"
        ".downloadDayBtn,.deleteDayBtn{"
            "width:auto;margin:0;padding:5px 9px;"
            "border-radius:4px;font-size:12px;cursor:pointer;"
        "}"
        ".downloadDayBtn{"
            "border:1px solid #777;background:#f7f7f7;color:#222;"
        "}"
        ".deleteDayBtn{"
            "background:#b00020;color:white;border:1px solid #b00020;"
        "}"
        ".daycontent{background:rgba(255,255,255,0.55);}"
        "@media(max-width:700px){"
            ".day summary{"
                "grid-template-columns:14px 1fr;"
                "row-gap:7px;align-items:center;"
            "}"
            ".dayActions{grid-column:2;padding-left:0;}"
        "}"
        "</style>"
        "<script>"
        "const OPEN_DAY_KEY='recordings.openDay';"
        "const CACHE_PREFIX='recordings.day.';"
        "const BOOT_KEY='recordings.bootId';"
        "const BOOT_ID='" +
        String(webBootSessionId, HEX) +
        "';"

        "let sameBoot=false;"
        "try{"
            "const previousBoot=sessionStorage.getItem(BOOT_KEY);"
            "sameBoot=(previousBoot===BOOT_ID);"
            "if(!sameBoot){"
                "sessionStorage.removeItem(OPEN_DAY_KEY);"
                "const removeKeys=[];"
                "for(let i=0;i<sessionStorage.length;i++){"
                    "const k=sessionStorage.key(i);"
                    "if(k&&k.indexOf(CACHE_PREFIX)===0)removeKeys.push(k);"
                "}"
                "removeKeys.forEach(function(k){sessionStorage.removeItem(k);});"
                "sessionStorage.setItem(BOOT_KEY,BOOT_ID);"
            "}"
        "}catch(e){}"

        "function reloadRecordingTree(){"
            "const b=document.getElementById('reloadRecordingsBtn');"
            "if(b){b.disabled=true;b.textContent='Lade neu...';}"
            "try{"
                "const removeKeys=[];"
                "for(let i=0;i<sessionStorage.length;i++){"
                    "const k=sessionStorage.key(i);"
                    "if(k&&k.indexOf('recordings.')===0)removeKeys.push(k);"
                "}"
                "removeKeys.forEach(function(k){sessionStorage.removeItem(k);});"
            "}catch(e){}"
            "window.location.reload();"
        "}"

        "function showRecordingBlockedDialog(){"
            "const m=document.getElementById('recordingBlockedModal');"
            "const state=document.getElementById('recordingBlockedState');"
            "if(m)m.hidden=false;"
            "if(state)state.textContent='Warte auf Aufnahmeende ...';"
        "}"

        "function watchRecordingBlockedDialog(){"
            "const m=document.getElementById('recordingBlockedModal');"
            "if(!m||m.hidden)return;"
            "fetch('/ui_status?t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error();return r.json();})"
            ".then(function(s){"
                "if(!s.recorder_open){"
                    "const state=document.getElementById('recordingBlockedState');"
                    "if(state)state.textContent='Aufnahme beendet – Verzeichnis kann wieder geladen werden.';"
                    "setTimeout(function(){reloadRecordingTree();},350);"
                "}"
            "}).catch(function(){});"
        "}"

        "setInterval(watchRecordingBlockedDialog,1500);"

        "function downloadDay(ev,day){"
            "ev.preventDefault();"
            "ev.stopPropagation();"
            "window.location='/download_day?day='+encodeURIComponent(day);"
        "}"

        "function deleteDay(ev,day){"
            "ev.preventDefault();"
            "ev.stopPropagation();"
            "const d=ev.target.closest('details.day');"
            "let label=day;"
            "if(/^\\d{8}$/.test(day)){"
                "label=day.substring(6,8)+'.'+day.substring(4,6)+'.'+day.substring(0,4);"
            "}"
            "if(!confirm('Alle Videos vom '+label+' wirklich löschen?\\n\\nDiese Aktion kann nicht rückgängig gemacht werden.'))return;"
            "ev.target.disabled=true;"
            "ev.target.textContent='Lösche...';"
            "fetch('/delete_day',{"
                "method:'POST',"
                "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
                "body:'day='+encodeURIComponent(day)"
            "})"
            ".then(function(r){"
                "return r.text().then(function(t){"
                    "if(!r.ok)throw new Error(t||('HTTP '+r.status));"
                    "return t;"
                "});"
            "})"
            ".then(function(){"
                "try{"
                    "sessionStorage.removeItem(CACHE_PREFIX+day);"
                    "if(sessionStorage.getItem(OPEN_DAY_KEY)===day){"
                        "sessionStorage.removeItem(OPEN_DAY_KEY);"
                    "}"
                "}catch(e){}"
                "if(d)d.remove();"
                "if(!document.querySelector('details.day'))location.reload();"
            "})"
            ".catch(function(e){"
                "ev.target.disabled=false;"
                "ev.target.textContent='Alle löschen';"
                "alert('Löschen fehlgeschlagen: '+e.message);"
            "});"
        "}"

        "function updateDayCount(d){"
            "const c=d.querySelector('.daycontent');"
            "const n=c.querySelectorAll('.recording').length;"
            "const count=d.querySelector('.count');"
            "if(count){"
                "count.textContent=n===1?' (1 Video)':' ('+n+' Videos)';"
            "}"
        "}"

        "function restoreDay(d){"
            "const day=d.dataset.day;"
            "let cached=null;"
            "try{cached=sessionStorage.getItem(CACHE_PREFIX+day);}catch(e){}"
            "if(!cached)return false;"
            "const c=d.querySelector('.daycontent');"
            "c.innerHTML=cached;"
            "d.dataset.loaded='1';"
            "updateDayCount(d);"
            "return true;"
        "}"

        "function loadDay(d){"
            "if(d.dataset.loaded==='1')return;"
            "if(restoreDay(d))return;"
            "d.dataset.loaded='1';"
            "const c=d.querySelector('.daycontent');"
            "c.textContent='Lade...';"
            "fetch('/files_day?day='+encodeURIComponent(d.dataset.day),{cache:'no-store'})"
            ".then(function(r){"
                "if(r.status===409){"
                    "showRecordingBlockedDialog();"
                    "const blocked=new Error('recording_active');"
                    "blocked.recordingBlocked=true;"
                    "throw blocked;"
                "}"
                "if(!r.ok)throw new Error('HTTP '+r.status);"
                "return r.text();"
            "})"
            ".then(function(t){"
                "c.innerHTML=t;"
                "try{sessionStorage.setItem(CACHE_PREFIX+d.dataset.day,t);}catch(e){}"
                "updateDayCount(d);"
            "})"
            ".catch(function(e){"
                "d.dataset.loaded='0';"
                "if(e&&e.recordingBlocked){"
                    "c.textContent='Wird nach Aufnahmeende neu geladen ...';"
                    "return;"
                "}"
                "c.textContent='Fehler beim Laden: '+e.message;"
            "});"
        "}"

        "const dayElements=Array.from(document.querySelectorAll('details.day'));"

        "dayElements.forEach(function(d){"
            "d.addEventListener('toggle',function(){"
                "if(d.open){"
                    "dayElements.forEach(function(other){"
                        "if(other!==d&&other.open)other.open=false;"
                    "});"
                    "try{sessionStorage.setItem(OPEN_DAY_KEY,d.dataset.day);}catch(e){}"
                    "loadDay(d);"
                "}else{"
                    "try{"
                        "if(sessionStorage.getItem(OPEN_DAY_KEY)===d.dataset.day){"
                            "sessionStorage.removeItem(OPEN_DAY_KEY);"
                        "}"
                    "}catch(e){}"
                "}"
            "});"
            "restoreDay(d);"
        "});"

        // Only restore the open day when this browser session still
        // belongs to the same ESP32 boot. After reset/reboot all days
        // intentionally start collapsed.
        "if(sameBoot){"
            "let openDay=null;"
            "try{openDay=sessionStorage.getItem(OPEN_DAY_KEY);}catch(e){}"
            "if(openDay){"
                "const d=dayElements.find(function(x){return x.dataset.day===openDay;});"
                "if(d){"
                    "d.open=true;"
                    "loadDay(d);"
                "}"
            "}"
        "}"
        "</script>";


    html +=
        "<br><a href='/'><button>Back</button></a>";

    html +=
        htmlFooter();


    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}


static void handleFile()
{
    if (rejectWhileRecording("file download"))
        return;

    String path = server.arg("path");

    if (!path.length() ||
        !path.startsWith("/") ||
        path.indexOf("..") >= 0) {

        server.send(
            400,
            "text/plain; charset=utf-8",
            "Invalid path"
        );

        return;
    }

    String lowerPath = path;
    lowerPath.toLowerCase();

    bool allowedFile =
        lowerPath.endsWith(".avi") ||
        lowerPath.endsWith(".mkv") ||
        lowerPath.endsWith(".srt");

    if (!allowedFile) {

        server.send(
            403,
            "text/plain; charset=utf-8",
            "Only AVI, MKV and SRT files are allowed"
        );

        return;
    }


    File f = STORAGE.open(
        path.c_str(),
        FILE_READ
    );

    if (!f) {

        server.send(
            404,
            "text/plain; charset=utf-8",
            "File not found"
        );

        return;
    }


    // ---------------------------------------------------------
    // Tatsächlichen Dateinamen ermitteln
    // z.B. /20260907/012345.avi -> 012345.avi
    // ---------------------------------------------------------

    int slashPos = path.lastIndexOf('/');

    String downloadName =
        slashPos >= 0
        ? path.substring(slashPos + 1)
        : path;


    // Sicherheitsbereinigung für HTTP Header
    downloadName.replace("\"", "_");
    downloadName.replace("\r", "_");
    downloadName.replace("\n", "_");


    // ---------------------------------------------------------
    // Download Header
    // ---------------------------------------------------------

    server.sendHeader(
        "Content-Disposition",
        "attachment; filename=\"" +
        downloadName +
        "\""
    );

    server.sendHeader(
        "Cache-Control",
        "no-store"
    );

    server.sendHeader(
        "X-Content-Type-Options",
        "nosniff"
    );


    // ---------------------------------------------------------
    // Datei senden
    // ---------------------------------------------------------

    const char *contentType =
        lowerPath.endsWith(".mkv")
        ? "video/x-matroska"
        : (
            lowerPath.endsWith(".srt")
            ? "application/x-subrip"
            : "video/x-msvideo"
        );

    size_t sent = server.streamFile(
        f,
        contentType
    );

    if (sent != f.size()) {

        Serial.printf(
            "Download incomplete: %u / %u bytes\n",
            (unsigned)sent,
            (unsigned)f.size()
        );
    }

    f.close();
}


// -------------------------------------------------------------
// SYSTEM INFO
// -------------------------------------------------------------

static void handleSysInfo()
{
    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>System Info</h2>"
        "<p>Hardware-, Speicher- und Zeitdiagnose des Fabric Node.</p>"
        "</div></div>";

    html +=
        "<section class='settings-section'>"
        "<h3>Systemressourcen</h3>"
        "Flash Size: " +
        String(ESP.getFlashChipSize() / 1024 / 1024) +
        " MB<br>"
        "PSRAM Size: " +
        String(ESP.getPsramSize() / 1024 / 1024) +
        " MB<br>"
        "Heap Free: " +
        String(ESP.getFreeHeap() / 1024) +
        " KB<br>"
        "Sketch Size: " +
        String(ESP.getSketchSize() / 1024) +
        " KB"
        "</section>";

    html +=
        "<section class='settings-section'>"
        "<h3>Echtzeituhr (RTC)</h3>";

    if (rtcDetected()) {

        html +=
            "<p><span class='status-pill " +
            String(rtcClockValid() ? "ok" : "warn") +
            "'>" +
            String(rtcClockValid() ? "DIAGNOSE OK" : "ERKANNT / ZEIT UNGÜLTIG") +
            "</span></p>"
            "Typ: <b>" +
            htmlEscape(String(rtcTypeName())) +
            "</b><br>"
            "I2C-Adresse: <code>0x68</code><br>"
            "I2C-Pins: SDA=GPIO" +
            String((int)RTC_SDA_PIN) +
            " / SCL=GPIO" +
            String((int)RTC_SCL_PIN) +
            "<br>"
            "RTC-Zeit: <b>" +
            htmlEscape(rtcTimeText()) +
            "</b><br>"
            "Oszillator-Status: " +
            String(rtcOscillatorStopped() ? "OSF gesetzt" : "OK") +
            "<br>"
            "Systemzeit beim Boot aus RTC übernommen: " +
            String(rtcRestoredSystemTime() ? "ja" : "nein") +
            "<br>"
            "AT24C32 EEPROM @0x57: " +
            String(rtcEepromDetected() ? "erkannt" : "nicht erkannt") +
            "<br>"
            "<p class='muted'>Die RTC wird automatisch erkannt. Nach erfolgreicher NTP-Synchronisation "
            "wird sie automatisch aktualisiert; dafür sind keine RTC-Einträge in config.txt nötig.</p>";

    } else {

        html +=
            "<p><span class='status-pill'>OPTIONAL / NICHT ERKANNT</span></p>"
            "Keine unterstützte externe RTC auf SDA=GPIO" +
            String((int)RTC_SDA_PIN) +
            " / SCL=GPIO" +
            String((int)RTC_SCL_PIN) +
            " erkannt."
            "<p class='muted'>Das ist kein Systemfehler. Ohne RTC verwendet SensorForge wie bisher "
            "die normale System-/NTP-Zeit.</p>";
    }

    html +=
        "</section>";

    html +=
        "<br><a href='/'><button>Back</button></a>";

    html += htmlFooter();

    server.send(
        200,
        "text/html; charset=utf-8",
        html
    );
}



// -------------------------------------------------------------
// BOARD INFO
// -------------------------------------------------------------

static void handleBoardInfo()
{
    String html = htmlHeader();

    html += "<h2>Board Info</h2>";

#if defined(BOARD_FREENOVE)

    html += "Board: Freenove FNK0085 ESP32-S3 WROOM<br>";
    html += "Storage: SDMMC 1-bit<br>";

#elif defined(BOARD_XIAO)

    html += "Board: Seeed XIAO ESP32S3 Sense<br>";
    html += "Storage: SPI SD<br>";

#endif

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// PSRAM TEST
// -------------------------------------------------------------

static void handlePSRAM()
{
    if (rejectWhileRecording("PSRAM test"))
        return;

    String html = htmlHeader();

    html += "<h2>PSRAM Test</h2>";

    const size_t testSize = 1024 * 1024;

    uint8_t *buf =
        (uint8_t *)ps_malloc(testSize);

    if (!buf) {

        html += "<p style='color:red;'>"
                "PSRAM Allocation FAILED"
                "</p>";

    } else {

        for (size_t i = 0; i < testSize; ++i)
            buf[i] = (uint8_t)(i & 0xFF);

        bool ok = true;

        for (size_t i = 0; i < testSize; ++i) {

            if (buf[i] !=
                (uint8_t)(i & 0xFF)) {

                ok = false;
                break;
            }
        }

        free(buf);

        if (ok) {
            html += "<p style='color:green;'>PSRAM OK</p>";
        } else {
            html += "<p style='color:red;'>PSRAM ERROR</p>";
        }
    }

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// SD BENCHMARK
// -------------------------------------------------------------

static void handleSDBench()
{
    if (rejectWhileRecording("SD benchmark"))
        return;

    String html = htmlHeader();

    html += "<h2>SD Benchmark</h2>";

    const size_t blockSize = 32 * 1024;
    const size_t totalSize = 1024 * 1024;

    uint8_t *buf =
        (uint8_t *)malloc(blockSize);

    if (!buf) {

        html += "<p style='color:red;'>"
                "Cannot allocate benchmark buffer"
                "</p>";

    } else {

        memset(
            buf,
            0xAA,
            blockSize
        );

        STORAGE.remove("/bench.bin");

        File f = STORAGE.open(
            "/bench.bin",
            FILE_WRITE
        );

        if (!f) {

            html += "<p style='color:red;'>"
                    "Cannot open bench.bin"
                    "</p>";

        } else {

            unsigned long start =
                millis();

            size_t writtenTotal = 0;

            while (writtenTotal < totalSize) {

                size_t remaining =
                    totalSize - writtenTotal;

                size_t chunk =
                    remaining < blockSize
                    ? remaining
                    : blockSize;

                size_t written =
                    f.write(
                        buf,
                        chunk
                    );

                if (written != chunk)
                    break;

                writtenTotal += written;

                serviceWebLongOperation();
            }

            f.flush();
            f.close();

            unsigned long elapsed =
                millis() - start;


            if (writtenTotal == totalSize) {

                html += "Write 1 MB: " +
                        String(elapsed) +
                        " ms<br>";

                if (elapsed > 0) {

                    float mbPerSec =
                        1000.0f /
                        (float)elapsed;

                    html += "Speed: " +
                            String(mbPerSec, 2) +
                            " MB/s<br>";
                }

            } else {

                html += "<p style='color:red;'>"
                        "Benchmark write incomplete"
                        "</p>";
            }
        }

        free(buf);

        STORAGE.remove(
            "/bench.bin"
        );
    }

    html += "<br><a href='/'><button>Back</button></a>";
    html += htmlFooter();

    server.send(200, "text/html; charset=utf-8", html);
}


// -------------------------------------------------------------
// START SERVER
// -------------------------------------------------------------

void webConfigStart()
{
    if (webActive)
        return;

    if (webBootSessionId == 0) {
        webBootSessionId =
            esp_random();

        if (webBootSessionId == 0)
            webBootSessionId = 1;
    }

    if (!webRoutesRegistered) {

        server.addMiddleware([](
            WebServer &requestServer,
            Middleware::Callback next
        ) -> bool {

            // Protect every route, including player, downloads, destructive
            // tools and the activity heartbeat. Unauthorized requests do not
            // keep the WiFi inactivity timer alive.
            if (
                cfg_web_auth_enabled &&
                !requestServer.authenticate(
                    cfg_web_username.c_str(),
                    cfg_web_password.c_str()
                )
            ) {
                requestServer.requestAuthentication();
                return true;
            }

            noteWebActivity();

            bool handled =
                next();

            // Long synchronous file/ZIP transfers cannot be interrupted by
            // the main loop. Refresh once more at completion so the timeout
            // starts only after the transfer is finished.
            noteWebActivity();

            return handled;
        });

        server.on("/activity", HTTP_GET, []() {
            server.send(
                204,
                "text/plain",
                ""
            );
        });

        server.on("/ui_status", HTTP_GET, handleUiStatus);
        server.on("/recording_pause", HTTP_POST, handleRecordingPause);
        server.on("/recording_pause_keepalive", HTTP_POST, handleRecordingPauseKeepalive);

        server.on("/", HTTP_GET, handleRoot);
        server.on("/config", HTTP_GET, handleConfig);
        server.on("/save", HTTP_POST, handleSave);
    server.on("/simulate_motion", HTTP_POST, handleSimulateMotion);

    server.on("/radar_config", HTTP_GET, handleRadarConfig);
    server.on("/radar_live", HTTP_GET, handleRadarLive);
    server.on("/radar_config_save", HTTP_POST, handleRadarConfigSave);
    server.on("/radar_config_defaults", HTTP_POST, handleRadarConfigDefaults);

    server.on("/sdstatus", HTTP_GET, handleSDStatus);
    server.on("/sd_maintenance", HTTP_GET, handleSDMaintenance);
    server.on("/sdformat", HTTP_GET, handleSDFormat);
    server.on("/sdformat_do", HTTP_POST, handleSDFormatDo);
    server.on("/sd_format_do", HTTP_POST, handleSDRealFormatDo);
    server.on("/sd_secure_erase_do", HTTP_POST, handleSDSecureEraseDo);
    server.on("/sd_secure_status", HTTP_GET, handleSDSecureStatus);
    server.on("/sd_secure_abort", HTTP_POST, handleSDSecureAbort);

    server.on("/preview", HTTP_GET, handlePreview);
    server.on("/snapshot", HTTP_GET, handleSnapshot);
    server.on("/preview_stop", HTTP_POST, handlePreviewStop);

    server.on("/reboot", HTTP_GET, handleReboot);
    server.on("/rebooting", HTTP_GET, handleRebooting);
    server.on("/reboot_do", HTTP_POST, handleRebootDo);

    server.on("/log", HTTP_GET, handleLog);
    server.on("/log_raw", HTTP_GET, handleLogRaw);
    server.on("/files", HTTP_GET, handleFiles);
    server.on("/files_day", HTTP_GET, handleFilesDay);
    server.on("/download_day", HTTP_GET, handleDownloadDay);
    server.on("/delete_day", HTTP_POST, handleDeleteDay);
    server.on("/file", HTTP_GET, handleFile);

    webPlayerRegisterRoutes(server);

    server.on("/sysinfo", HTTP_GET, handleSysInfo);
    server.on("/board", HTTP_GET, handleBoardInfo);
    server.on("/psram", HTTP_GET, handlePSRAM);
    server.on("/sdbench", HTTP_GET, handleSDBench);
   

        server.onNotFound([]() {
            server.send(
                404,
                "text/plain; charset=utf-8",
                "Not found"
            );
        });

        webRoutesRegistered = true;
    }

    noteWebActivity();

    server.begin();
    webActive = true;

    Serial.println("WebConfig started");
}


// -------------------------------------------------------------
// STOP SERVER
// -------------------------------------------------------------

void webConfigStop()
{
    if (!webActive)
        return;

    // A temporary maintenance pause is never allowed to survive the WebConfig
    // session that created it.
    if (recordingAutomationPaused) {
        setRecordingAutomationPaused(
            false,
            "WebConfig stopped"
        );
    }

    webPlayerStop();

    server.stop();
    webActive = false;

    simulatedMotionUntilMs = 0;
    stopCameraPreview();

    Serial.println("WebConfig stopped");
}


// -------------------------------------------------------------
// INACTIVITY STATUS
// -------------------------------------------------------------

bool webConfigInactiveFor(
    unsigned long timeoutSec
)
{
    if (
        !webActive ||
        timeoutSec == 0 ||
        sdSecureJobActive
    ) {
        return false;
    }

    uint32_t elapsedMs =
        (uint32_t)(
            millis() -
            webLastActivityMs
        );

    uint64_t timeoutMs =
        (uint64_t)timeoutSec *
        1000ULL;

    return
        (uint64_t)elapsedMs >=
        timeoutMs;
}


// -------------------------------------------------------------
// LOOP
// -------------------------------------------------------------

void webConfigLoop()
{
    if (
        rebootScheduled &&
        (int32_t)(
            millis() -
            rebootAtMs
        ) >= 0
    ) {
        rebootScheduled =
            false;

        ESP.restart();
        return;
    }

    if (!webActive) {
        // A manually closed WebConfig must not strand an in-progress secure
        // erase with the storage gate held. Continue the maintenance job even
        // without HTTP; only progress/abort control is unavailable then.
        processSecureEraseJob();
        return;
    }

    server.handleClient();

    // The operator pause is lease-based. Only a visible SensorForge page
    // renews the lease; a closed/hidden/abandoned UI therefore restores
    // automatic recording without requiring any manual cleanup.
    releaseRecordingPauseIfLeaseExpired();

    // Secure Erase writes one small zero-filled chunk per loop iteration.
    // Keeping it here lets the WebServer answer progress/abort requests
    // between chunks instead of blocking for the duration of the whole card.
    processSecureEraseJob();

    webPlayerLoop();

    // Expire a preview session if the browser vanished without sending the
    // explicit /preview_stop request.
    webConfigCameraPreviewActive();
}
