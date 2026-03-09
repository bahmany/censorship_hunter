# Hunter Dashboard MSI Installer Creator
# Creates a Windows MSI installer using WiX Toolset
# Run with: .\create_msi_installer.ps1 [OutputPath] [Version]

param(
    [string]$OutputPath = ".\output",
    [string]$Version = "1.0.0.0"
)

# Error handling: stop on errors
$ErrorActionPreference = "Stop"

Write-Host "=" * 60 -ForegroundColor Cyan
Write-Host "Hunter Dashboard MSI Installer Creator" -ForegroundColor Cyan
Write-Host "=" * 60 -ForegroundColor Cyan
Write-Host "Version: $Version" -ForegroundColor White
Write-Host "Output: $OutputPath" -ForegroundColor White
Write-Host ""

# Step 1: Check prerequisites
Write-Host "Step 1: Checking prerequisites..." -ForegroundColor Yellow

# Check WiX Toolset
$wixPaths = @(
    "${env:ProgramFiles}\WiX Toolset v3.11\bin\candle.exe",
    "${env:ProgramFiles(x86)}\WiX Toolset v3.11\bin\candle.exe",
    "${env:WIX}\bin\candle.exe"
)

$wixFound = $false
$wixBinPath = $null

foreach ($path in $wixPaths) {
    if (Test-Path $path) {
        $wixFound = $true
        $wixBinPath = Split-Path $path
        Write-Host "  Found WiX Toolset at: $wixBinPath" -ForegroundColor Green
        break
    }
}

if (-not $wixFound) {
    Write-Host "ERROR: WiX Toolset not found!" -ForegroundColor Red
    Write-Host "Please install WiX Toolset from: https://wixtoolset.org/releases/" -ForegroundColor Yellow
    exit 1
}

# Add WiX to PATH for this session
$env:PATH = "$wixBinPath;$env:PATH"

# Step 2: Verify source files exist
Write-Host "Step 2: Verifying source files..." -ForegroundColor Yellow

$requiredFiles = @(
    @("..\hunter_flutter_ui\build\windows\x64\runner\Release\hunter_dashboard.exe", "Flutter Dashboard"),
    @("..\bin\hunter_cli.exe", "CLI Executable"),
    @("..\bin\libcrypto-3-x64.dll", "OpenSSL Crypto"),
    @("..\bin\libssl-3-x64.dll", "OpenSSL SSL"),
    @("..\bin\libcurl-4.dll", "cURL Library"),
    @("..\bin\xray.exe", "XRay Proxy Engine"),
    @("..\bin\sing-box.exe", "Sing-box Proxy Engine"),
    @("..\config\All_Configs_Sub.txt", "Configuration File 1"),
    @("..\config\all_extracted_configs.txt", "Configuration File 2"),
    @("..\config\sub.txt", "Configuration File 3")
)

$missingFiles = @()
foreach ($fileInfo in $requiredFiles) {
    $filePath = $fileInfo[0]
    $description = $fileInfo[1]
    if (-not (Test-Path $filePath)) {
        $missingFiles += "$description ($filePath)"
        Write-Host "  MISSING: $description" -ForegroundColor Red
    } else {
        Write-Host "  OK: $description" -ForegroundColor Green
    }
}

