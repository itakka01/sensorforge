#!/usr/bin/env python3
"""SensorForge stability test manager v1: read-only, Python 3.10+, stdlib.

Uses beta134 /cluster_status; no restart, flash or write requests.
"""
from __future__ import annotations
import argparse, datetime as dt, json, pathlib, signal, socket, sqlite3, sys, time, urllib.parse, threading, html
from collections import defaultdict
import sensorforge_monitor as legacy

HOSTS = ['devfreenove1.local','devxiao1.local','devxiao2.local','devxiao3.local']
EXTRA_KEYS = ('active_time_source','source_changes','drift_valid','drift_ppm','drift_uncertainty_ppm',
              'drift_samples','drift_persisted','stored_drift_available','last_exchange_valid',
              'last_offset_us','last_rtt_us','precision_clock_active','precision_sample_age_ms',
              'probes_answered','broadcaster_lease_remaining_ms','broadcaster_time_base_invalid',
              'precision_guard_epoch_mismatch','precision_guard_drift','precision_guard_estimate_age',
              'precision_guard_tag_mismatch','last_series_estimate_age_ms')
STOP=False
STOP_EVENT=threading.Event()

def stop(*_):
    global STOP
    STOP=True
    STOP_EVENT.set()

def utc(): return dt.datetime.now(dt.timezone.utc).isoformat(timespec='milliseconds')

def cfg_read(path):
    cfg=json.loads(pathlib.Path(path).read_text(encoding='utf8'))
    cfg.setdefault('expected_hosts',HOSTS)
    cfg.setdefault('interval_seconds',10)
    cfg.setdefault('timeout_seconds',5)
    cfg.setdefault('stalled_probe_seconds',120)
    cfg.setdefault('fallback_ips',{})
    return cfg

def init_database(path):
    db=sqlite3.connect(path)
    db.execute('PRAGMA journal_mode=WAL')
    db.execute('CREATE TABLE IF NOT EXISTS observations(id INTEGER PRIMARY KEY, run_id TEXT, ts TEXT, host TEXT, candidate TEXT, resolved_ip TEXT, http_ok INTEGER, error TEXT, latency_ms REAL, payload TEXT)')
    db.execute('CREATE INDEX IF NOT EXISTS obs_run_host_ts ON observations(run_id,host,ts)')
    db.execute('CREATE TABLE IF NOT EXISTS runs(run_id TEXT PRIMARY KEY, start_utc TEXT, end_utc TEXT, requested_seconds REAL, interval_seconds REAL, expected_hosts TEXT)')
    db.execute('CREATE TABLE IF NOT EXISTS network_checks(run_id TEXT, ts TEXT, host TEXT, dns_ok INTEGER, dns_ips TEXT, fallback_used INTEGER, http_ok INTEGER, route TEXT, detail TEXT)')
    db.commit();return db

def resolve(host):
    try:
        return sorted({x[4][0] for x in socket.getaddrinfo(host,None,type=socket.SOCK_STREAM)})
    except OSError:
        return []

def call(host, ip, cfg):
    candidate='http://'+(ip or host)
    j,err,ms,n=legacy.fetch(candidate,cfg)
    return j,err,ms,n,candidate

def normalize_host(value):
    return str(value or '').lower().split('.')[0]

def identify(j, host):
    if not isinstance(j, dict): return False
    local=next((x for x in j.get('nodes',[]) if isinstance(x,dict) and x.get('local')),None)
    return bool(local and normalize_host(local.get('hostname'))==normalize_host(host))

def update_known_ips(payload, expected, known):
    if not isinstance(payload,dict):return
    for node in payload.get('nodes',[]):
        if not isinstance(node,dict):continue
        ip=node.get('ip'); hn=node.get('hostname')
        if hn and ip and any(normalize_host(hn)==normalize_host(h) for h in expected):
            try: socket.inet_aton(ip)
            except (OSError,TypeError):continue
            known[next(h for h in expected if normalize_host(h)==normalize_host(hn))]=ip

