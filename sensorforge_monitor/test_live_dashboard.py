import contextlib, io
from live_dashboard import Dashboard
from sensorforge_test_manager import live_device

def test_hundred_nodes_bounded():
    dash=Dashboard(12)
    dash.tty=True; dash.color=False
    nodes=[{'name':f'node-{n:03d}','ok':True,'offline':False,'source':'espnow_precision','warn':False,'reason':'','accepted_delta':1,'rejected_delta':0,'timeout_delta':0} for n in range(125)]
    nodes[37].update(ok=False,offline=True,warn=True,reason='HTTP nicht erreichbar')
    with contextlib.redirect_stdout(io.StringIO()) as output:
        dash.render(nodes,80,300,8)
    text=output.getvalue()
    assert '125' in text and 'node-037' in text and text.count('node-') <= 12
    dash.handle('n')
    assert dash.page == 1
    dash.handle('f')
    with contextlib.redirect_stdout(io.StringIO()) as output:
        dash.render(nodes,90,300,9)
    assert 'node-037' in output.getvalue()
    assert 'node-000' not in output.getvalue()

def test_series_deltas():
    entry='c001@b7dbf27e'
    payload={'nodes':[{'local':True,'boot_id':'b1','cluster_role':'node'}], 'cluster_time':{'source':'espnow_precision'},'espnow_time_local':{'series_accepted':3,'series_rejected':1,'exchange_timeouts':0}}
    row,state=live_device(entry,payload,None,True)
    assert row['accepted_delta']==0 and row['rejected_delta']==0
    payload['espnow_time_local']['series_accepted']=4
    row,state=live_device(entry,payload,state,True)
    assert row['accepted_delta']==1
    payload['espnow_time_local']['series_rejected']=2
    row,state=live_device(entry,payload,state,True)
    assert row['rejected_delta']==1 and row['warn']

if __name__ == '__main__':
    test_hundred_nodes_bounded();test_series_deltas();print('LIVE DASHBOARD TESTS PASSED')
