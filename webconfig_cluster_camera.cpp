#include "webconfig_cluster_camera.h"

#include "cluster.h"
#include "cluster_camera.h"

namespace {

static WebServer *g_server = nullptr;

static void handleCameraJson()
{
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "application/json; charset=utf-8", clusterCameraJson());
}

} // namespace

String webconfigClusterCameraSectionHtml()
{
    return R"__CAMERA__(<details class='settings-section' id='sfCameras'><summary><strong>Kameras im Cluster</strong> <span id='sfCamSummary' class='muted'></span></summary>
<p class='muted'>Sensortyp, Fähigkeiten und tatsächlich wirksame Kameraeinstellungen jedes Geräts. Nur Anzeige: hier wird keine Kamera verändert. Werte stammen aus dem Treiberstatus der laufenden Kamera; ist die Kamera aus, zeigt das Gerät die Werte, mit denen es sie lokal starten würde. Die Nodes melden bei Änderung (innerhalb von ca. 2 s) und spätestens alle 30 s, nie in den 5 s vor einer geplanten Aufnahme. Spalte „Quelle“: derzeit immer „lokal“; später können Werte vom Cluster-Profil (gemeinsam) oder als Node-Override kommen.</p>
<div class='cluster-select-tools'><button type='button' id='sfCamReload'>Aktualisieren</button> <span id='sfCamState' class='muted' role='status'></span></div>
<div class='cluster-table-wrap'><table class='cluster-table' style='min-width:1500px'><thead><tr><th>Gerät</th><th>Sensor</th><th>Kamera</th><th>Auflösung</th><th>JPEG-Q</th><th>Belichtung</th><th>Verstärkung</th><th>Weißabgleich</th><th>Bild</th><th>Drehung / Spiegelung</th><th>Crop</th><th>Quelle</th><th>Stand</th></tr></thead>
<tbody id='sfCamRows'><tr><td colspan='13' class='muted'>Zum Laden öffnen.</td></tr></tbody></table></div>
<details><summary>Sensorfähigkeiten</summary><p class='muted'>„ja“ heißt: Der Kameratreiber bietet die Funktion für diesen Sensor an (bekannte Platzhalter-Funktionen, z. B. Schärfe beim OV2640, sind ausgenommen). Die tatsächliche Wirkung ist damit noch nicht gemessen. Crop ist der SensorForge-Sensor-Crop (derzeit nur OV3660). Fähigkeiten werden ab dem ersten Kamerastart nach dem Booten gemeldet.</p>
<div class='cluster-table-wrap'><table class='cluster-table' style='min-width:1100px'><thead><tr><th>Gerät</th><th>Sensor</th><th>Max. Auflösung</th><th>Feste Belichtung</th><th>Feste Verstärkung</th><th>WB-Voreinstellungen</th><th>Helligkeit/Kontrast/Sättigung</th><th>Schärfe</th><th>Rauschfilter</th><th>Spiegeln</th><th>Crop</th></tr></thead>
<tbody id='sfCamCapRows'></tbody></table></div></details>
<script>(function(){
var box=document.getElementById('sfCameras'),rows=document.getElementById('sfCamRows'),caps=document.getElementById('sfCamCapRows'),st=document.getElementById('sfCamState'),sum=document.getElementById('sfCamSummary'),timer=null;
function esc(v){return String(v==null?'':v).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function v(x){return x==null?'–':x;}
function sg(x){return x==null?'–':(x>0?'+'+x:String(x));}
var CAP={exp:1,ael:2,aec2:4,gain:8,gceil:16,awb:32,wbp:64,awbg:128,bri:256,con:512,sat:1024,sharp:2048,dn:4096,mir:8192,crop:16384};
function has(n,b){return (Number(n.cap_mask)&b)!==0;}
function yes(n,b){return n.pid==null?'–':(has(n,b)?'ja':'nein');}
var WB=['Auto','Sonne','Wolken','Büro','Glühlampe'];
window.sfCamText={
 cam:function(n){var s={on:'aktiv',standby:'Standby',off:'aus'}[n.cam_state]||'–';if(n.cam_state==='off')s+=' (lokale Startwerte)';return s+(n.drone?' · Drone':'');},
 exposure:function(n){if(n.aec==null)return '–';if(n.aec===1)return 'Auto, Korrektur '+sg(n.ae_level)+(n.aec2?' · AEC2':'');return 'Manuell, Wert '+v(n.aec_value)+' (Zeilen)';},
 gain:function(n){if(n.agc==null)return '–';if(n.agc===1)return 'Auto'+(n.gainceiling!=null?(n.gainceiling<=6?', max. '+Math.pow(2,n.gainceiling+1)+'×':', Obergrenze '+n.gainceiling):'');return 'Manuell, Stufe '+v(n.agc_gain);},
 wb:function(n){if(n.awb==null)return '–';if(n.awb===0)return 'aus'+(n.awb_gain?' · AWB-Gain':'');return (n.wb_mode!=null?WB[n.wb_mode]||('Modus '+n.wb_mode):'Auto')+(n.awb_gain===0?' · ohne AWB-Gain':'');},
 image:function(n){var p=[];if(n.brightness!=null)p.push('Hell. '+sg(n.brightness));if(n.contrast!=null)p.push('Kontr. '+sg(n.contrast));if(n.saturation!=null)p.push('Sätt. '+sg(n.saturation));if(n.sharpness!=null&&has(n,CAP.sharp))p.push('Schärfe '+sg(n.sharpness));if(n.denoise!=null&&has(n,CAP.dn))p.push('Rauschf. '+n.denoise);return p.length?p.join(' · '):'–';},
 geo:function(n){return v(n.rotation)+'° · Spiegelung H '+(n.hmirror==null?'–':(n.hmirror?'ja':'nein'))+' / V '+(n.vflip==null?'–':(n.vflip?'ja':'nein'));},
 crop:function(n){if(n.pid!=null&&!has(n,CAP.crop)&&(n.crop_zoom_x10==null||n.crop_zoom_x10===10))return 'Vollbild (Crop nicht unterstützt)';if(n.crop_zoom_x10==null||n.crop_zoom_x10===10)return 'Vollbild';return (n.crop_zoom_x10/10).toFixed(1).replace('.',',')+'× · Position '+v(n.crop_x)+'/'+v(n.crop_y);},
 source:function(n){var s=n.sources||{},u={};Object.keys(s).forEach(function(k){u[s[k]]=1;});var names={local:'lokal',cluster:'Cluster-Profil',node:'Node-Override'};var l=Object.keys(u);return l.length?l.map(function(x){return names[x]||x;}).join(' + '):'–';}
};
var T=window.sfCamText;
function render(d){if(!d.ok){rows.innerHTML='<tr><td colspan="13">Übersicht nicht verfügbar ('+esc(d.error)+').</td></tr>';caps.innerHTML='';return;}
var ns=d.nodes||[],known=ns.filter(function(n){return n.report_known;}).length;
var sensors={};ns.forEach(function(n){if(n.report_known)sensors[n.sensor]=(sensors[n.sensor]||0)+1;});
sum.textContent='· '+known+' von '+ns.length+' Geräten melden · '+Object.keys(sensors).map(function(k){return sensors[k]+'× '+k;}).join(', ');
rows.innerHTML=ns.map(function(n){var name='<strong>'+esc(n.hostname||n.integration_id)+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'');
 if(!n.report_known)return '<tr><td>'+name+'</td><td colspan="12" class="muted">Keine Kameradaten (ältere Firmware oder noch nicht gemeldet)</td></tr>';
 return '<tr><td>'+name+'</td><td>'+esc(n.sensor)+'</td><td>'+esc(T.cam(n))+'</td><td>'+esc(v(n.resolution))+'</td><td>'+esc(v(n.quality))+'</td><td>'+esc(T.exposure(n))+'</td><td>'+esc(T.gain(n))+'</td><td>'+esc(T.wb(n))+'</td><td>'+esc(T.image(n))+'</td><td>'+esc(T.geo(n))+'</td><td>'+esc(T.crop(n))+'</td><td>'+esc(T.source(n))+'</td><td>'+(n.local?'lokal':(esc(n.age_s)+' s'))+'</td></tr>';}).join('')||'<tr><td colspan="13" class="muted">Keine Geräte.</td></tr>';
caps.innerHTML=ns.filter(function(n){return n.report_known;}).map(function(n){return '<tr><td>'+esc(n.hostname||n.integration_id)+'</td><td>'+esc(n.sensor)+'</td><td>'+esc(v(n.max_resolution))+'</td><td>'+yes(n,CAP.exp)+'</td><td>'+yes(n,CAP.gain)+'</td><td>'+yes(n,CAP.wbp)+'</td><td>'+(n.pid==null?'–':[has(n,CAP.bri),has(n,CAP.con),has(n,CAP.sat)].map(function(b){return b?'ja':'nein';}).join('/'))+'</td><td>'+yes(n,CAP.sharp)+'</td><td>'+yes(n,CAP.dn)+'</td><td>'+yes(n,CAP.mir)+'</td><td>'+yes(n,CAP.crop)+'</td></tr>';}).join('');}
function load(){st.textContent='Lädt …';fetch('/cluster_cameras?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t||('HTTP '+r.status));});return r.json();}).then(function(d){render(d);st.textContent='';}).catch(function(e){st.textContent='Fehler: '+(e.message||e);});}
document.getElementById('sfCamReload').addEventListener('click',load);
box.addEventListener('toggle',function(){if(box.open){load();timer=setInterval(function(){if(!document.hidden)load();},10000);}else if(timer){clearInterval(timer);timer=null;}});
})();</script></details>
)__CAMERA__";
}

void webconfigClusterCameraRegisterRoutes(WebServer &server)
{
    g_server = &server;
    server.on("/cluster_cameras", HTTP_GET, handleCameraJson);
}
