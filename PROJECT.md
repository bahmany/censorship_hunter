# Hunter — Censorship Hunter Project Documentation

## Overview

**Hunter** is an autonomous anti-censorship proxy configuration discovery, validation, and load-balancing system. It scrapes proxy configs (VLESS, VMess, Trojan, etc.) from Telegram channels and GitHub repos, tests them for liveness, and exposes working proxies through SOCKS5 balancers. A real-time web dashboard provides monitoring and control.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Nginx Reverse Proxy                     │
│              api.abharcable.com/myaddresses/mon            │
│                   (Basic Auth protected)                   │
└──────┬──────────┬──────────┬──────────┬───────────────────┘
       │          │          │          │
       ▼          ▼          ▼          ▼
   :7800       :7801      :7802     Next.js API
   Next.js     WS         WS        /api/configs/[type]
   Web UI      Command    Status    (reads runtime volume)
              (control)  (monitor)
       │          │          │
       │          ▼          │
       │   ┌──────────────────────────────────────────────┐
       │   │           C++ Backend (hunter_backend)         │
       │   │                                                │
       │   │  ┌──────────────┐  ┌──────────────────────┐  │
       │   │  │ Orchestrator  │  │  WebSocket Bridge     │  │
       │   │  │              │  │  (port 7801 control)  │  │
       │   │  │ - Scraper    │  │  (port 7802 monitor)  │  │
       │   │  │ - Validator  │  └──────────────────────┘  │
       │   │  │ - Balancer   │                            │
       │   │  │ - Config DB  │  ┌──────────────────────┐  │
       │   │  │ - DPI Evasion│  │  Proxy Engines        │  │
       │   │  └──────────────┘  │  - Xray               │  │
       │   │                    │  - Sing-box           │  │
       │   │                    │  - Mihomo (Clash)     │  │
       │   │                    │  - Tor                │  │
       │   │                    └──────────────────────┘  │
       │   └──────────────────────────────────────────────┘
       │
       ▼
  Docker Network (10.101.0.0/24)
