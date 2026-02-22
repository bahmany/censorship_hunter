# Hunter 🏹 - Advanced Proxy Hunting System

> Autonomous tool for discovering, testing, and managing V2Ray-compatible proxy configurations to bypass internet censorship.

[![Python Version](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

## Table of Contents

- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Usage](#usage)
- [Android App](#android-app)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

Hunter is a standalone proxy hunting system that autonomously discovers, validates, and manages V2Ray-compatible proxy configurations. It features a 2026-grade anti-DPI suite, multi-engine testing, and seamless load balancing for reliable internet access in heavily censored environments (Iran, China, etc.).

## Features

- **Autonomous Operation** — Continuous hunting for fresh proxy configs from Telegram channels and public GitHub sources.
- **Multi-Engine Support** — Compatible with XRay, Sing-box, and Mihomo; falls back automatically.
- **Load Balancing** — Dynamic multi-backend proxy server with health checks and auto-failover.
- **2026 Anti-DPI Suite** — TLS fragmentation, JA3/JA4 fingerprint spoofing, VLESS-Reality-Vision, MTU optimization, active probe defense, Hysteria2/TUIC UDP protocols.
- **Telegram Integration** — Scrapes configs from public channels; reports results via bot.
- **Web Dashboard** — Monitor proxy status and performance at `http://localhost:8080`.
- **Persistent Caching** — Saves working configs for faster restarts.
- **Android App** — Native Android VPN app in `native/android/` with full feature parity.

## Installation

### Prerequisites

- Python 3.8+
- At least one proxy engine binary in `bin/`:
  - [XRay](https://github.com/XTLS/Xray-core/releases) (`xray` / `xray.exe`)
  - [Sing-box](https://github.com/SagerNet/sing-box/releases) (`sing-box` / `sing-box.exe`)
  - [Mihomo](https://github.com/MetaCubeX/mihomo/releases) (`mihomo` / `mihomo.exe`)

### Setup

1. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/hunter.git
   cd hunter
   ```

2. Create a virtual environment:
   ```bash
   python -m venv .venv

   # Windows
   .venv\Scripts\activate

   # Linux / macOS
   source .venv/bin/activate
   ```

3. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

4. Copy and configure the environment file:
   ```bash
   cp .env.example .env
   # Edit .env with your Telegram API credentials
   ```

## Quick Start

See [QUICK_START.md](QUICK_START.md) for a step-by-step guide.

```bash
# Windows
run.bat

# Any platform
python main.py
```

The load balancer starts on `127.0.0.1:10808` (SOCKS5).

## Configuration

Hunter reads configuration from a `.env` file in the project root (copy from `.env.example`).

### Required

| Variable | Description |
|----------|-------------|
| `HUNTER_API_ID` | Telegram API ID — get from [my.telegram.org](https://my.telegram.org/apps) |
| `HUNTER_API_HASH` | Telegram API Hash |
| `HUNTER_PHONE` | Phone number in international format (e.g. `+1234567890`) |

### Key Optional Settings

| Variable | Default | Description |
|----------|---------|-------------|
| `TOKEN` | — | Telegram bot token for reporting |
| `CHAT_ID` | — | Telegram channel/group ID for reports |
| `HUNTER_MULTIPROXY_PORT` | `10808` | Local SOCKS5 proxy port |
| `HUNTER_SLEEP` | `300` | Seconds between hunting cycles |
| `HUNTER_WORKERS` | `10` | Concurrent config testers |
| `IRAN_FRAGMENT_ENABLED` | `false` | TLS fragmentation for DPI bypass |
| `HUNTER_DPI_EVASION` | `true` | Full 2026 DPI evasion suite |
| `ADEE_ENABLED` | `true` | Adversarial DPI Exhaustion Engine |
| `HUNTER_WEB_PORT` | `8080` | Web dashboard port |

See `.env.example` for the complete list including DPI evasion, UDP protocols, and cache options.

> **Security note:** Never commit `.env`, `*.session`, or `hunter_secrets.env` to version control. These are already in `.gitignore`.

## Project Structure

```
hunter/
├── main.py                  # Entry point
├── orchestrator.py          # Core workflow coordinator
├── launcher.py              # Interactive launcher
├── run.bat                  # Windows launcher script
├── .env.example             # Configuration template
├── requirements.txt         # Python dependencies
├── bin/                     # Proxy engine binaries (not tracked by git)
│   ├── xray[.exe]
│   ├── sing-box[.exe]
│   └── mihomo[.exe]
├── core/                    # Core modules
│   ├── config.py            # Configuration management
│   ├── models.py            # Data models
│   └── utils.py             # Utilities & 12-tier config prioritization
├── parsers/                 # Protocol URI parsers
│   └── uri_parser.py        # VMess, VLESS, Trojan, SS, Hysteria2, TUIC
├── security/                # Anti-DPI & obfuscation modules
│   ├── dpi_evasion_orchestrator.py
│   ├── tls_fingerprint_evasion.py   # JA3/JA4 spoofing
│   ├── tls_fragmentation.py         # ClientHello fragmentation
│   ├── reality_config_generator.py  # VLESS-Reality-Vision
│   ├── udp_protocols.py             # Hysteria2 / TUIC v5
│   ├── mtu_optimizer.py             # 5G PMTUD mitigation
│   ├── active_probe_defense.py
│   ├── split_http_transport.py      # SplitHTTP/XHTTP
│   └── stealth_obfuscation.py
├── proxy/                   # Load balancing
│   └── load_balancer.py
├── network/                 # HTTP client & config fetchers
│   └── http_client.py
├── telegram/                # Telegram scraper
│   └── scraper.py
├── performance/             # Adaptive thread management
│   └── adaptive_thread_manager.py
├── scripts/                 # Utility & diagnostic scripts
├── web/                     # Web dashboard
├── docs/                    # Technical documentation
├── native/android/          # Android VPN app (Java + C++)
├── logs/                    # Runtime logs (git ignored)
└── runtime/                 # Runtime cache (git ignored)
```

## Usage

```bash
python main.py           # Start hunting
python main.py --help    # Show help
```

Press `Ctrl+C` to gracefully shut down all services and save state.

**Web dashboard:** `http://localhost:8080`

## Android App

A full-featured Android VPN app is available in `native/android/`. It provides:

- Material3 dark UI with Persian (Farsi) localization
- Android VPN Service with load balancing (up to 10 concurrent configs)
- Per-app VPN (split tunneling)
- Auto config discovery from Telegram channels and GitHub
- Full 2026 DPI evasion suite
- Android 8.0+ (API 26+) support

See [`native/android/README.md`](native/android/README.md) for build instructions.

## Troubleshooting

**No working configs found** — Ensure at least one proxy engine binary is in `bin/` and Telegram credentials are correct.

**Port 10808 in use** — Set `HUNTER_MULTIPROXY_PORT` to another port in `.env`.

**Telegram auth loop** — Delete `*.session` files and re-authenticate.

**Permission errors on Windows** — Run the terminal as Administrator.

**Large cache files** — `subscriptions_cache.txt` and `working_configs_cache.txt` are git-ignored; delete them to free space.

## Contributing

Contributions are welcome!

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-feature`)
3. Commit your changes
4. Run tests: `python -m pytest testing/ -v`
5. Submit a pull request

For major changes, open an issue first to discuss the approach.

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE) file for details.
