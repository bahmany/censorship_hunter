# Hunter Operator Guide

## First Launch

1. Start `hunter_dashboard.exe`
2. Verify the paths in **Advanced**:
   - `CLI Executable`
   - `Config File`
   - `XRay Path`
   - `Sing-box Path`
   - `Mihomo Path`
3. Click **START**
4. Watch the **Logs** and **Dashboard** sections for startup progress

## What to expect during startup

A normal startup can look like this:

- raw files are loaded
- cache files are loaded
- censorship is detected
- worker threads start
- balancers begin empty
- tests start failing for a while
- healthy configs gradually appear later

This is expected on heavily filtered networks.
Do not treat `0 alive` in the first minute as a final result.

## Important Dashboard Areas

### Dashboard

Use this page for:

- alive count
- balancer health
- provisioned proxy ports
- system proxy actions
- trend visibility

### Configs

Use this page for:

- browsing alive/silver/balancer/gemini/all-cache/github-cache configs
- copying URIs
- QR exporting configs to mobile
- checking latency metadata where available

### Logs

Use this page for:

- startup diagnostics
- engine failures
- curl/network failures
- command result feedback

### Advanced

Use this page for:

- runtime path setup
- engine verification
- manual config add
- import/export database
- GitHub source editing
- Telegram scrape configuration

## Importing configs

Hunter supports two import methods.

### Method 1: Import from the dashboard

Use **Advanced → Import / Export Database → Import File**.

Rules:

- enter a **full path** to an existing file
- supported content can be raw URIs, mixed text containing URIs, or downloaded subscription-style text
- duplicates are removed automatically
- imported configs are promoted to the front of the scan queue for faster testing

If you see `import_file_not_found`, the path was invalid or the file no longer exists.

Recommended places to keep files before importing:

- `config\`
- `config\import\`
- `runtime\`

### Method 2: Drop files into import watcher directory

Put `.txt` files into:

- `config\import\`

The background import watcher will process them automatically.
Processed and invalid files may be moved to watcher-managed folders.

## Exporting the database

Use **Advanced → Import / Export Database → Export DB**.

Export behavior:

- exports the full current DB
- writes one text file
- includes all configs currently known to the database, not just healthy ones

Recommended export name:

- `HUNTER_config_db_export.txt`

## Understanding balancers and ports

### Main balancer

- Port: `10808`
- Intended to expose a working pool from the best currently available backends

### Gemini balancer

- Port: `10809`
- Secondary working pool

### Provisioned ports

- Range shown in UI/logs, commonly `2901-2999`
- Individual per-config runtime ports

If the logs say `No valid backends available for port 10808/10809`, Hunter has not yet found a currently healthy backend for that balancer.

## Manual config add vs import file

### Manual add

- paste configs directly into the Advanced text area
- useful for quick one-off testing

### Import file

- useful for larger downloaded lists
- easier to archive and re-run
- gives clearer reproducibility in logs

## Good operating practice on censored networks

- let Hunter run for several minutes before judging the result
- keep XRay, Sing-box, and Mihomo binaries present together if possible
- use dashboard export to snapshot the DB after a good run
- import your own trusted configs to promote them early
- keep GitHub source list curated instead of excessively large and noisy
