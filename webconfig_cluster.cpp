#include "webconfig_cluster.h"

#include "cluster.h"
#include "drone_mode.h"
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

    // Drone is configured in Configuration > Betriebsmodus. Coordinator
    // bulk commands use the same NVS flag via droneModeSet().
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
function render(d){if(!rows||!line)return;renderChoices(d);var pills=[];pills.push('<span class="status-pill '+(d.runtime_active?'ok':'warn')+'">Runtime '+(d.runtime_active?'AKTIV':'AUS')+'</span>');var coord=String(d.coordinator_id||'');pills.push('<span class="status-pill '+(coord?'ok':'warn')+'">Coordinator '+(coord?esc(coord):'keiner')+(d.coordinator_epoch?(' · Epoch '+esc(d.coordinator_epoch)):'')+'</span>');var ct=d.cluster_time||{};pills.push('<span class="status-pill '+(ct.valid?'ok':'warn')+'">Clusterzeit '+(ct.valid?'aktiv':'wartet auf Zeitquelle')+'</span>');if(d.restart_required)pills.push('<span class="status-pill warn">NEUSTART ERFORDERLICH</span>');if(d.error)pills.push('<span class="status-pill danger">'+esc(d.error)+'</span>');line.innerHTML=pills.join(' ');var diag=document.getElementById('clusterAckDiagnostics'),q=d.ack_diagnostics||{};if(diag){diag.textContent='ACK-Diagnose (seit Runtime-Start, lokal): HB gesendet '+(q.hb_sent||0)+' / Sendefehler '+(q.hb_send_failed||0)+' · ACK-Sendeversuche '+(q.ack_send_attempted||0)+' / Sendefehler '+(q.ack_send_failed||0)+' · signierte ACK empfangen '+(q.ack_authenticated||0)+' / akzeptiert '+(q.ack_accepted||0)+' / abgelehnt '+(q.ack_rejected||0)+' · Coordinator-Wechsel '+(q.coordinator_changes||0)+' / Epoch-Wechsel '+(q.coordinator_epoch_changes||0)+' / ACK-Kontext-Resets '+(q.ack_context_resets||0)+' / unvollständige Coordinator-Meldungen '+(q.incomplete_coord_announcements||0)+' · letzter Ablehnungsgrund '+(q.last_reject||'none');}var jd=document.getElementById('clusterJobDiagnostics'),j=d.job_probe_diagnostics||{};if(jd)jd.textContent='Job-Probe (lokal): empfangen '+(j.received||0)+' / ACK gesendet '+(j.ack_sent||0)+' / abgelehnt '+(j.rejected||0)+' ('+(j.last_reject||'none')+') · ACK empfangen '+(j.ack_received||0)+' / abgelehnt '+(j.ack_rejected||0)+' ('+(j.last_ack_reject||'none')+')';var rd=document.getElementById('clusterRestartDiagnostics'),r=d.job_restart_diagnostics||{};if(rd)rd.textContent='Neustart-Diagnose (lokal): Aufträge empfangen '+(r.received||0)+' / geplant und ACK gesendet '+(r.accepted||0)+' / abgelehnt '+(r.rejected||0)+' ('+(r.last_reject||'none')+') · ACK empfangen '+(r.ack_received||0)+' / akzeptiert '+(r.ack_accepted||0)+' / abgelehnt '+(r.ack_rejected||0)+' ('+(r.last_ack_reject||'none')+')';var td=document.getElementById('clusterTimeDiagnostics');if(td){var t=d.cluster_time||{};td.textContent='Clusterzeit: '+(t.valid?'gültig':'noch nicht verfügbar')+' · Quelle '+(t.source||'-')+' · Messungen '+(t.accepted||0)+' / verworfen '+(t.rejected||0)+' · RTT '+(t.last_rtt_us||0)+' µs · Alter '+(t.sample_age_ms||0)+' ms · Abweichung zur Geräteuhr '+(t.local_offset_available?((Number(t.local_offset_us||0)/1000).toFixed(2)+' ms'):'nicht messbar')+' · grobe Unsicherheit ±'+(Number(t.uncertainty_estimate_us||0)/1000).toFixed(2)+' ms (Schätzung, keine Garantie; keine Frame-Synchronität)';}var tr=document.getElementById('clusterElectionTrace');if(tr){var entries=Array.isArray(d.election_trace)?d.election_trace:[];tr.textContent='Letzte Election-Übergänge (RAM):\\n'+(entries.length?entries.join('\\n'):'keine');}var a=orderedNodes(Array.isArray(d.nodes)?d.nodes:[],d);var simple=document.getElementById('clusterSimpleRows');if(simple){simple.innerHTML=a.length?a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+'</td><td>'+esc(roleText(n.cluster_role))+nodeNavigationLink(n,d)+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join(''):'<tr><td colspan="5" class="muted">Keine Geräte sichtbar.</td></tr>';}if(!a.length){rows.innerHTML='<tr><td colspan="13" class="muted">Keine Geräte sichtbar.</td></tr>';}else{rows.innerHTML=a.map(function(n){return '<tr><td><strong>'+esc(n.hostname||'-')+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'')+'</td><td><strong>'+esc(roleText(n.cluster_role))+'</strong></td><td>'+esc(policyText(n.coordinator_policy))+'</td><td class="cluster-id">'+esc(n.integration_id||'-')+'</td><td>'+esc(n.ip||'-')+'</td><td>'+esc(n.mode||'-')+'</td><td>'+esc(statusText(n))+'</td><td>'+esc(n.release||'-')+'</td><td>'+esc(timeText(n))+'</td><td>'+esc(leaseText(n))+'</td><td>'+esc(linkText(n,d))+'</td><td>'+esc(wifiText(n))+'</td><td>'+esc(age(n.age_ms,n.local))+'</td></tr>';}).join('');}
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
    html += R"__HEALTH__(<section class='settings-section'><h3>Cluster-Geräte</h3>
