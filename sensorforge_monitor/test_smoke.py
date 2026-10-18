import json, tempfile, threading, pathlib
from http.server import HTTPServer, BaseHTTPRequestHandler
from sensorforge_monitor import discover, fetch, connect, make_row, generate_report

class Handler(BaseHTTPRequestHandler):
 def do_GET(self):
  if self.path!='/cluster_status':self.send_error(404);return
  b=json.dumps({'ok':True,'nodes':[{'local':True,'hostname':'devxiao3','ip':'127.0.0.1','mode':'time_broadcaster','cluster_role':'node','boot_id':'xx','online':True},{'local':False,'hostname':'devfreenove1','ip':'127.0.0.1','mode':'normal','cluster_role':'coordinator','online':True}], 'espnow_time_local':{'exchanges_complete':16,'last_series_selected':4,'last_series_spread_us':129,'broadcaster_production_beacons_sent':101},'cluster_time':{'valid':True}}).encode()
  self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(b)));self.end_headers();self.wfile.write(b)
 def log_message(self,*a):pass

server=HTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
seed=f'http://127.0.0.1:{server.server_port}'
cfg={'seed_hosts':[seed],'timeout_seconds':2,'auth':{}}
j,err,ms,ln=fetch(seed,cfg);assert j and err is None
known=discover(cfg);assert 'devfreenove1' in known
with tempfile.TemporaryDirectory() as temp:
 out=pathlib.Path(temp);conn=connect(out/'monitor.sqlite3')
 row=make_row(j,'devxiao3','127.0.0.1','2026-10-18T12:00:00+00:00',None,ms,ln)
 conn.execute('INSERT INTO samples(ts_utc,hostname,fields) VALUES(?,?,?)',(row['ts_utc'],row['hostname'],json.dumps(row)));conn.commit();conn.close()
 generate_report(out)
 assert (out/'report.md').exists() and (out/'report.html').exists() and (out/'report.json').exists()
 assert json.loads((out/'report.json').read_text())['nodes']['devxiao3']['last_spread_us']==129
 print('SMOKE TEST PASSED: HTTP JSON, discovery, SQLite, report, HTML')
server.shutdown()
