#include "webconfig_camera_tuning.h"

#include "access_control.h"
#include "camera_tuning.h"
#include "cluster.h"
#include "cluster_camera.h"
#include "cluster_capture.h"
#include "config.h"
#include <WiFi.h>

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
    // Timing for measurements: grab, thumbnail decode/encode (µs), sizes,
    // path 1 = esp_new_jpeg, 2 = fallback, 0 = original; cached = shared grab.
    const ClusterLiveFrameStats st = clusterCaptureLiveStats();
    g_server->sendHeader("X-SF-Frame-Timing",
                         String("grab_us=") + st.grabUs + ";decode_us=" + st.decodeUs + ";encode_us=" + st.encodeUs +
                             ";src=" + st.srcWidth + "x" + st.srcHeight + ";out=" + st.width + "x" + st.height +
                             ";path=" + st.path + ";cached=" + (st.cached ? 1 : 0));
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
    // wb_mode: preset 0..4 or kelvin directly (API); "k" takes wb_kelvin (page).
    t.wbMode = srv.arg("wb_mode") == "k" ? argInt(srv, "wb_kelvin", 0) : argInt(srv, "wb_mode", 0);
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

// JS string literal for inline scripts ('...', no '<' so no </script>).
String jsQuoted(const String &text)
{
    String m = text;
    m.replace("\\", "\\\\");
    m.replace("'", "\\'");
    m.replace("<", "\\x3c");
    return "'" + m + "'";
}

