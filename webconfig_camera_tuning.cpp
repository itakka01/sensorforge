#include "webconfig_camera_tuning.h"

#include "access_control.h"
#include "camera_tuning.h"
#include "cluster.h"
#include "cluster_camera.h"
#include "cluster_capture.h"
#include "config.h"

extern int cfg_web_auth_enabled;

namespace {

WebServer *g_server = nullptr;
String (*g_header)() = nullptr;
String (*g_footer)() = nullptr;

bool requireAdmin()
{
    if (cfg_web_auth_enabled && accessControlWebRole(*g_server) != SENSORFORGE_ACCESS_ADMIN) {
        g_server->send(403, "text/plain; charset=utf-8", "Administrator access required");
        return false;
    }
    return true;
}

// GET /camera_frame[?size=small|medium|full]: fresh JPEG, small (~1/8, the
// default), medium (about 800 px wide) or original; full=1 = size=full.
void handleCameraFrame()
{
    if (!requireAdmin())
        return;
    const uint8_t *data = nullptr;
    size_t length = 0;
    String reason;
    const String size = g_server->arg("size");
    uint16_t minWidth = kLiveFrameSmall;
    if (size == "full" || g_server->arg("full") == "1")
        minWidth = 0;
    else if (size == "medium")
        minWidth = kLiveFrameMedium;
    if (!clusterCaptureLiveFrame(minWidth, data, length, reason)) {
        g_server->sendHeader("Cache-Control", "no-store");
        g_server->send(409, "text/plain; charset=utf-8", reason);
        return;
    }
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->setContentLength(length);
    g_server->send(200, "image/jpeg", "");
    g_server->client().write(data, length);
}

int argInt(WebServer &srv, const char *name, int fallback)
{
    return srv.hasArg(name) ? srv.arg(name).toInt() : fallback;
}

// Form -> values; false (error) on a missing or out-of-range field.
bool readForm(WebServer &srv, int &autoExposure, int &aeLevel, CameraTuning::Values &t, String &error)
{
    static const char *const kFields[] = {"aec", "ae_level", "aec_value", "agc", "agc_gain", "gainceiling",
                                          "awb", "wb_mode", "awb_gain", "brightness", "contrast", "saturation"};
    for (const char *f : kFields) {
        if (!srv.hasArg(f)) {
            error = String("Feld fehlt: ") + f;
            return false;
        }
    }
    autoExposure = argInt(srv, "aec", 1);
    aeLevel = argInt(srv, "ae_level", 0);
    t.set = true;
    t.aecValue = argInt(srv, "aec_value", 300);
    t.autoGain = argInt(srv, "agc", 1);
    t.agcGain = argInt(srv, "agc_gain", 0);
    t.gainCeiling = argInt(srv, "gainceiling", 0);
    t.awb = argInt(srv, "awb", 1);
    t.wbMode = argInt(srv, "wb_mode", 0);
    t.awbGain = argInt(srv, "awb_gain", 1);
    t.brightness = argInt(srv, "brightness", 0);
    t.contrast = argInt(srv, "contrast", 0);
    t.saturation = argInt(srv, "saturation", 0);
    if ((autoExposure != 0 && autoExposure != 1) || aeLevel < -2 || aeLevel > 2 || !CameraTuning::valid(t)) {
        error = "Wert außerhalb des gültigen Bereichs";
        return false;
    }
    return true;
}

String jsonEscaped(const String &text)
{
    String m = text;
    m.replace("\\", "\\\\");
    m.replace("\"", "\\\"");
    return m;
}

int result(String &json, int code, bool ok, const String &message)
{
    json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscaped(message) + "\"}";
    return code;
}

// POST /camera_tuning_apply
void handleCameraTuningApply()
{
    if (!requireAdmin())
        return;
    String json;
    const int code = cameraTuningRequestFromArgs(*g_server, json);
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(code, "application/json; charset=utf-8", json);
}

String opt(int value, int selected, const String &label)
{
    return "<option value='" + String(value) + "'" + (value == selected ? " selected" : "") + ">" + label + "</option>";
}

String resolutionText(int32_t framesize)
{
    uint16_t w = 0, h = 0;
    return ClusterCamera::framesizeDims(framesize, w, h) ? String(w) + "×" + String(h) : String("–");
}

int known(int32_t v, int fallback) { return v == ClusterCamera::kUnknown ? fallback : (int)v; }

void handleCameraTuningPage()
{
    if (!requireAdmin())
        return;
    using namespace ClusterCamera;
    Report r;
    cameraFillClusterReport(r);
    CameraTuning::Values saved;
    CameraTuning::parse(cfg_camera_tuning.c_str(), saved);
    const CameraTuning::Values d;
    const int aec = known(r.aec, cfg_camera_auto_exposure);
    const int aeLevel = known(r.aeLevel, cfg_camera_ae_level);
    const int aecValue = known(r.aecValue, saved.set ? saved.aecValue : d.aecValue);
    const int agc = known(r.agc, saved.set ? saved.autoGain : d.autoGain);
    const int agcGain = known(r.agcGain, saved.set ? saved.agcGain : d.agcGain);
    const int ceiling = known(r.gainCeiling, saved.set ? saved.gainCeiling : d.gainCeiling);
    const int awb = known(r.awb, saved.set ? saved.awb : d.awb);
    const int wbMode = known(r.wbMode, saved.set ? saved.wbMode : d.wbMode);
    const int awbGain = known(r.awbGain, saved.set ? saved.awbGain : d.awbGain);
    const int bri = known(r.brightness, 0), con = known(r.contrast, 0), sat = known(r.saturation, 0);
    const uint32_t caps = r.capMask == kUnknown ? 0 : (uint32_t)r.capMask;
    auto dis = [&](uint32_t bit) { return (caps && !(caps & bit)) ? String(" disabled title='Vom Sensor nicht unterstützt'") : String(); };

    String html = g_header();
    html.reserve(9000);
    html += "<div class='page-title'><div><h2>Bildabstimmung</h2><p>Feste Belichtung, Verstärkung und Weißabgleich – "
            "Grundlage für einheitliche Cluster-Kameras (Gaussian Splats)</p></div></div>";
    html += "<style>.tune-grid{display:grid;grid-template-columns:minmax(150px,220px) minmax(120px,1fr);gap:8px 12px;align-items:center}"
            ".tune-grid label{font-weight:700;margin:0}.tune-grid input,.tune-grid select{margin:0;max-width:240px}</style>";
    html += "<section class='settings-section'><p class='muted'>Sensor: <strong>" + String(sensorName(r.pid)) +
            "</strong> · Auflösung " + resolutionText(r.framesize) +
            (saved.set ? " · gespeicherte Abstimmung aktiv" : " · keine gespeicherte Abstimmung (Treiberstandard)") +
            "</p>" +
            (r.profileState == (int32_t)ProfileState::Active
                 ? String("<p class='flash-notice'>Cluster-Kameraprofil aktiv (Drone Mode). „Anwenden“ ändert die "
                          "laufende Kamera bis zum nächsten Kamerastart; „Dauerhaft speichern“ ändert nur die lokalen "
                          "Werte, die ohne Profil gelten. Ist dieses Gerät die Referenzkamera, danach am Coordinator "
                          "erneut übertragen.</p>")
                 : String()) +
            "<p class='muted'>Für gleiche Bilder auf allen Kameras: Belichtungs- und Verstärkungsautomatik aus, "
            "Weißabgleich auf eine feste Voreinstellung (nicht „Auto“). Der Belichtungswert zählt in Sensorzeilen; "
            "er ist nur zwischen Kameras mit gleichem Sensor und gleicher Auflösung vergleichbar.</p>";
    html += "<div style='display:flex;flex-wrap:wrap;gap:16px;align-items:flex-start'>"
            "<div style='flex:1 1 360px;min-width:0'><img id='tuneImg' alt='Live-Bild' "
            "style='display:block;width:100%;max-height:70vh;object-fit:contain;background:#1f2937;border-radius:8px'>"
            "<div style='margin-top:6px'><label><input type='checkbox' id='tuneLive' checked> Live</label> "
            "<label>Bildgröße <select id='tuneSize'><option value='small'>Klein</option>"
            "<option value='medium' selected>Mittel (ca. 800 px)</option><option value='full'>Voll</option></select></label> "
            "<span id='tuneImgState' class='muted'></span></div></div>";
    html += "<form id='tuneForm' style='flex:1 1 300px;min-width:0' onsubmit='return false'><div class='tune-grid'>";
    html += "<label>Belichtungsautomatik</label><select name='aec'" + dis(CapManualExposure) + ">" + opt(1, aec, "An") + opt(0, aec, "Aus") + "</select>";
    html += "<label>AE-Korrektur (bei Automatik)</label><input name='ae_level' type='number' min='-2' max='2' value='" + String(aeLevel) + "'" + dis(CapAeLevel) + ">";
    html += "<label>Belichtungswert (Zeilen, bei Aus)</label><input name='aec_value' type='number' min='0' max='" + String(r.pid == 0x26 ? CameraTuning::kAecMaxOv2640 : CameraTuning::kAecMax) + "' value='" + String(aecValue) + "'" + dis(CapManualExposure) + ">";
    html += "<label>Verstärkungsautomatik</label><select name='agc'" + dis(CapManualGain) + ">" + opt(1, agc, "An") + opt(0, agc, "Aus") + "</select>";
    html += "<label>Verstärkung (0–30, bei Aus)</label><input name='agc_gain' type='number' min='0' max='" + String(CameraTuning::kAgcMax) + "' value='" + String(agcGain) + "'" + dis(CapManualGain) + ">";
    html += "<label>Verstärkungsobergrenze (bei An)</label><select name='gainceiling'" + dis(CapGainCeiling) + ">";
    for (int i = 0; i <= CameraTuning::kGainCeilingMax; ++i)
        html += opt(i, ceiling, String(2 << i) + "×");
    html += "</select>";
    html += "<label>Weißabgleich-Automatik</label><select name='awb'" + dis(CapAutoWb) + ">" + opt(1, awb, "An") + opt(0, awb, "Aus") + "</select>";
    html += "<label>Weißabgleich-Voreinstellung</label><select name='wb_mode'" + dis(CapWbPresets) + ">" + opt(0, wbMode, "Auto") +
            opt(1, wbMode, "Sonne") + opt(2, wbMode, "Wolken") + opt(3, wbMode, "Büro") + opt(4, wbMode, "Glühlampe") + "</select>";
    html += "<label>AWB-Gain</label><select name='awb_gain'" + dis(CapAwbGain) + ">" + opt(1, awbGain, "An") + opt(0, awbGain, "Aus") + "</select>";
    html += "<label>Helligkeit</label><input name='brightness' type='number' min='-2' max='2' value='" + String(bri) + "'" + dis(CapBrightness) + ">";
    html += "<label>Kontrast</label><input name='contrast' type='number' min='-2' max='2' value='" + String(con) + "'" + dis(CapContrast) + ">";
    html += "<label>Sättigung</label><input name='saturation' type='number' min='-2' max='2' value='" + String(sat) + "'" + dis(CapSaturation) + ">";
    html += "</div><p><button type='button' data-mode='session'>Anwenden (bis Neustart)</button> "
            "<button type='button' data-mode='save'>Dauerhaft speichern</button> "
            "<button type='button' data-mode='reset'>Gespeicherte Abstimmung entfernen</button></p>"
            "<p id='tuneState' class='muted' role='status'></p>"
            "<p class='muted'>„Anwenden“ ändert die laufende Kamera sofort. „Dauerhaft speichern“ wirkt auch nach jedem "
            "Kamerastart. Während einer geplanten Cluster-Aufnahme werden Änderungen abgelehnt.</p></form></div></section>";
    html += R"__TUNE__(<script>(function(){
var img=document.getElementById('tuneImg'),live=document.getElementById('tuneLive'),size=document.getElementById('tuneSize'),ist=document.getElementById('tuneImgState'),st=document.getElementById('tuneState'),form=document.getElementById('tuneForm'),busy=false,again=false,timer=null;
function next(){if(timer){clearTimeout(timer);timer=null;}if(live.checked&&!document.hidden)timer=setTimeout(load,800);}
function load(){if(busy){again=true;return;}busy=true;again=false;var u='/camera_frame?size='+size.value+'&t='+Date.now();
 fetch(u,{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t||('HTTP '+r.status));});return r.blob();})
 .then(function(b){var o=img.src;img.src=URL.createObjectURL(b);if(o&&o.indexOf('blob:')===0)URL.revokeObjectURL(o);ist.textContent=new Date().toLocaleTimeString('de-DE')+' · '+Math.round(b.size/1024)+' KB';})
 .catch(function(e){ist.textContent='Kein Bild: '+(e.message||e);}).then(function(){busy=false;if(again)load();else next();});}
