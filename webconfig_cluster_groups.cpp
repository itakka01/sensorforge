#include "webconfig_cluster_groups.h"

#include "cluster.h"
#include "cluster_group_logic.h"
#include "cluster_groups.h"
#include "webconfig_cluster.h"
#include "config.h"
#include "logger.h"

namespace {

static WebServer *g_server = nullptr;

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

static String deciText(int32_t deci)
{
    char text[16];
    ClusterGroups::formatDeci(deci, text);
    return String(text);
}

static bool requireCoordinator()
{
    if (clusterLocalIsCoordinator())
        return true;
    g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
    return false;
}

static void handleGroupsJson()
{
    if (!requireCoordinator())
        return;
    g_server->sendHeader("Cache-Control", "no-store");
    webconfigClusterSendLargeJson(*g_server, clusterGroupsJson());
}

static void handleGroupSave()
{
    if (!requireCoordinator())
        return;
    WebServer &server = *g_server;
    String error;
    if (!clusterGroupsSave(server.arg("group_id"), server.arg("orientation_deg"),
                           server.arg("distance_mm"), error)) {
        server.send(400, "text/plain; charset=utf-8", error);
        return;
    }
    logWrite("Cluster group saved | group=" + server.arg("group_id") +
             " | orientation_deg=" + server.arg("orientation_deg") +
             " | distance_mm=" + server.arg("distance_mm"));
    server.send(200, "text/plain; charset=utf-8", "Gruppe gespeichert und bestätigt.");
}

static void handleGroupDelete()
{
    if (!requireCoordinator())
        return;
    WebServer &server = *g_server;
    String error;
    if (!clusterGroupsDelete(server.arg("group_id"), error)) {
        server.send(400, "text/plain; charset=utf-8", error);
        return;
    }
    logWrite("Cluster group deleted | group=" + server.arg("group_id"));
    server.send(200, "text/plain; charset=utf-8", "Gruppe gelöscht.");
}

} // namespace

bool webconfigClusterMountSaveFromRequest(
    WebServer &server,
    bool writeToSd,
    bool &changed,
    String &error
)
{
    changed = false;
    error = "";
    // Older browser tabs without the mount fields: nothing to save.
    if (!server.hasArg("group_id"))
        return true;
    const ConfigSaveResult result = configSaveClusterMount(
        server.arg("group_id"),
        server.arg("mount_position"),
        server.arg("height_mm"),
        server.arg("orientation_horizontal_deg"),
        server.arg("orientation_vertical_deg"),
        writeToSd,
        error,
        &changed
    );
    if (result == CONFIG_SAVE_INTERNAL_FAILED || result == CONFIG_SAVE_SD_FAILED) {
        if (!error.length())
            error = "Montagedaten konnten nicht gespeichert werden";
        return false;
    }
    if (changed) {
        logWrite("Cluster mount saved | group=" + cfg_group_id + " | position=" + cfg_mount_position +
                 " | height_mm=" + String((long)cfg_height_mm) +
                 " | h_deg=" + deciText(cfg_orientation_horizontal_deci) +
                 " | v_deg=" + deciText(cfg_orientation_vertical_deci));
    }
    return true;
}

String webconfigClusterMountFormHtml()
{
    String html;
    html.reserve(2600);
    // Fields only: embedded in the cluster settings form and saved with the
    // common "Speichern" button (/cluster_save).
    html +=
        "<section class='settings-section' id='sfMount'><h3>Montage und Gruppe (optional)</h3>"
        "<p class='muted'>Beschreibt, wo dieses Gerät montiert ist. Der Coordinator erkennt daran Gruppen und kann Nodes danach filtern. "
        "Alle Felder sind optional; ohne Angaben arbeitet das Gerät wie bisher im Cluster. Gerätename bleibt die Gerätebezeichnung. "
        "Gespeichert wird mit „Speichern“. Ändern sich nur diese Felder, gilt die Änderung sofort ohne Neustart.</p>"
        "<div class='cluster-grid'>"
        "<label class='cluster-label' for='sfMountGroup'>Gruppe</label>"
        "<input id='sfMountGroup' name='group_id' maxlength='15' placeholder='z. B. G03' value='" + htmlEscape(cfg_group_id) + "'>"
        "<label class='cluster-label' for='sfMountPosition'>Montageposition</label>"
        "<input id='sfMountPosition' name='mount_position' maxlength='15' placeholder='z. B. P02' value='" + htmlEscape(cfg_mount_position) + "'>"
        "<div class='cluster-note'>Gruppe/Position: bis 15 Zeichen A–Z, 0–9, _ . -; Kleinbuchstaben werden zu Großbuchstaben.</div>"
        "<label class='cluster-label' for='sfMountHeight'>Montagehöhe (mm)</label>"
        "<input id='sfMountHeight' name='height_mm' type='number' min='0' max='100000' step='1' placeholder='unbekannt' value='" +
        String(cfg_height_mm >= 0 ? String((long)cfg_height_mm) : String("")) + "'>"
        "<label class='cluster-label' for='sfMountOrientH'>Horizontale Abweichung zur Gruppenausrichtung (°)</label>"
        "<input id='sfMountOrientH' name='orientation_horizontal_deg' type='number' min='-180' max='180' step='0.1' value='" +
        deciText(cfg_orientation_horizontal_deci) + "'>"
        "<label class='cluster-label' for='sfMountOrientV'>Vertikale Neigung (°)</label>"
        "<input id='sfMountOrientV' name='orientation_vertical_deg' type='number' min='-90' max='90' step='0.1' value='" +
        deciText(cfg_orientation_vertical_deci) + "'>"
        "<div class='cluster-note'>Winkel mit einer Nachkommastelle; Standard 0°. Höhe leer = unbekannt.</div>"
        "</div></section>";
    return html;
}

