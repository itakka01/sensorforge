"""Regression: commands must work when stdin.isatty() is False."""
import io, queue, sys
import sensorforge_test_manager as manager
from live_dashboard import Dashboard

old_stdin = sys.stdin
try:
    sys.stdin = io.StringIO('f\nn\ns\np\np\nq\n')
    manager.STOP_EVENT.clear()
    manager.STOP = False
    manager.terminal_stop_listener()
    dash = Dashboard()
    snapshots=[]
    paused=manager.drain_commands(dash,{},False,snapshots)
    assert dash.only_warnings is True
    assert len(snapshots)==1
    assert paused is False
    assert manager.STOP_EVENT.is_set()
finally:
    sys.stdin=old_stdin
    manager.STOP_EVENT.clear()
    manager.STOP=False
    while True:
        try: manager.COMMANDS.get_nowait()
        except queue.Empty: break
print('KEYBOARD COMMAND TESTS PASSED')