def poll(host,cfg,known):
    ips=resolve(host)
    fallback=cfg.get('fallback_ips',{}).get(host) or known.get(host)
    candidates=[('dns',ip) for ip in ips]
    if fallback and fallback not in ips:candidates.append(('fallback',fallback))
    if not candidates: candidates=[('hostname',None)]
    attempts=[];success=None; route='none'
    for method,ip in candidates:
        j,err,ms,_,url=call(host,ip,cfg)
        valid=bool(j and j.get('ok') and identify(j,host))
        if j and j.get('ok') and not valid:err='IDENTITY_MISMATCH: unexpected local hostname'
        attempts.append({'candidate':url,'resolved_ip':ip,'http_ok':valid,
                         'error':err or ('' if valid else 'API ok=false'),
                         'latency_ms':ms,'payload':j if valid else None})
        if valid:
            success=j;route=method
            if ip:known[host]=ip
            break
    return attempts,ips,route,success

def terminal_stop_listener():
    # Console input only; Ctrl+C remains available even without a TTY.
    while not STOP_EVENT.is_set():
        try:command=input().strip().lower()
        except (EOFError,OSError):break
        if command in ('q','quit','stop','x'):
            stop();break

def live_status(hosts, status, elapsed, duration, cycle, out):
    if duration>0:
        fraction=min(1,elapsed/duration);width=26
        bar='█'*round(width*fraction)+'░'*(width-round(width*fraction))
        progress=f'[{bar}] {fraction*100:5.1f}%  rest {max(0,(duration-elapsed)/60):.1f} min'
    else:progress=f'Laufzeit {elapsed/60:.1f} min (offen)'
    print(f'Cycle {cycle} | {progress} | {sum(status[h]["ok"] for h in hosts)}/{len(hosts)} HTTP OK',flush=True)
    for h in hosts:
        x=status[h];m=x['metrics']; flag='OK' if x['ok'] else 'AUSFALL'
        print(f'  {h:<21} {flag:<7} DNS={"OK" if x["dns"] else "FEHLER":<6} via={x["route"]:<8} '
              f'probe={m.get("probes_sent", "-")} exch={m.get("exchanges_complete", "-")} '
              f'replay={m.get("replay", "-")} source={m.get("active_time_source", "-")}',flush=True)
    print('  Abbruch: q + Enter oder einmal Ctrl+C',flush=True)

