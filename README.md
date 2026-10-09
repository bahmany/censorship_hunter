<div align="center">

# Hunter

**Autonomous Proxy Configuration Discovery & Load Balancing Engine**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6.svg)](#system-requirements)
[![Release](https://img.shields.io/github/v/release/bahmany/censorship_hunter)](https://github.com/bahmany/censorship_hunter/releases)

*A native C++ application with Dear ImGui that autonomously discovers, benchmarks, and load-balances proxy configurations from 20+ public sources — designed for environments with heavy internet restrictions.*

![huntercensor Main Interface](Screenshot%202026-03-23%20135206.png)

</div>

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Quick Start](#quick-start)
- [Building from Source](#building-from-source)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [How It Works](#how-it-works)
- [Network Ports](#network-ports)
- [Supported Protocols](#supported-protocols)
- [System Requirements](#system-requirements)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

---

## Overview

Hunter is an autonomous system that continuously scrapes proxy configuration URIs from public GitHub repositories and Telegram channels, parses and validates them by spawning temporary XRay processes, benchmarks latency, and feeds the best-performing configs into a multi-backend SOCKS5 load balancer — all without manual intervention.

The project consists of two main components:

| Component | Technology | Description |
|-----------|------------|-------------|
| **Native App** (`hountersansor`) | C++17, Dear ImGui, Win32, DirectX9 | Integrated monitoring and control UI |
| **Console App** (`hountersansor_cli`) | C++17, CMake, libcurl, zlib | Autonomous orchestrator with 9 concurrent worker threads |

The native UI talks directly to the orchestrator and also exposes websocket-based realtime control and monitoring channels.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                 Native App (hountersansor)               │
│  Home │ Configs │ Censorship │ Logs │ Advanced          │
│  ─────────────────────────────────────────────────────── │
│  Direct orchestrator calls + realtime monitor/control    │
└─────────────────────────┬───────────────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────────┐
│              C++ Orchestrator (hountersansor_cli)        │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │ Config   │  │ GitHub   │  │Continuous│  │Aggressive│ │
│  │ Scanner  │  │Downloader│  │Validator │  │Harvester │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬────┘ │
│       │              │              │              │      │
│       ▼              ▼              ▼              ▼      │
│  ┌──────────────────────────────────────────────────┐    │
│  │            ConfigDatabase (in-memory, 150K+)      │    │
│  └──────────────────────┬───────────────────────────┘    │
│                         │                                 │
│       ┌─────────────────▼─────────────────────┐          │
│       │     ProxyBenchmark (XRay subprocess)    │          │
│       │     Gold (<2s) │ Silver (<5s) │ Dead    │          │
│       └─────────────────┬─────────────────────┘          │
│                         │                                 │
│  ┌──────────────────────▼──────────────────────────┐     │
│  │       MultiProxyServer (Load Balancer)           │     │
│  │  Main :10808 │ Gemini :10809 │ Ports 2901-2999  │     │
│  │  Least-ping strategy │ Up to 20 backends         │     │
│  └──────────────────────────────────────────────────┘     │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐ │
│  │   DPI    │  │Obfusca-  │  │ Telegram │  │  Smart  │ │
│  │ Evasion  │  │  tion    │  │ Reporter │  │  Cache  │ │
│  └──────────┘  └──────────┘  └──────────┘  └─────────┘ │
└─────────────────────────────────────────────────────────┘
```

## Features

### Backend Engine (C++)

- **Multi-source Config Discovery** — Scrapes 20+ GitHub repositories, anti-censorship aggregators, and Iran-priority Reality sources in parallel
- **URI Parser** — Parses VMess, VLESS, Trojan, Shadowsocks, SSR, Hysteria2, and TUIC URIs into structured configs with full parameter extraction
- **XRay-based Benchmarking** — Spawns temporary XRay processes per config, tests HTTP connectivity through SOCKS, measures latency; classifies into Gold (<2s), Silver (<5s), or Dead tiers
- **Dual Load Balancers** — Two independent `MultiProxyServer` instances (main on port 10808, gemini on 10809) with least-ping backend selection, health monitoring, and hot config reload
- **Port Provisioning** — Spins up individual XRay proxy processes on ports 2901–2999 (up to 20 slots) with automatic health checks every 30 seconds
- **9 Concurrent Worker Threads** — ConfigScanner, GitHubDownloader, ContinuousValidator, AggressiveHarvester, BalancerMonitor, HealthMonitor, TelegramPublisher, DpiPressure, ImportWatcher
- **ConfigDatabase** — In-memory health database (up to 150K entries) tracking per-config alive/dead status, latency, first-seen/last-alive timestamps, test counts, and priority-based batch scheduling
- **DPI Evasion** — Adaptive strategy selection (Reality, SplitHTTP/CDN, WebSocket/CDN, gRPC/CDN, Hysteria2); network condition detection; config prioritization by anti-censorship features
- **Stealth Obfuscation** — SNI randomization, TLS fingerprint rotation (Chrome/Firefox/Safari/Edge), WebSocket path obfuscation
- **Telegram Bot Reporter** — Publishes validated configs to Telegram groups via Bot API with proxy fallback
- **Smart Cache** — File-persistent config cache with working/all separation, deduplication, and age-based staleness
- **Import Watcher** — Monitors `config/import/` folder for manually dropped `.txt` files, auto-processes and adds valid URIs to database
- **Hardware-Aware Scaling** — Detects CPU count and RAM, adjusts thread pools and batch sizes dynamically
- **Crash Resilience** — SEH exception handler (Windows), crash logging to `runtime/hunter_crash.log`, graceful shutdown on CTRL+C

### Native UI (Dear ImGui)

- **Simple Main Pages** — Home, Configs, Censorship, and Logs for day-to-day use
- **Advanced Workspace** — Runtime paths, Telegram settings, GitHub sources, provisioning, cleanup, and technical controls grouped together
- **Direct Orchestrator Integration** — Start/Stop, pause/resume, cycle control, import/export, and live state access from one native process
- **Realtime Discovery View** — Live discovery logs and censorship diagnostics in the native app
- **Native Windows Shell** — Win32 + DirectX9 rendering with no extra desktop runtime payload

## Client Mode (v2rayN-style)

Besides autonomous hunting, Hunter works as a regular proxy client — pick a
server, connect on a stable local port, and point your apps at it. Open the
**Client** tab in the UI.

- **Profiles** — every config Hunter discovers (or you import) is a profile.
  Connect with one click; the active profile holds a mixed SOCKS5+HTTP
  listener on `client_listen_port` (default `10808`). Connecting a new
  profile automatically replaces the previous one; rename via double-click,
  delete via checkboxes.
- **Subscriptions** — add subscription URLs in the Client tab; Hunter fetches
  them with a `Hunter/<ver>` User-Agent, decodes base64 line lists and
  Clash/Mihomo YAML, deduplicates into the config DB, and auto-updates on the
  `sub_update_interval_min` interval (default 60 min). Sources persist in
  `runtime/subscriptions.tsv`.
- **Import** — clipboard share links (`vmess://`, `vless://`, `ss://`,
  `trojan://`, `hysteria2://`, `hy2://`, `tuic://`, `wireguard://`) or a
  subscription URL.
- **System proxy** — optional `client_sysproxy_enabled`: Windows registry,
  GNOME `gsettings` (Linux), or `networksetup` (macOS). Cleared on disconnect.
- **Latency test** — TCP latency per selected profile (all when none
  selected), off the UI thread, with sort-by-latency.
- **Android** — VPN mode uses `HunterVpnService` + `hev-socks5-tunnel`;
  subscription import/parsing works the same through the shared core.

Client keys in `runtime/hunter_config.json` (or `HUNTER_*` env overrides):
`client_listen_port` (10808), `client_sysproxy_enabled` (false),
`sub_update_interval_min` (60), `sub_autoupdate` (true).

**Engine notes:** Xray handles VMess/VLESS/Trojan/Shadowsocks; sing-box adds
Hysteria2/TUIC/WireGuard. `ssr://` links are parsed for display but are not
launchable by any bundled engine and are excluded from imports.

## Quick Start

### Using Pre-built Release

1. Download the latest `.zip` from [Releases](https://github.com/bahmany/censorship_hunter/releases)
2. Extract to any folder
3. Run `hountersansor.exe`
4. Click **START** — the backend begins autonomous discovery immediately
5. Configure your browser to use `127.0.0.1:10808` as a SOCKS5 proxy

No installation required. No dependencies. Fully portable.

### Optional: Telegram Integration

To also scrape configs from Telegram channels, configure in the Advanced tab:
1. Get API credentials from [my.telegram.org](https://my.telegram.org/apps)
2. Enter API ID, API Hash, and phone number in the UI
3. Add target channel usernames
4. The backend will scrape configs from these channels alongside GitHub sources

## Building from Source

### Prerequisites

| Tool | Version | Purpose |
|------|---------|---------|
| [MSYS2](https://www.msys2.org/) | Latest | C++ toolchain (UCRT64 environment) |
| [CMake](https://cmake.org/) | ≥ 3.16 | C++ build system |
| [Ninja](https://ninja-build.org/) | Latest | Fast build backend |
| [Visual Studio](https://visualstudio.microsoft.com/) | 2022+ | Windows SDK & C++ desktop workload |

### Build C++ Backend

```bash
# In MSYS2 UCRT64 shell:
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,zlib}

cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja
ninja

# Output: hunter_cpp/build/hountersansor.exe and hunter_cpp/build/hountersansor_cli.exe
```

### Run Tests

```bash
# C++ unit tests (21 tests covering utils, models, URI parser, ConfigDatabase):
cd hunter_cpp/build
./hunter_tests.exe
```

## Configuration

The backend reads configuration from `runtime/hunter_config.json` (auto-created with defaults on first run). The native UI can modify settings live through the orchestrator runtime command surface.

Key configuration options are also available through environment variables. See [`.env.example`](.env.example) for the full list.

| Variable | Default | Description |
|----------|---------|-------------|
| `HUNTER_MULTIPROXY_PORT` | `10808` | Main SOCKS5 balancer port |
| `HUNTER_GEMINI_PORT` | `10809` | Secondary balancer port |
| `HUNTER_SCAN_LIMIT` | `50` | Configs per scan cycle |
| `HUNTER_MAX_CONFIGS` | `1000` | Max configs to keep in working set |
| `HUNTER_WORKERS` | `10` | Worker thread count |
| `HUNTER_TEST_TIMEOUT` | `10` | Per-config test timeout (seconds) |
| `HUNTER_GITHUB_BG_CAP` | `150000` | Max configs from GitHub background fetch |
| `HUNTER_DPI_EVASION` | `true` | Enable adaptive DPI evasion |
| `TELEGRAM_BOT_TOKEN` | — | Telegram Bot API token for reporting |

## Project Structure

```
hunter/
├── hunter_cpp/                    # C++ backend engine
│   ├── CMakeLists.txt             # Build configuration (C++17, static linking)
│   ├── build.bat                  # Quick build script
│   ├── include/
│   │   ├── core/                  # Config, models, utils, constants, task_manager
│   │   ├── network/               # HTTP client, URI parser, config fetcher,
│   │   │                          # continuous validator, proxy tester,
│   │   │                          # aggressive harvester, flexible fetcher
│   │   ├── proxy/                 # Multi-backend load balancer, XRay manager
│   │   ├── testing/               # Proxy benchmarking engine
│   │   ├── security/              # DPI evasion orchestrator, obfuscation engine
│   │   ├── telegram/              # Bot API reporter
│   │   ├── cache/                 # Smart file-persistent cache
│   │   ├── orchestrator/          # Main orchestrator, thread manager (9 workers)
│   │   └── web/                   # HTTP server, dashboard (optional)
│   ├── src/                       # Implementation files (mirrors include/ layout)
│   └── tests/
│       └── test_core.cpp          # 21 unit tests
│
├── config/                        # Runtime config directory
│   └── import/                    # Drop .txt files here for auto-import
├── .env.example                   # Environment variable reference
├── .gitignore
├── LICENSE                        # MIT License
├── CHANGELOG.md
└── CONTRIBUTING.md
```

## How It Works

### Autonomous Hunting Loop

1. **Startup** — Load cached configs from previous session → start balancers with cached backends for immediate connectivity → start all 9 worker threads
2. **Scrape** — ConfigScanner and GitHubDownloader fetch URIs from 20+ sources in parallel; AggressiveHarvester uses round-robin proxy ports for reliability
3. **Parse** — UriParser extracts structured `ParsedConfig` from each URI (protocol, address, port, TLS settings, transport, SNI, fingerprint, etc.)
4. **Benchmark** — ProxyBenchmark spawns temporary XRay processes, tests HTTP download through SOCKS, measures latency → Gold (<2s) / Silver (<5s) / Dead
5. **Balance** — Gold configs are fed to `MultiProxyServer` which manages up to 20 XRay backend processes with least-ping selection
6. **Validate** — ContinuousValidator continuously re-tests the database in priority order (untested → stale → alive), evicts configs dead >3 hours
7. **Publish** — TelegramPublisher sends validated Gold configs to configured Telegram group
8. **Repeat** — Main loop runs every 30 minutes with adaptive sleep; balancer health checks every 60 seconds; validator batches every 2 seconds

### Config Prioritization

Configs are scored by anti-censorship features:

| Priority | Feature | Rationale |
|----------|---------|-----------|
| Highest | VLESS + Reality | Indistinguishable from legitimate TLS traffic |
| High | gRPC over CDN | Hides behind CDN infrastructure |
| Medium | WebSocket over TLS | Common CDN transport |
| Lower | Trojan over TLS | Standard TLS encryption |
| Lowest | Plain VMess/SS | No advanced evasion |

## Network Ports

| Port | Service | Description |
|------|---------|-------------|
| `10808` | Main SOCKS5 balancer | Primary proxy endpoint — configure your browser here |
| `10809` | Gemini SOCKS5 balancer | Secondary independent balancer |
| `2901`–`2999` | Provisioned proxies | Individual XRay processes (up to 20 slots) |
| `11808`+ | Benchmark ports | Temporary ports used during config testing |

## Supported Protocols

| Protocol | URI Scheme | Proxy Engine |
|----------|-----------|--------------|
| VMess | `vmess://` | XRay |
| VLESS | `vless://` | XRay |
| VLESS + Reality | `vless://...&security=reality` | XRay |
| Trojan | `trojan://` | XRay |
| Shadowsocks | `ss://` | XRay |
| ShadowsocksR | `ssr://` | Parsed only — no bundled engine |
| Hysteria2 | `hysteria2://`, `hy2://` | Sing-box |
| TUIC v5 | `tuic://` | Sing-box |
| WireGuard | `wireguard://` | Sing-box |

**Transport types:** TCP, WebSocket, gRPC, HTTP/2, SplitHTTP, HTTPUpgrade

**Proxy engines included in release:**
- [XRay-core](https://github.com/XTLS/Xray-core) — Primary engine for V2Ray-family protocols
- [Sing-box](https://github.com/SagerNet/sing-box) — Universal proxy platform (Hysteria2, TUIC)
- [Mihomo](https://github.com/MetaCubeX/mihomo) — Clash Meta compatible engine
- [Tor](https://www.torproject.org/) — Onion routing network support

## System Requirements

| Requirement | Minimum | Recommended |
|-------------|---------|-------------|
| OS | Windows 10 x64 | Windows 11 x64 |
| RAM | 4 GB | 8 GB |
| Storage | 200 MB | 500 MB |
| CPU | 2 cores | 4+ cores |
| Network | Internet access | Unrestricted or censored |

## Troubleshooting

| Symptom | Cause | Solution |
|---------|-------|----------|
| `hountersansor_cli.exe` not found | Backend binary missing from `bin/` | Re-extract from release ZIP or build from source |
| DLL errors on launch | Antivirus quarantined files | Add extraction folder to antivirus exclusions |
| Port 10808 already in use | Another proxy or previous instance | Kill the process using the port, or change port in `runtime/hunter_config.json` |
| No configs appearing | No internet, or all sources blocked | Check connectivity; try adding manual configs via Advanced tab |
| UI won't start | Native app files missing or blocked | Ensure `hountersansor.exe` exists alongside the bundled engines and runtime files |
| Slow config discovery | Default speed profile is conservative | Switch to "High" speed profile in dashboard controls |
| Configs found but proxy not working | Balancer has no healthy backends yet | Wait for benchmarking to complete (watch Gold count in dashboard) |

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

<div align="center">

Made with determination for a free and open internet.

</div>
