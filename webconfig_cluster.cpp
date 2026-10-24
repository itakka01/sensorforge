#include <math.h>
#include <cmath>
#include "webconfig_cluster.h"

#include "cluster.h"
#include "cluster_espnow_radio.h"
#include "loop_load_diag.h"
#include "cluster_hw_time_meter.h"
#include "drone_mode.h"
#include "cluster_profiles.h"
#include "webconfig_cluster_groups.h"
#include "webconfig_cluster_camera.h"
#include "config.h"
#include "device_identity.h"
#include "board_config.h"
#include "logger.h"

#include <errno.h>
#include <stdlib.h>

namespace {

static WebServer *g_server = nullptr;
static WebConfigClusterUiHooks g_uiHooks = {nullptr, nullptr, nullptr};

static WebServer &serverRef()
{
    return *g_server;
}

#define server serverRef()

static String htmlEscape(const String &value)
{
    String out;
    out.reserve(value.length() + 16);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value[i];
        switch (c) {
            case '&': out += F("&amp;"); break;
            case '<': out += F("&lt;"); break;
            case '>': out += F("&gt;"); break;
            case '"': out += F("&quot;"); break;
            case '\'': out += F("&#39;"); break;
            default: out += c; break;
        }
    }
    return out;
}

static String htmlHeader()
{
    return g_uiHooks.htmlHeader ? g_uiHooks.htmlHeader() : String();
}

static String htmlFooter()
{
    return g_uiHooks.htmlFooter ? g_uiHooks.htmlFooter() : String();
}

static String clusterPageHtml()
{
    String html;
    html.reserve(12000);

    html +=
        "<div class='page-title'><div><h2>Cluster-Status auf Gerät " +
        htmlEscape(cfg_hostname.length() ? cfg_hostname : String("sensorforge")) + "</h2>"
        "<p class='muted'>Lokaler Geräteverbund für gemeinsame Statusdaten und spätere Multi-Kamera-Funktionen.</p>"
        "</div></div>";

    if (server.arg("notice") == "saved") {
        html +=
            "<div class='flash-notice'><strong>Cluster-Einstellungen gespeichert.</strong> "
            "<span class='muted'>Geänderte Cluster-Zugehörigkeit wird beim nächsten WiFi-Neustart bzw. Geräte-Neustart aktiv.</span></div>";
    }
    if (server.arg("notice") == "mount_saved") {
        html +=
            "<div class='flash-notice'><strong>Montagedaten gespeichert.</strong> "
            "<span class='muted'>Kein Neustart nötig; sie werden mit dem nächsten Heartbeat (max. 10 s) gemeldet.</span></div>";
    }

    html +=
        "<style>"
        ".cluster-grid{display:grid;grid-template-columns:minmax(190px,240px) minmax(220px,1fr);gap:10px 16px;align-items:center}"
        ".cluster-join-fields{grid-column:1/-1;min-width:0}.cluster-grid .cluster-label{font-weight:600}.cluster-grid input,.cluster-grid select{width:100%;max-width:430px;margin:0}"
        ".cluster-note{grid-column:2;color:var(--muted);font-size:.86rem;margin-top:-5px;margin-bottom:4px}"
        ".cluster-id{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.9rem;word-break:break-all}"
        ".cluster-table-wrap{overflow:auto}.cluster-table{width:100%;border-collapse:collapse;min-width:1480px}"
        ".cluster-resource-table{min-width:980px}"
        ".cluster-simple-table{min-width:500px}"
        ".cluster-join-fields[hidden]{display:none}.cluster-note details{display:inline}.cluster-note summary{cursor:pointer;text-decoration:underline;text-underline-offset:2px}.cluster-tech-details{margin-top:16px}"
        ".cluster-tech-details summary{cursor:pointer;font-weight:600;padding:10px 0}"
        ".cluster-tech-details[open] summary{margin-bottom:8px}"
        ".cluster-table th,.cluster-table td{text-align:left;padding:9px 10px;border-bottom:1px solid var(--border);white-space:nowrap;vertical-align:top}"
        ".cluster-table th{font-size:.82rem;color:var(--muted)}"
        ".cluster-runtime-line{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:8px 0 14px}"
        ".cluster-subtle{font-size:.82rem;color:var(--muted)}"
        "@media(max-width:720px){.cluster-grid{grid-template-columns:1fr;gap:5px}.cluster-note{grid-column:1;margin-top:-2px;margin-bottom:9px}.cluster-grid input,.cluster-grid select{max-width:none}}"
        "</style>";

    // Drone is configured in Configuration > Betriebsmodus. Coordinator
    // bulk commands use the same NVS flag via droneModeSet().
    html +=
        "<form id='clusterSettingsForm' method='POST' action='/cluster_save'>"
        "<section class='settings-section'><h3>Cluster-Mitgliedschaft</h3>"
        "<div class='cluster-grid'>"
        "<div class='cluster-label'>Teilnahme dieses Geräts</div>"
        "<div><select name='cluster_enabled' id='clusterEnabled'>"
        "<option value='0'" + String(!cfg_cluster_enabled ? " selected" : "") + ">Nicht am Clusterverbund teilnehmen</option>"
        "<option value='1'" + String(cfg_cluster_enabled ? " selected" : "") + boardOptionDisabledAttr(BOARD_SUPPORTS_CLUSTER_NODE) + ">Gerät mit Clusterverbund verbinden" + boardOptionUnsupportedSuffix(BOARD_SUPPORTS_CLUSTER_NODE) + "</option>"
        "</select>" +
        // A disabled selected option is not submitted; keep the saved value (UI release only).
        String(cfg_cluster_enabled && !BOARD_SUPPORTS_CLUSTER_NODE ? "<input type='hidden' name='cluster_enabled' value='1'>" : "") +
        "</div>"
        "<div class='cluster-note'>Das Gerät nutzt den Cluster nur, wenn die Teilnahme aktiviert und gespeichert wurde. Die untenstehende Übersicht ist auch ohne Teilnahme verfügbar.</div>"
        "<div id='clusterJoinFields' class='cluster-grid cluster-join-fields'>"
        "<div class='cluster-label'>Cluster auswählen</div>"
        "<div><select id='clusterChoice'><option value=''>Manuell / neuer Cluster</option></select></div>"
        "<div class='cluster-note'>Wähle einen gefundenen Cluster oder lege einen neuen an. <details><summary>Info zur Clustersuche</summary>Die Suche hört nur bei geöffneter Seite mit. Ohne aktive Teilnahme sendet das Gerät keine Cluster-Pakete.</details></div>"
        "<div class='cluster-label'>Cluster-Passwort</div>"
        "<div><input name='cluster_password' id='clusterPassword' type='password' minlength='8' maxlength='63' autocomplete='new-password' placeholder='" +
        String(cfg_cluster_password.length() ? "Unverändert lassen" : "Mindestens 8 Zeichen") + "'></div>"
        "<div id='clusterCredentialHint' class='cluster-note'>" +
        String(cfg_cluster_password.length()
            ? "Ein Passwort ist gespeichert. Leeres Feld behält das bestehende bzw. ein bekanntes gespeichertes Passwort bei."
            : "Noch kein Passwort für die aktuelle Auswahl gespeichert. Bei aktiviertem Cluster ist ein Passwort erforderlich.") +
        "</div>"
        "<div class='cluster-note'><details><summary>Info zur Passwortsicherheit</summary>Ein sichtbarer Cluster prüft das Passwort vor dem Speichern anhand signierter Pakete. Passwortänderungen eines laufenden Mehrgeräte-Clusters erfolgen später zentral.</details></div>"

        "<div class='cluster-label'>Cluster-ID</div>"
        "<div><input name='cluster_id' id='clusterId' maxlength='35' autocomplete='off' value='" +
        htmlEscape(clusterEffectiveId()) + "' placeholder='wird bei neuem Cluster automatisch erzeugt'></div>"
        "<div class='cluster-note'><details><summary>Info zur Cluster-ID</summary>Die eindeutige Cluster-ID bleibt auch bei einem Passwortwechsel gleich.</details></div>"
        "<div class='cluster-label'>Cluster-Name</div>"
        "<div><input name='cluster_name' id='clusterName' maxlength='63' autocomplete='off' value='" +
        htmlEscape(cfg_cluster_name) + "'></div>"
        "<input type='hidden' name='cluster_credential_epoch' id='clusterCredentialEpoch' value='" +
        String((unsigned long)cfg_cluster_credential_epoch) + "'>"
        "<div class='cluster-note'>Frei wählbarer Name für die Anzeige.</div>"
        "<div class='cluster-label'>Koordination im Cluster</div>"
        "<div><select name='cluster_coordinator_policy' id='clusterCoordinatorPolicy'>"
        "<option value='auto'" + String(cfg_cluster_coordinator_policy == "auto" ? " selected" : "") + boardOptionDisabledAttr(BOARD_SUPPORTS_COORDINATOR) + ">Automatisch (Standard)" + boardOptionUnsupportedSuffix(BOARD_SUPPORTS_COORDINATOR) + "</option>"
        "<option value='preferred'" + String(cfg_cluster_coordinator_policy == "preferred" ? " selected" : "") + boardOptionDisabledAttr(BOARD_SUPPORTS_COORDINATOR) + ">Bevorzugter Coordinator" + boardOptionUnsupportedSuffix(BOARD_SUPPORTS_COORDINATOR) + "</option>"
        "<option value='node'" + String(cfg_cluster_coordinator_policy == "node" ? " selected" : "") + ">Nur Node (nie Coordinator)</option>"
        "</select>" +
        // Disabled options are not submitted; /cluster_save then keeps the saved policy.
        String(!BOARD_SUPPORTS_COORDINATOR ? "<div class='cluster-note'>Coordinator-Rolle: " BOARD_UNSUPPORTED_HINT "</div>" : "") +
        "</div>"
        "<div class='cluster-note'>Normalerweise ist „Automatisch“ richtig. <details><summary>Info zur Koordination</summary>Geräte bestimmen ihren Coordinator selbst. Bevorzugt erhöht die Priorität, Nur Node verhindert eine Coordinator-Rolle.</details></div>"
        "<div class='cluster-label'>Technische Gerätekennung</div>"
        "<div class='cluster-id'>" + htmlEscape(deviceIntegrationId()) + "</div>"
        "<div class='cluster-note'><details><summary>Info zur Gerätekennung</summary>Stabile interne Integration-ID; unabhängig von Gerätename und Lizenz.</details></div>"
        "</div></div></section>";

    // Optional mount metadata, saved together with the cluster settings.
    html += webconfigClusterMountFormHtml();

    html +=
        "<div class='floating-save-space'></div>"
        "<div id='clusterSaveBar' class='floating-save-bar'>"
        "<span id='clusterSaveState' class='floating-save-state'>Keine ungespeicherten Änderungen</span>"
        "<button id='clusterSaveButton' type='submit' disabled>Speichern</button>"
        "</div></form>";

    html +=
        "<section class='settings-section'><h3>Cluster-Übersicht</h3>"
        "<div id='clusterRuntimeLine' class='cluster-runtime-line'><span class='status-pill'>Lädt …</span></div>"
        "<div id='clusterPrecisionWarning' class='flash-notice' hidden style='border-color:#f3d39b;border-left-color:var(--warn);background:#fff8e6'>"
        "<strong style='color:var(--warn)'>&#9888; Achtung – eingeschränkte Zeitsynchronisation</strong>"
        "<span class='muted'>Mindestens ein Gerät in diesem Cluster ist nicht für hochpräzise Zeitsynchronisation freigegeben. "
        "Zeitkritische Aktionen auf diesen Geräten können größere Abweichungen aufweisen.</span>"
        "<div id='clusterPrecisionWarningDevices' class='muted' style='margin-top:4px'></div></div>"
        "<div id='clusterCapacityWarning' class='flash-notice' hidden style='border-color:#f3d39b;border-left-color:var(--warn);background:#fff8e6'>"
        "<strong style='color:var(--warn)'>&#9888; Cluster-Kapazität</strong>"
        "<div id='clusterCapacityWarningText' class='muted' style='margin-top:4px'></div></div>"
        "<div class='cluster-table-wrap'><table class='cluster-table cluster-simple-table'>"
        "<thead><tr><th>Gerät</th><th>Rolle</th><th>Status</th><th>IP</th><th>Zuletzt gesehen</th></tr></thead>"
        "<tbody id='clusterSimpleRows'><tr><td colspan='5' class='muted'>Status wird geladen …</td></tr></tbody>"
        "</table></div>"
        "<details class='cluster-tech-details'><summary>Technische Details anzeigen</summary>"
        "<p id='clusterTimeDiagnostics' class='muted'>Clusterzeit: wartet auf Zeitquelle</p>"
        "<div class='cluster-table-wrap'><table class='cluster-table'>"
        "<thead><tr><th>Gerät</th><th>Cluster-Rolle</th><th>Policy</th><th>Integration-ID</th><th>IP</th><th>Mode</th><th>Status</th><th>Release</th><th>Zeit</th><th>Lease</th><th>Coordinator-Link</th><th>WiFi noch ca.</th><th>Zuletzt gesehen</th></tr></thead>"
        "<tbody id='clusterNodeRows'><tr><td colspan='13' class='muted'>Status wird geladen …</td></tr></tbody>"
        "</table></div>"
        "<p class='muted' id='clusterAckDiagnostics' style='margin-top:10px'>ACK-Diagnose wird geladen …</p><p class='muted' id='clusterJobDiagnostics'>Job-Diagnose wird geladen …</p><p class='muted' id='clusterRestartDiagnostics'>Neustart-Diagnose wird geladen …</p>"
        "<pre class='muted' id='clusterElectionTrace' style='white-space:pre-wrap;overflow-wrap:anywhere;font-size:11px'>Election-Verlauf wird geladen …</pre>"
        "<p class='muted' style='margin-top:12px'>Multicast-Presence standardmäßig alle 10 Sekunden, Lease 60 Sekunden. "
        "Zusätzlich sendet jeder Node dem gewählten Coordinator alle 10 Sekunden einen signierten Unicast-Heartbeat und erhält ein ACK. "
        "Beim geordneten WiFi-Abschalten sendet ein Gerät ein signiertes LEAVE; bei Strom- oder Funkverlust läuft die Lease aus. "
        "Der Cluster öffnet oder verlängert WiFi niemals selbst.</p>"
        "</details></section>";

    html +=
        "<details class='settings-section cluster-tech-details'><summary>Angekündigte Ressourcen</summary>"
        "<div class='cluster-table-wrap'><table class='cluster-table cluster-resource-table'>"
        "<thead><tr><th>Quelle</th><th>Resource-ID</th><th>Typ</th><th>Zeitstempel</th><th>Größe</th><th>TTL</th><th>Locator</th></tr></thead>"
        "<tbody id='clusterResourceRows'><tr><td colspan='7' class='muted'>Noch keine Ressourcen angekündigt.</td></tr></tbody>"
        "</table></div>"
        "<p class='muted' style='margin-top:12px'>Resource-Announcements enthalten nur kleine, HMAC-authentifizierte Metadaten. "
        "JPEGs, Videos oder andere Nutzdaten werden nicht per Multicast übertragen. Der automatische Peer-Download wird separat an den bestehenden Storage/API-Pfad angebunden.</p>"
        "</details>";

    html +=
        "<details class='settings-section cluster-tech-details'><summary>Discovery / Protokoll</summary>"
        "<p class='muted'>mDNS/DNS-SD <code>_sfcluster._udp</code> dient der Service-Ankündigung. Aktive Cluster senden zusätzlich eine kleine öffentliche Discovery-Metadatenmeldung mit Cluster-ID, Anzeigename und Credential-Epoch; sie enthält kein Passwort und ist keine Beitrittsberechtigung. "
        "Die laufende Presence, Lease-, LEAVE- und Resource-Metadaten bleiben HMAC-authentifiziert und verändern weder Recording noch Streaming.</p>"
        "</details>";

#if defined(BOARD_FREENOVE)
    // Passive GPIO meter: works on node or coordinator, uses only the local clock.
    html += R"__METER__(<details class='settings-section cluster-tech-details' id='sfMeter'><summary>GPIO-Messgerät (passiv)</summary>
<p class='muted'>Erfasst die steigenden Flanken des optionalen GPIO-Testimpulses mehrerer Kamera-Nodes und berechnet ihre Differenzen ausschließlich mit der lokalen Uhr dieses Geräts. Clusterzeit und ESP-NOW werden dafür nicht verwendet; die Cluster-Rolle spielt keine Rolle. Gemessen wird der Start des aufnehmenden <code>esp_camera_fb_get()</code> auf jedem Node, <strong>nicht</strong> der Belichtungsbeginn. Voraussetzung: Drone Mode auf diesem Gerät. Magnetkontakt, Präsenz und Radar sind während der Messung ausgesetzt (automatischer Stopp nach 2 h).</p>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Kanal</th><th>Eingang hier</th><th>Vorher abklemmen</th><th>Quelle</th></tr></thead><tbody>
<tr><td>1</td><td>GPIO14</td><td>Magnetkontakt</td><td>Node A D1/GPIO2 über 1 kΩ, 10 kΩ nach GND</td></tr>
<tr><td>2</td><td>GPIO21</td><td>Präsenz (OT2/PIR)</td><td>Node B D1/GPIO2 über 1 kΩ, 10 kΩ nach GND</td></tr>
<tr><td>3</td><td>GPIO41</td><td>Radar OT1 (UART RX)</td><td>optional Node C, sonst nur 10 kΩ nach GND</td></tr>
</tbody></table></div>
<p class='muted'>Alle Geräte über eine gemeinsame GND-Leitung verbinden. Nur 3,3-V-Signale. Differenzen zählen nur, wenn beide Kanäle genau eine Flanke im Fenster haben und keine verspätete Flanke auftritt; fehlende, doppelte oder verspätete Flanken machen das Ereignis ungültig.</p>
)__METER__";
#if CLUSTER_HW_TIME_IN_1 < 0 || CLUSTER_HW_TIME_IN_2 < 0 || CLUSTER_HW_TIME_IN_3 < 0
    html += "<p class='muted'><strong>Messeingänge gesperrt:</strong> GPIO14/21/41 müssen nach Prüfung von Pin-Konflikten und Verdrahtung in board_config.h explizit freigegeben werden (CLUSTER_HW_TIME_IN_1..3).</p>";
#endif
    html += R"__METER__(<div class='cluster-select-tools'><button type='button' id='sfMeterStart'>Messung starten</button> <button type='button' id='sfMeterStop'>Messung stoppen</button> <a href='/cluster_hw_meter_csv' download='sensorforge_gpio_meter.csv'>CSV herunterladen</a></div>