if ($missingFiles.Count -gt 0) {
    Write-Host ""
    Write-Host "ERROR: Missing required files:" -ForegroundColor Red
    foreach ($mf in $missingFiles) {
        Write-Host "  - $mf" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "Please build the Flutter app and ensure all binaries are in place." -ForegroundColor Yellow
    exit 1
}

# Step 3: Create output directories
Write-Host "Step 3: Creating output directories..." -ForegroundColor Yellow

if (-not (Test-Path $OutputPath)) {
    New-Item -ItemType Directory -Path $OutputPath -Force | Out-Null
    Write-Host "  Created: $OutputPath" -ForegroundColor Green
}

$wixObjPath = ".\wixobj"
if (-not (Test-Path $wixObjPath)) {
    New-Item -ItemType Directory -Path $wixObjPath -Force | Out-Null
    Write-Host "  Created: $wixObjPath" -ForegroundColor Green
}

# Step 4: Download Visual C++ Redistributable
Write-Host "Step 4: Checking Visual C++ Redistributable..." -ForegroundColor Yellow

$vcRedistPath = "vc_redist.x64.exe"
if (-not (Test-Path $vcRedistPath)) {
    Write-Host "  Downloading VC++ Redistributable..." -ForegroundColor Gray
    try {
        $vcUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe"
        Invoke-WebRequest -Uri $vcUrl -OutFile $vcRedistPath -UseBasicParsing
        Write-Host "  Downloaded: $vcRedistPath" -ForegroundColor Green
    } catch {
        Write-Host "  WARNING: Failed to download VC++ Redistributable" -ForegroundColor Yellow
        Write-Host "    $_" -ForegroundColor Gray
    }
} else {
    Write-Host "  Found: $vcRedistPath" -ForegroundColor Green
}

# Step 5: Prepare icon file
Write-Host "Step 5: Preparing icon file..." -ForegroundColor Yellow

$iconSource = "..\hunter_flutter_ui\windows\runner\resources\app_icon.ico"
$iconDest = "hunter_icon.ico"

if (Test-Path $iconSource) {
    Copy-Item $iconSource $iconDest -Force
    Write-Host "  Copied icon file" -ForegroundColor Green
} else {
    Write-Host "  WARNING: Icon not found, will use default" -ForegroundColor Yellow
}

# Step 6: Create license RTF file
Write-Host "Step 6: Creating license file..." -ForegroundColor Yellow

$licenseRtf = @"{\rtf1\ansi\deff0 {\fonttbl {\f0\fnil\fcharset0 Calibri;}}
{\colortbl ;\red0\green0\blue0;}
\f0\fs24 Hunter Anti-Censorship Dashboard\par
\par
Copyright (C) 2026 Hunter Project\par
\par
This software is provided "as-is" for educational and research purposes.\par
\par
The user is responsible for compliance with local laws and regulations.\par
\par
For more information, visit: https://github.com/bahmany/censorship_hunter\par
}"@

$licenseRtf | Out-File -FilePath "license.rtf" -Encoding ascii -NoNewline
Write-Host "  Created: license.rtf" -ForegroundColor Green

# Step 7: Create banner images if they don't exist
Write-Host "Step 7: Preparing banner images..." -ForegroundColor Yellow

if (-not (Test-Path "banner.bmp")) {
    Write-Host "  WARNING: banner.bmp not found (optional)" -ForegroundColor Yellow
}
if (-not (Test-Path "dialog.bmp")) {
    Write-Host "  WARNING: dialog.bmp not found (optional)" -ForegroundColor Yellow
}

# Step 8: Update version in WiX file
Write-Host "Step 8: Updating version in WiX source..." -ForegroundColor Yellow