<p class='muted'>Aktuelle Ressourcenwerte der verbundenen Geräte. SD-Daten werden normalerweise etwa einmal pro Minute aktualisiert, nach einem Cluster-Wipe unmittelbar erneut gemessen und signiert gemeldet. Fehlende Werte werden nicht geschätzt.</p>
<div class='cluster-select-tools'><button type='button' id='clusterSelectAll'>Alle Nodes auswählen</button> <button type='button' id='clusterSelectNone'>Auswahl aufheben</button> <span id='clusterSelectionCount'>0 ausgewählt</span></div>
<div class='cluster-select-tools'><button type='button' id='clusterBulkDroneOn'>Ausgewählte Nodes in Drone Mode</button> <button type='button' id='clusterBulkDroneOff'>Drone Mode ausschalten</button></div>
<div class='cluster-select-tools'><button type='button' id='clusterBulkProbe'>Verbindung testen</button> <button type='button' id='clusterBulkRestart'>Ausgewählte neu starten</button> <button type='button' id='clusterBulkShutdown'>Ausgewählte herunterfahren</button> <button type='button' id='clusterBulkSync'>Zeit ausgewählter Nodes synchronisieren</button> <button type='button' id='clusterBulkWipe' class='danger'>SD ausgewählter Nodes vollständig löschen</button></div>
<p class='muted'>Drone Mode hält Nodes passiv bereit: kein automatisches Recording, kein Power Shooter, keine Bewegungserkennung und keine Hintergrund-SD-Recovery. Laufende Aufnahmen werden beim Einschalten kontrolliert beendet. Clusterzeit, geplante Capture-Jobs, Webverwaltung und Temperaturschutz bleiben verfügbar.</p>
<div class='cluster-select-tools'><label for='clusterCaptureSeconds'>Foto in</label> <input id='clusterCaptureSeconds' type='number' min='0' max='59' step='1' value='10' style='width:5.5em' aria-label='Sekunden bis zum Foto'> Sekunden <input id='clusterCaptureMinutes' type='number' min='0' max='30' step='1' value='0' style='width:4.5em' aria-label='Minuten bis zum Foto'> Minuten <button type='button' id='clusterBulkCapture'>Aufnahme planen</button></div>
<div class='cluster-select-tools'><label for='clusterCaptureUtc'>Alternativ: fester UTC-Termin (optional)</label> <input id='clusterCaptureUtc' type='datetime-local' step='0.001'> <span class='muted'>Wenn ausgefüllt, hat UTC Vorrang.</span></div>
<div id='clusterCaptureCountdown' role='timer' aria-label='Countdown zum geplanten Capture' style='display:none;margin:12px 0;padding:15px 17px;border-radius:12px;border:1px solid #64748b55;align-items:center;gap:16px'>
 <span id='clusterCaptureLamp' aria-hidden='true' style='display:inline-block;flex:none;width:23px;height:23px;border-radius:50%;background:#dc2626;box-shadow:0 0 18px #dc262699'></span>
 <div><div id='clusterCaptureClock' style='font-size:2.1rem;font-weight:700;font-variant-numeric:tabular-nums;letter-spacing:.06em'>00:10.0</div><div id='clusterCaptureTargetText' class='muted' style='font-size:.85em'></div></div>
