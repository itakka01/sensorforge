#include "webconfig_cluster_time.h"

#include "cluster.h"
#include "cluster_espnow_radio.h"
#include "psram_alloc.h"
#include "webconfig_cluster.h"

namespace {

static WebServer *g_server = nullptr;

static String safeText(const char *text)
{
    String out;
    for (const char *p = text; *p; ++p)
        if (*p != '"' && *p != '\\' && *p != ',' && (uint8_t)*p >= 0x20)
            out += *p;
    return out;
}

// Online devices (mount snapshot) joined with the ESP-NOW reports of their
// current boot. Both buffers live in PSRAM for the duration of one request.
struct TimeView {
    SfPsramBuffer snapBuffer;
    SfPsramBuffer reportBuffer;
    ClusterMountEntry *snap = nullptr;
    ClusterEspNowRadio::NodeSyncReport *reports = nullptr;
    size_t snapCount = 0, reportCount = 0;
    TimeView()
        : snapBuffer(clusterMountSnapshotCapacity() * sizeof(ClusterMountEntry)),
          reportBuffer(ClusterEspNowRadio::kNodeReportCapacity *
                       sizeof(ClusterEspNowRadio::NodeSyncReport))
    {
        if (!snapBuffer || !reportBuffer)
            return;
        snap = reinterpret_cast<ClusterMountEntry *>(snapBuffer.data());
        reports = reinterpret_cast<ClusterEspNowRadio::NodeSyncReport *>(reportBuffer.data());
        snapCount = clusterMountSnapshot(snap, clusterMountSnapshotCapacity());
        reportCount = ClusterEspNowRadio::nodeSyncReports(reports, ClusterEspNowRadio::kNodeReportCapacity);
    }
    bool ok() const { return snap && reports; }
    const ClusterEspNowRadio::NodeSyncReport *reportFor(uint32_t boot) const
    {
        if (!boot)
            return nullptr;
        for (size_t i = 0; i < reportCount; ++i)
            if (reports[i].bootNonce == boot)
                return &reports[i];
        return nullptr;
    }
};

static String fixed(double v, unsigned digits) { return String(v, digits); }

// Compact JSON, about 200 bytes per node. Keys:
//   id integration id, h hostname, g group, b boot (hex), l local (coordinator)
//   r beacon report present, ra report age ms, pv precise, st precision state,
//   ds delay source, n beacon samples, j jitter us, dp drift ppm,
//   od own delay us (null = not calibrated), oa own delay age s,
//   mo monitored s, pr precise s, lo precision losses, la last loss age s
//   (null = none), mj max jitter us, ss/sr two-way series ok/rejected
static void handleTimeNodesJson()
{
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    TimeView v;
    if (!v.ok()) {
        g_server->send(503, "text/plain; charset=utf-8", "PSRAM nicht verfügbar.");
        return;
    }
    const ClusterEspNowRadio::Diagnostics d = ClusterEspNowRadio::diagnostics();
    String json;
    json.reserve(512 + v.snapCount * 220);
    json += "{\"coord\":{\"active\":";
    json += d.active ? "true" : "false";
    json += ",\"channel\":" + String((unsigned)d.channel);
    json += ",\"beacons\":" + String((unsigned long)d.beaconsSent);
    json += ",\"tx_min_us\":" + String((unsigned long)d.beaconTxPathMinUs);
    json += ",\"cluster_delay_us\":";
    json += d.clusterDelayKnown ? fixed(d.clusterDelayUs, 0) : String("null");
    json += ",\"contributors\":" + String((unsigned long)d.clusterDelayContributors);
    json += ",\"slot_cycle_ms\":" + String((unsigned long)d.slotCycleEstimateMs);
    json += ",\"slot_refresh_ms\":" + String((unsigned long)d.slotRefreshMs);
    json += "},\"nodes\":[";
    for (size_t i = 0; i < v.snapCount; ++i) {
        const ClusterMountEntry &e = v.snap[i];
        if (i)
            json += ',';
        char boot[9];
        snprintf(boot, sizeof(boot), "%08lx", (unsigned long)e.bootNonce);
        json += "{\"id\":\"" + safeText(e.integrationId) + "\",\"h\":\"" + safeText(e.hostname) +
            "\",\"g\":\"" + (e.mountKnown ? safeText(e.mount.group) : String()) +
            "\",\"b\":\"" + boot + "\"";
        if (e.local) {
            json += ",\"l\":1}";
            continue;
        }
        const ClusterEspNowRadio::NodeSyncReport *r = v.reportFor(e.bootNonce);
        if (!r || !r->hasBeaconReport) {
            json += ",\"r\":0}";
            continue;
        }
        json += ",\"r\":1,\"ra\":" + String((unsigned long)r->beaconReceivedAgeMs);
        json += ",\"pv\":" + String(r->precisionValid ? 1 : 0);
        json += ",\"st\":\"" + String(ClusterEspNowRadio::precisionStateName(r->precisionState)) + "\"";
        json += ",\"ds\":\"" + String(ClusterEspNowRadio::delaySourceName(r->delaySource)) + "\"";
        json += ",\"n\":" + String((unsigned)r->beaconSamples);
        json += ",\"j\":" + fixed(r->beaconJitterUs, 1);
        json += ",\"dp\":" + fixed(r->driftMilliPpm / 1000.0, 2);
        json += ",\"od\":";
        json += r->ownDelayKnown ? fixed(r->ownDelayUs, 0) : String("null");
        json += ",\"oa\":" + String((unsigned long)r->ownDelayAgeS);
        json += ",\"mo\":" + String((unsigned long)r->monitoredS);
        json += ",\"pr\":" + String((unsigned long)r->preciseS);
        json += ",\"lo\":" + String((unsigned long)r->precisionLosses);
        json += ",\"la\":";
        json += r->lastLossAgeS == UINT32_MAX ? String("null") : String((unsigned long)r->lastLossAgeS);
        json += ",\"mj\":" + fixed(r->maxJitterUs, 1);
        json += ",\"ss\":" + String((unsigned long)r->successfulSeries);
        json += ",\"sr\":" + String((unsigned long)r->rejectedSeries);
        json += '}';
    }
    json += "]}";
    g_server->sendHeader("Cache-Control", "no-store");
    webconfigClusterSendLargeJson(*g_server, json);
}

// Same data as CSV for long tests (one row per online device).
static void handleTimeCsv()
{
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    TimeView v;
    if (!v.ok()) {
        g_server->send(503, "text/plain; charset=utf-8", "PSRAM nicht verfügbar.");
        return;
    }
    String csv;
    csv.reserve(256 + v.snapCount * 160);
    csv += "hostname,integration_id,group,boot_id,role,report_age_ms,precise,precision_state,"
           "delay_source,own_delay_us,own_delay_age_s,jitter_us,drift_ppm,monitored_s,precise_s,"
           "precise_percent,precision_losses,last_loss_age_s,max_jitter_us,series_ok,series_rejected\n";
    for (size_t i = 0; i < v.snapCount; ++i) {
        const ClusterMountEntry &e = v.snap[i];
        char boot[9];
        snprintf(boot, sizeof(boot), "%08lx", (unsigned long)e.bootNonce);
        csv += safeText(e.hostname) + "," + safeText(e.integrationId) + "," +
            (e.mountKnown ? safeText(e.mount.group) : String()) + "," + boot + ",";
        const ClusterEspNowRadio::NodeSyncReport *r = e.local ? nullptr : v.reportFor(e.bootNonce);
        if (e.local) {
            csv += "coordinator,,,,,,,,,,,,,,,,\n";
            continue;
        }
        if (!r || !r->hasBeaconReport) {
            csv += "node,,,no_report,,,,,,,,,,,,,\n";
            continue;
        }
        csv += "node," + String((unsigned long)r->beaconReceivedAgeMs) + "," +
            String(r->precisionValid ? 1 : 0) + "," +
            ClusterEspNowRadio::precisionStateName(r->precisionState) + "," +
            ClusterEspNowRadio::delaySourceName(r->delaySource) + "," +
            (r->ownDelayKnown ? fixed(r->ownDelayUs, 1) : String()) + "," +
            String((unsigned long)r->ownDelayAgeS) + "," + fixed(r->beaconJitterUs, 1) + "," +
            fixed(r->driftMilliPpm / 1000.0, 3) + "," + String((unsigned long)r->monitoredS) + "," +
            String((unsigned long)r->preciseS) + "," +
            (r->monitoredS ? fixed(100.0 * r->preciseS / r->monitoredS, 2) : String()) + "," +
            String((unsigned long)r->precisionLosses) + "," +
            (r->lastLossAgeS == UINT32_MAX ? String() : String((unsigned long)r->lastLossAgeS)) + "," +
            fixed(r->maxJitterUs, 1) + "," + String((unsigned long)r->successfulSeries) + "," +
            String((unsigned long)r->rejectedSeries) + "\n";
    }
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->sendHeader("Content-Disposition", "attachment; filename=sensorforge_cluster_time.csv");
    g_server->send(200, "text/csv; charset=utf-8", csv);
}

} // namespace

