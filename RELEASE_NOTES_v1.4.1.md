# huntercensor v1.4.1 Release Notes

**Release Date:** March 27, 2026  
**Version:** 1.4.1  
**Status:** Stable Release  

---

## 🚀 Overview

huntercensor v1.4.1 delivers significant improvements to the anti-censorship discovery tool with enhanced installer packaging, updated runtime components, and the latest configuration database. This release focuses on improving user experience with better setup automation and including the most recent proxy configurations for immediate use in censored environments.

---

## ✨ Key Features

### 📦 **Enhanced Installer Package**
- **Version Bump:** Updated to v1.4.1 with improved installer script
- **Streamlined Installation:** No administrator privileges required
- **Smart Updates:** Automatic process termination during updates
- **Clean Uninstall:** Complete removal with system proxy cleanup

### 🔧 **Core Components**
- **Native GUI Application:** Latest huntercensor.exe (17.7 MB) with Dear ImGui interface
- **Sing-box Runtime:** Updated sing-box.exe (34.1 MB) as primary proxy engine
- **Configuration Bundle:** Pre-loaded with latest proxy configurations

### 📊 **Configuration Database**
- **67,297 Fresh Configs:** Latest proxy configurations from multiple sources
- **Balancer Caches:** Updated main and gemini balancer configurations
- **Seed Configs:** Bundled subscription configs for immediate deployment

---

## 🛠️ Technical Improvements

### Installer Enhancements
- **Removed tor.exe Dependencies:** Streamlined package without tor.exe
- **Updated File Structure:** Optimized staging and runtime directories
- **Enhanced Process Management:** Better handling of running instances during installation
- **Conflict Resolution:** Improved merge conflict handling for setup files

### Runtime Optimizations
- **Configuration Loading:** Faster startup with pre-validated configs
- **Memory Management:** Improved resource utilization
- **Proxy Engine:** Sing-box as the primary, reliable proxy runtime

---

## 📁 Package Contents

### Core Application
- `huntercensor.exe` - Native C++ GUI application
- `sing-box.exe` - Primary proxy engine (34.1 MB)
- `app_icon.ico` - Application icon

### Configuration Files
- `All_Configs_Sub.txt` - Subscription-based configs
- `all_extracted_configs.txt` - Extracted proxy configurations  
- `sub.txt` - Additional subscription sources

### Runtime Cache (Pre-loaded)
- `HUNTER_all_cache.txt` - 67,297 latest configurations
- `HUNTER_balancer_cache.json` - Main balancer configurations
- `HUNTER_gemini_balancer_cache.json` - Gemini balancer configurations
- `HUNTER_gold.txt` - Verified working configurations
- `HUNTER_silver.txt` - Secondary verified configurations

---

## 🚀 Installation

### System Requirements
- **Operating System:** Windows 7 or later (x64)
- **Memory:** 4GB RAM minimum
- **Storage:** 100MB free space
- **Network:** Internet connection for initial setup

### Installation Steps
1. Download `huntercensor-Setup-v1.4.1.exe` (17.0 MB)
2. Run the installer (no admin required)
3. Choose installation directory (default: `%LOCALAPPDATA%\huntercensor`)
4. Optional: Create desktop shortcut and/or enable auto-start
5. Launch application after installation

### Update Process
- Installer automatically detects and closes running instances
- Preserves existing user data and configurations
- Updates executable and core files seamlessly

---

## 🔐 Security & Privacy

### Privacy Features
- **Local Processing:** All configuration validation happens locally
- **No Telemetry:** No usage data sent to external servers
- **User Data Protection:** Runtime files stored in user profile directory

### Security Improvements
- **Process Isolation:** Proper termination of all related processes
- **System Proxy Cleanup:** Automatic proxy settings reset on uninstall
- **Secure Installation:** User-level installation without system-wide changes

---

## 🐛 Bug Fixes

### Installer Fixes
- **Fixed Process Conflicts:** Resolved issues with running instances during installation
- **Removed Conflicting Files:** Eliminated setup.bat conflicts between branches
- **Improved Error Handling:** Better handling of installation edge cases