</div>
<style>
@keyframes sfCaptureRedPulse{0%,42%{opacity:1;box-shadow:0 0 25px 9px #dc262655}55%,100%{opacity:.20;box-shadow:0 0 3px 0 #dc262622}}
@keyframes sfCaptureGreenPulse{0%,22%,44%,66%,88%,100%{opacity:1;box-shadow:0 0 34px 12px #22c55e88}11%,33%,55%,77%{opacity:.28;box-shadow:0 0 8px 0 #22c55e55}}
#clusterCaptureCountdown.sf-armed #clusterCaptureLamp{animation:sfCaptureRedPulse 1s infinite;background:#dc2626}
#clusterCaptureCountdown.sf-fired #clusterCaptureLamp{animation:sfCaptureGreenPulse 2.8s ease-in-out 1;background:#16a34a}
</style>
<div id='clusterCaptureFeedback' class='muted' role='status'>Aufträge werden vorab verteilt. Zeitangaben gelten als UTC; gemeldet wird zunächst der Software-Dispatch, nicht der Belichtungsbeginn.</div>
<div id='clusterCaptureReport' style='display:none;margin-top:14px;padding:14px;border:1px solid #64748b66;border-radius:12px' aria-live='polite'>
 <h4 style='margin:0 0 8px'>Capture-Fertigstellungsbericht</h4>
 <div id='clusterCaptureReportSummary' role='status'>Noch kein Auftrag gestartet.</div>
 <div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Node</th><th>Auftrag</th><th>Ergebnis</th><th>Foto / Zeitpunkt / Abruf</th></tr></thead><tbody id='clusterCaptureReportRows'></tbody></table></div>
 <p class='muted' style='margin-bottom:0'>„JPEG erfolgreich“ bestätigt die Kamera-Rückmeldung des Nodes; kein Nachweis für exakten Belichtungsbeginn oder langfristige Speicherung. Bericht nur während dieser Browser-Sitzung; Job-Verlauf im Coordinator-RAM ist begrenzt.</p>
</div>
<div class='cluster-select-tools'><button type='button' id='clusterSdPreflight'>SD-Löschung prüfen (ohne Löschen)</button></div><p class='muted' id='clusterSdPreflightResult' role='status'>Vorbereitung: Hier werden nur SD- und Recording-Status der ausgewählten Nodes geprüft. Es werden keine Dateien gelöscht.</p>
<p class='muted'>Sammelaktionen werden pro Node einzeln mit eigenen signierten Befehlen ausgeführt; keine UDP-Broadcasts für Reboot oder Shutdown. Der Coordinator selbst ist nicht auswählbar. Remote-Shutdown benötigt RESET/Stromversorgung zum Wiederaufwachen.</p>
<div id='clusterBulkFeedback' class='muted' role='status'>Keine Sammelaktion gestartet.</div>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Auswahl</th><th>Gerät</th><th>Firmware</th><th>Rolle</th><th>Letzte Aktion</th><th>SD belegt</th><th>SD frei</th><th>CPU-Temperatur</th><th>Uptime</th><th>Boot-ID</th><th>Status</th></tr></thead>
<tbody id='clusterHealthRows'><tr><td colspan='11'>Lädt …</td></tr></tbody></table></div>
<details><summary>Weitere Systemwerte</summary><div id='clusterHealthExtra' class='muted'>Lädt …</div><p>CPU-Auslastung wird derzeit nicht gemessen. Temperatur ist der interne ESP32-Messwert, kein Sensor für die Umgebung.</p></details>

</section><script>
(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function fmt(n){return (Number(n)/1024).toFixed(1)+' GiB';}
function up(s){s=Math.max(0,Number(s)||0);return Math.floor(s/86400)+'d '+Math.floor((s%86400)/3600)+'h '+Math.floor((s%3600)/60)+'m';}
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
return '<tr><td>'+(n.local?'–':'<input type="checkbox" class="cluster-node-choice" value="'+esc(n.integration_id)+'" '+(window.sfSelectedNodes&&window.sfSelectedNodes.has(String(n.integration_id))?'checked':'')+'>')+'</td><td>'+(nodeUrl(n)?'<a href="'+nodeUrl(n)+'" target="_blank" rel="noopener noreferrer">'+esc(n.hostname||n.integration_id)+'</a>':esc(n.hostname||n.integration_id))+'</td><td>'+esc(n.release||'–')+'</td><td>'+esc(n.cluster_role||'–')+'</td><td>'+esc(latestJob(n,jobs))+'</td><td>'+(ok&&total?fmt(used):'–')+'</td><td>'+(ok&&total?fmt(Math.max(0,total-used)):'–')+'</td><td>'+(ok&&Number(n.cpu_temp_deci_c)>-1000?(Number(n.cpu_temp_deci_c)/10).toFixed(1)+' °C':'–')+'</td><td>'+esc(up(n.uptime_seconds))+'</td><td><code>'+esc(n.boot_id||'–')+'</code></td><td>'+esc(n.recording?'Recording':'Bereit')+'</td></tr>';}).join('');
var extra=document.getElementById('clusterHealthExtra');if(extra)extra.textContent=ns.map(function(n){return (n.hostname||n.integration_id)+': freier Heap '+(n.health_valid?Math.round(Number(n.free_heap_bytes||0)/1024)+' KiB':'unbekannt');}).join(' · ');
if(window.sfCaptureReportUpdate)window.sfCaptureReportUpdate(d);
if(window.sfSelectionUpdate)window.sfSelectionUpdate();

}
function pollHealth(){window.sfCoordinateStatus().then(function(d){renderHealth(d);}).catch(function(){});}
window.sfSelectedNodes=new Set();
window.sfSelectionUpdate=function(){var els=Array.from(document.querySelectorAll('.cluster-node-choice'));var ids=new Set(els.map(function(x){return x.value;}));Array.from(window.sfSelectedNodes).forEach(function(id){if(!ids.has(id))window.sfSelectedNodes.delete(id);});var label=document.getElementById('clusterSelectionCount');if(label)label.textContent=window.sfSelectedNodes.size+' ausgewählt';};
document.addEventListener('change',function(e){if(e.target.classList.contains('cluster-node-choice')){if(e.target.checked)window.sfSelectedNodes.add(e.target.value);else window.sfSelectedNodes.delete(e.target.value);window.sfSelectionUpdate();}});
function setAll(v){document.querySelectorAll('.cluster-node-choice').forEach(function(e){e.checked=v;if(v)window.sfSelectedNodes.add(e.value);else window.sfSelectedNodes.delete(e.value);});window.sfSelectionUpdate();}
document.getElementById('clusterSelectAll').addEventListener('click',function(){setAll(true);});
document.getElementById('clusterSelectNone').addEventListener('click',function(){setAll(false);});
var actionBusy=false;
async function runBulk(path,label,danger){if(actionBusy)return;var ids=Array.from(window.sfSelectedNodes);var out=document.getElementById('clusterBulkFeedback');if(!ids.length){out.textContent='Bitte zuerst mindestens einen entfernten Node auswählen.';return;}
var text=label+' für '+ids.length+' ausgewählte Nodes auslösen?';if(danger)text+=' ACHTUNG: Shutdown ohne Wake-Quellen; RESET oder Stromversorgung zum Wiederaufwachen erforderlich.';
if((path==='/cluster_node_restart'||path==='/cluster_node_shutdown'||path==='/cluster_node_wipe')&&!confirm(path==='/cluster_node_wipe'?'ACHTUNG: Alle SD-Daten der ausgewählten Nodes unwiderruflich löschen? Laufende Aufnahmen werden SOFORT beendet und ebenfalls gelöscht. Konfiguration wird aus internem Backup wiederhergestellt.':text))return;actionBusy=true;window.sfCoordinatorBulkActive=true;var buttons=['clusterBulkProbe','clusterBulkRestart','clusterBulkShutdown','clusterBulkSync','clusterBulkWipe','clusterBulkDroneOn','clusterBulkDroneOff'];buttons.forEach(function(x){document.getElementById(x).disabled=true;});var done=0,errors=0,details=[];
try{for(var i=0;i<ids.length;i++){out.textContent=label+': '+(i+1)+'/'+ids.length+' – '+ids[i];var fd=new FormData();fd.append('node_id',ids[i]);if(path==='/cluster_node_drone')fd.append('enabled',label==='Drone Mode EIN'?'1':'0');var ctrl=new AbortController(),timer=setTimeout(function(){ctrl.abort();},12000);
try{var res=await fetch(path,{method:'POST',body:fd,credentials:'same-origin',cache:'no-store',redirect:'manual',signal:ctrl.signal});if(!(res.ok||res.status===0||res.status===303)){errors++;var body='';try{body=(await res.text()).slice(0,100);}catch(ignored){}details.push(ids[i]+': HTTP '+res.status+(body?' '+body:''));}else{done++;details.push(ids[i]+': Auftrag vom HTTP-Endpunkt angenommen');}}catch(e){errors++;details.push(ids[i]+': HTTP-Anfrage fehlgeschlagen ('+(e.name==='AbortError'?'12-s-Zeitlimit':String(e.message||e))+'); Ausführung unbekannt');}finally{clearTimeout(timer);}await new Promise(function(resolve){setTimeout(resolve,250);});}
out.textContent=label+': '+done+' HTTP-Annahmen, '+errors+' HTTP-Fehler. '+details.join(' | ')+'. HTTP-Erfolg bedeutet nicht ACK oder Ausführung; Einzelergebnisse im Status prüfen.';
}finally{window.sfCoordinatorBulkActive=false;actionBusy=false;window.sfCoordinateStatus().catch(function(){});buttons.forEach(function(x){document.getElementById(x).disabled=false;});}}
// Generic absolute-UTC schedule derivation: reuse for future actions such as GPIO/sequence/video.
function sfPlanUtcTarget(utcValue,seconds,minutes,nowMs){
 if(utcValue){var fixed=Date.parse(utcValue+'Z');return Number.isFinite(fixed)?fixed:NaN;}
 if(!Number.isFinite(seconds)||!Number.isFinite(minutes)||seconds<0||minutes<0||!Number.isInteger(seconds)||!Number.isInteger(minutes))return NaN;
 return nowMs+(minutes*60+seconds)*1000;
}
// Capture report tracks one specific submission set, not the last arbitrary Node action.
// Job IDs in the bounded coordinator RAM table are authoritative after signature verification.
var sfCaptureReport=null, sfLatestStatus=null;
var sfSeenCaptureJobs=new Set();
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
  var j=r.job||{},uuid=String(j.media_uuid||''),u=String(j.media_url||'');
  var isValid=/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(uuid);
  var safeUrl='';try{var parsed=new URL(u);if(parsed.protocol==='http:'&&parsed.pathname==='/capture_media'&&parsed.searchParams.get('uuid')===uuid)safeUrl=parsed.href;}catch(ignore){}
  var time=String(j.dispatch_utc_us||'0'),utcText='';
  if(/^\d{16}$/.test(time)){var millis=Number(time.slice(0,-3));if(Number.isFinite(millis))utcText=new Date(millis).toISOString();}
  var media=isValid?'<div class=\"muted\">UUID: '+esc(uuid)+'</div><div class=\"muted\">Software-Trigger UTC: '+esc(utcText||'unbekannt')+'</div>'+(safeUrl?'<a href=\"'+esc(safeUrl)+'\" target=\"_blank\" rel=\"noopener\">JPEG abrufen</a>':'<span class=\"muted\">Link nicht verfügbar</span>'):'–';
  return '<tr><td><strong>'+esc(r.name)+'</strong></td><td>'+esc(r.http==='error'?'HTTP ungeklärt':r.http==='accepted'?'HTTP angenommen':'Wird übertragen')+'</td><td><strong style=\"color:'+color+'\">'+esc(v[0])+'</strong><div class=\"muted\">'+esc(v[1])+'</div></td><td>'+media+'</td></tr>';
 }).join('');
 document.getElementById('clusterCaptureReportRows').innerHTML=html;
 var overall=pending?'Läuft / ausstehend':failed?'Abgeschlossen mit Fehlern':'Alle JPEGs bestätigt';
 document.getElementById('clusterCaptureReportSummary').textContent=overall+' · '+successes+'/'+report.rows.length+' erfolgreich · '+failed+' Fehler · '+pending+' ausstehend · UTC '+new Date(report.target).toISOString();
}
window.sfCaptureReportUpdate=function(d){
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
var sfCountdownTick=null,sfCountdownEnd=null;
function sfShowCaptureCountdown(target){
 if(sfCountdownTick!==null)clearInterval(sfCountdownTick);
 if(sfCountdownEnd!==null)clearTimeout(sfCountdownEnd);
 var panel=document.getElementById('clusterCaptureCountdown'),clock=document.getElementById('clusterCaptureClock'),lamp=document.getElementById('clusterCaptureLamp');
 document.getElementById('clusterCaptureTargetText').textContent='Geplanter UTC-Termin: '+new Date(target).toISOString()+' · Countdown nach geschätzter Coordinator-Clusterzeit';
 panel.style.display='flex';panel.className='sf-armed';lamp.style.background='#dc2626';
 // Countdown uses a monotonic elapsed duration so wall-clock corrections in the browser cannot jump it.
 var startPerf=performance.now(),delay=Math.max(0,target-sfCaptureUtcNow());
 function update(){var left=delay-(performance.now()-startPerf);
  if(left<=0){if(sfCountdownTick!==null){clearInterval(sfCountdownTick);sfCountdownTick=null;}
   clock.textContent='AUSLÖSEZEIT';panel.className='sf-fired';
   sfCountdownEnd=setTimeout(function(){panel.className='';clock.textContent='Termin erreicht';sfCountdownEnd=null;},2900);return;}
  var tenths=Math.ceil(left/100),secs=Math.floor(tenths/10),minutes=Math.floor(secs/60);
  clock.textContent=String(minutes).padStart(2,'0')+':'+String(secs%60).padStart(2,'0')+'.'+String(tenths%10);
 }
 update();if(panel.className==='sf-armed')sfCountdownTick=setInterval(update,50);
}
document.getElementById('clusterBulkCapture').addEventListener('click',async function(){
 var ids=Array.from(window.sfSelectedNodes),v=document.getElementById('clusterCaptureUtc').value,out=document.getElementById('clusterCaptureFeedback');
 if(!ids.length){out.textContent='Bitte Nodes auswählen.';return;}
 var seconds=Number(document.getElementById('clusterCaptureSeconds').value),minutes=Number(document.getElementById('clusterCaptureMinutes').value);
 if(!v&&(!document.getElementById('clusterCaptureSeconds').value||!document.getElementById('clusterCaptureMinutes').value)){out.textContent='Sekunden und Minuten müssen ausgefüllt sein.';return;}
 // Refresh coordinator time before computing a relative or absolute UTC job.
 // Do not silently fall back to the browser clock if the cluster time is unavailable.
 var snap;
 try{var readStart=performance.now();snap=await window.sfCoordinateStatus(true);var readEnd=performance.now();sfCaptureClock=sfCaptureClockFromStatus(snap,(readStart+readEnd)/2);}
 catch(e){out.textContent='Clusterzeit nicht abrufbar: Capture-Auftrag nicht gesendet.';return;}
 if(!sfCaptureClock){out.textContent='Coordinator hat noch keine gültige Clusterzeit: Capture-Auftrag nicht gesendet.';return;}
 var nowUtc=sfCaptureUtcNow(),target=sfPlanUtcTarget(v,seconds,minutes,nowUtc);
 if(!Number.isFinite(target)||target-nowUtc<5000||target-nowUtc>1800000){out.textContent='Zielzeit muss 5 Sekunden bis 30 Minuten in der Cluster-Zukunft liegen.';return;}
 var button=this;button.disabled=true;window.sfCoordinatorBulkActive=true;var results=[],accepted=0,shown=false;
 // Snapshot old jobs to avoid mistaking a previous photo for this capture.
 (Array.isArray((sfLatestStatus||{}).job_probes)?sfLatestStatus.job_probes:[]).forEach(function(j){if(Number(j.kind)===6)sfSeenCaptureJobs.add(sfCaptureJobKey(j));});
 var names=Array.isArray((sfLatestStatus||{}).nodes)?sfLatestStatus.nodes:[];
 sfCaptureReport={target:target,rows:ids.map(function(id){var n=names.find(function(x){return String(x.integration_id)===id;});return {id:id,name:n?(n.hostname||id):id,http:'pending',job:null};})};sfRenderCaptureReport();
 try{for(var i=0;i<ids.length;i++){
  var fd=new FormData();fd.append('node_id',ids[i]);fd.append('utc_ms',String(Math.round(target)));
  var ctrl=new AbortController(),timer=setTimeout(function(){ctrl.abort();},12000);
  try{var r=await fetch('/cluster_node_capture',{method:'POST',body:fd,credentials:'same-origin',redirect:'manual',cache:'no-store',signal:ctrl.signal});
      if(r.ok||r.status===303||r.status===0){accepted++;if(!shown){sfShowCaptureCountdown(target);shown=true;}sfCaptureReport.rows[i].http='accepted';results.push(ids[i]+': HTTP angenommen');}
      else{sfCaptureReport.rows[i].http='error';var msg='';try{msg=(await r.text()).slice(0,110);}catch(ignored){}results.push(ids[i]+': HTTP '+r.status+(msg?' '+msg:''));}}
  catch(e){sfCaptureReport.rows[i].http='error';results.push(ids[i]+': Übertragungsfehler ('+(e.name==='AbortError'?'Zeitlimit':String(e.message||e))+'); Ausführung unbekannt');}
  finally{clearTimeout(timer);sfRenderCaptureReport();}
 }
 out.textContent='UTC '+new Date(target).toISOString()+' · '+accepted+'/'+ids.length+' HTTP angenommen. '+results.join(' | ')+'. Die grüne Anzeige kennzeichnet nur den geplanten Zeitpunkt; Capture-Ergebnisse stehen in der Node-Tabelle.';
 if(accepted&&!shown)sfShowCaptureCountdown(target);
 }finally{window.sfCoordinatorBulkActive=false;button.disabled=false;sfRenderCaptureReport();window.sfCoordinateStatus().then(function(d){window.sfCaptureReportUpdate(d);}).catch(function(){});}
});
document.getElementById('clusterBulkDroneOn').addEventListener('click',function(){runBulk('/cluster_node_drone','Drone Mode EIN',false);});
document.getElementById('clusterBulkDroneOff').addEventListener('click',function(){runBulk('/cluster_node_drone','Drone Mode AUS',false);});
document.getElementById('clusterBulkProbe').addEventListener('click',function(){runBulk('/cluster_job_probe','Verbindungstest',false);});
document.getElementById('clusterBulkRestart').addEventListener('click',function(){runBulk('/cluster_node_restart','Neustart',false);});
document.getElementById('clusterBulkShutdown').addEventListener('click',function(){runBulk('/cluster_node_shutdown','Shutdown',true);});
document.getElementById('clusterBulkSync').addEventListener('click',function(){runBulk('/cluster_node_sync_time','Zeitsynchronisation',false);});
document.getElementById('clusterBulkWipe').addEventListener('click',function(){runBulk('/cluster_node_wipe','SD-Wipe',true);});
document.getElementById('clusterSdPreflight').addEventListener('click',function(){
 var out=document.getElementById('clusterSdPreflightResult'),ids=Array.from(window.sfSelectedNodes);
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
    html += R"__PROBE__(<section class='settings-section'><h3>Verbindung zu Nodes testen</h3>
<p class='muted'>Ein signierter, harmloser Probeauftrag prüft die Zustellung und die individuelle Antwort. Er startet keine Geräte neu und verändert keine Dateien.</p>
<div id='clusterProbeTargets'>Verfügbare Nodes werden geladen …</div>
<div id='clusterProbeFeedback' class='muted' role='status'></div><div id='clusterProbeSummary' class='muted'></div><div id='clusterProbeDiagnostics' class='muted'></div><div id='clusterHttpDiagnostics' class='muted'></div>
<details><summary>Letzte Probe-Ergebnisse (begrenzter RAM-Verlauf)</summary>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Node</th><th>Ergebnis</th><th>Code</th></tr></thead><tbody id='clusterProbeResults'><tr><td colspan='3'>Noch keine Probeaufträge.</td></tr></tbody></table></div>
<p class='muted'>„Bestätigt“ bedeutet nur: Der Empfänger hat den Probeauftrag authentifiziert und beantwortet. Es ist keine Bestätigung für Reboot, Löschen oder andere Aktionen.</p></details>
</section><script>
(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
var busy=false;
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
if(summary)summary.textContent='Probeaufträge seit Cluster-Runtime-Start: '+(diag.jobs_started||0)+' · gültige Probe-ACKs: '+(diag.ack_received||0)+' · aktuell im RAM-Verlauf sichtbar: '+jobs.length+' (gemeinsame Job-Tabelle mit maximal 16 Einträgen; ältere Ergebnisse können verdrängt werden).';
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
return '<tr><td><strong>'+esc(n.hostname||n.integration_id||'–')+'</strong></td><td>'+esc(status)+'</td><td>'+esc(local?clock((d.cluster_time||{}).utc_us):(valid?clock(n.time_report_utc_us):'–'))+'</td><td>'+esc(local?'Referenz':(valid?(Number(n.time_report_offset_us)===-86400000000?'nicht verfügbar':ms(n.time_report_offset_us)):'–'))+'</td><td>'+esc(local?'–':(valid?'RTT '+ms(n.time_report_rtt_us)+' · ±'+ms(n.time_report_uncertainty_us)+' (geschätzt)':'–'))+'</td><td>'+esc(local?'–':(valid?age(n.time_report_age_ms)+' her':'–'))+'</td></tr>';
}).join('')||'<tr><td colspan="6">Keine Geräte aktiv.</td></tr>';
sum.textContent=seq?('Letzter Broadcast-Sync #'+seq+': '+done+' von '+expected+' Nodes mit zugeordneter Messung. Gezielte Sync-Aufträge werden je Node in der Statusspalte mit ihrer Sequenz bestätigt.'):('Gezielte Syncs: Ergebnis je Node in der Tabelle. Noch kein manueller Broadcast seit Runtime-Start.');
}
function poll(){window.sfCoordinateStatus().then(render).catch(function(){var e=document.getElementById('clusterSyncSummary');if(e)e.textContent='Zeitstatus derzeit nicht erreichbar.';});}
poll();setInterval(poll,3000);document.addEventListener('visibilitychange',function(){if(!document.hidden)poll();});
})();
</script>)__SYNCJS__";
    html += htmlFooter();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", html);
}

