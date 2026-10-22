import contextlib, io
from unittest.mock import patch
from live_dashboard import Dashboard
from sensorforge_test_manager import live_device


def test_firmware_temp_heap_and_clock():
    payload = {'ok':True,'local_role':'coordinator',
        'nodes':[{'local':True,'cluster_role':'coordinator','boot_id':'b','release':'v87-beta138','cpu_temp_deci_c':582,'free_heap_bytes':17360}],
        'cluster_time':{'valid':True,'source':'local_coordinator','utc_us':1792704068445479},'espnow_time_local':{}}
    row,state = live_device('devxiao3@c7ff8e30',payload,None,True)
    assert row['name']=='devxiao3' and row['id_suffix']=='c7ff8e30'
    assert row['firmware']=='v87-beta138' and row['temperature_c']==58.2 and row['free_heap_bytes']==17360
    assert row['coordinator_time'] and row['coordinator_time'].endswith('UTC')
    second=dict(row, name='c001', id_suffix='b7dbf27e', firmware='v87-beta137',is_coordinator=False,coordinator_time=None)
    dash=Dashboard(12);dash.tty=True;dash.color=False
    with patch('live_dashboard.shutil.get_terminal_size',return_value=__import__('os').terminal_size((160,40))):
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            dash.render([row,second],30,300,3)
    output=buf.getvalue()
    assert 'Coordinator-Zeit:' in output and '58.2°C' in output and '17K' in output
    assert '!v87-beta137' in output and 'c7ff8e30' in output
    dash.handle('f')
    with patch('live_dashboard.shutil.get_terminal_size',return_value=__import__('os').terminal_size((160,40))):
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            dash.render([row,second],30,300,3)
    assert 'c001' in buf.getvalue() and 'devxiao3' not in buf.getvalue()
    for width in (60,85,115,160):
        with patch('live_dashboard.shutil.get_terminal_size',return_value=__import__('os').terminal_size((width,40))):
            with contextlib.redirect_stdout(io.StringIO()) as buf:
                dash.render([row,second],30,300,3)
        assert 'Firmware' in buf.getvalue()

if __name__=='__main__':
    test_firmware_temp_heap_and_clock();print('DIAGNOSTIC COLUMN TESTS PASSED')
