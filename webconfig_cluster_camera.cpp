#include "webconfig_cluster_camera.h"

#include "cluster.h"
#include "cluster_camera.h"
#include "cluster_camera_profile.h"
#include "webconfig_cluster.h"

namespace {

static WebServer *g_server = nullptr;

static void handleCameraJson()
{
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    g_server->sendHeader("Cache-Control", "no-store");
    webconfigClusterSendLargeJson(*g_server, clusterCameraJson());
}

static void handleCameraProfileGet()
{
    if (!clusterLocalIsCoordinator()) {
        g_server->send(403, "text/plain; charset=utf-8", "Nur am aktiven Coordinator verfügbar.");
        return;
    }
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(200, "application/json; charset=utf-8", clusterCameraProfileJson());
}

static void handleCameraProfilePost()
{
    String json;
    const int code = clusterCameraProfileRequestFromArgs(*g_server, json);
    g_server->sendHeader("Cache-Control", "no-store");
    g_server->send(code, "application/json; charset=utf-8", json);
}

} // namespace

String webconfigClusterCameraSectionHtml()
{
    return R"__CAMERA__(<details class='settings-section' id='sfCameras'><summary><strong>Kameras im Cluster</strong> <span id='sfCamSummary' class='muted'></span></summary>
<p class='muted'>Sensortyp, Fähigkeiten und tatsächlich wirksame Kameraeinstellungen jedes Geräts.  Werte stammen aus dem Treiberstatus der laufenden Kamera; ist die Kamera aus, zeigt das Gerät die Werte, mit denen es sie lokal starten würde. Die Nodes melden bei Änderung (innerhalb von ca. 2 s) und spätestens alle 30 s, nie in den 5 s vor einer geplanten Aufnahme. Alle Einstellungen eines Geräts unter „Details“. „Quelle“ ist derzeit immer „lokal“; später können Werte vom Cluster-Profil (gemeinsam) oder als Node-Override kommen.</p>
<div class='cluster-select-tools'><button type='button' id='sfCamReload'>Aktualisieren</button> <span id='sfCamState' class='muted' role='status'></span></div>
<h4 style='margin:12px 0 4px'>Kameraprofil je Sensortyp</h4>
<p class='muted'>Mit ♡ in der Tabelle wählst du je Sensortyp eine Referenzkamera. Ihre aktuellen Einstellungen (Auflösung, JPEG-Qualität, Belichtung, Verstärkung, Weißabgleich, Helligkeit/Kontrast/Sättigung) werden als Profil an alle Geräte mit gleichem Sensor übertragen – „nur Sitzung“ (bis zum Neustart) oder „dauerhaft“ (eigener Profilspeicher, nicht config.txt). Das Profil wirkt nur im Drone Mode; Drehung und Spiegelung bleiben je Gerät, ein Bildausschnitt (Crop) ruht. „Profil entfernen“ stellt die lokalen Werte wieder her. Geräte mit anderem Sensor erhalten das Profil nicht.</p>
<div id='sfCamProfiles'></div><p id='sfCamProfState' class='muted' role='status'></p>
<div class='cluster-table-wrap'><table class='cluster-table' style='min-width:0'><thead><tr><th>Gerät</th><th>Sensor</th><th>Kamera</th><th>Auflösung</th><th>Belichtung / Verstärkung</th><th>Profil</th><th>Stand</th><th></th></tr></thead>
<tbody id='sfCamRows'><tr><td colspan='8' class='muted'>Zum Laden öffnen.</td></tr></tbody></table></div>
<dialog id='sfCamDialog' style='width:min(94vw,560px);box-sizing:border-box;padding:16px;border-radius:12px'><div style='display:flex;justify-content:space-between;align-items:center;gap:12px'><h3 id='sfCamDlgTitle' style='margin:0'></h3><button type='button' id='sfCamDlgClose'>Schließen</button></div><table class='cluster-table' style='min-width:0;margin-top:10px'><tbody id='sfCamDlgRows'></tbody></table></dialog>
<h4 style='margin:16px 0 4px'>Kamera-Übersicht</h4>
<p class='muted'>Aktuelle Bilder aller Kameras als Kleinbild (ca. 1/8 der Auflösung, je ca. 5–15 KB), mit den echten Einstellungen. Der Browser holt sie direkt von den Geräten, eines nach dem anderen; bei „laufend“ beginnt nach jeder Runde nach 2 s die nächste. Auch im Drone Mode; während einer geplanten Cluster-Aufnahme liefern die Geräte kein Bild. „Abstimmen“ öffnet die Bildabstimmung des Geräts.</p>
<div class='cluster-select-tools'><button type='button' id='sfCamOvStart'>Übersicht starten</button> <label><input type='checkbox' id='sfCamOvLive' checked> laufend auffrischen</label> <span id='sfCamOvState' class='muted' role='status'></span></div>
<div id='sfCamOvGrid' style='display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px'></div>
<details><summary>Sensorfähigkeiten</summary><p class='muted'>„ja“ heißt: Der Kameratreiber bietet die Funktion für diesen Sensor an (bekannte Platzhalter-Funktionen, z. B. Schärfe beim OV2640, sind ausgenommen). Die tatsächliche Wirkung ist damit noch nicht gemessen. Crop ist der SensorForge-Sensor-Crop (derzeit nur OV3660). Fähigkeiten werden ab dem ersten Kamerastart nach dem Booten gemeldet.</p>
<div class='cluster-table-wrap'><table class='cluster-table' style='min-width:0'><thead><tr><th>Gerät</th><th>Sensor</th><th>Max. Auflösung</th><th>Unterstützt</th><th>Nicht unterstützt</th></tr></thead>
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
var FEAT=[['Feste Belichtung',CAP.exp],['Feste Verstärkung',CAP.gain],['WB-Voreinstellungen',CAP.wbp],['Helligkeit',CAP.bri],['Kontrast',CAP.con],['Sättigung',CAP.sat],['Schärfe',CAP.sharp],['Rauschfilter',CAP.dn],['Spiegeln',CAP.mir],['Crop',CAP.crop]];
function feats(n,want){return FEAT.filter(function(f){return has(n,f[1])===want;}).map(function(f){return f[0];}).join(', ')||'–';}
function name(n){return n.hostname||n.integration_id||'';}
var last=[];
function detail(i){var n=last[i];if(!n)return;
 document.getElementById('sfCamDlgTitle').textContent=name(n)+(n.sensor?' · '+n.sensor:'');
 var r=[['Kamera',T.cam(n)],['Auflösung',v(n.resolution)],['JPEG-Qualität',v(n.quality)],['Belichtung',T.exposure(n)],['Verstärkung',T.gain(n)],['Weißabgleich',T.wb(n)],['Bild',T.image(n)],['Drehung / Spiegelung',T.geo(n)],['Crop',T.crop(n)],['Quelle',T.source(n)],['Stand',n.local?'lokal':(v(n.age_s)+' s alt')],['Max. Auflösung',v(n.max_resolution)]];
 if(n.pid!=null){r.push(['Unterstützt',feats(n,true)]);r.push(['Nicht unterstützt',feats(n,false)]);}
 document.getElementById('sfCamDlgRows').innerHTML=r.map(function(x){return '<tr><th style="text-align:left;white-space:nowrap">'+esc(x[0])+'</th><td>'+esc(x[1])+'</td></tr>';}).join('');
 var dl=document.getElementById('sfCamDialog');if(dl.showModal)dl.showModal();else dl.setAttribute('open','');}
document.getElementById('sfCamDlgClose').addEventListener('click',function(){var dl=document.getElementById('sfCamDialog');if(dl.close)dl.close();else dl.removeAttribute('open');});
rows.addEventListener('click',function(e){var b=e.target.closest('[data-cam]');if(b)detail(Number(b.getAttribute('data-cam')));});
// Profile state of a node against the current profile of its sensor type.
var prof={sensors:[]},profBox=document.getElementById('sfCamProfiles'),profSt=document.getElementById('sfCamProfState');
function sensorOf(pid){var h=pid==null?'':Number(pid).toString(16);return (prof.sensors||[]).find(function(x){return x.pid===h;})||null;}
var RES={applied:'übernommen',waiting_drone:'gespeichert, wirkt im Drone Mode',sensor_mismatch:'anderer Sensor',invalid:'ungültig',busy:'abgelehnt: Aufnahme geplant',save_failed:'Speichern fehlgeschlagen (nur Sitzung aktiv)',camera_failed:'Kamera-Neustart fehlgeschlagen',cleared:'entfernt'};
function diffs(n,p){var d=[];function c(label,a,b){if(a!=null&&b!=null&&Number(a)!==Number(b))d.push(label);}
 if(n.resolution&&n.resolution!==p.resolution)d.push('Auflösung');c('Qualität',n.quality,p.quality);c('Belichtungsautomatik',n.aec,p.aec);
 if(p.aec===1)c('AE-Korrektur',n.ae_level,p.ae_level);else c('Belichtung',n.aec_value,p.aec_value);
 c('Verstärkungsautomatik',n.agc,p.agc);if(p.agc===0)c('Verstärkung',n.agc_gain,p.agc_gain);
 c('Weißabgleich',n.awb,p.awb);c('WB-Voreinstellung',n.wb_mode,p.wb_mode);c('AWB-Gain',n.awb_gain,p.awb_gain);
 c('Helligkeit',n.brightness,p.brightness);c('Kontrast',n.contrast,p.contrast);c('Sättigung',n.saturation,p.saturation);return d;}
function profCell(n){var s=sensorOf(n.pid),p=s&&s.profile,ref=s&&s.reference===n.integration_id;
 var heart='<button type="button" data-ref="'+esc(n.integration_id)+'" title="'+(ref?'Referenzkamera für '+esc(n.sensor):'Als Referenz für '+esc(n.sensor)+' wählen')+'" style="padding:0 6px">'+(ref?'♥':'♡')+'</button> ';
 if(n.pid==null)return '<span class="muted">Sensor unbekannt</span>';
 var txt,col='';var st=n.profile_state||'none',v=n.profile_version||null;
 if(!p){txt=v?'Profil '+v+' (am Coordinator entfernt)':'kein Profil';col=v?'#b45309':'';}
 else if(st==='sensor_mismatch'){txt='anderer Sensor';col='#dc2626';}
 else if(v!==p.version){txt=v?'altes Profil '+v:'kein Profil';col='#dc2626';}
 else if(st==='waiting_drone'){txt='Profil '+v+' · wartet auf Drone Mode';col='#b45309';}
 else if(st==='pending'){txt='Profil '+v+' · Kamera-Neustart ausstehend';col='#b45309';}
 else{var d=n.from_sensor?diffs(n,p):[];txt=d.length?'weicht ab: '+d.join(', '):'✓ Profil '+v;col=d.length?'#dc2626':'#16a34a';}
 var r=n.profile_result&&n.profile_result!=='applied'&&n.profile_result!=='cleared'?'<div class="muted">Letzte Antwort: '+esc(RES[n.profile_result]||n.profile_result)+' ('+esc(n.profile_result_age_s)+' s)</div>':'';
 return heart+'<span style="color:'+(col||'inherit')+'">'+esc(txt)+'</span>'+r;}
function renderProfiles(){if(!prof.sensors||!prof.sensors.length){profBox.innerHTML='<p class="muted">Noch keine Sensortypen bekannt.</p>';return;}
 profBox.innerHTML=prof.sensors.map(function(s){var refNode=last.find(function(n){return n.integration_id===s.reference;});var p=s.profile;
  var count=last.filter(function(n){return n.pid!=null&&Number(n.pid).toString(16)===s.pid;}).length;
  var summary=p?('Profil '+p.version+': '+p.resolution+' · Q '+p.quality+' · Belichtung '+(p.aec?'Auto':p.aec_value+' Zeilen')+' · Verstärkung '+(p.agc?'Auto':p.agc_gain)+' · WB '+(p.awb?(WB[p.wb_mode]||p.wb_mode):'aus')):'Noch kein Profil übertragen.';
  return '<div style="border:1px solid #64748b66;border-radius:10px;padding:8px;margin:6px 0"><strong>'+esc(s.sensor)+'</strong> <span class="muted">('+count+' Geräte)</span><br>Referenz: '+(s.reference?esc(refNode?name(refNode):s.reference):'<em>keine – ♡ in der Tabelle wählen</em>')+'<br><span class="muted">'+esc(summary)+'</span><div style="margin-top:6px"><button type="button" data-dist="session" data-pid="'+esc(s.pid)+'"'+(s.reference?'':' disabled')+'>Übertragen – nur Sitzung</button> <button type="button" data-dist="persistent" data-pid="'+esc(s.pid)+'"'+(s.reference?'':' disabled')+'>Übertragen – dauerhaft</button> <button type="button" data-dist="clear" data-pid="'+esc(s.pid)+'">Profil entfernen</button></div></div>';}).join('');}
function profPost(body){profSt.textContent='Wird gesendet …';return fetch('/cluster_camera_profile',{method:'POST',body:new URLSearchParams(body),credentials:'same-origin',cache:'no-store'}).then(function(r){return r.json().catch(function(){return {ok:false,message:'HTTP '+r.status};});});}
profBox.addEventListener('click',function(e){var b=e.target.closest('[data-dist]');if(!b)return;var m=b.getAttribute('data-dist'),pid=b.getAttribute('data-pid');
 if(m==='clear'&&!confirm('Profil für diesen Sensortyp auf allen Geräten entfernen? Die Geräte nutzen wieder ihre lokalen Werte.'))return;
 if(m==='persistent'&&!confirm('Profil dauerhaft auf allen Geräten mit diesem Sensor speichern?'))return;
 profPost(m==='clear'?{action:'clear',pid:pid}:{action:'distribute',pid:pid,mode:m}).then(function(j){
  profSt.textContent=j.ok?('Profil '+(j.version||'')+' an '+j.sent+' Gerät(e) gesendet'+(j.failed?', '+j.failed+' nicht erreichbar ('+j.errors+')':'')+(j.other_sensor?' · '+j.other_sensor+' mit anderem Sensor ausgelassen':'')+(j.unknown_sensor?' · '+j.unknown_sensor+' ohne Sensorangabe':'')+'. Die Geräte melden den neuen Stand innerhalb weniger Sekunden.'):('Fehler: '+(j.message||j.error));
  setTimeout(load,3000);setTimeout(load,8000);}).catch(function(e){profSt.textContent='Fehler: '+(e.message||e);});});
rows.addEventListener('click',function(e){var b=e.target.closest('[data-ref]');if(!b)return;profPost({action:'reference',node_id:b.getAttribute('data-ref')}).then(function(j){profSt.textContent=j.ok?'Referenz für '+j.sensor+' gesetzt.':'Fehler: '+(j.message||j.error);load();});});
function render(d){if(!d.ok){rows.innerHTML='<tr><td colspan="8">Übersicht nicht verfügbar ('+esc(d.error)+').</td></tr>';caps.innerHTML='';return;}
var ns=(d.nodes||[]).slice().sort(function(a,b){return name(a).localeCompare(name(b),'de',{numeric:true,sensitivity:'base'});});last=ns;
var known=ns.filter(function(n){return n.report_known;}).length;
var sensors={};ns.forEach(function(n){if(n.report_known)sensors[n.sensor]=(sensors[n.sensor]||0)+1;});
sum.textContent='· '+known+' von '+ns.length+' Geräten melden · '+Object.keys(sensors).map(function(k){return sensors[k]+'× '+k;}).join(', ');
rows.innerHTML=ns.map(function(n,i){var nm='<strong>'+esc(name(n))+'</strong>'+(n.local?' <span class="status-pill ok">LOKAL</span>':'');
 if(!n.report_known)return '<tr><td>'+nm+'</td><td colspan="7" class="muted">Keine Kameradaten (ältere Firmware oder noch nicht gemeldet)</td></tr>';
 var res=v(n.resolution)+(n.quality!=null?' · Q '+n.quality:'');
 var exp=n.aec==null?'–':(n.aec===1?'Auto':'Manuell')+' / '+(n.agc==null?'–':(n.agc===1?'Auto':'Manuell'));
 return '<tr><td>'+nm+'</td><td>'+esc(n.sensor)+'</td><td>'+esc(T.cam(n))+'</td><td>'+esc(res)+'</td><td title="Belichtung / Verstärkung">'+esc(exp)+'</td><td>'+profCell(n)+'</td><td>'+(n.local?'lokal':(esc(n.age_s)+' s'))+'</td><td><button type="button" data-cam="'+i+'">Details</button></td></tr>';}).join('')||'<tr><td colspan="8" class="muted">Keine Geräte.</td></tr>';
renderProfiles();
caps.innerHTML=ns.filter(function(n){return n.report_known;}).map(function(n){return '<tr><td>'+esc(name(n))+'</td><td>'+esc(n.sensor)+'</td><td>'+esc(v(n.max_resolution))+'</td><td>'+(n.pid==null?'–':esc(feats(n,true)))+'</td><td>'+(n.pid==null?'–':esc(feats(n,false)))+'</td></tr>';}).join('');}
function getJson(u){return fetch(u+'?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)return r.text().then(function(t){throw Error(t||('HTTP '+r.status));});return r.json();});}
function load(){st.textContent='Lädt …';Promise.all([getJson('/cluster_cameras'),getJson('/cluster_camera_profile').catch(function(){return {sensors:[]};})]).then(function(r){prof=r[1]||{sensors:[]};render(r[0]);st.textContent='';}).catch(function(e){st.textContent='Fehler: '+(e.message||e);});}
document.getElementById('sfCamReload').addEventListener('click',load);
box.addEventListener('toggle',function(){if(box.open){load();timer=setInterval(function(){if(!document.hidden)load();},10000);}else{if(timer){clearInterval(timer);timer=null;}ovStop();}});
// Camera overview: one thumbnail request at a time, straight from each device.
var ov={on:false,list:[],idx:0,timer:null},ovBtn=document.getElementById('sfCamOvStart'),ovLive=document.getElementById('sfCamOvLive'),ovSt=document.getElementById('sfCamOvState'),ovGrid=document.getElementById('sfCamOvGrid');
function ovBase(n,ips){if(n.local)return '';var ip=String(ips[n.integration_id]||''),p=ip.split('.');if(p.length!==4||!p.every(function(x){return /^\d{1,3}$/.test(x)&&Number(x)<=255;})||ip==='0.0.0.0')return null;return 'http://'+ip;}
function ovStop(){ov.on=false;if(ov.timer){clearTimeout(ov.timer);ov.timer=null;}ovBtn.textContent='Übersicht starten';}
function ovNext(){if(!ov.on)return;
 if(ov.idx>=ov.list.length){ov.idx=0;ovSt.textContent='Runde fertig '+new Date().toLocaleTimeString('de-DE');if(!ovLive.checked){ovStop();return;}ov.timer=setTimeout(ovNext,2000);return;}
 if(document.hidden){ov.timer=setTimeout(ovNext,1000);return;}
 var e=ov.list[ov.idx++],im=new Image(),done=false;
 function fin(ok){if(done)return;done=true;clearTimeout(t);
  if(ok){im.alt='Kamerabild '+e.name;im.style.cssText='display:block;width:100%;aspect-ratio:4/3;object-fit:contain;background:#1f2937;border-radius:8px';e.pic.replaceWith(im);e.pic=im;e.st.textContent=new Date().toLocaleTimeString('de-DE');}
  else{e.st.textContent='kein Bild (belegt, aus oder nicht erreichbar)';im.src='';}
  ovNext();}
 var t=setTimeout(function(){fin(false);},8000);
 im.onload=function(){fin(true);};im.onerror=function(){fin(false);};
 im.src=e.base+'/camera_frame?t='+Date.now();}
function ovStart(){if(ov.on){ovStop();return;}ovSt.textContent='Lädt Geräteliste …';
 Promise.all([fetch('/cluster_cameras?t='+Date.now(),{cache:'no-store',credentials:'same-origin'}).then(function(r){if(!r.ok)throw Error('HTTP '+r.status);return r.json();}),window.sfCoordinateStatus(true)]).then(function(r){
  var ips={};((r[1]||{}).nodes||[]).forEach(function(n){ips[n.integration_id]=n.ip;});
  var ns=((r[0]||{}).nodes||[]).slice().sort(function(a,b){return name(a).localeCompare(name(b),'de',{numeric:true,sensitivity:'base'});});
  ovGrid.innerHTML='';ov.list=[];
  ns.forEach(function(n){var base=ovBase(n,ips);var card=document.createElement('article');card.style.cssText='border:1px solid #64748b66;border-radius:10px;padding:8px;min-width:0';
   card.innerHTML='<strong>'+esc(name(n))+'</strong> <span class="muted">'+esc(n.sensor||'')+'</span><div class="sf-ov-pic" style="width:100%;aspect-ratio:4/3;background:#1f2937;border-radius:8px;margin:6px 0"></div><div class="muted sf-ov-st"></div>'+(base===null?'':'<div><a target="_blank" rel="noopener noreferrer" href="'+esc(base)+'/camera_frame?size=full">Groß</a> · <a target="_blank" rel="noopener noreferrer" href="'+esc(base)+'/camera_tuning">Abstimmen</a></div>');
   ovGrid.appendChild(card);var st=card.querySelector('.sf-ov-st');
   if(base===null){st.textContent='keine IP-Adresse bekannt';return;}
   ov.list.push({name:name(n),base:base,pic:card.querySelector('.sf-ov-pic'),st:st});});
  if(!ov.list.length){ovSt.textContent='Keine erreichbaren Geräte.';return;}
  ov.on=true;ov.idx=0;ovBtn.textContent='Übersicht stoppen';ovSt.textContent='';ovNext();
 }).catch(function(e){ovSt.textContent='Fehler: '+(e.message||e);});}
ovBtn.addEventListener('click',ovStart);
})();</script></details>
)__CAMERA__";
}

void webconfigClusterCameraRegisterRoutes(WebServer &server)
{
    g_server = &server;
    server.on("/cluster_cameras", HTTP_GET, handleCameraJson);
    server.on("/cluster_camera_profile", HTTP_GET, handleCameraProfileGet);
    server.on("/cluster_camera_profile", HTTP_POST, handleCameraProfilePost);
}