static void handleClusterJobProbe()
{
    if (!clusterLocalIsCoordinator()) {
        server.send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    const String nodeId = server.arg("node_id");
    String error;
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
    if(!clusterRequestNodeCapture(server.arg("node_id"),(int64_t)ms*1000LL,error)){
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
    if (!clusterRequestNodeWipe(server.arg("node_id"),error)) {
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
    if (!clusterRequestNodeTimeSync(server.arg("node_id"), error)) {
        server.send(409, "text/plain; charset=utf-8", error); return;
    }
    server.sendHeader("Location", "/cluster_coordinate");
    server.send(303, "text/plain; charset=utf-8", "");
}

static void handleClusterSyncTime()
{
    String error;
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
    serverInstance.on("/cluster_node_sync_time", HTTP_POST, handleClusterNodeSyncTime);
    serverInstance.on("/cluster_job_probe", HTTP_POST, handleClusterJobProbe);
    serverInstance.on("/cluster_node_restart", HTTP_POST, handleClusterNodeRestart);
    serverInstance.on("/cluster_node_shutdown", HTTP_POST, handleClusterNodeShutdown);
    serverInstance.on("/cluster_node_wipe", HTTP_POST, handleClusterNodeWipe);
    serverInstance.on("/cluster_node_capture", HTTP_POST, handleClusterNodeCapture);
    serverInstance.on("/cluster_node_drone", HTTP_POST, handleClusterNodeDrone);
    serverInstance.on("/cluster_local_drone", HTTP_POST, handleClusterLocalDrone);
}
