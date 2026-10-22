"""Startup dialog and read-only overview checks; no real network needed."""
import contextlib
import io
from unittest.mock import patch
import sensorforge_test_manager as m


def test_prompts_preserve_defaults():
    cfg={'cluster_id':'cl-test','seed_host':'devxiao3.local','interval_seconds':10,
         'timeout_seconds':5,'discovery_interval_seconds':30,
         'auth':{'username':'admin','password':'secret'}}
    with patch('builtins.input',side_effect=['']*6),patch('getpass.getpass',return_value=''):
        result=m.startup_config(cfg)
    assert result==cfg


def test_preview():
    cfg={'cluster_id':'cl-test','seed_host':'devxiao3.local','auth':{}}
    sample={'ok':True,'cluster_id':'cl-test','cluster_name':'Testcluster','nodes':[
      {'local':True,'integration_id':'sf-aaa','hostname':'devxiao3','online':True,
       'ip':'192.168.0.2','cluster_role':'coordinator'},
      {'integration_id':'sf-bbb','hostname':'c001','online':True,
       'ip':'192.168.0.3','cluster_role':'node'}]}
    buf=io.StringIO()
    with patch.object(m,'fetch_identity',return_value=(sample,None,1,200)),contextlib.redirect_stdout(buf):
        assert m.preview_cluster(cfg)
    assert 'c001' in buf.getvalue() and 'devxiao3' in buf.getvalue()
    sample['cluster_id']='cl-wrong'
    with patch.object(m,'fetch_identity',return_value=(sample,None,1,200)),contextlib.redirect_stdout(io.StringIO()):
        assert not m.preview_cluster(cfg)


if __name__=='__main__':
    test_prompts_preserve_defaults();test_preview();print('STARTUP TESTS PASSED')
