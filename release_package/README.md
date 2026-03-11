# Hunter VPN - Dual-Protocol Release

## Build Status

### ✅ C++ CLI (Latest)
- **File**: `hunter_cli.exe` (15.8 MB)
- **Build Date**: March 11, 2026 at 1:38 PM
- **Status**: ✅ Includes all latest dual-protocol changes
- **Features**:
  - Dual-protocol ports: SOCKS + HTTP (port + 100)
  - TLS fragment anti-DPI for Iranian censorship
  - Smart routing: DNS through proxy, Iranian sites direct
  - JSON status with http_port field

### ⚠️ Flutter UI (March 10 Build)
- **File**: `hunter_dashboard.exe` (92 KB)
- **Build Date**: March 10, 2026 at 11:23 AM
- **Status**: ⚠️ Does NOT include latest dual-protocol UI changes
- **Missing Features**:
  - Dual-port display (S:2901 + H:3001)
  - Separate S/H copy buttons
  - Enhanced system proxy with HTTP+SOCKS

## Files Included

```
hunter_cli.exe              - C++ backend with latest features
hunter_dashboard.exe        - Flutter UI (older build)
flutter_windows.dll         - Flutter runtime
screen_retriever_windows_plugin.dll
system_tray_plugin.dll
window_manager_plugin.dll
app_icon.ico
data/                       - Flutter app data
```

## What's Missing

The Flutter UI needs to be rebuilt with Visual Studio 2022 complete installation:
- Visual Studio Community 2022 is installed but incomplete
- Missing "Desktop development with C++" workload
- Current proxy (127.0.0.1:11808) doesn't work for HTTPS downloads

## To Complete Flutter UI Build

1. **Install Visual Studio Components**:
   - Open Visual Studio Installer
   - Modify "Visual Studio Community 2022"
   - Add "Desktop development with C++" workload
   - Install and restart

2. **Rebuild Flutter UI**:
   ```cmd
   cd hunter_flutter_ui
   flutter build windows --release
   ```

3. **Copy New Build**:
   ```cmd
   copy build\windows\x64\runner\Release\hunter_dashboard.exe release_package\
   ```

## Current Working Features

### CLI (hunter_cli.exe)
- ✅ Dual-protocol proxy ports (SOCKS + HTTP)
- ✅ TLS fragmentation anti-DPI
- ✅ Smart routing for Iranian networks
- ✅ Provisioned ports 2901-2910 + HTTP 3001-3010
- ✅ Status JSON with http_port field

### UI (hunter_dashboard.exe)
- ✅ Basic dashboard functionality
- ✅ Config management
- ✅ Port display (SOCKS only)
- ⚠️ Missing dual-port UI enhancements

## Testing

Run the CLI to test latest features:
```cmd
hunter_cli.exe
```

The CLI will start dual-protocol proxies and emit status with both SOCKS and HTTP ports.

## Notes

- The C++ backend is fully updated with dual-protocol support
- The Flutter UI needs Visual Studio completion for latest changes
- All dependencies are included in this package
- No internet connection required for basic operation
