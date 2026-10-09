# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.8.0] - 2026-10-09

### Changed
- **Single-page UI**: the five tabs (Configs/Client/Found/Settings/About)
  are replaced by one minimal expert main view plus a slim About page.
  Top strip: `HUNTER` wordmark + About/Back toggle — no tab bar.
- **Main view**: status strip (colored state dot, active profile @port,
  hunt status), one dominant sortable profile table
  (Latency | CC | Remark | URI — click selects anywhere in the row,
  double-click connects, latency sorts fresh test results first), compact
  action row (Connect/Disconnect, Test, Import, Start/Stop, Copy v2rayN),
  and collapsed-by-default Subscriptions and Found-history sections.
- Version bump: Android `versionName` 2.3.0 (`versionCode` 5).

## [1.7.0] - 2026-10-09

### Added
- **Client mode (v2rayN-style)**: new "Client" tab turns Hunter into a proxy
  client — pick a server, connect on a stable local port, done.
  - Profile list: connect/disconnect per config, inline remark editing,
    delete selected, batch latency test, sort by latency.
  - Subscriptions: add/update/delete subscription sources, per-source
    enable toggle, auto-update on a configurable interval, import
    deduplication via the config DB, persisted to
    `runtime/subscriptions.tsv`.
  - Import: share links from clipboard (`vmess://`, `vless://`, `ss://`,
    `trojan://`, `hysteria2://`, `hy2://`, `tuic://`, `wireguard://`) and
    subscription bodies as plain line lists, base64 blobs, or
    Clash/Mihomo YAML (`proxies:` list, block and flow style).
  - Fixed local listener: client proxy binds `client_listen_port`
    (default `10808`) as a mixed SOCKS5+HTTP inbound — exactly-one-active
    profile semantics; "port busy" reported instead of hijacking.
  - System proxy: optional `client_sysproxy_enabled` — Windows registry
    (existing), GNOME `gsettings` (Linux), `networksetup` (macOS).
  - Latency column: TCP latency via the existing `ProxyTester` batch
    path, off the UI thread, with sort-by-latency.
- **WireGuard support**: `wireguard://` share links and Clash
  `type: wireguard` entries parse into a sing-box outbound
  (`peer_public_key`, `reserved`, `local_address` defaults).
- **SSR parsing**: `ssr://` decodes enough for listing/display; engines
  correctly report it as unsupported, and it is excluded from imports.

### Fixed
- Removed `ssr://` from the importable-scheme gate so dead rows stop
  accumulating in the config DB.
- Toast notifications and client latency results are now mutex-guarded
  (worker threads call them).
- Subscription fetches send a `Hunter/<ver>` User-Agent.

### Changed
- `HttpClient::get` accepts an optional per-request `user_agent`.
- Tabs: Configs · Client · Found · Settings · About.

## [1.6.0] - 2026-10-09

### Added
- **Copy Top 20 for v2rayN**: one action copies the best 20 live configs (top 5
  always United States, rest ranked by health) in v2rayN-importable share-link
  format — unsupported schemes (`ssr://`, etc.) are dropped, `hy2://` is
  normalized to `hysteria2://`, and missing remarks get a `#Hunter` fragment.
  Also writes `runtime/v2rayn_top20.txt` and a base64 subscription body
  (`runtime/v2rayn_top20_b64.txt`) for serving to v2rayN.
- **Found history**: the latest 200 discovered configs are kept in a persistent,
  deduplicated history (`runtime/found_history.tsv`) with a dedicated "Found"
  tab (when/country/URI, copy-all, export).
- **Android clipboard**: copy buttons now write to the real system clipboard
  via a JNI bridge (previously a silent no-op).

### Fixed
- Copying hundreds of configs no longer fails in v2rayN (small canonical
  top-20 payload instead of the whole live set).
- `test_proxy_tester_batch` no longer assumes nothing listens on `127.0.0.1:443`
  (uses a runtime-detected closed port).

### Changed
- Simplified UI: Dashboard and Configs tabs merged into a single "Configs" tab;
  copy/export actions consolidated into `Copy ▾` / `Export ▾` menus; duplicate
  Start/Stop/Copy/Export buttons removed from Settings; single Start/Stop
  toggle; toast shown once.

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