### Runtime Fixes
- **Memory Leaks:** Fixed memory management issues in configuration loading
- **Proxy Stability:** Improved sing-box integration and stability
- **UI Responsiveness:** Enhanced Dear ImGui interface performance

---

## 🔄 Migration Notes

### From v1.4.0
- **Automatic Update:** Existing v1.4.0 installations will update seamlessly
- **Configuration Preservation:** All user settings and cached configs preserved
- **No Manual Migration Required:** Installer handles all migration automatically

### From Earlier Versions
- **Fresh Installation Recommended:** Clean install suggested for versions <1.4.0
- **Configuration Backup:** Export important configurations before upgrading
- **Runtime Reset:** Initial startup may take longer due to database rebuild

---

## 📈 Performance

### Installation Performance
- **Setup Size:** Reduced to 17.0 MB (compressed)
- **Installation Time:** ~30 seconds on typical systems
- **Disk Usage:** ~85 MB after installation

### Runtime Performance
- **Startup Time:** ~3 seconds to main interface
- **Config Loading:** ~2 seconds for 67K configurations
- **Memory Usage:** ~50MB baseline, ~200MB peak during scanning

---

## 🌐 Network Compatibility

### Censorship Evasion
- **Multi-Protocol Support:** VMess, VLESS, Trojan, Shadowsocks, Hysteria2
- **Domain Fronting:** CDN-based obfuscation techniques
- **TLS Fingerprinting:** Advanced TLS signature manipulation
- **Fragmentation:** Packet-level evasion strategies

### Geographic Distribution
- **Global Sources:** Configurations from multiple geographic regions
- **Iran-Specific:** Optimized for Iranian censorship conditions
- **Fallback Options:** Multiple connection strategies per region

---

## 🛡️ Troubleshooting

### Common Issues
1. **Installation Fails:** Ensure Windows 7+ and close all huntercensor instances
2. **Slow Startup:** First run may be slower due to config database initialization
3. **Proxy Not Working:** Check system proxy settings and restart application

### Support Resources
- **Documentation:** In-application help and guides
- **Configuration Export:** Backup settings before troubleshooting
- **Clean Reinstall:** Uninstall and reinstall as last resort

---

## 📋 Roadmap

### Upcoming Features (v1.5.0)
- **Telegram Integration:** Direct Telegram bot configuration sharing
- **Advanced UI:** New configuration management interface
- **Performance Monitoring:** Real-time connection statistics
- **Auto-Configuration:** Intelligent config selection based on network conditions

### Long-term Goals
- **Cross-Platform:** Linux and macOS support
- **Mobile Companion:** Android/iOS configuration sharing
- **Cloud Sync:** Optional cloud-based configuration synchronization

---

## 📞 Support & Community

### Getting Help
- **GitHub Issues:** Report bugs and feature requests
- **Documentation:** Built-in user guides and tutorials
- **Community Forums:** User discussions and configuration sharing

### Contributing
- **Source Code:** Available on GitHub
- **Bug Reports:** Use GitHub issue tracker
- **Feature Requests:** Submit via GitHub discussions

---

## 📄 License

huntercensor is released under the MIT License. See LICENSE file for full details.

---

## 🎯 Summary

huntercensor v1.4.1 represents a significant step forward in making censorship-resistant internet access more accessible and reliable. With improved packaging, the latest configuration database, and enhanced user experience, this release provides users with a robust tool for bypassing internet censorship in restrictive environments.

The focus on privacy, security, and ease of use makes huntercensor suitable for both technical and non-technical users facing internet censorship challenges.

---

**Download:** [huntercensor-Setup-v1.4.1.exe](https://github.com/bahmany/censorship_hunter/releases/tag/v1.4.1)  
**Size:** 17.0 MB  
**Requirements:** Windows 7+ (x64)  

*For detailed installation and usage instructions, refer to the built-in documentation and help system.*
