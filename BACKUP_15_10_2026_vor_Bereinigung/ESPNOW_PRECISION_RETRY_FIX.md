# ESP-NOW precision retry after failed series

## Reproduced defect
A completed failed quality series was delayed 60/120/180 seconds depending on consecutive failures, and a timed-out series was delayed 120/180 seconds. Precision clock freshness is only 120 seconds. This made a valid clock age out while waiting to retry, even while reporting/heartbeat traffic remained healthy.

## Change
- Successful series: retain a 60-second interval.
- Rejected or timed-out series: retry after 12–24 seconds, with a node-boot- and retry-dependent phase. Preserve the adaptive 16/32/64 probe count and 250 us quality gate.
- Does NOT extend the 120-second validity time or promote a rejected series to precision time.
- Does NOT change the four timestamp protocol, packet wire format, HMAC, grant/revoke or capture priority.

## Limits and test
- This addresses *unnecessarily long gaps between attempts*, not the underlying source of radio/software timestamp jitter.
- A run of repeated bad series can still cause fallbacks. More retry traffic with three nodes can itself create contention; measure this on hardware.
- Flash all XIAO nodes; Coordinator flash not necessary for this file-only node scheduler change. Preserve the Coordinator-side current baseline and all previous patches.
- Static review completed; full Arduino compilation and hardware verification not performed.
