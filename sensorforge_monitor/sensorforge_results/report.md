# SensorForge cluster measurement report

Created UTC: 2026-10-18T23:10:37.801+00:00
Recorded samples: 155

**WARNING:** This monitor evaluates operation, link health, estimator spread, and handover. It **does not** measure real relative GPIO synchronisation. A hardware GPIO capture test is still required to verify <=250 µs.

## devfreenove1
- Role: coordinator; mode: normal; boot: e35f98de
- HTTP availability: 31/31 polls (100.0%)
- Latest estimator spread: n/a (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: n/a / n/a / n/a µs (repeated estimates possible)
- Precision recently accepted: False; last accepted age: 0 ms
- Exchanges completed: 0; timeouts: 0
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 1577/248; grants authenticated: 69

## devxiao1
- Role: node; mode: normal; boot: c9431fe3
- HTTP availability: 31/31 polls (100.0%)
- Latest estimator spread: n/a (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: n/a / n/a / n/a µs (repeated estimates possible)
- Precision recently accepted: False; last accepted age: 0 ms
- Exchanges completed: 6; timeouts: 29
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 1605/1385; grants authenticated: 0

## devxiao2
- Role: node; mode: normal; boot: 52778133
- HTTP availability: 31/31 polls (100.0%)
- Latest estimator spread: n/a (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: n/a / n/a / n/a µs (repeated estimates possible)
- Precision recently accepted: False; last accepted age: 0 ms
- Exchanges completed: 3; timeouts: 18
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 1391/1193; grants authenticated: 0

## devxiao3
- Role: node; mode: time_broadcaster; boot: 67d0d01c
- HTTP availability: 62/62 polls (100.0%)
- Latest estimator spread: 122.0 µs (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: 47.5 / 47.5 / 122.0 µs (repeated estimates possible)
- Precision recently accepted: True; last accepted age: 33833 ms
- Exchanges completed: 48; timeouts: 0
- Broadcaster lease active: True; production beacons sent: 1336; recent TX rate: n/a
- ESP-NOW received/authenticated: 366/301; grants authenticated: 69

## Alerts / checks
- No rule-based alarms triggered; this is not proof of GPIO accuracy.

## Limitations
- last_series_spread_us is dispersion of selected offset estimates, NOT GPIO synchronization accuracy.
- clock uncertainty_estimate_us belongs to WiFi coordinator time, NOT a measured GPIO offset.
- HTTP response times and independently polled utc_us are NOT simultaneous clock measurements.
- A production beacon TX counter does not confirm successful reception by peers.
- Precision acceptance on one node does not prove inter-node GPIO skew <=250us.
- Rates are computed only within the same boot_id and nondecreasing counters.
