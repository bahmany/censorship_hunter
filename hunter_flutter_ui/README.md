# Hunter Dashboard

Flutter Windows desktop UI for the Hunter proxy discovery engine.

## Overview

A real-time monitoring and control dashboard that communicates with the C++ backend (`hunter_cli`) via bidirectional JSON lines over stdin/stdout.

## Sections

- **Dashboard** — Arc gauges, alive config counts, engine status, provisioned port health, system proxy controls
- **Configs** — 6-tab browser (Alive, Silver, Balancer, Gemini, All Cache, GitHub) with copy, speed test, and QR code per row
- **Logs** — Color-coded console output from the backend with auto-scroll
- **Advanced** — Speed profiles, thread/timeout sliders, Telegram configuration, GitHub source URL editor, manual config input
- **About** — Project information and version

## Build

```powershell
flutter pub get
flutter build windows --release
```

Output: `build/windows/x64/runner/Release/hunter_dashboard.exe`

## Dependencies

- `window_manager` — Window size/position control
- `system_tray` — System tray icon and context menu
- `path_provider` — Platform-specific directories
- `launch_at_startup` — Auto-start with Windows

## Theme

"Racing Neon" dark palette defined in `lib/theme.dart` — dark backgrounds with cyan, green, amber, and red accent colors.
