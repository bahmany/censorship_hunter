# Hunter v1.0.0 - Flutter UI Guide

## 🎨 Modern Flutter Dashboard

Hunter v1.0.0 introduces a complete Flutter-based UI that replaces the previous web dashboard with a native Windows application.

## ✨ UI Features

### Dark Racing Neon Theme
- Professional dark interface with neon cyan and amber accents
- Color-coded status indicators
- Glassmorphic card design
- Responsive layout that adapts to window size

### Dashboard Section
- **Hero Gauges**: Real-time speed and config count displays
- **Active Ports**: Live status of all proxy ports
- **System Proxy Controls**: One-click system proxy setting
- **Progress Indicators**: Current operation status

### Configs Section
- **6 Tabs**: Alive, Silver, Balancer, Gemini, All Cache, GitHub
- **Copy Buttons**: One-click config copying
- **QR Codes**: Generate QR for mobile transfer
- **Speed Test**: Test individual config speeds
- **Protocol Badges**: Visual identification (VLESS, SS, Trojan)

### Logs Section
- Color-coded log levels
- Auto-scroll toggle
- 100KB memory limit indicator
- Filter and search capabilities

### Advanced Section
- Path configuration
- Engine detection with file sizes
- Telegram scrape settings
- Manual config input

### About Section
- Version information
- Links to GitHub repository
- Credits and acknowledgments

## 🖱️ Navigation

### Sidebar
- 56px wide with neon indicators
- Sections: Dashboard, Configs, Logs, Advanced, About
- Status dot at bottom showing system state

### Top Bar
- **START/STOP** buttons with neon styling
- State pill (OFFLINE/RUNNING/PAUSED/ERROR)
- Error display area
- Minimize and Exit buttons

### System Tray
- Minimize to tray on close
- Tray icon with context menu
  - Show Hunter
  - Start/Stop
  - Exit
- Click tray icon to restore

## ⚡ Speed Controls

### Speed Profiles
- **LOW**: Conservative scanning (1-5 threads)
- **MED**: Balanced performance (10-20 threads)
- **HIGH**: Maximum throughput (30-50 threads)

### Staged Changes
1. Select profile or adjust sliders
2. Click "APPLY" button
3. Wait for "Applying..." confirmation
4. Changes take effect immediately

## 📱 QR Code Generation

### For Mobile Transfer
1. Find desired config in Configs section
2. Click purple QR icon
3. QR dialog opens with:
   - Large scannable QR code
   - Config URI text (selectable)
   - Close button

### Pure-Dart Implementation
- No external dependencies
- Versions 1-10 supported
- Byte mode encoding
- ECC-L error correction

## 🔔 Notifications

### Tray Notifications
- Appear when new configs discovered while minimized
- Show config count and discovery timestamp
- Click to restore window

### Sound Alerts
- Beep when new alive configs found
- Configurable in settings
- Uses system notification sound

## 🎮 Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl + S | Start/Stop Hunter |
| Ctrl + M | Minimize to tray |
| Ctrl + Q | Exit application |
| F5 | Refresh dashboard |

## 🛠️ Customization

### Window Size
- Auto-detects screen bounds
- Default: 75% × 85% of screen
- Minimum: 900×600 pixels
- Maximum: 1800×1200 pixels

### Theme
- Dark theme only (Racing Neon)
- Accent colors: Neon Cyan (#22D3EE), Neon Green (#34D399), Neon Amber (#FBBF24)
- Background: Dark navy (#0A0E17)

## 📊 Real-time Updates

### Status Updates (every 2 seconds)
- Phase (startup/running/bootstrap)
- Pause state
- Speed profile settings
- Thread counts

### Dashboard Refresh (every 2 seconds)
- Alive configs list
- Balancer backends
- Provisioned ports
- Gold/Silver/Bronze counts

### Log Updates (real-time)
- New log lines appended immediately
- Color coding applied instantly
- Auto-scroll when enabled

## 🔧 System Integration

### Single Instance Lock
- Prevents multiple Hunter instances
- Uses file lock in temp directory
- Shows warning if already running

### Window Management
- Proper minimize/restore behavior
- Saves window position (optional)
- Handles DPI scaling correctly

### Process Management
- Launches hunter_cli.exe from bin/
- Monitors process health
- Clean shutdown on exit
- Stops via stop.flag file

## 🐛 UI Troubleshooting

### Window doesn't open
- Check hunter_dashboard.exe exists
- Verify data/ folder with app.so
- Check Windows Event Viewer for errors

### UI is blank/white
- Check data/flutter_assets/ exists
- Verify GPU drivers are updated
- Try running as administrator

### Tray icon missing
- Check app_icon.ico exists
- Restart Explorer.exe
- Re-extract Hunter package

### Slow UI response
- Reduce update frequency in settings
- Lower thread counts
- Close other applications

### Text rendering issues
- Install latest Windows updates
- Check font installation
- Verify GPU acceleration enabled

## 🎨 UI Architecture

### File Structure
```
hunter_flutter_ui/
├── lib/
│   ├── main.dart              # App entry + HunterPage
│   ├── theme.dart             # Color palette (C class)
│   ├── models.dart            # Enums and data classes
│   ├── services.dart          # File I/O, speed test
│   └── widgets/
│       ├── dashboard_section.dart   # Hero gauges, ports
│       ├── configs_section.dart     # 6 tabs, QR codes
│       ├── logs_section.dart        # Console output
│       ├── advanced_section.dart    # Settings
│       ├── about_section.dart       # Info
│       ├── qr_painter.dart          # QR encoder
│       ├── qr_dialog.dart           # QR display
│       └── gauge_painter.dart       # Arc gauges
├── assets/
│   └── configs/               # Bundled seed configs
└── build/
    └── windows/
        └── x64/
            └── runner/
                └── Release/
                    └── hunter_dashboard.exe
```

### Communication with Backend
- **stdin**: Send commands to C++ CLI
- **stdout**: Receive status updates
- **JSON lines**: Structured data exchange
- **Status files**: Rich data (HUNTER_status.json)

### Build Process
```bash
# Development
flutter run

# Release build
flutter build windows --release

# Output
build/windows/x64/runner/Release/hunter_dashboard.exe
```

## 📈 Performance Tips

### For Smooth UI
- Use HIGH speed profile for faster scanning
- Keep 4GB+ RAM free
- Close unnecessary browser tabs
- Disable animations if needed

### For Faster Config Discovery
- Enable Telegram scraping
- Use GitHub background fetcher
- Set higher thread counts
- Enable all proxy engines

## 🔄 Updates

### UI Updates
- UI updates are included in Hunter releases
- No separate update mechanism needed
- Simply download new release ZIP

### Settings Persistence
- Telegram settings saved to runtime/hunter_config.json
- Window position optionally saved
- Speed controls reset to defaults on restart

---

**Version**: v1.0.0 Flutter UI  
**Theme**: Racing Neon Dark  
**Framework**: Flutter 3.x Windows Desktop
