SensorForge Radar Calibration - implementation summary

Based exactly on the uploaded files:
- radar(5).h
- radar(6).cpp
- webconfig(20260910-191617).cpp

Changed files:
- radar.h
- radar.cpp
- webconfig.cpp

Behavior:
- Two independent RAM-only data sets: quiet and motion.
- Start of a measurement clears only the selected data set.
- Statistics are accumulated in radar.cpp from every standard UART report, not from browser polling.
- Per gate: samples, minimum, mean, P50, P95, P99 and absolute peak.
- Percentiles use a 0.5 dB histogram; absolute peak remains separate so isolated spikes are visible.
- WebConfig marks a peak as an isolated spike when samples >= 100 and Peak-P99 >= 6 dB.
- Comparison uses Quiet P99 vs Motion P99.
- Recommendation requires >=100 samples in both sets and >=4 dB separation.
- Recommended trigger = Quiet P99 + 25% of the separation, with a 3..8 dB margin, capped at Motion P99-2 dB and 0..95.
- The Apply button changes only trigger input fields in the browser. It does NOT write the radar. The existing Write + Verify path remains authoritative. Hold thresholds are never changed automatically.
- No calibration data is written to SD, LittleFS or NVS. Reboot clears measurements.
- Existing live radar polling, motion detection, camera, WiFi firmware update and recording logic are not modified.

Suggested field test:
1. Keep the Radar Config page open.
2. Start Ruhemessung and leave the room unchanged for about 5 minutes.
3. Stop.
4. Start Bewegungsmessung and walk the relevant paths repeatedly for about 5 minutes.
5. Stop.
6. Review P99/Peak and comparison quality.
7. Optionally apply suggested trigger values, inspect them, then use the existing Write + Verify button.
