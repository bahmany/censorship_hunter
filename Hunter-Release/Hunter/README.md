<div align="center">

# 🏹 Hunter v1.0.0 — Complete Anti-Censorship Solution

**Self-contained Windows application with modern Flutter UI and high-performance C++ backend**

---

## 🚀 Quick Start

1. **Run** `hunter_dashboard.exe`
2. **Configure** Telegram settings in UI (optional)
3. **Click START** to begin config discovery

**That's it!** No installation, no dependencies, no Python required.

[![Python](https://img.shields.io/badge/Python-3.8+-blue.svg?logo=python&logoColor=white)](https://www.python.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20Android-lightgrey.svg)](https://github.com/yourusername/hunter)
[![Telegram](https://img.shields.io/badge/Telegram-Channel-blue.svg?logo=telegram)](https://t.me/your_channel)

**[English](#english) | [فارسی](#persian-farsi)**

</div>

---

<a name="english"></a>
## 🇬🇧 English

### 🌟 What is Hunter?

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

### 🏗️ Architecture Overview

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

### Proxy Engines (All Bundled)
- `xray.exe` — XRay Core (29MB)
- `sing-box.exe` — Sing-box universal proxy (34MB)
- `mihomo-windows-amd64-compatible.exe` — Mihomo/Clash (29MB)
- `tor.exe` — Tor network support (10MB)

### Complete Dependencies
- Flutter runtime (DLLs)
- MSYS2 C++ runtime
- OpenSSL, cURL, SSH2
- Visual C++ redistributables
- All plugin DLLs

### Documentation
- `docs/` — 27 comprehensive documentation files
- `config/` — Seed configurations
- Complete guides for installation, usage, and troubleshooting

---

## ✨ Key Features

### 🖥️ Modern Flutter UI
- **Dark Racing Neon Theme** — Professional dark interface
- **Real-time Dashboard** — Live monitoring of configs and speeds
- **QR Code Generation** — Mobile device integration
- **System Tray** — Minimize to tray with context menu
- **Single Instance** — Prevents multiple windows

### ⚡ High-Performance C++ Backend
- **Multi-threaded** — 9 parallel worker threads
- **Smart Caching** — Intelligent config prioritization
- **Continuous Validation** — Background config testing
- **Memory Efficient** — 150K config database with cleanup

### 🔍 Config Discovery
- **Multiple Sources** — Telegram, GitHub, custom feeds
- **Multi-Protocol** — VLESS, Shadowsocks, Trojan, Hysteria2, TUIC
- **DPI Evasion** — Advanced anti-censorship techniques
- **Auto-Prioritization** — 12-tier ranking system

---

## 🎯 System Requirements

- **OS**: Windows 10/11 x64
- **RAM**: 4GB minimum, 8GB recommended
- **Storage**: 150MB free space
- **Network**: Internet connection for config discovery
- **Privileges**: Standard user (no admin required)

---

## 📂 Folder Structure

```
Hunter/
├── hunter_dashboard.exe     # ← RUN THIS
├── bin/
│   ├── hunter_cli.exe       # C++ backend
│   ├── xray.exe             # Proxy engines
│   ├── sing-box.exe
│   ├── mihomo-windows-amd64-compatible.exe
│   └── tor.exe
├── data/                    # Flutter assets
├── config/                  # Seed configs
│   ├── All_Configs_Sub.txt
│   ├── sub.txt
│   └── import/              # Drop your configs here!
├── docs/                    # Documentation
├── runtime/                 # Cache (auto-created)
└── [All DLLs bundled]
```

---

## 🎮 Using Hunter

### Dashboard Tab
- View real-time statistics
- Monitor proxy ports
- Set system proxy with one click
- Track config discovery progress

### Configs Tab
- Browse discovered configs (6 tabs)
- Copy configs to clipboard
- Generate QR codes for mobile
- Test individual config speeds
- View protocol badges

### Logs Tab
- Monitor operation logs
- Color-coded by level
- Auto-scroll toggle
- 100KB memory limit

### Advanced Tab
- Configure paths
- View engine detection
- Set Telegram scrape options
- Add manual configs

---

## ⚙️ Configuration

### Via UI (Recommended)
1. Open Hunter dashboard
2. Go to Advanced section
3. Configure Telegram settings (optional)
4. Adjust speed profiles (LOW/MED/HIGH)

### Manual Config Import
1. Download configs from any source
2. Save as `.txt` file (one URI per line)
3. Copy to `config/import/`
4. Hunter scans every 30 seconds

**Supported formats**: `.txt`, `.conf`, `.list`, `.sub`  
**Supported protocols**: `vmess://`, `vless://`, `trojan://`, `ss://`, `hysteria2://`, `tuic://`

---

## 🌐 Network Ports

| Port | Purpose |
|------|---------|
| 10808 | Main SOCKS5 load balancer |
| 10809 | Gemini balancer (secondary) |
| 10801-10805 | Individual proxy ports |

Configure your browser/app to use `127.0.0.1:10808` (SOCKS5).

---

## 🛡️ Security & Privacy

- ✅ **Zero Dependencies** — Everything bundled
- ✅ **Local Processing** — No data leaves your machine
- ✅ **No Telemetry** — No tracking or data collection
- ✅ **Encrypted Storage** — Secure config storage
- ✅ **Privacy-First** — Your data stays private

---

## 🐛 Troubleshooting

| Issue | Solution |
|-------|----------|
| `hunter_cli.exe not found` | Ensure `bin/` folder exists alongside `hunter_dashboard.exe` |
| DLL errors | Check antivirus didn't quarantine files; re-extract ZIP |
| Port 10808 in use | Change port in `runtime/hunter_config.json` |
| No configs appearing | Check internet; verify Telegram settings if configured |
| UI won't open | Verify `data/` folder with `app.so` exists |

---

## 📞 Support Resources

- **Documentation**: See `docs/` folder for 27 detailed guides:
  - `v1.0.0_RELEASE_GUIDE.md` — Complete release information
  - `WINDOWS_INSTALLATION_GUIDE.md` — Detailed Windows setup
  - `FLUTTER_UI_GUIDE.md` — UI features and usage
  - `ARCHITECTURE.md` — Technical architecture
  
- **GitHub**: https://github.com/bahmany/censorship_hunter
- **Issues**: Report bugs on GitHub Issues
- **Release**: https://github.com/bahmany/censorship_hunter/releases/tag/v1.0.0

---

## 📄 License

MIT License — See `LICENSE` file for details.

---

## 🎉 Version Info

- **Version**: v1.0.0
- **Release Date**: March 10, 2026
- **Package Size**: 64.55 MB
- **Platform**: Windows 10/11 x64
- **Status**: Production Ready

**Made with ❤️ for a free and open internet**

---

**Quick Links**:
- [Download Latest](https://github.com/bahmany/censorship_hunter/releases/latest)
- [Report Bug](https://github.com/bahmany/censorship_hunter/issues)
- [View Source](https://github.com/bahmany/censorship_hunter)
