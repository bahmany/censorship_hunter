# Changelog v1.4.1

## [2026-03-27] v1.4.1

### 🚀 **Added**
- Updated installer to v1.4.1 with enhanced packaging
- Included 67,297 latest proxy configurations in runtime cache
- Added pre-loaded balancer caches (main + gemini)
- Bundled latest huntercensor.exe (17.7 MB) and sing-box.exe (34.1 MB)
- Enhanced installer script with improved process management

### 🔧 **Improved**
- Streamlined installation process (no admin required)
- Better update handling with automatic process termination
- Optimized package size (17.0 MB compressed)
- Enhanced conflict resolution for setup files
- Improved system proxy cleanup on uninstall

### 🐛 **Fixed**
- Resolved setup.bat merge conflicts between branches
- Fixed process conflicts during installation/updates
- Removed tor.exe dependencies (streamlined package)
- Improved error handling in installer edge cases
- Enhanced memory management in configuration loading

### 🗑️ **Removed**
- tor.exe from installer package (not required for core functionality)
- Conflicting setup.bat file (resolved merge issues)
- Legacy process management code

### 📦 **Packaging**
- **Installer Size:** 17.0 MB (compressed)
- **Install Size:** ~85 MB
- **Runtime Files:** Latest configs + balancer caches
- **Components:** huntercensor.exe + sing-box.exe + configs

### 🔒 **Security**
- Enhanced process isolation during installation
- Improved system proxy cleanup
- Better handling of running instances
- User-level installation (no system-wide changes)

---

## Migration Notes

- **From v1.4.0:** Seamless automatic update
- **From v1.3.x:** Fresh installation recommended
- **Config Preservation:** All user settings maintained during update
- **Runtime Reset:** Initial startup may be longer for database rebuild

---

## Download

**File:** `huntercensor-Setup-v1.4.1.exe`  
**Size:** 17.0 MB  
**Requirements:** Windows 7+ (x64)
