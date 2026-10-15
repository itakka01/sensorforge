#include "webconfig_transport.h"

#include "config.h"
#include "logger.h"
#include "recorder.h"
#include "storage_guard.h"
#include "streamer.h"
#include "webconfig.h"

#include <math.h>

// Uses the exact grayscale transport-check camera path from the main firmware.
extern bool cameraMeasureTransportBlackReference(
    float &referenceMean,
    uint8_t &referenceP95,
    uint8_t sampleCount,
    String &error
);

namespace {

static WebServer *g_webServer = nullptr;
static WebConfigTransportUiHooks g_uiHooks = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};

static WebServer &serverRef()
{
    return *g_webServer;
}

static String htmlHeader()
{
    return g_uiHooks.htmlHeader
        ? g_uiHooks.htmlHeader()
        : String();
}

static String htmlFooter()
{
    return g_uiHooks.htmlFooter
        ? g_uiHooks.htmlFooter()
        : String();
}

static String pageInfoButton(
    const String &title,
    const String &info
)
{
    return g_uiHooks.pageInfoButton
        ? g_uiHooks.pageInfoButton(title, info)
        : String();
}

static void appendPageInfoUi(String &html)
{
    if (g_uiHooks.appendPageInfoUi)
        g_uiHooks.appendPageInfoUi(html);
}

static void scheduleReboot(uint32_t delayMs)
{
    if (g_uiHooks.scheduleReboot)
        g_uiHooks.scheduleReboot(delayMs);
}

#define server serverRef()

static bool transportConfigWriteToSd();


// -------------------------------------------------------------
// TRANSPORT CALIBRATION / SETTINGS PAGE
// -------------------------------------------------------------

static int transportClampByte(int value)
{
    if (value < 0)
        return 0;

    if (value > 255)
        return 255;

    return value;
}


static bool transportPauseRecordingForOperation(
    const char *operation
)
{
    return
        g_uiHooks.pauseRecordingForOperation &&
        g_uiHooks.pauseRecordingForOperation(operation);
}

static void renewRecordingPauseLease(bool transportHold)
{
    if (g_uiHooks.renewRecordingPauseLease)
        g_uiHooks.renewRecordingPauseLease(transportHold);
}

static bool rejectWhileRecording(const char *operation)
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