```

---

## Components

### 1. C++ Backend (`hunter_cpp/`)

The core engine, built as a static library (`libhunter_core.a`) with a Linux server executable (`hunter_backend`).

**Modules:**

- **`core/`** — Configuration, data models, utilities, task manager, seed data
- **`network/`** — HTTP client, URI parser, config fetcher, continuous validator, proxy tester, aggressive harvester, flexible fetcher, system proxy
- **`orchestrator/`** — Main orchestration loop, thread management, command processing (`processRealtimeCommand`)
- **`realtime/`** — WebSocket bridge for real-time control (port 7801) and status broadcasting (port 7802)
- **`proxy/`** — Load balancer, runtime engine manager, Xray manager
- **`security/`** — DPI evasion, switch bypass, obfuscation, packet bypass
- **`telegram/`** — Bot reporter for publishing validated configs
- **`cache/`** — Smart caching layer
- **`testing/`** — Benchmarking
- **`linux/`** — Linux entry point (`main.cpp`)
- **`win32/`** — Windows GUI (ImGui-based desktop app, not used in Docker deployment)

**Build system:** CMake + Ninja, C++17, static linking.

**Key WebSocket commands** (sent as JSON to port 7801):
- `run_cycle` — Trigger a scan/validation cycle
- `refresh_ports` — Re-read provisioned port configuration
- `recheck_live_ports` — Re-check live provisioned ports
- `reprovision_ports` — Stop and re-provision all proxy ports
- `load_raw_files` — Load raw config files
- `load_bundle_files` — Load bundled config files
- `export_config_db` — Export config database to file
- `download_configs` — Download configs (handled internally)
- `detect_censorship` / `probe_censorship` — DPI evasion probing

### 2. Web UI (`web-ui/`)

Next.js 14 (App Router) + React 18 + MUI 5 dashboard.

**Tech stack:**
- Next.js 14.0.4 (App Router, standalone output)
- React 18, TypeScript 5.3
- MUI 5 (Material Components), MUI X DataGrid
- Recharts (charts), Lucide React (icons)
- TailwindCSS 3.3
- next-pwa (disabled in dev mode)
- file-saver (config downloads)

**Key files:**
- `app/page.tsx` — Main dashboard (1572 lines), real-time WebSocket status, command controls, config table, charts
- `app/layout.tsx` — Root layout with MUI ThemeProvider
- `app/theme.ts` — Dark theme definition
- `app/api/configs/[type]/route.ts` — API route for config downloads (reads from shared volume)
- `next.config.js` — basePath support via `NEXT_PUBLIC_PROXY_PATH`, standalone output, PWA config

**Environment variables:**
- `NEXT_PUBLIC_BACKEND_HOST` — Backend hostname (default: `hunter-backend`)
- `NEXT_PUBLIC_WS_STATUS_PORT` — WebSocket monitor port (7802)
- `NEXT_PUBLIC_WS_COMMAND_PORT` — WebSocket control port (7801)
- `NEXT_PUBLIC_API_PORT` — API port (7801, used for direct non-proxied access)
- `NEXT_PUBLIC_PROXY_PATH` — Nginx basePath (e.g., `/myaddresses/mon`)

### 3. Nginx Reverse Proxy

Serves the entire UI and WebSocket endpoints under `/myaddresses/mon` with HTTP Basic Auth.

**Location blocks (in `nginx_hunter_snippet.conf`):**

| Path | Backend | Auth | Purpose |
|------|---------|------|---------|
| `/myaddresses/mon/ws-status` | `:7802` | Yes | WebSocket monitor (status broadcasts) |
| `/myaddresses/mon/ws-command` | `:7801` | Yes | WebSocket control (commands) |
| `/myaddresses/mon` | `:7800` | Yes | Next.js UI + internal API routes |

**Auth file:** `/etc/nginx/.htpasswd_hunter` (user: `admin`)

**WebSocket proxying:** Uses `proxy_set_header Upgrade $http_upgrade` and `Connection 'upgrade'` with 86400s timeouts.

**Config file location:** Inserted into `/etc/nginx/sites-available/beta.abharcable.conf` (symlinked from `sites-enabled/`).

---

## Docker Deployment

### Production (`docker-compose.yml`)

Two services on a bridge network (`10.101.0.0/24`):

| Service | Container | Port | Purpose |
|---------|-----------|------|---------|
| `hunter-backend` | `hunter-backend` | 7801-7804, 7810-7820 | C++ backend |
| `hunter-web-ui` | `hunter-web-ui` | 7800→3000 | Next.js UI |

**Volumes:** `hunter-runtime`, `hunter-config`, `hunter-logs`

### Development (`docker-compose.dev.yml`)

Same services with hot-reload:
- C++ backend: bind-mounted source + ninja incremental compilation
- Next.js: bind-mounted source + Fast Refresh
- `hunter-runtime` volume shared to web-ui at `/app/backend-runtime:ro` for API route file access

---

## Port Reference

| Port | Service | Protocol | Purpose |
|------|---------|----------|---------|
| 7800 | hunter-web-ui | HTTP | Next.js web dashboard |
| 7801 | hunter-backend | WebSocket | Control commands (REST API port name is legacy) |
| 7802 | hunter-backend | WebSocket | Status/monitor broadcasts |
| 7803 | hunter-backend | SOCKS5 | Main proxy load balancer |
| 7804 | hunter-backend | SOCKS5 | Gemini proxy load balancer |
| 7810-7820 | hunter-backend | SOCKS5 | Provisioned per-config proxy ports |

---

## Configuration

### Backend Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HUNTER_REST_API_PORT` | 7801 | WebSocket control port |
| `HUNTER_WS_MONITOR_PORT` | 7802 | WebSocket monitor port |
| `HUNTER_MULTIPROXY_PORT` | 7803 | Main SOCKS5 balancer |
| `HUNTER_GEMINI_PORT` | 7804 | Secondary SOCKS5 balancer |
| `HUNTER_SPEED_PROFILE` | low | Speed profile |
| `HUNTER_MAX_CONCURRENT_TEST_PROCESSES` | 8 | Max parallel tests |
| `HUNTER_XRAY_PATH` | bin/xray | Xray binary path |
| `HUNTER_SINGBOX_PATH` | bin/sing-box | Sing-box binary path |
| `HUNTER_MIHOMO_PATH` | bin/mihomo | Mihomo binary path |
| `HUNTER_TOR_PATH` | bin/tor | Tor binary path |
| `HUNTER_SCAN_LIMIT` | 50 | Configs per scan cycle |
| `HUNTER_MAX_CONFIGS` | 1000 | Max configs in working set |
| `HUNTER_WORKERS` | 10 | Worker thread count |
| `HUNTER_TEST_TIMEOUT` | 10 | Per-config test timeout (s) |
| `HUNTER_DPI_EVASION` | true | Enable adaptive DPI evasion |
| `HUNTER_DPI_PRESSURE_INTENSITY` | 0.7 | Pressure engine intensity |

