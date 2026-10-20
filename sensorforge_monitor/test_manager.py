import datetime as dt, json, pathlib, tempfile
from sensorforge_test_manager import init_database, make_report, events_for

def test_events():
    t0=dt.datetime(2026,10,19,tzinfo=dt.timezone.utc)
    def row(secs,probes,ok=True,boot='a',epoch=1):
        return dict(ts=(t0+dt.timedelta(seconds=secs)).isoformat(),ok=ok,payload={'nodes':[{'local':True,'boot_id':boot,'cluster_role':'node'}], 'coordinator_epoch':epoch,'espnow_time_local':{'probes_sent':probes}} if ok else None)
    incidents=events_for([row(0,1),row(130,1),row(140,2),row(150,2,False),row(170,3),row(180,4,boot='b',epoch=2)],120)
    types=[i['type'] for i in incidents]
    assert 'probe_stall' in types and 'http_outage' in types and 'boot_change' in types and 'epoch_change' in types,types
    assert next(i for i in incidents if i['type']=='probe_stall')['closed']

def test_report():
    with tempfile.TemporaryDirectory() as td:
        p=pathlib.Path(td);db=init_database(p/'stability.sqlite3')
        db.execute('INSERT INTO runs VALUES(?,?,?,?,?,?)',('run','2026-10-19T00:00:00+00:00','2026-10-19T00:10:00+00:00',600,10,json.dumps(['devxiao1.local'])))
        payload={'ok':True,'nodes':[{'local':True,'hostname':'devxiao1','boot_id':'a','cluster_role':'node'}],'espnow_time_local':{'replay':0,'authenticated':10,'probes_sent':5,'drift_ppm':1.1}}
        db.execute('INSERT INTO observations(run_id,ts,host,candidate,http_ok,payload) VALUES(?,?,?,?,?,?)',('run','2026-10-19T00:00:00+00:00','devxiao1.local','http://devxiao1.local',1,json.dumps(payload)))
        db.commit();db.close()
        result=make_report(p,'run')
        assert result['nodes']['devxiao1.local']['last_metrics']['drift_ppm']==1.1
        assert (p/'stability_report.md').exists()

if __name__=='__main__':
    test_events();test_report();print('MANAGER TESTS PASSED')

def test_ip_fallback():
    import sensorforge_test_manager as m
    original_resolve,original_call=m.resolve,m.call
    try:
        m.resolve=lambda host:[]
        j={'ok':True,'nodes':[{'local':True,'hostname':'devxiao3','ip':'192.168.1.22'}], 'espnow_time_local':{}}
        m.call=lambda host,ip,cfg:(j,None,2,100,'http://'+str(ip or host))
        attempts,ips,route,payload=m.poll('devxiao3.local',{'fallback_ips':{'devxiao3.local':'192.168.1.22'}},{})
        assert route=='fallback' and payload==j and not ips
        m.call=lambda host,ip,cfg:({'ok':True,'nodes':[{'local':True,'hostname':'devxiao2'}]},None,2,100,'http://x')
        attempts,ips,route,payload=m.poll('devxiao3.local',{'fallback_ips':{'devxiao3.local':'192.168.1.22'}},{})
        assert payload is None and attempts[0]['error'].startswith('IDENTITY_MISMATCH')
    finally:m.resolve,m.call=original_resolve,original_call

test_ip_fallback()
print('FALLBACK TEST PASSED')