int result(String &json, int code, bool ok, const String &message)
{
    json = String("{\"ok\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscaped(message) + "\"}";
    return code;
}

const char *const kResolutions[] = {"160x120", "320x240", "640x480", "800x600",
                                    "1024x768", "1280x1024", "1600x1200", "2048x1536"};

// Largest resolution of the running sensor (from its report), 0x0 = unknown.
void sensorMaxResolution(const ClusterCamera::Report &r, uint16_t &w, uint16_t &h)
{
    w = h = 0;
    if (r.maxFramesize != ClusterCamera::kUnknown)
        ClusterCamera::framesizeDims(r.maxFramesize, w, h);
}

bool resolutionFits(const char *res, uint16_t maxW, uint16_t maxH)
{
    unsigned w = 0, h = 0;
    if (sscanf(res, "%ux%u", &w, &h) != 2)
        return false;
    return !maxW || (w <= maxW && h <= maxH);
}

// mode=resolution: save the camera resolution (camera settings path, same
// checks as the camera page) and restart the camera. The recording frame rate
// is lowered when the new resolution would exceed the board's pixel budget.
int applyResolution(WebServer &srv, String &json)
{
    using namespace ClusterCamera;
    const String res = srv.arg("resolution");
    Report r;
    cameraFillClusterReport(r);
    if (r.profileState == (int32_t)ProfileState::Active || r.profileState == (int32_t)ProfileState::Pending)
        return result(json, 409, false, "Cluster-Kameraprofil aktiv – die Auflösung gibt das Profil vor.");
    uint16_t maxW = 0, maxH = 0;
    sensorMaxResolution(r, maxW, maxH);
    bool known = false;
    for (const char *k : kResolutions)
        known = known || res == k;
    if (!known || !resolutionFits(res.c_str(), maxW, maxH))
        return result(json, 400, false, "Auflösung nicht unterstützt: " + res);
    int fps = cfg_fps;
    String note;
    const int maxFps = configRecordingPerformanceMaxFps(res, cfg_quality);
    if (maxFps >= 1 && fps > maxFps) {
        note = " Aufnahme-Bildrate von " + String(fps) + " auf " + String(maxFps) + " fps gesenkt.";
        fps = maxFps;
    }
    String error;
    configRefreshSdStatus();
    const bool writeToSd = configSdAvailable() && configSdPresent();
    const ConfigSaveResult saved = configSaveCameraSettings(
        cfg_camera, res, fps, cfg_quality, cfg_camera_xclk_mhz, cfg_camera_auto_exposure, cfg_camera_ae_level,
        cfg_rotation, cfg_camera_crop_zoom, cfg_camera_crop_x, cfg_camera_crop_y, writeToSd, error);
    if (saved != CONFIG_SAVE_BOTH && saved != CONFIG_SAVE_INTERNAL_ONLY)
        return result(json, 400, false, "Speichern fehlgeschlagen: " + error);
    clusterCaptureLiveInvalidate();
    if (!cameraRestartWithSettings(error))
        return result(json, 200, true, "Auflösung " + res + " gespeichert; " + error + "." + note);
    return result(json, 200, true, "Auflösung " + res + " gespeichert, Kamera neu gestartet." + note);
}

// mode=orientation: rotation 0/180, hmirror/vflip 0/1 – done by the sensor
// (set_hmirror/set_vflip), saved, camera restart. Not part of the cluster
// profile: every camera is mounted differently.
int applyOrientation(WebServer &srv, String &json)
{
    const int rotation = argInt(srv, "rotation", -1);
    const int hmirror = argInt(srv, "hmirror", -1);
    const int vflip = argInt(srv, "vflip", -1);
    if ((rotation != 0 && rotation != 180) || (hmirror != 0 && hmirror != 1) || (vflip != 0 && vflip != 1))
        return result(json, 400, false, "rotation muss 0/180, hmirror/vflip 0/1 sein");
    // The fields are absolute: choosing the current orientation changes
    // nothing (reported, no camera restart).
    const bool eh = (rotation == 180) != (bool)hmirror, ev = (rotation == 180) != (bool)vflip;
    const String target = eh && ev ? "„Auf dem Kopf (180°)“"
                          : eh     ? "„Links und rechts vertauscht“"
                          : ev     ? "„Oben und unten vertauscht“"
                                   : "„Normal“";
    if (rotation == cfg_rotation && (bool)hmirror == (bool)cfg_camera_hmirror && (bool)vflip == (bool)cfg_camera_vflip)
        return result(json, 200, true, "Keine Änderung – Lage ist bereits " + target + ".");
    String error;
    configRefreshSdStatus();
    const bool writeToSd = configSdAvailable() && configSdPresent();
    const ConfigSaveResult saved = configSaveCameraOrientation(rotation, hmirror, vflip, writeToSd, error);
    if (saved != CONFIG_SAVE_BOTH && saved != CONFIG_SAVE_INTERNAL_ONLY)
        return result(json, 400, false, "Speichern fehlgeschlagen: " + error);
    clusterCaptureLiveInvalidate();
    if (!cameraRestartWithSettings(error))
        return result(json, 200, true, "Lage " + target + " gespeichert; " + error + ".");
    return result(json, 200, true, "Lage jetzt " + target + " – gespeichert, Kamera neu gestartet.");
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
    String host = cfg_hostname.length() ? cfg_hostname : String("SensorForge");
    host.replace("&", "&amp;");
    host.replace("<", "&lt;");
    host.replace(">", "&gt;");
    html += "<div class='page-title'><div><h2>Bildabstimmung – " + host + "</h2><p>Gerät <strong>" + host +
            "</strong> (" + WiFi.localIP().toString() + ") · Feste Belichtung, Verstärkung und Weißabgleich – "
            "Grundlage für einheitliche Cluster-Kameras (Gaussian Splats)</p></div></div>";
    html += "<script>document.title='Bildabstimmung – '+" + jsQuoted(cfg_hostname) + ";</script>";
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
            "<div style='margin-top:6px'><label><input type='checkbox' id='tuneLive' checked> Vorschau live aktualisieren</label> "
            "<label>Vorschaugröße <select id='tuneSize'><option value='small'>Klein</option>"
            "<option value='medium' selected>Mittel (ca. 800 px)</option><option value='full'>Voll</option></select></label> "
            "<span id='tuneImgState' class='muted'></span></div>"
            "<p class='muted' style='margin:4px 0 0'>Vorschau-Einstellungen betreffen nur die Übertragung in den Browser, "
            "nicht die Kamera.</p>";
    {
        uint16_t curW = 0, curH = 0, maxW = 0, maxH = 0;
        framesizeDims(r.framesize, curW, curH);
        sensorMaxResolution(r, maxW, maxH);
        const String cur = String(curW) + "x" + String(curH);
        const bool profileOwns = r.profileState == (int32_t)ProfileState::Active || r.profileState == (int32_t)ProfileState::Pending;
        html += "<div style='margin-top:12px'><label for='tuneRes'><strong>Kamera-Auflösung</strong></label> "
                "<select id='tuneRes'" + String(profileOwns ? " disabled" : "") + ">";
        for (const char *k : kResolutions)
            if (resolutionFits(k, maxW, maxH))
                html += String("<option value='") + k + "'" + (cur == k ? " selected" : "") + ">" + k + "</option>";
        html += "</select> <button type='button' id='tuneResApply'" + String(profileOwns ? " disabled" : "") +
                ">Übernehmen (Kamera startet neu)</button>"
                "<p class='muted' style='margin:4px 0 0'>" +
                String(profileOwns ? "Cluster-Kameraprofil aktiv – die Auflösung gibt das Profil vor."
                                   : "Gilt dauerhaft (wie auf der Kamera-Seite). Die Kamera startet kurz neu. Der "
                                     "Belichtungswert zählt in Sensorzeilen – nach einem Wechsel ergibt derselbe Wert eine "
                                     "andere Belichtungszeit.") +
                "</p></div>";
        const bool mirrorCap = !caps || (caps & CapMirror);
        const String dm = mirrorCap ? String() : String(" disabled title='Vom Sensor nicht unterstützt'");
        // One choice, four distinct images: the sensor can only mirror each
        // axis, and 180° is both axes together. Older configs combining
        // 180° with mirror/flip are shown as the image they produce.
        const bool eh = (cfg_rotation == 180) != (bool)cfg_camera_hmirror;
        const bool ev = (cfg_rotation == 180) != (bool)cfg_camera_vflip;
        const int lage = eh && ev ? 3 : (eh ? 1 : (ev ? 2 : 0));
        static const char *const kLage[4][2] = {
            {"0,0,0", "Normal"},
            {"0,1,0", "Links und rechts vertauscht (horizontal gespiegelt)"},
            {"0,0,1", "Oben und unten vertauscht (vertikal gespiegelt)"},
            {"180,0,0", "Auf dem Kopf (180° gedreht)"}};
        html += "<div style='margin-top:12px'><label><strong>Lage des Bildes</strong> <select id='tuneLage'" + dm + ">";
        for (int i = 0; i < 4; ++i)
            html += String("<option value='") + kLage[i][0] + "'" + (i == lage ? " selected" : "") + ">" +
                    kLage[i][1] + "</option>";
        html += "</select></label> <span id='tuneOriState' class='muted'></span>"
                "<p class='muted' style='margin:4px 0 0'>Wirkt sofort beim Auswählen (die Kamera startet kurz neu, "
                "gespeichert). Die Auswahl ist die gewünschte Lage, nicht „um … drehen“. Macht der Sensor selbst "
                "(keine Bildumrechnung); 90° ist mit diesen Sensoren nicht möglich. Gilt nur für dieses Gerät, das "
                "Cluster-Profil überträgt die Lage nicht. Zum Beurteilen braucht es ein erkennbares Bild "
                "(bei Dunkelheit Belichtung auf Auto).</p></div>";
    }
    html += "</div>";
    html += "<form id='tuneForm' style='flex:1 1 300px;min-width:0' onsubmit='return false'><div class='tune-grid'>";
    html += "<label>Belichtungsautomatik</label><select name='aec'" + dis(CapManualExposure) + ">" + opt(1, aec, "An") + opt(0, aec, "Aus") + "</select>";
    html += "<label>AE-Korrektur (bei Automatik)</label><input name='ae_level' type='number' min='-2' max='2' value='" + String(aeLevel) + "'" + dis(CapAeLevel) + ">";
    html += "<label>Belichtungswert (Zeilen, bei Aus)</label><input name='aec_value' id='tuneAecValue' type='number' min='0' max='" + String(r.pid == 0x26 ? CameraTuning::kAecMaxOv2640 : CameraTuning::kAecMax) + "' value='" + String(aecValue) + "'" + dis(CapManualExposure) + ">";
    html += "<label>Belichtungszeit (bei Aus)</label><div><select id='tuneShutter' disabled><option value=''>– Zeilenwert –</option>"
            "<option value='30'>1/30 s</option><option value='60'>1/60 s</option><option value='125'>1/125 s</option>"
            "<option value='250'>1/250 s</option><option value='500'>1/500 s</option><option value='1000'>1/1000 s</option>"
            "<option value='2000'>1/2000 s</option></select> <span id='tuneShutterInfo' class='muted'>wird gemessen …</span></div>";
    html += "<label>Verstärkungsautomatik</label><select name='agc'" + dis(CapManualGain) + ">" + opt(1, agc, "An") + opt(0, agc, "Aus") + "</select>";
    html += "<label>Verstärkung (0–30, bei Aus)</label><input name='agc_gain' type='number' min='0' max='" + String(CameraTuning::kAgcMax) + "' value='" + String(agcGain) + "'" + dis(CapManualGain) + ">";
    html += "<label>Verstärkungsobergrenze (bei An)</label><select name='gainceiling'" + dis(CapGainCeiling) + ">";
    for (int i = 0; i <= CameraTuning::kGainCeilingMax; ++i)
        html += opt(i, ceiling, String(2 << i) + "×");
    html += "</select>";
    html += "<label>Weißabgleich-Automatik</label><select name='awb'" + dis(CapAutoWb) + ">" + opt(1, awb, "An") + opt(0, awb, "Aus") + "</select>";
    const bool kelvin = CameraTuning::isKelvin(wbMode);
    const bool kelvinCap = !caps || (caps & CapWbKelvin);
    html += "<label>Weißabgleich-Voreinstellung</label><select name='wb_mode' id='tuneWb'" + dis(CapWbPresets) + ">" + opt(0, wbMode, "Auto") +
            opt(1, wbMode, "Sonne") + opt(2, wbMode, "Wolken") + opt(3, wbMode, "Büro") + opt(4, wbMode, "Glühlampe") +
            (kelvinCap ? String("<option value='k'") + (kelvin ? " selected" : "") + ">Farbtemperatur (Kelvin)</option>" : String()) +
            "</select>";
    html += "<label>Farbtemperatur (K, " + String(CameraTuning::kWbKelvinMin) + "–" + String(CameraTuning::kWbKelvinMax) +
            ")</label><input name='wb_kelvin' id='tuneKelvin' type='number' step='100' min='" + String(CameraTuning::kWbKelvinMin) +
            "' max='" + String(CameraTuning::kWbKelvinMax) + "' value='" + String(kelvin ? wbMode : 5500) + "'" +
            (kelvinCap ? String() : String(" disabled title='Nur OV3660/OV5640'")) + ">";
    html += "<label>AWB-Gain</label><select name='awb_gain'" + dis(CapAwbGain) + ">" + opt(1, awbGain, "An") + opt(0, awbGain, "Aus") + "</select>";
    html += "<label>Helligkeit</label><input name='brightness' type='number' min='-2' max='2' value='" + String(bri) + "'" + dis(CapBrightness) + ">";
    html += "<label>Kontrast</label><input name='contrast' type='number' min='-2' max='2' value='" + String(con) + "'" + dis(CapContrast) + ">";
    html += "<label>Sättigung</label><input name='saturation' type='number' min='-2' max='2' value='" + String(sat) + "'" + dis(CapSaturation) + ">";
    html += "</div><p><button type='button' data-mode='session'>Anwenden (bis Neustart)</button> "
            "<button type='button' data-mode='save'>Dauerhaft speichern</button> "
            "<button type='button' data-mode='reset'>Auf Standardwerte zurücksetzen</button></p>"
            "<p><label><input type='checkbox' id='tuneAuto' checked> Änderungen sofort anwenden (bis Neustart)</label></p>"
            "<p id='tuneState' class='muted' role='status'></p>"
            "<p class='muted'>„Anwenden“ ändert die laufende Kamera sofort. „Dauerhaft speichern“ wirkt auch nach jedem "
            "Kamerastart. „Auf Standardwerte zurücksetzen“ stellt alle Werte dieser Seite auf die Werkseinstellung "
            "(Automatiken an, Weißabgleich Auto, Helligkeit/Kontrast/Sättigung 0) und speichert das. Farbtemperatur: "
            "ungefährer Kelvin-Wert, 5500 K entspricht der Voreinstellung „Sonne“ des Sensors. Während einer geplanten Cluster-Aufnahme werden Änderungen abgelehnt.</p></form></div></section>";
    html += R"__TUNE__(<script>(function(){
var wb=document.getElementById('tuneWb'),kel=document.getElementById('tuneKelvin');
function wbView(){if(kel&&wb&&!kel.hasAttribute('title'))kel.disabled=wb.value!=='k';}if(wb){wb.addEventListener('change',wbView);wbView();}
var img=document.getElementById('tuneImg'),live=document.getElementById('tuneLive'),size=document.getElementById('tuneSize'),ist=document.getElementById('tuneImgState'),st=document.getElementById('tuneState'),form=document.getElementById('tuneForm'),busy=false,again=false,timer=null;
function next(){if(timer){clearTimeout(timer);timer=null;}if(live.checked&&!document.hidden)timer=setTimeout(load,800);}
function load(){if(busy){again=true;return;}busy=true;again=false;var u='/camera_frame?size='+size.value+'&t='+Date.now();
 fetch(u,{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t||('HTTP '+r.status));});return r.blob();})
 .then(function(b){var o=img.src;img.src=URL.createObjectURL(b);if(o&&o.indexOf('blob:')===0)URL.revokeObjectURL(o);ist.textContent=new Date().toLocaleTimeString('de-DE')+' · '+Math.round(b.size/1024)+' KB';})
 .catch(function(e){ist.textContent='Kein Bild: '+(e.message||e);}).then(function(){busy=false;if(again)load();else next();});}
