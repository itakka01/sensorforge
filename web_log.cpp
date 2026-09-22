#include "web_log.h"

#include "board_config.h"
#include "config.h"
#include "logger.h"
#include "log_storage.h"
#include "recorder.h"
#include "storage_guard.h"

#include <FS.h>
#include <stdint.h>

namespace {

static WebServer *g_webServer = nullptr;
static WebLogUiHooks g_uiHooks = { nullptr, nullptr };

static WebServer &webServer()
{
    return *g_webServer;
}

static String pageHeader()
{
    return g_uiHooks.htmlHeader
        ? g_uiHooks.htmlHeader()
        : String();
}

static String pageFooter()
{
    return g_uiHooks.htmlFooter
        ? g_uiHooks.htmlFooter()
        : String();
}

static String escapeHtml(const String &value)
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

static bool rejectWhileRecording(const char *operation)
{
    if (!recorderIsOpen())
        return false;

    webServer().send(
        409,
        "text/plain; charset=utf-8",
        "Recording active - " +
        String(operation) +
        " is temporarily unavailable."
    );

    return true;
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


static bool parseUnsignedLongLongArg(
    const String &text,
    uint64_t &value
)
{
    if (!text.length()) {
        value = 0;
        return true;
    }

    uint64_t result = 0;

    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];

        if (c < '0' || c > '9')
            return false;

        uint8_t digit =
            (uint8_t)(c - '0');

        if (
            result >
            (UINT64_MAX - digit) / 10ULL
        ) {
            return false;
        }

        result =
            result * 10ULL +
            digit;
    }

    value = result;
    return true;
}


static void handleLogRaw()
{
    if (rejectWhileRecording("log download"))
        return;

    logFlush();

    String logPath =
        activeWebLogPath();

    String body;
    uint64_t nextCursor = 0;
    uint64_t physicalSize = 0;
    bool more = false;
    bool reset = false;
    bool encrypted = false;
    String generation;
    bool recoveredTornRecord = false;
    String error;

    static const size_t MAX_LOG_STREAM_CHUNK =
        16U * 1024U;

    if (!logStorageReadChunk(
            logPath,
            0,
            MAX_LOG_STREAM_CHUNK,
            body,
            nextCursor,
            physicalSize,
            more,
            reset,
            encrypted,
            generation,
            recoveredTornRecord,
            error
        )) {
        webServer().send(
            STORAGE.exists(logPath.c_str()) ? 500 : 404,
            "text/plain; charset=utf-8",
            "Log read failed: " + error
        );
        return;
    }

    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    webServer().sendHeader(
        "X-Log-Size",
        String((unsigned long)physicalSize)
    );

    webServer().sendHeader(
        "X-Log-Generation",
        generation
    );

    webServer().sendHeader(
        "X-Log-Encrypted",
        encrypted ? "1" : "0"
    );

    if (
        webServer().hasArg("download") &&
        webServer().arg("download") == "1"
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

        webServer().sendHeader(
            "Content-Disposition",
            "attachment; filename=\"" +
            downloadName +
            "\""
        );
    }

    webServer().setContentLength(
        CONTENT_LENGTH_UNKNOWN
    );

    webServer().send(
        200,
        "text/plain; charset=utf-8",
        ""
    );

    if (body.length())
        webServer().sendContent(body);

    if (recoveredTornRecord) {
        Serial.println(
            "SFLOG1 reader: recovered after torn record"
        );
    }

    uint64_t cursor = nextCursor;

    while (more && webServer().client().connected()) {
        body = "";
        reset = false;
        recoveredTornRecord = false;

        if (!logStorageReadChunk(
                logPath,
                cursor,
                MAX_LOG_STREAM_CHUNK,
                body,
                nextCursor,
                physicalSize,
                more,
                reset,
                encrypted,
                generation,
                recoveredTornRecord,
                error
            )) {
            Serial.println(
                "SFLOG1 raw stream failed | " +
                error
            );
            webServer().client().stop();
            return;
        }

        if (body.length())
            webServer().sendContent(body);

        if (recoveredTornRecord) {
            Serial.println(
                "SFLOG1 reader: recovered after torn record"
            );
        }

        if (nextCursor <= cursor && more) {
            Serial.println(
                "SFLOG1 raw stream stalled"
            );
            webServer().client().stop();
            return;
        }

        cursor = nextCursor;
    }
}