static void handleTransportMeasure()
{
    if (streamerModeEnabled()) {
        server.send(409, "text/plain; charset=utf-8", "Transport camera measurement is unavailable while streamer mode owns the camera");
        return;
    }

    if (!transportPauseRecordingForOperation("transport calibration"))
        return;

    String measurementSource =
        server.arg("source");

    measurementSource.trim();

    if (!measurementSource.length())
        measurementSource = "unknown";

    consoleWrite(
        "TRANSPORT",
        "Black-reference measurement started | source=" +
        measurementSource
    );

    logWrite(
        "Transport black-reference measurement started | source=" +
        measurementSource
    );

    if (webConfigCameraPreviewActive()) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Live Preview ist noch aktiv. Bitte Preview schließen und die Messung erneut starten."
        );
        return;
    }

    float referenceMean = 0.0f;
    uint8_t referenceP95 = 0;
    String measureError;

    const uint8_t sampleCount = 10;

    bool measured =
        cameraMeasureTransportBlackReference(
            referenceMean,
            referenceP95,
            sampleCount,
            measureError
        );

    // The measurement is synchronous. Refresh the dedicated transport lease only
    // after the camera has been fully restored.
    renewRecordingPauseLease(true);

    if (!measured) {
        String failure =
            measureError.length()
            ? measureError
            : String("Schwarzmessung fehlgeschlagen");

        consoleWrite(
            "TRANSPORT",
            "Black-reference measurement FAILED | source=" +
            measurementSource +
            " | " +
            failure
        );

        logWrite(
            "Transport black-reference measurement FAILED | source=" +
            measurementSource +
            " | " +
            failure
        );

        server.sendHeader("Cache-Control", "no-store");
        server.send(
            500,
            "text/plain; charset=utf-8",
            failure
        );
        return;
    }

    int suggestedMean =
        transportClampByte(
            (int)ceilf(referenceMean) + 10
        );

    int suggestedP95 =
        transportClampByte(
            (int)referenceP95 + 15
        );

    if (suggestedP95 < suggestedMean)
        suggestedP95 = suggestedMean;

    bool suspiciouslyBright =
        referenceMean > 40.0f ||
        referenceP95 > 70;

    String measurementSummary =
        "Transport black-reference measurement complete | source=" +
        measurementSource +
        " | samples=" +
        String((unsigned)sampleCount) +
        " | mean=" +
        String(referenceMean, 1) +
        " | p95=" +
        String((unsigned)referenceP95) +
        " | suggested_mean=" +
        String(suggestedMean) +
        " | suggested_p95=" +
        String(suggestedP95) +
        " | dark_check=" +
        String(suspiciouslyBright ? "WARN_BRIGHT" : "OK");

    consoleWrite(
        "TRANSPORT",
        measurementSummary
    );

    logWrite(
        measurementSummary
    );

    String json;
    json.reserve(192);
    json += "{\"reference_mean\":";
    json += String(referenceMean, 1);
    json += ",\"reference_p95\":";
    json += String((unsigned)referenceP95);
    json += ",\"suggested_mean\":";
    json += String(suggestedMean);
    json += ",\"suggested_p95\":";
    json += String(suggestedP95);
    json += ",\"suspiciously_bright\":";
    json += suspiciouslyBright ? "true" : "false";
    json += ",\"sample_count\":";
    json += String((unsigned)sampleCount);
    json += "}";

    server.sendHeader("Cache-Control", "no-store");
    server.send(
        200,
        "application/json",
        json
    );
}


