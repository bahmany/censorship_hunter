# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2026-03-21

### Added

#### Packet-Level DPI Bypass System
- **TLS ClientHello Fragmentation**: Splits TLS handshake into 100-200 byte chunks with 10-20ms delays to bypass SNI-based filtering
- **TTL Manipulation**: Configurable packet TTL to expire before DPI boxes (hop 8-12)
- **TCP Fragmentation**: Forces small MSS (536) and disables Nagle's algorithm for additional evasion
- **Domain Fronting**: CDN-based destination hiding for compatible proxies
- **Fake SNI Injection**: Decoy SNI handshake for experimental evasion
- **GoodbyeDPI-style Methods**: HTTP Host fragmentation, TCP window manipulation, fake packet injection

#### Edge Router Bypass Enhancements
- **Domestic Zone Filtering**: Bypass methods now apply ONLY to Iranian/domestic hops (not international)
- **MAC Address Display**: Shows resolved MAC addresses for each domestic hop in logs
- **Manual Testing API**: `testBypassOnSpecificIp()` for targeted bypass testing
- **Domestic Hop Discovery**: `getDomesticHopsWithMac()` returns list of domestic hops with MAC addresses
- **Transparent Logging**: Detailed step-by-step bypass execution logs with hop categorization

#### XRay Integration
- **Automatic Fragmentation**: All TLS proxy connections automatically use ClientHello fragmentation
- **Config Injection**: Fragment settings injected into XRay streamSettings transparently
- **Zero Configuration**: Bypass enabled by default, no user setup required

### Changed

#### DPI Evasion Strategy
- **From Route Injection to Packet-Level**: Shifted from ineffective local route injection to proven packet manipulation
- **Targeted Application**: Bypass methods now target only domestic (Iranian) network equipment
- **Enhanced Logging**: Added MAC address display and domestic hop categorization

### Technical Details

#### Packet Bypass Implementation
- **Files Added**: `include/security/packet_bypass.h` (352 lines), `src/security/packet_bypass.cpp` (485 lines)
- **API Classes**: `PacketBypass`, `GoodbyeDpiBypass`, `IranianDpiBypass`
- **XRay Config**: Automatic `"fragment": {"packets": "tlshello", "length": "100-200", "interval": "10-20"}`

#### Edge Router Bypass
- **Domestic Detection**: Uses `isLikelyIranianIp()` to filter Iranian IP ranges
- **MAC Resolution**: ARP resolution for each domestic hop with fallback handling
- **Manual Controls**: API for testing specific IPs and retrieving hop information

## [1.2.2] - 2026-03-14

### Added

#### Native App (`hountersansor`)
- Dedicated `Advanced` workspace that consolidates stats, configs, logs, and runtime tools into one page
- Live discovery counters and simplified main pages for daily operation

### Changed

#### Native App (`hountersansor`)
- Main UI messaging now clearly explains that config discovery runs continuously in repeated cycles

### Fixed

#### Native App (`hountersansor`)
- Advanced page selection no longer resets during routine live refreshes
- Navigation across stats, logs, and advanced controls now lands on the intended section reliably

## [1.0.0] - 2026-03-11

### Added

#### C++ Core (`hountersansor_cli`)
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
- Import watcher for `config/import/` folder (auto-processes dropped `.txt` files)
- Hardware-aware resource scaling (CPU/RAM detection, adaptive thread pools)
- Crash resilience with SEH exception handler and crash logging
- Realtime command and status surfaces for local UI and automation
- Config scraping from 20+ GitHub repositories, anti-censorship aggregators, and Iran-priority sources
- Pause/Resume and dynamic speed profiles (Low/Medium/High) with live thread/timeout adjustment
- 21 unit tests covering core utilities, models, URI parser, and ConfigDatabase

#### Native App (`hountersansor`)
- Real-time monitoring and control UI built directly into the native Windows executable
- Config browser, log viewer, censorship tools, and advanced runtime controls in one process
- Lightweight native shell without extra desktop runtime payload

[1.2.2]: https://github.com/bahmany/censorship_hunter/releases/tag/v1.2.2
[1.0.0]: https://github.com/bahmany/censorship_hunter/releases/tag/v1.0.0
