# Hunter Changelog

## v1.2.2 — 2026-03-14

### Bug Fixes
- **Advanced tabs now stay selected correctly** — Routine live rebuilds and status refreshes no longer force the Advanced page back to an older tab.
- **Legacy advanced navigation is now stable** — Links to Statistics, Logs, and Docs correctly open the matching tab inside the new Advanced workspace.

### New Features
- **Advanced workspace restored** — All professional tools are available again in a dedicated tabbed page with Stats, Configs, Logs, Docs, and Runtime sections.

### UI / Dashboard
- **Live discovery counters on the main dashboard** — The simplified home screen now shows `Available now` and `Checked` counts.
- **Continuous-cycle messaging** — The dashboard explicitly explains that Hunter keeps scanning in repeated discovery cycles.

---

## v1.2.0 — 2026-03-13

### Bug Fixes
- **Sequential engine testing** — Changed from parallel to sequential (xray → sing-box → mihomo). Prevents resource exhaustion from spawning 3 engine processes per config simultaneously. Returns immediately on first success.
- **Telegram-only proxies accepted as alive** — In Iran, many working proxies route TCP to Telegram but fail HTTP downloads due to DPI. These are now counted as alive with a high latency penalty so HTTP-capable configs are preferred.
- **Import timeout for large config batches** — Large imports (10k+ configs) now processed in batches of 5000 with progress reporting, preventing UI/main-thread blocking.
- **Speed test timeout** — Now uses the user-configured timeout parameter instead of hardcoded 3s/5s which were too short for Iranian censorship conditions.

### New Features
- **Config database persistence** — Health records saved to `runtime/HUNTER_config_db.tsv` every 60 seconds and on shutdown. All discovered configs survive restarts.
- **WebSocket real-time communication** — Two channels: `ws://127.0.0.1:15491` (command/control with ack/result) and `ws://127.0.0.1:15492` (monitoring/log/status streaming).
- **Mixed inbound ports** — Single port serves both HTTP and SOCKS auto-detection (like v2rayNG), no separate SOCKS and HTTP ports needed.

### Performance
- **Exponential backoff for failed configs** — Configs with <5 consecutive failures retest after 60s, 5-10 failures after 5 minutes, 10+ failures after 30 minutes. Prevents wasting resources on repeatedly failing configs.
- **3x fewer concurrent processes** — Sequential engine testing means only 1 engine process per config instead of 3.

### UI / Dashboard
- **Discovery timestamps** — Each config row shows when it was first seen, last confirmed alive, and last tested.
- **QR code generation** — Generate QR codes for any config URI for easy mobile transfer.
- **Version display** — CLI and Dashboard versions shown in About section and top bar.
- **Full changelog** — About section now includes structured changelog with color-coded tags (FIX, NEW, PERF, UI).

---

## v1.1.0 — 2026-02-20

### New Features
- **Multi-engine support** — XRay, Sing-box, and Mihomo engines for config testing and proxy serving.
- **Continuous background validation** — ConfigDatabase tracks health of all discovered configs with automatic retesting.
- **Provisioned proxy ports** — Individual proxy ports (2901-2999) with automatic health checks and dead proxy replacement.
- **System proxy integration** — Set/clear Windows system proxy per port from the dashboard.

### UI / Dashboard
- **Racing Neon dark theme** — Professional color psychology palette (neon cyan, green, amber, red, purple).
- **Real-time dashboard** — Stats gauges, sparkline trends, engine status, live activity feed.
- **Pause/Resume + Speed controls** — Profile presets (LOW/MED/HIGH), thread count slider, timeout slider.

---

## v1.0.0 — 2026-01-15

### Initial Release
- **Config scraping** — Telegram channels, GitHub repositories, HTTP/anti-censorship sources.
- **XRay-based proxy testing** — Automated config validation with latency measurement.
- **Load balancing** — Main + Gemini balancers with XRay leastPing strategy.
- **Flutter desktop dashboard** — Start/Stop controls, log viewer, config browser with copy/export.
- **System tray** — Minimize to tray, notifications for new configs.
- **Bundled seed configs** — First-run bootstrap with pre-packaged working configs.
- **Single instance enforcement** — Prevents multiple dashboard instances.
