# Hunter Dashboard MSI Installer

This installer package provides a complete, ready-to-use Hunter Anti-Censorship Dashboard with all necessary components.

## What's Included

### Core Components
- **Flutter Dashboard** (`hunter_dashboard.exe`) - Modern UI for monitoring and control
- **C++ CLI** (`hunter_cli.exe`) - Backend proxy testing engine
- **All Required DLLs** - OpenSSL, curl, zlib, and other dependencies

### Proxy Engines
- **XRay** (`xray.exe`) - Primary proxy engine
- **sing-box** (`sing-box.exe`) - Alternative proxy engine
- **mihomo** (`mihomo-windows-amd64-compatible.exe`) - Clash Meta fork
- **Tor** (`tor.exe`) - Tor network integration

### Configuration Files
- **All_Configs_Sub.txt** (2.8 MB) - Comprehensive proxy configuration collection
- **all_extracted_configs.txt** (7.0 MB) - Additional extracted configurations
- **sub.txt** (9.6 KB) - Subscription-based configurations

### Runtime Components
- **Visual C++ Redistributable** - Automatically installed if needed
- **Runtime directory** - Created for logs and temporary files
- **Start Menu & Desktop shortcuts** - Easy access to the application

## Installation

### Prerequisites
- Windows 10/11 (x64)
- Administrator privileges (for VC++ redistributable installation)

### Installation Steps
1. Download `hunter_dashboard.msi`
2. Right-click and select "Run as administrator"
3. Follow the installation wizard
4. Launch Hunter Dashboard from Start Menu or Desktop shortcut

## Post-Installation Setup

### First Run
1. Launch Hunter Dashboard
2. The application will automatically:
   - Create the `runtime` directory
   - Load configuration files from the installation directory
   - Start with thousands of pre-loaded proxy configurations

### Configuration Location
- **Install Directory**: `C:\Program Files\Hunter\`
- **Configuration Files**: `C:\Program Files\Hunter\`
- **Runtime Data**: `C:\Program Files\Hunter\runtime\`
- **User Data**: `%USERPROFILE%\Documents\Hunter\` (created automatically)

## Features

### Dashboard Capabilities
- Real-time proxy testing and validation
- Automatic configuration sorting (Gold/Silver categories)
- Live activity monitoring
- Responsive design for all screen sizes
- Built-in documentation browser

### Proxy Testing
- Multi-engine support (XRay, sing-box, mihomo)
- Automatic Telegram connectivity testing
- Speed testing with fallback servers
- DNS resolution through proxy tunnel

### Configuration Management
- Thousands of pre-loaded configurations
- Automatic deduplication
- Smart caching system
- Import/export capabilities

## Troubleshooting

### Common Issues

#### Application Won't Start
- Ensure Visual C++ Redistributable was installed
- Check Windows Event Viewer for error details
- Run as administrator if permission issues occur

#### Proxy Testing Fails
- Verify internet connectivity
- Check firewall settings
- Ensure proxy engines are not blocked by antivirus

#### Configuration Files Missing
- Reinstall the MSI package
- Check installation directory integrity
- Verify files weren't quarantined by security software

### Getting Help
- GitHub Repository: https://github.com/bahmany/censorship_hunter
- Documentation: Available in-app under "Docs" section
- Issues: Report bugs on GitHub Issues

## Technical Details

### Installation Size
- **Base Installation**: ~150 MB
- **Runtime Data**: Variable (up to 500 MB)
- **Total Disk Space**: ~650 MB recommended

### Network Requirements
- Internet connection for proxy testing
- No proxy required for initial setup
- HTTPS access to configuration sources

### System Impact
- CPU Usage: Low during idle, moderate during testing
- Memory Usage: ~50-200 MB depending on activity
- Network Usage: Variable based on proxy testing intensity

## Security & Privacy

### Data Collection
- No personal data collected
- Anonymous usage statistics only
- Local configuration storage only

### Network Security
- All proxy testing done through configured proxies
- DNS queries routed through proxy tunnel
- No direct connections to external servers

## Version Information

- **Version**: 1.0.0.0
- **Build Date**: 2026-03-08
- **Supported Platforms**: Windows 10/11 x64
- **Architecture**: 64-bit only

## License

This software is provided "as-is" for educational and research purposes.
The user is responsible for compliance with local laws and regulations.

See included license.rtf file for full terms.