static void handleLogChunk()
{
    if (rejectWhileRecording("log live update"))
        return;

    String logPath =
        activeWebLogPath();

    uint64_t offset = 0;

    if (
        webServer().hasArg("offset") &&
        !parseUnsignedLongLongArg(
            webServer().arg("offset"),
            offset
        )
    ) {
        webServer().send(
            400,
            "text/plain; charset=utf-8",
            "Invalid log offset"
        );
        return;
    }

    // Full-page loading uses many small requests. Flushing the writer before
    // every chunk would add unnecessary SD and SFLOG1 work. Flush once for the
    // first chunk; live polling explicitly requests a fresh writer flush.
    bool flushRequested =
        offset == 0 ||
        (
            webServer().hasArg("flush") &&
            webServer().arg("flush") == "1"
        );

    if (flushRequested)
        logFlush();

    uint64_t infoSize = 0;
    bool infoEncrypted = false;
    String infoGeneration;
    String error;

    if (!logStorageGetInfo(
            logPath,
            infoSize,
            infoEncrypted,
            infoGeneration,
            error
        )) {
        webServer().send(
            STORAGE.exists(logPath.c_str()) ? 500 : 404,
            "text/plain; charset=utf-8",
            "Log read failed: " + error
        );
        return;
    }

    bool generationReset = false;

    if (
        infoEncrypted &&
        webServer().hasArg("generation") &&
        webServer().arg("generation").length() &&
        webServer().arg("generation") != infoGeneration
    ) {
        offset = 0;
        generationReset = true;
    }

    static const size_t MAX_LOG_CHUNK_BYTES =
        16U * 1024U;

    String body;
    uint64_t nextOffset = 0;
    uint64_t fileSize = 0;
    bool more = false;
    bool reset = false;
    bool encrypted = false;
    String generation;
    bool recoveredTornRecord = false;

    if (!logStorageReadChunk(
            logPath,
            offset,
            MAX_LOG_CHUNK_BYTES,
            body,
            nextOffset,
            fileSize,
            more,
            reset,
            encrypted,
            generation,
            recoveredTornRecord,
            error
        )) {
        webServer().send(
            500,
            "text/plain; charset=utf-8",
            "Log read failed: " + error
        );
        return;
    }

    reset = reset || generationReset;

    if (recoveredTornRecord) {
        Serial.println(
            "SFLOG1 reader: recovered after torn record"
        );
    }

    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    webServer().sendHeader(
        "X-Log-Offset",
        String((unsigned long)nextOffset)
    );

    webServer().sendHeader(
        "X-Log-Size",
        String((unsigned long)fileSize)
    );

    webServer().sendHeader(
        "X-Log-Reset",
        reset ? "1" : "0"
    );

    webServer().sendHeader(
        "X-Log-More",
        more ? "1" : "0"
    );

    webServer().sendHeader(
        "X-Log-Generation",
        generation
    );

    webServer().sendHeader(
        "X-Log-Encrypted",
        encrypted ? "1" : "0"
    );

    webServer().sendHeader(
        "X-Log-Recovered",
        recoveredTornRecord ? "1" : "0"
    );

    webServer().send(
        200,
        "text/plain; charset=utf-8",
        body
    );
}


