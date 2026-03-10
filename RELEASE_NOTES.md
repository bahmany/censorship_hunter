# Hunter v1.0.0 - Complete Anti-Censorship Solution

## 🎯 Overview
Hunter is a comprehensive proxy configuration discovery and management system designed to bypass internet censorship. This release includes a fully self-contained Windows application with all dependencies bundled.

## ✨ Key Features

### 🖥️ Modern Flutter UI
- **Dark Racing Neon Theme** - Professional dark interface with neon accents
- **Real-time Dashboard** - Live monitoring of configs, speeds, and system status
- **QR Code Generation** - Pure-Dart QR encoder for mobile config transfer
- **Staged Speed Controls** - Apply speed changes with explicit Apply button and wait feedback
- **System Tray Integration** - Minimize to tray with context menu
- **Single Instance Lock** - Prevents multiple instances

### ⚡ High-Performance C++ Backend
- **Multi-threaded Architecture** - Optimized for concurrent operations
- **Smart Caching** - Intelligent config caching and prioritization
- **Continuous Validation** - Background testing of discovered configs
- **Memory Efficient** - 150K config database with automatic cleanup
- **Real-time Status** - JSON-based communication with UI

### 🔍 Config Discovery Engine
- **Multiple Sources** - GitHub repositories, Telegram channels, and custom sources
- **Protocol Support** - VLESS, Shadowsocks, Trojan, Hysteria2, TUIC
- **Intelligent Testing** - Multi-tier validation with speed testing
- **DPI Evasion** - Advanced techniques for Iranian censorship
- **Automatic Prioritization** - 12-tier priority system

### 🛡️ Security & Privacy
- **No External Dependencies** - Completely self-contained
- **Local Processing** - All operations performed locally
- **Encrypted Storage** - Secure config storage
- **Privacy-First** - No telemetry or data collection

## 📦 What's Included

### Core Applications
- `hunter_dashboard.exe` - Flutter UI (92KB)
- `hunter_cli.exe` - C++ Backend (15.8MB)

### Proxy Engines
- `xray.exe` - XRay proxy engine (29MB)
- `sing-box.exe` - Sing-box universal proxy (34MB)
- `mihomo-windows-amd64-compatible.exe` - Mihomo/Clash core (29MB)
- `tor.exe` - Tor network support (10MB)

### Dependencies (All Bundled)
- **Flutter Runtime** - flutter_windows.dll + plugins
- **MSYS2 Runtime** - Complete C++ runtime libraries
- **Network Libraries** - OpenSSL, cURL, SSH2, HTTP/3
- **Visual C++ Runtime** - Microsoft runtime components

### Documentation & Configs
- **Complete Documentation** - 27 files in docs/
- **Seed Configs** - Initial working configurations
- **README.md** - Comprehensive setup guide
- **LICENSE** - MIT License

## 🚀 Installation

1. **Download** `Hunter-v1.0.0-Final.zip` (64.55 MB)
2. **Extract** to any directory (e.g., `C:\Hunter\`)
3. **Run** `hunter_dashboard.exe`
4. **Start** discovery with the START button

### System Requirements
- Windows 10/11 x64
- 4GB+ RAM recommended
- Internet connection for config discovery

## 🔧 Usage

### Quick Start
1. Extract ZIP to desired location
2. Run `hunter_dashboard.exe`
3. Click START to begin config discovery
4. Monitor progress in real-time dashboard

### Advanced Features
- **Speed Control**: Adjust scanning speed with LOW/MED/HIGH profiles
- **Manual Configs**: Add custom configurations
- **Telegram Integration**: Configure Telegram bot for notifications
- **QR Transfer**: Generate QR codes for mobile devices

## 🛠️ Technical Details

### Architecture
- **UI**: Flutter with custom Racing Neon theme
- **Backend**: C++17 with multi-threading
- **Communication**: JSON lines over stdin/stdout
- **Storage**: JSON files + SQLite-like database

### Performance
- **Memory**: Optimized for 4GB+ systems
- **CPU**: Adaptive thread management
- **Network**: Concurrent validation with rate limiting
- **Storage**: 150K config limit with automatic cleanup

### Security Features
- **Sandboxed**: All operations in isolated environment
- **No Internet Dependencies** for core functionality
- **Local DNS Resolution** with multiple fallbacks
- **Encrypted Config Storage**

## 🐛 Bug Fixes

### Critical Fixes
- **DLL Dependencies** - All required libraries bundled
- **Path Resolution** - Fixed relative path issues
- **Memory Leaks** - Proper resource cleanup
- **Thread Safety** - Fixed race conditions
- **DNS Resolution** - Multiple DNS servers with fallback

### UI Improvements
- **Window Management** - Proper sizing and positioning
- **Status Updates** - Real-time status synchronization
- **Error Handling** - Graceful error recovery
- **Performance** - Optimized rendering and updates

## 🔮 Future Roadmap

### v1.1 Features
- [ ] Linux support
- [ ] macOS support
- [ ] Mobile companion app
- [ ] Cloud sync for configs
- [ ] Advanced analytics dashboard

### v1.2 Features
- [ ] AI-powered config selection
- [ ] Automatic protocol switching
- [ ] Network condition adaptation
- [ ] Advanced DPI evasion techniques

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🤝 Contributing

Contributions are welcome! Please read the contributing guidelines and submit pull requests.

## 📞 Support

- **Documentation**: See `docs/` directory
- **Issues**: Report on GitHub Issues
- **Community**: Join our Telegram channel

## 🏆 Acknowledgments

- **XRay Project** - Core proxy engine
- **Sing-box** - Universal proxy platform
- **Flutter Team** - Cross-platform UI framework
- **MSYS2** - Windows development environment
- **OpenSSL** - Cryptographic library

---

**Download**: [Hunter-v1.0.0-Final.zip](https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip)

**Size**: 64.55 MB  
**Platform**: Windows x64  
**Version**: 1.0.0  
**Release Date**: March 10, 2026