String webconfigClusterTimeSectionHtml()
{
    return R"__TIME__(<details class='settings-section' id='sfTimeBox'><summary><strong>Zeitsynchronisation</strong> <span id='sfTimeSummary' class='muted'></span></summary>
<p class='muted'>Jeder Node führt seine Uhr anhand der Zeitsignale des Coordinators (etwa eins pro Sekunde). Die Funklaufzeit misst jeder Node alle 10 Minuten per Hin-und-zurück-Messung ein; bis zur ersten eigenen Einmessung gilt der Cluster-Mittelwert. Die Nodes melden ihren Zustand alle 10 s an den Coordinator. Diese Ansicht verursacht keinen zusätzlichen Funkverkehr. Alle Werte sind Software-Zeitstempel; die echte Auslösegenauigkeit zeigt nur die GPIO-Prüfung unten.</p>
<div id='sfTimeCoord' class='muted' role='status'>Wird mit „Aktualisieren“ in der Geräteliste geladen.</div>
<p class='muted'>Langzeitwerte gelten seit dem letzten Start des Nodes bzw. des Coordinators. „Präzise“ zählt die Zeit, in der der Node präzise Zeit gemeldet hat; bleibt seine Meldung länger als 30 s aus, zählt das als Ausfall. <a href='/cluster_time_csv' download='sensorforge_cluster_time.csv'>Langzeitwerte als CSV herunterladen</a></p>
<div class='cluster-table-wrap'><table class='cluster-table'><thead><tr><th>Gerät</th><th>Zustand</th><th>Streuung</th><th>Gangabweichung</th><th>Funklaufzeit</th><th>Präzise seit Start</th><th>Ausfälle</th><th>Größte Streuung</th><th>Einmessungen ok / verworfen</th><th>Meldung</th></tr></thead>
<tbody id='sfTimeRows'><tr><td colspan='10' class='muted'>Noch nicht geladen.</td></tr></tbody></table></div>
<details><summary>Genauigkeit mit GPIO prüfen</summary>
<ol class='muted'>
<li>Freenove-Board als Messgerät verdrahten (Anleitung auf dessen Seite <code>/cluster</code>, Abschnitt „GPIO-Messgerät (passiv)“). Eingänge: GPIO14, GPIO21, optional GPIO41; Quellen: D1/GPIO2 der XIAO-Nodes; gemeinsame Masse.</li>
<li>Am Freenove „Messung starten“.</li>
<li>Hier in der Geräteliste die verdrahteten Nodes auswählen, unter „Foto in“ das Häkchen „GPIO-Testimpuls (D1)“ setzen und eine Aufnahme planen. Mehrmals wiederholen.</li>
<li>Die Differenzen zeigt das Messgerät an (auch als CSV).</li>
</ol></details>
</details>
<script>(function(){
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function dur(s){s=Number(s)||0;if(s<120)return Math.round(s)+' s';if(s<7200)return Math.round(s/60)+' min';if(s<172800)return (s/3600).toFixed(1)+' h';return (s/86400).toFixed(1)+' d';}
function num(v,u,d){return v==null||!Number.isFinite(Number(v))?'–':Number(v).toFixed(d||0)+' '+u;}
var STATE={valid:'präzise',inactive:'ESP-NOW inaktiv (Drone Mode aus?)',no_beacons:'kein Zeitsignal',too_few_samples:'startet (sammelt Zeitsignale)',span_too_short:'startet',jitter_too_high:'Zeitsignal zu unruhig',drift_out_of_range:'Gangabweichung unplausibel',stale:'Zeitsignal ausgeblieben',no_delay:'Laufzeit unbekannt',no_utc:'Coordinator ohne UTC'};
var byId={};
// Severity: 0 ok, 1 hint, 2 starting, 3 problem, 4 offline/unknown.
function info(id,online){
 var n=byId[String(id||'')];
 if(online===false)return {sev:4,cls:'danger',text:'offline',title:'Gerät nicht erreichbar'};
 if(!n)return {sev:4,cls:'danger',text:'unbekannt',title:'Keine Zeitdaten – „Aktualisieren“ drücken'};
 if(n.l)return {sev:0,cls:'ok',text:'Zeitquelle',title:'Coordinator'};
 if(!n.r)return {sev:3,cls:'danger',text:'keine Meldung',title:'Noch kein Zeitbericht. Drone Mode und ESP-NOW prüfen.'};
 if(n.ra>30000)return {sev:3,cls:'danger',text:'Meldung fehlt',title:'Letzter Zeitbericht vor '+dur(n.ra/1000)};
 var recent=n.la!=null&&n.la<3600,loss=recent?' · Ausfall vor '+dur(n.la):'';
 if(!n.pv){var st=n.st==='too_few_samples'||n.st==='span_too_short';return {sev:st?2:3,cls:st?'warn':'danger',text:st?'startet':'nicht präzise',title:(STATE[n.st]||n.st)};}
 if(n.ds!=='own')return {sev:recent?2:1,cls:'warn',text:'präzise (Cluster-Wert)'+loss,title:'Funklaufzeit noch nicht selbst eingemessen'};
 if(recent)return {sev:2,cls:'warn',text:'präzise'+loss,title:n.lo+' Ausfall/Ausfälle seit Start'};
 return {sev:0,cls:'ok',text:'präzise',title:'Streuung '+num(n.j,'µs',0)};
}
window.sfTimeInfo=info;
window.sfTimePill=function(id,online){var i=info(id,online);return '<span class="status-pill '+i.cls+'" title="'+esc(i.title)+'">'+esc(i.text)+'</span>';};
function render(d){
 var c=d.coord||{},nodes=Array.isArray(d.nodes)?d.nodes:[];
 byId={};nodes.forEach(function(n){byId[String(n.id)]=n;});
 var el=document.getElementById('sfTimeCoord');
 if(el)el.textContent='Coordinator: Funk '+(c.active?'aktiv':'inaktiv')+' · Kanal '+(c.channel||'–')+' · Zeitsignale gesendet '+(c.beacons||0)+' · kürzester Sendeweg '+(c.tx_min_us||0)+' µs · Cluster-Funklaufzeit '+(c.cluster_delay_us==null?'noch keine':c.cluster_delay_us+' µs aus '+c.contributors+' Node(s)')+' · Einmess-Runde '+(c.slot_cycle_ms?dur(c.slot_cycle_ms/1000):'–')+' von '+dur((c.slot_refresh_ms||600000)/1000);
 var list=nodes.filter(function(n){return !n.l;}).map(function(n){return {n:n,i:info(n.id,true)};});
 list.sort(function(a,b){return b.i.sev-a.i.sev||String(a.n.h).localeCompare(String(b.n.h));});
 var bad=list.filter(function(x){return x.i.sev>=2;}).length;
 var sum=document.getElementById('sfTimeSummary');if(sum)sum.textContent=list.length?(bad?bad+' von '+list.length+' Nodes mit Hinweis':'alle '+list.length+' Nodes präzise'):'';
 var rows=document.getElementById('sfTimeRows');if(!rows)return;
 rows.innerHTML=list.length?list.map(function(x){var n=x.n,has=!!n.r;
  return '<tr id="sfTime-'+esc(n.id)+'"><td><strong>'+esc(n.h||n.id)+'</strong></td><td><span class="status-pill '+x.i.cls+'">'+esc(x.i.text)+'</span>'+(has&&!n.pv?'<div class="muted">'+esc(STATE[n.st]||n.st)+'</div>':'')+'</td>'+
  '<td>'+(has?num(n.j,'µs',0):'–')+'</td><td>'+(has?num(n.dp,'ppm',1):'–')+'</td>'+
  '<td>'+(has?(n.od!=null?num(n.od,'µs',0)+'<div class="muted">eigen, vor '+dur(n.oa)+'</div>':'Cluster-Wert'):'–')+'</td>'+
  '<td>'+(has&&n.mo?(100*n.pr/n.mo).toFixed(n.pr<n.mo?2:0)+' %<div class="muted">von '+dur(n.mo)+'</div>':'–')+'</td>'+
  '<td>'+(has?String(n.lo)+(n.la!=null?'<div class="muted">zuletzt vor '+dur(n.la)+'</div>':''):'–')+'</td>'+
  '<td>'+(has?num(n.mj,'µs',0):'–')+'</td><td>'+(has?n.ss+' / '+n.sr:'–')+'</td>'+
  '<td>'+(has?'vor '+dur(n.ra/1000):'keine')+'</td></tr>';}).join(''):'<tr><td colspan="10" class="muted">Keine Nodes online.</td></tr>';
}
// A click on the time pill in the device list opens this section.
document.addEventListener('click',function(e){if(e.target.closest&&e.target.closest('.sf-time-link')){var b=document.getElementById('sfTimeBox');if(b)b.open=true;}});
window.sfTimeLoad=function(){return fetch('/cluster_time_nodes?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw Error('HTTP '+r.status);return r.json();}).then(function(d){render(d);return d;});};
})();</script>
)__TIME__";
}

void webconfigClusterTimeRegisterRoutes(WebServer &server)
{
    g_server = &server;
    server.on("/cluster_time_nodes", HTTP_GET, handleTimeNodesJson);
    server.on("/cluster_time_csv", HTTP_GET, handleTimeCsv);
}
