<div align="center">

# 🏹 Hunter — Complete Anti-Censorship Solution

**v1.0.0 Released! Modern Flutter UI + High-Performance C++ Backend**

[![Release](https://img.shields.io/badge/Release-v1.0.0-blue.svg)](https://github.com/bahmany/censorship_hunter/releases/tag/v1.0.0)
[![Download](https://img.shields.io/badge/Download-64.55%20MB-green.svg)](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-lightgrey.svg)]()
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)

**📦 [Download Hunter v1.0.0](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip) (64.55 MB, Self-Contained, No Dependencies)**

**[English](#english) | [فارسی](#persian-farsi)**

</div>

---

## 🎯 What's New in v1.0.0

### 🖥️ Modern Flutter UI
- **Dark Racing Neon Theme** - Professional interface with stunning visual design
- **Real-time Dashboard** - Live monitoring of configs, speeds, and system status  
- **QR Code Generation** - Seamless mobile device integration
- **System Tray Integration** - Minimize to tray with context menu
- **Single Instance Lock** - Prevents multiple instances

### ⚡ High-Performance C++ Backend
- **Multi-threaded Architecture** - Optimized for concurrent operations
- **Smart Caching System** - Intelligent configuration prioritization
- **Continuous Validation** - Background testing with automatic failover
- **Memory Efficient** - Handles 150K+ configurations with automatic cleanup

### 📦 Self-Contained Package
- **Zero Dependencies** - Everything bundled, no installation required
- **Portable** - Works from any directory, just extract and run
- **Complete** - Includes all proxy engines, libraries, and documentation
- **Tested** - Verified in multiple isolated environments

---

<a name="english"></a>
## 🇬🇧 English

### 🚀 Quick Start (Windows)

#### Option 1: Download Pre-built Executable (Recommended)

1. **Download**: [Hunter-v1.0.0-Final.zip](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip) (64.55 MB)
2. **Extract** to any folder (e.g., `C:\Hunter\`)
3. **Run** `hunter_dashboard.exe`
4. **Click START** to begin config discovery

**System Requirements**: Windows 10/11 x64, 4GB+ RAM recommended

#### Option 2: Python Version (Advanced Users)

Hunter is an **autonomous, production-grade proxy hunting system** designed for users in heavily censored regions (Iran, China, Russia, etc.). It continuously:

1. **Scrapes** — Fetches fresh V2Ray/XRay configs from Telegram channels and GitHub repositories
2. **Benchmarks** — Tests each config with **two independent pipelines** (Telegram-sourced vs. GitHub-sourced)
3. **Validates** — Ranks configs by latency, stability, and anti-DPI features
4. **Serves** — Provides a local SOCKS5 load balancer with automatic failover

### 🚀 Key Features

| Feature | Description |
|---------|-------------|
| **🔀 Dual Benchmark Pipelines** | Separate validation lines for Telegram and GitHub configs with isolated port ranges |
| **⚖️ Smart Load Balancer** | Multi-backend SOCKS5 proxy with health checks and auto-failover |
| **🛡️ 2026 Anti-DPI Suite** | TLS fragmentation, JA3/JA4 spoofing, VLESS-Reality-Vision, MTU optimization |
| **🌐 Multi-Protocol** | VMess, VLESS, Trojan, Shadowsocks, Hysteria2, TUIC v5, WireGuard |
| **🤖 Telegram Integration** | Auto-scrape from channels + report results via bot |
| **📊 Web Dashboard** | Real-time monitoring at `http://localhost:8585` |
| **📱 Android App** | Native VPN app with full feature parity |
| **🧠 Adaptive Intelligence** | DPI-aware config prioritization, memory-safe chunking, circuit breakers |
| **🎨 Modern UI** | Flutter-based dashboard with Racing Neon theme |

### 🏗️ Architecture Overview

#### v1.0.0 Windows Package Structure
```
Hunter/
├── hunter_dashboard.exe     # Flutter UI - Main application entry point
├── hunter_cli.exe           # C++ Backend - Core engine (in bin/)
├── bin/                     # Proxy engines and backend
│   ├── hunter_cli.exe       # C++ orchestrator
│   ├── xray.exe             # XRay Core proxy engine
│   ├── sing-box.exe         # Sing-box universal proxy
│   ├── mihomo-windows-amd64-compatible.exe  # Mihomo/Clash core
│   └── tor.exe              # Tor network support
├── data/                    # Flutter AOT compiled assets
├── config/                  # Configuration files and seed configs
├── docs/                    # Comprehensive documentation (27 files)
├── runtime/                 # Runtime cache and temporary files
└── [All DLLs bundled]       # Complete dependencies included
```

#### System Architecture
```
┌─────────────────────────────────────────────────────────────────┐
│                         Hunter System                          │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐  │
│  │   Telegram  │    │    GitHub   │    │  Anti-Censorship   │  │
│  │  Scrapers   │    │   Sources   │    │     Sources        │  │
│  └──────┬──────┘    └──────┬──────┘    └──────────┬──────────┘  │
│         │                    │                      │            │
│         └────────────────────┼──────────────────────┘            │
│                              ▼                                   │
│                    ┌─────────────────┐                          │
│                    │ Config Pipeline │                          │
│                    │  (separate TG/   │                          │
│                    │   GH queues)     │                          │
│                    └────────┬────────┘                          │
│                             │                                    │
│         ┌───────────────────┼───────────────────┐                 │
│         ▼                   ▼                   ▼                │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │   Benchmark  │    │   Benchmark  │    │    Tier      │      │
│  │   Line 1:    │    │   Line 2:    │    │   Ranking    │      │
│  │  Telegram    │    │   GitHub     │    │ (Gold/Silver/│      │
│  │  (port 11808)│    │  (port 16808)│    │   Bronze)    │      │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘      │
│         │                    │                    │              │
│         └────────────────────┼────────────────────┘            │
│                              ▼                                   │
│                    ┌─────────────────┐                          │
│                    │  Load Balancer  │                          │
│                    │   (SOCKS5)      │                          │
│                    └────────┬────────┘                          │
│                             │                                    │
│                             ▼                                    │
│                      [Your Applications]                         │
└─────────────────────────────────────────────────────────────────┘
```

### 📦 Installation

#### Option 1: Windows Executable (Recommended for v1.0.0)

**Prerequisites**: Windows 10/11 x64, 4GB+ RAM

1. **Download**: [Hunter-v1.0.0-Final.zip](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip) (64.55 MB)
2. **Extract** to any folder (e.g., `C:\Hunter\` or `D:\Tools\Hunter\`)
3. **Run** `hunter_dashboard.exe`
4. **Configure** Telegram settings in the UI (optional)
5. **Click START** to begin config discovery

**That's it!** No Python, no dependencies, no installation. Fully portable.

#### Option 2: Python Version (Advanced Users / Developers)

**Prerequisites**: Python 3.8+

```bash
# 1. Clone repository
git clone https://github.com/bahmany/censorship_hunter.git
cd hunter

# 2. Create virtual environment
python -m venv .venv
# Windows: .venv\Scripts\activate
# Linux/macOS: source .venv/bin/activate

# 3. Install dependencies
pip install -r requirements.txt

# 4. Configure environment
cp .env.example .env
# Edit .env with your Telegram API credentials

# 5. Run
python main.py
```

The load balancer starts on `127.0.0.1:10808` (SOCKS5).

### ⚙️ Configuration (Python Version)

#### Required Variables

| Variable | Source | Description |
|----------|--------|-------------|
| `HUNTER_API_ID` | [my.telegram.org](https://my.telegram.org/apps) | Telegram API ID |
| `HUNTER_API_HASH` | [my.telegram.org](https://my.telegram.org/apps) | Telegram API Hash |
| `HUNTER_PHONE` | Your phone | International format (e.g., `+1234567890`) |

#### Key Optional Settings

| Variable | Default | Description |
|----------|---------|-------------|
| `TOKEN` | — | Telegram bot token for reporting |
| `CHAT_ID` | — | Telegram channel/group ID for reports |
| `HUNTER_MULTIPROXY_PORT` | `10808` | Local SOCKS5 proxy port |
| `HUNTER_WEB_PORT` | `8585` | Web dashboard port |
| `IRAN_FRAGMENT_ENABLED` | `false` | TLS fragmentation for Iran's DPI |
| `ADEE_ENABLED` | `true` | Adversarial DPI Exhaustion Engine |

See `.env.example` for the complete list.

### 🔒 Security Notes

- **Never commit** `.env`, `*.session`, or `hunter_secrets.env`
- Telegram sessions are stored locally and encrypted
- All proxy traffic is routed through your local machine
- No telemetry or data collection

### 🛠️ Advanced Usage

```bash
# Run with custom config
python main.py --config custom_config.json

# Diagnostic mode
python scripts/diagnostic.py

# Enhanced hunter with advanced features
python scripts/enhanced_hunter.py
```

### 🗂️ Project Structure

```
hunter/
├── main.py                  # Entry point
├── orchestrator.py          # Core workflow coordinator (dual benchmark lines)
├── launcher.py              # Interactive launcher
├── run.bat                  # Windows launcher script
├── .env.example             # Configuration template
├── requirements.txt         # Python dependencies
├── bin/                     # Proxy engine binaries
│   ├── xray[.exe]          # XRay Core
│   ├── sing-box[.exe]      # Sing-box
│   └── mihomo[.exe]        # Clash Meta
├── core/                    # Core modules
│   ├── config.py            # Configuration management
│   ├── models.py            # Data models (HunterBenchResult, etc.)
│   └── utils.py             # 12-tier config prioritization
├── parsers/                 # Protocol URI parsers
│   └── uri_parser.py        # VMess, VLESS, Trojan, SS, Hysteria2, TUIC
├── security/                # 2026 Anti-DPI Suite
│   ├── dpi_evasion_orchestrator.py    # Central coordinator
│   ├── tls_fingerprint_evasion.py     # JA3/JA4 spoofing
│   ├── tls_fragmentation.py           # ClientHello fragmentation
│   ├── reality_config_generator.py    # VLESS-Reality-Vision
│   ├── udp_protocols.py                 # Hysteria2 / TUIC v5
│   ├── mtu_optimizer.py                 # 5G PMTUD mitigation
│   ├── active_probe_defense.py
│   ├── split_http_transport.py        # SplitHTTP/XHTTP
│   └── stealth_obfuscation.py
├── proxy/                   # Load balancing
│   └── load_balancer.py     # Multi-backend SOCKS5 with health checks
├── network/                 # HTTP client & config fetchers
│   ├── http_client.py
│   └── flexible_fetcher.py  # Telegram-first fetching
├── telegram/                # Telegram integration
│   ├── scraper.py         # Channel scraper
│   └── fallback_sender.py   # Bot reporter
├── performance/             # Adaptive thread management
│   └── adaptive_thread_manager.py
├── web/                     # Web dashboard (Flask)
│   └── server.py
├── scripts/                 # Utility & diagnostic scripts
├── native/android/          # Android VPN app (Java + C++)
├── logs/                    # Runtime logs (git ignored)
└── runtime/                 # Runtime cache (git ignored)
```

### 📱 Android App

Full-featured Android VPN app in `native/android/`:

| Feature | Description |
|---------|-------------|
| **UI** | Material3 dark theme with Persian (Farsi) localization |
| **VPN Service** | Load balancing with up to 10 concurrent configs |
| **Split Tunneling** | Per-app VPN routing |
| **Auto-Discovery** | Fetch configs from Telegram/GitHub automatically |
| **DPI Evasion** | Full 2026 suite on Android |
| **Requirements** | Android 8.0+ (API 26+) |

See [`native/android/README.md`](native/android/README.md) for build instructions.

### 🐛 Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| **No working configs** | Missing binaries or wrong Telegram credentials | Check `bin/` has xray/sing-box/mihomo, verify `.env` |
| **Port 10808 in use** | Another app using the port | Change `HUNTER_MULTIPROXY_PORT` in `.env` |
| **Telegram auth loop** | Corrupted session | Delete `*.session` files, re-authenticate |
| **Windows permission errors** | Insufficient privileges | Run terminal as Administrator |
| **Large cache files** | Accumulated cache | Delete `runtime/*.json` and `subscriptions_cache.txt` |
| **High memory usage** | Too many configs loaded | Reduce `max_total` in config, enable REDUCED mode |
| **DPI detection** | Basic configs blocked | Enable `IRAN_FRAGMENT_ENABLED=true` |

### 🤝 Contributing

We welcome contributions from the community!

1. **Fork** the repository
2. **Branch** — Create a feature branch: `git checkout -b feature/amazing-feature`
3. **Commit** — Make your changes with clear messages
4. **Test** — Run the test suite: `python -m pytest testing/ -v`
5. **PR** — Submit a pull request to `main`

#### Development Guidelines

- Follow PEP 8 style guide
- Add docstrings for new functions/classes
- Update README.md if adding user-facing features
- Test on both Windows and Linux if possible

### 📄 License

MIT License — see [LICENSE](LICENSE) file for details.

---

## 🖥️ Hunter C++ (Native Engine)

Hunter also includes a high-performance **C++ native engine** (`hunter_cpp/`) that runs as a standalone Windows/Linux binary. This is the recommended version for production use.

### Architecture

```
hunter_cli.exe
├── Orchestrator          Main coordinator — manages all phases
│   ├── Phase -1          Kill port occupants
│   ├── Phase 0           Load configs (raw files → cache → bundle → import)
│   ├── Phase 1           Censorship detection + emergency bootstrap
│   ├── Phase 2           Start DPI evasion engines
│   ├── Phase 3           Startup banner
│   ├── Phase 4           Start all worker threads
│   └── Phase 5           Main loop + dashboard
│
├── Worker Threads (9 parallel)
│   ├── config_scanner    Scrapes configs from Telegram + GitHub
│   ├── github_bg         Background GitHub config downloader
│   ├── harvester         Aggressive multi-source harvester
│   ├── validator         Tests configs for speed and connectivity
│   ├── balancer          Health-checks the load balancer
│   ├── import_watcher    Watches config/import/ for manual imports
│   ├── telegram_pub      Publishes working configs to Telegram
│   ├── dpi_pressure      Anti-DPI pressure testing
│   └── health_monitor    RAM/CPU monitoring
│
├── ConfigDatabase        In-memory config store with health tracking
├── Load Balancer         SOCKS5 multi-backend (port 10808)
├── Gemini Balancer       Secondary balancer (port 10809)
└── Provisioned Ports     Individual proxies on 10801-10805
```

### Ports

| Port | Purpose |
|------|---------|
| **10808** | Main SOCKS5 load balancer (multi-backend) |
| **10809** | Gemini balancer (secondary) |
| **10801-10805** | Individual proxy ports (top configs) |

### Config Sources

Hunter loads configs from multiple sources in priority order:

1. **Raw files** — `config/All_Configs_Sub.txt`, `config/all_extracted_configs.txt`, `config/sub.txt`
2. **Cache** — `runtime/HUNTER_all_cache.txt` (previous session's configs)
3. **Bundle files** — `bundle/*.txt` (shipped config packs)
4. **Manual import** — `config/import/*.txt` (user-added configs)
5. **GitHub** — Automatic background fetching from GitHub repos
6. **Telegram** — Scraping from configured Telegram channels
7. **Harvester** — Aggressive multi-source harvesting

### 📥 Manual Config Import

You can manually import configs from any source:

1. **Download** configs from GitHub, Telegram, or any website
2. **Save** them as a `.txt` file (one URI per line)
3. **Copy** the file into `config/import/`
4. **Done!** Hunter scans this folder every 30 seconds

**Supported formats:**
- `.txt`, `.conf`, `.list`, `.sub` files
- One proxy URI per line (`vmess://`, `vless://`, `trojan://`, `ss://`, etc.)
- Base64-encoded subscription content (auto-decoded)

**Automatic cleanup:**
- ✅ Duplicates are removed automatically
- ✅ Malformed/invalid URIs are rejected
- ✅ vmess:// payloads are validated (must decode to JSON with `"add"` field)
- ✅ vless:// and trojan:// must have `uuid@host:port` format
- ✅ Processed files are moved to `config/import/processed/`
- ✅ Invalid URIs are logged in `config/import/invalid/last_invalid.txt`

**Supported protocols:** `vmess://`, `vless://`, `trojan://`, `ss://`, `ssr://`, `hysteria2://`, `hy2://`, `tuic://`

### Directory Structure (C++)

```
hunter/
├── bin/
│   ├── hunter_cli.exe          CLI binary
│   ├── hunter_ui.exe           Windows GUI binary
│   └── xray.exe                XRay core engine
├── config/
│   ├── All_Configs_Sub.txt     Raw config files
│   ├── all_extracted_configs.txt
│   ├── sub.txt
│   └── import/                 ★ Drop your .txt files here!
│       ├── README.txt          Instructions
│       ├── processed/          Processed files moved here
│       └── invalid/            Invalid URI logs
├── bundle/                     Bundled config packs
├── runtime/
│   ├── HUNTER_status.json      Live status (DB stats, validator progress)
│   ├── HUNTER_all_cache.txt    All-configs cache
│   ├── hunter_state.json       Persisted state
│   └── HUNTER_balancer_cache.json
└── hunter_cpp/                 C++ source code
    ├── CMakeLists.txt
    ├── include/                Headers
    └── src/                    Source files
```

### Environment Variables (C++)

| Variable | Default | Description |
|----------|---------|-------------|
| `HUNTER_CONTINUOUS` | `true` | Continuous scanning mode |
| `HUNTER_SCANNER_INTERVAL_S` | auto | Config scanner interval |
| `HUNTER_VALIDATOR_INTERVAL_S` | auto | Validator interval |
| `HUNTER_GITHUB_BG_ENABLED` | `true` | Enable GitHub background fetch |
| `HUNTER_GITHUB_BG_INTERVAL_S` | auto | GitHub fetch interval |
| `HUNTER_GITHUB_BG_CAP` | `5000` | Max configs per GitHub fetch |
| `HUNTER_HARVESTER_INTERVAL_S` | auto | Harvester interval |

### Building from Source

```bash
# Requires MSYS2 UCRT64 on Windows
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-curl \
          mingw-w64-ucrt-x86_64-zlib

cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja
cmake --build . --target hunter_cli
```

### Status Monitoring

Hunter writes live status to `runtime/HUNTER_status.json`:

```json
{
  "db": {
    "total": 355,
    "alive": 12,
    "tested_unique": 200,
    "untested_unique": 155,
    "avg_latency_ms": 450.5
  },
  "validator": {
    "last_tested": 20,
    "last_passed": 3,
    "rate_per_s": 0.8
  }
}
```

The dashboard prints every 15 seconds in the console showing uptime, DB size, alive configs, RAM usage, worker status, and DPI strategy.

---

<a name="persian-farsi"></a>
<div dir="rtl" align="right">

## 🇮🇷 فارسی

### 🌟 Hunter چیست؟

**Hunter** یک سیستم **خودکار و صنعتی** برای یافتن، تست و مدیریت پروکسی‌های V2Ray/XRay است. این ابزار برای کاربران مناطق با سانسور سنگین (ایران، چین، روسیه و ...) طراحی شده و به صورت مداوم:

1. **جمع‌آوری** — کانفیگ‌های تازه را از کانال‌های تلگرام و مخازن گیت‌هاب دریافت می‌کند
2. **تست** — هر کانفیگ را با **دو خط بنچمارک مستقل** (تلگرام و گیت‌هاب) بررسی می‌کند
3. **رتبه‌بندی** — بر اساس سرعت، پایداری و ویژگی‌های ضد DPI امتیاز می‌دهد
4. **سرویس‌دهی** — یک پروکسی SOCKS5 محلی با بالانسر هوشمند و failover خودکار ارائه می‌دهد

### 🚀 ویژگی‌های کلیدی

| ویژگی | توضیحات |
|-------|---------|
| **🔀 دو خط بنچمارک** | اعتبارسنجی جداگانه کانفیگ‌های تلگرام و گیت‌هاب با رنج پورت متفاوت |
| **⚖️ بالانسر هوشمند** | پروکسی SOCKS5 چندسروره با چک سلامت و failover خودکار |
| **🛡️ سوییت ضد DPI 2026** | تکه‌تکه‌سازی TLS، جعل JA3/JA4، VLESS-Reality-Vision، بهینه‌سازی MTU |
| **🌐 چند پروتکل** | VMess، VLESS، Trojan، Shadowsocks، Hysteria2، TUIC v5، WireGuard |
| **🤖 یکپارچگی تلگرام** | جمع‌آوری خودکار از کانال‌ها + گزارش‌دهی از طریق بات |
| **📊 داشبورد وب** | مانیتورینگ لحظه‌ای در `http://localhost:8585` |
| **📱 اپلیکیشن اندروید** | VPN-native با تمام قابلیت‌ها |
| **🧠 هوش تطبیقی** | اولویت‌بندی بر اساس DPI، chunking حافظه‌ایمن، circuit breaker |
| **🎨 رابط کاربری مدرن** | داشبورد مبتنی بر Flutter با تم Racing Neon |

### 📦 راه‌اندازی سریع

#### گزینه ۱: نسخه اجرایی ویندوز (توصیه شده برای v1.0.0)

**پیش‌نیازها**: ویندوز ۱۰/۱۱ x64، ۴ گیگابایت رم یا بیشتر

۱. **دانلود**: [Hunter-v1.0.0-Final.zip](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip) (۶۴.۵۵ مگابایت)
۲. **استخراج** در هر پوشه‌ای (مثلاً `C:\Hunter\` یا `D:\Tools\Hunter\`)
۳. **اجرا**ی `hunter_dashboard.exe`
۴. **تنظیم** تلگرام در رابط کاربری (اختیاری)
۵. **کلیک** روی START برای شروع کشف کانفیگ

**تمام!** بدون پایتون، بدون وابستگی، بدون نصب. کاملاً portable.

#### گزینه ۲: نسخه پایتون (کاربران پیشرفته / توسعه‌دهندگان)

**پیش‌نیازها**: پایتون ۳.۸+

```bash
# ۱. کلون کردن مخزن
git clone https://github.com/bahmany/censorship_hunter.git
cd hunter

# ۲. ساخت محیط مجازی
python -m venv .venv
# ویندوز: .venv\Scripts\activate
# لینوکس/مک: source .venv/bin/activate

# ۳. نصب وابستگی‌ها
pip install -r requirements.txt

# ۴. پیکربندی
# فایل .env.example را به .env کپی کنید
# مقادیر API تلگرام را از my.telegram.org دریافت کنید

# ۵. اجرا
python main.py
```

بالانسر روی `۱۲۷.۰.۰.۱:۱۰۸۰۸` (SOCKS5) راه‌اندازی می‌شود.

### ⚙️ پیکربندی (نسخه پایتون)

#### متغیرهای ضروری

| متغیر | منبع | توضیحات |
|-------|------|---------|
| `HUNTER_API_ID` | [my.telegram.org](https://my.telegram.org/apps) | شناسه API تلگرام |
| `HUNTER_API_HASH` | [my.telegram.org](https://my.telegram.org/apps) | هش API تلگرام |
| `HUNTER_PHONE` | شماره شما | فرمت بین‌الملل (مثال: `+989123456789`) |

#### تنظیمات اختیاری مهم

| متغیر | پیش‌فرض | توضیحات |
|-------|---------|---------|
| `TOKEN` | — | توکن بات تلگرام برای گزارش‌دهی |
| `CHAT_ID` | — | شناسه کانال/گروه تلگرام |
| `HUNTER_MULTIPROXY_PORT` | `10808` | پورت پروکسی SOCKS5 محلی |
| `HUNTER_WEB_PORT` | `8585` | پورت داشبورد وب |
| `IRAN_FRAGMENT_ENABLED` | `false` | تکه‌تکه‌سازی TLS برای DPI ایران |
| `ADEE_ENABLED` | `true` | موتور خستگی DPI adversarial |

فایل `.env.example` را برای لیست کامل ببینید.

### 🔒 نکات امنیتی

- **هرگز** فایل‌های `.env`، `*.session` یا `hunter_secrets.env` را commit نکنید
- session‌های تلگرام به صورت محلی و رمزنگاری‌شده ذخیره می‌شوند
- تمام ترافیک پروکسی از طریق ماشین محلی شما مسیریابی می‌شود
- هیچ telemetry یا جمع‌آوری داده‌ای وجود ندارد

### 🛠️ استفاده پیشرفته

```bash
# اجرا با تنظیمات سفارشی
python main.py --config custom_config.json

# حالت diagnostic
python scripts/diagnostic.py

# هانتر پیشرفته با ویژگی‌های اضافی
python scripts/enhanced_hunter.py
```

### 📱 اپلیکیشن اندروید

اپلیکیشن VPN کامل در `native/android/`:

| ویژگی | توضیحات |
|-------|---------|
| **رابط** | Material3 تم تیره با محلی‌سازی فارسی |
| **VPN Service** | بالانسر با تا 10 کانفیگ همزمان |
| **Split Tunneling** | مسیریابی برنامه به برنامه |
| **کشف خودکار** | دریافت کانفیگ از تلگرام/گیت‌هاب |
| **ضد DPI** | سوییت کامل 2026 روی اندروید |
| **نیازمندی** | اندروید 8.0+ (API 26+) |

برای دستورالعمل ساخت، [`native/android/README.md`](native/android/README.md) را ببینید.

### 📥 وارد کردن دستی کانفیگ

می‌توانید کانفیگ‌هایی که از گیت‌هاب، تلگرام یا هر منبع دیگری دانلود کرده‌اید را به صورت دستی وارد کنید:

1. کانفیگ‌ها را در یک فایل `.txt` ذخیره کنید (هر خط یک URI)
2. فایل را در پوشه `config/import/` کپی کنید
3. **تمام!** Hunter هر ۳۰ ثانیه این پوشه را اسکن می‌کند

**پاکسازی خودکار:**
- ✅ کانفیگ‌های تکراری حذف می‌شوند
- ✅ کانفیگ‌های خراب و نامعتبر رد می‌شوند
- ✅ فایل‌های پردازش‌شده به `config/import/processed/` منتقل می‌شوند
- ✅ URIهای نامعتبر در `config/import/invalid/last_invalid.txt` ذخیره می‌شوند

**فرمت‌های پشتیبانی‌شده:** `.txt`، `.conf`، `.list`، `.sub`

**پروتکل‌های پشتیبانی‌شده:** `vmess://`، `vless://`، `trojan://`، `ss://`، `ssr://`، `hysteria2://`، `hy2://`، `tuic://`

### 🐛 عیب‌یابی

| مشکل | علت | راه‌حل |
|------|-----|--------|
| **کانفیگ سالم پیدا نشد** | باینری ناقص یا اعتبار اشتباه | `bin/` را بررسی کنید، `.env` را Verify کنید |
| **پورت 10808 اشغال است** | برنامه دیگر از پورت استفاده می‌کند | `HUNTER_MULTIPROXY_PORT` را در `.env` تغییر دهید |
| **حلقه احراز هویت تلگرام** | session خراب | فایل‌های `*.session` را حذف و دوباره احراز هویت کنید |
| **خطای دسترسی ویندوز** | دسترسی ناکافی | ترمینال را به عنوان Administrator اجرا کنید |
| **فایل‌های کش حجیم** | انباشت cache | `runtime/*.json` و `subscriptions_cache.txt` را حذف کنید |
| **مصرف حافظه بالا** | کانفیگ‌های زیاد بارگذاری شده | `max_total` را کاهش دهید، REDUCED mode را فعال کنید |
| **تشخیص DPI** | کانفیگ‌های ساده بلاک شده‌اند | `IRAN_FRAGMENT_ENABLED=true` را فعال کنید |

### 🤝 مشارکت

مشارکت شما خوش‌آمد است!

1. مخزن را **Fork** کنید
2. **Branch** بسازید: `git checkout -b feature/amazing-feature`
3. **Commit** — تغییرات را با پیام‌های واضح commit کنید
4. **Test** — تست‌ها را اجرا کنید: `python -m pytest testing/ -v`
5. **PR** — Pull request به `main` ارسال کنید

### 📄 مجوز

مجوز MIT — فایل [LICENSE](LICENSE) را ببینید.

</div>

---

<div align="center">

**Made with ❤️ for a free and open internet**

[⭐ Star us on GitHub](https://github.com/yourusername/hunter) | [🐛 Report Bug](../../issues) | [💡 Request Feature](../../issues)

</div>
