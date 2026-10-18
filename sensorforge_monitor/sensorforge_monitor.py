#!/usr/bin/env python3
"""SensorForge cluster monitor, Python 3.10+, stdlib; optional matplotlib for PNGs.

Read-only HTTP polls of /cluster_status. No credentials in output reports.
"""
from __future__ import annotations
import argparse, base64, csv, datetime as dt, getpass, html, json, os, pathlib, signal, sqlite3, statistics, sys, time, urllib.error, urllib.parse, urllib.request
from collections import defaultdict
from typing import Any

DEFAULT_SEED = 'http://devxiao3.local'
FIELDS = [
 'ts_utc','hostname','ip','mode','role','online','ok','error','boot_id','uptime_seconds',
 'coordinator_id','coordinator_epoch','cluster_id','source','cluster_time_valid','cluster_uncertainty_us',
 'espnow_active','espnow_channel','espnow_sent','espnow_received','espnow_authenticated',
 'espnow_rejected','espnow_replay','espnow_dropped','probes_sent','exchanges_complete',
 'exchange_timeouts','last_series_spread_us','last_series_selected','last_series_rtt_min_us',
 'last_series_rtt_max_us','last_accepted_age_ms','precision_recently_accepted',
 'broadcaster_lease_active','broadcaster_production_beacons_sent',
 'coordinator_suppressing_beacons','coordinator_beacons_suppressed',
 'broadcaster_precision_failed','precision_guard_ref_lifetime','precision_guard_other',
 'probe_tx_pre_send_mean_us','probe_tx_pre_send_max_us','rx_queue_delay_max_us',
 'tx_callback_failed','tx_pending_overflow','free_heap_bytes','rtt_http_ms','payload_bytes',
 'grant_received','grant_authenticated','grant_rejected','grant_acks_sent',
 'grant_acks_verified','grant_grants_sent', 'node_report_count'
]
COUNTERS = ['espnow_sent','espnow_received','espnow_authenticated','espnow_rejected','espnow_replay',
 'espnow_dropped','probes_sent','exchanges_complete','exchange_timeouts',
 'broadcaster_production_beacons_sent','coordinator_beacons_suppressed',
 'broadcaster_precision_failed','precision_guard_ref_lifetime','precision_guard_other',
 'tx_callback_failed','tx_pending_overflow','grant_received','grant_authenticated',
 'grant_rejected','grant_acks_sent','grant_acks_verified','grant_grants_sent']
STOP = False

def on_sigint(*_):
 global STOP
 STOP = True

def iso_now(): return dt.datetime.now(dt.timezone.utc).isoformat(timespec='milliseconds')
def number(v):
 if isinstance(v, bool) or v is None: return None
 try: return float(v)
 except (TypeError, ValueError): return None

def endpoint(host):
 host = str(host).strip()
 if not host: raise ValueError('Empty host')
 if '://' not in host: host = 'http://' + host
 p = urllib.parse.urlparse(host)
 if p.scheme not in ('http','https') or not p.netloc: raise ValueError('Invalid HTTP host')
 return urllib.parse.urlunparse((p.scheme,p.netloc,'/cluster_status','','',''))

def fetch(host, cfg):
 url = endpoint(host)
 headers = {'Accept':'application/json','User-Agent':'SensorForgeMonitor/1.0'}
 auth = cfg.get('auth',{})
 if auth.get('username'):
  raw = (auth['username']+':'+auth.get('password','')).encode('utf-8')
  headers['Authorization']='Basic '+base64.b64encode(raw).decode('ascii')
 req = urllib.request.Request(url,headers=headers)
 t0 = time.monotonic()
 try:
  with urllib.request.urlopen(req,timeout=cfg.get('timeout_seconds',5)) as response:
   body=response.read(4_000_001)
  if len(body)>4_000_000: raise ValueError('JSON response too large')
  obj=json.loads(body.decode('utf-8'))
  if not isinstance(obj,dict): raise ValueError('Response is not an object')
  return obj,None,round((time.monotonic()-t0)*1000,1),len(body)
 except (urllib.error.URLError,TimeoutError,ValueError,UnicodeError, OSError) as exc:
  return None, type(exc).__name__+': '+str(exc)[:160],round((time.monotonic()-t0)*1000,1),0

