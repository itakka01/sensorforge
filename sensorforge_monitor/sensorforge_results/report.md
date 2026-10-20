# SensorForge cluster measurement report

Created UTC: 2026-10-19T07:27:55.948+00:00
Recorded samples: 11405

**WARNING:** This monitor evaluates operation, link health, estimator spread, and handover. It **does not** measure real relative GPIO synchronisation. A hardware GPIO capture test is still required to verify <=250 µs.

## devfreenove1
- Role: coordinator; mode: normal; boot: 96bca8c2
- HTTP availability: 2279/2281 polls (99.9%)
- Latest estimator spread: n/a (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: n/a / n/a / n/a µs (repeated estimates possible)
- Precision recently accepted: False; last accepted age: 0 ms
- Exchanges completed: 0; timeouts: 0
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 48206/42358; grants authenticated: 306

## devxiao1
- Role: node; mode: normal; boot: 01bbbf98
- HTTP availability: 2278/2281 polls (99.9%)
- Latest estimator spread: 32.0 µs (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: 2.5 / 31.5 / 1270.5 µs (repeated estimates possible)
- Precision recently accepted: True; last accepted age: 26752 ms
- Exchanges completed: 9792; timeouts: 0
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 67024/45946; grants authenticated: 0

## devxiao2
- Role: node; mode: normal; boot: 6546ad35
- HTTP availability: 2281/2281 polls (100.0%)
- Latest estimator spread: 30.0 µs (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: 2.0 / 31.0 / 3339.0 µs (repeated estimates possible)
- Precision recently accepted: True; last accepted age: 9341 ms
- Exchanges completed: 9792; timeouts: 3
- Broadcaster lease active: False; production beacons sent: 0; recent TX rate: 0.00 Hz
- ESP-NOW received/authenticated: 67677/45839; grants authenticated: 0

## devxiao3
- Role: node; mode: time_broadcaster; boot: a8253250
- HTTP availability: 360/2461 polls (14.6%)
- Latest estimator spread: 13.0 µs (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: 13.0 / 63.5 / 206.5 µs (repeated estimates possible)
- Precision recently accepted: True; last accepted age: 23149 ms
- Exchanges completed: 208; timeouts: 0
- Broadcaster lease active: True; production beacons sent: 5863; recent TX rate: n/a
- ESP-NOW received/authenticated: 1724/1378; grants authenticated: 306

## devxiao3.local
- Role: None; mode: None; boot: None
- HTTP availability: 0/2101 polls (0.0%)
- Latest estimator spread: n/a (quality threshold 250 µs; not GPIO accuracy)
- Observed spread min / median / max across polling snapshots: n/a / n/a / n/a µs (repeated estimates possible)
- Precision recently accepted: None; last accepted age: None ms
- Exchanges completed: None; timeouts: None
- Broadcaster lease active: None; production beacons sent: None; recent TX rate: n/a
- ESP-NOW received/authenticated: None/None; grants authenticated: None

## Alerts / checks
- devxiao3: HTTP availability below 80%
- devxiao3.local: HTTP availability below 80%

## Limitations
- last_series_spread_us is dispersion of selected offset estimates, NOT GPIO synchronization accuracy.
- clock uncertainty_estimate_us belongs to WiFi coordinator time, NOT a measured GPIO offset.
- HTTP response times and independently polled utc_us are NOT simultaneous clock measurements.
- A production beacon TX counter does not confirm successful reception by peers.
- Precision acceptance on one node does not prove inter-node GPIO skew <=250us.
- Rates are computed only within the same boot_id and nondecreasing counters.
