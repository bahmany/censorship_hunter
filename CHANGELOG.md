# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.2.2] - 2026-03-14

### Added

#### Flutter Desktop UI (`hunter_dashboard`)
- Dedicated `Advanced` workspace that consolidates Stats, Configs, Logs, Docs, and Runtime tools into one page
- Live `Available now` and `Checked` discovery counters on the simplified dashboard

### Changed

#### Flutter Desktop UI (`hunter_dashboard`)
- Main dashboard messaging now clearly explains that config discovery runs continuously in repeated cycles

### Fixed

#### Flutter Desktop UI (`hunter_dashboard`)
- Advanced tab selection no longer resets during routine live rebuilds and status refreshes
- Legacy statistics, logs, and docs navigation now opens the matching Advanced tab reliably
- Added widget coverage for Advanced tab persistence and external tab switching

## [1.0.0] - 2026-03-11

### Added

#### C++ Backend (`hunter_cli`)
- Autonomous orchestrator with 9 concurrent worker threads (ConfigScanner, GitHubDownloader, ContinuousValidator, AggressiveHarvester, BalancerMonitor, HealthMonitor, TelegramPublisher, DpiPressure, ImportWatcher)
- Multi-protocol URI parser supporting VMess, VLESS, Trojan, Shadowsocks, SSR, Hysteria2, and TUIC
- XRay-based proxy benchmarking with Gold/Silver/Dead tier classification
- Dual SOCKS5 load balancers (main port 10808, gemini port 10809) with least-ping strategy and up to 20 backends each
- Individual proxy port provisioning on ports 2901–2999 with 30-second health checks
- In-memory ConfigDatabase (150K+ capacity) with priority-based batch scheduling for continuous validation
- Adaptive DPI evasion orchestrator with network condition detection and strategy selection (Reality, SplitHTTP/CDN, WebSocket/CDN, gRPC/CDN, Hysteria2)
- Stealth obfuscation engine (SNI randomization, TLS fingerprint rotation, WebSocket path obfuscation)
- Telegram Bot API reporter with proxy fallback for censored networks
- Smart file-persistent config cache with deduplication and age-based staleness
- Import watcher for `config/import/` folder (auto-processes dropped .txt files)
- Hardware-aware resource scaling (CPU/RAM detection, adaptive thread pools)
- Crash resilience with SEH exception handler and crash logging
- Bidirectional stdin/stdout JSON communication with Flutter UI
- Config scraping from 20+ GitHub repositories, anti-censorship aggregators, and Iran-priority sources
- Pause/Resume and dynamic speed profiles (Low/Medium/High) with live thread/timeout adjustment
- 21 unit tests covering core utilities, models, URI parser, and ConfigDatabase

#### Flutter Desktop UI (`hunter_dashboard`)
- Real-time monitoring dashboard with arc gauges, sparkline trends, and live status indicators
- 6-tab config browser (Alive, Silver, Balancer, Gemini, All Cache, GitHub) with per-row copy, speed test, and QR code
- Color-coded log viewer with auto-scroll and 100KB memory cap
- Advanced controls: speed profiles, thread/timeout sliders, config age cleanup, manual config input, GitHub source URL editor
- System proxy integration (one-click set/clear Windows proxy per active port)
- System tray with minimize-on-close, context menu, and tooltip notifications
- Pure-Dart QR code encoder for sharing config URIs to mobile devices
- Bundled seed configurations copied to Documents/Hunter/config on first run
- Single-instance enforcement via file-based exclusive lock
- Window auto-adaptation to screen resolution (75%×85%, clamped 900×600 to 1800×1200)
- "Racing Neon" dark theme with color psychology palette

[1.2.2]: https://github.com/bahmany/censorship_hunter/releases/tag/v1.2.2
[1.0.0]: https://github.com/bahmany/censorship_hunter/releases/tag/v1.0.0