live.addEventListener('change',function(){if(live.checked)load();});size.addEventListener('change',load);
document.addEventListener('visibilitychange',function(){if(!document.hidden&&live.checked)load();});
// Exposure time: aec_value (sensor lines) x measured line duration.
var aecV=document.getElementById('tuneAecValue'),aecSel=form.querySelector('[name=aec]'),sh=document.getElementById('tuneShutter'),shi=document.getElementById('tuneShutterInfo'),lineUs=0;
function fmtT(us){if(us>=1e6)return (us/1e6).toFixed(1)+' s';return '1/'+Math.round(1e6/us)+' s';}
function shView(){if(!lineUs)return;var v=parseInt(aecV.value,10);shi.textContent=(v>0?'≈ '+fmtT(v*lineUs)+' · ':'')+'1 Zeile ≈ '+lineUs.toFixed(1)+' µs';}
fetch('/camera_line_time',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(j){
 if(!j.ok){shi.textContent='Zeit nicht messbar: '+j.message;return;}lineUs=j.line_us;var max=parseInt(aecV.max,10)||j.aec_max;
 Array.from(sh.options).forEach(function(o){if(!o.value)return;var n=Math.round(1e6/(+o.value*lineUs));if(n<1||n>max){o.disabled=true;o.textContent+=n<1?' (zu kurz)':' (zu lang)';}});
 sh.disabled=aecV.disabled;shView();}).catch(function(e){shi.textContent='Zeit nicht messbar: '+(e.message||e);});