<div id='sfMeterState' class='muted' role='status'>Status wird geladen …</div>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Ereignis</th><th>K1 ms</th><th>K2 ms</th><th>K3 ms</th><th>K1−K2 ms</th><th>K1−K3 ms</th><th>K2−K3 ms</th><th>Flanken</th></tr></thead><tbody id='sfMeterRows'><tr><td colspan='8' class='muted'>Noch keine Ereignisse.</td></tr></tbody></table></div>
<script>(function(){
var box=document.getElementById('sfMeter'),st=document.getElementById('sfMeterState'),rows=document.getElementById('sfMeterRows');
function v(x){return x===null||x===undefined?'–':Number(x).toFixed(3);}
function esc(t){return String(t).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function pairText(p,name){return p&&p.n?name+': n='+p.n+' min '+v(p.min_ms)+' / Mittel '+v(p.mean_ms)+' / max '+v(p.max_ms)+' ms':'';}
function render(d){var pr=Array.isArray(d.pairs)?d.pairs:[];st.textContent=(d.active?'Aktiv':'Inaktiv')+' · Zustand '+d.state+' · Drone Mode '+(d.drone_mode?'an':'aus')+' · Eingänge '+(d.inputs||[]).join('/')+' · Ereignisfenster '+d.window_ms+' ms · Ereignisse '+d.events+' · gültig '+(d.valid_rows||0)+' / ungültig '+(d.invalid_rows||0)+[pairText(pr[0],' · K1−K2'),pairText(pr[1],' · K1−K3'),pairText(pr[2],' · K2−K3')].join('');
 var s=Array.isArray(d.samples)?d.samples.slice().reverse():[];
 rows.innerHTML=s.length?s.map(function(r){var multi=!r.valid;return '<tr><td>'+esc(r.n)+'</td><td>'+v(r.ms[0])+'</td><td>'+v(r.ms[1])+'</td><td>'+v(r.ms[2])+'</td><td>'+v(r.pair_ms[0])+'</td><td>'+v(r.pair_ms[1])+'</td><td>'+v(r.pair_ms[2])+'</td><td>'+(multi?'<span class="status-pill warn">':'')+esc((r.rises||[]).join('/'))+(r.late?' +'+esc(r.late)+' spät':'')+(multi?' ungültig</span>':'')+'</td></tr>';}).join(''):'<tr><td colspan="8" class="muted">Noch keine Ereignisse.</td></tr>';}
function poll(){if(!box.open)return;fetch('/cluster_hw_meter_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw Error();return r.json();}).then(render).catch(function(){st.textContent='Status nicht abrufbar.';});}
function post(url){fetch(url,{method:'POST',credentials:'same-origin',cache:'no-store'}).then(function(r){return r.text().then(function(t){st.textContent=t;poll();});}).catch(function(){st.textContent='Anfrage fehlgeschlagen.';});}
document.getElementById('sfMeterStart').addEventListener('click',function(){post('/cluster_hw_meter_start');});
document.getElementById('sfMeterStop').addEventListener('click',function(){post('/cluster_hw_meter_stop');});
box.addEventListener('toggle',poll);setInterval(poll,2000);
})();</script></details>)__METER__";
#endif

    html += R"__JS__(
<script>(function(){
var f=document.getElementById('clusterSettingsForm'),b=document.getElementById('clusterSaveBar'),s=document.getElementById('clusterSaveState'),btn=document.getElementById('clusterSaveButton'),rows=document.getElementById('clusterNodeRows'),resRows=document.getElementById('clusterResourceRows'),line=document.getElementById('clusterRuntimeLine');
var choice=document.getElementById('clusterChoice'),idField=document.getElementById('clusterId'),nameField=document.getElementById('clusterName'),epochField=document.getElementById('clusterCredentialEpoch'),passwordField=document.getElementById('clusterPassword'),credentialHint=document.getElementById('clusterCredentialHint');
var choiceData={},choiceSignature='';
var clusterEnabled=document.getElementById('clusterEnabled'),joinFields=document.getElementById('clusterJoinFields');
function syncJoinFields(){if(joinFields&&clusterEnabled)joinFields.hidden=clusterEnabled.value!=='1';}
if(clusterEnabled)clusterEnabled.addEventListener('change',syncJoinFields);syncJoinFields();
function dirty(){if(!b||!s||!btn)return;b.classList.add('dirty');s.textContent='Ungespeicherte Änderungen';btn.disabled=false;}
if(f&&b&&s&&btn){f.addEventListener('input',dirty);f.addEventListener('change',dirty);f.addEventListener('submit',function(){btn.disabled=true;s.textContent='Speichert …';});}
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function age(ms,local){if(local)return 'lokal';ms=Number(ms)||0;if(ms<1500)return 'jetzt';if(ms<60000)return Math.round(ms/1000)+' s';return Math.round(ms/60000)+' min';}
function duration(v,unknown){v=Number(v);if(!Number.isFinite(v)||v<0)return unknown||'unbekannt';if(v<60)return Math.round(v)+' s';if(v<3600)return Math.round(v/60)+' min';if(v<86400)return (v/3600).toFixed(v<7200?1:0)+' h';return (v/86400).toFixed(1)+' d';}
function timeText(n){if(!n.time_valid)return 'nicht gültig';var e=Number(n.epoch)||0;if(!e)return 'nicht gültig';try{return new Date(e*1000).toLocaleString();}catch(_){return String(e);}}
function resourceTime(us){us=Number(us)||0;if(!us)return '-';var ms=Math.floor(us/1000),micro=Math.abs(Math.trunc(us%1000000));try{var d=new Date(ms);var base=d.toLocaleString();return base+'.'+String(micro).padStart(6,'0');}catch(_){return String(us);}}
function sizeText(v){v=Number(v)||0;if(!v)return '-';if(v<1024)return v+' B';if(v<1048576)return (v/1024).toFixed(1)+' KiB';return (v/1048576).toFixed(1)+' MiB';}
function sfPrecisionBadge(n){var v=String(n&&n.precision_sync||'unknown');if(v==='yes')return '';if(v==='no')return ' <span class="status-pill warn" title="Präzisionssynchronisation für diesen Boardtyp nicht freigegeben.">&#9888; Präzisionssynchronisation für diesen Boardtyp nicht freigegeben.</span>';return ' <span class="status-pill" title="Gerät meldet keine Board-Eignung (z. B. ältere Firmware).">Präzisionseignung unbekannt</span>';}window.sfPrecisionBadge=sfPrecisionBadge;
function statusText(n){var x=[];if(n.recording)x.push('Recording');if(n.streaming)x.push('Streaming');if(n.transport)x.push('Transport');return x.length?x.join(' · '):'Bereit';}
function policyText(v){if(v==='preferred')return 'Bevorzugt';if(v==='node')return 'Nur Node';return 'Auto';}
function roleText(v){return v==='coordinator'?'Coordinator':'Node';}
function isStandbyBroadcaster(n){var m=String(n&&n.mode||'');return m==='time_broadcaster'||m==='time_broadcaste';}
function displayRole(n){return isStandbyBroadcaster(n)?'Zeit-Broadcaster (Standby)':roleText(n.cluster_role);}
function isActionableNode(n){return n&&!n.local&&!isStandbyBroadcaster(n)&&!!n.online;}
function nodeNavigationLink(n,d){
 if(n.local||!d.runtime_active||!n.online)return '';
 var ip=String(n.ip||'');var parts=ip.split('.');
 if(parts.length!==4||!parts.every(function(p){return /^\d{1,3}$/.test(p)&&Number(p)<=255;})||ip==='0.0.0.0'||ip==='255.255.255.255')return '';
 var isCoordinator=n.cluster_role==='coordinator'&&String(n.integration_id||'')===String(d.coordinator_id||'');
 var path=isCoordinator?'/cluster_coordinate':'/';
 var label=isCoordinator?'Cluster steuern ↗':'Gerät öffnen ↗';
 return ' <a href="http://'+esc(ip)+path+'" title="'+(isCoordinator?'Cluster-Steuerung':'Startseite')+' auf '+esc(n.hostname||ip)+' öffnen">'+label+'</a>';
}
function orderedNodes(nodes,d){
 return nodes.slice().sort(function(a,b){
  var ac=a.cluster_role==='coordinator'&&String(a.integration_id||'')===String(d.coordinator_id||'');
  var bc=b.cluster_role==='coordinator'&&String(b.integration_id||'')===String(d.coordinator_id||'');
  if(ac!==bc)return ac?-1:1;
  if(!!a.local!==!!b.local)return a.local?-1:1;
  return String(a.hostname||a.integration_id||'').localeCompare(String(b.hostname||b.integration_id||''));
 });
}
function linkText(n,d){var a=Number(n.direct_heartbeat_age_ms),ack=Number(n.coordinator_ack_age_ms);if(d.local_role==='coordinator'&&!n.local&&a>=0)return 'Heartbeat '+age(a,false);if(n.local&&d.local_role!=='coordinator'){if(ack>=0)return 'ACK '+age(ack,false);var q=d.ack_diagnostics||{};var accepted=Number(q.ack_accepted)||0;return accepted>0?'ACK bestätigt ('+accepted+'× seit Start), aktuell kein ACK-Alter':'Noch kein ACK';}return '-';}
function leaseText(n){var left=duration(n.lease_remaining_sec,'-'),total=duration(n.lease_sec,'-');return n.local?total:(left+' / '+total);}
function wifiText(n){var v=Number(n.wifi_remaining_sec);if(v===-1)return 'offen / unbekannt';if(v===0)return 'endet';return duration(v,'unbekannt');}
function renderChoices(d){
 if(!choice)return;
 var known={},knownList=Array.isArray(d.known_clusters)?d.known_clusters:[];
 knownList.forEach(function(k){if(k&&k.cluster_id)known[String(k.cluster_id)]=k;});
 var grouped={};
 (Array.isArray(d.discovered_nodes)?d.discovered_nodes:[]).forEach(function(n){var id=String(n.cluster_id||'');if(!id)return;var g=grouped[id]||(grouped[id]={id:id,name:String(n.cluster_name||''),epochs:{},nodes:0,auth:0,coord:false});g.nodes++;if(n.authenticated)g.auth++;g.epochs[String(Number(n.credential_epoch)||1)]=1;if(n.coordinator)g.coord=true;if(!g.name&&n.cluster_name)g.name=String(n.cluster_name);});
 var entries=[];
 Object.keys(grouped).sort().forEach(function(id){var g=grouped[id],eps=Object.keys(g.epochs),k=known[id],conflict=eps.length!==1,ep=conflict?0:Number(eps[0]);entries.push({id:id,name:g.name||((k&&k.cluster_name)||id),epoch:ep,online:true,nodes:g.nodes,auth:g.auth,coord:g.coord,conflict:conflict,saved:!!(k&&k.password_saved),savedEpoch:k?Number(k.credential_epoch)||1:0,stale:!!(k&&ep&&Number(k.credential_epoch)!==ep),authMismatch:!!(d.runtime_active&&id===String(d.runtime_cluster_id||'')&&g.nodes>g.auth)});});
 knownList.forEach(function(k){var id=String(k.cluster_id||'');if(!id||grouped[id])return;entries.push({id:id,name:String(k.cluster_name||id),epoch:Number(k.credential_epoch)||1,online:false,nodes:0,auth:0,coord:false,conflict:false,saved:!!k.password_saved,savedEpoch:Number(k.credential_epoch)||1,stale:false,authMismatch:false});});
 var current=String(d.cluster_id||'');if(current&&!entries.some(function(e){return e.id===current;}))entries.push({id:current,name:String(d.cluster_name||current),epoch:Number(d.credential_epoch)||1,online:false,nodes:0,auth:0,coord:false,conflict:false,saved:!!d.password_configured,savedEpoch:Number(d.credential_epoch)||1,stale:false,authMismatch:false});
 var sig=JSON.stringify(entries);choiceData={};entries.forEach(function(e){choiceData[e.id]=e;});
 if(sig===choiceSignature)return;choiceSignature=sig;var selected=choice.value||current;choice.innerHTML='<option value="">Manuell / neuer Cluster</option>'+entries.map(function(e){var label=e.name+' · '+e.id+(e.online?' · '+e.nodes+' online':' · bekannt/offline')+(e.coord?' · Coordinator':'')+(e.saved&&!e.stale?' · Passwort gespeichert':'')+(e.stale?' · Passwort geändert?':'')+(e.conflict?' · CREDENTIAL-EPOCH-KONFLIKT':'')+(e.authMismatch?' · NICHT AUTHENTIFIZIERT':'');return '<option value="'+esc(e.id)+'"'+(e.conflict?' disabled':'')+'>'+esc(label)+'</option>';}).join('');if(selected&&choiceData[selected]&&!choiceData[selected].conflict)choice.value=selected;}
 if(choice){choice.addEventListener('change',function(){var e=choiceData[choice.value];if(!e)return;if(idField)idField.value=e.id;if(nameField)nameField.value=e.name;if(epochField)epochField.value=String(e.epoch||1);if(passwordField){passwordField.value='';passwordField.placeholder=(e.saved&&!e.stale)?'Gespeichertes Passwort wird verwendet':(e.stale?'Passwort erneut eingeben':'Mindestens 8 Zeichen');}if(credentialHint)credentialHint.textContent=e.stale?'Der sichtbare Cluster meldet eine andere Credential-Epoch. Das gespeicherte Passwort wird nicht automatisch verwendet.':((e.saved&&!e.stale)?'Passwort für diesen Cluster ist auf diesem Gerät hardwaregebunden gespeichert. Leeres Feld verwendet es intern.':'Für diesen Cluster ist kein passendes Passwort gespeichert.');dirty();});}
 if(idField)idField.addEventListener('input',function(){if(choice)choice.value='';});if(nameField)nameField.addEventListener('input',function(){if(choice)choice.value='';});
function render(d){if(!rows||!line)return;renderChoices(d);var pills=[];pills.push('<span class="status-pill '+(d.runtime_active?'ok':'warn')+'">Runtime '+(d.runtime_active?'AKTIV':'AUS')+'</span>');var coord=String(d.coordinator_id||'');pills.push('<span class="status-pill '+(coord?'ok':'warn')+'">Coordinator '+(coord?esc(coord):'keiner')+(d.coordinator_epoch?(' · Epoch '+esc(d.coordinator_epoch)):'')+'</span>');var ct=d.cluster_time||{};pills.push('<span class="status-pill '+(ct.valid?'ok':'warn')+'">Clusterzeit '+(ct.valid?'aktiv':'wartet auf Zeitquelle')+'</span>');if(d.restart_required)pills.push('<span class="status-pill warn">NEUSTART ERFORDERLICH</span>');if(d.error)pills.push('<span class="status-pill danger">'+esc(d.error)+'</span>');line.innerHTML=pills.join(' ');var diag=document.getElementById('clusterAckDiagnostics'),q=d.ack_diagnostics||{};if(diag){diag.textContent='ACK-Diagnose (seit Runtime-Start, lokal): HB gesendet '+(q.hb_sent||0)+' / Sendefehler '+(q.hb_send_failed||0)+' · ACK-Sendeversuche '+(q.ack_send_attempted||0)+' / Sendefehler '+(q.ack_send_failed||0)+' · signierte ACK empfangen '+(q.ack_authenticated||0)+' / akzeptiert '+(q.ack_accepted||0)+' / abgelehnt '+(q.ack_rejected||0)+' · Coordinator-Wechsel '+(q.coordinator_changes||0)+' / Epoch-Wechsel '+(q.coordinator_epoch_changes||0)+' / ACK-Kontext-Resets '+(q.ack_context_resets||0)+' / unvollständige Coordinator-Meldungen '+(q.incomplete_coord_announcements||0)+' · letzter Ablehnungsgrund '+(q.last_reject||'none');}var jd=document.getElementById('clusterJobDiagnostics'),j=d.job_probe_diagnostics||{};if(jd)jd.textContent='Job-Probe (lokal): empfangen '+(j.received||0)+' / ACK gesendet '+(j.ack_sent||0)+' / abgelehnt '+(j.rejected||0)+' ('+(j.last_reject||'none')+') · ACK empfangen '+(j.ack_received||0)+' / abgelehnt '+(j.ack_rejected||0)+' ('+(j.last_ack_reject||'none')+')';var rd=document.getElementById('clusterRestartDiagnostics'),r=d.job_restart_diagnostics||{};if(rd)rd.textContent='Neustart-Diagnose (lokal): Aufträge empfangen '+(r.received||0)+' / geplant und ACK gesendet '+(r.accepted||0)+' / abgelehnt '+(r.rejected||0)+' ('+(r.last_reject||'none')+') · ACK empfangen '+(r.ack_received||0)+' / akzeptiert '+(r.ack_accepted||0)+' / abgelehnt '+(r.ack_rejected||0)+' ('+(r.last_ack_reject||'none')+')';var td=document.getElementById('clusterTimeDiagnostics');if(td){var t=d.cluster_time||{};td.textContent='Clusterzeit: '+(t.valid?'gültig':'noch nicht verfügbar')+' · Quelle '+(t.source||'-')+' · Messungen '+(t.accepted||0)+' / verworfen '+(t.rejected||0)+' · RTT '+(t.last_rtt_us||0)+' µs · Alter '+(t.sample_age_ms||0)+' ms · Abweichung zur Geräteuhr '+(t.local_offset_available?((Number(t.local_offset_us||0)/1000).toFixed(2)+' ms'):'nicht messbar')+' · grobe Unsicherheit ±'+(Number(t.uncertainty_estimate_us||0)/1000).toFixed(2)+' ms (Schätzung, keine Garantie; keine Frame-Synchronität)';}var tr=document.getElementById('clusterElectionTrace');if(tr){var entries=Array.isArray(d.election_trace)?d.election_trace:[];tr.textContent='Letzte Election-Übergänge (RAM):\\n'+(entries.length?entries.join('\\n'):'keine');}var a=orderedNodes(Array.isArray(d.nodes)?d.nodes:[],d);var pw=document.getElementById('clusterPrecisionWarning'),pwd=document.getElementById('clusterPrecisionWarningDevices');if(pw){var lim=a.filter(function(n){return (n.local?!!d.runtime_active:!!n.online)&&n.precision_sync==='no';});pw.hidden=!lim.length;if(pwd)pwd.textContent=lim.length?'Betroffene Geräte: '+lim.map(function(n){return n.hostname||n.integration_id||'-';}).join(', '):'';}var cw=document.getElementById('clusterCapacityWarning'),cwt=document.getElementById('clusterCapacityWarningText');if(cw&&cwt){var msgs=[],warn=false,es=d.espnow_time_local||{},slotCap=Number(es.slot_capacity)||0,peerCap=Number(d.peer_capacity)||0,others=a.filter(function(n){return !n.local&&!!n.online;}).length;if(es.slot_table_full_recent){warn=true;msgs.push('Mehr aktive Nodes als Plätze in der Präzisions-Zeitsynchronisation ('+slotCap+'). Mindestens ein Node erhält derzeit keine ESP-NOW-Präzisionssynchronisation und arbeitet nur mit der groben WLAN-Zeit.');}else if(slotCap&&others>=slotCap-2){msgs.push('Präzisions-Zeitsynchronisation: '+others+' Nodes online, Grenze '+slotCap+' Nodes. Weitere Nodes darüber hinaus werden nicht präzise synchronisiert.');}var cyc=Number(es.slot_cycle_estimate_ms)||0;if(cyc>100000){if(cyc>=120000)warn=true;msgs.push('Synchronisationszyklus etwa '+Math.round(cyc/1000)+' s bei einer Gültigkeit von 120 s: Die Funkzeit ist nahezu ausgeschöpft, einzelne Nodes können zeitweise auf die WLAN-Zeit zurückfallen.');}if(d.peer_table_full_recent){warn=true;msgs.push('Mehr Geräte im Cluster als die Mitgliederliste fasst ('+peerCap+' + dieses Gerät). Aktive Geräte werden aus der Liste verdrängt; Anzeige und Jobs können unvollständig sein.');}else if(peerCap&&others>=peerCap-2){msgs.push('Mitgliederliste: '+others+' von '+peerCap+' Plätzen belegt (ohne dieses Gerät).');}cw.hidden=!msgs.length;cw.style.borderLeftColor=warn?'var(--danger)':'var(--warn)';cwt.textContent=msgs.join(' ');}var simple=document.getElementById('clusterSimpleRows');if(simple){simple.innerHTML=a.length?a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+sfPrecisionBadge(n)+'</td><td>'+esc(displayRole(n))+nodeNavigationLink(n,d)+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join(''):'<tr><td colspan="5" class="muted">Keine Geräte sichtbar.</td></tr>';}if(!a.length){rows.innerHTML='<tr><td colspan="13" class="muted">Keine Geräte sichtbar.</td></tr>';}else{rows.innerHTML=a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+sfPrecisionBadge(n)+'</td><td><strong>'+esc(displayRole(n))+'</strong></td><td>'+esc(policyText(n.coordinator_policy))+'</td><td class="cluster-id">'+esc(n.integration_id||'-')+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(n.mode||'-')+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.release||'-')+'</td><td>'+esc(timeText(n))+'</td><td>'+esc(leaseText(n))+'</td><td>'+esc(linkText(n,d))+'</td><td>'+esc(wifiText(n))+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join('');}
if(resRows){var byId={};a.forEach(function(n){byId[String(n.integration_id||'')]=n.hostname||n.integration_id||'-';});var rr=Array.isArray(d.resources)?d.resources:[];if(!rr.length){resRows.innerHTML='<tr><td colspan="7" class="muted">Noch keine Ressourcen angekündigt.</td></tr>';}else{resRows.innerHTML=rr.map(function(r){var owner=byId[String(r.owner_integration_id||'')]||r.owner_integration_id||'-';return '<tr><td><strong>'+esc(owner)+'</strong><div class="cluster-subtle">'+esc(r.source_ip||'-')+'</div></td><td class="cluster-id">'+esc(r.resource_id||'-')+'</td><td>'+esc(r.type||'-')+'</td><td>'+esc(resourceTime(r.timestamp_us))+'</td><td>'+esc(sizeText(r.size_bytes))+'</td><td>'+esc(duration(r.ttl_remaining_sec,'-'))+'</td><td class="cluster-id">'+esc(r.locator||'-')+'</td></tr>';}).join('');}}}
function poll(){fetch('/cluster_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw new Error();return r.json();}).then(render).catch(function(e){console.error('Cluster-Status: Abruf oder Anzeige fehlgeschlagen',e);if(line)line.innerHTML='<span class="status-pill danger">Cluster-Status konnte nicht angezeigt werden</span>';});}poll();setInterval(poll,3000);document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});
})();</script>
)__JS__";

    return html;
}

static void handleClusterPage()
{
    clusterDiscoveryTouch();
    String html = htmlHeader();
    html += clusterPageHtml();
    html += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleClusterHwTimeStart() {
    if (!clusterLocalIsCoordinator() || !droneModeEnabled()) {
        server.send(403, "text/plain; charset=utf-8", "Coordinator muss im Drone Mode sein."); return;
    }
    String ids[3] = {server.arg("node1"), server.arg("node2"), server.arg("node3")};
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterHwTimeStart(ids, error)) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", "Messfenster gestartet; ARM-Rueckmeldungen und Flanken im Status pruefen.");
}

static void handleClusterHwTimeStatus() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", clusterHwTimeStatusJson());
}

static void handleClusterHwTimeCsv() {
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am Coordinator verfuegbar."); return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", "attachment; filename=sensorforge_hw_time.csv");
    server.send(200, "text/csv; charset=utf-8", clusterHwTimeCsv());
}

static void handleClusterHwMeterStart() {
    String error;
    if (!ClusterHwTimeMeter::start(error)) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", "Messgeraet scharf; Flanken werden erfasst.");
}

static void handleClusterHwMeterStop() {
    ClusterHwTimeMeter::stop("stopped_by_user");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", "Messgeraet gestoppt.");
}

static void handleClusterHwMeterStatus() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", ClusterHwTimeMeter::statusJson());
}

static void handleClusterHwMeterCsv() {
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Content-Disposition", "attachment; filename=sensorforge_gpio_meter.csv");
    server.send(200, "text/csv; charset=utf-8", ClusterHwTimeMeter::csv());
}

static void handleClusterStatus()
{
    clusterDiscoveryTouch();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", clusterStatusJson());
}

// Loop section and task CPU load (diagnostics only, no secrets).
static void handleLoadDiag()
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", LoopLoadDiag::json());
}