### Telegram Integration

| Variable | Description |
|----------|-------------|
| `TELEGRAM_BOT_TOKEN` | Bot token from @BotFather |
| `CHAT_ID` | Target chat/channel ID |
| `HUNTER_API_ID` | Telegram API ID (from my.telegram.org) |
| `HUNTER_API_HASH` | Telegram API hash |
| `HUNTER_PHONE` | Telegram phone number |

---

## Runtime Data

The backend stores runtime data in `/app/runtime/` (Docker volume `hunter-runtime`):

| File | Description |
|------|-------------|
| `HUNTER_config_db.tsv` | Config database (URI, status, source, timestamps, latency) |
| `HUNTER_status.json` | Current status snapshot |
| `HUNTER_all_cache.txt` | All cached configs |
| `HUNTER_gold.txt` | Validated "gold" configs (alive) |
| `HUNTER_balancer_cache.json` | Load balancer cache |
| `HUNTER_gemini_balancer_cache.json` | Gemini balancer cache |
| `HUNTER_github_configs_cache.txt` | GitHub-fetched configs |
| `hunter_config.json` | Runtime configuration |
| `assets/` | Asset files |
| `engine_tmp/` | Engine temporary files |

The Next.js API route at `/api/configs/[type]` reads `HUNTER_config_db.tsv` from the shared volume to serve config downloads (`all`, `gold`, `silver`).

---

## API Endpoints

### Next.js API Routes (served via port 7800, proxied through Nginx)

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| GET | `/api/configs/all` | Yes (via Nginx) | Download all configs as text file |
| GET | `/api/configs/gold` | Yes (via Nginx) | Download alive/validated configs |
| GET | `/api/configs/silver` | Yes (via Nginx) | Download untested configs |

### WebSocket Endpoints

| Path | Port | Direction | Description |
|------|------|-----------|-------------|
| `/myaddresses/mon/ws-status` | 7802 | Server→Client | Real-time status broadcasts (JSON) |
| `/myaddresses/mon/ws-command` | 7801 | Bidirectional | Send commands, receive responses |

---

## Build & Deploy

### Development

```bash
# On the server:
cd /home/abharcable/censorship_hunter
docker-compose -f docker-compose.dev.yml up -d

# Rebuild after dependency changes:
docker-compose -f docker-compose.dev.yml build
```

### Production

```bash
docker-compose up -d
```

### Nginx Setup

1. Copy `nginx_hunter_snippet.conf` content into the server block for `api.abharcable.com` in `/etc/nginx/sites-available/beta.abharcable.conf`
2. Create auth file: `sudo htpasswd -c /etc/nginx/.htpasswd_hunter admin`
3. Test: `sudo nginx -t`
4. Reload: `sudo nginx -s reload`

---

## Server Details

- **Server IP:** `192.168.1.77`
- **SSH user:** `abharcable`
- **Domain:** `api.abharcable.com`
- **Proxy path:** `/myaddresses/mon`
- **Auth password:** `mohammaD123$%`
- **Project path (server):** `/home/abharcable/censorship_hunter`
- **Nginx config:** `/etc/nginx/sites-available/beta.abharcable.conf` (symlinked from `sites-enabled/`)

---

## Key Design Decisions

1. **No HTTP REST API in backend** — The C++ backend is WebSocket-only on ports 7801 (control) and 7802 (monitor). There is no traditional HTTP REST API. All commands are sent as JSON over WebSocket.

2. **Next.js API route for config downloads** — Since the backend has no HTTP API, a Next.js API route reads the config database file directly from the shared `hunter-runtime` Docker volume to serve config downloads.

3. **basePath support** — The `NEXT_PUBLIC_PROXY_PATH` env var configures Next.js `basePath` so all routes, assets, and links work under the Nginx subpath `/myaddresses/mon`.

4. **PWA disabled in dev** — `next-pwa` is disabled when `NODE_ENV=development`. Stale PWA files (`manifest.json`, `sw.js`, `icon-192.png`) from production builds should be removed from `public/` when running in dev mode behind a subpath.

5. **Static linking** — The C++ backend is statically linked (`-static-libgcc -static-libstdc++`) for portability across Alpine Linux.

6. **Dual-platform** — The C++ codebase supports both Windows (ImGui desktop GUI) and Linux (headless server with WebSocket). The Docker deployment uses the Linux target.
