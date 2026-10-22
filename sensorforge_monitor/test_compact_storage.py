import json,sqlite3,tempfile,pathlib,unittest
from compact_storage import CompactRecorder

class TestCompact(unittest.TestCase):
 def test_sampling_and_events(self):
  with tempfile.TemporaryDirectory() as t:
   path=pathlib.Path(t)/'compact.sqlite3';rec=CompactRecorder(path,60)
   base={'release':'v87','coordinator_epoch':10,'cluster_time':{'source':'espnow_precision','utc_us':1200000},'espnow_time_local':{'series_accepted':10,'series_rejected':0,'last_series_spread_us':52}}
   rec.observe('2026-10-22T10:00:00+00:00','c001',base)
   bad=json.loads(json.dumps(base));bad['cluster_time']['source']='wifi_coordinator';bad['espnow_time_local']['series_rejected']=1
   rec.observe('2026-10-22T10:00:10+00:00','c001',bad)
   rec.observe('2026-10-22T10:01:00+00:00','c001',bad)
   rec.commit();rec.close();db=sqlite3.connect(path)
   assert db.execute('select count(*) from samples').fetchone()[0]==2
   ev=db.execute('select kind from events').fetchall()
   assert ('source',) in ev and ('rejected',) in ev
   assert db.execute('select firmware from samples limit 1').fetchone()[0]=='v87'
   db.close()
if __name__=='__main__': unittest.main()