static void handleClusterCoordinatePage()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    String html = htmlHeader();
    html += "<div class='page-title'><div><h2>Cluster koordinieren – " +
        htmlEscape(cfg_hostname.length() ? cfg_hostname : String("sensorforge")) +
        "</h2><p class='muted'>Aktionen für den lokalen Clusterverbund</p></div></div>";
    // One bounded status request shared by all coordinator widgets. The old
    // independent 3s polling loops could accumulate requests on a busy ESP32.
    html += R"__STATUS__(<script>
(function(){
var pending=null, last=null, lastAt=0;
window.sfCoordinateHttpStats={lastMs:null,lastError:'',success:0,fail:0};
window.sfCoordinatorBulkActive=false;
window.sfCoordinateStatus=function(force){
  if(pending)return pending;
  if(window.sfCoordinatorBulkActive){return last?Promise.resolve(last):Promise.reject(Error("Sammelaktion läuft; Statusabfrage pausiert"));}
  if(!force && last && Date.now()-lastAt<1200)return Promise.resolve(last);
  var started=Date.now(),ctrl=new AbortController(), timer=setTimeout(function(){ctrl.abort();},8000);
  pending=fetch('/cluster_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin',signal:ctrl.signal})
    .then(function(r){if(!r.ok)throw Error('HTTP '+r.status);return r.json();})
    .then(function(d){last=d;lastAt=Date.now();window.sfCoordinateHttpStats.lastMs=lastAt-started;window.sfCoordinateHttpStats.lastError='';window.sfCoordinateHttpStats.success++;return d;})
    .catch(function(e){window.sfCoordinateHttpStats.fail++;window.sfCoordinateHttpStats.lastError=String(e.message||e);throw e;})
    .finally(function(){clearTimeout(timer);pending=null;});
  return pending;
};
})();
</script>)__STATUS__";
    html += "<script>window.sfHwTimeNotReady='";
#if CLUSTER_HW_TIME_IN_1 < 0 || CLUSTER_HW_TIME_IN_2 < 0 || CLUSTER_HW_TIME_IN_3 < 0
    html += "Messung gesperrt: GPIO14/21/41 muessen nach Pruefung der Verdrahtung in board_config.h explizit freigegeben werden.";
#else
    html += "Messung nur mit drei XIAO-Nodes (GPIO2) und gemeinsamer GND-Leitung starten.";
#endif
    html += "';</script>";
    html += "<script>window.sfHwTimeMeta={coordinatorBoard:\"";
    html += BOARD_DISPLAY_NAME;
    html += "\",inputs:[";
    html += String(CLUSTER_HW_TIME_IN_1);
    html += ",";
    html += String(CLUSTER_HW_TIME_IN_2);
    html += ",";
    html += String(CLUSTER_HW_TIME_IN_3);
    html += "],nodeOutputs:{xiao:2,freenove:48},inputCandidates:[14,21,41],maxNodes:3};</script>";
    html += webconfigClusterGroupsSectionHtml();
    html += webconfigClusterCameraSectionHtml();
    html += R"__HEALTH__(<section class='settings-section'><h3>Cluster-Geräte</h3>
<p class='muted'>Aktuelle Ressourcenwerte der verbundenen Geräte. SD-Daten werden normalerweise etwa einmal pro Minute aktualisiert, nach einem Cluster-Wipe unmittelbar erneut gemessen und signiert gemeldet. Fehlende Werte werden nicht geschätzt.</p>
<div class='cluster-select-tools'><button type='button' id='clusterSelectAll'>Alle sichtbaren Nodes auswählen</button> <button type='button' id='clusterSelectNone'>Auswahl aufheben</button> <span id='clusterSelectionCount'>0 ausgewählt</span></div>
<div class='cluster-select-tools'><label for='clusterActionChoice'>Aktion für ausgewählte Nodes:</label> <select id='clusterActionChoice'><option value=''>Aktion auswählen …</option><option value='clusterBulkProbe'>Verbindung testen</option><option value='clusterBulkDroneOn'>Drone Mode einschalten</option><option value='clusterBulkDroneOff'>Drone Mode ausschalten</option><option value='clusterBulkSync'>Zeit synchronisieren</option><option value='clusterBulkRestart'>Neu starten</option><option value='clusterBulkShutdown'>Herunterfahren</option><option value='clusterBulkWipe'>SD vollständig löschen</option><option value='clusterHardwareTime'>Hardware-Zeitmessung</option></select> <button type='button' id='clusterActionRun'>Ausführen</button> <button type='button' id='clusterActionInfo' aria-label='Hinweise zu Cluster-Aktionen' title='Hinweise zu Cluster-Aktionen'>Info</button></div>
<details class='settings-section'><summary>ESP-NOW: lokale Funkdiagnose und Node-Sync</summary><p class='muted'>Coordinator-Funkstatus sowie authentifizierte Node-Mess- oder Fehlerberichte. Nicht eingetroffene Ergebnisse werden nicht geschätzt.</p><div id='sfEspNowActionDiag' role='status'>Warte auf Status …</div><div id='sfEspNowNodeDiag'></div></details>
<div id='clusterHardwareTimeFeedback' role='status' style='display:none;padding:10px 12px;border:1px solid #b45309;border-radius:8px;margin:8px 0'></div>
<div id='clusterHardwareTimeInfo' style='display:none;margin:10px 0;padding:12px 14px;border:1px solid #64748b66;border-radius:10px;background:#0f172a08'>
 <div style='display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap'><div><strong>Hardware-Zeitmessung</strong><div class='muted'>Misst die relative Uhrabweichung von bis zu drei Nodes über direkte Messleitungen am Coordinator. Kamera- und Belichtungsverzögerungen sind ausdrücklich nicht Teil dieser Messung.</div></div></div>
 <p class='muted' style='margin:8px 0'>Erforderlich: je Node eine Signalleitung zum Coordinator sowie eine gemeinsame Masse (GND). Die Reihenfolge der ausgewählten Nodes entspricht den Coordinator-Eingängen 1 bis 3.</p>
 <div id='clusterHardwareTimeNodeConfig' class='cluster-select-tools' style='margin:8px 0 10px 0'></div>
 <div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Signal</th><th>Node / Ausgang</th><th>Verbindung</th><th>Coordinator / Eingang</th></tr></thead><tbody id='clusterHardwareTimeWiringRows'><tr><td colspan='4' class='muted'>Wähle „Hardware-Zeitmessung“ und markiere 1 bis 3 Nodes.</td></tr></tbody></table></div>
 <div id='clusterHardwareTimeStatus' class='muted' style='margin-top:8px'></div>
 <div style='margin-top:10px'><a href='/cluster_hw_time_csv' download='sensorforge_hw_time.csv'>CSV-Messreihe herunterladen</a></div>
 <div id='sfHwTimeReading' role='status' class='muted'>Noch keine Messung gestartet.</div>
 <svg id='sfHwTimePlot' viewBox='0 0 650 210' role='img' aria-label='Zeitversatz der drei Nodes in Millisekunden' style='width:100%;max-width:850px;min-height:160px;border:1px solid #64748b44;border-radius:8px'></svg>
 <p class='muted'>Messwerte: GPIO-Flanken relativ zum geplanten Coordinator-Zeitpunkt. Die ISR- und Timer-Latenz ist nicht kalibriert. Für Drift mehrere Messungen nacheinander starten.</p>
</div>
<div id='clusterActionHandlers' style='display:none'><button type='button' id='clusterBulkDroneOn'>Drone Mode EIN</button><button type='button' id='clusterBulkDroneOff'>Drone Mode AUS</button><button type='button' id='clusterBulkProbe'>Verbindung testen</button><button type='button' id='clusterBulkRestart'>Neustart</button><button type='button' id='clusterBulkShutdown'>Shutdown</button><button type='button' id='clusterBulkSync'>Zeitsync</button><button type='button' id='clusterBulkWipe'>SD löschen</button></div>
<dialog id='clusterActionDialog' style='max-width:min(90vw,650px);padding:24px;border-radius:14px'><h3>Cluster-Aktionen</h3><p>Drone Mode hält Nodes passiv bereit: kein automatisches Recording, kein Power Shooter, keine Bewegungserkennung und keine Hintergrund-SD-Recovery. Laufende Aufnahmen werden beim Einschalten kontrolliert beendet. Clusterzeit, geplante Capture-Jobs, Webverwaltung und Temperaturschutz bleiben verfügbar.</p><p>Sammelaktionen werden einzeln und signiert pro Node ausgeführt. Der Coordinator ist nicht auswählbar. Remote-Shutdown benötigt RESET oder Stromversorgung zum Wiederaufwachen.</p><p>HTTP-Annahme bestätigt nicht die Ausführung. Ergebnisse bitte im Status prüfen.</p><button type='button' id='clusterActionDialogClose'>Schließen</button></dialog>
<div class='cluster-select-tools'><strong>Foto in</strong> <label for='clusterCaptureHours'>Stunden</label> <input id='clusterCaptureHours' type='number' min='0' max='23' step='1' value='0' style='width:4.5em'> <label for='clusterCaptureMinutes'>Minuten</label> <input id='clusterCaptureMinutes' type='number' min='0' max='59' step='1' value='0' style='width:4.5em'> <label for='clusterCaptureSeconds'>Sekunden</label> <input id='clusterCaptureSeconds' type='number' min='0' max='59' step='1' value='5' style='width:4.5em'> <label title='Nur XIAO-Nodes mit aktueller Firmware: Impuls an D1/GPIO2 direkt am Start des aufnehmenden fb_get(). Ältere Firmware verwirft solche Aufträge.'><input type='checkbox' id='clusterCaptureTestPulse'> GPIO-Testimpuls (D1)</label> <button type='button' id='clusterBulkCapture'>Aufnahme planen</button> <output id='clusterPlannedTime' style='font-variant-numeric:tabular-nums;font-weight:700'>UTC-Zeit wird geladen …</output></div>
<!-- Direkte UTC-Eingabe kann später wieder aktiviert werden. sfPlanUtcTarget() akzeptiert dafür einen optionalen UTC-ISO-Termin; aktuell nutzt das Formular ausschließlich relative Zeit. -->
<div class='cluster-select-tools'><button type='button' id='sfSyncOpen'>Sync-Testbild im Vollbild öffnen</button> <span class='muted'>Beim Capture öffnet sich automatisch ein großes Vollbild-Popup. Countdown und Balken laufen bis 5 s nach dem Auslösezeitpunkt.</span></div>
<div id='sfSyncScreen' aria-label='Visuelle Synchronisationsanzeige' style='display:none'>
 <div class='sf-sync-stage'><div class='sf-sync-top'><strong>CLUSTER SYNC · BILDSCHIRMTEST</strong><button id='sfSyncClose' type='button'>Anzeige schließen</button></div><div id='sfSyncPhase' class='sf-sync-phase'>BEREIT</div><div id='sfSyncClock' class='sf-sync-clock'>--:--.---</div><div id='sfSyncTarget' class='sf-sync-target'>Noch kein Capture geplant</div><div id='sfSyncBars' class='sf-sync-bars'></div><div class='sf-sync-foot'>Bildschirm-Refresh und Browser-Timing begrenzen die Genauigkeit. Balken sind eine visuelle Orientierung, kein Millisekunden-Nachweis.</div></div>
</div>
<div id='clusterCaptureCountdown' role='timer' aria-label='Countdown zum geplanten Capture' style='display:none;margin:12px 0;padding:15px 17px;border-radius:12px;border:1px solid #64748b55;align-items:center;gap:16px'>
 <span id='clusterCaptureLamp' aria-hidden='true' style='display:inline-block;flex:none;width:23px;height:23px;border-radius:50%;background:#dc2626;box-shadow:0 0 18px #dc262699'></span>
 <div><div id='clusterCaptureClock' style='font-size:2.1rem;font-weight:700;font-variant-numeric:tabular-nums;letter-spacing:.06em'>00:10.0</div><div id='clusterCaptureTargetText' class='muted' style='font-size:.85em'></div></div>
