"""Bounded, responsive SensorForge terminal diagnostics. Read-only; stdlib only."""
from __future__ import annotations
import datetime as dt
import os
import shutil
import sys
from collections import Counter, deque

class Dashboard:
    def __init__(self, page_size=12):
        self.tty = sys.stdout.isatty()
        self.page = 0
        self.page_size = max(4, min(30, int(page_size)))
        self.only_warnings = False
        self.events = deque(maxlen=6)
        self._old_lines = 0
        self.color = self.tty and not os.environ.get('NO_COLOR')

    def emit(self, message):
        self.events.appendleft(message)

    def handle(self, command):
        if command == 'n': self.page += 1
        elif command == 'b': self.page = max(0, self.page - 1)
        elif command == 'f': self.only_warnings = not self.only_warnings; self.page = 0
        elif command == 'a': self.only_warnings = False; self.page = 0

    def _style(self, value, color):
        return f'\033[{color}m{value}\033[0m' if self.color else value

    @staticmethod
    def _temperature(v):
        try: return f'{float(v):.1f}°C' if v is not None else '—'
        except (TypeError, ValueError): return '—'

    @staticmethod
    def _heap(v):
        try: return f'{int(v)/1024:.0f}K' if v is not None else '—'
        except (TypeError, ValueError): return '—'

    @staticmethod
    def _split_name(node):
        name = str(node.get('name') or '?')
        suffix = str(node.get('id_suffix') or '')
        if not suffix and '@' in name:
            name, suffix = name.rsplit('@', 1)
        return name, suffix

    def render(self, devices, elapsed, duration, cycle, paused=False, interval=10, overrun=0, title='Cluster-Diagnose'):
        width = max(58, min(shutil.get_terminal_size((120, 40)).columns, 160))
        height = shutil.get_terminal_size((120, 40)).lines
        rows_on_screen = max(4, min(self.page_size, height - 20)) if self.tty else min(8, self.page_size)
        devices = list(devices)
        versions = Counter(str(v['firmware']) for v in devices if v.get('ok') and v.get('firmware'))
        # The dominant release is a reference, not proof that another release is outdated.
        coordinator_versions = [str(v['firmware']) for v in devices if v.get('ok') and v.get('is_coordinator') and v.get('firmware')]
        reference = coordinator_versions[0] if coordinator_versions else (sorted(versions, key=lambda k: (-versions[k], k))[0] if versions else None)
        for v in devices:
            v['firmware_diff'] = bool(reference and v.get('firmware') and str(v['firmware']) != reference)
        devices.sort(key=lambda v: (not (v.get('warn') or v.get('firmware_diff')), not v.get('offline'), self._split_name(v)[0]))
        online = sum(bool(v.get('ok')) for v in devices)
        precision = sum(bool(v.get('ok') and v.get('source') == 'espnow_precision') for v in devices)
        warn = sum(bool(v.get('warn') or v.get('firmware_diff')) for v in devices)
        shown = [v for v in devices if v.get('warn') or v.get('firmware_diff')] if self.only_warnings else devices
        pages = max(1, (len(shown) + rows_on_screen - 1) // rows_on_screen)
        self.page = min(self.page, pages - 1)
        chunk = shown[self.page * rows_on_screen:(self.page + 1) * rows_on_screen]
        if duration:
            fraction = min(1, elapsed/duration)
            progress = f'{elapsed/60:.1f}/{duration/60:.1f} min | Rest {max(0,duration-elapsed)/60:.1f} min | {fraction*100:.1f}%'
        else:
            fraction = None
            progress = f'{elapsed/60:.1f} min | unbegrenzte Laufzeit'
        barw = max(15, min(54, width - 18))
        filled = round(barw*fraction) if fraction is not None else int(elapsed/2) % barw
        bar = '█'*filled + '░'*(barw-filled)
        line_width = min(width, 138)
        ln = '═'*line_width
        coordinators = [v for v in devices if v.get('is_coordinator')]
        coordinator = next((v for v in coordinators if v.get('ok') and v.get('coordinator_time')), None)
        if coordinator:
            cluster_clock = f' Coordinator-Zeit: {coordinator["coordinator_time"]} (letzte Abfrage)'
        else:
            cluster_clock = ' Coordinator-Zeit: — (nicht verfügbar/ungültig)'
        mismatch = sum(v.get('firmware_diff', False) for v in devices)
        version_line = f' Firmware-Referenz (Coord./häufigste): {reference or "—"} | abweichend: {mismatch}'
        lines = [ln, f' SENSORFORGE  •  {title}  •  {"PAUSIERT" if paused else "MESSUNG AKTIV"}', ln,
                 f' Laufzeit [{bar}]', f' {progress}',
                 f' HTTP {online}/{len(devices)} | ESP-NOW-Zeit {precision} | Hinweise {warn} | Zyklus {cycle} | Überläufe {overrun}',
                 cluster_clock[:width], version_line[:width], '─'*line_width,
                 f' GERÄTE  •  {"nur Auffällige" if self.only_warnings else "alle"}  •  Seite {self.page+1}/{pages} ({len(shown)} Einträge)',
                 '─'*line_width]
        # Keep name and 8-character identity separate. Width-aware table for smaller terminals.
        if width >= 136: mode = 'wide'
        elif width >= 107: mode = 'normal'
        elif width >= 83: mode = 'compact'
        else: mode = 'small'
        if mode == 'wide':
            lines.append(' '+f'{"Node":<16} {"ID (hex)":<8} {"Firmware":<15} {"Temp":>7} {"Heap":>7} {"HTTP":<7} {"Zeitquelle":<15} {"Serien +/−":<10} {"TO+":>3} Hinweis')
        elif mode == 'normal':
            lines.append(' '+f'{"Node":<13} {"ID (hex)":<8} {"Firmware":<14} {"Temp":>7} {"Heap":>6} {"HTTP":<7} {"Zeitquelle":<14} {"Serien":<7} Hinweis')
        elif mode == 'compact':
            lines.append(' '+f'{"Node":<12} {"ID (hex)":<8} {"Firmware":<14} {"Temp":>7} {"Heap":>6} {"HTTP":<7} Zeitquelle')
        else:
            lines.append(' '+f'{"Node":<13} {"ID (hex)":<8} {"Firmware":<14} {"Temp":>6} {"Heap":>5} Status')
        for v in chunk:
            name, suffix = self._split_name(v)
            fw = str(v.get('firmware') or '—')
            if v.get('firmware_diff'): fw = '!'+fw
            status = 'OK' if v.get('ok') else ('OFFLINE' if v.get('offline') else 'FEHLER')
            src = {'espnow_precision':'ESP-NOW', 'wifi_coordinator':'WiFi-Fallback', 'local_coordinator':'Coordinator'}.get(v.get('source'), v.get('source') or '—')
            counts = f'{v.get("accepted_delta", 0)}/{v.get("rejected_delta", 0)}'
            temp = self._temperature(v.get('temperature_c'))
            heap = self._heap(v.get('free_heap_bytes'))
            reason = str(v.get('reason') or ('FW abweichend' if v.get('firmware_diff') else '—'))
            if mode == 'wide':
                text = f' {name[:16]:<16} {suffix[:8]:<8} {fw[:15]:<15} {temp:>7} {heap:>7} {status:<7} {src[:15]:<15} {counts:<10} {v.get("timeout_delta", 0):>3} {reason[:17]}'
            elif mode == 'normal':
                text = f' {name[:13]:<13} {suffix[:8]:<8} {fw[:14]:<14} {temp:>7} {heap:>6} {status:<7} {src[:14]:<14} {counts:<7} {reason[:12]}'
            elif mode == 'compact':
                text = f' {name[:12]:<12} {suffix[:8]:<8} {fw[:14]:<14} {temp:>7} {heap:>6} {status:<7} {src[:13]}'
            else:
                text = f' {name[:13]:<13} {suffix[:8]:<8} {fw[:14]:<14} {temp:>6} {heap:>5} {status}'
            if v.get('warn') or v.get('firmware_diff'): text = self._style(text, '33' if v.get('ok') else '31')
            else: text = self._style(text, '32')
            lines.append(text)
        if not chunk: lines.append('  Keine Geräte in dieser Ansicht.')
        lines += ['─'*line_width, ' EREIGNISSE (letzte 4)']
        lines += ['  '+msg[:width-3] for msg in list(self.events)[:4]] or ['  Keine neuen Ereignisse.']
        lines += ['─'*line_width,
                  ' STEUERUNG [q] Beenden [s] Bericht [p] Pause [n/b] Seite',
                  '           [f] Auffällige [a] Alle  •  jeweils + Enter', ln]
        output = '\n'.join(lines) + '\n'
        if self.tty:
            sys.stdout.write('\033[H\033[2J' + output)
        else:
            sys.stdout.write(f'Cycle {cycle} | {progress} | HTTP {online}/{len(devices)} | ESP-NOW {precision} | Hinweise {warn} | FW abweichend {mismatch} | Überläufe {overrun}\n')
        sys.stdout.flush()
