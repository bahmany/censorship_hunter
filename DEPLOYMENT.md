# Deployment & Development Workflow

## Overview

Two workflows are available depending on the type of change:

| Change Type | Workflow | Time | Command |
|---|---|---|---|
| C++ source code | Fast sync + restart | ~10s | `.\dev.ps1 -BackendOnly` |
| Web-UI source code | Fast sync + restart | ~10s | `.\dev.ps1 -WebUIOnly` |
| Both | Fast sync + restart | ~15s | `.\dev.ps1` |
| C++ with in-container rebuild | Incremental compile | ~30s | `.\dev.ps1 -RebuildBackend` |
| Auto-watch mode | File watcher | Ongoing | `.\dev.ps1 -Watch` |
| Dockerfile/dependency change | Full image rebuild | ~15min | `.\deploy.ps1 -Rebuild` |
| Check status | - | ~2s | `.\dev.ps1 -Status` |
| View logs | - | Ongoing | `.\dev.ps1 -Logs` |

## Issue 1 Fix: SSH Output Streaming

### Root Cause
- `plink -batch` blocks until the remote command completes
- Docker buildkit buffers all output until each build step finishes
- `Start-Sleep` in PowerShell blocks the entire pipeline with no output

### Solution
- `deploy.ps1` streams all SSH output line-by-line with timestamps
- Build commands pipe through `while read` on the remote side for real-time output
- No `Start-Sleep` blocking — all output is streamed immediately
- Status checks and log tailing use streaming `ForEach-Object`

### Usage
```powershell
# Full rebuild (when Dockerfile or deps change)
.\deploy.ps1 -Rebuild

# Full rebuild with no cache
.\deploy.ps1 -Rebuild -NoCache

# Rebuild backend only
.\deploy.ps1 -RebuildBackend

# Check container status
.\deploy.ps1 -Status

# Tail container logs
.\deploy.ps1 -Logs

# Tail last build log
.\deploy.ps1 -BuildLogs
```

## Issue 2 Fix: Fast Development Without Image Rebuilds

### Root Cause
- Every code change triggered `docker-compose build --no-cache`
- Full rebuild = C++ compile + Go engine builds + npm install + Next.js build
- No volume mounts — source code was baked into the image
- No incremental compilation — everything rebuilt from scratch

### Solution: Three-Tier Workflow

#### Tier 1: Fast Sync (default, ~10s)
```powershell
.\dev.ps1
```
- Syncs changed source files via `pscp`
- Restarts containers (backend re-reads source, web-ui restarts)
- No image rebuild, no compilation

#### Tier 2: Incremental Compile (~30s)
```powershell
.\dev.ps1 -RebuildBackend
```
- Syncs C++ source files
- Runs `ninja` inside the container (only recompiles changed `.cpp` files)
- Copies fresh binary and restarts backend
- Uses persistent build cache volume (`hunter-build-cache`)

#### Tier 3: Full Image Rebuild (~15min, rare)
```powershell
.\deploy.ps1 -Rebuild
```
- Only needed when:
  - `Dockerfile` or `Dockerfile.dev` changes
  - C++ dependencies (CMakeLists.txt, new headers) change
  - npm dependencies (`package.json`) change
  - Base images change
- Uses Docker layer caching (no `--no-cache` by default)

#### Tier 4: Auto-Watch Mode
```powershell
.\dev.ps1 -Watch
```
- Monitors local files for changes
- Auto-syncs and restarts on save
- C++ changes → sync + restart (or in-container rebuild with `-RebuildBackend`)
- Web-UI changes → sync + restart

### Development Docker Compose
```yaml
# docker-compose.dev.yml
# Uses bind mounts for source code:
#   ./hunter_cpp -> /app/hunter_cpp
#   ./web-ui     -> /app
# Persistent volumes:
#   hunter-build-cache -> /app/build (ninja incremental cache)
#   hunter-runtime     -> /app/runtime
```

### New Files Created

| File | Purpose |
|---|---|
| `Dockerfile.dev` | Backend dev image with build tools, engines pre-built |
| `dev-entrypoint.sh` | Incremental compile + run script for backend |
| `web-ui/Dockerfile.dev` | Web-UI dev image with `next dev` (Fast Refresh) |
| `docker-compose.dev.yml` | Dev compose with bind mounts and volumes |
| `deploy.ps1` | Streaming SSH deploy script |
| `dev.ps1` | Fast dev sync + restart script |
| `.dockerignore` | Expanded to exclude build artifacts, third_party, etc. |

## Anti-Patterns Eliminated

1. **`Start-Sleep` + `plink -batch`** → Replaced with streaming `ForEach-Object` pipeline
2. **`--no-cache` on every build** → Layer caching by default, `--noCache` flag only when needed
3. **Source code baked into image** → Bind mounts in dev compose
4. **Full rebuild for every change** → Three-tier workflow (sync → incremental → full)
5. **No build context filtering** → Expanded `.dockerignore`
6. **Silent long-running commands** → All commands stream with timestamps
7. **No status visibility** → `-Status`, `-Logs`, `-BuildLogs` commands

## Server Setup (one-time)

```powershell
# First time: build dev images on server
.\deploy.ps1 -Rebuild

# Then switch to dev compose
# On server:
# cd /home/abharcable/censorship_hunter
# docker-compose -f docker-compose.dev.yml up -d
```

## Local Development (optional)

If Docker is available locally:
```powershell
# Start dev environment locally
docker-compose -f docker-compose.dev.yml up

# Or just the web-ui with hot reload
docker-compose -f docker-compose.dev.yml up hunter-web-ui
```