</div>
<style>
@keyframes sfCaptureRedPulse{0%,42%{opacity:1;box-shadow:0 0 25px 9px #dc262655}55%,100%{opacity:.20;box-shadow:0 0 3px 0 #dc262622}}
@keyframes sfCaptureGreenPulse{0%,22%,44%,66%,88%,100%{opacity:1;box-shadow:0 0 34px 12px #22c55e88}11%,33%,55%,77%{opacity:.28;box-shadow:0 0 8px 0 #22c55e55}}
#clusterCaptureCountdown.sf-armed #clusterCaptureLamp{animation:sfCaptureRedPulse 1s infinite;background:#dc2626}
#clusterCaptureCountdown.sf-fired #clusterCaptureLamp{animation:sfCaptureGreenPulse 2.8s ease-in-out 1;background:#16a34a}
#sfSyncScreen{position:fixed;inset:0;z-index:30000;background:#050816;color:#f8fafc;box-sizing:border-box;padding:clamp(12px,2vw,28px);overflow:auto;font-family:system-ui,sans-serif}
.sf-sync-stage{min-height:100%;display:flex;flex-direction:column;justify-content:center;gap:clamp(14px,2vh,24px);max-width:1800px;margin:0 auto}
.sf-sync-top{display:flex;align-items:center;justify-content:space-between;gap:16px;font-size:clamp(20px,2.4vw,38px);letter-spacing:.08em}
#sfSyncClose{font-size:clamp(16px,1.7vw,24px);padding:12px 18px;flex-shrink:0;cursor:pointer}
.sf-sync-phase{text-align:center;font-weight:900;font-size:clamp(28px,4vw,64px);color:#f87171}
.sf-sync-clock{text-align:center;font-weight:900;font-size:clamp(88px,18vw,360px);line-height:1.02;font-variant-numeric:tabular-nums;letter-spacing:.02em;white-space:nowrap}
.sf-sync-target{text-align:center;font-size:clamp(18px,2.1vw,34px);margin-bottom:1vh}
.sf-sync-bars{display:grid;gap:clamp(12px,2vh,24px)}
.sf-sync-row{display:flex;align-items:center;gap:18px}
.sf-sync-label{width:clamp(90px,13vw,180px);flex:none;text-align:right;font-size:clamp(20px,2.6vw,38px);font-weight:800;font-variant-numeric:tabular-nums}
.sf-sync-track{flex:1;height:clamp(34px,6vh,82px);background:#1f2937;position:relative;overflow:hidden;border:2px solid #64748b;border-radius:14px;box-shadow:inset 0 0 0 1px #0f172a,0 0 20px #02061766}
.sf-sync-fill{height:100%;width:0;background:var(--bar-color);box-shadow:0 0 28px var(--bar-color)}
.sf-sync-block{position:absolute;top:0;bottom:0;width:12%;left:0;background:var(--bar-color);box-shadow:0 0 28px var(--bar-color)}
.sf-sync-foot{margin-top:1vh;text-align:center;font-size:clamp(14px,1.3vw,22px);color:#cbd5e1}
@media(max-width:700px){.sf-sync-top{font-size:16px}.sf-sync-top strong{font-size:16px}.sf-sync-label{width:74px;font-size:15px}.sf-sync-track{height:28px}.sf-sync-clock{font-size:16vw}}
</style>
<div id='clusterCaptureFeedback' class='muted' role='status'>Aufträge werden vorab verteilt. Zeitangaben gelten als UTC; gemeldet wird zunächst der Software-Dispatch, nicht der Belichtungsbeginn.</div>
<div id='clusterCaptureReport' style='display:none;margin-top:14px;padding:14px;border:1px solid #64748b66;border-radius:12px' aria-live='polite'>
 <h4 style='margin:0 0 8px'>Capture-Fertigstellungsbericht</h4>
 <div id='clusterCaptureReportSummary' role='status'>Noch kein Auftrag gestartet.</div>
 <button type='button' id='sfShowAllCaptureResults' style='margin:8px 0'>Alle Ergebnisse anzeigen (<span id='sfCaptureGalleryCount'>0</span>)</button>
 <div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Node</th><th>Auftrag</th><th>Ergebnis</th><th>Foto / Zeitpunkt / Abruf</th></tr></thead><tbody id='clusterCaptureReportRows'></tbody></table></div>
 <p class='muted' style='margin-bottom:0'>„JPEG erfolgreich“ bestätigt die Kamera-Rückmeldung des Nodes; kein Nachweis für exakten Belichtungsbeginn oder langfristige Speicherung. Bericht nur während dieser Browser-Sitzung; Job-Verlauf im Coordinator-RAM ist begrenzt.</p>
</div>
<dialog id='sfCaptureGalleryDialog' style='width:min(96vw,1250px);max-width:1250px;max-height:90vh;box-sizing:border-box;padding:18px;border-radius:14px'>
 <div style='display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap'><h3 style='margin:0'>Ergebnisse des letzten Shots</h3><button type='button' id='sfCaptureGalleryClose'>Schließen</button></div>
 <p class='muted'>Nur vom Node bestätigte JPEGs seit dem Öffnen dieser Seite. Die Bilder liegen auf den Nodes und können später nicht mehr verfügbar sein. Ein fehlgeschlagener Abruf ist kein erfolgreicher Snapshot.</p>
 <div id='sfCaptureGalleryItems' style='display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px'></div>
</dialog>
<details><summary>SD-Wartung: Vorprüfung (ohne Löschen)</summary><p class='muted'>Prüft nur SD- und Recording-Status der ausgewählten Nodes. Es werden keine Dateien gelöscht.</p><button type='button' id='clusterSdPreflight'>SD-Löschung prüfen</button><p class='muted' id='clusterSdPreflightResult' role='status'>Noch nicht geprüft.</p></details>
<div id='sfPowerActionNotice' role='status' aria-live='polite' style='display:none;position:fixed;z-index:31001;top:16px;right:16px;max-width:min(88vw,390px);box-sizing:border-box;padding:12px 16px;border:1px solid #60a5fa;border-radius:10px;background:#101c33;color:#f8fafc;box-shadow:0 6px 24px #0008;font:500 14px/1.5 system-ui,sans-serif;white-space:normal'></div>
<div id='sfSyncNotice' role='status' aria-live='polite' style='display:none;position:fixed;z-index:31000;top:20px;left:50%;transform:translateX(-50%);width:min(90vw,640px);box-sizing:border-box;padding:20px 24px;border:2px solid #60a5fa;border-radius:16px;background:#101c33;color:#f8fafc;box-shadow:0 12px 55px #0009;font:600 clamp(16px,2vw,22px)/1.4 system-ui,sans-serif;text-align:center;white-space:normal'></div>
)__HEALTH__";
    html += webconfigClusterMountFilterHtml();
    html += R"__HEALTH__(<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Auswahl</th><th>Gerät</th><th>Montage</th><th>Firmware</th><th>Rolle</th><th>Letzte Aktion</th><th>SD belegt</th><th>SD frei</th><th>CPU-Temperatur</th><th>Uptime</th><th>Boot-ID</th><th>Status</th></tr></thead>
<tbody id='clusterHealthRows'><tr><td colspan='12'>Lädt …</td></tr></tbody></table></div>
<section class='settings-section sf-cluster-overview'><h3>Letzte Cluster-Aktion</h3>
<div id='clusterBulkFeedback' role='status'>Noch keine Sammelaktion gestartet.</div>
<div id='clusterBulkActionRows' class='cluster-table-wrap' style='display:none'><table class='cluster-table'><thead><tr><th>Node</th><th>Übertragung</th><th>Nachweis</th></tr></thead><tbody id='clusterBulkActionBody'></tbody></table></div>
<details><summary>Systemwerte und technische Hinweise</summary><div id='clusterHealthExtra' class='muted'>Lädt …</div><p class='muted'>CPU-Auslastung wird derzeit nicht gemessen. Temperatur ist der interne ESP32-Messwert, kein Sensor für die Umgebung.</p></details></section>

</section><script>
(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function fmt(n){return (Number(n)/1024).toFixed(1)+' GiB';}
function up(s){s=Math.max(0,Number(s)||0);return Math.floor(s/86400)+'d '+Math.floor((s%86400)/3600)+'h '+Math.floor((s%3600)/60)+'m';}
function isStandbyBroadcaster(n){var m=String(n&&n.mode||'');return m==='time_broadcaster'||m==='time_broadcaste';}
function isActionableNode(n){return !!n&&!n.local&&!isStandbyBroadcaster(n)&&!!n.online;}
function nodeUrl(n){var ip=String(n.ip||''),p=ip.split('.');if(p.length!==4||!p.every(function(x){return /^\d{1,3}$/.test(x)&&Number(x)<=255;})||ip==='0.0.0.0'||ip==='255.255.255.255')return '';return 'http://'+ip+'/';}
function latestJob(n,jobs){var id=String(n.integration_id||'');var a=jobs.filter(function(j){return String(j.node)===id&&Number(j.kind)!==4;});if(!a.length)return '–';var j=a[a.length-1], kind=Number(j.kind),state=Number(j.state);var action=kind===1?'Neustart':kind===2?'Shutdown':kind===3?'SD Wipe':kind===6?'Foto':kind===7?'Drone Mode':'Verbindungstest';var stateText={1:'Wartet',2:'Gesendet',3:'ACK bestätigt',5:'Erfolgreich',6:'Abgelehnt',7:'Keine ACK'}[state]||'Status '+state;
if(kind===7&&state===5)stateText=Number(j.result)===1?'Eingeschaltet':'Ausgeschaltet';
if(kind===3&&state===6)stateText=Number(j.result)===100?'Keine SD / Wartung belegt':('SD Wipe fehlgeschlagen (Code '+(j.result||0)+')');
else if(state===6)stateText=Number(j.result)===2?'Abgelehnt: Aufnahme/Sicherheitsbedingung':(Number(j.result)===1?'Sendefehler':'Fehler (Code '+(j.result||0)+')');
if(j.boot_change_observed&&kind===1)stateText+=' · neuer Boot erkannt';
if(j.ack_latency_ms!=null)stateText+=' · '+j.ack_latency_ms+' ms';
return action+': '+stateText;}
function renderHealth(d){var rows=document.getElementById('clusterHealthRows');if(!rows)return;
var ns=Array.isArray(d.nodes)?d.nodes:[],jobs=Array.isArray(d.job_probes)?d.job_probes:[];ns.sort(function(a,b){return (a.cluster_role==='coordinator'?0:1)-(b.cluster_role==='coordinator'?0:1);});
rows.innerHTML=ns.map(function(n){var ok=!!n.health_valid,total=Number(n.sd_total_mib||0),used=Number(n.sd_used_mib||0);
return '<tr data-node-id="'+esc(n.integration_id||'')+'"><td>'+(isActionableNode(n)?'<input type="checkbox" class="cluster-node-choice" value="'+esc(n.integration_id)+'" '+(window.sfSelectedNodes&&window.sfSelectedNodes.has(String(n.integration_id))?'checked':'')+'>':'–')+'</td><td>'+(nodeUrl(n)?'<a href="'+nodeUrl(n)+'" target="_blank" rel="noopener noreferrer">'+esc(n.hostname||n.integration_id)+'</a>':esc(n.hostname||n.integration_id))+(window.sfPrecisionBadge?window.sfPrecisionBadge(n):'')+'</td><td>'+esc(window.sfMountText?window.sfMountText(n):'–')+'</td><td>'+esc(n.release||'–')+'</td><td>'+esc(isStandbyBroadcaster(n)?'Zeit-Broadcaster (Standby)':(n.cluster_role||'–'))+'</td><td>'+esc(latestJob(n,jobs))+'</td><td>'+(ok&&total?fmt(used):'–')+'</td><td>'+(ok&&total?fmt(Math.max(0,total-used)):'–')+'</td><td>'+(ok&&Number(n.cpu_temp_deci_c)>-1000?(Number(n.cpu_temp_deci_c)/10).toFixed(1)+' °C':'–')+'</td><td>'+esc(up(n.uptime_seconds))+'</td><td><code>'+esc(n.boot_id||'–')+'</code></td><td>'+esc(n.recording?'Recording':'Bereit')+'</td></tr>';}).join('');
var extra=document.getElementById('clusterHealthExtra');if(extra)extra.textContent=ns.map(function(n){return (n.hostname||n.integration_id)+': freier Heap '+(n.health_valid?Math.round(Number(n.free_heap_bytes||0)/1024)+' KiB':'unbekannt');}).join(' · ');
sfRenderEspNowDiag(d);
if(window.sfCaptureReportUpdate)window.sfCaptureReportUpdate(d);
if(window.sfMountApplyFilter)window.sfMountApplyFilter(ns);
if(window.sfSelectionUpdate)window.sfSelectionUpdate();

}
function pollHealth(){window.sfCoordinateStatus().then(function(d){renderHealth(d);}).catch(function(){});}
window.sfSelectedNodes=new Set();
function sfActionableSelectedIds(){var nodes=Array.isArray((sfLatestStatus||{}).nodes)?sfLatestStatus.nodes:[];return nodes.filter(function(n){return isActionableNode(n)&&window.sfSelectedNodes.has(String(n.integration_id||''));}).map(function(n){return String(n.integration_id);});}
window.sfSelectionUpdate=function(){var els=Array.from(document.querySelectorAll('.cluster-node-choice'));var ids=new Set(els.map(function(x){return x.value;}));Array.from(window.sfSelectedNodes).forEach(function(id){if(!ids.has(id))window.sfSelectedNodes.delete(id);});var label=document.getElementById('clusterSelectionCount');if(label)label.textContent=window.sfSelectedNodes.size+' ausgewählt';sfRenderHwTimeInfo();};
window.sfHwTimeNodeBoards=window.sfHwTimeNodeBoards||{};
function sfHwBoardLabel(kind){return kind==='freenove'?'Freenove':'XIAO';}
function sfHwSourcePin(kind){return kind==='xiao'?'GPIO2 (D1)':(kind==='freenove'?'GPIO48 (RGB-LED, Diagnose-Reserve)':'unbekannt');}
function sfHwPinText(pin){return Number(pin)>=0?('GPIO'+pin):'NONE';}
function sfHwSelectedNodes(){var d=sfLatestStatus||{},nodes=Array.isArray(d.nodes)?d.nodes:[];return orderedNodes(nodes,d).filter(function(n){return isActionableNode(n)&&window.sfSelectedNodes.has(String(n.integration_id||''));});}
function sfRenderHwTimeInfo(){
  var box=document.getElementById('clusterHardwareTimeInfo'),choice=document.getElementById('clusterActionChoice');
  if(!box||!choice)return;
  if(choice.value!=='clusterHardwareTime'){box.style.display='none';return;}
  box.style.display='block';
  var d=sfLatestStatus||{},selected=sfHwSelectedNodes(),cfg=document.getElementById('clusterHardwareTimeNodeConfig'),rows=document.getElementById('clusterHardwareTimeWiringRows'),status=document.getElementById('clusterHardwareTimeStatus');
  var localName='Coordinator';
  var allNodes=Array.isArray(d.nodes)?d.nodes:[];
  for(var i=0;i<allNodes.length;i++){if(allNodes[i]&&allNodes[i].local){localName=String(allNodes[i].hostname||localName);break;}}
  var html='';
  if(!selected.length){
    html="<span class='muted'>Noch keine Nodes ausgewählt. Standardannahme für XIAO-Nodes: Messausgang GPIO2 (D1).</span>";
  }else{
    html="<span class='muted'>Boardtyp der ausgewählten Mess-Nodes:</span> ";
    selected.slice(0,window.sfHwTimeMeta.maxNodes||3).forEach(function(n){
      var id=String(n.integration_id||''),v=window.sfHwTimeNodeBoards[id]||'xiao';
      html+="<label style='display:inline-flex;align-items:center;gap:6px;margin-right:10px;flex-wrap:wrap'>"+esc(n.hostname||id)+" <select class='sf-hwtime-node-board' data-node-id='"+esc(id)+"'><option value='xiao'"+(v==='xiao'?' selected':'')+">XIAO</option><option value='freenove'"+(v==='freenove'?' selected':'')+">Freenove</option></select></label>";
    });
    if(selected.length>(window.sfHwTimeMeta.maxNodes||3))html+="<span class='muted'>Es sind mehr als "+(window.sfHwTimeMeta.maxNodes||3)+" Nodes ausgewählt. Für die Hardware-Zeitmessung werden nur die ersten "+(window.sfHwTimeMeta.maxNodes||3)+" berücksichtigt.</span>";
  }
  cfg.innerHTML=html;
  var used=selected.slice(0,window.sfHwTimeMeta.maxNodes||3);
  var wiring='';
  used.forEach(function(n,idx){
    var id=String(n.integration_id||''),kind=window.sfHwTimeNodeBoards[id]||'xiao',coordPin=(window.sfHwTimeMeta.inputs||[])[idx];
    var srcText=sfHwSourcePin(kind);
    var note=(kind==='freenove')?"<div class='muted'>GPIO48 ist mit der Onboard-RGB-LED geteilt. Nur für die spätere Diagnose nach Hardwareprüfung verwenden.</div>":'';
    wiring+="<tr><td><strong>Signal "+(idx+1)+"</strong></td><td><strong>"+esc(n.hostname||id)+"</strong> · "+esc(sfHwBoardLabel(kind))+" · "+esc(srcText)+note+"</td><td>→</td><td><strong>"+esc(localName)+"</strong> · Eingang "+(idx+1)+" · "+esc(sfHwPinText(coordPin))+"</td></tr>";
  });
  if(!used.length)wiring="<tr><td colspan='4' class='muted'>Wähle zuerst 1 bis 3 entfernte Nodes aus.</td></tr>";
  wiring+="<tr><td><strong>GND</strong></td><td>Alle ausgewählten Nodes · GND</td><td>↔</td><td><strong>"+esc(localName)+"</strong> · GND<div class='muted'>Für direkte GPIO-Messleitungen erforderlich. Eine gemeinsame USB-Stromversorgung allein nicht ungeprüft voraussetzen.</div></td></tr>";
  rows.innerHTML=wiring;
  var missing=(window.sfHwTimeMeta.inputs||[]).slice(0,3).some(function(v){return !(Number(v)>=0);});
  var text='Coordinator-Board: '+String(window.sfHwTimeMeta.coordinatorBoard||'-')+'. ';
  if(missing)text+='Hardware-Zeitmessung bleibt gesperrt (Eingänge NONE). Freenove-Kandidaten: GPIO14 (Magnetschalter), GPIO21 (Präsenz), GPIO41 (Radar-UART RX). Diese Anschlüsse sind NICHT frei und benötigen vor Aktivierung eine exklusive, sichere Umschaltung. GPIO35–37 sind wegen OPI-PSRAM ungeeignet.';
  else text+='Die Tabelle zeigt die vorgesehene Verdrahtung in der Reihenfolge der ausgewählten Nodes.';
  status.textContent=text;
}
document.addEventListener('change',function(e){if(e.target.classList.contains('cluster-node-choice')){if(e.target.checked)window.sfSelectedNodes.add(e.target.value);else window.sfSelectedNodes.delete(e.target.value);window.sfSelectionUpdate();}else if(e.target.id==='clusterActionChoice'){sfRenderHwTimeInfo();}else if(e.target.classList.contains('sf-hwtime-node-board')){window.sfHwTimeNodeBoards[e.target.getAttribute('data-node-id')]=e.target.value;sfRenderHwTimeInfo();}});
function setAll(v){document.querySelectorAll('.cluster-node-choice').forEach(function(e){var tr=e.closest('tr');if(v&&tr&&tr.style.display==='none')return;e.checked=v;if(v)window.sfSelectedNodes.add(e.value);else window.sfSelectedNodes.delete(e.value);});window.sfSelectionUpdate();}
document.getElementById('clusterSelectAll').addEventListener('click',function(){setAll(true);});
document.getElementById('clusterSelectNone').addEventListener('click',function(){setAll(false);});
var actionBusy=false,sfSyncNoticeTimer=null,sfPowerNoticeTimer=null;
function sfShowPowerNotice(message,autohide){
 var el=document.getElementById('sfPowerActionNotice');if(!el)return;
 if(sfPowerNoticeTimer!==null){clearTimeout(sfPowerNoticeTimer);sfPowerNoticeTimer=null;}
 el.textContent=message;el.style.display='block';
 if(autohide)sfPowerNoticeTimer=setTimeout(function(){el.style.display='none';sfPowerNoticeTimer=null;},7000);
}
function sfRenderEspNowDiag(d){
 var local=document.getElementById('sfEspNowActionDiag'),nodes=document.getElementById('sfEspNowNodeDiag');
 var s=(d||{}).espnow_time_local||{};
 if(local){
  var fresh=s.precision_recently_accepted===true;
  var last=fresh?'vor '+(Number(s.last_accepted_age_ms)/1000).toFixed(1)+' s':'kein aktuell gültiger Präzisions-Sync';
  local.textContent='Coordinator-Funk: '+(s.active?'aktiv':'inaktiv')+' · Kanal '+(s.channel||'-')+' · gültige Pakete '+(s.authenticated||0)+' · abgelehnt '+(s.rejected||0)+' · beantwortete Probes '+(s.probes_answered||0)+' · Coordinator beantwortet Messprobes (eigener Node-Offset nicht anwendbar) · Aktionspause '+Math.ceil((Number(s.action_pause_remaining_ms)||0)/1000)+' s';
 }
 if(nodes){
   var a=Array.isArray(d.nodes)?d.nodes.filter(function(n){return n.cluster_role!=='coordinator';}):[];
   var reports=Array.isArray(d.espnow_node_reports)?d.espnow_node_reports:[];
   function num(v,unit,digits){return Number.isFinite(Number(v))?Number(v).toFixed(digits||0)+' '+unit:'–';}
   function seriesStatsRows(r){
     if(!r.has_series_statistics)return '<tr><td>Serienstatistik</td><td>Noch kein Statistikbericht empfangen</td></tr>';
     var good=Number(r.series_successful)||0,bad=Number(r.series_rejected)||0,total=good+bad;
     var result=({0:'Noch keine abgeschlossene Serie',1:'Erfolgreich',2:'Verworfen',3:'Zeitlimit überschritten'})[Number(r.last_series_result)]||'Unbekannt';
     var since=Number(r.last_series_result)?num((Number(r.last_series_age_ms)+Number(r.received_age_ms))/1000,'s',1):'–';
     return '<tr><td>Messserien erfolgreich / verworfen</td><td>'+good+' / '+bad+'</td></tr>'+
       '<tr><td>Erfolgsquote (abgeschlossene Serien)</td><td>'+(total?(100*good/total).toFixed(1)+' %':'–')+'</td></tr>'+
       '<tr><td>Letzter abgeschlossener Messversuch</td><td>'+esc(result)+' · vor '+esc(since)+'</td></tr>'+
       (r.has_last_rejected_series ? (function(){
          var reason=({1:'Ungültige/zu wenige Samples',2:'Offset-Streuung zu groß',3:'RTT zu hoch',4:'Zeitlimit der Serie überschritten'})[Number(r.last_rejected_reason)]||'Unbekannt';
          var failAge=(Number(r.last_rejected_age_ms||0)+Number(r.received_age_ms||0))/1000;
          return '<tr><td>Letzte verworfene Serie (auch nach Erfolg)</td><td>'+esc(reason)+' · vor '+num(failAge,'s',1)+'</td></tr>'+
           '<tr><td>Letzte verworfene Qualitätswerte</td><td>'+(r.last_rejected_metrics_valid?'RTT '+num(r.last_rejected_rtt_us,'µs')+' · Streuung '+num(r.last_rejected_spread_us,'µs'):'Keine vollständige Schätzung')+'</td></tr>'+
           (r.last_rejected_metrics_valid && r.has_failure_offsets ? '<tr><td>Verworfene Offset-Schätzer</td><td>'+num(r.failed_min_rtt_offset_us,'µs')+' / '+num(r.failed_selected_offset_us,'µs')+'</td></tr>':'');
        })() : '');
   }
   var opened={};nodes.querySelectorAll('details[data-sync-node]').forEach(function(el){if(el.open)opened[el.getAttribute('data-sync-node')]=true;});
   nodes.innerHTML='<p class="muted">Authentifizierte ESP-NOW-Ergebnisberichte, nach Node-Boot-ID zugeordnet. Alter und Qualität sind keine Garantie für GPIO-Genauigkeit.</p>'+
     a.map(function(n){
       var r=reports.find(function(x){return String(x.boot_id||'').toLowerCase()===String(n.boot_id||'').toLowerCase();});
       var name=esc(n.hostname||n.integration_id||'Node');
       if(!r)return '<details data-sync-node="'+esc(String(n.boot_id||n.hostname||''))+'"><summary><strong>'+name+'</strong> · ESP-NOW: keine Messung gemeldet</summary><p class="muted">Kein authentifizierter Ergebnisbericht mit passender aktueller Boot-ID. Bitte Drone Mode, Sync-Freigabe und Node-Verbindung prüfen.</p></details>';
       if(!r.precision_accepted){
         var reason=({0:'Noch kein vollständiger, akzeptierter Sync',1:'Zu wenige/ungültige Samples',2:'Offset-Streuung zu groß',3:'RTT zu hoch',4:'Zeitlimit der Serie überschritten'})[Number(r.failure_code)]||'Unbekannt';
         return '<details data-sync-node="'+esc(String(n.boot_id||n.hostname||''))+'"><summary><strong>'+name+'</strong> · ESP-NOW: Diagnose aktiv, noch kein gültiger Sync</summary>'+
          '<table><tr><td>Probes gesendet</td><td>'+esc(r.probes_sent||0)+'</td></tr>'+
          '<tr><td>Vollständige Exchanges</td><td>'+esc(r.exchanges_complete||0)+'</td></tr>'+
          seriesStatsRows(r)+
          '<tr><td>Fehlgeschlagene Serien (Legacy, max. 255)</td><td>'+esc(r.failed_series||0)+'</td></tr>'+
          '<tr><td>Letzter Ablehnungsgrund</td><td>'+esc(reason)+'</td></tr>'+
          '<tr><td>Letzte verworfene Serie</td><td>'+(r.has_failure_metrics?'RTT '+num(r.failed_rtt_us,'µs')+' · Streuung '+num(r.failed_spread_us,'µs'):'noch nicht gemeldet')+'</td></tr>'+
          '<tr><td>Min-RTT vs. Quartilsmedian</td><td>'+(r.has_failure_offsets?num(r.failed_min_rtt_offset_us,'µs')+' / '+num(r.failed_selected_offset_us,'µs'):'–')+'</td></tr>'+
          '<tr><td>Samples gültig / nach RTT gewählt</td><td>'+(r.has_failure_metrics?esc(String(r.failed_accepted))+' / '+esc(String(r.failed_selected)):'–')+'</td></tr>'+
          '<tr><td>Bericht empfangen</td><td>vor '+num(Number(r.received_age_ms)/1000,'s',1)+'</td></tr></table></details>';
       }
       var age=(Number(r.received_age_ms)||0)+(Number(r.sample_age_ms)||0);
       var recent=age<=120000&&Number(r.accepted_samples)>=8&&r.precision_accepted===true&&Number(r.received_age_ms)<=15000;
       var head=recent?'gültige Messung vor '+Math.round(age/1000)+' s':'veraltet / nicht bestätigt (Alter '+Math.round(age/1000)+' s)';
       return '<details data-sync-node="'+esc(String(n.boot_id||n.hostname||''))+'"><summary><strong>'+name+'</strong> · ESP-NOW: '+esc(head)+' · RTT '+num(r.rtt_us,'µs')+'</summary>'+
        '<table><tr><td>Letzte gültige Messung (Alter)</td><td>'+num(age/1000,'s',1)+'</td></tr>'+
        '<tr><td>Letzter Bericht empfangen</td><td>'+num(Number(r.received_age_ms)/1000,'s',1)+' her</td></tr>'+
        '<tr><td>Offset (Node → Coordinator)</td><td>'+num(r.offset_us,'µs',0)+'</td></tr>'+
        '<tr><td>Median-RTT</td><td>'+num(r.rtt_us,'µs',0)+'</td></tr>'+
        '<tr><td>Akzeptierte Samples</td><td>'+esc(String(r.accepted_samples))+'</td></tr>'+
        '<tr><td>Offset-Streuung</td><td>'+num(r.spread_us,'µs',0)+'</td></tr>'+
        seriesStatsRows(r)+
        '<tr><td>Drift</td><td>'+(r.has_drift_report&&r.drift_valid?num(r.drift_ppm,'ppm',3):'noch nicht kalibriert')+'</td></tr>'+
        '<tr><td>ESP-NOW-Bericht</td><td>'+ (r.precision_accepted?'authentifiziert':'ungültig')+'</td></tr>'+
        '<tr><td>Aktive Zeitquelle</td><td>'+(r.has_time_source?(r.using_espnow_time?'ESP-NOW-Präzision':'ESP-NOW nicht gewählt (Fallback/keine UTC)'):'nicht zentral gemeldet')+'</td></tr></table></details>';
     }).join('');
   nodes.querySelectorAll('details[data-sync-node]').forEach(function(el){if(opened[el.getAttribute('data-sync-node')])el.open=true;});
 }
}
function sfIsPowerAction(path){return path==='/cluster_node_restart'||path==='/cluster_node_shutdown'||path==='/cluster_node_drone';}
function sfShowSyncNotice(message,autohide){
 var el=document.getElementById('sfSyncNotice');
 if(sfSyncNoticeTimer!==null){clearTimeout(sfSyncNoticeTimer);sfSyncNoticeTimer=null;}
 el.textContent=message;el.style.display='block';
 if(autohide)sfSyncNoticeTimer=setTimeout(function(){el.style.display='none';sfSyncNoticeTimer=null;},6500);
}
async function runBulk(path,label,danger){if(actionBusy)return;var ids=sfActionableSelectedIds();var out=document.getElementById('clusterBulkFeedback');if(!ids.length){out.textContent='Bitte zuerst mindestens einen entfernten Node auswählen.';if(path==='/cluster_node_sync_time')sfShowSyncNotice('Keine Nodes ausgewählt – Zeitsynchronisierung nicht gestartet.',true);if(sfIsPowerAction(path))sfShowPowerNotice('Bitte zuerst mindestens einen Node auswählen.',true);return;}
var text=label+' für '+ids.length+' ausgewählte Nodes auslösen?';if(danger)text+=' ACHTUNG: Shutdown ohne Wake-Quellen; RESET oder Stromversorgung zum Wiederaufwachen erforderlich.';
if((path==='/cluster_node_restart'||path==='/cluster_node_shutdown'||path==='/cluster_node_wipe')&&!confirm(path==='/cluster_node_wipe'?'ACHTUNG: Alle SD-Daten der ausgewählten Nodes unwiderruflich löschen? Laufende Aufnahmen werden SOFORT beendet und ebenfalls gelöscht. Konfiguration wird aus internem Backup wiederhergestellt.':text))return;actionBusy=true;window.sfCoordinatorBulkActive=true;var buttons=['clusterBulkProbe','clusterBulkRestart','clusterBulkShutdown','clusterBulkSync','clusterBulkWipe','clusterBulkDroneOn','clusterBulkDroneOff'];buttons.forEach(function(x){document.getElementById(x).disabled=true;});var done=0,errors=0,details=[],actionRows=[];var actionTable=document.getElementById('clusterBulkActionRows'),actionBody=document.getElementById('clusterBulkActionBody');if(actionTable)actionTable.style.display='none';
if(path==='/cluster_node_sync_time')sfShowSyncNotice('Zeitsynchronisierung: Aufträge an '+ids.length+' Node(s) werden gesendet …',false);
if(sfIsPowerAction(path))sfShowPowerNotice(label+': Anfrage wird an '+ids.length+' Node(s) gesendet …',false);
try{for(var i=0;i<ids.length;i++){out.textContent=label+': Anfrage '+(i+1)+' von '+ids.length+' wird gesendet. Noch keine Ausführung bestätigt.';var fd=new FormData();fd.append('node_id',ids[i]);if(path==='/cluster_node_drone')fd.append('enabled',label==='Drone Mode EIN'?'1':'0');var ctrl=new AbortController(),timer=setTimeout(function(){ctrl.abort();},12000);
try{var res=await fetch(path,{method:'POST',body:fd,credentials:'same-origin',cache:'no-store',redirect:'manual',signal:ctrl.signal});if(!(res.ok||res.status===0||res.status===303)){errors++;var body='';try{body=(await res.text()).slice(0,100);}catch(ignored){}details.push(ids[i]+': HTTP '+res.status+(body?' '+body:''));actionRows.push({id:ids[i],result:'HTTP '+res.status+(body?' · '+body:''),proof:'Ausführung unbekannt'});}else{done++;details.push(ids[i]+': Auftrag vom HTTP-Endpunkt angenommen');actionRows.push({id:ids[i],result:'Vom HTTP-Endpunkt angenommen',proof:'Keine Node-Ausführung bestätigt'});}}catch(e){errors++;details.push(ids[i]+': HTTP-Anfrage fehlgeschlagen ('+(e.name==='AbortError'?'12-s-Zeitlimit':String(e.message||e))+'); Ausführung unbekannt');actionRows.push({id:ids[i],result:'HTTP ungeklärt ('+(e.name==='AbortError'?'Zeitlimit':String(e.message||e))+')',proof:'Ausführung unbekannt'});}finally{clearTimeout(timer);}await new Promise(function(resolve){setTimeout(resolve,250);});}
out.textContent=label+': '+done+' von '+ids.length+' Aufträgen vom HTTP-Endpunkt angenommen; '+errors+' HTTP-Fehler/ungeklärte Anfragen. Eine Node-Ausführung ist damit nicht bestätigt.';
if(actionBody){var names=Array.isArray((sfLatestStatus||{}).nodes)?sfLatestStatus.nodes:[];actionBody.innerHTML=actionRows.map(function(r){var n=names.find(function(x){return String(x.integration_id)===r.id;});return '<tr><td>'+esc(n?(n.hostname||r.id):r.id)+'</td><td>'+esc(r.result)+'</td><td>'+esc(r.proof)+'</td></tr>';}).join('');if(actionTable)actionTable.style.display='block';}
if(path==='/cluster_node_sync_time')sfShowSyncNotice('Zeitsynchronisierung: '+done+' von '+ids.length+' Aufträgen HTTP-angenommen, '+errors+' Fehler. Die tatsächliche Synchronisierung wird dadurch noch nicht bestätigt.',true);
if(sfIsPowerAction(path))sfShowPowerNotice(label+': '+done+' von '+ids.length+' Aufträgen vom HTTP-Endpunkt angenommen, '+errors+' HTTP-Fehler. Empfangsbestätigung und Ausführung am Node noch nicht nachgewiesen.',true);
}finally{window.sfCoordinatorBulkActive=false;actionBusy=false;window.sfCoordinateStatus().catch(function(){});buttons.forEach(function(x){document.getElementById(x).disabled=false;});}}
// Generic absolute-UTC schedule derivation: reuse for future actions such as GPIO/sequence/video.
function sfPlanUtcTarget(utcValue,seconds,minutes,hours,nowMs){
 // Future direct UTC input: pass datetime-local UTC value as utcValue; null means relative.
 if(utcValue){var fixed=Date.parse(utcValue+'Z');return Number.isFinite(fixed)?fixed:NaN;}
 if(![seconds,minutes,hours].every(function(n){return Number.isInteger(n)&&n>=0;}))return NaN;
 return nowMs+((hours*3600)+(minutes*60)+seconds)*1000;
}
// Capture report tracks one specific submission set, not the last arbitrary Node action.
// Job IDs in the bounded coordinator RAM table are authoritative after signature verification.
var sfCaptureReport=null, sfLatestStatus=null;
var sfSeenCaptureJobs=new Set();
// Only verified JPEG results associated with the most recently scheduled shot.
var sfCaptureGallery=new Map();
function sfSafeCaptureMedia(j){
 var uuid=String((j||{}).media_uuid||''),raw=String((j||{}).media_url||'');
 if(!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(uuid))return null;
 try{var p=new URL(raw);if(p.protocol!=='http:'||p.pathname!=='/capture_media'||p.searchParams.get('uuid')!==uuid||p.username||p.password||p.hash||Array.from(p.searchParams.keys()).some(function(k){return k!=='uuid';}))return null;
 // URLs are emitted from authenticated coordinator status, never from free-form HTML.
 var view=new URL(p.href);view.searchParams.set('view','1');return {download:p.href,view:view.href,uuid:uuid};
 }catch(ignore){return null;}
}
function sfRememberCapture(row){
 var j=row.job||{},media=sfSafeCaptureMedia(j);
 if(Number(j.state)!==5||!media)return;
 var key=sfCaptureJobKey(j);if(sfCaptureGallery.has(key))return;
 sfCaptureGallery.set(key,{name:String(row.name),node:String(row.id),media:media,key:key});
 var count=document.getElementById('sfCaptureGalleryCount');if(count)count.textContent=String(sfCaptureGallery.size);
}
function sfRenderCaptureGallery(){
 var host=document.getElementById('sfCaptureGalleryItems');if(!host)return;
 host.textContent='';
 if(!sfCaptureGallery.size){host.textContent='Für den letzten Shot liegen noch keine bestätigten JPEG-Ergebnisse vor.';return;}
 Array.from(sfCaptureGallery.values()).reverse().forEach(function(e){
  var card=document.createElement('article');card.style.cssText='border:1px solid #64748b66;border-radius:12px;padding:10px;min-width:0';
  var title=document.createElement('strong');title.textContent=e.name;card.appendChild(title);
  var a=document.createElement('a');a.href=e.media.view;a.target='_blank';a.rel='noopener noreferrer';a.title='Bild in voller Größe öffnen';
  var img=document.createElement('img');img.loading='lazy';img.alt='JPEG-Vorschau von '+e.name;img.style.cssText='display:block;width:100%;height:170px;object-fit:contain;margin:8px 0;background:#1f2937;border-radius:8px';img.src=e.media.view;
  a.appendChild(img);card.appendChild(a);
  var controls=document.createElement('div');controls.style.cssText='display:flex;gap:12px;flex-wrap:wrap';
  var view=document.createElement('a');view.href=e.media.view;view.target='_blank';view.rel='noopener noreferrer';view.textContent='Groß ansehen';controls.appendChild(view);
  var dl=document.createElement('a');dl.href=e.media.download;dl.target='_blank';dl.rel='noopener noreferrer';dl.textContent='Herunterladen';controls.appendChild(dl);card.appendChild(controls);host.appendChild(card);
 });
}
document.getElementById('sfShowAllCaptureResults').addEventListener('click',function(){sfRenderCaptureGallery();var d=document.getElementById('sfCaptureGalleryDialog');if(d.showModal)d.showModal();else d.setAttribute('open','');});
document.getElementById('sfCaptureGalleryClose').addEventListener('click',function(){var d=document.getElementById('sfCaptureGalleryDialog');if(d.close)d.close();else d.removeAttribute('open');});

function sfCaptureJobKey(j){return String(j.node)+'/'+String(j.job_boot)+'/'+String(j.job_seq);}
function sfCaptureState(row,now,target){
 if(row.http==='error')return ['HTTP-Fehler','Übertragung fehlgeschlagen; Ausführung unbekannt',false];
 var j=row.job;
 if(!j){return now>target+200000?['Unbestätigt','Kein zugeordneter Capture-Job mehr sichtbar',false]:['Warten','Warte auf signierten Auftragsstatus',false];}
 var st=Number(j.state),rc=Number(j.result||0);
 if(st===5)return ['JPEG erfolgreich','Signierter Abschluss vom Node bestätigt',true];
 if(st===6)return ['Fehlgeschlagen','Node meldet Fehlercode '+rc,false];
 if(st===7)return ['Zeitüberschreitung','Keine bestätigte Fertigstellung',false];
 if(st===8)return ['Abgebrochen','Auftrag abgebrochen',false];
 if(st===3)return ['Vorbereitet','Node hat den Auftrag angenommen',false];
 if(st===4)return ['In Ausführung','Warte auf JPEG-Abschluss',false];
 if(st===2)return ['Gesendet','Warte auf Node-Bestätigung',false];
 return ['Geplant','Auftrag im Coordinator vorgemerkt',false];
}
function sfRenderCaptureReport(){
 var report=sfCaptureReport,box=document.getElementById('clusterCaptureReport');if(!report||!box)return;
 box.style.display='block';var successes=0,failed=0,pending=0,now=Date.now();
 var html=report.rows.map(function(r){
  var v=sfCaptureState(r,now,report.target);
  if(v[2])successes++;else if(v[0]==='Fehlgeschlagen'||v[0]==='Zeitüberschreitung'||v[0]==='Abgebrochen'||v[0]==='HTTP-Fehler')failed++;else pending++;
  var color=v[2]?'#16a34a':failed&&['Fehlgeschlagen','Zeitüberschreitung','Abgebrochen','HTTP-Fehler'].indexOf(v[0])>=0?'#dc2626':'#b45309';
  var j=r.job||{},mediaLink=sfSafeCaptureMedia(j);
  if(v[2])sfRememberCapture(r);
  var time=String(j.dispatch_utc_us||'0'),utcText='';
  if(/^\d{16}$/.test(time)){var millis=Number(time.slice(0,-3));if(Number.isFinite(millis))utcText=new Date(millis).toISOString();}
  var media=mediaLink?'<div class=\"muted\">UUID: '+esc(mediaLink.uuid)+'</div><div class=\"muted\">Software-Trigger UTC: '+esc(utcText||'unbekannt')+'</div><a href=\"'+esc(mediaLink.download)+'\" target=\"_blank\" rel=\"noopener noreferrer\">Herunterladen</a> · <a href=\"'+esc(mediaLink.view)+'\" target=\"_blank\" rel=\"noopener noreferrer\">Ansehen</a>':'–';
  return '<tr><td><strong>'+esc(r.name)+'</strong></td><td>'+esc(r.http==='error'?'HTTP ungeklärt':r.http==='accepted'?'HTTP angenommen':'Wird übertragen')+'</td><td><strong style=\"color:'+color+'\">'+esc(v[0])+'</strong><div class=\"muted\">'+esc(v[1])+'</div></td><td>'+media+'</td></tr>';
 }).join('');
 document.getElementById('clusterCaptureReportRows').innerHTML=html;
 var overall=pending?'Läuft / ausstehend':failed?'Abgeschlossen mit Fehlern':'Alle JPEGs bestätigt';
 document.getElementById('clusterCaptureReportSummary').textContent=overall+' · '+successes+'/'+report.rows.length+' erfolgreich · '+failed+' Fehler · '+pending+' ausstehend · UTC '+new Date(report.target).toISOString();
}
window.sfCaptureReportUpdate=function(d){
 // Refresh the preview clock from each authenticated coordinator status snapshot.
 // Never use the browser wall clock as a fallback when cluster UTC is invalid.
 sfCaptureClock=sfCaptureClockFromStatus(d,performance.now());
 sfUpdatePlannedTime();
 sfLatestStatus=d;var jobs=(Array.isArray(d.job_probes)?d.job_probes:[]).filter(function(j){return Number(j.kind)===6;});
 if(sfCaptureReport){sfCaptureReport.rows.forEach(function(r){
  if(r.job)return;var candidates=jobs.filter(function(j){return String(j.node)===r.id&&!sfSeenCaptureJobs.has(sfCaptureJobKey(j));});
  if(candidates.length&&r.http!=='error'){r.job=candidates[candidates.length-1];sfSeenCaptureJobs.add(sfCaptureJobKey(r.job));}
 });sfCaptureReport.rows.forEach(function(r){if(r.job){var update=jobs.find(function(j){return sfCaptureJobKey(j)===sfCaptureJobKey(r.job);});if(update)r.job=update;}});sfRenderCaptureReport();}
};
// Shared coordinator UTC reference, anchored to a monotonic browser clock.
// This is a display/scheduling estimate, not proof of sensor exposure timing.
var sfCaptureClock=null;
function sfCaptureClockFromStatus(d,receivedPerf){
 var t=(d||{}).cluster_time||{},us=Number(t.utc_us);
 if(!t.valid||!Number.isFinite(us)||us<1600000000000000)return null;
 return {utcMs:us/1000,perfMs:receivedPerf};
}
function sfCaptureUtcNow(){return sfCaptureClock?sfCaptureClock.utcMs+(performance.now()-sfCaptureClock.perfMs):NaN;}
// Visual test uses the same monotonic target anchor as the compact countdown.
// The overlay remains visible for five seconds after the scheduled capture.
var sfCountdownTick=null,sfCountdownStartPerf=0,sfCountdownDelay=0,sfSyncRaf=null,sfSyncLastDraw=0,sfSyncAutoOpened=false,sfCountdownActive=false;
var sfSyncPeriods=[['2 s',2000,'#ffffff'],['1 s',1000,'#ff4242'],['500 ms',500,'#00aaff'],['100 ms',100,'#00f070'],['50 ms',50,'#ffe600']];
var sfSyncScreen=document.getElementById('sfSyncScreen'),sfSyncBarEls=[];
(function(){var host=document.getElementById('sfSyncBars');sfSyncPeriods.forEach(function(spec,i){
 var row=document.createElement('div');row.className='sf-sync-row';
 var label=document.createElement('div');label.className='sf-sync-label';label.textContent=spec[0];
 var track=document.createElement('div');track.className='sf-sync-track';track.style.setProperty('--bar-color',spec[2]);
 var bar=document.createElement('div');bar.className=i===0?'sf-sync-block':'sf-sync-fill';track.appendChild(bar);row.appendChild(label);row.appendChild(track);host.appendChild(row);sfSyncBarEls.push(bar);
});})();
function sfSyncOpen(autoOpened){sfSyncAutoOpened=!!autoOpened;sfSyncScreen.style.display='block';sfSyncLastDraw=0;
 if(sfSyncScreen.requestFullscreen)sfSyncScreen.requestFullscreen().catch(function(){});
 sfSyncPaint(performance.now());}
function sfSyncClose(){sfSyncAutoOpened=false;sfSyncScreen.style.display='none';if(document.fullscreenElement===sfSyncScreen&&document.exitFullscreen)document.exitFullscreen().catch(function(){});}
document.getElementById('sfSyncOpen').addEventListener('click',function(){sfSyncOpen(false);});
document.getElementById('sfSyncClose').addEventListener('click',sfSyncClose);
document.addEventListener('fullscreenchange',function(){if(document.fullscreenElement!==sfSyncScreen&&sfSyncScreen.style.display==='block')sfSyncScreen.style.display='none';});
function sfSyncFormatted(ms){var abs=Math.abs(ms),mins=Math.floor(abs/60000),secs=Math.floor(abs%60000/1000),milli=Math.floor(abs%1000);
 return (ms<0?'-':'+')+String(mins).padStart(2,'0')+':'+String(secs).padStart(2,'0')+'.'+String(milli).padStart(3,'0');}
function sfStopCaptureCountdown(){
 if(sfCountdownTick!==null){clearInterval(sfCountdownTick);sfCountdownTick=null;}
 sfCountdownActive=false;sfCountdownStartPerf=0;sfCountdownDelay=0;
 document.getElementById('clusterCaptureCountdown').style.display='none';
 if(sfSyncAutoOpened||sfSyncScreen.style.display==='block')sfSyncClose();
}
function sfSyncPaint(now){
 if(!sfCountdownStartPerf)return;
 var elapsed=now-sfCountdownStartPerf,relative=elapsed-sfCountdownDelay,after=relative>=0;
 if(relative>5000){sfStopCaptureCountdown();return;}
 var clock=document.getElementById('clusterCaptureClock'),panel=document.getElementById('clusterCaptureCountdown');
 var abs=Math.abs(relative),tenths=Math.floor(abs/100),secs=Math.floor(tenths/10);
 clock.textContent=(after?'+':'-')+String(Math.floor(secs/60)).padStart(2,'0')+':'+String(secs%60).padStart(2,'0')+'.'+String(tenths%10);
 if(panel.className!==(after?'sf-fired':'sf-armed'))panel.className=after?'sf-fired':'sf-armed';
 if(sfSyncScreen.style.display!=='block'||now-sfSyncLastDraw<15)return;
 sfSyncLastDraw=now;
 document.getElementById('sfSyncClock').textContent=sfSyncFormatted(relative);
 var phase=document.getElementById('sfSyncPhase');phase.textContent=after?'NACH DEM SOLL-ZEITPUNKT (+5 s Fenster)':'BIS ZUM SOLL-ZEITPUNKT';phase.style.color=after?'#4ade80':'#f87171';
 // Phase is anchored to the target, not to when the overlay was opened.
 sfSyncPeriods.forEach(function(spec,i){var cycle=((relative%spec[1])+spec[1])%spec[1],triangle=1-Math.abs(2*cycle/spec[1]-1),el=sfSyncBarEls[i];
 if(i===0)el.style.left=(88*triangle).toFixed(2)+'%';else el.style.width=(100*triangle).toFixed(2)+'%';});
}
function sfShowCaptureCountdown(target){
 if(sfCountdownTick!==null){clearInterval(sfCountdownTick);sfCountdownTick=null;}
 var panel=document.getElementById('clusterCaptureCountdown');
 document.getElementById('clusterCaptureTargetText').textContent='Geplanter UTC-Termin: '+new Date(target).toISOString()+' · Countdown nach geschätzter Coordinator-Clusterzeit';
 document.getElementById('sfSyncTarget').textContent='SOLL UTC '+new Date(target).toISOString()+' · Anzeige endet 5 s danach';
 panel.style.display='flex';panel.className='sf-armed';
 sfCountdownStartPerf=performance.now();sfCountdownDelay=Math.max(0,target-sfCaptureUtcNow());sfCountdownActive=true;
 sfSyncOpen(true);
 sfSyncPaint(sfCountdownStartPerf);
 // Keep the compact timer and the fullscreen test image running until +5 s.
 sfCountdownTick=setInterval(function(){sfSyncPaint(performance.now());},50);
}
document.getElementById('clusterBulkCapture').addEventListener('click',async function(){
 var ids=sfActionableSelectedIds(),out=document.getElementById('clusterCaptureFeedback');
 if(!ids.length){out.textContent='Bitte Nodes auswählen.';return;}
 var seconds=Number(document.getElementById('clusterCaptureSeconds').value),minutes=Number(document.getElementById('clusterCaptureMinutes').value),hours=Number(document.getElementById('clusterCaptureHours').value);
 if(['clusterCaptureHours','clusterCaptureMinutes','clusterCaptureSeconds'].some(function(id){return document.getElementById(id).value==='';})){out.textContent='Stunden, Minuten und Sekunden müssen ausgefüllt sein.';return;}
 // Refresh coordinator time before computing a relative or absolute UTC job.
 // Do not silently fall back to the browser clock if the cluster time is unavailable.
 var snap;
 try{var readStart=performance.now();snap=await window.sfCoordinateStatus(true);var readEnd=performance.now();sfCaptureClock=sfCaptureClockFromStatus(snap,(readStart+readEnd)/2);}
 catch(e){out.textContent='Clusterzeit nicht abrufbar: Capture-Auftrag nicht gesendet.';return;}
 if(!sfCaptureClock){out.textContent='Coordinator hat noch keine gültige Clusterzeit: Capture-Auftrag nicht gesendet.';return;}
 var nowUtc=sfCaptureUtcNow(),target=sfPlanUtcTarget(null,seconds,minutes,hours,nowUtc);
 if(!Number.isFinite(target)||target-nowUtc<5000||target-nowUtc>1800000){out.textContent='Zielzeit muss 5 Sekunden bis 30 Minuten in der Cluster-Zukunft liegen.';return;}
 // A new valid shot replaces the previous gallery; never mix results from different shots.
 sfCaptureGallery.clear();
 var galleryCount=document.getElementById('sfCaptureGalleryCount');if(galleryCount)galleryCount.textContent='0';
 var galleryDialog=document.getElementById('sfCaptureGalleryDialog');
 if(galleryDialog&&galleryDialog.open){if(galleryDialog.close)galleryDialog.close();else galleryDialog.removeAttribute('open');}
 var testPulse=!!(document.getElementById('clusterCaptureTestPulse')||{}).checked;
 var button=this;button.disabled=true;window.sfCoordinatorBulkActive=true;var results=[],accepted=0,shown=false;
 // Use the freshly fetched pre-submit status, not the possibly stale UI poll status.
 // Otherwise an old completed JPEG can be assigned to the next capture report.
 (Array.isArray(snap.job_probes)?snap.job_probes:[]).forEach(function(j){if(Number(j.kind)===6)sfSeenCaptureJobs.add(sfCaptureJobKey(j));});
 var names=Array.isArray((sfLatestStatus||{}).nodes)?sfLatestStatus.nodes:[];
 sfCaptureReport={target:target,rows:ids.map(function(id){var n=names.find(function(x){return String(x.integration_id)===id;});return {id:id,name:n?(n.hostname||id):id,http:'pending',job:null};})};sfRenderCaptureReport();
 try{for(var i=0;i<ids.length;i++){
  var fd=new FormData();fd.append('node_id',ids[i]);fd.append('utc_ms',String(Math.round(target)));if(testPulse)fd.append('test_pulse','1');
  var ctrl=new AbortController(),timer=setTimeout(function(){ctrl.abort();},12000);
  try{var r=await fetch('/cluster_node_capture',{method:'POST',body:fd,credentials:'same-origin',redirect:'manual',cache:'no-store',signal:ctrl.signal});
      if(r.ok||r.status===303||r.status===0){accepted++;if(!shown){sfShowCaptureCountdown(target);shown=true;}sfCaptureReport.rows[i].http='accepted';results.push(ids[i]+': HTTP angenommen');}
      else{sfCaptureReport.rows[i].http='error';var msg='';try{msg=(await r.text()).slice(0,110);}catch(ignored){}results.push(ids[i]+': HTTP '+r.status+(msg?' '+msg:''));}}
  catch(e){sfCaptureReport.rows[i].http='error';results.push(ids[i]+': Übertragungsfehler ('+(e.name==='AbortError'?'Zeitlimit':String(e.message||e))+'); Ausführung unbekannt');}
  finally{clearTimeout(timer);sfRenderCaptureReport();}
 }
 out.textContent='UTC '+new Date(target).toISOString()+' · '+accepted+'/'+ids.length+' HTTP angenommen'+(testPulse?' · mit GPIO-Testimpuls':'')+'. '+results.join(' | ')+'. Die grüne Anzeige kennzeichnet nur den geplanten Zeitpunkt; Capture-Ergebnisse stehen in der Node-Tabelle.';
 if(accepted&&!shown)sfShowCaptureCountdown(target);
 }finally{window.sfCoordinatorBulkActive=false;button.disabled=false;sfRenderCaptureReport();window.sfCoordinateStatus().then(function(d){window.sfCaptureReportUpdate(d);}).catch(function(){});}
});
var sfActionDialog=document.getElementById('clusterActionDialog');
document.getElementById('clusterActionInfo').addEventListener('click',function(){if(sfActionDialog.showModal)sfActionDialog.showModal();else sfActionDialog.setAttribute('open','');});
document.getElementById('clusterActionDialogClose').addEventListener('click',function(){if(sfActionDialog.close)sfActionDialog.close();else sfActionDialog.removeAttribute('open');});
async function sfStartHwTime(){
 sfRenderHwTimeInfo();
 var info=document.getElementById('clusterHardwareTimeInfo');info.style.display='block';
 var el=document.getElementById('clusterHardwareTimeFeedback');el.style.display='block';
 var selected=sfHwSelectedNodes();
 if(selected.length!==3){el.textContent='Genau drei unterschiedliche Nodes auswaehlen.';return;}
 if(selected.some(function(n){return (window.sfHwTimeNodeBoards[String(n.integration_id||'')]||'xiao')!=='xiao';})){
   el.textContent='Der Messimpuls ist bisher nur fuer XIAO GPIO2 qualifiziert. Freenove-Node-Ausgaenge sind gesperrt.';return;
 }
 if((window.sfHwTimeMeta.inputs||[]).some(function(v){return Number(v)<0;})){
   el.textContent=window.sfHwTimeNotReady;return;
 }
 if(!window.confirm('Drei Nodes mit gemeinsamer GND-Leitung verbunden? Alle Geraete im Drone Mode? GPIO14/21/41 am Coordinator exklusiv verfuegbar?'))return;
 var fd=new FormData();selected.forEach(function(n,i){fd.append('node'+(i+1),String(n.integration_id||''));});
 el.textContent='Messauftrag wird gestartet ...';
 try{
  var r=await fetch('/cluster_hw_time_start',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'});
  var msg=await r.text();el.textContent=(r.ok?'Messung geplant: ':'Start nicht moeglich: ')+msg;
 }catch(e){el.textContent='Verbindung zur Messsteuerung fehlgeschlagen.';}
}
function sfPollHwTime(){
 if(document.getElementById('clusterHardwareTimeInfo').style.display==='none')return;
 fetch('/cluster_hw_time_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw Error();return r.json();}).then(function(d){
 var rows=d.samples||[],drift=d.drift_ppm||[],reading=document.getElementById('sfHwTimeReading');
 var latest=rows.length?rows[rows.length-1].ms:[];
 reading.textContent='Status: '+(d.state||'-')+(d.active?' (laeuft)':'')+' · Messungen: '+(d.rows||0)+' · ARM: '+(d.arm||[]).join('/')+' · Versatz Node 1/2/3: '+latest.map(function(v){return v===null?'kein Signal':Number(v).toFixed(3)+' ms';}).join(' / ')+' · Drift 1/2/3: '+drift.map(function(v){return v===null?'—':Number(v).toFixed(1)+' ppm';}).join(' / ');
 var svg=document.getElementById('sfHwTimePlot');while(svg.firstChild)svg.removeChild(svg.firstChild);
 var ns='http://www.w3.org/2000/svg',palette=['#e879f9','#22c55e','#38bdf8'];
 function line(x1,y1,x2,y2,color){var l=document.createElementNS(ns,'line');l.setAttribute('x1',x1);l.setAttribute('y1',y1);l.setAttribute('x2',x2);l.setAttribute('y2',y2);l.setAttribute('stroke',color);svg.appendChild(l);}
 function label(x,y,v,color){var a=document.createElementNS(ns,'text');a.setAttribute('x',x);a.setAttribute('y',y);a.setAttribute('fill',color);a.setAttribute('font-size','12');a.textContent=v;svg.appendChild(a);}
 var vals=[];rows.forEach(function(r){(r.ms||[]).forEach(function(v){if(v!==null&&Number.isFinite(Number(v)))vals.push(Number(v));});});
 var min=vals.length?Math.min.apply(null,vals):-.5,max=vals.length?Math.max.apply(null,vals):.5;
 if(max-min<.01){max+=.5;min-=.5;}var margin=(max-min)*.12;max+=margin;min-=margin;
 line(55,12,55,177,'#94a3b8');line(55,177,625,177,'#94a3b8');label(2,20,max.toFixed(2)+' ms','#94a3b8');label(2,174,min.toFixed(2),'#94a3b8');
 palette.forEach(function(color,ch){var last=null;rows.forEach(function(r,i){var v=(r.ms||[])[ch];if(v===null||v===undefined){last=null;return;}var x=55+(rows.length>1?i/(rows.length-1):.5)*570,y=177-(Number(v)-min)/(max-min)*165;if(last)line(last[0],last[1],x,y,color);last=[x,y];});label(58+ch*145,203,'Node '+(ch+1),color);});
 }).catch(function(){document.getElementById('sfHwTimeReading').textContent='Messstatus nicht erreichbar';});
}
setInterval(sfPollHwTime,2000);
document.getElementById('clusterActionRun').addEventListener('click',function(){var choice=document.getElementById('clusterActionChoice').value;if(!choice)return;if(choice==='clusterHardwareTime'){sfStartHwTime();sfPollHwTime();return;}document.getElementById(choice).click();});
// Estimate uses coordinator UTC reference, not the browser's unsynchronized wall clock.
function sfUpdatePlannedTime(){var h=document.getElementById('clusterCaptureHours').value,m=document.getElementById('clusterCaptureMinutes').value,s=document.getElementById('clusterCaptureSeconds').value,out=document.getElementById('clusterPlannedTime');var t=sfPlanUtcTarget(null,Number(s),Number(m),Number(h),sfCaptureUtcNow());out.textContent=(h===''||m===''||s===''||!Number.isFinite(t))?'UTC-Zeit nicht verfügbar':('Geplant: '+new Date(t).toLocaleString('de-DE',{timeZone:'UTC',hour12:false})+' UTC'+(t-sfCaptureUtcNow()>1800000?' (max. 30 min)':''));}
['clusterCaptureHours','clusterCaptureMinutes','clusterCaptureSeconds'].forEach(function(id){document.getElementById(id).addEventListener('input',sfUpdatePlannedTime);});
setInterval(sfUpdatePlannedTime,1000);sfUpdatePlannedTime();
document.getElementById('clusterBulkDroneOn').addEventListener('click',function(){runBulk('/cluster_node_drone','Drone Mode EIN',false);});
document.getElementById('clusterBulkDroneOff').addEventListener('click',function(){runBulk('/cluster_node_drone','Drone Mode AUS',false);});
document.getElementById('clusterBulkProbe').addEventListener('click',function(){runBulk('/cluster_job_probe','Verbindungstest',false);});
document.getElementById('clusterBulkRestart').addEventListener('click',function(){runBulk('/cluster_node_restart','Neustart',false);});
document.getElementById('clusterBulkShutdown').addEventListener('click',function(){runBulk('/cluster_node_shutdown','Shutdown',true);});
document.getElementById('clusterBulkSync').addEventListener('click',function(){runBulk('/cluster_node_sync_time','Zeitsynchronisation',false);});
document.getElementById('clusterBulkWipe').addEventListener('click',function(){runBulk('/cluster_node_wipe','SD-Wipe',true);});
document.getElementById('clusterSdPreflight').addEventListener('click',function(){
 var out=document.getElementById('clusterSdPreflightResult'),ids=sfActionableSelectedIds();
 if(!ids.length){out.textContent='Bitte mindestens einen entfernten Node auswählen.';return;}
 out.textContent='SD-Status wird geprüft …';
 window.sfCoordinateStatus().then(function(d){
  var ns=Array.isArray(d.nodes)?d.nodes:[];
  out.textContent=ids.map(function(id){
   var n=ns.find(function(x){return String(x.integration_id||'')===id;});
   if(!n)return id+': nicht erreichbar / keine Statusdaten';
   var name=n.hostname||id;
   if(n.recording)return name+': GESPERRT – Aufnahme läuft';
   if(!n.health_valid||Number(n.sd_total_mib||0)<=0)return name+': SD nicht verfügbar / Daten unbekannt – keine Löschung möglich';
   return name+': SD verfügbar ('+fmt(n.sd_used_mib||0)+' belegt, '+fmt(Math.max(0,Number(n.sd_total_mib||0)-Number(n.sd_used_mib||0)))+' frei); keine laufende Aufnahme gemeldet. Vollständiger Wipe über die Auswahl oben möglich.';
  }).join(' | ');
 }).catch(function(e){out.textContent='Statusprüfung fehlgeschlagen: '+String(e.message||e)+'; keine Löschung erfolgt.';});
});
pollHealth();setInterval(pollHealth,10000);
})();
</script>)__HEALTH__";
    html += R"__PROBE__(<details class='settings-section'><summary>Verbindungsdiagnose und Einzeltests</summary>
<p class='muted'>Ein signierter, harmloser Probeauftrag prüft die Zustellung und die individuelle Antwort. Er startet keine Geräte neu und verändert keine Dateien.</p>
<details><summary>ESP-NOW Benchmark: 100 Pakete je Richtung und Node</summary>
<p class='muted'>Aktiver, HMAC-geschützter Transporttest. Alle aktuell über ESP-NOW bekannten Nodes werden nacheinander getestet; reguläre Sync-Messungen pausieren vorübergehend. 100 Pakete bei 50 ms Abstand je Richtung. Der Test misst Paketankunft und ACK-RTT, keine absolute Kamera-Synchronität.</p>
<label>Nodes: <select id='sfBenchTarget'><option value=''>Alle Nodes</option></select></label> <label>Abstand: <select id='sfBenchGap'><option value='50'>50 ms (Referenz)</option><option value='10'>10 ms</option><option value='5'>5 ms (Lasttest)</option></select></label> <button type='button' id='sfBenchStart'>ESP-NOW-Benchmark starten</button> <button type='button' id='sfBenchAbort'>Abbrechen</button> <a href='/cluster_espnow_benchmark_csv'>CSV nach Testende</a> <a href='/cluster_espnow_benchmark_status'>JSON</a>
<p>Vier-Zeitstempel Rohdaten (auf jedem Node): <span id='sfBenchRawLinks'>Links werden geladen</span></p><p>Uplink-ACK-RTT und Downlink-Empfang: <span id='sfBenchTransportLinks'>Links werden geladen</span></p><pre id='sfBenchStatus' class='muted'>Noch kein Benchmark gestartet.</pre>
</details>
<details><summary>ESP-NOW Broadcast-Intervalltest (nur Diagnose)</summary>
<p class='muted'>Sendet 15 Sekunden lang zusätzliche authentifizierte Zeitbeacons. Optional werden individuelle ESP-NOW-Sync-Slots pausiert (bestehende Leases laufen innerhalb weniger Sekunden ab). WLAN-Verbindung und Cluster bleiben aktiv. Kein Einfluss auf die Capture-Zeitquelle. Die CSV-Messwerte anschließend auf den Nodes herunterladen.</p>
<label>Intervall: <select id='sfBeaconGap'><option value='200'>200 ms</option><option value='500'>500 ms</option><option value='100'>100 ms</option><option value='50'>50 ms</option></select></label>
<label><input type='checkbox' id='sfBeaconQuiet'> Individuelle ESP-NOW-Sync-Slots während des Tests pausieren</label> <button type='button' id='sfBeaconStart'>Test starten</button> <button type='button' id='sfBeaconAbort'>Abbrechen</button>
<pre id='sfBeaconStatus' class='muted' role='status'>Bereit.</pre>
</details>
<div id='clusterProbeTargets'>Verfügbare Nodes werden geladen …</div>
<div id='clusterProbeFeedback' class='muted' role='status'></div><div id='clusterProbeSummary' class='muted'></div><details><summary>Technische Übertragungszähler</summary><div id='clusterProbeDiagnostics' class='muted'></div><div id='clusterHttpDiagnostics' class='muted'></div></details>
<details><summary>Letzte Probe-Ergebnisse (begrenzter RAM-Verlauf)</summary>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Node</th><th>Ergebnis</th><th>Code</th></tr></thead><tbody id='clusterProbeResults'><tr><td colspan='3'>Noch keine Probeaufträge.</td></tr></tbody></table></div>
<p class='muted'>„Bestätigt“ bedeutet nur: Der Empfänger hat den Probeauftrag authentifiziert und beantwortet. Es ist keine Bestätigung für Reboot, Löschen oder andere Aktionen.</p></details>
</details><script>
(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
var busy=false;
function sfBeaconPoll(){fetch('/cluster_espnow_beacon_test_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw Error('Status: HTTP '+r.status);return r.json();}).then(function(d){document.getElementById('sfBeaconStatus').textContent=d.active?('Test läuft: '+d.interval_ms+' ms'+(d.quiet?' · Sync-Slots pausiert':'')+' · gesendet '+d.sent+' · verbleibend '+Math.ceil(d.remaining_ms/1000)+' s'):('Test inaktiv · zuletzt gesendet '+d.sent);}).catch(function(e){document.getElementById('sfBeaconStatus').textContent=String(e.message||e);});}
function sfBeaconCommand(path){fetch(path,{method:'POST',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error('HTTP '+r.status+': '+t);});return r.json();}).then(sfBeaconPoll).catch(function(e){document.getElementById('sfBeaconStatus').textContent=String(e.message||e);});}
document.getElementById('sfBeaconStart').addEventListener('click',function(){sfBeaconCommand('/cluster_espnow_beacon_test_start?interval_ms='+encodeURIComponent(document.getElementById('sfBeaconGap').value)+'&quiet='+(document.getElementById('sfBeaconQuiet').checked?'1':'0'));});
document.getElementById('sfBeaconAbort').addEventListener('click',function(){sfBeaconCommand('/cluster_espnow_beacon_test_abort');});
sfBeaconPoll();setInterval(sfBeaconPoll,2000);
function sfBenchCommand(path){fetch(path,{method:'POST',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t);});return r.json();}).then(sfBenchPoll).catch(function(e){document.getElementById('sfBenchStatus').textContent=String(e.message||e);});}
function sfBenchPoll(){fetch('/cluster_espnow_benchmark_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(d){
var o=['Test '+(d.running?'läuft':(d.aborted?'abgebrochen':(d.finished?'beendet':'nicht gestartet'))),'Run '+d.run_id+' · Node '+(d.node_index+1)+' · Phase '+d.phase];
(d.peers||[]).forEach(function(p){o.push(p.boot_id+': C→N '+p.down_ack+'/100 ACK, N→C '+p.up_received+'/100 empfangen; Down-ACK RTT Ø '+(p.down_rtt_mean_us==null?'-':p.down_rtt_mean_us)+' µs');});
document.getElementById('sfBenchStatus').textContent=o.join('\n');
}).catch(function(){});}
document.getElementById('sfBenchStart').addEventListener('click',function(){sfBenchCommand('/cluster_espnow_benchmark_start?interval_ms='+encodeURIComponent(document.getElementById('sfBenchGap').value)+'&boot_id='+encodeURIComponent(document.getElementById('sfBenchTarget').value));});
document.getElementById('sfBenchAbort').addEventListener('click',function(){sfBenchCommand('/cluster_espnow_benchmark_abort');});
window.sfCoordinateStatus().then(function(d){var ns=(d.nodes||[]).filter(function(n){return !n.local&&n.ip;});document.getElementById('sfBenchTarget').innerHTML='<option value="">Alle Nodes</option>'+ns.filter(function(n){return n.boot_id;}).map(function(n){return '<option value="'+esc(n.boot_id)+'">'+esc(n.hostname||n.ip)+'</option>';}).join('');document.getElementById('sfBenchRawLinks').innerHTML=ns.map(function(n){return '<a href="http://'+esc(n.ip)+'/cluster_espnow_benchmark_exchanges">'+esc(n.hostname||n.ip)+'</a>';}).join(' · ');document.getElementById('sfBenchTransportLinks').innerHTML=ns.map(function(n){return '<a href="http://'+esc(n.ip)+'/cluster_espnow_benchmark_node_transport">'+esc(n.hostname||n.ip)+'</a>';}).join(' · ');}).catch(function(){});
sfBenchPoll();setInterval(sfBenchPoll,2000);
var probeStartMs=0;
document.addEventListener('submit',function(ev){
 var form=ev.target;
 if(!form||form.action.indexOf('/cluster_job_probe')===-1)return;
 ev.preventDefault();
 if(busy)return;
 busy=true;probeStartMs=Date.now();
 var msg=document.getElementById('clusterProbeFeedback');
 if(msg)msg.textContent='Probeauftrag wird gesendet … (maximal 15 Sekunden bis zum Job-Ergebnis).';
 var ctrl=new AbortController(),timer=setTimeout(function(){ctrl.abort();},5000);
 fetch('/cluster_job_probe',{method:'POST',body:new FormData(form),credentials:'same-origin',cache:'no-store',redirect:'manual',signal:ctrl.signal})
 .then(function(r){if(r.status===0||r.status===303||r.ok)return;
   return r.text().then(function(t){throw Error('HTTP '+r.status+' '+t.slice(0,120));});})
 .then(function(){if(msg)msg.textContent='Probeauftrag gestartet; Ergebnis folgt im Status (bis 15 s).';})
 .catch(function(e){if(msg)msg.textContent='HTTP-Probeversand fehlgeschlagen oder dauert länger als 5 s: '+e.message+'. Der Auftrag kann dennoch angekommen sein; Ergebnisliste prüfen.';})
 .finally(function(){clearTimeout(timer);busy=false;refresh();});
},true);
function refresh(){window.sfCoordinateStatus().then(function(d){
var targets=document.getElementById('clusterProbeTargets'),results=document.getElementById('clusterProbeResults');
if(!targets||!results)return;
var ns=(Array.isArray(d.nodes)?d.nodes:[]).filter(function(n){return !n.local&&n.integration_id&&n.ip;});
targets.innerHTML=ns.length?ns.map(function(n){return '<form method="POST" action="/cluster_job_probe" style="margin:6px 0"><input type="hidden" name="node_id" value="'+esc(n.integration_id)+'"><button type="submit" '+(busy?'disabled':'')+'>Verbindung testen: '+esc(n.hostname||n.integration_id)+'</button></form>';}).join(''):'Keine entfernten Nodes verfügbar.';
var states=['Leer','Wartet','Gesendet','Angenommen','In Bearbeitung','Bestätigt','Fehlgeschlagen','Zeitüberschreitung','Abgebrochen'];
var diag=d.job_probe_diagnostics||{};
var dx=document.getElementById('clusterProbeDiagnostics');
var hd=document.getElementById('clusterHttpDiagnostics'),hs=window.sfCoordinateHttpStats||{};
if(hd)hd.textContent='HTTP-Status: letzte erfolgreiche Antwort '+(hs.lastMs==null?'–':hs.lastMs+' ms')+' · erfolgreich '+(hs.success||0)+' · Fehler '+(hs.fail||0)+(hs.lastError?' · letzter Fehler '+hs.lastError:'')+' (Browserwerte, kein UDP-RTT; ein einzelner Abruffehler bedeutet nicht Node-Ausfall).';
if(dx)dx.textContent='Lokale Transportdiagnose: Wiederholungen '+(diag.retransmissions||0)+'; Aufträge empfangen '+(diag.received||0)+', ACK gesendet '+(diag.ack_sent||0)+', abgelehnt '+(diag.rejected||0)+' ('+(diag.last_reject||'none')+'); ACK empfangen '+(diag.ack_received||0)+', abgelehnt '+(diag.ack_rejected||0)+' ('+(diag.last_ack_reject||'none')+'). Bei Timeout bitte auch die Cluster-Seite des Zielgeräts prüfen.';
var jobs=(Array.isArray(d.job_probes)?d.job_probes:[]).filter(function(j){return Number(j.kind)===5;});
var summary=document.getElementById('clusterProbeSummary');
if(summary)summary.textContent='Probeaufträge seit Start: '+(diag.jobs_started||0)+' · gültige Probe-ACKs: '+(diag.ack_received||0)+' · Verlaufseinträge: '+jobs.length+' von maximal 16 (ältere können verdrängt werden).';
var feedback=document.getElementById('clusterProbeFeedback');if(feedback&&!busy&&feedback.dataset.statusError==='1'){feedback.textContent='Status wieder erreichbar; letzter Abruffehler war vorübergehend.';feedback.dataset.statusError='0';}
results.innerHTML=jobs.length?jobs.map(function(j){var st=Number(j.state);return '<tr><td>'+esc(j.node)+'</td><td>'+esc(states[st]||'Unbekannt')+(j.ack_latency_ms!=null?' · ACK '+j.ack_latency_ms+' ms':'')+'</td><td>'+esc(j.result||0)+'</td></tr>';}).join(''):'<tr><td colspan="3">Noch keine Probeaufträge.</td></tr>';
}).catch(function(){var e=document.getElementById('clusterProbeFeedback');if(e&&!busy){e.dataset.statusError='1';e.textContent='Einzelne HTTP-Statusabfrage fehlgeschlagen (8-s-Grenze). Letzte bekannte Daten bleiben sichtbar; Cluster-Ausfall nicht nachgewiesen.';}var hd=document.getElementById('clusterHttpDiagnostics'),hs=window.sfCoordinateHttpStats||{};if(hd)hd.textContent='HTTP-Abruffehler '+(hs.fail||0)+' · letzter Fehler '+(hs.lastError||'unbekannt')+'; letzte bekannte Ansicht unverändert.';});}
refresh();setInterval(refresh,3000);
})();
</script>)__PROBE__";
    html += "<details class='settings-section'><summary>Zeitmessungen und Sync-Diagnose</summary>"
        "<p class='muted'>Synchronisation für ausgewählte Nodes ausschließlich über die Aktionen oberhalb der Gerätetabelle starten. Diese Ansicht zeigt nur die authentifizierten Messberichte.</p>"
        "<div id='clusterSyncSummary' class='muted'>Zeitstatus wird geladen …</div>"
        "<div class='cluster-table-wrap'><table class='cluster-table'>"
        "<thead><tr><th>Gerät</th><th>Sync-Status</th><th>Zeit beim Sync</th><th>Abweichung zur Geräteuhr</th><th>Messqualität</th><th>Letzte Messung</th></tr></thead>"
        "<tbody id='clusterSyncRows'><tr><td colspan='6'>Lädt …</td></tr></tbody>"
        "</table></div>"
        "<p class='muted'>Die RTT-basierte Unsicherheit ist keine garantierte Genauigkeit; die Systemuhr wird nicht verstellt.</p></details>";
    html += R"__SYNCJS__(<script>