live.addEventListener('change',function(){if(live.checked)load();});size.addEventListener('change',load);
document.addEventListener('visibilitychange',function(){if(!document.hidden&&live.checked)load();});
form.addEventListener('click',function(e){var b=e.target.closest('button[data-mode]');if(!b)return;var m=b.getAttribute('data-mode');
 if(m==='reset'&&!confirm('Gespeicherte Abstimmung entfernen? Die Kamera nutzt nach dem nächsten Neustart wieder den Treiberstandard.'))return;
 // Disabled (unsupported) fields are sent too; the server needs a complete set.
 var fd=new URLSearchParams();Array.from(form.elements).forEach(function(el){if(el.name)fd.append(el.name,el.value);});fd.append('mode',m);st.textContent='Wird übertragen …';
 fetch('/camera_tuning_apply',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(function(r){return r.json().catch(function(){return {ok:false,message:'HTTP '+r.status};});})
 .then(function(j){st.textContent=(j.ok?'✓ ':'✗ ')+j.message;if(j.ok)load();}).catch(function(e){st.textContent='✗ '+(e.message||e);});});
load();
})();</script>)__TUNE__";
    html += g_footer();
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "text/html; charset=utf-8", html);
}

} // namespace

int cameraTuningRequestFromArgs(WebServer &srv, String &json)
{
    if (clusterCaptureProtectionActive())
        return result(json, 409, false, "Cluster-Aufnahme geplant – bitte danach erneut.");
    const String mode = srv.arg("mode");
    int autoExposure = cfg_camera_auto_exposure, aeLevel = cfg_camera_ae_level;
    CameraTuning::Values t;
    String error;
    if (mode == "reset") {
        // Driver defaults return with the next camera start.
    } else if (!readForm(srv, autoExposure, aeLevel, t, error)) {
        return result(json, 400, false, error);
    }
    if (mode == "save" || mode == "reset") {
        char text[80];
        CameraTuning::format(t, text, sizeof(text));
        configRefreshSdStatus();
        const bool writeToSd = configSdAvailable() && configSdPresent();
        const ConfigSaveResult saved = configSaveCameraTuning(autoExposure, aeLevel, String(text), writeToSd, error);
        if (saved != CONFIG_SAVE_BOTH && saved != CONFIG_SAVE_INTERNAL_ONLY)
            return result(json, 500, false, "Speichern fehlgeschlagen: " + error);
        if (mode == "reset")
            return result(json, 200, true, "Gespeicherte Abstimmung entfernt. Treiberstandard gilt nach dem nächsten Kamerastart.");
    } else if (mode != "session") {
        return result(json, 400, false, "mode muss session, save oder reset sein");
    }
    if (!cameraApplyTuningRuntime(autoExposure, aeLevel, t, error)) {
        if (mode == "save")
            return result(json, 200, true, "Gespeichert; wirkt beim nächsten Kamerastart (" + error + ").");
        return result(json, 409, false, error);
    }
    return result(json, 200, true, mode == "save" ? "Gespeichert und angewendet." : "Angewendet (bis zum Neustart).");
}

