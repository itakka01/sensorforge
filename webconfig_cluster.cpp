#include "webconfig_cluster.h"

#include "cluster.h"
#include "cluster_profiles.h"
#include "config.h"
#include "device_identity.h"
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

    html +=
        "<form id='clusterSettingsForm' method='POST' action='/cluster_save'>"
        "<section class='settings-section'><h3>Cluster-Mitgliedschaft</h3>"
        "<div class='cluster-grid'>"
        "<div class='cluster-label'>Teilnahme dieses Geräts</div>"
        "<div><select name='cluster_enabled' id='clusterEnabled'>"
        "<option value='0'" + String(!cfg_cluster_enabled ? " selected" : "") + ">Nicht am Clusterverbund teilnehmen</option>"
        "<option value='1'" + String(cfg_cluster_enabled ? " selected" : "") + ">Gerät mit Clusterverbund verbinden</option>"
        "</select></div>"
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
        "<option value='auto'" + String(cfg_cluster_coordinator_policy == "auto" ? " selected" : "") + ">Automatisch (Standard)</option>"
        "<option value='preferred'" + String(cfg_cluster_coordinator_policy == "preferred" ? " selected" : "") + ">Bevorzugter Coordinator</option>"
        "<option value='node'" + String(cfg_cluster_coordinator_policy == "node" ? " selected" : "") + ">Nur Node (nie Coordinator)</option>"
        "</select></div>"
        "<div class='cluster-note'>Normalerweise ist „Automatisch“ richtig. <details><summary>Info zur Koordination</summary>Geräte bestimmen ihren Coordinator selbst. Bevorzugt erhöht die Priorität, Nur Node verhindert eine Coordinator-Rolle.</details></div>"
        "<div class='cluster-label'>Technische Gerätekennung</div>"
        "<div class='cluster-id'>" + htmlEscape(deviceIntegrationId()) + "</div>"
        "<div class='cluster-note'><details><summary>Info zur Gerätekennung</summary>Stabile interne Integration-ID; unabhängig von Gerätename und Lizenz.</details></div>"
        "</div></div></section>"
        "<div class='floating-save-space'></div>"
        "<div id='clusterSaveBar' class='floating-save-bar'>"
        "<span id='clusterSaveState' class='floating-save-state'>Keine ungespeicherten Änderungen</span>"
        "<button id='clusterSaveButton' type='submit' disabled>Speichern</button>"
        "</div></form>";

    html +=
        "<section class='settings-section'><h3>Cluster-Übersicht</h3>"
        "<div id='clusterRuntimeLine' class='cluster-runtime-line'><span class='status-pill'>Lädt …</span></div>"
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
        "<p class='muted' id='clusterAckDiagnostics' style='margin-top:10px'>ACK-Diagnose wird geladen …</p>"
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
function statusText(n){var x=[];if(n.recording)x.push('Recording');if(n.streaming)x.push('Streaming');if(n.transport)x.push('Transport');return x.length?x.join(' · '):'Bereit';}
function policyText(v){if(v==='preferred')return 'Bevorzugt';if(v==='node')return 'Nur Node';return 'Auto';}
function roleText(v){return v==='coordinator'?'Coordinator':'Node';}
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
function render(d){if(!rows||!line)return;renderChoices(d);var pills=[];pills.push('<span class="status-pill '+(d.runtime_active?'ok':'warn')+'">Runtime '+(d.runtime_active?'AKTIV':'AUS')+'</span>');var coord=String(d.coordinator_id||'');pills.push('<span class="status-pill '+(coord?'ok':'warn')+'">Coordinator '+(coord?esc(coord):'keiner')+(d.coordinator_epoch?(' · Epoch '+esc(d.coordinator_epoch)):'')+'</span>');var ct=d.cluster_time||{};pills.push('<span class="status-pill '+(ct.valid?'ok':'warn')+'">Clusterzeit '+(ct.valid?'aktiv':'wartet auf Zeitquelle')+'</span>');if(d.restart_required)pills.push('<span class="status-pill warn">NEUSTART ERFORDERLICH</span>');if(d.error)pills.push('<span class="status-pill danger">'+esc(d.error)+'</span>');line.innerHTML=pills.join(' ');var diag=document.getElementById('clusterAckDiagnostics'),q=d.ack_diagnostics||{};if(diag){diag.textContent='ACK-Diagnose (seit Runtime-Start, lokal): HB gesendet '+(q.hb_sent||0)+' / Sendefehler '+(q.hb_send_failed||0)+' · ACK-Sendeversuche '+(q.ack_send_attempted||0)+' / Sendefehler '+(q.ack_send_failed||0)+' · signierte ACK empfangen '+(q.ack_authenticated||0)+' / akzeptiert '+(q.ack_accepted||0)+' / abgelehnt '+(q.ack_rejected||0)+' · Coordinator-Wechsel '+(q.coordinator_changes||0)+' / Epoch-Wechsel '+(q.coordinator_epoch_changes||0)+' / ACK-Kontext-Resets '+(q.ack_context_resets||0)+' / unvollständige Coordinator-Meldungen '+(q.incomplete_coord_announcements||0)+' · letzter Ablehnungsgrund '+(q.last_reject||'none');}var td=document.getElementById('clusterTimeDiagnostics');if(td){var t=d.cluster_time||{};td.textContent='Clusterzeit: '+(t.valid?'gültig':'noch nicht verfügbar')+' · Quelle '+(t.source||'-')+' · Messungen '+(t.accepted||0)+' / verworfen '+(t.rejected||0)+' · RTT '+(t.last_rtt_us||0)+' µs · Alter '+(t.sample_age_ms||0)+' ms · Abweichung zur Geräteuhr '+(t.local_offset_available?((Number(t.local_offset_us||0)/1000).toFixed(2)+' ms'):'nicht messbar')+' · grobe Unsicherheit ±'+(Number(t.uncertainty_estimate_us||0)/1000).toFixed(2)+' ms (Schätzung, keine Garantie; keine Frame-Synchronität)';}var tr=document.getElementById('clusterElectionTrace');if(tr){var entries=Array.isArray(d.election_trace)?d.election_trace:[];tr.textContent='Letzte Election-Übergänge (RAM):\\n'+(entries.length?entries.join('\\n'):'keine');}var a=orderedNodes(Array.isArray(d.nodes)?d.nodes:[],d);var simple=document.getElementById('clusterSimpleRows');if(simple){simple.innerHTML=a.length?a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+'</td><td>'+esc(roleText(n.cluster_role))+nodeNavigationLink(n,d)+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join(''):'<tr><td colspan="5" class="muted">Keine Geräte sichtbar.</td></tr>';}if(!a.length){rows.innerHTML='<tr><td colspan="13" class="muted">Keine Geräte sichtbar.</td></tr>';}else{rows.innerHTML=a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+'</td><td><strong>'+esc(roleText(n.cluster_role))+'</strong></td><td>'+esc(policyText(n.coordinator_policy))+'</td><td class="cluster-id">'+esc(n.integration_id||'-')+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(n.mode||'-')+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.release||'-')+'</td><td>'+esc(timeText(n))+'</td><td>'+esc(leaseText(n))+'</td><td>'+esc(linkText(n,d))+'</td><td>'+esc(wifiText(n))+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join('');}
if(resRows){var byId={};a.forEach(function(n){byId[String(n.integration_id||'')]=n.hostname||n.integration_id||'-';});var rr=Array.isArray(d.resources)?d.resources:[];if(!rr.length){resRows.innerHTML='<tr><td colspan="7" class="muted">Noch keine Ressourcen angekündigt.</td></tr>';}else{resRows.innerHTML=rr.map(function(r){var owner=byId[String(r.owner_integration_id||'')]||r.owner_integration_id||'-';return '<tr><td><strong>'+esc(owner)+'</strong><div class="cluster-subtle">'+esc(r.source_ip||'-')+'</div></td><td class="cluster-id">'+esc(r.resource_id||'-')+'</td><td>'+esc(r.type||'-')+'</td><td>'+esc(resourceTime(r.timestamp_us))+'</td><td>'+esc(sizeText(r.size_bytes))+'</td><td>'+esc(duration(r.ttl_remaining_sec,'-'))+'</td><td class="cluster-id">'+esc(r.locator||'-')+'</td></tr>';}).join('');}}}
function poll(){fetch('/cluster_status?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw new Error();return r.json();}).then(render).catch(function(){if(line)line.innerHTML='<span class="status-pill danger">Status nicht erreichbar</span>';});}poll();setInterval(poll,3000);document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});
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

static void handleClusterStatus()
{
    clusterDiscoveryTouch();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", clusterStatusJson());
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
    html += "<section class='settings-section'><h3>Zeit synchronisieren</h3>"
        "<p>Fordert die aktuell verbundenen Nodes auf, ihre Clusterzeit jetzt "
        "neu abzugleichen. Die Systemuhren und laufenden Aufnahmen werden nicht verändert.</p>"
        "<form method='POST' action='/cluster_sync_time'>"
        "<button class='primary' type='submit'>Zeit jetzt synchronisieren</button></form>"
        "<p class='muted'>Der Befehl wird authentifiziert übertragen. "
        "Einzelne offline Nodes oder verlorene Funkpakete werden nicht bestätigt. "
        "Die Synchronisationsqualität ist auf der jeweiligen Cluster-Seite sichtbar.</p>"
        "</section>";
    if (server.arg("notice") == "sent")
        html += "<p class='flash-notice'>Synchronisation angefordert. "
                "Der Empfang durch alle Nodes ist damit noch nicht bestätigt.</p>";
    html += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleClusterSyncTime()
{
    String error;
    if (!clusterRequestTimeSync(error)) {
        server.send(409, "text/plain; charset=utf-8", error);
        return;
    }
    server.sendHeader("Location", "/cluster_coordinate?notice=sent");
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
        server.send(
            400,
            "text/plain; charset=utf-8",
            error.length() ? error : String("Cluster-Einstellungen konnten nicht gespeichert werden")
        );
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
    serverInstance.on("/cluster_save", HTTP_POST, handleClusterSave);
    serverInstance.on("/cluster_coordinate", HTTP_GET, handleClusterCoordinatePage);
    serverInstance.on("/cluster_sync_time", HTTP_POST, handleClusterSyncTime);
}
