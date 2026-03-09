# Hunter Anti-Censorship Dashboard v1.0.0

## Quick Start
Double-click **hunter_dashboard.exe** (or **Start_Hunter.bat**)

## Directory Structure
`
Hunter/
â”œâ”€â”€ hunter_dashboard.exe      â† Main GUI application (start this)
â”œâ”€â”€ Start_Hunter.bat          â† Alternative launcher
â”œâ”€â”€ data/                     â† Flutter runtime (DO NOT DELETE)
â”œâ”€â”€ bin/                      â† Backend engines
â”‚   â”œâ”€â”€ hunter_cli.exe        â† C++ backend (started by dashboard)
â”‚   â”œâ”€â”€ hunter_ui.exe         â† Win32 native UI (alternative)
â”‚   â”œâ”€â”€ xray.exe              â† XRay proxy engine
â”‚   â”œâ”€â”€ sing-box.exe          â† Sing-box proxy engine
â”‚   â””â”€â”€ mihomo-*.exe          â† Mihomo (Clash Meta) engine
â”œâ”€â”€ runtime/                  â† Runtime data (auto-managed)
â”‚   â””â”€â”€ hunter_config.json    â† Configuration file
â””â”€â”€ config/import/            â† Drop custom configs here
`

## System Requirements
- Windows 10/11 (x64)
- 4 GB RAM minimum (8 GB recommended)
- 500 MB free disk space
- Internet connection

## How It Works
1. The dashboard starts the C++ backend automatically
2. The backend discovers, tests, and ranks proxy configurations
3. Working proxies are available on SOCKS5 port 10808
4. Configurations are categorized as Gold (fast) or Silver (working)

## Troubleshooting
- **Dashboard won't start**: Install Visual C++ Redistributable:
  https://aka.ms/vs/17/release/vc_redist.x64.exe
- **No configs found**: Check your internet connection and firewall
- **Antivirus blocking**: Add the Hunter folder to exclusions

## License
Educational and research purposes only.
https://github.com/bahmany/censorship_hunter