String cameraTuningJson()
{
    using namespace ClusterCamera;
    Report r;
    cameraFillClusterReport(r);
    CameraTuning::Values saved;
    CameraTuning::parse(cfg_camera_tuning.c_str(), saved);
    auto num = [](int32_t v) { return v == kUnknown ? String("null") : String((long)v); };
    char savedText[80];
    CameraTuning::format(saved, savedText, sizeof(savedText));
    char version[12];
    snprintf(version, sizeof(version), "%lx", (unsigned long)(uint32_t)r.profileVersion);
    static const char *const profileStates[] = {"none", "active", "waiting_drone", "sensor_mismatch", "pending"};
    String json = "{\"ok\":true,\"sensor\":\"" + String(sensorName(r.pid)) + "\",\"camera_on\":" +
        String(r.fromSensor == 1 ? "true" : "false") + ",\"resolution\":\"" + resolutionText(r.framesize) + "\"";
    json += ",\"current\":{\"aec\":" + num(r.aec) + ",\"ae_level\":" + num(r.aeLevel) +
        ",\"aec_value\":" + num(r.aecValue) + ",\"agc\":" + num(r.agc) + ",\"agc_gain\":" + num(r.agcGain) +
        ",\"gainceiling\":" + num(r.gainCeiling) + ",\"awb\":" + num(r.awb) + ",\"wb_mode\":" + num(r.wbMode) +
        ",\"awb_gain\":" + num(r.awbGain) + ",\"brightness\":" + num(r.brightness) +
        ",\"contrast\":" + num(r.contrast) + ",\"saturation\":" + num(r.saturation) + "}";
    json += ",\"saved\":{\"camera_auto_exposure\":" + String(cfg_camera_auto_exposure) +
        ",\"camera_ae_level\":" + String(cfg_camera_ae_level) + ",\"camera_tuning\":\"" + String(savedText) + "\"}";
    json += ",\"cluster_profile\":{\"version\":" +
        (r.profileVersion > 0 ? "\"" + String(version) + "\"" : String("null")) + ",\"state\":\"" +
        String(r.profileState >= 0 && r.profileState <= 4 ? profileStates[r.profileState] : "none") + "\"}}";
    return json;
}

void webconfigCameraTuningRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)())
{
    g_server = &server;
    g_header = htmlHeader;
    g_footer = htmlFooter;
    server.on("/camera_frame", HTTP_GET, handleCameraFrame);
    server.on("/camera_tuning", HTTP_GET, handleCameraTuningPage);
    server.on("/camera_tuning_apply", HTTP_POST, handleCameraTuningApply);
}