def node_list(j):
 if not isinstance(j,dict): return []
 return [n for n in j.get('nodes',[]) if isinstance(n,dict) and n.get('hostname')]

def make_row(j,host,ip,ts,err,ms,bodylen):
 n=next((x for x in node_list(j) if x.get('local')), {}) if j else {}
 e=(j or {}).get('espnow_time_local') or {}
 g=(j or {}).get('espnow_grant_diagnostic') or {}
 c=(j or {}).get('cluster_time') or {}
 row={k:None for k in FIELDS}
 row.update(ts_utc=ts,hostname=n.get('hostname') or host,ip=n.get('ip') or ip,
            mode=n.get('mode'),role=n.get('cluster_role') or (j or {}).get('local_role'),
            online=n.get('online') if j else False,ok=bool(j and j.get('ok',False)),error=err or (j or {}).get('error') or '',
            boot_id=n.get('boot_id'),uptime_seconds=n.get('uptime_seconds'),
            coordinator_id=(j or {}).get('coordinator_id'),coordinator_epoch=(j or {}).get('coordinator_epoch'),
            cluster_id=(j or {}).get('runtime_cluster_id') or (j or {}).get('cluster_id'),
            source=c.get('source'),cluster_time_valid=c.get('valid'),cluster_uncertainty_us=c.get('uncertainty_estimate_us'),
            espnow_active=e.get('active'),espnow_channel=e.get('channel'),rtt_http_ms=ms,payload_bytes=bodylen,
            free_heap_bytes=n.get('free_heap_bytes'),node_report_count=len((j or {}).get('espnow_node_reports') or []))
 for key in ['sent','received','authenticated','rejected','replay','dropped']:
  row['espnow_'+key]=e.get(key)
 for key in ['probes_sent','exchanges_complete','exchange_timeouts','last_series_spread_us',
 'last_series_selected','last_series_rtt_min_us','last_series_rtt_max_us','last_accepted_age_ms',
 'precision_recently_accepted','broadcaster_lease_active','broadcaster_production_beacons_sent',
 'coordinator_suppressing_beacons','coordinator_beacons_suppressed',
 'broadcaster_precision_failed','precision_guard_ref_lifetime','precision_guard_other',
 'probe_tx_pre_send_mean_us','probe_tx_pre_send_max_us','rx_queue_delay_max_us',
 'tx_callback_failed','tx_pending_overflow']:
  row[key]=e.get(key)
 for key in ['received','authenticated','rejected','acks_sent','acks_verified','grants_sent']:
  row['grant_'+key]=g.get(key)
 return row

def connect(dbpath):
 conn=sqlite3.connect(dbpath)
 conn.execute('PRAGMA journal_mode=WAL')
 conn.execute('CREATE TABLE IF NOT EXISTS samples (id INTEGER PRIMARY KEY, ts_utc TEXT NOT NULL, hostname TEXT NOT NULL, fields TEXT NOT NULL)')
 conn.execute('CREATE INDEX IF NOT EXISTS ix_samples_host_time ON samples(hostname,ts_utc)')
 conn.execute('CREATE TABLE IF NOT EXISTS discovery (id INTEGER PRIMARY KEY, ts_utc TEXT NOT NULL, hostname TEXT NOT NULL, ip TEXT, mode TEXT, cluster_role TEXT, boot_id TEXT, coordinator_id TEXT)')
 conn.commit()
 return conn

def discover(cfg,previous=None):
 known=dict(previous or {})
 for h in cfg['seed_hosts']:
  try:
   p=urllib.parse.urlparse(endpoint(h)); known.setdefault(p.netloc,{'hostname':p.netloc,'ip':None})
  except ValueError: pass
 for h in list(known):
  j,_,_,_=fetch(known[h].get('ip') or h,cfg)
  if not j: continue
  for n in node_list(j):
   hn=n['hostname']
   known[hn]={'hostname':hn,'ip':n.get('ip'), 'mode':n.get('mode'), 'cluster_role':n.get('cluster_role'), 'boot_id':n.get('boot_id')}
 return known

