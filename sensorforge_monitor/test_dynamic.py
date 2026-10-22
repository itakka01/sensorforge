import json, pathlib, tempfile
import sensorforge_test_manager as m
import dynamic_discovery as d

CID='cl-test'

def payload(uid,name,ip,others):
    return {'ok':True,'cluster_id':CID,'runtime_cluster_id':CID,'nodes':[
        {'local':True,'integration_id':uid,'hostname':name,'ip':ip,'boot_id':'ab', 'cluster_role':'node'},
        *[{'local':False,'integration_id':u,'hostname':n,'ip':addr} for u,n,addr in others]],
        'espnow_time_local':{'authenticated':1,'replay':0},'cluster_time':{'source':'espnow_precision'}}

u1='sf-aaaaaaaaaaaaaaaa';u2='sf-bbbbbbbbbbbbbbbb';u3='sf-cccccccccccccccc'
known={};events=[]
a=payload(u1,'xiao','10.0.0.1',[(u2,'c001','10.0.0.2')])
assert d.observe(known,a,CID,'t1',events)
assert len(known)==2
assert not d.observe(known,{**a,'cluster_id':'cl-other','runtime_cluster_id':'cl-other'},CID,'t2',events)
b=payload(u1,'xiao','10.0.0.1',[(u2,'c001','10.0.0.22'),(u3,'c001','10.0.0.3')])
assert d.observe(known,b,CID,'t3',events)
assert len(known)==3 and known[u2]['ip']=='10.0.0.22'
assert d.host_label(known[u2])!=d.host_label(known[u3])
orig=m.fetch_identity
try:
    m.fetch_identity=lambda host,cfg:(payload(u3,'c001','10.0.0.3',[]),None,1,100)
    att,route,p=m.dynamic_probe(known[u2],{'auth':{}},CID)
    assert p is None and all(x['error']=='CLUSTER_OR_IDENTITY_MISMATCH' for x in att)
finally:
    m.fetch_identity=orig
with tempfile.TemporaryDirectory() as tmp:
    db=m.init_database(pathlib.Path(tmp)/'stability.sqlite3')
    db.execute('INSERT INTO runs VALUES(?,?,?,?,?,?)',('run','a','b',10,10,json.dumps([d.host_label(known[u2])])))
    db.commit();db.close()
    report=m.make_report(tmp,'run')
    assert report['result']=='INCOMPLETE'
print('DYNAMIC TESTS PASSED')
