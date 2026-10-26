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
            "fällt es auf die alte zurück. Kommt ein Gerät nicht zurück, wird es rot markiert und der Rollout macht mit dem nächsten weiter – das markierte Gerät kurz stromlos machen.</p>"
            "<p><label for='fwUrl'><strong>Manifest-URL</strong></label><br>"
            "<input id='fwUrl' type='text' style='width:100%;max-width:520px' placeholder='http://192.168.0.10:8765/manifest.json'></p>"
            "<p><label><input type='checkbox' id='fwOlder'> Ältere Version erlauben (Downgrade)</label></p>"
            "<p><button type='button' id='fwCheck'>Manifest prüfen &amp; URL speichern</button> "
            "<button type='button' class='primary' id='fwStart'>Nodes aktualisieren (nacheinander)</button> "
            "<button type='button' id='fwStop'>Anhalten</button> "
            "<button type='button' id='fwLocal'>Coordinator aktualisieren</button> "
            "<button type='button' id='fwClear'>Liste leeren</button></p>"
            "<p id='fwMsg' class='muted' role='status'></p><div id='fwManifest'></div>"
            "<div id='fwProg' class='fw-prog' hidden><div class='fw-prog-text' id='fwProgText'></div>"
            "<div class='fw-bar'><span id='fwProgBar'></span></div></div>"
            "<div id='fwLocalBox' class='fw-local' hidden><div class='fw-prog-text' id='fwLocalText'></div>"
            "<div class='fw-bar fw-indet'><span></span></div></div>"
            "<div style='overflow-x:auto'><table class='data-table'><thead><tr><th>Gerät</th><th>Version</th>"
            "<th>Status</th><th>Hinweis</th></tr></thead><tbody id='fwRows'></tbody></table></div>"
            "<p class='muted'>Manifest und Pakete erzeugt <code>tools/sf_firmware_publish.py</code> auf dem PC "
            "(signieren + Webserver). Während eines Updates nimmt das Gerät keine Aufnahmen oder Cluster-Aufnahmen "
            "an; Voraussetzung ist Drone Mode. Den Coordinator zuletzt aktualisieren.</p></section>";
    html += R"__FW__(<style>
