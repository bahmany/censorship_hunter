# ============================================================================
# Hunter Dashboard — Full Release Build & Package Script
# Creates a self-contained portable ZIP and optional NSIS installer
# ============================================================================
param(
    [string]$Version = "1.0.0",
    [string]$OutputDir = "$PSScriptRoot\output",
    [switch]$SkipCppBuild,
    [switch]$SkipFlutterBuild,
    [switch]$NsisOnly
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path "$PSScriptRoot\.."
$CppDir = "$Root\hunter_cpp"
$FlutterDir = "$Root\hunter_flutter_ui"
$BinDir = "$Root\bin"
$FlutterRelease = "$FlutterDir\build\windows\x64\runner\Release"
$StageDir = "$OutputDir\_stage\Hunter"

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Hunter Dashboard Release Builder v$Version" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# ── Step 0: Validate prerequisites ──
Write-Host "[0/6] Validating prerequisites..." -ForegroundColor Yellow

$msysCmake = "C:\msys64\ucrt64\bin\cmake.exe"
if (!(Test-Path $msysCmake)) {
    throw "MSYS2 cmake not found at $msysCmake. Install with: pacman -S mingw-w64-ucrt-x86_64-cmake"
}

$flutterExe = Get-Command flutter -ErrorAction SilentlyContinue
if (!$flutterExe) {
    throw "Flutter not found in PATH. Install Flutter SDK first."
}

# Check required proxy engine binaries
$requiredBins = @(
    "$BinDir\xray.exe",
    "$BinDir\sing-box.exe",
    "$BinDir\mihomo-windows-amd64-compatible.exe"
)
foreach ($bin in $requiredBins) {
    if (!(Test-Path $bin)) {
        throw "Required binary not found: $bin"
    }
}

Write-Host "  All prerequisites OK" -ForegroundColor Green

# ── Step 1: C++ Release Build ──
if (!$SkipCppBuild) {
    Write-Host "`n[1/6] Building C++ backend (static release)..." -ForegroundColor Yellow

    # Kill any running hunter_cli to avoid file lock
    Stop-Process -Name hunter_cli -Force -ErrorAction SilentlyContinue
    Stop-Process -Name hunter_ui -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    $buildDir = "$CppDir\build"
    if (!(Test-Path $buildDir)) {
        New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
        & $msysCmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S $CppDir -B $buildDir
    }
    & $msysCmake --build $buildDir --target hunter_cli hunter_ui -j1
    if ($LASTEXITCODE -ne 0) {
        # Retry through MSYS2 shell for better compatibility
        Write-Host "  Retrying build through MSYS2 shell..." -ForegroundColor Yellow
        $msysBuild = "$buildDir" -replace '\\','/' -replace '^([A-Z]):','/$1'.ToLower()
        & C:\msys64\msys2_shell.cmd -defterm -no-start -ucrt64 -c "cd $msysBuild && ninja hunter_cli hunter_ui 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "C++ build failed" }
    }

    if (!(Test-Path "$buildDir\hunter_cli.exe")) { throw "hunter_cli.exe not found after build" }
    if (!(Test-Path "$buildDir\hunter_ui.exe")) { throw "hunter_ui.exe not found after build" }
    Write-Host "  C++ build complete" -ForegroundColor Green
} else {
    Write-Host "`n[1/6] Skipping C++ build (--SkipCppBuild)" -ForegroundColor DarkGray
}

# ── Step 2: Flutter Release Build ──
if (!$SkipFlutterBuild) {
    Write-Host "`n[2/6] Building Flutter dashboard (windows release)..." -ForegroundColor Yellow
    Push-Location $FlutterDir
    try {
        & flutter build windows --release 2>&1 | ForEach-Object { Write-Host "  $_" }
        if ($LASTEXITCODE -ne 0) { throw "Flutter build failed" }
    } finally {
        Pop-Location
    }
    if (!(Test-Path "$FlutterRelease\hunter_dashboard.exe")) {
        throw "hunter_dashboard.exe not found after Flutter build"
    }
    Write-Host "  Flutter build complete" -ForegroundColor Green
} else {
    Write-Host "`n[2/6] Skipping Flutter build (--SkipFlutterBuild)" -ForegroundColor DarkGray
}

# ── Step 3: Prepare staging directory ──
Write-Host "`n[3/6] Staging release files..." -ForegroundColor Yellow

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Path $StageDir -Force | Out-Null
New-Item -ItemType Directory -Path "$StageDir\bin" -Force | Out-Null
New-Item -ItemType Directory -Path "$StageDir\runtime" -Force | Out-Null
New-Item -ItemType Directory -Path "$StageDir\config\import" -Force | Out-Null
New-Item -ItemType Directory -Path "$StageDir\data" -Force | Out-Null

# 3a. Flutter dashboard (top-level)
Write-Host "  Copying Flutter dashboard..." -ForegroundColor DarkGray
Copy-Item "$FlutterRelease\hunter_dashboard.exe" "$StageDir\" -Force
Copy-Item "$FlutterRelease\flutter_windows.dll" "$StageDir\" -Force

# Flutter plugin DLLs
Get-ChildItem "$FlutterRelease\*.dll" | ForEach-Object {
    Copy-Item $_.FullName "$StageDir\" -Force
}

# Flutter data directory (app.so, icudtl.dat, flutter_assets/)
Copy-Item "$FlutterRelease\data\*" "$StageDir\data\" -Recurse -Force

# App icon
if (Test-Path "$FlutterRelease\app_icon.ico") {
    Copy-Item "$FlutterRelease\app_icon.ico" "$StageDir\" -Force
} elseif (Test-Path "$FlutterDir\windows\runner\resources\app_icon.ico") {
    Copy-Item "$FlutterDir\windows\runner\resources\app_icon.ico" "$StageDir\" -Force
}

# 3b. C++ backend binaries → bin/
Write-Host "  Copying C++ backend..." -ForegroundColor DarkGray
Copy-Item "$CppDir\build\hunter_cli.exe" "$StageDir\bin\" -Force
Copy-Item "$CppDir\build\hunter_ui.exe" "$StageDir\bin\" -Force

# 3c. Proxy engines → bin/
Write-Host "  Copying proxy engines..." -ForegroundColor DarkGray
$engines = @("xray.exe", "sing-box.exe", "mihomo-windows-amd64-compatible.exe")
foreach ($engine in $engines) {
    if (Test-Path "$BinDir\$engine") {
        Copy-Item "$BinDir\$engine" "$StageDir\bin\" -Force
    }
}
# tor.exe is optional
if (Test-Path "$BinDir\tor.exe") {
    Copy-Item "$BinDir\tor.exe" "$StageDir\bin\" -Force
}

# 3d. Default config
Write-Host "  Creating default configuration..." -ForegroundColor DarkGray
$defaultConfig = @{
    multiproxy_port = 10808
    gemini_port = 10809
    max_total = 1000
    max_workers = 12
    scan_limit = 50
    sleep_seconds = 300
    telegram_limit = 50
    xray_path = "xray.exe"
    state_file = "runtime/hunter_state.json"
    gold_file = "runtime/HUNTER_gold.txt"
    silver_file = "runtime/HUNTER_silver.txt"
}
$defaultConfig | ConvertTo-Json -Depth 2 | Out-File -FilePath "$StageDir\runtime\hunter_config.json" -Encoding UTF8

# 3e. Launcher BAT (fallback for users who don't use the Flutter UI)
$launcherContent = @"
@echo off
title Hunter Dashboard
echo ============================================
echo   Hunter Anti-Censorship Dashboard v$Version
echo ============================================
echo.

:: Create runtime directory
if not exist "runtime" mkdir runtime
if not exist "config\import" mkdir config\import

:: Start the Flutter dashboard
if exist "hunter_dashboard.exe" (
    echo Starting Hunter Dashboard...
    start "" "hunter_dashboard.exe"
) else (
    echo [ERROR] hunter_dashboard.exe not found!
    echo Please ensure all files are extracted correctly.
    pause
)
"@
$launcherContent | Out-File -FilePath "$StageDir\Start_Hunter.bat" -Encoding ASCII

# 3f. README
$readmeContent = @"
# Hunter Anti-Censorship Dashboard v$Version

## Quick Start
Double-click **hunter_dashboard.exe** (or **Start_Hunter.bat**)

## Directory Structure
```
Hunter/
├── hunter_dashboard.exe      ← Main GUI application (start this)
├── Start_Hunter.bat          ← Alternative launcher
├── data/                     ← Flutter runtime (DO NOT DELETE)
├── bin/                      ← Backend engines
│   ├── hunter_cli.exe        ← C++ backend (started by dashboard)
│   ├── hunter_ui.exe         ← Win32 native UI (alternative)
│   ├── xray.exe              ← XRay proxy engine
│   ├── sing-box.exe          ← Sing-box proxy engine
│   └── mihomo-*.exe          ← Mihomo (Clash Meta) engine
├── runtime/                  ← Runtime data (auto-managed)
│   └── hunter_config.json    ← Configuration file
└── config/import/            ← Drop custom configs here
```

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
"@
[System.IO.File]::WriteAllText("$StageDir\README.md", $readmeContent, [System.Text.UTF8Encoding]::new($false))

# 3g. LICENSE
if (Test-Path "$Root\LICENSE") {
    Copy-Item "$Root\LICENSE" "$StageDir\LICENSE.txt" -Force
}

# Count staged files
$fileCount = (Get-ChildItem -Recurse -File $StageDir).Count
$totalSize = [math]::Round((Get-ChildItem -Recurse -File $StageDir | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host "  Staged $fileCount files ($totalSize MB)" -ForegroundColor Green

# ── Step 4: Create portable ZIP ──
Write-Host "`n[4/6] Creating portable ZIP archive..." -ForegroundColor Yellow

if (!(Test-Path $OutputDir)) { New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null }
$zipPath = "$OutputDir\Hunter_v${Version}_portable.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

# Use .NET compression for better results
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    "$OutputDir\_stage",
    $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $true  # includeBaseDirectory = true → zip contains "Hunter/" folder
)

$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "  Created: $zipPath ($zipSize MB)" -ForegroundColor Green

# ── Step 5: Create NSIS installer (if NSIS is available) ──
Write-Host "`n[5/6] Creating NSIS installer..." -ForegroundColor Yellow

$nsisExe = $null
$nsisLocations = @(
    "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
    "${env:ProgramFiles}\NSIS\makensis.exe",
    "C:\Program Files (x86)\NSIS\makensis.exe",
    "C:\Program Files\NSIS\makensis.exe"
)
foreach ($loc in $nsisLocations) {
    if (Test-Path $loc) { $nsisExe = $loc; break }
}

if ($nsisExe) {
    # Generate the NSIS script with correct paths
    $nsiScript = "$OutputDir\hunter_setup.nsi"
    $nsiContent = @"
; Hunter Dashboard Installer — Auto-generated
; Version: $Version

!include "MUI2.nsh"
!include "FileFunc.nsh"

!define APPNAME "Hunter Dashboard"
!define VERSION "$Version"
!define PUBLISHER "Hunter Project"
!define WEBSITE "https://github.com/bahmany/censorship_hunter"

Name "`${APPNAME} v`${VERSION}"
OutFile "Hunter_v${Version}_setup.exe"
InstallDir "`$PROGRAMFILES64\Hunter"
InstallDirRegKey HKLM "Software\Hunter" "InstallPath"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

; UI
!define MUI_ABORTWARNING
!define MUI_ICON "$($StageDir -replace '\\','\\')\app_icon.ico"

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "`$INSTDIR\hunter_dashboard.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch Hunter Dashboard"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; Version info
VIProductVersion "${Version}.0"
VIAddVersionKey "ProductName" "`${APPNAME}"
VIAddVersionKey "FileVersion" "${Version}.0"
VIAddVersionKey "CompanyName" "`${PUBLISHER}"

Section "Install"
    SectionIn RO
    SetOutPath "`$INSTDIR"

    ; Flutter dashboard (top-level)
    File "$StageDir\hunter_dashboard.exe"
    File "$StageDir\flutter_windows.dll"
    File /nonfatal "$StageDir\screen_retriever_windows_plugin.dll"
    File /nonfatal "$StageDir\system_tray_plugin.dll"
    File /nonfatal "$StageDir\window_manager_plugin.dll"
    File /nonfatal "$StageDir\app_icon.ico"
    File "$StageDir\Start_Hunter.bat"
    File "$StageDir\README.md"
    File /nonfatal "$StageDir\LICENSE.txt"

    ; Flutter data directory
    SetOutPath "`$INSTDIR\data"
    File "$StageDir\data\app.so"
    File "$StageDir\data\icudtl.dat"
    SetOutPath "`$INSTDIR\data\flutter_assets"
    File /r "$StageDir\data\flutter_assets\*.*"

    ; Backend binaries
    SetOutPath "`$INSTDIR\bin"
    File "$StageDir\bin\hunter_cli.exe"
    File "$StageDir\bin\hunter_ui.exe"
    File "$StageDir\bin\xray.exe"
    File "$StageDir\bin\sing-box.exe"
    File "$StageDir\bin\mihomo-windows-amd64-compatible.exe"
    File /nonfatal "$StageDir\bin\tor.exe"

    ; Runtime directory
    SetOutPath "`$INSTDIR\runtime"
    File "$StageDir\runtime\hunter_config.json"

    ; Config import directory
    CreateDirectory "`$INSTDIR\config\import"

    ; Uninstaller
    WriteUninstaller "`$INSTDIR\Uninstall.exe"

    ; Registry
    WriteRegStr HKLM "Software\Hunter" "InstallPath" "`$INSTDIR"
    WriteRegStr HKLM "Software\Hunter" "Version" "`${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "DisplayName" "`${APPNAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "UninstallString" "`$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "DisplayVersion" "`${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "Publisher" "`${PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "URLInfoAbout" "`${WEBSITE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "DisplayIcon" "`$INSTDIR\app_icon.ico"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "NoRepair" 1

    ; Estimate size
    `${GetSize} "`$INSTDIR" "/S=0K" `$0 `$1 `$2
    IntFmt `$0 "0x%08X" `$0
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter" \
        "EstimatedSize" `$0

    ; Start Menu
    CreateDirectory "`$SMPROGRAMS\Hunter"
    CreateShortCut "`$SMPROGRAMS\Hunter\Hunter Dashboard.lnk" "`$INSTDIR\hunter_dashboard.exe" \
        "" "`$INSTDIR\app_icon.ico"
    CreateShortCut "`$SMPROGRAMS\Hunter\Uninstall Hunter.lnk" "`$INSTDIR\Uninstall.exe"

    ; Desktop shortcut
    CreateShortCut "`$DESKTOP\Hunter Dashboard.lnk" "`$INSTDIR\hunter_dashboard.exe" \
        "" "`$INSTDIR\app_icon.ico"

SectionEnd

Section "Uninstall"
    ; Kill running processes
    nsExec::ExecToLog 'taskkill /F /IM hunter_dashboard.exe'
    nsExec::ExecToLog 'taskkill /F /IM hunter_cli.exe'
    nsExec::ExecToLog 'taskkill /F /IM hunter_ui.exe'
    Sleep 1000

    ; Remove files
    RMDir /r "`$INSTDIR\data"
    RMDir /r "`$INSTDIR\bin"
    RMDir /r "`$INSTDIR\runtime"
    RMDir /r "`$INSTDIR\config"
    Delete "`$INSTDIR\hunter_dashboard.exe"
    Delete "`$INSTDIR\flutter_windows.dll"
    Delete "`$INSTDIR\*.dll"
    Delete "`$INSTDIR\*.ico"
    Delete "`$INSTDIR\*.bat"
    Delete "`$INSTDIR\*.md"
    Delete "`$INSTDIR\*.txt"
    Delete "`$INSTDIR\Uninstall.exe"
    RMDir "`$INSTDIR"

    ; Shortcuts
    Delete "`$DESKTOP\Hunter Dashboard.lnk"
    RMDir /r "`$SMPROGRAMS\Hunter"

    ; Registry
    DeleteRegKey HKLM "Software\Hunter"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hunter"
SectionEnd

Function .onInit
    ; Check for existing installation
    ReadRegStr `$0 HKLM "Software\Hunter" "InstallPath"
    StrCmp `$0 "" done
    MessageBox MB_OKCANCEL|MB_ICONEXCLAMATION \
        "Hunter is already installed at `$0.$\n$\nClick OK to upgrade or Cancel to abort." \
        IDOK done
    Abort
    done:
FunctionEnd
"@
    [System.IO.File]::WriteAllText($nsiScript, $nsiContent, [System.Text.UTF8Encoding]::new($false))

    Write-Host "  Running NSIS compiler..." -ForegroundColor DarkGray
    & $nsisExe /V2 $nsiScript
    if ($LASTEXITCODE -eq 0) {
        $setupPath = "$OutputDir\Hunter_v${Version}_setup.exe"
        $setupSize = [math]::Round((Get-Item $setupPath).Length / 1MB, 1)
        Write-Host "  Created: $setupPath ($setupSize MB)" -ForegroundColor Green
    } else {
        Write-Host "  NSIS build failed (exit code $LASTEXITCODE)" -ForegroundColor Red
        Write-Host "  The portable ZIP is still available." -ForegroundColor Yellow
    }
} else {
    Write-Host "  NSIS not found. Skipping installer creation." -ForegroundColor Yellow
    Write-Host "  To create an installer, install NSIS from https://nsis.sourceforge.io" -ForegroundColor DarkGray
    Write-Host "  The portable ZIP is ready for distribution." -ForegroundColor Yellow
}

# ── Step 6: Summary ──
Write-Host "`n[6/6] Cleanup..." -ForegroundColor Yellow
# Keep staging dir for debugging; clean up on next run
# Remove-Item -Recurse -Force "$OutputDir\_stage"

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  BUILD COMPLETE" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Version:  $Version" -ForegroundColor White
Write-Host "  Date:     $(Get-Date -Format 'yyyy-MM-dd HH:mm')" -ForegroundColor White
Write-Host ""
Write-Host "  Output files:" -ForegroundColor Yellow

if (Test-Path "$OutputDir\Hunter_v${Version}_portable.zip") {
    $z = Get-Item "$OutputDir\Hunter_v${Version}_portable.zip"
    Write-Host "    Portable ZIP: $($z.FullName) ($([math]::Round($z.Length/1MB,1)) MB)" -ForegroundColor White
}
if (Test-Path "$OutputDir\Hunter_v${Version}_setup.exe") {
    $s = Get-Item "$OutputDir\Hunter_v${Version}_setup.exe"
    Write-Host "    Installer:    $($s.FullName) ($([math]::Round($s.Length/1MB,1)) MB)" -ForegroundColor White
}

Write-Host ""
Write-Host "  Staged directory (for inspection): $StageDir" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  To distribute:" -ForegroundColor Yellow
Write-Host "    - Portable: Users extract the ZIP and run hunter_dashboard.exe" -ForegroundColor Gray
Write-Host "    - Installer: Users run the setup EXE (admin required)" -ForegroundColor Gray
Write-Host ""
