# Hunter Architecture Overview

## Components

Hunter has two main applications:

- `hountersansor.exe`
  - The native C++ Dear ImGui application.
  - Hosts the main monitoring and control interface for everyday use and advanced operations.

- `hountersansor_cli.exe`
  - The C++ orchestrator and scanning engine.
  - Owns discovery, validation, balancers, cache files, runtime status, and proxy process lifecycle.

## Runtime Communication Model

The native app and backend use a hybrid control model:

- **Primary control channel**
  - The native app can invoke orchestrator actions directly.
  - Realtime commands and monitor snapshots are also available over websocket.

- **Secondary file-based state**
  - The CLI continuously writes runtime artifacts into `runtime\`.
  - The native app can read those files for richer state like config lists, balancer caches, and full status snapshots.

## Main Runtime Files

These files are the authoritative runtime outputs the native app reads:

- `runtime\HUNTER_status.json`
  - Main status snapshot.
  - Includes phase, counters, alive configs, speed settings, hardware data, ports, and other telemetry.

- `runtime\HUNTER_all_cache.txt`
  - File cache of all discovered configs.

- `runtime\HUNTER_github_configs_cache.txt`
  - Configs fetched from GitHub/raw subscription sources.

- `runtime\HUNTER_silver.txt`
  - Validated medium-quality configs.

- `runtime\HUNTER_gold.txt`
  - Validated best configs.

- `runtime\HUNTER_balancer_cache.json`
  - Main balancer working backend cache.

- `runtime\HUNTER_gemini_balancer_cache.json`
  - Gemini balancer working backend cache.

## Startup Flow

The current backend startup flow is intentionally staged:

### Phase 1: Load configs

On start, the CLI loads configs from multiple sources:

- raw seed files in `config\`
- runtime caches
- balancer cache JSON files
- previously discovered configs already persisted by the system

This is why a fresh launch can quickly report a very large `ConfigDB` count.

### Phase 2: Detect censorship and initialize evasion

The orchestrator checks direct reachability to several public hosts.
If multiple baseline endpoints fail, Hunter marks the network as censored and keeps DPI-evasion features active.

### Phase 3: Start balancers and engines

Hunter prepares:

- main balancer on `10808`
- gemini balancer on `10809`
- provisioned ports in the `2901-2999` range
- runtime DPI-evasion strategy

### Phase 4: Start workers

A thread manager launches the background workers.
The worker pool includes:

- balancer monitor
- config scanner
- dpi pressure
- github background fetch
- harvester
- health monitor
- import watcher
- telegram publisher
- validator

### Phase 5: Continuous operation

During runtime, Hunter continuously:

- validates candidate configs
- updates balancers
- rotates provisioned ports
- fetches from GitHub/Telegram/import sources
- writes new status snapshots

## Why `0 alive` early in startup is not automatically a bug

In censored networks, the dashboard can show a large config database but still display:

- `0 alive`
- `0/0` balancer backends
- many untested configs
- repeated timeout/handshake failures

This does **not** necessarily mean startup is broken.
It usually means:

- the first validation rounds are still warming up
- many scraped configs are dead or incompatible
- transport/TLS/DNS failures are preventing early passes
- the balancer has not yet received any healthy backend

The important signals are:

- whether tests continue to run
- whether `validated` count grows
- whether healthy configs eventually reach `gold`, `silver`, or balancer cache files

## Import Pipeline

Hunter supports two user-driven import paths:

### Advanced UI import

The dashboard sends `import_config_file` to the CLI.
The CLI reads the file, extracts URIs, validates them, removes duplicates, and promotes imported configs to higher scan priority.

### Import watcher directory

The background `ImportWatcherWorker` monitors:

- `config\import\`
- `config\import\processed\`
- `config\import\invalid\`

Dropping `.txt` files into `config\import\` lets the backend import them automatically.

## Export Pipeline

The dashboard sends `export_config_db` to the CLI.
The CLI exports the entire current config database to a text file.

## Priority Model for User Imports

Imported/manual configs receive special treatment:

- duplicate URIs are not inserted again
- existing records can be promoted instead of duplicated
- imported configs receive a temporary priority boost so they are tested earlier than ordinary backlog entries

This behavior is designed to make user-provided configs surface earlier in scanning without damaging deduplication guarantees.
