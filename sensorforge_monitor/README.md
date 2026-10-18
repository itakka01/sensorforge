# SensorForge Cluster Monitor

Read-only, auto-discovering cluster monitor using `/cluster_status`. Python 3.10+; no mandatory third-party libraries.

## Setup and run

```bash
python sensorforge_monitor.py init
python sensorforge_monitor.py run --minutes 15 --interval 10
```

Specify an existing config or output directory via `--config` / `--output`. Use `--minutes 0` to run until Ctrl+C. Regenerate reports with `python sensorforge_monitor.py report --output sensorforge_results`.

Optional graph support:

```bash
python -m pip install matplotlib
```

## Files

- `measurements.csv`: one row per host and sample, including failure rows
- `monitor.sqlite3`: all polls in SQLite; no password stored
- `report.json`: machine-readable summary/alerts, safe to share after inspecting local identifiers
- `report.md`: readable report, safe to paste into ChatGPT after reviewing identifiers
- `spread.png`, `accepted_age.png`, `exchanges.png`, `beacons.png`, `rx.png`: time-series charts if matplotlib is installed

## Discovery and scalability

The script queries one or more `seed_hosts`, reads the `nodes` array, and polls discovered IPs directly. Discovery runs periodically to find new nodes. Adjust `rediscover_every_cycles`, `timeout_seconds`, and `interval_seconds` in the JSON config. For a large cluster, use a longer interval to limit query traffic. Results distinguish coordinator, time broadcaster and normal nodes. It works with V1 or V2 JSON fields where present.

## Authentication and security

`init` optionally collects **HTTP Basic Authentication** username/password and writes them only to `sensorforge_config.json` (best-effort owner-only permissions). Credentials never appear in CSV, SQLite, or reports. **This tool does not configure cluster pairing credentials or Wi-Fi passwords**; those do not belong in the monitor. Do not share the config file. Use on a trusted LAN. If your firmware uses a web login form/session cookie instead of HTTP Basic, direct API authentication may need a firmware-specific adapter; don't send credentials through unsupported auth mechanisms.

Read-only HTTP GET; no settings changes or flash commands. Nodes remain responsible for their own time computation.

## Interpretation limits

The monitor measures **estimator quality, continuity, exchanges, error counters, lease state, and Beacon TX counts**, not physical GPIO skew. `last_series_spread_us <= 250` does **not** imply GPIO offsets <=250 microseconds. To measure the real requirement, connect a common pulse to multiple GPIO capture inputs (or use an appropriate shared-trigger capture method), then compare node timestamps for the same event and publish capture results. HTTP-delivered `utc_us` are **not simultaneous** and cannot be subtracted to obtain microsecond-level precision.

`release` may be stale because firmware metadata is independent of the wire protocol version; confirm firmware versions from build artifacts/logs separately.
