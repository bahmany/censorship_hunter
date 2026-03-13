# Hunter VPN Release Package v1.2.1

## Status

- **CLI**: up to date
- **Dashboard**: up to date
- **Installer**: built from `installer/hunter_setup.iss`

## Included Highlights

- Deep live recheck via `xray`, `sing-box`, and `mihomo`
- Runtime-wide pause/resume coordination during maintenance
- Post-recheck cleanup flow for dead configs or all tested configs
- Realtime WebSocket control with immediate command acknowledgements
- Adaptive command timeout handling for long-running operations

## Package Contents

```text
hunter_dashboard.exe
hunter_cli.exe
flutter_windows.dll
screen_retriever_windows_plugin.dll
system_tray_plugin.dll
window_manager_plugin.dll
app_icon.ico
data/
runtime/
```

## Notes

- The dashboard executable is the current Windows release build.
- The CLI binary is the current C++ release build.
- Proxy core binaries are bundled in the installer staging under `bin/`.
- This package is intended for publication as the `v1.2.1` release line.