def save_discovery(conn,known,stamp,coord=None):
 for n in known.values():
  conn.execute('INSERT INTO discovery(ts_utc,hostname,ip,mode,cluster_role,boot_id,coordinator_id) VALUES(?,?,?,?,?,?,?)',
    (stamp,n.get('hostname'),n.get('ip'),n.get('mode'),n.get('cluster_role'),n.get('boot_id'),coord))
 conn.commit()

def run(cfg,out,duration,interval):
 out.mkdir(parents=True,exist_ok=True)
 conn=connect(out/'monitor.sqlite3')
 known=discover(cfg)
 save_discovery(conn,known,iso_now())
 print('Found:', ', '.join(sorted(known)),flush=True)
 if len(known)<2: print('Warning: found fewer than two nodes; check network / seed URL.',flush=True)
 started=time.monotonic(); cycle=0; next_at=started
 try:
  with open(out/'measurements.csv','a',newline='',encoding='utf-8') as f:
   writer=csv.DictWriter(f,fieldnames=FIELDS)
   if f.tell()==0: writer.writeheader()
   while not STOP and (duration<=0 or time.monotonic()-started<duration):
    if cycle % max(1,int(cfg.get('rediscover_every_cycles',6))) == 0:
     known=discover(cfg,known)
     save_discovery(conn,known,iso_now())
    ts=iso_now(); good=0
    for h in sorted(known):
     n=known[h]; j,err,ms,nbytes=fetch(n.get('ip') or h,cfg)
     row=make_row(j,h,n.get('ip'),ts,err,ms,nbytes)
     writer.writerow(row)
     conn.execute('INSERT INTO samples(ts_utc,hostname,fields) VALUES(?,?,?)',
                  (ts,row['hostname'],json.dumps(row,ensure_ascii=False)))
     if j and j.get('ok'):
      good+=1
      for node in node_list(j):
       if node.get('hostname'):
        known[node['hostname']]={'hostname':node['hostname'],'ip':node.get('ip'),'mode':node.get('mode'),'cluster_role':node.get('cluster_role'),'boot_id':node.get('boot_id')}
    conn.commit();f.flush()
    print(f'{ts} | reachable {good}/{len(known)} | sample {cycle+1}',flush=True)
    cycle+=1;next_at+=interval
    delay=max(0,next_at-time.monotonic())
    if delay: time.sleep(delay)
 finally: conn.close()
 print('Saved:',out,flush=True)
 generate_report(out)

def collect_rows(conn):
 return [json.loads(x[0]) for x in conn.execute('SELECT fields FROM samples ORDER BY ts_utc,id')]

def clean(values):
 return [x for x in values if isinstance(x,(int,float)) and not isinstance(x,bool)]

def delta_rate(a,b,key):
 if not a or not b or a.get('boot_id')!=b.get('boot_id'): return None
 x,y=number(a.get(key)),number(b.get(key))
 if x is None or y is None or y<x:return None
 try: seconds=(dt.datetime.fromisoformat(b['ts_utc'])-dt.datetime.fromisoformat(a['ts_utc'])).total_seconds()
 except ValueError:return None
 return (y-x)/seconds if seconds>0 else None

def fnum(n,unit='',digits=1):
 return 'n/a' if n is None else f'{n:.{digits}f}{unit}'