(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function ms(v){return (Number(v||0)/1000).toFixed(2)+' ms';}
function clock(v){var n=Number(v);return n>1577836800000000?new Date(n/1000).toLocaleString('de-DE'):'–';}
function age(v){var n=Number(v||0);return n<60000?Math.round(n/1000)+' s':Math.round(n/60000)+' min';}
function render(d){
var rows=document.getElementById('clusterSyncRows'),sum=document.getElementById('clusterSyncSummary');
if(!rows||!sum)return;
var seq=Number((d.cluster_time||{}).manual_sync_sequence||0);
var nodes=Array.isArray(d.nodes)?d.nodes:[];
var done=0,expected=0;
rows.innerHTML=nodes.map(function(n){
var local=!!n.local,valid=!!n.time_report_valid,match=valid&&Number(n.time_report_command_seq)===seq;
var selectedSeq=Number(n.selected_sync_seq||0),selectedMatch=valid&&selectedSeq>0&&Number(n.time_report_command_seq)===selectedSeq;
var status=local?'Zeitquelle (Coordinator)':(selectedSeq>0?(selectedMatch?'Gezielter Sync bestätigt (#'+selectedSeq+')':('Gezielter Sync ausstehend (#'+selectedSeq+')')):(!seq?(valid?'Messung vorhanden':'Wartet auf erste Messung'):(match?'Broadcast-Sync bestätigt':'Broadcast-Antwort ausstehend')));
if(!local){expected++;if(seq&&match)done++;}
return '<tr><td><strong>'+esc(n.hostname||n.integration_id||'–')+'</strong></td><td>'+esc(status)+'</td><td>'+esc(local?clock((d.cluster_time||{}).utc_us):(valid?clock(n.time_report_utc_us):'–'))+'</td><td>'+esc(local?'Referenz':(valid?(Number(n.time_report_offset_us)===-86400000000?'Systemzeit des Nodes fehlt (NTP/RTC)':ms(n.time_report_offset_us)):'–'))+'</td><td>'+esc(local?'–':(valid?'RTT '+ms(n.time_report_rtt_us)+' · ±'+ms(n.time_report_uncertainty_us)+' (geschätzt)':'–'))+'</td><td>'+esc(local?'–':(valid?age(n.time_report_age_ms)+' her':'–'))+'</td></tr>';
}).join('')||'<tr><td colspan="6">Keine Geräte aktiv.</td></tr>';
sum.textContent=seq?('Letzter Broadcast-Sync #'+seq+': '+done+' von '+expected+' Nodes zugeordnet. Einzelne gezielte Syncs stehen unabhängig davon in der Tabelle und zählen nicht zu diesem Broadcast.'):('Noch kein Broadcast-Sync seit Start. Ergebnisse gezielter Syncs stehen in der Tabelle.');
}
function poll(){window.sfCoordinateStatus().then(render).catch(function(){var e=document.getElementById('clusterSyncSummary');if(e)e.textContent='Zeitstatus derzeit nicht erreichbar.';});}
poll();setInterval(poll,3000);document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});
})();
</script>)__SYNCJS__";
    html += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleEspNowBenchmarkExchangeCsv(){
    // Avoid a duplicate 100-exchange buffer on memory-constrained ESP32-S3.
    // The radio service and synchronous WebServer handler share the main loop.
    const size_t count=ClusterEspNowRadio::benchmarkExchangeCount();
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Disposition","attachment; filename=espnow_4timestamp.csv");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"text/csv; charset=utf-8","");
    server.sendContent("run_id,sequence,t1_node_us,t2_coordinator_us,t3_coordinator_us,t4_node_us,rtt_us,offset_us\n");
    for(size_t i=0;i<count;++i){
        ClusterEspNowRadio::BenchmarkExchange r;
        if(!ClusterEspNowRadio::benchmarkExchangeAt(i,r))break;
        const int64_t rtt=(r.t4-r.t1)-(r.t3-r.t2);
        const int64_t offset=((r.t2-r.t1)+(r.t3-r.t4))/2;
        char line[210];
        snprintf(line,sizeof(line),"%lu,%u,%lld,%lld,%lld,%lld,%lld,%lld\n",
                 (unsigned long)r.run,(unsigned)r.seq,
                 (long long)r.t1,(long long)r.t2,(long long)r.t3,(long long)r.t4,
                 (long long)rtt,(long long)offset);
        server.sendContent(line);
    }
    server.sendContent("");
}
static void handleEspNowBenchmarkNodeTransportCsv(){
    uint32_t run=0,down=0,up=0;
    ClusterEspNowRadio::benchmarkLocalTransport(run,down,up);
    server.sendHeader("Content-Disposition","attachment; filename=espnow_node_transport.csv");
    server.sendHeader("Cache-Control","no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"text/csv; charset=utf-8","");
    server.sendContent("run_id,sequence,down_received,up_acknowledged,up_ack_rtt_us\n");
    for(size_t seq=0;seq<100;++seq){
        bool downSeen=false,upAck=false;uint32_t upRtt=0;
        if(!ClusterEspNowRadio::benchmarkLocalPacket(seq,downSeen,upAck,upRtt))break;
        server.sendContent(String(run)+","+String(seq)+","+String(downSeen?1:0)+","+
                           String(upAck?1:0)+","+String(upRtt)+"\n");
    }
    server.sendContent("");
}
static void handleEspNowBeaconExperimentStart(){
    if(!clusterLocalIsCoordinator()){server.send(403,"text/plain","Nur Coordinator");return;}
    const String interval=server.arg("interval_ms");
    if(interval!="50" && interval!="100" && interval!="200" && interval!="500"){
        server.send(400,"text/plain","interval_ms: 50, 100, 200 oder 500");return;
    }
    if(!ClusterEspNowRadio::beaconExperimentStart(static_cast<uint32_t>(interval.toInt()),server.arg("quiet")=="1")){
        server.send(409,"text/plain","Radio nicht bereit, Capture-Pause, Benchmark oder Experiment aktiv");return;
    }
    server.send(200,"application/json","{\"ok\":true,\"duration_ms\":15000}");
}
static void handleEspNowBeaconExperimentAbort(){
    if(!clusterLocalIsCoordinator()){server.send(403,"text/plain","Nur Coordinator");return;}
    ClusterEspNowRadio::beaconExperimentAbort();
    server.send(200,"application/json","{\"ok\":true}");
}
static void handleEspNowBeaconExperimentStatus(){
    const auto st=ClusterEspNowRadio::beaconExperimentSnapshot();
    String json="{\"active\":";
    json+=(st.active?"true":"false");
    json+=",\"interval_ms\":"+String(st.intervalMs);
    json+=",\"quiet\":"+String(st.quiet?"true":"false");
    json+=",\"sent\":"+String(st.sent);
    json+=",\"remaining_ms\":"+String(st.remainingMs)+"}";
    server.send(200,"application/json",json);
}
static void handleEspNowBenchmarkStart(){
    if(!clusterLocalIsCoordinator()){
        server.send(403,"text/plain; charset=utf-8","Nur am Coordinator");return;
    }
    const uint32_t interval=static_cast<uint32_t>(server.arg("interval_ms").toInt());
    const String targetText=server.arg("boot_id");
    if(targetText.length()!=0 && targetText.length()!=8){
        server.send(400,"text/plain; charset=utf-8","Ungültige Node-Boot-ID");return;
    }
    for(size_t i=0;i<targetText.length();++i){
        const char c=targetText[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))){
            server.send(400,"text/plain; charset=utf-8","Ungültige Node-Boot-ID");return;
        }
    }
    const uint32_t target=targetText.length()==8 ? static_cast<uint32_t>(strtoul(targetText.c_str(),nullptr,16)) : 0;
    if(!ClusterEspNowRadio::benchmarkStart(interval,target)){
        server.send(409,"text/plain; charset=utf-8","Benchmark nicht startbar: Radio inaktiv, Capture-Pause, Test bereits aktiv oder noch keine ESP-NOW-Node entdeckt.");return;
    }
    server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
}
static void handleEspNowBenchmarkAbort(){
    if(!clusterLocalIsCoordinator()){
        server.send(403,"text/plain; charset=utf-8","Nur am Coordinator");return;
    }
    ClusterEspNowRadio::benchmarkAbort();
    server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
}
static void handleEspNowBenchmarkStatus(){
    const auto b=ClusterEspNowRadio::benchmarkSnapshot();
    String out;out.reserve(1200);
    out+="{\"running\":";out+=(b.running?"true":"false");
    out+=",\"finished\":";out+=(b.finished?"true":"false");
    out+=",\"aborted\":";out+=(b.aborted?"true":"false");
    out+=",\"run_id\":";out+=String(b.runId);
    out+=",\"interval_ms\":";out+=String(b.intervalMs);
    out+=",\"node_index\":";out+=String(b.peerIndex);
    out+=",\"phase\":";out+=String(b.phase);
    out+=",\"peers\":[";
    for(uint8_t i=0;i<b.peerCount;++i){
        if(i)out+=",";
        const auto &n=b.peers[i];
        out+="{\"boot_id\":\""; char id[9];snprintf(id,sizeof(id),"%08lx",(unsigned long)n.boot);out+=id;
        out+="\",\"down_ack\":";out+=String(n.downReceived);
        out+=",\"up_received\":";out+=String(n.upReceived);
        out+=",\"down_rtt_min_us\":";out+=String(n.downRttMinUs);
        out+=",\"down_rtt_max_us\":";out+=String(n.downRttMaxUs);
        out+=",\"down_rtt_mean_us\":";
        out+=(n.downReceived?String((double)n.downRttTotalUs/n.downReceived,1):String("null"));
        out+="}";
    }
    out+="]}";
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8",out);
}
// Read-only, passive SFDB diagnostic data. Not a clock-source selector.
static void handleEspNowDiagnosticBeaconCsv(){
    const auto s=ClusterEspNowRadio::diagnosticBeaconRxStats();
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Disposition","attachment; filename=espnow_diagnostic_beacon.csv");
    String csv="received,authenticated,rejected,replay,dropped,last_sequence,last_epoch,last_sender_tag,last_sender_boot,last_rx_callback_us,last_remote_monotonic_us,last_remote_utc_us\n";
    char line[280];
    snprintf(line,sizeof(line),"%lu,%lu,%lu,%lu,%lu,%lu,%lu,%llu,%llu,%lld,%lld,%lld\n",
        s.shadowActive ? "true" : "false",
        (unsigned long)s.shadowActivations,(unsigned long)s.shadowFallbacks,
        (unsigned long)s.shadowSourceChanges,
        s.candidateFresh ? "true" : "false",
        (unsigned long)s.candidateAgeMs,(unsigned long)s.candidateConsecutive,
        (unsigned long)s.received,(unsigned long)s.authenticated,
        (unsigned long)s.rejected,(unsigned long)s.replay,(unsigned long)s.dropped,
        (unsigned long)s.lastSequence,(unsigned long)s.lastEpoch,
        (unsigned long long)s.lastSenderTag,(unsigned long long)s.lastSenderBoot,
        (long long)s.lastRxCallbackUs,(long long)s.lastRemoteMonotonicUs,
        (long long)s.lastRemoteUtcUs);
    csv+=line;
    server.send(200,"text/csv; charset=utf-8",csv);
}
static void handleEspNowDiagnosticBeaconJson(){
    const auto s=ClusterEspNowRadio::diagnosticBeaconRxStats();
    char data[720];
    snprintf(data,sizeof(data),"{\"shadow_active\":%s,\"shadow_activations\":%lu,\"shadow_fallbacks\":%lu,\"shadow_source_changes\":%lu,\"candidate_fresh\":%s,\"candidate_age_ms\":%lu,\"candidate_consecutive\":%lu,\"handover_enabled\":false,\"received\":%lu,\"authenticated\":%lu,\"rejected\":%lu,\"replay\":%lu,\"dropped\":%lu,\"last_sequence\":%lu,\"last_epoch\":%lu,\"last_sender_tag\":\"%llu\",\"last_sender_boot\":\"%llu\",\"last_rx_callback_us\":%lld,\"last_remote_monotonic_us\":%lld,\"last_remote_utc_us\":%lld}",
        s.shadowActive ? "true" : "false",
        (unsigned long)s.shadowActivations,(unsigned long)s.shadowFallbacks,
        (unsigned long)s.shadowSourceChanges,
        s.candidateFresh ? "true" : "false",
        (unsigned long)s.candidateAgeMs,(unsigned long)s.candidateConsecutive,
        (unsigned long)s.received,(unsigned long)s.authenticated,
        (unsigned long)s.rejected,(unsigned long)s.replay,(unsigned long)s.dropped,
        (unsigned long)s.lastSequence,(unsigned long)s.lastEpoch,
        (unsigned long long)s.lastSenderTag,(unsigned long long)s.lastSenderBoot,
        (long long)s.lastRxCallbackUs,(long long)s.lastRemoteMonotonicUs,
        (long long)s.lastRemoteUtcUs);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8",data);
}
// Small constant-memory summary; do not construct a large page or CSV history.
// Passive A/B packet predictability only, never clock synchronization accuracy.
static void handleEspNowAbPredictionJson(){
    const auto a=ClusterEspNowRadio::beaconPredictionStats();
    const auto b=ClusterEspNowRadio::diagnosticPredictionStats();
    const double aSd=a.valid>1?sqrt(a.errorM2/double(a.valid-1)):0.0;
    const double bSd=b.valid>1?sqrt(b.errorM2/double(b.valid-1)):0.0;
    char hostname[65];size_t used=0;
    for(size_t i=0;i<cfg_hostname.length() && used<sizeof(hostname)-1;++i){
        const char c=cfg_hostname[i];
        hostname[used++]=((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
            (c>='0'&&c<='9')||c=='-'||c=='_')?c:'_';
    }
    hostname[used]=0;
    if(!used)snprintf(hostname,sizeof(hostname),"sensorforge");
    char data[850];
    snprintf(data,sizeof(data),
        "{\"device\":\"%s\",\"measurement\":\"passive_one_step_rx_prediction\","
        "\"unit\":\"us\",\"capture_clock_modified\":false,"
        "\"sync_accuracy_measured\":false,"
        "\"coordinator\":{\"samples\":%lu,\"stddev_us\":%.2f,\"mean_abs_us\":%.2f,\"min_us\":%ld,\"max_us\":%ld},"
        "\"broadcaster\":{\"samples\":%lu,\"warmup\":%lu,\"invalid\":%lu,\"stddev_us\":%.2f,\"mean_abs_us\":%.2f,\"min_us\":%ld,\"max_us\":%ld},"
        "\"free_heap\":%lu}",
        hostname,(unsigned long)a.valid,aSd,a.meanAbsErrorUs,
        (long)a.minErrorUs,(long)a.maxErrorUs,
        (unsigned long)b.valid,(unsigned long)b.warmup,(unsigned long)b.invalid,
        bSd,b.meanAbsErrorUs,(long)b.minErrorUs,(long)b.maxErrorUs,
        (unsigned long)ESP.getFreeHeap());
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8",data);
}
static void handleEspNowDiagnosticIntervalsJson(){
    const auto s=ClusterEspNowRadio::diagnosticBeaconRxStats();
    const double stddev=s.intervalSamples>1 ?
        sqrt(s.intervalM2/static_cast<double>(s.intervalSamples-1)) : 0.0;
    // Sanitize the configurable hostname before writing it into JSON.
    // Fixed stack buffers: no String concatenation or heap allocation.
    char hostname[65];
    const size_t hostnameLen=cfg_hostname.length();
    size_t used=0;
    for(size_t i=0;i<hostnameLen && used<sizeof(hostname)-1;++i){
        const char c=cfg_hostname[i];
        if((c>='a' && c<='z') || (c>='A' && c<='Z') ||
           (c>='0' && c<='9') || c=='-' || c=='_') hostname[used++]=c;
        else hostname[used++]='_';
    }
    hostname[used]='\0';
    if(!used){snprintf(hostname,sizeof(hostname),"sensorforge");}
    char data[440];
    snprintf(data,sizeof(data),
        "{\"device\":\"%s\",\"measurement\":\"diagnostic_broadcaster_rx_interval_error\",\"unit\":\"us\",\"samples\":%lu,\"skipped\":%lu,\"mean_us\":%.2f,\"stddev_us\":%.2f,\"min_us\":%ld,\"max_us\":%ld,\"free_heap\":%lu}",
        hostname,(unsigned long)s.intervalSamples,(unsigned long)s.intervalSkipped,
        s.intervalMeanUs,stddev,(long)s.intervalMinUs,(long)s.intervalMaxUs,
        (unsigned long)ESP.getFreeHeap());
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json; charset=utf-8",data);
}
// Deliberately no UI element; diagnostics use existing authentication gates.
static void handleEspNowBeaconCsv(){
    server.sendHeader("Content-Disposition","attachment; filename=espnow_beacon_observations.csv");
    server.sendHeader("Cache-Control","no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"text/csv; charset=utf-8","");
    server.sendContent("sequence,epoch,coordinator_boot,coordinator_mono_low,coordinator_utc_us,node_rx_callback_us,prediction_samples,prediction_error_us,drift_ppm_x100,prediction_status\n");
    const size_t count=ClusterEspNowRadio::beaconObservationCount();
    for(size_t i=0;i<count;++i){
        ClusterEspNowRadio::BeaconObservation sample;
        if(!ClusterEspNowRadio::beaconObservationAt(i,sample))continue;
        char line[200];
        snprintf(line,sizeof(line),"%lu,%lu,%llu,%lu,%lld,%lld,%u,%ld,%ld,%u\n",
            (unsigned long)sample.sequence,(unsigned long)sample.epoch,
            (unsigned long long)sample.coordinatorBoot,
            (unsigned long)sample.coordinatorMonoLow,
            (long long)sample.coordinatorUtcUs,(long long)sample.nodeRxCallbackUs,
            (unsigned)sample.predictionSamples,(long)sample.predictionErrorUs,(long)sample.driftPpmX100,
            (unsigned)sample.predictionStatus);
        server.sendContent(line);
    }
    server.sendContent("");
}
// O(1) CSV export: no long-lived HTTP connection and no large HTML/String.
// Bounded shared-event trace; streaming CSV avoids constructing a large String.
static void handleEspNowSharedEventsCsv(){
    server.sendHeader("Content-Disposition","attachment; filename=espnow_shared_events.csv");
    server.sendHeader("Cache-Control","no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"text/csv; charset=utf-8","");
    server.sendContent("sequence,epoch,coordinator_boot,coordinator_mono_low,node_rx_callback_us\n");
    const size_t count=ClusterEspNowRadio::beaconSharedEventCount();
    const uint64_t boot=ClusterEspNowRadio::beaconSharedEventBoot();
    for(size_t i=0;i<count;++i){
        ClusterEspNowRadio::BeaconSharedEvent event;
        if(!ClusterEspNowRadio::beaconSharedEventAt(i,event))continue;
        // Trace resets across coordinator restarts; avoid mixing old/new boot IDs.
        if(static_cast<uint32_t>(boot)!=event.coordinatorBootLow)continue;
        char line[112];
        snprintf(line,sizeof(line),"%lu,%lu,%llu,%lu,%llu\n",
            (unsigned long)event.sequence,(unsigned long)event.epoch,
            (unsigned long long)boot,(unsigned long)event.coordinatorMonoLow,
            (unsigned long long)event.nodeRxCallbackUs);
        server.sendContent(line);
    }
    server.sendContent("");
}
static void handleEspNowBeaconStatsCsv(){
    const auto st=ClusterEspNowRadio::beaconPredictionStats();
    server.sendHeader("Cache-Control","no-store");
    server.sendHeader("Content-Disposition","attachment; filename=espnow_beacon_statistics.csv");
    String csv="received,valid,first_sequence,last_sequence,epoch,coordinator_boot,mean_error_us,stddev_error_us,rmse_error_us,mean_abs_error_us,min_error_us,max_error_us,mean_drift_ppm,status_0,status_1,status_2,status_3,status_4,status_5,status_6,status_7,status_8\n";
    char line[320];
    const double variance=st.valid>1 ? st.errorM2/static_cast<double>(st.valid-1) : 0.0;
    const double rmse=st.valid ? sqrt((st.errorM2+st.valid*st.meanErrorUs*st.meanErrorUs)/st.valid) : 0.0;
    snprintf(line,sizeof(line),"%lu,%lu,%lu,%lu,%lu,%llu,%.2f,%.2f,%.2f,%.2f,%ld,%ld,%.4f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        (unsigned long)st.received,(unsigned long)st.valid,
        (unsigned long)st.firstSequence,(unsigned long)st.lastSequence,
        (unsigned long)st.epoch,(unsigned long long)st.coordinatorBoot,
        st.meanErrorUs,sqrt(variance),rmse,st.meanAbsErrorUs,
        (long)st.minErrorUs,(long)st.maxErrorUs,st.meanDriftPpm,
        (unsigned long)st.statusCounts[0],(unsigned long)st.statusCounts[1],
        (unsigned long)st.statusCounts[2],(unsigned long)st.statusCounts[3],
        (unsigned long)st.statusCounts[4],(unsigned long)st.statusCounts[5],
        (unsigned long)st.statusCounts[6],(unsigned long)st.statusCounts[7],
        (unsigned long)st.statusCounts[8]);
    csv+=line;
    server.send(200,"text/csv; charset=utf-8",csv);
}
static void handleEspNowBenchmarkCsv(){
    const auto b=ClusterEspNowRadio::benchmarkSnapshot();
    if(!b.finished){server.send(409,"text/plain; charset=utf-8","Benchmark noch nicht beendet");return;}
    server.sendHeader("Content-Disposition","attachment; filename=espnow_benchmark.csv");
    server.sendHeader("Cache-Control","no-store");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200,"text/csv; charset=utf-8","");
    server.sendContent("run_id,boot_id,direction,sequence,received,ack_rtt_us\n");
    for(uint8_t i=0;i<b.peerCount;++i){
        const auto &n=b.peers[i];
        char id[9];snprintf(id,sizeof(id),"%08lx",(unsigned long)n.boot);
        for(unsigned seq=0;seq<100;++seq){
            String row=String(b.runId)+","+id+",coordinator_to_node,"+String(seq)+","+String(n.downSeen[seq])+","+String(n.downRttUs[seq])+"\n";
            server.sendContent(row);
            row=String(b.runId)+","+id+",node_to_coordinator,"+String(seq)+","+String(n.upSeen[seq])+",0"+"\n";
            server.sendContent(row);
        }
    }
    server.sendContent("");
}

static void handleClusterJobProbe()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    const String nodeId = server.arg("node_id");
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterSendJobProbe(nodeId, error)) {
        server.send(409, "text/plain; charset=utf-8", error);
        return;
    }
    server.sendHeader("Location", "/cluster_coordinate");
    server.send(303, "text/plain; charset=utf-8", "");
}

