# Hunter Dashboard Portable Bundle Creator
# Creates a self-extracting executable with all components

param(
    [string]$OutputPath = ".\output",
    [string]$Version = "1.0.0.0"
)

Write-Host "Creating Hunter Dashboard Portable Bundle..." -ForegroundColor Green

# Create output directory
if (!(Test-Path $OutputPath)) {
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
}

# Create temporary bundle directory
$BundleDir = ".\bundle_temp"
if (Test-Path $BundleDir) {
    Remove-Item -Recurse -Force $BundleDir
}
New-Item -ItemType Directory -Path $BundleDir | Out-Null

Write-Host "Step 1: Copying application files..." -ForegroundColor Yellow

# Copy Flutter Dashboard
Copy-Item "..\hunter_flutter_ui\build\windows\x64\runner\Release\hunter_dashboard.exe" $BundleDir -Force

# Copy C++ CLI and dependencies
Copy-Item "..\bin\hunter_cli.exe" $BundleDir -Force
Copy-Item "..\bin\libcrypto-3-x64.dll" $BundleDir -Force
Copy-Item "..\bin\libssl-3-x64.dll" $BundleDir -Force
Copy-Item "..\bin\libcurl-4.dll" $BundleDir -Force
Copy-Item "..\bin\libzstd.dll" $BundleDir -Force
Copy-Item "..\bin\zlib1.dll" $BundleDir -Force
Copy-Item "..\bin\libssh2-1.dll" $BundleDir -Force
Copy-Item "..\bin\libnghttp2-14.dll" $BundleDir -Force
Copy-Item "..\bin\libnghttp3-9.dll" $BundleDir -Force
Copy-Item "..\bin\libpsl-5.dll" $BundleDir -Force
Copy-Item "..\bin\libidn2-0.dll" $BundleDir -Force
Copy-Item "..\bin\libbrotlidec.dll" $BundleDir -Force

# Copy proxy engines
Copy-Item "..\bin\xray.exe" $BundleDir -Force
Copy-Item "..\bin\sing-box.exe" $BundleDir -Force
Copy-Item "..\bin\mihomo-windows-amd64-compatible.exe" $BundleDir -Force
Copy-Item "..\bin\tor.exe" $BundleDir -Force

# Copy configuration files
Write-Host "Step 2: Adding configuration files..." -ForegroundColor Yellow
Copy-Item "..\config\All_Configs_Sub.txt" $BundleDir -Force
Copy-Item "..\config\all_extracted_configs.txt" $BundleDir -Force
Copy-Item "..\config\sub.txt" $BundleDir -Force

# Create runtime directory
New-Item -ItemType Directory -Path "$BundleDir\runtime" | Out-Null

# Create documentation
Write-Host "Step 3: Creating documentation..." -ForegroundColor Yellow
$ReadmeContent = @"
# Hunter Dashboard - Portable Version

## Version: $Version
## Build Date: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')

## Quick Start

1. **Double-click `hunter_dashboard.exe`** to launch the application
2. The dashboard will automatically load thousands of pre-configured proxy settings
3. No installation required - everything is self-contained

## What's Included

### Core Applications
- **hunter_dashboard.exe** - Modern Flutter UI dashboard
- **hunter_cli.exe** - C++ backend proxy testing engine

### Proxy Engines
- **xray.exe** - Primary proxy engine (XRay core)
- **sing-box.exe** - Universal proxy platform
- **mihomo-windows-amd64-compatible.exe** - Clash Meta fork
- **tor.exe** - Tor network integration

### Configuration Files
- **All_Configs_Sub.txt** (2.8 MB) - 1000+ proxy configurations
- **all_extracted_configs.txt** (7.0 MB) - Additional configurations
- **sub.txt** (9.6 KB) - Subscription-based configurations

### Dependencies
- All required DLL files included (OpenSSL, curl, zlib, etc.)
- No external dependencies needed
- Visual C++ Redistributable may be required on some systems

## Features

### Dashboard Capabilities
- Real-time proxy testing and validation
- Automatic configuration categorization (Gold/Silver)
- Live activity monitoring with animations
- Responsive design for all screen sizes
- Built-in documentation browser

### Advanced Features
- Multi-engine proxy testing
- Telegram connectivity verification
- Speed testing with intelligent fallback
- DNS routing through proxy tunnel
- DPI evasion techniques

## Directory Structure

```
Hunter Dashboard/
├── hunter_dashboard.exe          # Main application
├── hunter_cli.exe                # CLI backend
├── xray.exe / sing-box.exe        # Proxy engines
├── All_Configs_Sub.txt            # Configuration file 1
├── all_extracted_configs.txt     # Configuration file 2
├── sub.txt                        # Configuration file 3
├── runtime/                       # Runtime data (auto-created)
└── *.dll                          # Required libraries
```