def launch(cfg,out,minutes):
    global STOP
    STOP=False;STOP_EVENT.clear()
    out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
    db=init_database(out/'stability.sqlite3')
    hosts=list(dict.fromkeys(cfg['expected_hosts']))
    interval=float(cfg['interval_seconds']);duration=minutes*60
    run_id=utc();db.execute('INSERT INTO runs VALUES(?,?,?,?,?,?)',(run_id,run_id,None,duration,interval,json.dumps(hosts)));db.commit()
    (out/'test_metadata.json').write_text(json.dumps({'run_id':run_id,'hosts':hosts,'interval':interval,
        'start_utc':run_id,'duration_minutes':minutes,'firmware_expected':'v87-beta134'},indent=2),encoding='utf8')
    print('Test',run_id,'|',len(hosts),'hosts | interval',interval,'s | duration',minutes,'min',flush=True)
    print('Beenden: q + Enter oder einmal Ctrl+C',flush=True)
    if sys.stdin.isatty():threading.Thread(target=terminal_stop_listener,daemon=True).start()
    started=time.monotonic(); cycle=0;known={}
    # Static fallback IPs always take precedence over learned addresses.
    while not STOP_EVENT.is_set() and (duration<=0 or time.monotonic()-started<duration):
        stamp=utc();statuses={};t0=time.monotonic()
        try:
            for host in hosts:
                if STOP_EVENT.is_set():break
                attempts,ips,route,payload=poll(host,cfg,known)
                ok=payload is not None
                if ok:update_known_ips(payload,hosts,known)
                for a in attempts:
                    db.execute('INSERT INTO observations(run_id,ts,host,candidate,resolved_ip,http_ok,error,latency_ms,payload) VALUES(?,?,?,?,?,?,?,?,?)',
                     (run_id,stamp,host,a['candidate'],a['resolved_ip'],int(a['http_ok']),a['error'],a['latency_ms'],json.dumps(a['payload'],ensure_ascii=False) if a['payload'] else None))
                detail='; '.join(a['error'] for a in attempts if a['error'])
                db.execute('INSERT INTO network_checks VALUES(?,?,?,?,?,?,?,?,?)',
                           (run_id,stamp,host,int(bool(ips)),json.dumps(ips),int(route=='fallback'),int(ok),route,detail))
                metrics=(payload or {}).get('espnow_time_local') or {}
                statuses[host]={'ok':ok,'dns':bool(ips),'route':route,'metrics':metrics}
            db.commit();cycle+=1
            if len(statuses)==len(hosts):live_status(hosts,statuses,time.monotonic()-started,duration,cycle,out)
            delay=max(0,interval-(time.monotonic()-t0))
            STOP_EVENT.wait(delay)
        except KeyboardInterrupt:
            stop();break
    db.execute('UPDATE runs SET end_utc=? WHERE run_id=?',(utc(),run_id));db.commit();db.close()
    result=make_report(out,run_id,cfg.get('stalled_probe_seconds',120))
    print('\nAuswertungsdateien:',flush=True)
    for name in ('stability_report.html','stability_report.md','stability_report.json','stability.sqlite3','test_metadata.json'):
        path=(out/name).resolve()
        if path.exists(): print(' ',path,flush=True)
    htmlpath=(out/'stability_report.html').resolve()
    print('Bericht im Browser:',htmlpath.as_uri(),flush=True)
    return result

def n(v):
    try:return float(v) if not isinstance(v,bool) and v is not None else None
    except (TypeError, ValueError):return None

def events_for(records,stall):
    incidents=[]; periods=[];last=None;failed_since=None;probe_last=None;probe_since=None
    for r in records:
        stamp=r['ts'];j=r.get('payload');ok=r['ok']
        if not ok:
            if failed_since is None:failed_since=stamp
            continue
        if failed_since is not None:
            incidents.append({'type':'http_outage','from':failed_since,'to':stamp,'recovered':True});failed_since=None
        local=next((x for x in j.get('nodes',[]) if x.get('local')),{})
        radio=j.get('espnow_time_local') or {}
        epoch=j.get('coordinator_epoch');boot=local.get('boot_id')
        if last:
            for key,a,b in [('boot_change',last['boot'],boot),('epoch_change',last['epoch'],epoch)]:
                if a is not None and b is not None and a!=b:
                    incidents.append({'type':key,'at':stamp,'before':a,'after':b})
            if last['boot'] == boot:
                for key in ('replay','rejected','exchange_timeouts','source_changes'):
                    old=n(last['radio'].get(key));new=n(radio.get(key))
                    if old is not None and new is not None and new>old:
                        incidents.append({'type':'counter_increase','at':stamp,'counter':key,'delta':new-old})
        probes=n(radio.get('probes_sent'))
        if periods and not periods[-1].get('closed') and probes is not None and probes!=probe_last:
            periods[-1]['closed']=True;periods[-1]['to']=stamp
        if probes is not None and local.get('cluster_role')!='coordinator':
            if probe_last is None or (last and last['boot']!=boot) or probes!=probe_last:
                probe_last=probes;probe_since=stamp
            elif probe_since and (dt.datetime.fromisoformat(stamp)-dt.datetime.fromisoformat(probe_since)).total_seconds()>=stall:
                if not periods or periods[-1].get('closed'):
                    periods.append({'type':'probe_stall','from':probe_since,'detected':stamp,'closed':False})
        last={'boot':boot,'epoch':epoch,'radio':radio}
    if failed_since is not None:incidents.append({'type':'http_outage','from':failed_since,'to':None,'recovered':False})
    incidents.extend(periods)
    return incidents

