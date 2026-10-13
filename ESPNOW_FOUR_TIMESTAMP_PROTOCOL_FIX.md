# ESP-NOW four-timestamp protocol correction (v87-beta105)

## Root cause (source audit)

Prior behavior: t1=Node Probe TX (software); t2=Coordinator Probe RX callback;
t3=Coordinator timestamp sampled AFTER sending Reply, then carried in Final;
t4=Node FINAL RX callback. The pair t3/t4 therefore described the Final
transmission, not the Reply reception that completes the conventional NTP
four-timestamp exchange. Final is a second ESP-NOW frame; its TX/airtime delay
and variable node callback/scheduling time add avoidable asymmetry and noise.

Corrected behavior: t1=Node Probe TX software call; t2=Coordinator Probe RX
callback; t3=Coordinator time sampled immediately before initiating Reply send;
t4=Node Reply RX callback. Final only transports t3, authenticated and bound
via the existing nonce. Estimator still uses ((t2-t1)+(t3-t4))/2 and RTT
(t4-t1)-(t3-t2). The Final frame reception is *not* part of the timing interval.

Both Coordinator and XIAO must install this protocol fix. The packet format
and HMAC/replay framing are unchanged; the time semantics are corrected.

## Remaining limitations

- The coordinator t3 precedes encoding/HMAC and esp_now_send; it remains a
  software approximation to physical Reply transmission. The estimated
  offset can retain send-queue/airtime bias and jitter. This patch fixes the
  proven packet-pairing error, not the fundamental PHY timestamp limitation.
- Due to separate Reply and Final frames, a dropped/reordered Final makes an
  exchange incomplete; this must not be treated as a valid sample.
- The <=250 us spread quality gate is unchanged. A passed gate does *not*
  establish true inter-camera exposure simultaneity; GPIO / analyzer tests
  remain necessary.
- No change to TX callback instrumentation, Capture preemption, leases,
  Wi-Fi fallback, drift regression or web diagnostics.
- Independently observed `UTC-Zeit nicht verfuegbar` remains unresolved.

## Reproduction

Use one Coordinator + one XIAO, flash both. After boot and Drone Mode on the
node, compare complete exchanges, valid-series rate, selected RTT and spread
across at least 10 min. Preserve accepted/rejected reports. Later measure
at least two Nodes with externally captured GPIO edges.

## Validation scope

Source-level pairing verified against latest available TX instrumentation
patch. Small host-level synthetic interval checks and ZIP integrity checked.
No complete Arduino ESP32 build or hardware validation was run.