## System Requirements

- Windows 10/11 (x64)
- 4GB RAM minimum (8GB recommended)
- 500MB free disk space
- Internet connection for proxy testing

## Troubleshooting

### Application Won't Start
1. Install Visual C++ Redistributable: https://aka.ms/vs/17/release/vc_redist.x64.exe
2. Run as Administrator
3. Check Windows Event Viewer for errors
4. Temporarily disable antivirus software

### Proxy Testing Issues
1. Verify internet connectivity
2. Check firewall settings
3. Ensure proxy engines aren't blocked
4. Try different network connections

### Performance Tips
1. Close unnecessary applications
2. Use SSD for better I/O performance
3. Ensure sufficient RAM is available
4. Update graphics drivers for UI smoothness

## Security & Privacy

- **No data collection**: All processing is local
- **Anonymous usage**: No telemetry or tracking
- **Local storage**: Configurations stored locally only
- **Secure connections**: All testing done through proxy tunnels

## Getting Help

- **Documentation**: Available in-app under "Docs" section
- **GitHub**: https://github.com/bahmany/censorship_hunter
- **Issues**: Report bugs on GitHub Issues page

## License

This software is provided "as-is" for educational and research purposes.
The user is responsible for compliance with local laws and regulations.

---

**Important**: This tool is designed for research and educational purposes only.
Users must comply with their local laws and regulations regarding proxy usage.
"@

$ReadmeContent | Out-File -FilePath "$BundleDir\README.txt" -Encoding UTF8

# Create launcher script
$LauncherContent = @"
@echo off
title Hunter Dashboard
echo Starting Hunter Dashboard...
echo.

REM Check if Visual C++ Redistributable is installed
reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{DA5E371C-6333-3D8A-A1CE-1C3B849911EF}" >nul 2>&1
if errorlevel 1 (
    echo Visual C++ Redistributable may not be installed.
    echo If the application fails to start, please install:
    echo https://aka.ms/vs/17/release/vc_redist.x64.exe
    echo.
)

REM Create runtime directory if not exists
if not exist "runtime" mkdir runtime

REM Start the dashboard
hunter_dashboard.exe

if errorlevel 1 (
    echo.
    echo Application failed to start. Possible solutions:
    echo 1. Install Visual C++ Redistributable
    echo 2. Run as Administrator
    echo 3. Check Windows Event Viewer for errors
    echo 4. Temporarily disable antivirus software
    echo.
    pause
)
"@

$LauncherContent | Out-File -FilePath "$BundleDir\Start_Hunter_Dashboard.bat" -Encoding ASCII

# Create license file
$LicenseContent = @"
Hunter Anti-Censorship Dashboard
Version $Version

Copyright (C) 2026 Hunter Project

LICENSE TERMS

This software is provided "as-is" for educational and research purposes only.

DISCLAIMER:
- The user is solely responsible for compliance with local laws and regulations
- The developers assume no liability for misuse or illegal activities
- This software is intended for legitimate research and circumvention of censorship

PERMISSIONS:
- Free to use, modify, and distribute for educational purposes
- Commercial use requires explicit permission from the developers
- Attribution to the Hunter Project is appreciated

LIMITATIONS:
- No warranty is provided, express or implied
- The software may contain bugs or vulnerabilities
- Use at your own risk

SUPPORT:
- GitHub: https://github.com/bahmany/censorship_hunter
- Documentation: Available in the application

For more information about the legal aspects of using circumvention tools,
please consult local laws and regulations in your jurisdiction.
"@

$LicenseContent | Out-File -FilePath "$BundleDir\LICENSE.txt" -Encoding UTF8

Write-Host "Step 4: Creating ZIP archive..." -ForegroundColor Yellow

# Create ZIP file
$ZipPath = "$OutputPath\hunter_dashboard_v$Version.zip"
if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

# Use PowerShell's Compress-Archive
Compress-Archive -Path "$BundleDir\*" -DestinationPath $ZipPath -Force

# Calculate file size
$FileInfo = Get-Item $ZipPath
$SizeInMB = [math]::Round($FileInfo.Length / 1MB, 2)

Write-Host "Step 5: Creating self-extracting executable (optional)..." -ForegroundColor Yellow

# Try to create self-extracting EXE using IExpress (built into Windows)
$IExpressConfig = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=1
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSize=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=%TargetName%
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles

[Strings]
InstallPrompt=Do you want to install Hunter Dashboard?
DisplayLicense=LICENSE.txt
FinishMessage=Hunter Dashboard has been extracted successfully. Run hunter_dashboard.exe to start the application.
TargetName=hunter_dashboard_v$Version.exe
FriendlyName=Hunter Dashboard v$Version
AppLaunched=cmd /c echo Hunter Dashboard extracted successfully. Start hunter_dashboard.exe to run the application.
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
FILE0="README.txt"
FILE1="LICENSE.txt"
FILE2="Start_Hunter_Dashboard.bat"
FILE3="hunter_dashboard.exe"
FILE4="hunter_cli.exe"
FILE5="xray.exe"
FILE6="sing-box.exe"
FILE7="mihomo-windows-amd64-compatible.exe"
FILE8="tor.exe"
FILE9="All_Configs_Sub.txt"
FILE10="all_extracted_configs.txt"
FILE11="sub.txt"
FILE12="libcrypto-3-x64.dll"
FILE13="libssl-3-x64.dll"
FILE14="libcurl-4.dll"
FILE15="libzstd.dll"
FILE16="zlib1.dll"
FILE17="libssh2-1.dll"
FILE18="libnghttp2-14.dll"
FILE19="libnghttp3-9.dll"
FILE20="libpsl-5.dll"
FILE21="libidn2-0.dll"
FILE22="libbrotlidec.dll"

[SourceFiles]
SourceFiles0=$BundleDir\
[SourceFiles0]
%FILE0%=
%FILE1%=
%FILE2%=
%FILE3%=
%FILE4%=
%FILE5%=
%FILE6%=
%FILE7%=
%FILE8%=
%FILE9%=
%FILE10%=
%FILE11%=
%FILE12%=
%FILE13%=
%FILE14%=
%FILE15%=
%FILE16%=
%FILE17%=
%FILE18%=
%FILE19%=
%FILE20%=
%FILE21%=
%FILE22%=
"@

$IExpressConfig | Out-File -FilePath "$BundleDir\hunter_setup.sed" -Encoding ASCII

# Create self-extracting EXE using IExpress
try {
    $IExpressPath = "$env:WINDIR\System32\iexpress.exe"
    if (Test-Path $IExpressPath) {
        & $IExpressPath /N /Q "$BundleDir\hunter_setup.sed"
        if (Test-Path "$BundleDir\hunter_dashboard_v$Version.exe") {
            Move-Item "$BundleDir\hunter_dashboard_v$Version.exe" $OutputPath -Force
            Write-Host "Self-extracting EXE created successfully!" -ForegroundColor Green
        }
    }
} catch {
    Write-Host "Self-extracting EXE creation failed: $($_.Exception.Message)" -ForegroundColor Yellow
}

# Cleanup
Remove-Item -Recurse -Force $BundleDir

# Display results
Write-Host "`n" + "="*60 -ForegroundColor Cyan
Write-Host "HUNTER DASHBOARD BUNDLE CREATION COMPLETED" -ForegroundColor Cyan
Write-Host "="*60 -ForegroundColor Cyan
Write-Host "Version: $Version" -ForegroundColor White
Write-Host "Created: $(Get-Date)" -ForegroundColor White
Write-Host "`nFiles created:" -ForegroundColor Green

if (Test-Path $ZipPath) {
    $ZipInfo = Get-Item $ZipPath
    Write-Host "• ZIP Archive: $($ZipInfo.FullName)" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($ZipInfo.Length / 1MB, 2)) MB" -ForegroundColor Gray
}

$ExePath = "$OutputPath\hunter_dashboard_v$Version.exe"
if (Test-Path $ExePath) {
    $ExeInfo = Get-Item $ExePath
    Write-Host "• Self-Extracting EXE: $($ExeInfo.FullName)" -ForegroundColor White
    Write-Host "  Size: $([math]::Round($ExeInfo.Length / 1MB, 2)) MB" -ForegroundColor Gray
}

Write-Host "`nBundle includes:" -ForegroundColor Yellow
Write-Host "• Flutter Dashboard Application" -ForegroundColor Gray
Write-Host "• C++ CLI with all dependencies" -ForegroundColor Gray
Write-Host "• Proxy Engines (XRay, sing-box, mihomo, Tor)" -ForegroundColor Gray
Write-Host "• Pre-loaded configurations (10MB+)" -ForegroundColor Gray
Write-Host "• Documentation and launcher scripts" -ForegroundColor Gray
Write-Host "`nInstallation:" -ForegroundColor Yellow
Write-Host "1. Extract ZIP or run self-extracting EXE" -ForegroundColor Gray
Write-Host "2. Double-click hunter_dashboard.exe" -ForegroundColor Gray
Write-Host "3. No installation required - fully portable" -ForegroundColor Gray
Write-Host "`n" + "="*60 -ForegroundColor Cyan