$wixFile = "hunter_installer.wxs"
if (Test-Path $wixFile) {
    $wixContent = Get-Content $wixFile -Raw
    # Update version attribute
    $wixContent = $wixContent -replace 'Version="[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"', "Version=`"$Version`""
    $wixContent | Out-File $wixFile -Encoding utf8
    Write-Host "  Updated version to $Version" -ForegroundColor Green
} else {
    Write-Host "  ERROR: $wixFile not found!" -ForegroundColor Red
    exit 1
}

# Step 9: Compile WiX source
Write-Host "Step 9: Compiling WiX source..." -ForegroundColor Yellow

$wixObjOutput = "$wixObjPath\hunter_installer.wixobj"

try {
    $candleOutput = & candle.exe -out "$wixObjPath\" $wixFile 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: WiX compilation error" -ForegroundColor Red
        foreach ($line in $candleOutput) {
            Write-Host "    $line" -ForegroundColor Red
        }
        exit 1
    }
    Write-Host "  Compiled successfully" -ForegroundColor Green
} catch {
    Write-Host "  FAILED: $_" -ForegroundColor Red
    exit 1
}

# Step 10: Link MSI
Write-Host "Step 10: Creating MSI package..." -ForegroundColor Yellow

$msiOutput = "$OutputPath\hunter_dashboard_v$Version.msi"

try {
    $lightOutput = & light.exe -ext WixUIExtension -ext WixUtilExtension `
        -out $msiOutput `
        "$wixObjPath\hunter_installer.wixobj" 2>&1
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAILED: WiX linking error" -ForegroundColor Red
        foreach ($line in $lightOutput) {
            Write-Host "    $line" -ForegroundColor Red
        }
        exit 1
    }
    Write-Host "  MSI created successfully" -ForegroundColor Green
} catch {
    Write-Host "  FAILED: $_" -ForegroundColor Red
    exit 1
}

# Step 11: Calculate file size and verify
Write-Host "Step 11: Verifying output..." -ForegroundColor Yellow

if (Test-Path $msiOutput) {
    $msiInfo = Get-Item $msiOutput
    $sizeMB = [math]::Round($msiInfo.Length / 1MB, 2)
    Write-Host "  Output: $($msiInfo.FullName)" -ForegroundColor Green
    Write-Host "  Size: $sizeMB MB" -ForegroundColor White
} else {
    Write-Host "  ERROR: MSI file not created!" -ForegroundColor Red
    exit 1
}

# Step 12: Cleanup temporary files (optional)
Write-Host "Step 12: Cleaning up temporary files..." -ForegroundColor Yellow
if (Test-Path $wixObjPath) {
    Remove-Item -Recurse -Force $wixObjPath
    Write-Host "  Removed: $wixObjPath" -ForegroundColor Gray
}

# Summary
Write-Host ""
Write-Host "=" * 60 -ForegroundColor Cyan
Write-Host "MSI INSTALLER CREATION COMPLETED" -ForegroundColor Cyan
Write-Host "=" * 60 -ForegroundColor Cyan
Write-Host ""
Write-Host "Installer Details:" -ForegroundColor Yellow
Write-Host "  File: $msiOutput" -ForegroundColor White
Write-Host "  Version: $Version" -ForegroundColor White
Write-Host "  Size: $sizeMB MB" -ForegroundColor White
Write-Host ""
Write-Host "Included Components:" -ForegroundColor Yellow
Write-Host "  - Hunter Dashboard (Flutter UI)" -ForegroundColor Gray
Write-Host "  - Hunter CLI (C++ Backend)" -ForegroundColor Gray
Write-Host "  - Proxy Engines (XRay, sing-box, mihomo, Tor)" -ForegroundColor Gray
Write-Host "  - Configuration Files (10MB+ pre-loaded)" -ForegroundColor Gray
Write-Host "  - All Dependencies (OpenSSL, cURL, etc.)" -ForegroundColor Gray
Write-Host "  - Visual C++ Redistributable installer" -ForegroundColor Gray
Write-Host ""
Write-Host "Installation Features:" -ForegroundColor Yellow
Write-Host "  - Desktop shortcut" -ForegroundColor Gray
Write-Host "  - Start Menu entry" -ForegroundColor Gray
Write-Host "  - Program Files installation" -ForegroundColor Gray
Write-Host "  - Automatic VC++ Redist check" -ForegroundColor Gray
Write-Host "  - Uninstall support" -ForegroundColor Gray
Write-Host ""
Write-Host "Usage:" -ForegroundColor Yellow
Write-Host "  Double-click the MSI file to install" -ForegroundColor Gray
Write-Host "  Or run: msiexec /i `"$msiOutput`"" -ForegroundColor Gray
Write-Host ""
Write-Host "=" * 60 -ForegroundColor Cyan