static void handleClusterNodeShutdown()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403,"text/plain; charset=utf-8","Nur am aktiven Coordinator."); return;
    }
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterRequestNodeShutdown(server.arg("node_id"),error)) {
        server.send(409,"text/plain; charset=utf-8",error); return;
    }
    server.sendHeader("Location","/cluster_coordinate");
    server.send(303,"text/plain; charset=utf-8","");
}

static void handleClusterLocalDrone(){
    // Local explicit operator setting; cluster participation not required.
    if(!droneModeSet(server.hasArg("enabled"))){
        server.send(500,"text/plain; charset=utf-8","NVS-Speicherung fehlgeschlagen");return;
    }
    server.sendHeader("Location","/cluster_coordinate");
    server.send(303,"text/plain","");
}
static void handleClusterNodeDrone(){
    if(!clusterLocalIsCoordinator()){
        server.send(403,"text/plain; charset=utf-8","Nur am Coordinator");return;
    }
    const String enabled=server.arg("enabled");
    if(enabled!="0"&&enabled!="1"){
        server.send(400,"text/plain; charset=utf-8","Ungültiger Drone-Wert");return;
    }
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if(!clusterRequestNodeDrone(server.arg("node_id"),enabled=="1",error)){
        server.send(409,"text/plain; charset=utf-8",error);return;
    }
    server.sendHeader("Location","/cluster_coordinate");
    server.send(303,"text/plain","");
}
static void handleClusterNodeCapture(){
    if(!clusterLocalIsCoordinator()) {server.send(403,"text/plain","Nur am Coordinator");return;}
    String raw=server.arg("utc_ms");char *end=nullptr;errno=0;
    long long ms=strtoll(raw.c_str(),&end,10);
    if(errno||!end||*end||ms<1600000000000LL||ms>4102444800000LL){server.send(400,"text/plain","Ungültige UTC-Zeit");return;}
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    const bool testPulse=server.arg("test_pulse")=="1";  // optional, default off
    if(!clusterRequestNodeCapture(server.arg("node_id"),(int64_t)ms*1000LL,error,testPulse)){
        server.send(409,"text/plain; charset=utf-8",error);return;
    }
    server.sendHeader("Location","/cluster_coordinate");server.send(303,"text/plain","");
}