def dashboard_html(result, records_by_host):
    """Standalone offline HTML including SVG timelines, no third-party dependencies."""
    def esc(v): return html.escape(str(v))
    def plot(metric, title, ylabel='', threshold=None):
        series=[]
        for host, records in records_by_host.items():
            points=[]
            for r in records:
                if not r['ok'] or not r['payload']: continue
                obj=r['payload']; e=obj.get('espnow_time_local') or {}
                value=(obj.get('cluster_time') or {}).get(metric) if metric=='uncertainty_estimate_us' else e.get(metric)
                if isinstance(value,(float,int)) and not isinstance(value,bool):
                    try: ts=dt.datetime.fromisoformat(r['ts']).timestamp()
                    except ValueError: continue
                    points.append((ts,float(value)))
            if points: series.append((host,points))
        if not series: return '<section><h3>'+esc(title)+'</h3><p>Keine Messdaten vorhanden.</p></section>'
        allpts=[pt for _,pts in series for pt in pts]
        xmin=min(x for x,_ in allpts);xmax=max(x for x,_ in allpts)
        ymin=min(y for _,y in allpts);ymax=max(y for _,y in allpts)
        if threshold is not None:ymin=min(ymin,threshold);ymax=max(ymax,threshold)
        if xmax<=xmin:xmax=xmin+1
        if ymax<=ymin:ymax=ymin+1
        margin=(ymax-ymin)*.07;ymin-=margin;ymax+=margin
        W,H=960,245;left,right,top,bottom=74,15,12,38;pw=W-left-right;ph=H-top-bottom
        xx=lambda x:left+(x-xmin)/(xmax-xmin)*pw
        yy=lambda y:top+ph-(y-ymin)/(ymax-ymin)*ph
        colors=['#2563eb','#d97706','#059669','#9333ea']
        parts=[f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{esc(title)}" xmlns="http://www.w3.org/2000/svg">']
        for i in range(5):
            val=ymin+(ymax-ymin)*i/4;y=yy(val)
            parts.append(f'<path d="M{left} {y:.1f}H{W-right}" stroke="#e5e7eb"/><text x="{left-6}" y="{y+4:.1f}" font-size="12" text-anchor="end" fill="#6b7280">{val:.1f}</text>')
        if threshold is not None:
            y=yy(threshold);parts.append(f'<path d="M{left} {y:.1f}H{W-right}" stroke="#dc2626" stroke-dasharray="5 5"/>')
        for i,(host,pts) in enumerate(series):
            # bounded data count for reasonably-sized HTML on multi-day tests
            stride=max(1,(len(pts)+1199)//1200);reduced=pts[::stride]
            if reduced[-1]!=pts[-1]:reduced.append(pts[-1])
            d=' '.join(('M' if j==0 else 'L')+f'{xx(x):.1f},{yy(y):.1f}' for j,(x,y) in enumerate(reduced))
            parts.append(f'<path d="{d}" fill="none" stroke="{colors[i%len(colors)]}" stroke-width="1.7"/>')
        fmt=lambda t:dt.datetime.fromtimestamp(t,dt.timezone.utc).strftime('%d.%m. %H:%M UTC')
        parts += [f'<text x="{left}" y="{H-9}" font-size="12" fill="#6b7280">{esc(fmt(xmin))}</text>',f'<text x="{W-right}" y="{H-9}" font-size="12" text-anchor="end" fill="#6b7280">{esc(fmt(xmax))}</text>','</svg>']
        legend=' '.join(f'<span class="key"><i style="background:{colors[i%len(colors)]}"></i>{esc(h)}</span>' for i,(h,_) in enumerate(series))
        return '<section><h3>'+esc(title)+'</h3>'+''.join(parts)+'<div class="legend">'+legend+'</div></section>'
    css='body{font:15px system-ui,sans-serif;margin:0;background:#f4f6f9;color:#1f2937}main{max-width:1160px;margin:auto;padding:30px 18px}h1{margin-bottom:5px}h2{margin:30px 0 12px}section,.panel{background:white;border:1px solid #e5e7eb;border-radius:12px;padding:17px;margin:15px 0;overflow-x:auto}section svg{width:100%;height:auto}table{border-collapse:collapse;width:100%;font-size:14px}th,td{text-align:left;padding:10px;border-bottom:1px solid #eee}th{background:#f8fafc}.key{display:inline-flex;align-items:center;margin-right:16px;font-size:13px}.key i{width:12px;height:4px;display:inline-block;margin-right:5px}.legend{margin:8px 4px}small{color:#64748b}.bad{color:#b91c1c}.good{color:#047857}.warn{color:#92400e}.pill{font-weight:700}.sub{color:#64748b}a{color:#2563eb}'
    status=result['result']
    bits=['<!doctype html><html lang="de"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>SensorForge Stabilitätsbericht</title><style>'+css+'</style></head><body><main>',
          '<h1>SensorForge – Stabilitätsbericht</h1><p class="sub">Testlauf: '+esc(result['run_id'])+'<br>Zeitraum: '+esc(result['start_utc'])+' – '+esc(result.get('end_utc') or 'läuft')+'</p>',
          '<div class="panel"><strong>Gesamtbewertung: '+esc(status)+'</strong><p>HTTP-Monitoring beweist keine absolute Geräteverfügbarkeit, keine Funkpräzision und keine GPIO-Synchronität. PASS_MONITORING_ONLY bedeutet ausschließlich, dass die implementierten Monitoring-Prüfungen bestanden wurden.</p></div>',
          '<h2>Geräteübersicht</h2><section><table><thead><tr><th>Gerät</th><th>Status</th><th>HTTP</th><th>Boots</th><th>Epochen</th><th>Probes</th><th>Exchanges</th><th>Replay</th><th>Drift ppm</th><th>Vorfälle</th></tr></thead><tbody>']
    for host, inf in result['nodes'].items():
        m=inf['last_metrics'];av=inf['http_availability']; vals=[host,inf['status'],f'{av*100:.1f}%' if av is not None else 'n/a',len(inf['boot_ids']),len(inf['epochs']),m.get('probes_sent'),m.get('exchanges_complete'),m.get('replay'),m.get('drift_ppm'),len(inf['incidents'])]
        bits.append('<tr>'+''.join('<td>'+esc(v if v is not None else 'n/a')+'</td>' for v in vals)+'</tr>')
    bits.append('</tbody></table></section><h2>Verlauf</h2>')
    for metric,title,unit,limit in [('probes_sent','Probes gesendet',None,None),('exchanges_complete','Abgeschlossene Exchanges',None,None),('replay','Replay-Ablehnungen',None,None),('drift_ppm','Gemeldeter Drift (ppm)',None,None),('last_series_spread_us','Offset-Schätzstreuung (µs), nicht GPIO-Skew',None,250),('broadcaster_production_beacons_sent','Production-Beacons gesendet',None,None),('authenticated','ESP-NOW authentifiziert',None,None)]:
        bits.append(plot(metric,title,unit,limit))
    bits.append('<h2>Netzwerkdiagnose</h2><section><table><tr><th>Gerät</th><th>DNS-Fehler</th><th>IP-Fallback erfolgreich</th><th>HTTP-Fehler</th></tr>')
    for host,nd in result.get('network_diagnostics',{}).items():
        bits.append('<tr>'+''.join('<td>'+esc(v)+'</td>' for v in (host,nd['dns_failures'],nd['fallback_successes'],nd['http_failures']))+'</tr>')
    bits.append('</table></section><h2>Vorfälle</h2>')
    for host,inf in result['nodes'].items():
        bits.append('<section><h3>'+esc(host)+'</h3>')
        if not inf['incidents']:bits.append('<p>Keine durch den Monitor erkannten Vorfälle.</p>')
        else:
            bits.append('<table><tr><th>Typ</th><th>Beginn</th><th>Ende/Erkennung</th></tr>')
            for incident in inf['incidents']:
                bits.append('<tr><td>'+esc(incident.get('type'))+'</td><td>'+esc(incident.get('from',''))+'</td><td>'+esc(incident.get('to') or incident.get('detected') or 'offen')+'</td></tr>')
            bits.append('</table>')
        bits.append('</section>')
    bits.append('<p class="sub">Offline generierter Bericht. Rohdaten: stability.sqlite3; zusätzliche Auswertung: stability_report.json und stability_report.md.</p></main></body></html>')
    return ''.join(bits)

def make_report(out,run_id=None,stall=120):
    out=pathlib.Path(out);db=init_database(out/'stability.sqlite3');db.row_factory=sqlite3.Row
    if run_id is None:
        row=db.execute('SELECT run_id FROM runs ORDER BY start_utc DESC LIMIT 1').fetchone()
        if not row:raise ValueError('No runs found')
        run_id=row[0]
    run=db.execute('SELECT * FROM runs WHERE run_id=?',(run_id,)).fetchone()
    hosts=json.loads(run['expected_hosts']);data=defaultdict(lambda:defaultdict(list))
    for row in db.execute('SELECT * FROM observations WHERE run_id=? ORDER BY ts,id',(run_id,)):
        data[row['host']][row['ts']].append(row)
    result={'run_id':run_id,'start_utc':run['start_utc'],'end_utc':run['end_utc'],
            'expected_hosts':hosts,'nodes':{},'result':'INCOMPLETE','notes':[
                'No radio or GPIO precision proof from HTTP polling.',
                'Missing samples and unavailable firmware fields are not PASS.',
                'Outage durations are bounded by polling cadence, not continuous measurements.',
                'Drift ppm is firmware-reported estimate, not independent clock-drift verification.']}
    lines=['# SensorForge stability test',f'Run: {run_id}',f'Start: {run["start_utc"]} | End: {run["end_utc"] or "running"}','']
    any_fail=False;any_unknown=False
    html_records={}
    for host in hosts:
        rec=[]
        for stamp,arr in sorted(data[host].items()):
            best=next((x for x in arr if x['http_ok']),arr[-1]);j=json.loads(best['payload']) if best['payload'] else None
            rec.append({'ts':stamp,'ok':bool(best['http_ok']),'payload':j,'attempts':len(arr),
                        'errors':[x['error'] for x in arr if not x['http_ok']]})
        html_records[host]=rec
        successful=[r for r in rec if r['ok']];incidents=events_for(rec,stall)
        latest=successful[-1]['payload'] if successful else {}
        local=next((x for x in latest.get('nodes',[]) if x.get('local')),{})
        radio=latest.get('espnow_time_local') or {}
        sources=sorted(set((r['payload'].get('cluster_time') or {}).get('source','') for r in successful))
        epochs=sorted(set(str(r['payload'].get('coordinator_epoch')) for r in successful if r['payload'].get('coordinator_epoch') is not None))
        boots=sorted(set(str(next((x for x in r['payload'].get('nodes',[]) if x.get('local')),{}).get('boot_id')) for r in successful))
        last_metrics={k:radio.get(k) for k in ('replay','authenticated','probes_sent','exchanges_complete','exchange_timeouts',
                                'precision_recently_accepted','broadcaster_lease_active',*EXTRA_KEYS)}
        fail=any(x['type'] in ('http_outage','probe_stall') for x in incidents)
        unknown=not successful or any(last_metrics.get(k) is None for k in ('replay','authenticated'))
        if fail:any_fail=True
        if unknown:any_unknown=True
        info={'cycles':len(rec),'ok_cycles':len(successful),'http_availability':len(successful)/len(rec) if rec else None,
              'mode':local.get('mode'),'role':local.get('cluster_role'),'boot_ids':boots,'epochs':epochs,
              'time_sources':sources,'last_metrics':last_metrics,'incidents':incidents,
              'status':'FAIL' if fail else ('INCOMPLETE' if unknown else 'PASS_MONITORING_ONLY')}
        result['nodes'][host]=info
        lines += [f'## {host} — {info["status"]}',f'HTTP cycles {len(successful)}/{len(rec)}; boots {len(boots)}; epochs {len(epochs)}',
                 f'Time sources: {", ".join(sources)}',f'Incidents: {len(incidents)}',
                 f'Latest: probes={radio.get("probes_sent")}, exchanges={radio.get("exchanges_complete")}, replay={radio.get("replay")}, drift_ppm={radio.get("drift_ppm")}', '']
        for e in incidents[:40]:lines.append(f'- {e}')
        if len(incidents)>40:lines.append(f'- … {len(incidents)-40} more (see JSON)')
        lines.append('')
    result['network_diagnostics']={}
    for host in hosts:
        checks=db.execute('SELECT dns_ok,fallback_used,http_ok,route FROM network_checks WHERE run_id=? AND host=?',(run_id,host)).fetchall()
        result['network_diagnostics'][host]={'checks':len(checks),'dns_failures':sum(not r[0] for r in checks),
            'fallback_successes':sum(bool(r[1] and r[2]) for r in checks),
            'http_failures':sum(not r[2] for r in checks),
            'routes':dict((name,sum(r[3]==name for r in checks)) for name in ('dns','fallback','none','hostname'))}
        if checks:
            lines += [f'## Network: {host}',str(result['network_diagnostics'][host]),'']
    result['result']='FAIL' if any_fail else ('INCOMPLETE' if any_unknown or run['end_utc'] is None else 'PASS_MONITORING_ONLY')
    lines.insert(3,f'Overall: **{result["result"]}**')
    (out/'stability_report.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
    (out/'stability_report.md').write_text('\n'.join(lines),encoding='utf8')
    (out/'stability_report.html').write_text(dashboard_html(result,html_records),encoding='utf8')
    db.close()
    print('Report:',out/'stability_report.md','| result:',result['result'])
    return result

def menu():
    print('\nSensorForge Test Manager (read-only)')
    print('1  Cluster-Schnelltest (5 min)\n2  Stabilität (24 h)\n3  Stabilität (72 h)\n4  Freie Laufzeit\n5  Vorhandene Messung auswerten\n0  Ende')
    return input('Auswahl: ').strip()

def main():
    p=argparse.ArgumentParser(description='SensorForge Test Manager (read-only)')
    p.add_argument('--config',default='sensorforge_config.json')
    p.add_argument('--output',default='sensorforge_tests')
    p.add_argument('--minutes',type=float)
    p.add_argument('--report',action='store_true')
    a=p.parse_args()
    if a.report:make_report(a.output);return
    cfg=cfg_read(a.config)
    if a.minutes is None:
        opt=menu()
        if opt=='0':return
        if opt=='5':make_report(a.output);return
        if opt=='1':minutes=5
        elif opt=='2':minutes=1440
        elif opt=='3':minutes=4320
        elif opt=='4':
            hours=float(input('Stunden (z.B. 2,5; 0 möglich): ').strip().replace(',','.'))
            mins=float(input('Zusätzliche Minuten (z.B. 30): ').strip().replace(',','.'))
            minutes=hours*60+mins
        else:p.error('Unknown option')
    else:minutes=a.minutes
    if minutes<0:p.error('Minutes must be >=0')
    if float(cfg['interval_seconds'])<2:p.error('Interval must be >=2 sec')
    signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop)
    launch(cfg,a.output,minutes)

if __name__=='__main__':main()