static void handleLogClear()
{
    if (rejectWhileRecording("log clear"))
        return;

    if (g_storageLocked) {
        webServer().send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    String error;

    if (!logClear(error)) {
        webServer().send(
            500,
            "text/plain; charset=utf-8",
            "Log konnte nicht geleert werden: " +
            error
        );
        return;
    }

    consoleWrite(
        "LOG",
        "Log cleared from WebConfig"
    );

    // POST/Redirect/GET: browser refresh must never repeat a destructive action.
    webServer().sendHeader(
        "Location",
        "/log?notice=cleared"
    );

    webServer().send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


static void handleLog()
{
    if (rejectWhileRecording("log viewer"))
        return;

    // Keep the page request lightweight. The progressive log loader flushes
    // once before its first chunk, rather than blocking page generation here.
    String logPath =
        activeWebLogPath();

    uint64_t logSize = 0;
    bool logEncrypted = false;
    String logGeneration;
    String logInfoError;

    bool logInfoAvailable =
        logStorageGetInfo(
            logPath,
            logSize,
            logEncrypted,
            logGeneration,
            logInfoError
        );

    // A transient SD/SFLOG1 read error must not make the whole Log Viewer page
    // fail. The browser loads the plaintext stream progressively via /log_chunk
    // and retries individual chunks. Keep the page available and let that path
    // surface a persistent error with more useful context.
    if (!logInfoAvailable) {
        logSize = 0;
        logEncrypted = false;
        logGeneration = "";
    }

    String sizeText;

    if (!logInfoAvailable) {
        sizeText = "--";
    } else if (logSize >= 1024ULL * 1024ULL) {
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

    bool justCleared =
        webServer().hasArg("notice") &&
        webServer().arg("notice") == "cleared";

    const bool logUiEnglish =
        cfg_web_language == "en";

    const String logAnalyzeButtonText =
        logUiEnglish
        ? "Calculate statistics"
        : "Statistik berechnen";

    const String logAnalysisIdleText =
        logUiEnglish
        ? "Not calculated yet"
        : "Noch nicht berechnet";

    const String logAnalysisStaleText =
        logUiEnglish
        ? "Log changed - recalculate statistics"
        : "Log geändert - Statistik neu berechnen";

    const String logLoadingChunkText =
        logUiEnglish
        ? "Loading log in chunks..."
        : "Log wird blockweise geladen...";

    const String logRetryText =
        logUiEnglish
        ? "Read failed - retrying..."
        : "Lesefehler - neuer Versuch...";

    String html = pageHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Log Viewer</h2>"
        "<p>SensorForge System- und Ereignisprotokoll</p>"
        "</div></div>";

    if (justCleared) {
        html +=
            "<div class='flash-notice success'>"
            "<strong>Log wurde geleert.</strong> "
            "Aktive Logdatei und Rotationsarchiv wurden entfernt. "
            "Eine neue Audit-Zeile markiert den manuellen Neustart des Logs."
            "</div>";
    }

    html +=
        "<div class='log-analysis'>"
        "<div class='log-analysis-head'><div><h3>Log Analyse</h3>"
        "<div class='muted'>Statistik aus der aktuell geladenen Logdatei. Der Suchfilter beeinflusst die Analyse nicht.</div></div>"
        "<div style='display:flex;gap:10px;align-items:center;flex-wrap:wrap;justify-content:flex-end'>"
        "<button id='logAnalyze' class='primary' type='button'>" +
        escapeHtml(logAnalyzeButtonText) +
        "</button>"
        "<div id='logAnalysisStatus' class='log-analysis-status'>" +
        escapeHtml(logAnalysisIdleText) +
        "</div></div></div>"
        "<div class='log-analysis-grid'>"
        "<div class='log-stat'><div class='log-stat-label'>Log Start</div><div id='statLogStart' class='log-stat-value'>--</div><div class='log-stat-note'>frühester Zeitstempel</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Log Ende</div><div id='statLogEnd' class='log-stat-value'>--</div><div class='log-stat-note'>spätester Zeitstempel</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Zeitraum</div><div id='statPeriod' class='log-stat-value'>--</div><div class='log-stat-note'>zwischen Start und Ende</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Sleep gesamt</div><div id='statSleep' class='log-stat-value'>--</div><div id='statSleepNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Alarmereignisse</div><div id='statEvents' class='log-stat-value'>--</div><div class='log-stat-note'>Recording START, NEXT zählt nicht neu</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Clips abgeschlossen</div><div id='statClips' class='log-stat-value'>--</div><div class='log-stat-note'>Clips mit STOP + duration</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Aufnahmemodus gesamt</div><div id='statModeTime' class='log-stat-value'>--</div><div id='statModeNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Clip-Länge gesamt</div><div id='statRecordTime' class='log-stat-value'>--</div><div id='statRecordNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Cliplänge</div><div id='statClipAvg' class='log-stat-value'>--</div><div id='statClipNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Frames gesamt</div><div id='statFrames' class='log-stat-value'>--</div><div class='log-stat-note'>aus abgeschlossenen Clips</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Boots</div><div id='statBoots' class='log-stat-value'>--</div><div id='statBootNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Warnungen / Fehler</div><div id='statHealth' class='log-stat-value'>--</div><div id='statHealthNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Safety Limits</div><div id='statSafety' class='log-stat-value'>--</div><div id='statSafetyNote' class='log-stat-note'>--</div></div>"
        "</div>"
        "<div style='margin:18px 0 8px;font-weight:800;font-size:1.02rem'>Temperatur / Thermal Guard</div>"
        "<div class='log-analysis-grid'>"
        "<div class='log-stat'><div class='log-stat-label'>Thermal Warnings</div><div id='statThermalWarnings' class='log-stat-value'>--</div><div id='statThermalWarningsNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Thermal Abschaltungen</div><div id='statThermalEmergency' class='log-stat-value'>--</div><div id='statThermalEmergencyNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>CPU Spitze</div><div id='statThermalCpuMax' class='log-stat-value'>--</div><div id='statThermalCpuMaxNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>RTC/Gehäuse Spitze</div><div id='statThermalRtcMax' class='log-stat-value'>--</div><div id='statThermalRtcMaxNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Ø CPU bei Aufnahme</div><div id='statThermalCpuAvgRec' class='log-stat-value'>--</div><div id='statThermalCpuAvgRecNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Ø RTC bei Aufnahme</div><div id='statThermalRtcAvgRec' class='log-stat-value'>--</div><div id='statThermalRtcAvgRecNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Thermal Recoveries</div><div id='statThermalRecoveries' class='log-stat-value'>--</div><div id='statThermalRecoveriesNote' class='log-stat-note'>--</div></div>"
        "<div class='log-stat'><div class='log-stat-label'>Aufnahme-Thermaldaten</div><div id='statThermalCoverage' class='log-stat-value'>--</div><div id='statThermalCoverageNote' class='log-stat-note'>--</div></div>"
        "</div>"
        "<div class='log-chart-card'>"
        "<div class='log-chart-title'>Alarmereignisse nach Uhrzeit</div>"
        "<div class='log-chart-note'>24 Stunden-Buckets über alle Tage im geladenen Log: 0–1, 1–2, ... 23–24 Uhr. Gezählt wird jedes <code>Recording START</code>; Segmentwechsel <code>NEXT</code> sind kein neuer Alarm.</div>"
        "<div class='log-chart-scroll'><div class='log-chart-inner'><canvas id='logHourChart' class='log-hour-chart' width='1000' height='320' aria-label='Alarmereignisse pro Stunde'></canvas></div></div>"
        "</div></div>"
        ;

    html +=
        "<div class='log-toolbar'>"
        "<button id='logReload' class='primary' type='button'>Neu laden</button>"
        "<button id='logBottom' type='button'>Zum Ende</button>"
        "<a class='button' href='/log_raw?download=1'>Download</a>"
        "<form method='POST' action='/log_clear' style='display:inline;margin:0' "
        "onsubmit=\"return confirm('Log wirklich leeren? Die aktuelle Logdatei und das .1-Rotationsarchiv werden gelöscht. Dieser Vorgang kann nicht rückgängig gemacht werden.');\">"
        "<button class='danger' type='submit'>Log leeren</button>"
        "</form>"
        "<span id='logStatus' class='log-status'>Bereit</span>"
        "</div>"
        "<div class='log-filter-row'>"
        "<input id='logSearch' class='log-search' type='search' autocomplete='off' "
        "placeholder='Log durchsuchen / filtern...'>"
        "<label class='log-live-label'><input id='logLive' type='checkbox'> "
        "Live aktualisieren (5 s)</label>"
        "</div>"
        "<div class='log-meta'>"
        "<span>Datei: <code>" +
        escapeHtml(logPath) +
        "</code></span>"
        "<span>Größe: <span id='logSize'>" +
        escapeHtml(sizeText) +
        "</span></span>"
        "<span id='logFilterMeta'></span>"
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
        "var search=document.getElementById('logSearch');"
        "var live=document.getElementById('logLive');"
        "var sizeEl=document.getElementById('logSize');"
        "var filterMeta=document.getElementById('logFilterMeta');"
        "var analysisStatus=document.getElementById('logAnalysisStatus');"
        "var analyzeButton=document.getElementById('logAnalyze');"
        "var hourChart=document.getElementById('logHourChart');"
        "var lastHourBuckets=new Array(24).fill(0);"
        "var rawText='';"
        "var logOffset=0;"
        "var logGeneration='';"
        "var loading=false;"
        "var analysisCalculated=false;"
        "var liveTimer=0;"
        "var uiAnalysisIdle='" + logAnalysisIdleText + "';"
        "var uiAnalysisStale='" + logAnalysisStaleText + "';"
        "var uiLoadingChunks='" + logLoadingChunkText + "';"
        "var uiRetry='" + logRetryText + "';"
        "function scrollBottom(){term.scrollTop=term.scrollHeight;}"
        "function byteSizeText(bytes){"
            "if(bytes>=1048576)return (bytes/1048576).toFixed(1)+' MB';"
            "if(bytes>=1024)return (bytes/1024).toFixed(1)+' KB';"
            "return bytes+' B';"
        "}"
        "function statText(id,value){var e=document.getElementById(id);if(e)e.textContent=value;}"
        "function parseTimestamp(line){"
        "var m=/^\\[(\\d{4})-(\\d{2})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})(?:\\.(\\d{1,3}))?\\]/.exec(line);"
        "if(!m)return null;"
        "var ms=m[7]?Number((m[7]+'00').slice(0,3)):0;"
        "var value=Date.UTC(+m[1],+m[2]-1,+m[3],+m[4],+m[5],+m[6],ms);"
        "if(!Number.isFinite(value))return null;"
        "return {ms:value,hour:+m[4],label:m[3]+'.'+m[2]+'.'+m[1]+' '+m[4]+':'+m[5]+':'+m[6]};"
        "}"
        "function durationText(ms){"
        "if(!Number.isFinite(ms)||ms<0)return '--';"
        "var total=Math.round(ms/1000),days=Math.floor(total/86400);total%=86400;"
        "var hours=Math.floor(total/3600);total%=3600;"
        "var mins=Math.floor(total/60),secs=total%60;"
        "var parts=[];"
        "if(days)parts.push(days+' d');"
        "if(hours||days)parts.push(hours+' h');"
        "if(mins||hours||days)parts.push(mins+' min');"
        "if(!days&&!hours)parts.push(secs+' s');"
        "return parts.join(' ');"
        "}"
        "function secondsText(seconds){return durationText(seconds*1000);}"
        "function niceStep(maxValue){"
        "if(maxValue<=4)return 1;"
        "var rough=maxValue/4;"
        "var p=Math.pow(10,Math.floor(Math.log10(rough)));"
        "var n=rough/p;"
        "if(n<=1)return p;if(n<=2)return 2*p;if(n<=5)return 5*p;return 10*p;"
        "}"
        "function drawHourChart(counts){"
        "if(!hourChart||!hourChart.getContext)return;"
        "var ctx=hourChart.getContext('2d'),w=hourChart.width,h=hourChart.height;"
        "ctx.clearRect(0,0,w,h);"
        "var root=getComputedStyle(document.documentElement);"
        "var accent=(root.getPropertyValue('--accent')||'#2563eb').trim();"
        "var muted=(root.getPropertyValue('--muted')||'#667085').trim();"
        "var line=(root.getPropertyValue('--line')||'#d8dee6').trim();"
        "var text=(root.getPropertyValue('--text')||'#1f2933').trim();"
        "var left=52,right=18,top=26,bottom=48,pw=w-left-right,ph=h-top-bottom;"
        "var maxValue=0;for(var i=0;i<24;i++)if(counts[i]>maxValue)maxValue=counts[i];"
        "var step=niceStep(Math.max(1,maxValue));var yMax=Math.max(step,Math.ceil(maxValue/step)*step);"
        "ctx.font='12px Arial';ctx.textBaseline='middle';"
        "for(var yv=0;yv<=yMax;yv+=step){"
        "var y=top+ph-(yv/yMax)*ph;ctx.strokeStyle=line;ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(w-right,y);ctx.stroke();"
        "ctx.fillStyle=muted;ctx.textAlign='right';ctx.fillText(String(yv),left-8,y);"
        "}"
        "var slot=pw/24,barW=Math.max(4,slot*0.68);"
        "for(var hour=0;hour<24;hour++){"
        "var value=counts[hour]||0;var bh=(value/yMax)*ph;var x=left+hour*slot+(slot-barW)/2;var yb=top+ph-bh;"
        "ctx.fillStyle=accent;ctx.fillRect(x,yb,barW,bh);"
        "if(value>0){ctx.fillStyle=text;ctx.textAlign='center';ctx.textBaseline='bottom';ctx.fillText(String(value),x+barW/2,Math.max(top+12,yb-3));ctx.textBaseline='middle';}"
        "}"
        "ctx.strokeStyle=text;ctx.lineWidth=1.2;ctx.beginPath();ctx.moveTo(left,top+ph);ctx.lineTo(w-right,top+ph);ctx.stroke();"
        "ctx.fillStyle=muted;ctx.textAlign='center';ctx.textBaseline='top';"
        "for(var tick=0;tick<=24;tick++){var x=left+(tick/24)*pw;ctx.beginPath();ctx.moveTo(x,top+ph);ctx.lineTo(x,top+ph+5);ctx.strokeStyle=text;ctx.stroke();ctx.fillText(String(tick),x,top+ph+9);}"
        "ctx.save();ctx.translate(15,top+ph/2);ctx.rotate(-Math.PI/2);ctx.textAlign='center';ctx.textBaseline='top';ctx.fillStyle=muted;ctx.fillText('Anzahl Alarmereignisse',0,0);ctx.restore();"
        "if(maxValue===0){ctx.fillStyle=muted;ctx.textAlign='center';ctx.textBaseline='middle';ctx.fillText('Keine Recording START Ereignisse im geladenen Log',left+pw/2,top+ph/2);}"
        "}"
        "function markAnalysisDirty(){"
        "if(analysisStatus)analysisStatus.textContent=analysisCalculated?uiAnalysisStale:uiAnalysisIdle;"
        "}"
        "function analyzeLog(){"
        "var lines=rawText?rawText.split(/\\r?\\n/):[];"
        "var first=null,last=null,events=0,clips=0,recordSeconds=0,frames=0,clipMin=null,clipMax=0;"
        "var modeMs=0,modeEvents=0,activeEventStart=null;"
        "var sleepMs=0,sleepCycles=0,sleepUnknown=0,sleepMax=0;"
        "var boots=0,brownouts=0,wdt=0,warnings=0,errors=0,safety=0,sdRecoveries=0;"
        "var thermalWarnCpu=0,thermalWarnRtc=0,thermalEmergencyCpu=0,thermalEmergencyRtc=0,thermalEmergencyBoth=0,thermalRecoveries=0;"
        "var thermalCpuMax=null,thermalRtcMax=null;"
        "var recThermalEvents=0,recCpuSamples=0,recCpuWeighted=0,recCpuMax=null,recRtcSamples=0,recRtcWeighted=0,recRtcMax=null;"
        "var buckets=new Array(24).fill(0);"
        "for(var i=0;i<lines.length;i++){"
        "var line=lines[i];if(!line)continue;"
        "var ts=parseTimestamp(line);"
        "if(ts){if(!first||ts.ms<first.ms)first=ts;if(!last||ts.ms>last.ms)last=ts;}"
        "if(line.indexOf('Recording START |')>=0){"
        "events++;"
        "activeEventStart=ts?ts.ms:null;"
        "var hm=/\\/\\d{8}\\/(\\d{2})\\d{4}\\.[A-Za-z0-9]+/.exec(line);"
        "var hour=hm?Number(hm[1]):(ts?ts.hour:-1);"
        "if(hour>=0&&hour<24)buckets[hour]++;"
        "}"
        "var stop=/Recording STOP \\|.*?duration=([0-9]+(?:\\.[0-9]+)?) s/i.exec(line);"
        "if(stop){var sec=Number(stop[1]);if(Number.isFinite(sec)&&sec>=0){clips++;recordSeconds+=sec;if(clipMin===null||sec<clipMin)clipMin=sec;if(sec>clipMax)clipMax=sec;}var fm=/frames=(\\d+)/i.exec(line);if(fm)frames+=Number(fm[1])||0;}"
        "var eventEnded=line.indexOf('Recording stop | finalized')>=0||line.indexOf('Recording finalization failed')>=0||line.indexOf('Recording segment finalization failed')>=0;"
        "if(eventEnded&&activeEventStart!==null&&ts&&ts.ms>=activeEventStart){modeMs+=ts.ms-activeEventStart;modeEvents++;activeEventStart=null;}"
        "var sm=/SLEEP \\| mode=(light|deep) \\| duration_ms=(\\d+)/i.exec(line);"
        "if(sm){var d=Number(sm[2]);if(Number.isFinite(d)&&d>=0){sleepCycles++;sleepMs+=d;if(d>sleepMax)sleepMax=d;}}"
        "else if(/SLEEP \\| mode=(light|deep) \\| duration_ms=unknown/i.test(line)){sleepCycles++;sleepUnknown++;}"
        "if(line.indexOf('BOOT | reset=')>=0){boots++;if(line.indexOf('reset=BROWNOUT')>=0)brownouts++;if(/reset=(?:INT_WDT|TASK_WDT|WDT)/.test(line))wdt++;}"
        "if(/\\bWARNING\\b/i.test(line))warnings++;"
        "if(/\\bERROR\\b/i.test(line)||/\\bfailed\\b/i.test(line))errors++;"
        "if(line.indexOf('Recording safety limit reached')>=0)safety++;"
        "if(line.indexOf('SD recovery successful')>=0)sdRecoveries++;"
        "if(line.indexOf('THERMAL |')>=0){"
        "var ev=/THERMAL \\| ([A-Z0-9_]+)/.exec(line);var eventName=ev?ev[1]:'';"
        "if(eventName==='WARNING_CPU')thermalWarnCpu++;else if(eventName==='WARNING_RTC')thermalWarnRtc++;"
        "else if(eventName==='EMERGENCY_CPU')thermalEmergencyCpu++;else if(eventName==='EMERGENCY_RTC')thermalEmergencyRtc++;else if(eventName==='EMERGENCY_CPU_RTC')thermalEmergencyBoth++;"
        "if(eventName.indexOf('RECOVERED_')===0)thermalRecoveries++;"
        "var cpuEvent=/\\| CPU=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line);"
        "if(cpuEvent){var cv=Number(cpuEvent[1]);if(Number.isFinite(cv)&&(thermalCpuMax===null||cv>thermalCpuMax))thermalCpuMax=cv;}"
        "var rtcEvent=/\\| RTC=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line);"
        "if(rtcEvent){var rv=Number(rtcEvent[1]);if(Number.isFinite(rv)&&(thermalRtcMax===null||rv>thermalRtcMax))thermalRtcMax=rv;}"
        "if(eventName==='RECORDING_SUMMARY'){"
        "recThermalEvents++;"
        "var cs=/cpu_samples=(\\d+)/i.exec(line),ca=/cpu_avg=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line),cx=/cpu_max=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line);"
        "var rs=/rtc_samples=(\\d+)/i.exec(line),ra=/rtc_avg=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line),rx=/rtc_max=([-+]?[0-9]+(?:\\.[0-9]+)?) C/i.exec(line);"
        "var csn=cs?Number(cs[1]):0,cav=ca?Number(ca[1]):NaN,cxv=cx?Number(cx[1]):NaN;"
        "if(csn>0&&Number.isFinite(cav)){recCpuSamples+=csn;recCpuWeighted+=cav*csn;}"
        "if(Number.isFinite(cxv)){if(recCpuMax===null||cxv>recCpuMax)recCpuMax=cxv;if(thermalCpuMax===null||cxv>thermalCpuMax)thermalCpuMax=cxv;}"
        "var rsn=rs?Number(rs[1]):0,rav=ra?Number(ra[1]):NaN,rxv=rx?Number(rx[1]):NaN;"
        "if(rsn>0&&Number.isFinite(rav)){recRtcSamples+=rsn;recRtcWeighted+=rav*rsn;}"
        "if(Number.isFinite(rxv)){if(recRtcMax===null||rxv>recRtcMax)recRtcMax=rxv;if(thermalRtcMax===null||rxv>thermalRtcMax)thermalRtcMax=rxv;}"
        "}"
        "}"
        "}"
        "var periodMs=(first&&last&&last.ms>=first.ms)?last.ms-first.ms:null;"
        "var avgClip=clips?recordSeconds/clips:0;"
        "var modePct=(periodMs&&periodMs>0)?(modeMs/periodMs)*100:null;"
        "statText('statLogStart',first?first.label:'nicht verfügbar');"
        "statText('statLogEnd',last?last.label:'nicht verfügbar');"
        "statText('statPeriod',periodMs!==null?durationText(periodMs):'nicht verfügbar');"
        "statText('statSleep',sleepCycles?durationText(sleepMs):'noch keine Messdaten');"
        "var sleepNote=sleepCycles?(sleepCycles+' Zyklen · Ø '+durationText(sleepMs/Math.max(1,sleepCycles))+' · max '+durationText(sleepMax)):'SLEEP-Datensätze werden ab dieser Firmware exakt protokolliert';"
        "if(sleepUnknown)sleepNote+=' · '+sleepUnknown+' ohne Zeitwert';"
        "statText('statSleepNote',sleepNote);"
        "statText('statEvents',String(events));"
        "statText('statClips',String(clips));"
        "statText('statModeTime',modeEvents?durationText(modeMs):'--');"
        "statText('statModeNote',modeEvents?(modeEvents+' abgeschlossene Ereignisse'+(modePct!==null?' · '+modePct.toFixed(2)+' % des Log-Zeitraums':'')):'keine vollständig begrenzten Ereignisse');"
        "statText('statRecordTime',secondsText(recordSeconds));"
        "statText('statRecordNote','Summe der STOP-duration-Werte');"
        "statText('statClipAvg',clips?secondsText(avgClip):'--');"
        "statText('statClipNote',clips?('min '+secondsText(clipMin)+' · max '+secondsText(clipMax)):'keine abgeschlossenen Clips');"
        "statText('statFrames',String(frames));"
        "statText('statBoots',String(boots));"
        "statText('statBootNote','Brownout '+brownouts+' · WDT '+wdt);"
        "statText('statHealth',warnings+' / '+errors);"
        "statText('statHealthNote','Warnungen / Fehlerhinweise');"
        "statText('statSafety',String(safety));"
        "statText('statSafetyNote','SD-Recoveries '+sdRecoveries);"
        "var thermalWarnings=thermalWarnCpu+thermalWarnRtc;"
        "var thermalShutdowns=thermalEmergencyCpu+thermalEmergencyRtc+thermalEmergencyBoth;"
        "var recCpuAvg=recCpuSamples?recCpuWeighted/recCpuSamples:null;"
        "var recRtcAvg=recRtcSamples?recRtcWeighted/recRtcSamples:null;"
        "statText('statThermalWarnings',String(thermalWarnings));"
        "statText('statThermalWarningsNote','CPU '+thermalWarnCpu+' · RTC '+thermalWarnRtc+' · gezählt werden Warn-Episoden');"
        "statText('statThermalEmergency',String(thermalShutdowns));"
        "statText('statThermalEmergencyNote','CPU '+thermalEmergencyCpu+' · RTC '+thermalEmergencyRtc+' · beide '+thermalEmergencyBoth);"
        "statText('statThermalCpuMax',thermalCpuMax!==null?thermalCpuMax.toFixed(1)+' °C':'--');"
        "statText('statThermalCpuMaxNote',thermalCpuMax!==null?'höchster protokollierter CPU-Wert':'keine Thermal-Temperaturdaten im Log');"
        "statText('statThermalRtcMax',thermalRtcMax!==null?thermalRtcMax.toFixed(1)+' °C':'--');"
        "statText('statThermalRtcMaxNote',thermalRtcMax!==null?'höchster protokollierter RTC/Gehäuse-Wert':'keine RTC-Thermaldaten im Log');"
        "statText('statThermalCpuAvgRec',recCpuAvg!==null?recCpuAvg.toFixed(1)+' °C':'--');"
        "statText('statThermalCpuAvgRecNote',recCpuSamples?(recCpuSamples+' Samples · max '+(recCpuMax!==null?recCpuMax.toFixed(1)+' °C':'--')):'mit neuen RECORDING_SUMMARY-Einträgen verfügbar');"
        "statText('statThermalRtcAvgRec',recRtcAvg!==null?recRtcAvg.toFixed(1)+' °C':'--');"
        "statText('statThermalRtcAvgRecNote',recRtcSamples?(recRtcSamples+' Samples · max '+(recRtcMax!==null?recRtcMax.toFixed(1)+' °C':'--')):'mit RTC und neuen RECORDING_SUMMARY-Einträgen verfügbar');"
        "statText('statThermalRecoveries',String(thermalRecoveries));"
        "statText('statThermalRecoveriesNote','Warning-Recovery und Recovery nach Cooldown');"
        "statText('statThermalCoverage',String(recThermalEvents));"
        "statText('statThermalCoverageNote',recThermalEvents?('Aufnahmeereignisse · CPU '+recCpuSamples+' / RTC '+recRtcSamples+' Samples'):'ältere Logs enthalten noch keine Aufnahme-Thermalsummaries');"
        "lastHourBuckets=buckets;drawHourChart(buckets);"
        "if(analysisStatus){"
        "var msg=events+' Alarmereignisse · '+clips+' Clips';"
        "if(sleepCycles)msg+=' · '+sleepCycles+' Sleep-Zyklen';else msg+=' · Sleep ab neuer Firmware messbar';"
        "if(thermalWarnings||thermalShutdowns)msg+=' · Thermal '+thermalWarnings+' Warnungen / '+thermalShutdowns+' Abschaltungen';"
        "analysisStatus.textContent=msg;"
        "}"
        "analysisCalculated=true;"
        "}"
        "function render(scroll){"
            "var q=search.value.trim().toLowerCase();"
            "var shown=rawText;"
            "var matchCount=0,totalCount=0;"
            "if(rawText.length){totalCount=rawText.split(/\\r?\\n/).filter(function(x){return x.length>0;}).length;}"
            "if(q){"
                "var lines=rawText.split(/\\r?\\n/);"
                "var filtered=[];"
                "for(var i=0;i<lines.length;i++){if(lines[i].toLowerCase().indexOf(q)>=0)filtered.push(lines[i]);}"
                "matchCount=filtered.length;"
                "shown=filtered.join('\\n');"
                "filterMeta.textContent='Filter: '+matchCount+' von '+totalCount+' Zeilen';"
            "}else{filterMeta.textContent=totalCount?totalCount+' Zeilen':'';}"
            "out.textContent=shown.length?shown:(q?'(Keine Treffer)':'(Log ist leer)');"
            "if(scroll)scrollBottom();"
        "}"
        "function loadLog(scroll){"
            "if(loading)return;"
            "loading=true;reload.disabled=true;if(analyzeButton)analyzeButton.disabled=true;"
            "rawText='';logOffset=0;logGeneration='';analysisCalculated=false;markAnalysisDirty();"
            "out.textContent='Log wird geladen...';status.textContent=uiLoadingChunks;"
            "function loadNext(attempt){"
                "fetch('/log_chunk?offset='+encodeURIComponent(logOffset)+'&generation='+encodeURIComponent(logGeneration)+'&t='+Date.now(),{cache:'no-store'})"
                ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);"
                    "var reset=r.headers.get('X-Log-Reset')==='1';"
                    "var generation=r.headers.get('X-Log-Generation')||'';"
                    "var next=Number(r.headers.get('X-Log-Offset')||String(logOffset));"
                    "var size=Number(r.headers.get('X-Log-Size')||String(next));"
                    "var more=r.headers.get('X-Log-More')==='1';"
                    "return r.text().then(function(text){return {text:text,reset:reset,next:next,size:size,more:more,generation:generation};});})"
                ".then(function(x){"
                    "if(x.reset)rawText='';"
                    "if(x.text.length)rawText+=x.text;"
                    "logOffset=x.next;"
                    "logGeneration=x.generation||logGeneration;"
                    "if(Number.isFinite(x.size))sizeEl.textContent=byteSizeText(x.size);"
                    "if(x.more){"
                        "var pct=(Number.isFinite(x.size)&&x.size>0)?Math.min(100,Math.round((logOffset/x.size)*100)):0;"
                        "status.textContent=uiLoadingChunks+(pct?' '+pct+' %':'');"
                        "setTimeout(function(){loadNext(0);},25);"
                        "return;"
                    "}"
                    "loading=false;reload.disabled=false;if(analyzeButton)analyzeButton.disabled=false;"
                    "status.textContent='Aktualisiert';markAnalysisDirty();render(scroll);"
                "})"
                ".catch(function(err){"
                    "if(attempt<2){status.textContent=uiRetry+' ('+(attempt+2)+'/3)';setTimeout(function(){loadNext(attempt+1);},250*(attempt+1));return;}"
                    "loading=false;reload.disabled=false;if(analyzeButton)analyzeButton.disabled=true;"
                    "status.textContent='Fehler: '+err.message;"
                    "if(rawText.length){render(false);}else{out.textContent='Log konnte nicht geladen werden: '+err.message;}"
                    "markAnalysisDirty();"
                "});"
            "}"
            "loadNext(0);"
        "}"
        "function pollLog(){"
            "if(!live.checked||loading)return;"
            "loading=true;status.textContent='Prüfe neue Einträge...';"
            "fetch('/log_chunk?offset='+encodeURIComponent(logOffset)+'&generation='+encodeURIComponent(logGeneration)+'&flush=1&t='+Date.now(),{cache:'no-store'})"
            ".then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);"
                "var reset=r.headers.get('X-Log-Reset')==='1';"
                "var generation=r.headers.get('X-Log-Generation')||'';"
                "var next=Number(r.headers.get('X-Log-Offset')||String(logOffset));"
                "var size=Number(r.headers.get('X-Log-Size')||String(next));"
                "var more=r.headers.get('X-Log-More')==='1';"
                "return r.text().then(function(text){return {text:text,reset:reset,next:next,size:size,more:more,generation:generation};});})"
            ".then(function(x){"
                "if(x.reset)rawText='';"
                "if(x.text.length)rawText+=x.text;"
                "logOffset=x.next;"
                "logGeneration=x.generation||logGeneration;"
                "if(Number.isFinite(x.size))sizeEl.textContent=byteSizeText(x.size);"
                "status.textContent=x.text.length?'Neue Einträge':'Keine neuen Einträge';"
                "if(x.reset||x.text.length)markAnalysisDirty();"
                "render(x.text.length>0&&!search.value.trim());"
                "loading=false;"
                "if(x.more&&live.checked)setTimeout(pollLog,80);"
            "})"
            ".catch(function(err){loading=false;status.textContent='Live-Update Fehler: '+err.message;});"
        "}"
        "function scheduleLive(){"
            "if(liveTimer){clearInterval(liveTimer);liveTimer=0;}"
            "if(live.checked){pollLog();liveTimer=setInterval(pollLog,5000);status.textContent='Live-Update aktiv';}"
            "else status.textContent='Live-Update aus';"
        "}"
        "reload.addEventListener('click',function(){loadLog(true);});"
        "bottom.addEventListener('click',scrollBottom);"
        "search.addEventListener('input',function(){render(false);});"
        "live.addEventListener('change',scheduleLive);"
        "if(analyzeButton)analyzeButton.addEventListener('click',function(){if(loading)return;analyzeLog();});"
        "window.addEventListener('resize',function(){if(analysisCalculated)drawHourChart(lastHourBuckets);});"
        "loadLog(true);"
        "})();"
        "</script>";

    html += pageFooter();

    webServer().sendHeader(
        "Cache-Control",
        "no-store"
    );

    webServer().send(
        200,
        "text/html; charset=utf-8",
        html
    );
}



} // namespace


void webLogRegisterRoutes(
    WebServer &server,
    const WebLogUiHooks &uiHooks
)
{
    g_webServer = &server;
    g_uiHooks = uiHooks;

    server.on("/log", HTTP_GET, handleLog);
    server.on("/log_raw", HTTP_GET, handleLogRaw);
    server.on("/log_chunk", HTTP_GET, handleLogChunk);
    server.on("/log_clear", HTTP_POST, handleLogClear);
}
