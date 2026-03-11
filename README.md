<div align="center">

# Hunter

**Autonomous Proxy Configuration Discovery & Load Balancing Engine**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Flutter](https://img.shields.io/badge/Flutter-3.11-02569B.svg)](https://flutter.dev)
[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6.svg)](#system-requirements)
[![Release](https://img.shields.io/github/v/release/bahmany/censorship_hunter)](https://github.com/bahmany/censorship_hunter/releases)

*A high-performance C++ backend with a modern Flutter desktop UI that autonomously discovers, benchmarks, and load-balances proxy configurations from 20+ public sources — designed for environments with heavy internet restrictions.*

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
| **Backend Engine** (`hunter_cli`) | C++17, CMake, libcurl, zlib | Autonomous orchestrator with 9 concurrent worker threads |
| **Desktop UI** (`hunter_dashboard`) | Flutter 3.11, Dart | Real-time monitoring dashboard with neon dark theme |

Communication between the UI and backend uses **bidirectional JSON lines over stdin/stdout**, with file-based status updates as a secondary channel.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   Flutter Desktop UI                     │
│  Dashboard │ Configs │ Logs │ Advanced │ About           │
│  ─────────────────────────────────────────────────────── │
│  stdin JSON commands ↓          ↑ ##STATUS## JSON lines  │
└─────────────────────────┬───────┴───────────────────────┘
                          │
┌─────────────────────────▼───────────────────────────────┐
│                 C++ Orchestrator (hunter_cli)             │
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

### Desktop UI (Flutter)

- **Real-time Dashboard** — Arc gauges, sparkline trends, alive config count, engine status, provisioned port status with health indicators
- **Config Browser** — 6 tabs (Alive, Silver, Balancer, Gemini, All Cache, GitHub) with per-row copy, speed test, and QR code generation
- **Log Viewer** — Color-coded console output with auto-scroll, 100KB memory cap with oldest-line trimming
- **Advanced Controls** — Pause/Resume, speed profiles (Low/Medium/High), thread count slider (1–50), test timeout slider (1–10s), config age cleanup, manual config addition, GitHub source URL editor
- **System Proxy Integration** — One-click "USE" button sets Windows system proxy to any active port; "CLEAR" removes it
- **System Tray** — Minimize to tray on close, context menu (Show/Start/Stop/Exit), tooltip updates on new config discovery
- **QR Code** — Pure-Dart QR encoder (no external dependencies) for sharing config URIs to mobile devices
- **Bundled Seed Configs** — Ships with initial config files in `assets/configs/` that are copied to `Documents/Hunter/config` on first run
- **Single Instance Lock** — File-based exclusive lock prevents multiple dashboard instances
- **Window Auto-Adapt** — Detects screen resolution, sets window to 75% × 85% clamped between 900×600 and 1800×1200

## Quick Start

### Using Pre-built Release

1. Download the latest `.zip` from [Releases](https://github.com/bahmany/censorship_hunter/releases)
2. Extract to any folder
3. Run `hunter_dashboard.exe`
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
| [Flutter SDK](https://flutter.dev/) | ≥ 3.11 | Desktop UI framework |
| [Visual Studio](https://visualstudio.microsoft.com/) | 2022+ | Windows SDK & C++ desktop workload |

### Build C++ Backend

```bash
# In MSYS2 UCRT64 shell:
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,zlib}

cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja
ninja

# Output: hunter_cpp/build/hunter_cli.exe (also copied to bin/)
```

### Build Flutter UI

```powershell
cd hunter_flutter_ui
flutter pub get
flutter build windows --release

# Output: hunter_flutter_ui/build/windows/x64/runner/Release/hunter_dashboard.exe
```

### Run Tests

```bash
# C++ unit tests (21 tests covering utils, models, URI parser, ConfigDatabase):
cd hunter_cpp/build
./hunter_tests.exe
```

## Configuration

The backend reads configuration from `runtime/hunter_config.json` (auto-created with defaults on first run). The UI can modify settings live via stdin JSON commands.

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
├── hunter_flutter_ui/             # Flutter desktop UI
│   ├── lib/
│   │   ├── main.dart              # App entry, process management, stdin/stdout IPC
│   │   ├── theme.dart             # "Racing Neon" dark color palette
│   │   ├── models.dart            # Enums and data classes
│   │   ├── services.dart          # File I/O, engine detection, speed test helpers
│   │   └── widgets/
│   │       ├── dashboard_section  # Gauges, alive configs, port status, controls
│   │       ├── configs_section    # 6-tab config browser with copy/QR/speed test
│   │       ├── logs_section       # Color-coded log viewer
│   │       ├── advanced_section   # Speed controls, GitHub URL editor, Telegram
│   │       ├── about_section      # Project info
│   │       ├── gauge_painter      # Arc gauge + sparkline custom painters
│   │       ├── qr_painter         # Pure-Dart QR encoder (no deps)
│   │       └── qr_dialog          # QR code display dialog
│   ├── assets/configs/            # Bundled seed configurations
│   ├── pubspec.yaml               # Dependencies: window_manager, system_tray, etc.
│   └── windows/                   # Windows runner (Visual Studio project)
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
| ShadowsocksR | `ssr://` | XRay |
| Hysteria2 | `hysteria2://`, `hy2://` | Sing-box |
| TUIC v5 | `tuic://` | Sing-box |

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
| `hunter_cli.exe` not found | Backend binary missing from `bin/` | Re-extract from release ZIP or build from source |
| DLL errors on launch | Antivirus quarantined files | Add extraction folder to antivirus exclusions |
| Port 10808 already in use | Another proxy or previous instance | Kill the process using the port, or change port in `runtime/hunter_config.json` |
| No configs appearing | No internet, or all sources blocked | Check connectivity; try adding manual configs via Advanced tab |
| UI won't start | Missing `data/` folder | Ensure `data/` with `app.so` exists alongside `hunter_dashboard.exe` |
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