static void handleTransportPage()
{
    bool justSaved =
        server.hasArg("notice") &&
        server.arg("notice") == "saved";

    int transportCheckTotalMinutes =
        (cfg_transport_check_seconds + 59) / 60;
    if (transportCheckTotalMinutes < 1) transportCheckTotalMinutes = 1;
    if (transportCheckTotalMinutes > 24 * 60) transportCheckTotalMinutes = 24 * 60;

    int transportCheckHours =
        transportCheckTotalMinutes / 60;
    int transportCheckMinutes =
        transportCheckTotalMinutes % 60;

    int transportMaxTotalMinutes =
        (cfg_transport_max_duration_seconds + 59) / 60;
    if (transportMaxTotalMinutes < 1) transportMaxTotalMinutes = 1;
    if (transportMaxTotalMinutes > 7 * 24 * 60)
        transportMaxTotalMinutes = 7 * 24 * 60;

    int transportMaxDays =
        transportMaxTotalMinutes / (24 * 60);
    int transportMaxRemainderMinutes =
        transportMaxTotalMinutes % (24 * 60);
    int transportMaxHours =
        transportMaxRemainderMinutes / 60;
    int transportMaxMinutes =
        transportMaxRemainderMinutes % 60;

    String html = htmlHeader();

    html +=
        "<div class='page-title'><div>"
        "<h2>Transportsicherung</h2>"
        "<p>Kamera abkleben, Grenzwerte prüfen, speichern und Transportmodus aktivieren.</p>"
        "</div></div>";

    if (justSaved) {
        html +=
            "<section class='settings-section' style='border-left:5px solid #087a00;background:#f3fbf2'>"
            "<b>Transportwerte gespeichert.</b>"
            "</section>";
    }

    html +=
        "<section class='settings-section'>"
        "<div style='display:flex;align-items:center;gap:6px;flex-wrap:wrap'><h3 style='margin:0'>Aktuelle Schwarzmessung</h3>" +
        pageInfoButton(
            "Aktuelle Schwarzmessung",
            "Beim Öffnen dieser Seite werden automatisch 10 Messframes mit derselben 160x120-Graustufen-Methode wie beim späteren Transport-Wake aufgenommen. Eine laufende Aufnahme wird vorher sauber beendet und die Aufnahmeautomatik während der Transportvorbereitung pausiert."
        ) +
        "</div>"
        "<div class='dashboard-grid'>"
        "<div class='dash-card'><div class='card-label'>Durchschnittliche Helligkeit</div>"
        "<div id='transportReferenceMean' class='card-value'>--</div>"
        "<div class='card-note'>höchster Durchschnittswert aus 10 Messbildern</div></div>"
        "<div class='dash-card'><div class='card-label'>Helle Bildbereiche (P95)</div>"
        "<div id='transportReferenceP95' class='card-value'>--</div>"
        "<div class='card-note'>erkennt hellere Bereiche, die im Durchschnitt untergehen können</div></div>"
        "<div class='dash-card'><div class='card-label'>Sicherheitsreserve</div><div class='card-value'>+10 / +15</div>"
        "<div class='card-note'>Reserve über den gemessenen Referenzwerten</div></div>"
        "</div>"
        "<p><span id='transportMeasureState' class='status-pill warn'>Messung wird vorbereitet ...</span></p>"
        "<p id='transportMeasureErrorText' class='muted' hidden></p>"
        "</section>"
        "<form id='transportSettingsForm' method='POST' action='/transport_save'>"
        "<style>"
        "#transportSettingsForm .transport-time-field{width:88px;min-width:88px;max-width:88px;box-sizing:border-box;}"
        "#transportSettingsForm .transport-time-row{display:flex;align-items:flex-end;gap:10px;flex-wrap:wrap;}"
        "#transportSettingsForm .transport-time-part{display:flex;flex-direction:column;gap:4px;}"
        "#transportSettingsForm .transport-time-part small{white-space:nowrap;}"
        "</style>"
        "<section class='settings-section'>"
        "<h3>Grenzwerte und Zeiten</h3>"
        "<p class='muted'>Die aktuelle Config-Version verwendet einen gemeinsamen Schwarzwert. "
        "Der automatische Vorschlag basiert auf der gemessenen durchschnittlichen Bildhelligkeit plus Reserve.</p>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportBlackThreshold'><b>Schwarzgrenze</b></label><br>"
        "<input id='transportBlackThreshold' name='transport_black_threshold' "
        "type='number' min='0' max='255' value='" +
        String(cfg_transport_black_threshold) +
        "'> <small id='transportThresholdSuggestion'>Automatischer Vorschlag wird gemessen ...</small>"
        "<div class='muted'>Wie hell das vollständig abgedeckte Bild noch sein darf. "
        "Ein kleinerer Wert bedeutet eine strengere Schwarzerkennung.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label><b>Prüfintervall während des Transports</b></label>"
        "<div class='transport-time-row' style='margin-top:6px'>"
        "<label class='transport-time-part'><select class='transport-time-field' name='transport_check_minutes'>";

    for (int minute = 0; minute <= 59; ++minute) {
        html +=
            "<option value='" + String(minute) + "'" +
            String(minute == transportCheckMinutes ? " selected" : "") +
            ">" + String(minute) + "</option>";
    }

    html +=
        "</select><small>Minuten</small></label>"
        "<label class='transport-time-part'><select class='transport-time-field' name='transport_check_hours'>";

    for (int hour = 0; hour <= 24; ++hour) {
        html +=
            "<option value='" + String(hour) + "'" +
            String(hour == transportCheckHours ? " selected" : "") +
            ">" + String(hour) + "</option>";
    }

    html +=
        "</select><small>Stunden</small></label>"
        "</div>"
        "<small>Standard: 10 Minuten</small>"
        "<div class='muted'>Nach diesem Abstand wacht SensorForge kurz auf und prüft, ob die Kamera weiterhin abgedeckt ist. "
        "Dazwischen bleibt das Gerät im stromsparenden Timer-Deep-Sleep.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportLightConfirmSeconds'><b>Bestätigungszeit nach erkannter Helligkeit</b></label><br>"
        "<input class='transport-time-field' id='transportLightConfirmSeconds' name='transport_light_confirm_seconds' type='number' min='0' max='120' value='" +
        String(cfg_transport_light_confirm_seconds) +
        "'> <small>Sekunden · Standard: 10 Sekunden</small>"
        "<div class='muted'>Die Abdeckung gilt erst als entfernt, wenn die Kamera über diesen Zeitraum mehrfach hell bleibt. "
        "Kurze Lichtblitze lösen den Transportmodus dadurch nicht versehentlich.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label for='transportInstallDelaySeconds'><b>Wartezeit nach Entfernen der Abdeckung</b></label><br>"
        "<input class='transport-time-field' id='transportInstallDelaySeconds' name='transport_install_delay_seconds' type='number' min='0' max='86400' value='" +
        String(cfg_transport_install_delay_seconds) +
        "'> <small>Sekunden</small>"
        "<div class='muted'>Zeit für Montage und Verlassen des Bildbereichs, bevor SensorForge wieder in den normalen Betrieb zurückkehrt.</div></div>";

    html +=
        "<div style='margin-bottom:16px'>"
        "<label><b>Maximale Transportdauer</b></label>"
        "<div class='transport-time-row' style='margin-top:6px'>"
        "<label class='transport-time-part'><select class='transport-time-field' name='transport_max_minutes'>";

    for (int minute = 0; minute <= 59; ++minute) {
        html +=
            "<option value='" + String(minute) + "'" +
            String(minute == transportMaxMinutes ? " selected" : "") +
            ">" + String(minute) + "</option>";
    }

    html +=
        "</select><small>Minuten</small></label>"
        "<label class='transport-time-part'><select class='transport-time-field' name='transport_max_hours'>";

    for (int hour = 0; hour <= 23; ++hour) {
        html +=
            "<option value='" + String(hour) + "'" +
            String(hour == transportMaxHours ? " selected" : "") +
            ">" + String(hour) + "</option>";
    }

    html +=
        "</select><small>Stunden</small></label>"
        "<label class='transport-time-part'><select class='transport-time-field' name='transport_max_days'>";

    for (int day = 0; day <= 7; ++day) {
        html +=
            "<option value='" + String(day) + "'" +
            String(day == transportMaxDays ? " selected" : "") +
            ">" + String(day) + "</option>";
    }

    html +=
        "</select><small>Tage</small></label>"
        "</div>"
        "<div class='muted'>Harte Sicherheitsgrenze für die gesamte Transportphase. Nach Ablauf wechselt SensorForge unabhängig von der Lichtmessung in den Normalbetrieb.</div></div>";

    html +=
        "<div class='form-actions'><button id='transportRemeasureButton' type='button'>Neu messen</button></div>"
        "</section>"
        "<div class='floating-save-space'></div>"
        "<div id='transportSaveBar' class='floating-save-bar'>"
        "<span id='transportSaveState' class='floating-save-state'>Keine ungespeicherten Änderungen</span>"
        "<button id='transportSaveButton' type='submit' disabled>Speichern</button>"
        "</div></form>";

    html +=
        "<section class='settings-section' style='border-left:5px solid #d97706'>"
        "<h3>Transportmodus</h3>"
        "<p>Status: <span class='status-pill " +
        String(cfg_transport_mode ? "warn" : "ok") +
        "'>" +
        String(cfg_transport_mode ? "AKTIV" : "AUS") +
        "</span></p>";

    if (!cfg_transport_mode) {
        html +=
            "<p class='muted'>Nach dem Aktivieren wird transport_mode=1 persistent gespeichert und SensorForge neu gestartet. "
            "Beim Boot springt die Firmware direkt in den Timer-only Transportpfad.</p>"
            "<form method='POST' action='/transport_activate' "
            "onsubmit=\"return confirm('Grenzwerte gespeichert und Kamera vollständig schwarz abgeklebt? Transportmodus jetzt aktivieren?');\">"
            "<button id='transportActivateButton' class='primary' type='submit'>Transportmodus aktivieren</button>"
            "</form>";
    } else {
        html +=
            "<form method='POST' action='/transport_cancel' "
            "onsubmit=\"return confirm('Transportmodus wirklich deaktivieren?');\">"
            "<button type='submit'>Transportmodus deaktivieren</button>"
            "</form>";
    }

    html +=
        "</section>";

    // This modal is intentionally present and visible in the very first HTML
    // response. The slow camera operation is started only afterwards via fetch,
    // so the browser can explain the delay instead of looking frozen.
    html +=
        "<style>"
        ".transport-spinner{width:34px;height:34px;margin:4px 0 12px;border:4px solid #d8dee6;"
        "border-top-color:#2563eb;border-radius:50%;animation:transportSpin .8s linear infinite;}"
        "@keyframes transportSpin{to{transform:rotate(360deg);}}"
        "</style>"
        "<div id='transportMeasureModal' class='modal-backdrop'>"
        "<div class='modal-card' role='dialog' aria-modal='true' aria-labelledby='transportMeasureTitle'>"
        "<span class='status-pill warn'>KAMERA-MESSUNG</span>"
        "<div id='transportMeasureSpinner' class='transport-spinner' aria-hidden='true'></div>"
        "<h3 id='transportMeasureTitle'>Messung wird durchgeführt ...</h3>"
        "<p id='transportMeasureText'>Eine laufende Aufnahme wird zuerst sauber beendet. Anschließend wird der "
        "Schwarzwert der abgedeckten Kamera gemessen. Bitte Kamera abgedeckt lassen. Dies kann einige Sekunden dauern.</p>"
        "<div class='modal-actions'><button id='transportMeasureCloseButton' type='button' hidden>Schließen</button></div>"
        "</div></div>";

    html +=
        "<script>"
        "(function(){"
        "var modal=document.getElementById('transportMeasureModal');"
        "var spinner=document.getElementById('transportMeasureSpinner');"
        "var title=document.getElementById('transportMeasureTitle');"
        "var text=document.getElementById('transportMeasureText');"
        "var closeBtn=document.getElementById('transportMeasureCloseButton');"
        "var transportForm=document.getElementById('transportSettingsForm');"
        "var saveBar=document.getElementById('transportSaveBar');"
        "var saveState=document.getElementById('transportSaveState');"
        "var remeasureBtn=document.getElementById('transportRemeasureButton');"
        "var saveBtn=document.getElementById('transportSaveButton');"
        "var activateBtn=document.getElementById('transportActivateButton');"
        "var meanEl=document.getElementById('transportReferenceMean');"
        "var p95El=document.getElementById('transportReferenceP95');"
        "var stateEl=document.getElementById('transportMeasureState');"
        "var errorEl=document.getElementById('transportMeasureErrorText');"
        "var thresholdInput=document.getElementById('transportBlackThreshold');"
        "var thresholdSuggestion=document.getElementById('transportThresholdSuggestion');"
        "var preserveSavedValues=" + String(justSaved ? "true" : "false") + ";"
        "var measuring=false,dirty=false;"
        "function markDirty(){dirty=true;if(saveBar)saveBar.classList.add('dirty');if(saveState)saveState.textContent='Ungespeicherte Änderungen';if(saveBtn&&!measuring)saveBtn.disabled=false;}"
        "function setControlsDisabled(v){"
        "if(remeasureBtn)remeasureBtn.disabled=v;"
        "if(saveBtn)saveBtn.disabled=v||!dirty;"
        "if(activateBtn)activateBtn.disabled=v;"
        "}"
        "function showBusy(){"
        "measuring=true;setControlsDisabled(true);"
        "if(modal)modal.hidden=false;if(spinner)spinner.hidden=false;if(closeBtn)closeBtn.hidden=true;"
        "if(title)title.textContent='Messung wird durchgeführt ...';"
        "if(text)text.textContent='Eine laufende Aufnahme wird zuerst sauber beendet. Anschließend wird der Schwarzwert der abgedeckten Kamera gemessen. Bitte Kamera abgedeckt lassen. Dies kann einige Sekunden dauern.';"
        "if(stateEl){stateEl.className='status-pill warn';stateEl.textContent='Messung läuft ...';}"
        "if(errorEl){errorEl.hidden=true;errorEl.textContent='';}"
        "}"
        "function finishControls(){measuring=false;setControlsDisabled(false);}"
        "function hideModal(){if(modal)modal.hidden=true;}"
        "function applyMeasurement(d,applySuggestions){"
        "var m=Number(d.reference_mean),p=Number(d.reference_p95),sm=Number(d.suggested_mean),sp=Number(d.suggested_p95),bright=!!d.suspiciously_bright;"
        "if(meanEl)meanEl.textContent=isFinite(m)?m.toFixed(1):'--';"
        "if(p95El)p95El.textContent=isFinite(p)?String(p):'--';"
        "if(thresholdSuggestion)thresholdSuggestion.textContent=bright?'Kein automatischer Vorschlag: Messung ungewöhnlich hell.':'Automatischer Vorschlag: '+sm+' (gemessene Durchschnittshelligkeit + 10 Reserve)';"
        "if(applySuggestions&&!bright&&thresholdInput){var next=String(sm);if(thresholdInput.value!==next){thresholdInput.value=next;markDirty();}}"
        "if(stateEl){stateEl.className='status-pill '+(bright?'warn':'ok');"
        "stateEl.textContent=bright?'Abgedecktes Bild ungewöhnlich hell - gespeicherter Grenzwert bleibt unverändert':'Messung plausibel dunkel';}"
        "}"
        "function measurementFailed(message){"
        "if(stateEl){stateEl.className='status-pill danger';stateEl.textContent='Schwarzmessung fehlgeschlagen';}"
        "if(errorEl){errorEl.hidden=false;errorEl.textContent=message||'Unbekannter Fehler';}"
        "if(spinner)spinner.hidden=true;if(closeBtn)closeBtn.hidden=false;"
        "if(title)title.textContent='Messung fehlgeschlagen';"
        "if(text)text.textContent=message||'Die Schwarzmessung konnte nicht durchgeführt werden. Die gespeicherten Grenzwerte bleiben unverändert.';"
        "}"
        "function runMeasurement(applySuggestions,source){"
        "if(measuring)return;showBusy();"
        "fetch('/transport_measure?source='+encodeURIComponent(source||'unknown')+'&t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin'})"
        ".then(function(r){if(!r.ok)return r.text().then(function(t){throw new Error(t||('HTTP '+r.status));});return r.json();})"
        ".then(function(d){applyMeasurement(d,applySuggestions);hideModal();finishControls();})"
        ".catch(function(e){measurementFailed(e&&e.message?e.message:String(e));finishControls();});"
        "}"
        "function keepTransportPause(){"
        "fetch('/recording_pause_keepalive?transport=1&t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "function releaseTransportHold(){"
        "fetch('/transport_pause_release?t='+Date.now(),{method:'POST',cache:'no-store',credentials:'same-origin',keepalive:true}).catch(function(){});"
        "}"
        "if(closeBtn)closeBtn.addEventListener('click',hideModal);"
        "if(remeasureBtn)remeasureBtn.addEventListener('click',function(){runMeasurement(true,'manual-remeasure');});"
        "if(transportForm){transportForm.addEventListener('input',markDirty);transportForm.addEventListener('change',markDirty);transportForm.addEventListener('submit',function(){if(saveBtn)saveBtn.disabled=true;if(saveState)saveState.textContent='Speichert …';});}"
        // Unlike the normal maintenance pause, the transport preparation page
        // also renews while hidden. Closing/navigating away stops this timer and
        // the existing 35 s safety timeout resumes automatic recording.
        "keepTransportPause();"
        "setInterval(keepTransportPause,10000);"
        "window.addEventListener('pagehide',releaseTransportHold);"
        "setTimeout(function(){runMeasurement(!preserveSavedValues,'page-open');},0);"
        "})();"
        "</script>";

    appendPageInfoUi(html);
    html += htmlFooter();

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}


static void handleTransportSave()
{
    if (!transportPauseRecordingForOperation("transport settings save"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    int checkSeconds = 0;

    if (
        server.hasArg("transport_check_minutes") ||
        server.hasArg("transport_check_hours")
    ) {
        int checkMinutes =
            server.arg("transport_check_minutes").toInt();
        int checkHours =
            server.arg("transport_check_hours").toInt();

        if (checkMinutes < 0) checkMinutes = 0;
        if (checkHours < 0) checkHours = 0;

        checkSeconds =
            (checkHours * 60 + checkMinutes) * 60;
    } else {
        // Backward compatibility for an already open Beta-31 browser tab.
        checkSeconds =
            server.arg("transport_check_seconds").toInt();
    }

    int lightConfirmSeconds =
        server.arg("transport_light_confirm_seconds").toInt();

    int installDelaySeconds =
        server.arg("transport_install_delay_seconds").toInt();

    int maxDurationSeconds = 0;

    if (
        server.hasArg("transport_max_minutes") ||
        server.hasArg("transport_max_hours") ||
        server.hasArg("transport_max_days")
    ) {
        int maxMinutes =
            server.arg("transport_max_minutes").toInt();
        int maxHours =
            server.arg("transport_max_hours").toInt();
        int maxDays =
            server.arg("transport_max_days").toInt();

        if (maxMinutes < 0) maxMinutes = 0;
        if (maxHours < 0) maxHours = 0;
        if (maxDays < 0) maxDays = 0;

        maxDurationSeconds =
            (maxDays * 24 * 60 + maxHours * 60 + maxMinutes) * 60;
    } else {
        // Backward compatibility for an already open Beta-31 browser tab.
        maxDurationSeconds =
            server.arg("transport_max_duration_seconds").toInt();
    }

    int blackThreshold =
        server.arg("transport_black_threshold").toInt();

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportSettings(
            checkSeconds,
            lightConfirmSeconds,
            installDelaySeconds,
            maxDurationSeconds,
            blackThreshold,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            error.length()
                ? error
                : String("Transportwerte konnten nicht gespeichert werden")
        );
        return;
    }

    String savedSummary =
        "Transport settings saved | black<=" +
        String(blackThreshold) +
        " | check=" +
        String(checkSeconds) +
        " s | light_confirm=" +
        String(lightConfirmSeconds) +
        " s | install_delay=" +
        String(installDelaySeconds) +
        " s | max_duration=" +
        String(maxDurationSeconds) +
        " s";

    consoleWrite(
        "TRANSPORT",
        savedSummary
    );

    logWrite(
        savedSummary
    );

    server.sendHeader(
        "Location",
        "/transport?notice=saved"
    );

    server.send(
        303,
        "text/plain; charset=utf-8",
        ""
    );
}


// -------------------------------------------------------------
// TRANSPORT MODE
// -------------------------------------------------------------

static bool transportConfigWriteToSd()
{
    configRefreshSdStatus();

    return
        configSdAvailable() &&
        configSdPresent();
}


static void sendTransportTransitionPage(
    bool activating
)
{
    String html =
        htmlHeader();

    html +=
        "<div class='page-title'><div><h2>" +
        String(
            activating
            ? "Transportmodus aktiviert"
            : "Transportmodus deaktiviert"
        ) +
        "</h2></div></div>";

    html +=
        "<section class='settings-section' style='text-align:center'>";

    if (activating) {
        html +=
            "<p><b>Kamera muss jetzt vollständig schwarz abgeklebt sein.</b></p>"
            "<p class='muted'>SensorForge startet neu. Radar/PIR werden danach nicht als Wake-Quelle verwendet; WLAN bleibt aus. "
            "Das Gerät wacht nur per Timer auf, prüft die Kamera und schläft bei weiterhin schwarzem Bild wieder ein. "
            "Nach bestätigtem Entfernen der Abdeckung läuft die konfigurierte Installations-Wartezeit ab. "
            "Spätestens nach der maximalen Transportdauer wird unabhängig von der Lichtmessung in den Normalbetrieb gewechselt.</p>";
    } else {
        html +=
            "<p><b>Transportmodus ist deaktiviert.</b></p>"
            "<p class='muted'>SensorForge startet neu und kehrt in den normalen Betrieb zurück.</p>";
    }

    html +=
        "<div class='countdown' id='transportCountdown'>3</div>"
        "<div class='muted'>Sekunden bis zum Neustart</div>"
        "</section>"
        "<script>"
        "(function(){var s=3;var e=document.getElementById('transportCountdown');"
        "setInterval(function(){if(s>0)s--;if(e)e.textContent=s;},1000);})();"
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


static void handleTransportActivate()
{
    if (!transportPauseRecordingForOperation("transport mode activation"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportMode(
            1,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Transportmodus konnte nicht gespeichert werden")
        );
        return;
    }

    consoleWrite(
        "TRANSPORT",
        "Transport mode activated - reboot scheduled"
    );

    logWrite(
        "Transport mode activated | check=" +
        String(cfg_transport_check_seconds) +
        " s | light_confirm=" +
        String(cfg_transport_light_confirm_seconds) +
        " s | install_delay=" +
        String(cfg_transport_install_delay_seconds) +
        " s | max_duration=" +
        String(cfg_transport_max_duration_seconds) +
        " s | black<=" +
        String(cfg_transport_black_threshold)
    );

    sendTransportTransitionPage(
        true
    );

    scheduleReboot(3000UL);
}


static void handleTransportCancel()
{
    if (rejectWhileRecording("transport mode cancel"))
        return;

    if (g_storageLocked) {
        server.send(
            409,
            "text/plain; charset=utf-8",
            "Storage ist momentan gesperrt"
        );
        return;
    }

    configRefreshSdStatus();

    String error;

    ConfigSaveResult result =
        configSaveTransportMode(
            0,
            transportConfigWriteToSd(),
            error
        );

    if (
        result != CONFIG_SAVE_BOTH &&
        result != CONFIG_SAVE_INTERNAL_ONLY
    ) {
        server.send(
            500,
            "text/plain; charset=utf-8",
            error.length()
            ? error
            : String("Transportmodus konnte nicht deaktiviert werden")
        );
        return;
    }

    consoleWrite(
        "TRANSPORT",
        "Transport mode cancelled - reboot scheduled"
    );

    logWrite(
        "Transport mode cancelled"
    );

    sendTransportTransitionPage(
        false
    );

    scheduleReboot(3000UL);
}



#undef server

} // namespace

void webconfigTransportRegisterRoutes(
    WebServer &server,
    const WebConfigTransportUiHooks &uiHooks
)
{
    g_webServer = &server;
    g_uiHooks = uiHooks;

    server.on("/transport", HTTP_GET, handleTransportPage);
    server.on("/transport_measure", HTTP_POST, handleTransportMeasure);
    server.on("/transport_save", HTTP_POST, handleTransportSave);
    server.on("/transport_activate", HTTP_POST, handleTransportActivate);
    server.on("/transport_cancel", HTTP_POST, handleTransportCancel);
}
