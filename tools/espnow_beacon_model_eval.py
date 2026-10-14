#!/usr/bin/env python3
"""SensorForge: one-step-ahead ESP-NOW beacon clock-model evaluation.

Usage: python3 tools/espnow_beacon_model_eval.py csv1 [csv2 ...]
Requires only Python 3 standard library. Read-only, no firmware changes.
"""
import csv
import math
import statistics
import sys
from pathlib import Path


def read(path):
    with open(path, newline='', encoding='utf-8-sig') as file:
        rows = list(csv.DictReader(file))
    needed = ('coordinator_mono_low', 'node_rx_callback_us', 'coordinator_boot', 'epoch')
    if not rows or any(k not in rows[0] for k in needed):
        raise ValueError(f'{path}: empty or missing CSV columns')
    return rows


def predict(data, window=12, mode='ols'):
    errors = []
    for i in range(4, len(data)):
        cur = data[i]
        prior = [r for r in data[max(0, i-window):i]
                 if (r['coordinator_boot'], r['epoch']) == (cur['coordinator_boot'], cur['epoch'])]
        if len(prior) < 4:
            continue
        x0 = int(prior[0]['coordinator_mono_low'])
        y0 = int(prior[0]['node_rx_callback_us'])
        # Unwrap sender monotonic 32-bit counter within the window.
        xs = [float((int(r['coordinator_mono_low']) - x0) & 0xffffffff) for r in prior]
        ys = [float(int(r['node_rx_callback_us']) - y0) for r in prior]
        target_x = float((int(cur['coordinator_mono_low']) - x0) & 0xffffffff)
        if not all(xs[j] > xs[j-1] for j in range(1, len(xs))) or target_x <= xs[-1]:
            continue
        if mode == 'ols':
            mx = statistics.mean(xs); my = statistics.mean(ys)
            denom = sum((x-mx)**2 for x in xs)
            if denom == 0: continue
            slope = sum((x-mx)*(y-my) for x,y in zip(xs,ys)) / denom
            intercept = my - slope*mx
        elif mode == 'median-pair':
            slopes = [(ys[j]-ys[k])/(xs[j]-xs[k]) for j in range(1,len(xs))
                      for k in range(j) if xs[j] != xs[k]]
            if not slopes: continue
            slope = statistics.median(slopes)
            intercept = statistics.median(y-slope*x for x,y in zip(xs,ys))
        else:
            raise ValueError(mode)
        if not (0.9995 <= slope <= 1.0005):
            continue
        estimate = y0 + intercept + slope*target_x
        errors.append(int(cur['node_rx_callback_us']) - estimate)
    return errors


def stats(errors):
    if not errors: return 'no valid one-step-ahead predictions'
    rmse = math.sqrt(sum(e*e for e in errors)/len(errors))
    mae = sum(abs(e) for e in errors)/len(errors)
    return f'n={len(errors)} RMSE={rmse:.0f}us MAE={mae:.0f}us max_abs={max(map(abs,errors)):.0f}us'


def main(paths):
    if not paths:
        print('Usage: python3 espnow_beacon_model_eval.py csv1 [csv2 ...]', file=sys.stderr)
        return 2
    for path in paths:
        rows = read(path)
        print(f'\n{Path(path).name}: {len(rows)} rows')
        if len(rows) < 20:
            print('WARNING: short data set; window comparisons are not statistically robust.')
        for window in (4,6,8,12):
            for mode in ('ols','median-pair'):
                print(f'{mode:11} window={window:2} {stats(predict(rows,window,mode))}')
        print('NOTE: these are local receive-time prediction errors, NOT camera capture skew.')
    return 0

if __name__ == '__main__':
    try: sys.exit(main(sys.argv[1:]))
    except (ValueError, OSError) as exc:
        print(f'Error: {exc}',file=sys.stderr)
        sys.exit(1)
