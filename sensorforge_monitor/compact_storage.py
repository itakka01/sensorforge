"""Independent, optional compact long-run telemetry store.

Full fidelity remains in stability.sqlite3. Samples preserve cumulative counters
at 60 s (configurable); events are never downsampled and keep poll timestamps.
"""
import datetime as dt
import json
import sqlite3

METRICS = (
    'active', 'channel', 'probes_sent', 'exchanges_complete', 'exchange_timeouts',
    'series_accepted', 'series_rejected', 'series_reject_invalid',
    'series_reject_spread', 'series_reject_rtt', 'series_reject_deadline',
    'series_consecutive_failures', 'series_last_failure_reason',
    'last_series_spread_us', 'last_series_rtt_min_us', 'last_series_rtt_max_us',
    'last_series_selected', 'last_accepted_age_ms', 'precision_clock_active',
    'precision_recently_accepted', 'precision_sample_age_ms', 'last_rtt_us',
    'drift_ppm', 'drift_uncertainty_ppm', 'drift_valid', 'replay',
    'rx_queue_delay_max_us', 'tx_callback_delay_max_us', 'tx_callback_failed',
    'dropped', 'wifi_ps_mode', 'precision_guard_ref_lifetime',
    'precision_guard_estimate_age', 'precision_guard_other', 'source_changes',
)

class CompactRecorder:
    def __init__(self, path, sample_seconds=60):
        self.conn = sqlite3.connect(str(path))
        self.conn.execute('PRAGMA journal_mode=DELETE')
        self.conn.execute('CREATE TABLE IF NOT EXISTS samples('
                          'ts TEXT, host TEXT, http_ok INTEGER, source TEXT, '
                          'firmware TEXT, cpu_temp_c REAL, free_heap_bytes INTEGER, '
                          'boot_id TEXT, coordinator_epoch TEXT, time_source TEXT, '
                          'cluster_utc_us INTEGER, metrics_json TEXT, '
                          'PRIMARY KEY(ts,host))')
        self.conn.execute('CREATE TABLE IF NOT EXISTS events('
                          'ts TEXT, host TEXT, kind TEXT, before_value TEXT, after_value TEXT)')
        self.conn.execute('CREATE INDEX IF NOT EXISTS events_time ON events(ts)')
        self.interval = max(10.,float(sample_seconds))
        self.last = {}

    def observe(self, ts, host, payload):
        data = payload or {}
        esp = data.get('espnow_time_local') or {}
        clock = data.get('cluster_time') or {}
        source = clock.get('source') or ''
        state = {'http_ok': bool(payload), 'source': source,
                 'boot': str(data.get('boot_id') or data.get('boot_id_local') or ''),
                 'epoch': str(data.get('coordinator_epoch') or ''),
                 'accepted': esp.get('series_accepted'), 'rejected': esp.get('series_rejected'),
                 'timeouts': esp.get('exchange_timeouts'), 'replay': esp.get('replay'),
                 'precision': bool(esp.get('precision_clock_active'))}
        now = dt.datetime.fromisoformat(ts.replace('Z','+00:00')).timestamp()
        previous = self.last.get(host)
        if previous:
            old = previous['state']
            for key in ('http_ok','source','boot','epoch','precision'):
                if old[key] != state[key]:
                    self.conn.execute('INSERT INTO events VALUES(?,?,?,?,?)',
                                      (ts,host,key,str(old[key]),str(state[key])))
            for key in ('accepted','rejected','timeouts','replay'):
                a,b=old.get(key),state.get(key)
                if isinstance(a,(int,float)) and isinstance(b,(int,float)) and b!=a:
                    self.conn.execute('INSERT INTO events VALUES(?,?,?,?,?)',
                                      (ts,host,key,str(a),str(b)))
        if not previous or now-previous['sample_time'] >= self.interval:
            resources=data.get('resources') or []
            r=resources[0] if resources and isinstance(resources[0],dict) else {}
            firmware=data.get('release') or data.get('firmware_version') or r.get('release')
            temp=data.get('cpu_temp_deci_c')
            if temp is None:temp=r.get('cpu_temp_deci_c')
            heap=data.get('free_heap_bytes')
            if heap is None:heap=r.get('free_heap_bytes')
            self.conn.execute('INSERT OR REPLACE INTO samples VALUES(?,?,?,?,?,?,?,?,?,?,?,?)',
                              (ts,host,int(bool(payload)),source,firmware,
                               temp/10 if isinstance(temp,(int,float)) else None,
                               heap,state['boot'],state['epoch'],
                               str(esp.get('active_time_source') or ''),
                               clock.get('utc_us'),
                               json.dumps({k:esp[k] for k in METRICS if k in esp},separators=(',',':'))))
            sample_time=now
        else:
            sample_time=previous['sample_time']
        self.last[host]={'sample_time':sample_time,'state':state}

    def commit(self): self.conn.commit()
    def close(self): self.conn.commit();self.conn.close()
