#include "webconfig_cluster_firmware.h"

#include "access_control.h"
#include "cluster.h"
#include "cluster_firmware_rollout.h"
#include "firmware_security.h"
#include "sensorforge_version.h"

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

void handleStatus()
{
    if (!requireAdmin())
        return;
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "application/json; charset=utf-8", clusterFirmwareRolloutJson());
}

void handleAction()
{
    if (!requireAdmin())
        return;
    String json;
    const int code = clusterFirmwareRolloutRequestFromArgs(*g_server, json);
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(code, "application/json; charset=utf-8", json);
}

void handlePage()
{
    if (!requireAdmin())
        return;
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    String html = g_header();
    html += "<div class='page-title'><div><h2>Firmware im Cluster</h2><p>Signierte Firmware von einem Webserver im "
            "LAN laden – ein Gerät nach dem anderen</p></div></div>";
    html += "<section class='settings-section'>"
            "<p class='muted'>Coordinator: " SENSORFORGE_RELEASE_TAG " · Board " + String(firmwareSecurityLocalBoardId()) +
            ". Jedes Gerät lädt aus dem Manifest die höchste Version für sein Board, prüft Signatur, Board und "
            "Version und schaltet erst dann um. Nach dem Neustart bestätigt es die neue Firmware nach 60 s, sonst "
            "fällt es auf die alte zurück. Der Rollout hält beim ersten Fehler an.</p>"
            "<p><label for='fwUrl'><strong>Manifest-URL</strong></label><br>"
            "<input id='fwUrl' type='text' style='width:100%;max-width:520px' placeholder='http://192.168.0.10:8765/manifest.json'></p>"
            "<p><label><input type='checkbox' id='fwOlder'> Ältere Version erlauben (Downgrade)</label></p>"
            "<p><button type='button' id='fwCheck'>Manifest prüfen</button> "
            "<button type='button' class='primary' id='fwStart'>Nodes aktualisieren (nacheinander)</button> "
            "<button type='button' id='fwStop'>Anhalten</button> "
            "<button type='button' id='fwLocal'>Coordinator aktualisieren</button> "
            "<button type='button' id='fwClear'>Liste leeren</button></p>"
            "<p id='fwMsg' class='muted' role='status'></p><div id='fwManifest'></div>"
            "<div style='overflow-x:auto'><table class='data-table'><thead><tr><th>Gerät</th><th>Version</th>"
            "<th>Status</th><th>Hinweis</th></tr></thead><tbody id='fwRows'></tbody></table></div>"
            "<p class='muted'>Manifest und Pakete erzeugt <code>tools/sf_firmware_publish.py</code> auf dem PC "
            "(signieren + Webserver). Während eines Updates nimmt das Gerät keine Aufnahmen oder Cluster-Aufnahmen "
            "an; Voraussetzung ist Drone Mode. Den Coordinator zuletzt aktualisieren.</p></section>";
    html += R"__FW__(<script>(function(){
var url=document.getElementById('fwUrl'),older=document.getElementById('fwOlder'),msg=document.getElementById('fwMsg'),rows=document.getElementById('fwRows'),man=document.getElementById('fwManifest'),timer=null,urlSet=false;
var ST={pending:'wartet',sent:'Auftrag gesendet',updating:'lädt und prüft …',restarting:'startet neu …',done:'✓ aktualisiert',skipped:'keine neuere Version',failed:'✗ Fehler',stopped:'angehalten'};
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function post(body){var fd=new URLSearchParams(body);return fetch('/cluster_firmware_action',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(function(r){return r.json().catch(function(){return {ok:false,message:'HTTP '+r.status};});});}
function render(d){if(!urlSet&&d.url){url.value=d.url;urlSet=true;}
 var lr=d.local_result?' · Coordinator: '+d.local_result.code_name+(d.local_result.text?' – '+d.local_result.text:''):'';
 if(d.message||lr)msg.textContent=(d.message||'')+lr;
 rows.innerHTML=(d.devices||[]).map(function(n){var st=n.state?(ST[n.state]||n.state):(n.local?'Coordinator':'');
  var col=n.state==='done'?'#16a34a':n.state==='failed'?'#dc2626':(n.state==='updating'||n.state==='restarting'||n.state==='sent')?'#b45309':'';
  return '<tr><td><strong>'+esc(n.hostname)+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+(n.online?'':' <span class="muted">offline</span>')+'</td><td>'+esc(n.release)+(n.version&&n.version!==n.release?' → '+esc(n.version):'')+'</td><td style="color:'+(col||'inherit')+'">'+esc(st)+(n.state_age_s!=null&&n.state!=='done'&&n.state!=='pending'?' ('+n.state_age_s+' s)':'')+'</td><td class="muted">'+esc(n.text||'')+'</td></tr>';}).join('');
 clearTimeout(timer);timer=setTimeout(load,(d.running||d.local_busy)?2000:10000);}
function load(){fetch('/cluster_firmware_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(render).catch(function(e){msg.textContent='Status nicht abrufbar: '+(e.message||e);clearTimeout(timer);timer=setTimeout(load,5000);});}
function act(a,confirmText){if(confirmText&&!confirm(confirmText))return;msg.textContent='…';post({action:a,url:url.value.trim(),allow_older:older.checked?'1':'0'}).then(function(j){msg.textContent=(j.ok?'✓ ':'✗ ')+j.message;load();});}
document.getElementById('fwCheck').addEventListener('click',function(){msg.textContent='Manifest wird geladen …';man.innerHTML='';
 post({action:'check',url:url.value.trim()}).then(function(j){msg.textContent=(j.ok?'✓ ':'✗ ')+j.message;
  if(j.releases&&j.releases.length)man.innerHTML='<p><strong>Im Manifest:</strong> '+j.releases.map(function(r){return esc(r.board)+' '+esc(r.version)+' ('+Math.round(r.size/1024)+' KB)';}).join(' · ')+'</p>';});});
document.getElementById('fwStart').addEventListener('click',function(){act('start','Alle Nodes nacheinander aktualisieren? Jeder Node startet dabei neu.'+(older.checked?' Ältere Versionen sind erlaubt!':''));});
document.getElementById('fwStop').addEventListener('click',function(){act('stop');});
document.getElementById('fwClear').addEventListener('click',function(){act('clear');});
document.getElementById('fwLocal').addEventListener('click',function(){act('local','Den Coordinator jetzt aktualisieren? Er startet danach neu, der Cluster ist kurz ohne Coordinator.');});
load();})();</script>)__FW__";
    html += g_footer();
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "text/html; charset=utf-8", html);
}

} // namespace

void webconfigClusterFirmwareRegisterRoutes(WebServer &server, String (*htmlHeader)(), String (*htmlFooter)())
{
    g_server = &server;
    g_header = htmlHeader;
    g_footer = htmlFooter;
    server.on("/cluster_firmware", HTTP_GET, handlePage);
    server.on("/cluster_firmware_status", HTTP_GET, handleStatus);
    server.on("/cluster_firmware_action", HTTP_POST, handleAction);
}