String webconfigClusterGroupsSectionHtml()
{
    return R"__GROUPS__(<details class='settings-section' id='sfGroups'><summary><strong>Gruppenverwaltung</strong> <span id='sfGroupsSummary' class='muted'></span></summary>
<p class='muted'>Gruppen entstehen automatisch aus der Gruppe, die Nodes in ihren Montagedaten melden. Ausrichtung und Abstand zum Objekt werden nur hier am Coordinator gespeichert. Gruppen ohne Nodes online bleiben als <em>inaktiv</em> erhalten. Wird eine Gruppe nach längerer Abwesenheit (≥ 10 min) wieder genutzt, gelten ihre alten Werte nur als <em>Vorschlag</em> und müssen bestätigt werden – sie sind keine vermessene Geometrie.</p>
<div class='cluster-select-tools'><button type='button' id='sfGroupsReload'>Aktualisieren</button> <label><input type='checkbox' id='sfGroupsShowInactive' checked> Inaktive anzeigen</label> <span id='sfGroupsState' class='muted' role='status'></span></div>
<div class='cluster-table-wrap'><table class='cluster-table' style='min-width:900px'><thead><tr><th>Gruppe</th><th>Status</th><th>Nodes online</th><th>Ausrichtung (°)</th><th>Abstand zum Objekt (mm)</th><th>Aktion</th></tr></thead>
<tbody id='sfGroupsRows'><tr><td colspan='6' class='muted'>Zum Laden öffnen.</td></tr></tbody></table></div>
<script>(function(){
var box=document.getElementById('sfGroups'),rows=document.getElementById('sfGroupsRows'),st=document.getElementById('sfGroupsState'),sum=document.getElementById('sfGroupsSummary'),inactive=document.getElementById('sfGroupsShowInactive');
var last=null;
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
var label={new:['Neu – Werte festlegen','warn'],confirmed:['Bestätigt','ok'],suggestion:['Vorschlag – bitte prüfen und bestätigen','warn'],inactive:['Inaktiv','']};
function nodesText(g){var n=g.nodes||[];if(!n.length)return '–';var t=n.slice(0,6).map(function(x){return esc(x.hostname)+(x.position?' ('+esc(x.position)+')':'');}).join(', ');return n.length+': '+t+(n.length>6?' … +'+(n.length-6):'');}
function render(d){last=d;if(!d.ok){rows.innerHTML='<tr><td colspan="6">Gruppenverwaltung nicht verfügbar ('+esc(d.error)+').</td></tr>';return;}
var gs=(d.groups||[]).filter(function(g){return inactive.checked||g.state!=='inactive';});
sum.textContent='· '+(d.groups||[]).filter(function(g){return g.state!=='inactive';}).length+' aktiv · '+d.stored+'/'+d.max_stored+' gespeichert · '+d.ungrouped_nodes+' Gerät(e) ohne Gruppe'+(d.unknown_mount_nodes?' · '+d.unknown_mount_nodes+' ohne Montagedaten (ältere Firmware)':'');
rows.innerHTML=gs.length?gs.map(function(g){var l=label[g.state]||[g.state,''];var o=g.orientation_deg==null?'':g.orientation_deg,dist=g.distance_mm==null?'':g.distance_mm;
var act=g.state==='inactive'?'<button type="button" data-act="save">Werte speichern</button> <button type="button" data-act="delete">Löschen</button>':'<button type="button" data-act="save">'+(g.state==='suggestion'?'Bestätigen':'Speichern')+'</button>';
return '<tr data-group="'+esc(g.id)+'"><td><strong>'+esc(g.id)+'</strong></td><td><span class="status-pill '+l[1]+'">'+esc(l[0])+'</span>'+(g.state==='inactive'&&g.last_seen_age_s!=null?' <span class="muted">zuletzt vor '+Math.round(g.last_seen_age_s/60)+' min</span>':'')+'</td><td style="white-space:normal">'+nodesText(g)+'</td><td><input type="number" min="0" max="359.9" step="0.1" style="width:7em" data-f="orientation_deg" value="'+esc(o)+'" placeholder="unbekannt"></td><td><input type="number" min="0" max="1000000" step="1" style="width:9em" data-f="distance_mm" value="'+esc(dist)+'" placeholder="unbekannt"></td><td>'+act+'</td></tr>';}).join(''):'<tr><td colspan="6" class="muted">Keine Gruppen. Gruppen entstehen, sobald Nodes in „Montage und Gruppe“ eine Gruppe melden.</td></tr>';}
function load(){st.textContent='Lädt …';fetch('/cluster_groups?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t||('HTTP '+r.status));});return r.json();}).then(function(d){render(d);st.textContent='';}).catch(function(e){st.textContent='Fehler: '+(e.message||e);});}
function post(url,fd){return fetch(url,{method:'POST',body:fd,credentials:'same-origin'}).then(function(r){return r.text().then(function(t){if(!r.ok)throw Error(t||('HTTP '+r.status));return t;});});}
rows.addEventListener('click',function(e){var b=e.target.closest('button[data-act]');if(!b)return;var tr=b.closest('tr'),id=tr.getAttribute('data-group'),fd=new FormData();fd.append('group_id',id);
if(b.getAttribute('data-act')==='delete'){if(!confirm('Gespeicherte Werte der Gruppe '+id+' löschen?'))return;b.disabled=true;post('/cluster_group_delete',fd).then(function(t){st.textContent=t;load();}).catch(function(e){st.textContent='Fehler: '+(e.message||e);b.disabled=false;});return;}
tr.querySelectorAll('input[data-f]').forEach(function(i){fd.append(i.getAttribute('data-f'),i.value);});b.disabled=true;post('/cluster_group_save',fd).then(function(t){st.textContent=id+': '+t;load();}).catch(function(e){st.textContent='Fehler: '+(e.message||e);b.disabled=false;});});
document.getElementById('sfGroupsReload').addEventListener('click',load);
inactive.addEventListener('change',function(){if(last)render(last);});
box.addEventListener('toggle',function(){if(box.open)load();});
})();</script></details>
)__GROUPS__";
}

String webconfigClusterMountFilterHtml()
{
    return R"__FILTER__(<div id='sfMountFilter' class='cluster-select-tools' style='flex-wrap:wrap;align-items:center;gap:6px'>
<strong>Filter</strong>
<label>Gruppe <select id='sfFltGroup'><option value=''>alle</option></select></label>
<label>Position <input id='sfFltPos' size='10' placeholder='z. B. P01,P02'></label>
<label>Höhe mm <input id='sfFltHMin' type='number' style='width:6em' placeholder='min'> – <input id='sfFltHMax' type='number' style='width:6em' placeholder='max'></label>
<details style='display:inline-block'><summary>Ausrichtung</summary>
<label>horizontal ° <input id='sfFltOhMin' type='number' step='0.1' style='width:5em' placeholder='min'> – <input id='sfFltOhMax' type='number' step='0.1' style='width:5em' placeholder='max'></label>
<label>vertikal ° <input id='sfFltOvMin' type='number' step='0.1' style='width:5em' placeholder='min'> – <input id='sfFltOvMax' type='number' step='0.1' style='width:5em' placeholder='max'></label></details>
<button type='button' id='sfFltSelect'>Treffer auswählen</button> <button type='button' id='sfFltReset'>Filter zurücksetzen</button>
<span id='sfFltCount' class='muted'></span></div>
<script>(function(){
function num(v){v=String(v==null?'':v).trim().replace(',','.');if(v==='')return null;var n=Number(v);return Number.isFinite(n)?n:null;}
function range(x,min,max){if(min===null&&max===null)return true;if(x===null||x===undefined||x==='')return false;x=Number(x);if(!Number.isFinite(x))return false;return (min===null||x>=min)&&(max===null||x<=max);}
// Pure match function: n = node from /cluster_status, f = filter values.
window.sfMountMatches=function(n,f){
 var known=!!n.mount_known,g=known?String(n.group_id||''):'',p=known?String(n.mount_position||''):'';
 if(f.group==='__none__'){if(g)return false;}else if(f.group&&g!==f.group)return false;
 if(f.positions&&f.positions.length&&f.positions.indexOf(p.toUpperCase())<0)return false;
 if(!range(known?n.height_mm:null,f.hMin,f.hMax))return false;
 if(!range(known?n.orientation_horizontal_deg:null,f.ohMin,f.ohMax))return false;
 if(!range(known?n.orientation_vertical_deg:null,f.ovMin,f.ovMax))return false;
 return true;};
window.sfMountText=function(n){if(!n.mount_known)return 'unbekannt (ältere Firmware)';var x=[];if(n.group_id)x.push(n.group_id);if(n.mount_position)x.push(n.mount_position);if(n.height_mm!=null)x.push(n.height_mm+' mm');var h=Number(n.orientation_horizontal_deg)||0,v=Number(n.orientation_vertical_deg)||0;if(h||v)x.push('H '+h+'° · V '+v+'°');return x.length?x.join(' · '):'–';};
function el(id){return document.getElementById(id);}
function current(){var pos=String(el('sfFltPos').value||'').split(',').map(function(s){return s.trim().toUpperCase();}).filter(Boolean);
 return {group:el('sfFltGroup').value,positions:pos,hMin:num(el('sfFltHMin').value),hMax:num(el('sfFltHMax').value),ohMin:num(el('sfFltOhMin').value),ohMax:num(el('sfFltOhMax').value),ovMin:num(el('sfFltOvMin').value),ovMax:num(el('sfFltOvMax').value)};}
var lastNodes=[],groupSig='';
function fillGroups(nodes){var gs={};nodes.forEach(function(n){if(n.mount_known&&n.group_id)gs[n.group_id]=1;});var list=Object.keys(gs).sort(),sig=list.join('|');if(sig===groupSig)return;groupSig=sig;var sel=el('sfFltGroup'),v=sel.value;
 sel.innerHTML='<option value="">alle</option><option value="__none__">ohne Gruppe</option>'+list.map(function(g){return '<option>'+g.replace(/[&<>"']/g,'')+'</option>';}).join('');
 if(v==='__none__'||list.indexOf(v)>=0)sel.value=v;else if(v){var o=document.createElement('option');o.value=v;o.textContent=v+' (keine Node online)';sel.appendChild(o);sel.value=v;}}
// Hidden rows are also removed from the selection, so bulk actions only ever
// address nodes that are visible under the active filter.
window.sfMountApplyFilter=function(nodes){if(Array.isArray(nodes))lastNodes=nodes;fillGroups(lastNodes);var f=current(),byId={};lastNodes.forEach(function(n){byId[String(n.integration_id||'')]=n;});
 var rows=document.querySelectorAll('#clusterHealthRows tr[data-node-id]'),shown=0;
 rows.forEach(function(tr){var n=byId[tr.getAttribute('data-node-id')],ok=!n||window.sfMountMatches(n,f);tr.style.display=ok?'':'none';if(ok){shown++;return;}var c=tr.querySelector('.cluster-node-choice');if(c&&c.checked){c.checked=false;}if(c&&window.sfSelectedNodes)window.sfSelectedNodes.delete(c.value);});
 el('sfFltCount').textContent=shown+' von '+rows.length+' Geräten angezeigt';};
function apply(){window.sfMountApplyFilter();if(window.sfSelectionUpdate)window.sfSelectionUpdate();}
['sfFltGroup','sfFltPos','sfFltHMin','sfFltHMax','sfFltOhMin','sfFltOhMax','sfFltOvMin','sfFltOvMax'].forEach(function(id){el(id).addEventListener('input',apply);el(id).addEventListener('change',apply);});
el('sfFltReset').addEventListener('click',function(){['sfFltPos','sfFltHMin','sfFltHMax','sfFltOhMin','sfFltOhMax','sfFltOvMin','sfFltOvMax'].forEach(function(id){el(id).value='';});el('sfFltGroup').value='';apply();});
el('sfFltSelect').addEventListener('click',function(){if(!window.sfSelectedNodes)return;window.sfSelectedNodes.clear();document.querySelectorAll('#clusterHealthRows tr[data-node-id]').forEach(function(tr){var c=tr.querySelector('.cluster-node-choice');if(!c)return;var vis=tr.style.display!=='none';c.checked=vis;if(vis)window.sfSelectedNodes.add(c.value);});if(window.sfSelectionUpdate)window.sfSelectionUpdate();});
})();</script>
)__FILTER__";
}

void webconfigClusterGroupsRegisterRoutes(WebServer &server)
{
    g_server = &server;
    server.on("/cluster_groups", HTTP_GET, handleGroupsJson);
    server.on("/cluster_group_save", HTTP_POST, handleGroupSave);
    server.on("/cluster_group_delete", HTTP_POST, handleGroupDelete);
}