static void handleClusterNodeWipe()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403,"text/plain; charset=utf-8","Nur am aktiven Coordinator.");return;
    }
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if(!clusterRequestNodeWipe(server.arg("node_id"),error)) {
        server.send(409,"text/plain; charset=utf-8",error);return;
    }
    server.sendHeader("Location","/cluster_coordinate");
    server.send(303,"text/plain; charset=utf-8","");
}

static void handleClusterNodeRestart()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator."); return;
    }
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterRequestNodeRestart(server.arg("node_id"), error)) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.sendHeader("Location", "/cluster_coordinate");
    server.send(303, "text/plain; charset=utf-8", "");
}

static void handleClusterNodeSyncTime()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar."); return;
    }
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterRequestNodeTimeSync(server.arg("node_id"), error)) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.sendHeader("Location", "/cluster_coordinate");
    server.send(303, "text/plain; charset=utf-8", "");
}

static void handleClusterSyncTime()
{
    String error;
    ClusterEspNowRadio::pauseForCapture(10000UL);
    if (!clusterRequestTimeSync(error)) {
        server.send(409, "text/plain; charset=utf-8", error);
        return;
    }
    server.sendHeader("Location", "/cluster_coordinate");
    server.send(303, "text/plain; charset=utf-8", "");
}

static void handleClusterSave()
{
    const int enabled =
        server.hasArg("cluster_enabled") && server.arg("cluster_enabled") == "1"
        ? 1
        : 0;

    String clusterId = server.arg("cluster_id");
    clusterId.trim();
    clusterId.toLowerCase();
    bool generatedNewId = false;
    if (!clusterId.length()) {
        clusterId = clusterGenerateId();
        generatedNewId = true;
    }

    if (!clusterIdValid(clusterId)) {
        server.send(400, "text/plain; charset=utf-8", "Ungültige Cluster-ID");
        return;
    }

    String clusterName = server.arg("cluster_name");
    clusterName.trim();

    uint32_t requestedEpoch = 1;
    if (server.hasArg("cluster_credential_epoch")) {
        const String epochText = server.arg("cluster_credential_epoch");
        errno = 0;
        char *end = nullptr;
        unsigned long parsed = strtoul(epochText.c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0' && parsed >= 1)
            requestedEpoch = (uint32_t)parsed;
    }

    const String currentClusterId = clusterEffectiveId();
    const bool sameClusterId =
        currentClusterId.length() && clusterId == currentClusterId;

    String password = server.arg("cluster_password");
    const bool passwordWasProvided = password.length() != 0;

    if (password.indexOf('\r') >= 0 || password.indexOf('\n') >= 0) {
        server.send(400, "text/plain; charset=utf-8", "Cluster-Passwort enthält ungültige Steuerzeichen");
        return;
    }

    if (!passwordWasProvided) {
        if (sameClusterId &&
            cfg_cluster_password.length() &&
            requestedEpoch == cfg_cluster_credential_epoch) {
            password = cfg_cluster_password;
        } else {
            bool staleEpoch = false;
            String profileError;
            if (!clusterProfilesGetPassword(
                    clusterId,
                    requestedEpoch,
                    password,
                    staleEpoch,
                    profileError
                )) {
                if (staleEpoch) {
                    server.send(
                        400,
                        "text/plain; charset=utf-8",
                        "Das gespeicherte Cluster-Passwort gehört zu einer anderen Credential-Epoch. Bitte Passwort erneut eingeben."
                    );
                    return;
                }
                if (enabled) {
                    server.send(
                        400,
                        "text/plain; charset=utf-8",
                        profileError.length()
                            ? profileError
                            : String("Für diesen Cluster ist kein passendes Passwort gespeichert. Bitte Passwort eingeben.")
                    );
                    return;
                }
            }
        }
    }

    uint32_t credentialEpoch = requestedEpoch;

    // Joining an already visible cluster must prove the candidate password
    // before anything is persisted. The passive scanner keeps a short-lived
    // copy of an existing HMAC-signed SFC1 Presence packet and verifies it
    // locally with the candidate credentials; the password never leaves this
    // device. Existing active membership with unchanged credentials is already
    // authenticated by the running cluster and does not need this join check.
    const bool unchangedActiveMembership =
        enabled && cfg_cluster_enabled && sameClusterId &&
        password == cfg_cluster_password &&
        requestedEpoch == cfg_cluster_credential_epoch &&
        clusterRuntimeActive();

    bool credentialsVerifiedAgainstVisibleCluster = false;
    uint32_t verifiedRemoteEpoch = 0;
    if (enabled && !unchangedActiveMembership) {
        String credentialError;
        const ClusterCredentialCheckResult credentialCheck =
            clusterVerifyDiscoveredCredentials(
                clusterId,
                requestedEpoch,
                password,
                credentialError,
                verifiedRemoteEpoch
            );

        credentialsVerifiedAgainstVisibleCluster =
            credentialCheck == CLUSTER_CREDENTIAL_VERIFIED;
        if (credentialsVerifiedAgainstVisibleCluster)
            credentialEpoch = verifiedRemoteEpoch;

        if (credentialCheck != CLUSTER_CREDENTIAL_NOT_DISCOVERED &&
            credentialCheck != CLUSTER_CREDENTIAL_VERIFIED) {
            int statusCode = 409;
            if (credentialCheck == CLUSTER_CREDENTIAL_INVALID_PASSWORD)
                statusCode = 403;
            else if (credentialCheck == CLUSTER_CREDENTIAL_INTERNAL_ERROR)
                statusCode = 500;

            server.send(
                statusCode,
                "text/plain; charset=utf-8",
                credentialError.length()
                    ? credentialError
                    : String("Cluster-Zugangsdaten konnten nicht bestätigt werden")
            );
            return;
        }
    }

    if (sameClusterId && passwordWasProvided && password != cfg_cluster_password) {
        // Never split a healthy multi-node cluster through a local form edit.
        // Central password rotation is a future Coordinator transaction.
        if (clusterRuntimeActive() && clusterAuthenticatedPeerCount() > 0) {
            server.send(
                409,
                "text/plain; charset=utf-8",
                "Passwortwechsel eines aktiven Mehrgeräte-Clusters muss zentral über den Coordinator erfolgen. Lokale Änderung wurde abgelehnt."
            );
            return;
        }

        // If this is a recovery/rejoin to a currently visible cluster and the
        // candidate password was proven against that cluster, keep the epoch it
        // advertises. Only a truly local/offline password replacement advances
        // the generation; such local rotation is not propagated to peers.
        if (!credentialsVerifiedAgainstVisibleCluster) {
            if (credentialEpoch <= cfg_cluster_credential_epoch)
                credentialEpoch = cfg_cluster_credential_epoch + 1U;
            if (credentialEpoch == 0)
                credentialEpoch = 1;
        }
    }

    String coordinatorPolicy = server.hasArg("cluster_coordinator_policy")
        ? server.arg("cluster_coordinator_policy")
        : cfg_cluster_coordinator_policy;
    coordinatorPolicy.trim();
    coordinatorPolicy.toLowerCase();

    configRefreshSdStatus();
    const bool writeToSd = configSdAvailable() && configSdPresent();

    const bool legacyCurrentWithoutPersistedId =
        cfg_cluster_enabled && !cfg_cluster_id.length() && sameClusterId;

    // Cluster membership/policy exactly as already persisted: only the mount
    // fields can have changed, which apply without a reboot.
    const bool clusterSettingsUnchanged =
        !generatedNewId && !legacyCurrentWithoutPersistedId &&
        enabled == cfg_cluster_enabled &&
        clusterId == cfg_cluster_id &&
        clusterName == cfg_cluster_name &&
        credentialEpoch == cfg_cluster_credential_epoch &&
        password == cfg_cluster_password &&
        coordinatorPolicy == cfg_cluster_coordinator_policy;

    // Optional mount metadata from the same form. Saved first (strictly
    // validated); configSaveClusterSettings() below re-reads config.txt and
    // therefore keeps these keys.
    bool mountChanged = false;
    String mountError;
    if (!webconfigClusterMountSaveFromRequest(server, writeToSd, mountChanged, mountError)) {
        server.send(400, "text/plain; charset=utf-8", "Montagedaten: " + mountError);
        return;
    }

    if (clusterSettingsUnchanged && mountChanged) {
        server.sendHeader("Location", "/cluster?notice=mount_saved");
        server.send(303, "text/plain; charset=utf-8", "");
        return;
    }

    String error;
    ConfigSaveResult result = configSaveClusterSettings(
        enabled,
        clusterId,
        clusterName,
        credentialEpoch,
        password,
        coordinatorPolicy,
        writeToSd,
        error
    );

    if (result == CONFIG_SAVE_INTERNAL_FAILED || result == CONFIG_SAVE_SD_FAILED) {
        String message =
            error.length() ? error : String("Cluster-Einstellungen konnten nicht gespeichert werden");
        if (mountChanged)
            message += " (Montagedaten wurden gespeichert.)";
        server.send(400, "text/plain; charset=utf-8", message);
        return;
    }

    // A freshly created cluster has no remote peer that could authenticate it
    // yet. Remember its credentials immediately. Legacy Beta-37 membership is
    // treated the same way when its derived ID is persisted for the first time.
    if (enabled && password.length() &&
        (generatedNewId || legacyCurrentWithoutPersistedId)) {
        String rememberError;
        if (!clusterProfilesRemember(
                clusterId,
                clusterName,
                password,
                credentialEpoch,
                rememberError
            ) && rememberError.length()) {
            logWrite("Cluster known-profile cache warning: " + rememberError);
        }
    }

    logWrite(
        "Cluster settings saved | enabled=" + String(cfg_cluster_enabled) +
        " | id=" + cfg_cluster_id +
        " | name=" + cfg_cluster_name +
        " | credential_epoch=" + String((unsigned long)cfg_cluster_credential_epoch) +
        " | password=" + String(cfg_cluster_password.length() ? "set" : "empty") +
        " | coordinator_policy=" + cfg_cluster_coordinator_policy +
        " | restart_required=" + String(clusterRestartRequired() ? "yes" : "no")
    );

    // Cluster runtime is owned by the WiFi/network lifecycle. Apply a saved
    // membership/policy change only after a clean reboot so the current radio
    // session can never run with old runtime credentials and new persisted
    // settings mixed together. The config commit above has already completed.
    if (g_uiHooks.scheduleReboot) {
        g_uiHooks.scheduleReboot(4000UL);
        server.sendHeader("Location", "/rebooting?reason=cluster_settings");
    } else {
        server.sendHeader("Location", "/cluster?notice=saved");
    }
    server.send(303, "text/plain; charset=utf-8", "");
}

} // namespace