def generate_report(out):
 conn=connect(out/'monitor.sqlite3');rows=collect_rows(conn);conn.close()
 byhost=defaultdict(list)
 for r in rows: byhost[r['hostname']].append(r)
 summary={'created_at_utc':iso_now(),'sample_count':len(rows),'nodes':{},
 'interpretation_limits':[
 'last_series_spread_us is dispersion of selected offset estimates, NOT GPIO synchronization accuracy.',
 'clock uncertainty_estimate_us belongs to WiFi coordinator time, NOT a measured GPIO offset.',
 'HTTP response times and independently polled utc_us are NOT simultaneous clock measurements.',
 'A production beacon TX counter does not confirm successful reception by peers.',
 'Precision acceptance on one node does not prove inter-node GPIO skew <=250us.',
 'Rates are computed only within the same boot_id and nondecreasing counters.' ]}
 lines=['# SensorForge cluster measurement report','',f'Created UTC: {summary["created_at_utc"]}',f'Recorded samples: {len(rows)}','',
 '**WARNING:** This monitor evaluates operation, link health, estimator spread, and handover. It **does not** measure real relative GPIO synchronisation. A hardware GPIO capture test is still required to verify <=250 µs.','']
 for host, rr in sorted(byhost.items()):
  valid=[r for r in rr if r['ok']]
  recent=valid[-1] if valid else rr[-1]
  spreads=clean([number(r.get('last_series_spread_us')) for r in valid if r.get('last_series_estimate_age_ms') is not None or number(r.get('last_series_selected'))])
  selected=[number(r['last_series_selected']) for r in valid if number(r.get('last_series_selected')) and number(r.get('last_series_selected'))>0]
  spreadvals=clean([number(r.get('last_series_spread_us')) for r in valid if number(r.get('last_series_selected')) and number(r.get('last_series_selected'))>0])
  prev=valid[-2] if len(valid)>=2 else None
  # Latest estimates can be repeated across polls; don't count them as distinct accepted series.
  rate=delta_rate(prev,recent,'broadcaster_production_beacons_sent')
  exchange_rate=delta_rate(prev,recent,'exchanges_complete')
  info={'samples':len(rr),'successful_polls':len(valid),'availability_fraction':len(valid)/len(rr),
        'last_mode':recent.get('mode'),'last_role':recent.get('role'),'last_boot_id':recent.get('boot_id'),
        'last_uptime_seconds':recent.get('uptime_seconds'),
        'last_spread_us':recent.get('last_series_spread_us') if (number(recent.get('last_series_selected')) or 0)>0 else None,
        'spread_observed_min_us':min(spreadvals) if spreadvals else None,
        'spread_observed_median_us':statistics.median(spreadvals) if spreadvals else None,
        'spread_observed_max_us':max(spreadvals) if spreadvals else None,
        'last_accepted_age_ms':recent.get('last_accepted_age_ms'),
        'last_precision_recently_accepted':recent.get('precision_recently_accepted'),
        'last_probes_sent':recent.get('probes_sent'),
        'last_exchanges_complete':recent.get('exchanges_complete'),
        'last_exchange_timeouts':recent.get('exchange_timeouts'),
        'last_broadcaster_lease_active':recent.get('broadcaster_lease_active'),
        'last_production_beacons_sent':recent.get('broadcaster_production_beacons_sent'),
        'recent_production_rate_hz':rate,'recent_exchange_rate_hz':exchange_rate,
        'last_source':recent.get('source'),
        'last_grants_authenticated':recent.get('grant_authenticated'),
        'last_coordinator_suppressing_beacons':recent.get('coordinator_suppressing_beacons'),
        'last_espnow_received':recent.get('espnow_received'),
        'last_espnow_authenticated':recent.get('espnow_authenticated'),
        'last_http_latency_ms':recent.get('rtt_http_ms')}
  summary['nodes'][host]=info
  lines += [f'## {host}',f'- Role: {info["last_role"]}; mode: {info["last_mode"]}; boot: {info["last_boot_id"]}',
      f'- HTTP availability: {len(valid)}/{len(rr)} polls ({info["availability_fraction"]*100:.1f}%)',
      f'- Latest estimator spread: {fnum(number(info["last_spread_us"])," µs")} (quality threshold 250 µs; not GPIO accuracy)',
      f'- Observed spread min / median / max across polling snapshots: {fnum(info["spread_observed_min_us"])} / {fnum(info["spread_observed_median_us"])} / {fnum(info["spread_observed_max_us"])} µs (repeated estimates possible)',
      f'- Precision recently accepted: {info["last_precision_recently_accepted"]}; last accepted age: {info["last_accepted_age_ms"]} ms',
      f'- Exchanges completed: {info["last_exchanges_complete"]}; timeouts: {info["last_exchange_timeouts"]}',
      f'- Broadcaster lease active: {info["last_broadcaster_lease_active"]}; production beacons sent: {info["last_production_beacons_sent"]}; recent TX rate: {fnum(rate," Hz",2)}',
      f'- ESP-NOW received/authenticated: {info["last_espnow_received"]}/{info["last_espnow_authenticated"]}; grants authenticated: {info["last_grants_authenticated"]}',
      '']
 # detect situations at latest observation; do not flag normal nodes for inactive lease
 alerts=[]
 for host,info in summary['nodes'].items():
  if info['availability_fraction']<.8:alerts.append(f'{host}: HTTP availability below 80%')
  if info['last_mode']=='time_broadcaster':
   if info['last_exchanges_complete']==0:alerts.append(f'{host}: no precision exchanges')
   if info['last_spread_us'] is not None and info['last_spread_us']>250:alerts.append(f'{host}: latest estimator spread >250 us')
   if not info['last_production_beacons_sent']:alerts.append(f'{host}: no productive beacon TX')
   if info['last_accepted_age_ms'] is not None and number(info['last_accepted_age_ms']) and info['last_accepted_age_ms']>120000:alerts.append(f'{host}: last accepted precision estimate older than 120s')
 summary['alerts']=alerts
 lines+=['## Alerts / checks']+(['- '+a for a in alerts] if alerts else ['- No rule-based alarms triggered; this is not proof of GPIO accuracy.'])+['','## Limitations']+['- '+lim for lim in summary['interpretation_limits']]
 (out/'report.json').write_text(json.dumps(summary,indent=2,ensure_ascii=False),encoding='utf-8')
 (out/'report.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
 # Standalone HTML summary, links to locally generated charts.
 table_rows=[]
 for h,inf in sorted(summary['nodes'].items()):
  table_rows.append('<tr>'+''.join('<td>'+html.escape(str(v))+'</td>' for v in [h,inf['last_mode'],inf['last_role'], f"{inf['availability_fraction']*100:.0f}%",fnum(number(inf['last_spread_us']),' µs'),str(inf['last_exchanges_complete']),str(inf['last_production_beacons_sent']),str(inf['last_broadcaster_lease_active'])])+'</tr>')
 notice='; '.join(alerts) if alerts else 'No rule-based alarms triggered.'
 report_html=f"""<!doctype html><html lang='de'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>SensorForge Cluster Monitor</title><style>body{{font-family:system-ui,sans-serif;max-width:1100px;margin:36px auto;padding:0 20px;line-height:1.5;color:#20232a}}table{{border-collapse:collapse;width:100%;font-size:14px}}th,td{{padding:10px;border-bottom:1px solid #ddd;text-align:left}}th{{background:#f1f3f5}}img{{max-width:100%;height:auto}}.warning{{border-left:4px solid #c18b2e;padding:14px;background:#faf7ef}}small{{color:#555}}</style></head><body><h1>SensorForge Cluster Monitor</h1><small>Generated {html.escape(summary['created_at_utc'])}; samples {len(rows)}</small><p class='warning'><strong>Important:</strong> Estimator spread does not prove real GPIO synchronisation accuracy. No physical GPIO comparison is performed.</p><h2>Cluster summary</h2><table><thead><tr><th>Host</th><th>Mode</th><th>Role</th><th>Availability</th><th>Spread</th><th>Exchanges</th><th>Beacon TX</th><th>Lease</th></tr></thead><tbody>{''.join(table_rows)}</tbody></table><h2>Checks</h2><p>{html.escape(notice)}</p><h2>Time series</h2>{''.join(f'<h3>{html.escape(name)}</h3><img src="{file}" alt="{html.escape(name)}">' for name,file in [('Estimator spread','spread.png'),('Accepted age','accepted_age.png'),('Precision exchanges','exchanges.png'),('Productive beacons','beacons.png'),('ESP-NOW receive','rx.png')])}<p><a href='report.md'>Full report</a> · <a href='report.json'>JSON summary</a> · <a href='measurements.csv'>Measurements CSV</a></p></body></html>"""
 (out/'report.html').write_text(report_html,encoding='utf-8')
 # PNG visualization is optional; report always works without extra packages.
 try:
  import matplotlib
  matplotlib.use('Agg')
  import matplotlib.pyplot as plt
  plots=[('last_series_spread_us','Estimator spread (µs); threshold 250','spread.png',250),
         ('last_accepted_age_ms','Last accepted age (ms); limit 120000','accepted_age.png',120000),
         ('exchanges_complete','Completed precision exchanges (counter)','exchanges.png',None),
         ('broadcaster_production_beacons_sent','Productive beacons sent (counter)','beacons.png',None),
         ('espnow_received','ESP-NOW RX (counter)','rx.png',None)]
  for key,title,filename,limit in plots:
   fig,ax=plt.subplots(figsize=(11,4.5));drawn=False
   for host,rr in sorted(byhost.items()):
    pts=[(dt.datetime.fromisoformat(r['ts_utc']),number(r.get(key))) for r in rr if r['ok'] and number(r.get(key)) is not None]
    if key=='last_series_spread_us':pts=[(dt.datetime.fromisoformat(r['ts_utc']),number(r.get(key))) for r in rr if r['ok'] and (number(r.get('last_series_selected')) or 0)>0]
    if pts:ax.plot([p[0] for p in pts],[p[1] for p in pts],marker='.',markersize=3,label=host);drawn=True
   if limit is not None:ax.axhline(limit,linestyle='--',linewidth=1,label='Threshold')
   ax.set_title(title);ax.set_xlabel('UTC');ax.grid(True,alpha=.25)
   if drawn or limit is not None:ax.legend(fontsize=8)
   fig.autofmt_xdate();fig.tight_layout();fig.savefig(out/filename,dpi=130);plt.close(fig)
 except ImportError:
  pass
 print('Report written:',out/'report.md',out/'report.json',flush=True)

def create_config(path):
 print('SensorForge setup (credentials stored in local JSON file; chmod 600)')
 seed=input(f'Seed host [{DEFAULT_SEED}]: ').strip() or DEFAULT_SEED
 user=input('HTTP Basic Auth username (blank if not needed): ').strip()
 pw=getpass.getpass('HTTP Basic Auth password (blank if not needed): ') if user else ''
 cfg={'seed_hosts':[seed],'interval_seconds':10,'timeout_seconds':5,
      'rediscover_every_cycles':6,'auth':{'username':user,'password':pw}}
 path.parent.mkdir(parents=True,exist_ok=True)
 with os.fdopen(os.open(path,os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),'w',encoding='utf-8') as f:
  json.dump(cfg,f,indent=2,ensure_ascii=False)
 try:os.chmod(path,0o600)
 except OSError:pass
 print('Config saved:',path)

def main():
 parser=argparse.ArgumentParser(description='SensorForge dynamic read-only cluster monitor')
 sub=parser.add_subparsers(dest='action',required=True)
 ip=sub.add_parser('init',help='interactive configuration');ip.add_argument('--config',default='sensorforge_config.json')
 rp=sub.add_parser('run',help='poll cluster and generate report')
 rp.add_argument('--config',default='sensorforge_config.json');rp.add_argument('--output',default='sensorforge_results')
 rp.add_argument('--minutes',type=float,default=15,help='0 = run until Ctrl+C')
 rp.add_argument('--interval',type=float,default=None)
 ep=sub.add_parser('report',help='regenerate reports from SQLite');ep.add_argument('--output',default='sensorforge_results')
 args=parser.parse_args()
 if args.action=='init':create_config(pathlib.Path(args.config));return
 if args.action=='report':generate_report(pathlib.Path(args.output));return
 cfgpath=pathlib.Path(args.config)
 if not cfgpath.exists():parser.error(f'Configuration not found: {cfgpath} (run init)')
 cfg=json.loads(cfgpath.read_text(encoding='utf-8'))
 interval=args.interval or cfg.get('interval_seconds',10)
 if interval<2:parser.error('Interval must be >= 2 seconds')
 signal.signal(signal.SIGINT,on_sigint)
 signal.signal(signal.SIGTERM,on_sigint)
 run(cfg,pathlib.Path(args.output),max(0,args.minutes*60),interval)

if __name__=='__main__':main()
