# Build, Commands, and Artifacts

## Important Realtime Commands

The native app communicates with the orchestrator using runtime commands. The most important commands currently exposed by the orchestrator are:

- `pause`
- `resume`
- `speed_profile`
- `set_speed`
- `set_threads`
- `set_timeout`
- `clear_old`
- `add_configs`
- `import_config_file`
- `export_config_db`
- `run_cycle`
- `refresh_ports`
- `reprovision_ports`
- `load_raw_files`
- `load_bundle_files`
- `detect_censorship`
- `update_runtime_settings`
- `get_status`
- `stop`
- `ping`

## Common command results

### Import / export

- `import_file_not_found`
- `imported X configs, promoted Y existing, invalid Z`
- `exported X configs to PATH`
- `export_failed`

### Maintenance / scan

- `raw_loaded_N`
- `bundle_loaded_N`
- `cycle_completed`
- `cycle_skipped`
- `ports_refreshed`
- `ports_reprovisioned`
- `censored`
- `open`

## Current build outputs

### C++

Built artifacts are typically generated here:

- `hunter_cpp\build\hountersansor.exe`
- `hunter_cpp\build\hountersansor_cli.exe`
- `hunter_cpp\build\hunter_tests.exe`

## Build notes

### C++ backend

Project helper script:

- `hunter_cpp\build.bat`

The script configures the MSYS2 UCRT64 toolchain and runs a Ninja build.

## Test notes

C++ unit tests cover:

- utils
- models
- URI parser
- ConfigDatabase
- priority promotion for imported configs

Run from the backend build directory or by executing the built test binary:

```text
hunter_cpp\build\hunter_tests.exe
```

## Recommended artifact handling

- keep the built native app and console app together with the `bin\`, `config\`, and `runtime\` directories
- archive exported DB snapshots after successful long runs
- keep a known-good export for rollback/testing
- if you want reproducible user imports, store them as named text files and import them explicitly instead of pasting large blobs into the UI
