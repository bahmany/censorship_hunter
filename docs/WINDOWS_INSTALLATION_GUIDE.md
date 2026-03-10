# Hunter v1.0.0 - Windows Installation Guide

## 🪟 Windows Installation (Recommended Method)

This guide covers installing Hunter v1.0.0 on Windows 10/11 using the pre-built executable package.

## 📥 Download

**Package**: `Hunter-v1.0.0-Final.zip`  
**Size**: 64.55 MB  
**Download URL**: https://github.com/bahmany/censorship_hunter/releases/download/v1.0.0/Hunter-v1.0.0-Final.zip

## 🚀 Quick Installation

### Step 1: Download
1. Go to the GitHub release page
2. Download `Hunter-v1.0.0-Final.zip`
3. Save to your Downloads folder or preferred location

### Step 2: Extract
1. Right-click on the ZIP file
2. Select "Extract All..."
3. Choose destination folder (e.g., `C:\Hunter\`)
4. Click "Extract"

### Step 3: Run
1. Open the extracted `Hunter` folder
2. Double-click `hunter_dashboard.exe`
3. Windows may show SmartScreen warning - click "More info" then "Run anyway"

### Step 4: Configure (Optional)
1. In the Hunter UI, go to Advanced section
2. Configure Telegram API settings if desired
3. Set your preferences

### Step 5: Start
1. Click the START button in the dashboard
2. Hunter will begin discovering and testing configs
3. Monitor progress in the real-time dashboard

## 📁 Recommended Installation Locations

- `C:\Hunter\` - System drive
- `D:\Tools\Hunter\` - Secondary drive
- `%USERPROFILE%\Hunter\` - User directory
- Any external drive for portable use

## ✅ System Requirements

- **OS**: Windows 10 or 11 (64-bit)
- **RAM**: 4GB minimum, 8GB recommended
- **Storage**: 150MB free space
- **Network**: Internet connection for config discovery
- **Privileges**: Standard user (no admin required)

## 🔧 Post-Installation

### First Run
1. Hunter creates `runtime/` folder for cache
2. Default SOCKS5 proxy starts on port 10808
3. Dashboard shows real-time statistics

### Configuration Files
- `runtime/hunter_config.json` - Application settings
- `config/` - Seed configs and imports
- `runtime/HUNTER_status.json` - Live status data

### System Tray
- Hunter minimizes to system tray
- Right-click tray icon for menu
- Double-click to restore window

## 🔄 Uninstallation

Hunter is fully portable:
1. Exit Hunter (File → Exit)
2. Delete the Hunter folder
3. No registry entries or system changes left behind

## 🛡️ Windows Defender / Antivirus

Hunter may trigger false positives due to:
- Proxy engine binaries (xray.exe, etc.)
- Network activity for config testing
- DLL files from MSYS2 runtime

**To allow Hunter**:
1. Windows Security → Virus & threat protection
2. Manage settings → Add or remove exclusions
3. Add exclusion for Hunter folder

## 🐛 Troubleshooting

### "hunter_cli.exe not found" error
- Ensure `bin/hunter_cli.exe` exists
- Check extraction was complete
- Don't move executables from their locations

### DLL errors
- All required DLLs are bundled
- Check no files were quarantined by antivirus
- Re-extract the ZIP file

### Port 10808 already in use
- Change port in runtime/hunter_config.json
- Or stop other application using port 10808

### No configs appearing
- Check internet connection
- Verify Telegram settings if configured
- Check logs in runtime/ folder

## 📊 Verifying Installation

Successful installation shows:
1. Hunter dashboard window opens
2. Status shows "Ready" or "Offline"
3. Clicking START begins config discovery
4. Working configs appear in Alive tab

## 🎮 Using Hunter

### Basic Usage
1. **START** - Begin config discovery and testing
2. **STOP** - Pause all operations
3. **Dashboard** - Monitor statistics and charts
4. **Configs** - View and copy working configs
5. **Logs** - View detailed operation logs

### Copy Configs
1. Go to Configs section
2. Click copy button on any alive config
3. Or click QR code to generate for mobile

### Set System Proxy
1. In Dashboard section
2. Click "USE" button on any port
3. Or click "CLEAR PROXY" to disable

## 📞 Support

For issues or questions:
- Check docs/ folder for more guides
- Visit GitHub Issues page
- Review troubleshooting section above

---

**Version**: v1.0.0  
**Last Updated**: March 10, 2026