aecV.addEventListener('input',function(){sh.value='';shView();});
sh.addEventListener('change',function(){if(!sh.value||!lineUs)return;aecV.value=Math.max(1,Math.round(1e6/(+sh.value*lineUs)));
 if(aecSel&&!aecSel.disabled)aecSel.value='0';shView();aecV.dispatchEvent(new Event('change',{bubbles:true}));});
// A restarted device has a new per-boot CSRF token: a page loaded before the
// restart gets 403 on every change. Reload it instead of failing silently.
function tuneJson(r){if(r.status===403)return r.text().then(function(t){if(/csrf/i.test(t)){setTimeout(function(){location.reload();},2500);return {ok:false,message:'Seite veraltet (Gerät wurde neu gestartet) – wird neu geladen, danach die Änderung bitte wiederholen.'};}return {ok:false,message:t||'HTTP 403'};});
 return r.json().catch(function(){return {ok:false,message:'HTTP '+r.status};});}
// Camera resolution: saved like on the camera page, camera restarts.
var resSel=document.getElementById('tuneRes'),resBtn=document.getElementById('tuneResApply');
if(resBtn)resBtn.addEventListener('click',function(){if(!confirm('Kamera-Auflösung auf '+resSel.value+' ändern? Die Kamera startet neu.'))return;
 var fd=new URLSearchParams();fd.append('mode','resolution');fd.append('resolution',resSel.value);st.textContent='Auflösung wird übernommen …';resBtn.disabled=true;
 fetch('/camera_tuning_apply',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(tuneJson)
 .then(function(j){st.textContent=(j.ok?'✓ ':'✗ ')+j.message;if(j.ok)setTimeout(function(){location.reload();},1500);else resBtn.disabled=false;}).catch(function(e){st.textContent='✗ '+(e.message||e);resBtn.disabled=false;});});
// Image orientation: one of four positions, applied on selection.
var lage=document.getElementById('tuneLage'),lageSt=document.getElementById('tuneOriState');
if(lage){var lageOld=lage.value;lage.addEventListener('change',function(){var p=lage.value.split(','),fd=new URLSearchParams();
 fd.append('mode','orientation');fd.append('rotation',p[0]);fd.append('hmirror',p[1]);fd.append('vflip',p[2]);
 lage.disabled=true;lageSt.textContent='wird übernommen …';
 fetch('/camera_tuning_apply',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(tuneJson)
 .then(function(j){lageSt.textContent=(j.ok?'✓ ':'✗ ')+j.message;if(j.ok){lageOld=lage.value;setTimeout(load,1500);}else lage.value=lageOld;})
 .catch(function(e){lageSt.textContent='✗ '+(e.message||e);lage.value=lageOld;}).then(function(){lage.disabled=false;});});}
var sending=false,queued=null,autoTimer=null,auto=document.getElementById('tuneAuto');
// Disabled (unsupported) fields are sent too; the server needs a complete set.
function send(m){if(sending){queued=m;return;}sending=true;queued=null;
 var fd=new URLSearchParams();Array.from(form.elements).forEach(function(el){if(el.name)fd.append(el.name,el.value);});fd.append('mode',m);st.textContent='Wird übertragen …';
 fetch('/camera_tuning_apply',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(tuneJson)
 .then(function(j){st.textContent=(j.ok?'✓ ':'✗ ')+j.message;if(j.ok&&m==='reset'){setTimeout(function(){location.reload();},800);return;}if(j.ok)load();}).catch(function(e){st.textContent='✗ '+(e.message||e);})
 .then(function(){sending=false;if(queued)send(queued);});}
form.addEventListener('click',function(e){var b=e.target.closest('button[data-mode]');if(!b)return;var m=b.getAttribute('data-mode');
 if(m==='reset'&&!confirm('Alle Werte dieser Seite auf die Standardwerte zurücksetzen und speichern?'))return;
 if(autoTimer){clearTimeout(autoTimer);autoTimer=null;}send(m);});
// Instant preview: every change is applied for the session after a short pause.
function changed(e){if(!auto||!auto.checked||!e.target.name)return;if(e.target.type==='number'&&!e.target.checkValidity())return;
 if(autoTimer)clearTimeout(autoTimer);autoTimer=setTimeout(function(){autoTimer=null;send('session');},e.type==='input'?600:150);}
form.addEventListener('input',changed);form.addEventListener('change',changed);
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
    if (mode == "resolution")
        return applyResolution(srv, json);
    if (mode == "orientation")
        return applyOrientation(srv, json);
    int autoExposure = cfg_camera_auto_exposure, aeLevel = cfg_camera_ae_level;
    CameraTuning::Values t;
    String error;
    if (mode == "reset") {
        // Firmware defaults: saved tuning removed, auto exposure/AE level reset,
        // the default values applied to the running camera right away.
        autoExposure = CameraTuning::kDefaultAutoExposure;
        aeLevel = CameraTuning::kDefaultAeLevel;
    } else if (!readForm(srv, autoExposure, aeLevel, t, error)) {
        return result(json, 400, false, error);
    }
    if (mode == "save" || mode == "reset") {
        char text[80];
        CameraTuning::format(mode == "reset" ? CameraTuning::Values{} : t, text, sizeof(text));
        configRefreshSdStatus();
        const bool writeToSd = configSdAvailable() && configSdPresent();
        const ConfigSaveResult saved = configSaveCameraTuning(autoExposure, aeLevel, String(text), writeToSd, error);
        if (saved != CONFIG_SAVE_BOTH && saved != CONFIG_SAVE_INTERNAL_ONLY)
            return result(json, 500, false, "Speichern fehlgeschlagen: " + error);
        if (mode == "reset")
            t.set = true; // apply the default values now
    } else if (mode != "session") {
        return result(json, 400, false, "mode muss session, save, reset, resolution oder orientation sein");
    }
    clusterCaptureLiveInvalidate();
    if (!cameraApplyTuningRuntime(autoExposure, aeLevel, t, error)) {
        if (mode == "save" || mode == "reset")
            return result(json, 200, true, "Gespeichert; wirkt beim nächsten Kamerastart (" + error + ").");
        return result(json, 409, false, error);
    }
    return result(json, 200, true, mode == "reset" ? "Standardwerte gespeichert und angewendet."
                                   : mode == "save" ? "Gespeichert und angewendet." : "Angewendet (bis zum Neustart).");
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

String cameraLineTimeJson()
{
    float lineUs = 0, frameMs = 0;
    uint32_t vts = 0;
    String reason;
    if (!clusterCaptureMeasureLineTime(lineUs, vts, frameMs, reason))
        return "{\"ok\":false,\"message\":\"" + jsonEscaped(reason) + "\"}";
    return "{\"ok\":true,\"line_us\":" + String(lineUs, 2) + ",\"vts\":" + String((unsigned long)vts) +
           ",\"frame_ms\":" + String(frameMs, 1) + ",\"aec_max\":" + String(CameraTuning::kAecMax) + "}";
}

namespace {
// GET /camera_line_time: measured sensor line duration for the exposure time display.
void handleCameraLineTime()
{
    if (!requireAdmin())
        return;
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "application/json; charset=utf-8", cameraLineTimeJson());
}
} // namespace

void webconfigCameraTuningRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)())
{
    g_server = &server;
    g_header = htmlHeader;
    g_footer = htmlFooter;
    server.on("/camera_frame", HTTP_GET, handleCameraFrame);
    server.on("/camera_tuning", HTTP_GET, handleCameraTuningPage);
    server.on("/camera_tuning_apply", HTTP_POST, handleCameraTuningApply);
    server.on("/camera_line_time", HTTP_GET, handleCameraLineTime);
}