void webconfigClusterRegisterRoutes(
    WebServer &serverInstance,
    const WebConfigClusterUiHooks &uiHooks
)
{
    g_server = &serverInstance;
    g_uiHooks = uiHooks;

    serverInstance.on("/cluster", HTTP_GET, handleClusterPage);
    serverInstance.on("/cluster_status", HTTP_GET, handleClusterStatus);
    serverInstance.on("/load_diag", HTTP_GET, handleLoadDiag);
    serverInstance.on("/cluster_hw_time_start", HTTP_POST, handleClusterHwTimeStart);
    serverInstance.on("/cluster_hw_time_status", HTTP_GET, handleClusterHwTimeStatus);
    serverInstance.on("/cluster_hw_time_csv", HTTP_GET, handleClusterHwTimeCsv);
    serverInstance.on("/cluster_hw_meter_start", HTTP_POST, handleClusterHwMeterStart);
    serverInstance.on("/cluster_hw_meter_stop", HTTP_POST, handleClusterHwMeterStop);
    serverInstance.on("/cluster_hw_meter_status", HTTP_GET, handleClusterHwMeterStatus);
    serverInstance.on("/cluster_hw_meter_csv", HTTP_GET, handleClusterHwMeterCsv);
    serverInstance.on("/cluster_save", HTTP_POST, handleClusterSave);
    serverInstance.on("/cluster_coordinate", HTTP_GET, handleClusterCoordinatePage);
    serverInstance.on("/cluster_sync_time", HTTP_POST, handleClusterSyncTime);
    serverInstance.on("/cluster_node_sync_time", HTTP_POST, handleClusterNodeSyncTime);
    serverInstance.on("/cluster_espnow_benchmark_exchanges", HTTP_GET, handleEspNowBenchmarkExchangeCsv);
    serverInstance.on("/cluster_espnow_benchmark_node_transport", HTTP_GET, handleEspNowBenchmarkNodeTransportCsv);
    serverInstance.on("/cluster_espnow_beacon_test_start", HTTP_POST, handleEspNowBeaconExperimentStart);
    serverInstance.on("/cluster_espnow_beacon_test_abort", HTTP_POST, handleEspNowBeaconExperimentAbort);
    serverInstance.on("/cluster_espnow_beacon_test_status", HTTP_GET, handleEspNowBeaconExperimentStatus);
    serverInstance.on("/cluster_espnow_benchmark_start", HTTP_POST, handleEspNowBenchmarkStart);
    serverInstance.on("/cluster_espnow_benchmark_abort", HTTP_POST, handleEspNowBenchmarkAbort);
    serverInstance.on("/cluster_espnow_benchmark_status", HTTP_GET, handleEspNowBenchmarkStatus);
    serverInstance.on("/cluster_espnow_benchmark_csv", HTTP_GET, handleEspNowBenchmarkCsv);
    serverInstance.on("/cluster_espnow_diagnostic_beacon_csv", HTTP_GET, handleEspNowDiagnosticBeaconCsv);
    serverInstance.on("/cluster_espnow_diagnostic_beacon_json", HTTP_GET, handleEspNowDiagnosticBeaconJson);
    serverInstance.on("/cluster_espnow_diagnostic_intervals_json", HTTP_GET, handleEspNowDiagnosticIntervalsJson);
    serverInstance.on("/cluster_espnow_ab_prediction_json", HTTP_GET, handleEspNowAbPredictionJson);
    serverInstance.on("/cluster_espnow_beacons_csv", HTTP_GET, handleEspNowBeaconCsv);
    serverInstance.on("/cluster_espnow_beacon_stats_csv", HTTP_GET, handleEspNowBeaconStatsCsv);
    serverInstance.on("/cluster_espnow_shared_events_csv", HTTP_GET, handleEspNowSharedEventsCsv);
    serverInstance.on("/cluster_job_probe", HTTP_POST, handleClusterJobProbe);
    serverInstance.on("/cluster_node_restart", HTTP_POST, handleClusterNodeRestart);
    serverInstance.on("/cluster_node_shutdown", HTTP_POST, handleClusterNodeShutdown);
    serverInstance.on("/cluster_node_wipe", HTTP_POST, handleClusterNodeWipe);
    serverInstance.on("/cluster_node_capture", HTTP_POST, handleClusterNodeCapture);
    serverInstance.on("/cluster_node_drone", HTTP_POST, handleClusterNodeDrone);
    serverInstance.on("/cluster_local_drone", HTTP_POST, handleClusterLocalDrone);
    webconfigClusterGroupsRegisterRoutes(serverInstance);
    webconfigClusterCameraRegisterRoutes(serverInstance);
}
