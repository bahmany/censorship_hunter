# Hunter VPN Release Package v1.2.2

## Status

- **CLI**: up to date
- **Dashboard**: up to date
- **Installer**: built from `installer/hunter_setup.iss`

## Included Highlights

- Live `Available now` and `Checked` discovery counters on the simplified dashboard
- Continuous-cycle messaging for repeated config discovery passes
- Restored `Advanced` workspace with Stats, Configs, Logs, Docs, and Runtime tabs
- Stable Advanced tab behavior during live refreshes and rebuilds
- Legacy advanced navigation now opens the matching tab reliably

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
- This package is intended for publication as the `v1.2.2` release line.
