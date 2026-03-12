# Runtime Logs and Troubleshooting

This document explains the most important log patterns seen in real Hunter runs.

## Interpreting the sample startup log

The observed run shows this pattern:

- startup succeeds
- raw and cache config loading succeeds
- censorship is detected
- workers start normally
- import command fails once due to missing file path target
- export command succeeds
- many validation attempts fail with curl and engine errors
- balancers remain empty because no backend has passed yet

That pattern means the system is alive, but current candidate configs are not producing successful proxy sessions yet.

## Common log lines and what they mean

### `[RawFiles] Added 47290 new configs to database`

Meaning:

- seed files were parsed successfully
- deduplication ran
- the database now contains new candidate configs

Not a problem.

### `[Censorship] *** CENSORSHIP DETECTED ***`

Meaning:

- direct connectivity checks to known public endpoints failed
- Hunter considers the current network filtered/restricted
- DPI-evasion strategy remains active

Expected in restricted environments.

### `[Startup] Main balancer :10808 (empty, waiting for first scan)`

Meaning:

- the balancer exists
- but no healthy backend has been validated yet

This is normal at the beginning of a run.

### `CLI import_config_file: import_file_not_found`

Meaning:

- the import command reached the CLI
- but the given file path did not exist from the backend working directory perspective

Fix:

- use a full Windows path
- verify the file exists before import
- avoid passing just a folder path

Example valid import path:

```text
D:\projects\v2ray\pythonProject1\hunter\config\downloaded_configs.txt
```

### `CLI export_config_db: exported 100673 configs to D:\HUNTER_config_db_export.txt`

Meaning:

- export feature worked correctly
- the entire DB was written successfully

This is a successful operation.

## Curl error reference

The log contains repeated curl error codes. These are often more useful than the short text alone.

### `error=28 (Timeout was reached)`

Most likely causes:

- backend proxy process started but cannot reach the remote destination
- remote server is dead or blocked
- TLS/session never completed in time
- test timeout is too short for a poor route

Interpretation:

- common and expected when scanning large noisy config sets
- not enough by itself to prove a bug in Hunter

### `error=97 (proxy handshake error)`

Most likely causes:

- local proxy process did not speak the expected SOCKS protocol correctly for this test
- engine failed to fully establish the outbound before curl connected
- config is malformed or unsupported for that engine

Interpretation:

- strong sign that the candidate config/engine combination is unusable
- if it happens on nearly every config for one engine only, inspect that engine’s generated runtime config path and startup output

### `error=35 (SSL connect error)`

Most likely causes:

- TLS handshake failed
- fingerprint/SNI/transport mismatch
- remote side closed the session during TLS setup

Interpretation:

- often indicates transport-level incompatibility rather than a pure timeout

### `error=52 (Server returned nothing)`

Most likely causes:

- remote peer accepted connection but returned no usable HTTP response
- upstream broke connection after handshake

Interpretation:

- partial connectivity exists, but end-to-end success is still failing

### `error=56 (Failure when receiving data from the peer)`

Most likely causes:

- connection established, then remote side aborted
- upstream path unstable or incompatible

Interpretation:

- stronger than a plain timeout; often indicates mid-stream breakage

## Engine-specific log lines

### `[sing-box:PID] FAIL ... - all tests failed`

Meaning:

- Sing-box ran for that config, but none of the HTTP/Telegram checks passed

### `[mihomo:PID] TG-ONLY ...`

Meaning:

- Telegram-specific connectivity behavior was observed but HTTP download validation still failed

### `[Test:PORT] Starting xray: "bin/xray.exe" run -c "runtime/temp_xray_test_PORT.json"`

Meaning:

- XRay test process launched for that candidate config
- the runtime temp config file path is shown

Useful for deeper inspection when debugging a single candidate.

## Why balancers stay empty

Log lines like:

- `[Balancer] No valid backends available for port 10808`
- `[Balancer] No valid backends available for port 10809`

mean:

- Hunter does not currently have any healthy config to serve through that balancer
- the balancer itself is not necessarily broken

Typical causes:

- dead scraped configs
- transport incompatibility
- strong censorship conditions
- not enough time elapsed yet for good configs to be found

## Reading worker status correctly

When you see:

- workers sleeping
- `validator` sometimes at `runs:0`
- yet many `[Test:PORT]` lines in live activity

do not assume the system is stalled.
Worker snapshots are periodic and coarse.
Always combine them with:

- live activity lines
- validated count
- db counters
- balancer cache files

## Recommended response checklist when no configs pass

1. Confirm engine binaries really exist in `bin\`
2. Verify the configured paths in **Advanced**
3. Let the system run longer than the first 2–3 minutes
4. Import a small trusted set of known-good configs
5. Watch whether imported configs move earlier through validation
6. Export the DB after the run for offline review
7. Inspect `runtime\temp_xray_test_*.json` indirectly via related log lines if engine-specific issues persist

## When to treat logs as a real bug

Investigate as a likely product bug if you see any of these patterns:

- no new log activity after startup
- no worker counters moving for a long period
- no temp engine launches at all
- import/export commands always fail despite valid paths
- status file stops updating
- crashes or abrupt process exit

Otherwise, a large amount of timeout/handshake noise during scanning is expected in this domain.