.fw-prog,.fw-local{margin:10px 0;padding:12px 14px;border-radius:10px;border:1px solid #64748b55;background:#f8fafc}
.fw-local{border-color:#2563eb88;background:#eff6ff}
.fw-prog-text{font-weight:700;margin-bottom:6px}
.fw-bar{height:12px;border-radius:999px;background:#e2e8f0;overflow:hidden;position:relative}
.fw-bar span{display:block;height:100%;width:0;background:#16a34a;transition:width .4s ease}
.fw-indet span{width:30%;background:#2563eb;position:absolute;animation:fwSlide 1.4s ease-in-out infinite}
@keyframes fwSlide{0%{left:-30%}100%{left:100%}}
.fw-badge{display:inline-block;padding:3px 9px;border-radius:999px;font-weight:700;font-size:.85rem;white-space:nowrap}
.fw-active{background:#f59e0b;color:#1f1300;animation:fwPulse 1s ease-in-out infinite}
.fw-ok{background:#dcfce7;color:#166534}.fw-fail{background:#dc2626;color:#fff}.fw-idle{background:#e2e8f0;color:#334155}
@keyframes fwPulse{0%,100%{opacity:1}50%{opacity:.45}}
.fw-steps{display:flex;gap:4px;margin-top:4px}.fw-steps i{width:18px;height:5px;border-radius:3px;background:#cbd5e1}
.fw-steps i.on{background:#16a34a}.fw-steps i.cur{background:#f59e0b;animation:fwPulse 1s infinite}
tr.fw-row-active td{background:#fff7e6}tr.fw-row-fail td{background:#fee2e2}
</style><script>(function(){
var url=document.getElementById('fwUrl'),older=document.getElementById('fwOlder'),msg=document.getElementById('fwMsg'),rows=document.getElementById('fwRows'),man=document.getElementById('fwManifest'),timer=null,urlSet=false;
var prog=document.getElementById('fwProg'),progText=document.getElementById('fwProgText'),progBar=document.getElementById('fwProgBar');
var loc=document.getElementById('fwLocalBox'),locText=document.getElementById('fwLocalText');
// state -> [label, css, step 0..4]
var ST={pending:['wartet','fw-idle',0],sent:['Auftrag gesendet','fw-active',1],updating:['lädt und prüft …','fw-active',2],restarting:['startet neu …','fw-active',3],done:['✓ aktualisiert','fw-ok',4],skipped:['aktuell – übersprungen','fw-idle',4],failed:['✗ kurz stromlos machen','fw-fail',4],stopped:['angehalten','fw-idle',0]};
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
// The page's CSRF token is valid until the coordinator restarts; after a
// restart (e.g. its own update) the page must be loaded again.
function reloadSoon(text){say(text,true);setTimeout(function(){location.reload();},1500);}
function post(body){var fd=new URLSearchParams(body);return fetch('/cluster_firmware_action',{method:'POST',body:fd,credentials:'same-origin',cache:'no-store'}).then(function(r){
 if(r.status===403)return r.text().then(function(t){if(/CSRF/i.test(t)){reloadSoon('Der Coordinator wurde neu gestartet – die Seite wird neu geladen, dann bitte erneut klicken.');return {ok:false,message:'Seite wird neu geladen'};}return {ok:false,message:'HTTP 403 '+t};});
 return r.json().catch(function(){return {ok:false,message:'HTTP '+r.status};});});}
function say(text,bad){msg.textContent=text;msg.style.color=bad?'#b91c1c':'';msg.style.fontWeight=bad?'700':'';}
function steps(n,cur){var h='<div class="fw-steps" title="Auftrag · Laden/Prüfen · Neustart · Fertig">';for(var i=1;i<=4;i++)h+='<i class="'+(i<cur||(cur===4&&i===4)?'on':(i===cur?'cur':''))+'"></i>';return h+'</div>';}
function render(d){if(!urlSet&&d.url){url.value=d.url;urlSet=true;}
 if(d.message)say(d.message,/Abgebrochen|Fehler|markiert/.test(d.message)&&!d.running);
 var devs=(d.devices||[]).filter(function(n){return !n.local;}),inRoll=devs.filter(function(n){return n.state;});
 if(inRoll.length){var fin=inRoll.filter(function(n){return ['done','skipped','failed','stopped'].indexOf(n.state)>=0;}).length,
  cur=inRoll.find(function(n){return ['sent','updating','restarting'].indexOf(n.state)>=0;}),failed=inRoll.filter(function(n){return n.state==='failed';}).length;
  prog.hidden=false;progBar.style.width=Math.round(100*fin/inRoll.length)+'%';progBar.style.background=failed?'#f59e0b':'#16a34a';
  progText.textContent=(d.running?'Rollout läuft: ':'Rollout beendet: ')+fin+' von '+inRoll.length+' Nodes bearbeitet'+(cur?' · gerade: '+cur.hostname+' – '+ST[cur.state][0]:'')+(failed?' · '+failed+' rot markiert':'');}
 else prog.hidden=true;
 rows.innerHTML=(d.devices||[]).map(function(n){var s=n.state?ST[n.state]||[n.state,'fw-idle',0]:null;
  var badge=n.local?'<span class="fw-badge fw-idle">Coordinator</span>':(s?'<span class="fw-badge '+s[1]+'">'+esc(s[0])+(n.state_age_s!=null&&s[1]==='fw-active'?' · '+n.state_age_s+' s':'')+'</span>'+(s[2]>0&&n.state!=='skipped'?steps(4,s[2]):''):'');
  var cls=s&&s[1]==='fw-active'?'fw-row-active':(n.state==='failed'?'fw-row-fail':'');
  return '<tr class="'+cls+'"><td><strong>'+esc(n.hostname)+'</strong>'+(n.online?'':' <span class="muted">offline</span>')+'</td><td>'+esc(n.release)+(n.version&&n.version!==n.release?' → '+esc(n.version):'')+'</td><td>'+badge+'</td><td class="muted">'+esc(n.text||'')+'</td></tr>';}).join('');
 clearTimeout(timer);timer=setTimeout(load,(d.running||d.local_busy)?1500:8000);}
function load(){if(localActive){clearTimeout(timer);timer=setTimeout(load,4000);return;}
 fetch('/cluster_firmware_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(render).catch(function(e){if(!localActive)say('Status nicht abrufbar: '+(e.message||e),true);clearTimeout(timer);timer=setTimeout(load,4000);});}
function act(a,confirmText){if(confirmText&&!confirm(confirmText))return;say('Wird gesendet …');post({action:a,url:url.value.trim(),allow_older:older.checked?'1':'0'}).then(function(j){say((j.ok?'✓ ':'✗ ')+j.message,!j.ok);load();});}
// Coordinator update: follow it until it is back. While it downloads,
// verifies and restarts it does not answer at all (its loop is busy), so the
// text runs on its own clock and failed requests are expected, not errors.
var localActive=false;
function watchLocal(before){var t0=Date.now(),phase='Coordinator bereitet das Update vor',done=false;loc.hidden=false;localActive=true;
 var bar=loc.querySelector('.fw-bar');bar.hidden=false;
 function show(){if(done)return;var sec=Math.round((Date.now()-t0)/1000);locText.textContent=phase+' … ('+sec+' s)';
  if(sec>240){done=true;localActive=false;bar.hidden=true;locText.textContent='✗ Coordinator nach 4 min nicht zurück – kurz stromlos machen.';}}
 var tick=setInterval(function(){if(done){clearInterval(tick);return;}show();},1000);
 function finish(text){done=true;localActive=false;bar.hidden=true;locText.textContent=text;clearInterval(tick);}
 function step(){if(done)return;var ctrl=new AbortController(),to=setTimeout(function(){ctrl.abort();},4000);
  fetch('/cluster_firmware_status',{cache:'no-store',credentials:'same-origin',signal:ctrl.signal}).then(function(r){return r.json();}).then(function(d){
   clearTimeout(to);var me=(d.devices||[]).find(function(n){return n.local;})||{},sec=Math.round((Date.now()-t0)/1000);
   if(me.release&&me.release!==before){finish('✓ Coordinator läuft wieder – jetzt '+me.release+' (nach '+sec+' s). Seite wird neu geladen …');render(d);setTimeout(function(){location.reload();},3000);return;}
   if(d.local_result&&d.local_result.code_name!=='selected'&&!d.local_busy){finish('✗ Coordinator-Update: '+d.local_result.code_name+(d.local_result.text?' – '+d.local_result.text:''));return;}
   phase=d.local_busy?'Coordinator lädt und prüft die Firmware':'Coordinator bereitet das Update vor';show();setTimeout(step,1500);
  }).catch(function(){clearTimeout(to);
   phase='Coordinator lädt, prüft und startet neu – er antwortet währenddessen nicht, die Seite verbindet sich danach von selbst';show();setTimeout(step,2000);});}
 show();step();}
document.getElementById('fwCheck').addEventListener('click',function(){say('Manifest wird geladen …');man.innerHTML='';
 post({action:'check',url:url.value.trim()}).then(function(j){say((j.ok?'✓ ':'✗ ')+j.message,!j.ok);
  if(j.releases&&j.releases.length)man.innerHTML='<p><strong>Im Manifest:</strong> '+j.releases.map(function(r){return esc(r.board)+' '+esc(r.version)+' ('+Math.round(r.size/1024)+' KB)';}).join(' · ')+'</p>';});});
document.getElementById('fwStart').addEventListener('click',function(){act('start','Alle Nodes nacheinander aktualisieren? Jeder Node startet dabei neu.'+(older.checked?' Ältere Versionen sind erlaubt!':''));});
document.getElementById('fwStop').addEventListener('click',function(){act('stop');});
document.getElementById('fwClear').addEventListener('click',function(){act('clear');});
document.getElementById('fwLocal').addEventListener('click',function(){
 if(!confirm('Den Coordinator jetzt aktualisieren? Er startet danach neu, der Cluster ist kurz ohne Coordinator.'))return;
 fetch('/cluster_firmware_status',{cache:'no-store',credentials:'same-origin'}).then(function(r){return r.json();}).then(function(d){
  var me=(d.devices||[]).find(function(n){return n.local;})||{};var before=me.release||'';
  return post({action:'local',url:url.value.trim(),allow_older:older.checked?'1':'0'}).then(function(j){
   if(!j.ok){say('✗ '+j.message,true);return;}say('✓ '+j.message);watchLocal(before);});
 }).catch(function(e){say('✗ '+(e.message||e),true);});});
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
