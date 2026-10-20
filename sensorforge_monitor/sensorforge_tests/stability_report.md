# SensorForge stability test
Run: 2026-10-19T22:47:50.972+00:00
Start: 2026-10-19T22:47:50.972+00:00 | End: 2026-10-19T23:47:51.037+00:00
Overall: **FAIL**

## devfreenove1.local — PASS_MONITORING_ONLY
HTTP cycles 360/360; boots 1; epochs 1
Time sources: local_coordinator
Incidents: 0
Latest: probes=0, exchanges=0, replay=0, drift_ppm=0.0


## devxiao1.local — FAIL
HTTP cycles 359/360; boots 1; epochs 1
Time sources: espnow_precision, wifi_coordinator
Incidents: 379
Latest: probes=1395, exchanges=1392, replay=0, drift_ppm=2.7196

- {'type': 'counter_increase', 'at': '2026-10-19T22:48:00.974+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:10.974+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:20.974+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:30.974+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:40.974+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:50.975+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:50.975+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:00.975+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:10.975+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:20.975+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:30.975+00:00', 'counter': 'rejected', 'delta': 28.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:40.975+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:50.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:00.976+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:10.976+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:20.976+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:30.976+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:40.977+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:50.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:00.977+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:10.977+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:20.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:30.977+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:40.978+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:50.978+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:50.978+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:00.978+00:00', 'counter': 'rejected', 'delta': 29.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:10.978+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:20.978+00:00', 'counter': 'rejected', 'delta': 32.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:30.978+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:40.979+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:50.979+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:00.979+00:00', 'counter': 'rejected', 'delta': 29.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:10.979+00:00', 'counter': 'rejected', 'delta': 29.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:20.979+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:20.979+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:30.979+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:40.980+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:50.980+00:00', 'counter': 'rejected', 'delta': 31.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:00.980+00:00', 'counter': 'rejected', 'delta': 11.0}
- … 339 more (see JSON)

## devxiao2.local — PASS_MONITORING_ONLY
HTTP cycles 360/360; boots 1; epochs 1
Time sources: espnow_precision, wifi_coordinator
Incidents: 381
Latest: probes=1352, exchanges=1349, replay=0, drift_ppm=5.0667

- {'type': 'counter_increase', 'at': '2026-10-19T22:48:00.974+00:00', 'counter': 'rejected', 'delta': 28.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:10.974+00:00', 'counter': 'rejected', 'delta': 8.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:20.974+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:30.974+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:40.974+00:00', 'counter': 'rejected', 'delta': 8.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:50.975+00:00', 'counter': 'rejected', 'delta': 31.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:50.975+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:00.975+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:10.975+00:00', 'counter': 'rejected', 'delta': 25.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:20.975+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:30.975+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:40.975+00:00', 'counter': 'rejected', 'delta': 24.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:50.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:00.976+00:00', 'counter': 'rejected', 'delta': 32.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:10.976+00:00', 'counter': 'rejected', 'delta': 6.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:20.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:30.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:40.977+00:00', 'counter': 'rejected', 'delta': 24.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:50.977+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:00.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:10.977+00:00', 'counter': 'rejected', 'delta': 8.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:20.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:30.977+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:40.978+00:00', 'counter': 'rejected', 'delta': 8.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:50.978+00:00', 'counter': 'rejected', 'delta': 32.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:50.978+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:00.978+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:10.978+00:00', 'counter': 'rejected', 'delta': 12.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:20.978+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:30.978+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:40.979+00:00', 'counter': 'rejected', 'delta': 25.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:50.979+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:00.979+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:10.979+00:00', 'counter': 'rejected', 'delta': 24.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:10.979+00:00', 'counter': 'source_changes', 'delta': 1.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:20.979+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:30.979+00:00', 'counter': 'rejected', 'delta': 31.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:40.980+00:00', 'counter': 'rejected', 'delta': 8.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:50.980+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:00.980+00:00', 'counter': 'rejected', 'delta': 14.0}
- … 341 more (see JSON)

## devxiao3.local — PASS_MONITORING_ONLY
HTTP cycles 360/360; boots 1; epochs 1
Time sources: wifi_coordinator
Incidents: 359
Latest: probes=1445, exchanges=1445, replay=0, drift_ppm=-20.5335

- {'type': 'counter_increase', 'at': '2026-10-19T22:48:00.974+00:00', 'counter': 'rejected', 'delta': 9.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:10.974+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:20.974+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:30.974+00:00', 'counter': 'rejected', 'delta': 27.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:40.974+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:48:50.975+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:00.975+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:10.975+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:20.975+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:30.975+00:00', 'counter': 'rejected', 'delta': 12.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:40.975+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:49:50.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:00.976+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:10.976+00:00', 'counter': 'rejected', 'delta': 9.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:20.976+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:30.976+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:40.977+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:50:50.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:00.977+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:10.977+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:20.977+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:30.977+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:40.978+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:51:50.978+00:00', 'counter': 'rejected', 'delta': 33.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:00.978+00:00', 'counter': 'rejected', 'delta': 31.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:10.978+00:00', 'counter': 'rejected', 'delta': 18.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:20.978+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:30.978+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:40.979+00:00', 'counter': 'rejected', 'delta': 30.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:52:50.979+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:00.979+00:00', 'counter': 'rejected', 'delta': 29.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:10.979+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:20.979+00:00', 'counter': 'rejected', 'delta': 18.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:30.979+00:00', 'counter': 'rejected', 'delta': 14.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:40.980+00:00', 'counter': 'rejected', 'delta': 13.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:53:50.980+00:00', 'counter': 'rejected', 'delta': 15.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:00.980+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:10.981+00:00', 'counter': 'rejected', 'delta': 11.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:20.981+00:00', 'counter': 'rejected', 'delta': 16.0}
- {'type': 'counter_increase', 'at': '2026-10-19T22:54:30.981+00:00', 'counter': 'rejected', 'delta': 9.0}
- … 319 more (see JSON)

## Network: devfreenove1.local
{'checks': 360, 'dns_failures': 0, 'fallback_successes': 0, 'http_failures': 0, 'routes': {'dns': 360, 'fallback': 0, 'none': 0, 'hostname': 0}}

## Network: devxiao1.local
{'checks': 360, 'dns_failures': 0, 'fallback_successes': 0, 'http_failures': 1, 'routes': {'dns': 359, 'fallback': 0, 'none': 1, 'hostname': 0}}

## Network: devxiao2.local
{'checks': 360, 'dns_failures': 0, 'fallback_successes': 0, 'http_failures': 0, 'routes': {'dns': 360, 'fallback': 0, 'none': 0, 'hostname': 0}}

## Network: devxiao3.local
{'checks': 360, 'dns_failures': 0, 'fallback_successes': 0, 'http_failures': 0, 'routes': {'dns': 360, 'fallback': 0, 'none': 0, 'hostname': 0}}
