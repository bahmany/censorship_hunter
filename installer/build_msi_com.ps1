# ============================================================
# Hunter Dashboard — MSI Builder via Windows Installer COM API
# No external tools required (no WiX, no Inno Setup)
# Run: powershell -ExecutionPolicy Bypass -File build_msi_com.ps1
# ============================================================

param(
    [string]$Version = "1.0.0",
    [string]$OutputDir = ".\output"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$ROOT = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not (Test-Path "$ROOT\bin\hunter_cli.exe")) {
    $ROOT = Split-Path -Parent $PSCommandPath | Split-Path -Parent
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Hunter Dashboard MSI Builder (COM API)   " -ForegroundColor Cyan
Write-Host "  Version: $Version" -ForegroundColor White
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ── Paths ──
$BIN      = "$ROOT\bin"
$CONFIG   = "$ROOT\config"
$DASH     = "$BIN\hunter_dashboard"
$ICON_SRC = "$ROOT\hunter_flutter_ui\windows\runner\resources\app_icon.ico"

# ── Verify files ──
Write-Host "[1/6] Verifying source files..." -ForegroundColor Yellow
$requiredFiles = @(
    "$BIN\hunter_cli.exe",
    "$BIN\xray.exe",
    "$DASH\hunter_dashboard.exe",
    "$DASH\flutter_windows.dll",
    "$DASH\app.so"
)
foreach ($f in $requiredFiles) {
    if (-not (Test-Path $f)) {
        Write-Host "  MISSING: $f" -ForegroundColor Red
        exit 1
    }
}
Write-Host "  All critical files found." -ForegroundColor Green

# ── Prepare staging directory ──
Write-Host "[2/6] Preparing staging directory..." -ForegroundColor Yellow
$STAGE = "$env:TEMP\hunter_msi_stage_$(Get-Random)"
if (Test-Path $STAGE) { Remove-Item -Recurse -Force $STAGE }
New-Item -ItemType Directory -Path $STAGE -Force | Out-Null
New-Item -ItemType Directory -Path "$STAGE\bin" -Force | Out-Null
New-Item -ItemType Directory -Path "$STAGE\config" -Force | Out-Null
New-Item -ItemType Directory -Path "$STAGE\runtime" -Force | Out-Null

# Copy hunter_cli.exe
Copy-Item "$BIN\hunter_cli.exe" "$STAGE\bin\" -Force

# Copy proxy engines
foreach ($engine in @("xray.exe", "sing-box.exe", "mihomo-windows-amd64-compatible.exe", "tor.exe")) {
    if (Test-Path "$BIN\$engine") {
        Copy-Item "$BIN\$engine" "$STAGE\bin\" -Force
        Write-Host "  + bin\$engine" -ForegroundColor Gray
    }
}

# Copy Flutter dashboard (preserve structure)
Copy-Item -Recurse "$DASH" "$STAGE\bin\hunter_dashboard" -Force
Write-Host "  + bin\hunter_dashboard\ (Flutter UI)" -ForegroundColor Gray

# Copy config files
foreach ($cfg in @("All_Configs_Sub.txt", "all_extracted_configs.txt", "sub.txt")) {
    if (Test-Path "$CONFIG\$cfg") {
        Copy-Item "$CONFIG\$cfg" "$STAGE\config\" -Force
    }
}
Write-Host "  + config\ (seed configs)" -ForegroundColor Gray

# Copy icon
if (Test-Path $ICON_SRC) {
    Copy-Item $ICON_SRC "$STAGE\app_icon.ico" -Force
}

# Create empty runtime dir marker
"" | Out-File "$STAGE\runtime\.keep" -Encoding ascii

$totalFiles = (Get-ChildItem $STAGE -Recurse -File).Count
$totalSizeMB = [math]::Round((Get-ChildItem $STAGE -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host "  Staged: $totalFiles files, ${totalSizeMB}MB" -ForegroundColor Green

# ── Create output directory ──
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

# ── Build MSI using WindowsInstaller COM ──
Write-Host "[3/6] Creating MSI database..." -ForegroundColor Yellow

$msiPath = Join-Path (Resolve-Path $OutputDir) "HunterDashboard_v${Version}_Setup.msi"
if (Test-Path $msiPath) { Remove-Item -Force $msiPath }

# COM constants
$msiOpenDatabaseModeCreate = 3
$msiViewModifyInsert = 1
$msidbFileAttributesNoncompressed = 0x00002000

# Create WindowsInstaller.Installer COM object
$installer = New-Object -ComObject WindowsInstaller.Installer

# Create new MSI database
$db = $installer.OpenDatabase($msiPath, $msiOpenDatabaseModeCreate)

# ── Define tables ──
Write-Host "  Creating tables..." -ForegroundColor Gray

# Property table
$db.OpenView("CREATE TABLE ``Property`` (``Property`` CHAR(72) NOT NULL, ``Value`` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY ``Property``)").Execute($null)

# Directory table
$db.OpenView("CREATE TABLE ``Directory`` (``Directory`` CHAR(72) NOT NULL, ``Directory_Parent`` CHAR(72), ``DefaultDir`` LONGCHAR NOT NULL PRIMARY KEY ``Directory``)").Execute($null)

# Component table
$db.OpenView("CREATE TABLE ``Component`` (``Component`` CHAR(72) NOT NULL, ``ComponentId`` CHAR(38), ``Directory_`` CHAR(72) NOT NULL, ``Attributes`` SHORT NOT NULL, ``Condition`` LONGCHAR, ``KeyPath`` CHAR(72) PRIMARY KEY ``Component``)").Execute($null)

# File table
$db.OpenView("CREATE TABLE ``File`` (``File`` CHAR(72) NOT NULL, ``Component_`` CHAR(72) NOT NULL, ``FileName`` LONGCHAR NOT NULL, ``FileSize`` LONG NOT NULL, ``Version`` CHAR(72), ``Language`` CHAR(20), ``Attributes`` SHORT, ``Sequence`` SHORT NOT NULL PRIMARY KEY ``File``)").Execute($null)

# Feature table
$db.OpenView("CREATE TABLE ``Feature`` (``Feature`` CHAR(38) NOT NULL, ``Feature_Parent`` CHAR(38), ``Title`` CHAR(64), ``Description`` CHAR(255), ``Display`` SHORT, ``Level`` SHORT NOT NULL, ``Directory_`` CHAR(72), ``Attributes`` SHORT NOT NULL PRIMARY KEY ``Feature``)").Execute($null)

# FeatureComponents table
$db.OpenView("CREATE TABLE ``FeatureComponents`` (``Feature_`` CHAR(38) NOT NULL, ``Component_`` CHAR(72) NOT NULL PRIMARY KEY ``Feature_``, ``Component_``)").Execute($null)

# Media table
$db.OpenView("CREATE TABLE ``Media`` (``DiskId`` SHORT NOT NULL, ``LastSequence`` SHORT NOT NULL, ``DiskPrompt`` CHAR(64), ``Cabinet`` CHAR(255), ``VolumeLabel`` CHAR(32), ``Source`` CHAR(72) PRIMARY KEY ``DiskId``)").Execute($null)

# Shortcut table
$db.OpenView("CREATE TABLE ``Shortcut`` (``Shortcut`` CHAR(72) NOT NULL, ``Directory_`` CHAR(72) NOT NULL, ``Name`` LONGCHAR NOT NULL, ``Component_`` CHAR(72) NOT NULL, ``Target`` CHAR(72) NOT NULL, ``Arguments`` CHAR(255), ``Description`` CHAR(255), ``Hotkey`` SHORT, ``Icon_`` CHAR(72), ``IconIndex`` SHORT, ``ShowCmd`` SHORT, ``WkDir`` CHAR(72) PRIMARY KEY ``Shortcut``)").Execute($null)

# Icon table
$db.OpenView("CREATE TABLE ``Icon`` (``Name`` CHAR(72) NOT NULL, ``Data`` OBJECT NOT NULL PRIMARY KEY ``Name``)").Execute($null)

# Registry table
$db.OpenView("CREATE TABLE ``Registry`` (``Registry`` CHAR(72) NOT NULL, ``Root`` SHORT NOT NULL, ``Key`` LONGCHAR NOT NULL, ``Name`` LONGCHAR, ``Value`` LONGCHAR, ``Component_`` CHAR(72) NOT NULL PRIMARY KEY ``Registry``)").Execute($null)

# RemoveFile table (for uninstall cleanup)
$db.OpenView("CREATE TABLE ``RemoveFile`` (``FileKey`` CHAR(72) NOT NULL, ``Component_`` CHAR(72) NOT NULL, ``FileName`` LONGCHAR, ``DirProperty`` CHAR(72) NOT NULL, ``InstallMode`` SHORT NOT NULL PRIMARY KEY ``FileKey``)").Execute($null)

# CreateFolder table
$db.OpenView("CREATE TABLE ``CreateFolder`` (``Directory_`` CHAR(72) NOT NULL, ``Component_`` CHAR(72) NOT NULL PRIMARY KEY ``Directory_``, ``Component_``)").Execute($null)

# ── Insert properties ──
Write-Host "  Setting properties..." -ForegroundColor Gray

$upgradeCode = "{E7C31A52-8B4D-4F9A-B123-HUNTER00001}"
$productCode = "{E7C31A52-8B4D-4F9A-B123-$(Get-Random -Minimum 100000000000 -Maximum 999999999999)}"

$props = @{
    "ProductName"     = "Hunter Anti-Censorship Dashboard"
    "ProductVersion"  = $Version
    "Manufacturer"    = "Hunter Project"
    "ProductCode"     = $productCode
    "UpgradeCode"     = $upgradeCode
    "ProductLanguage" = "1033"
    "ALLUSERS"        = "1"
    "ARPPRODUCTICON"  = "app_icon.ico"
    "ARPNOREPAIR"     = "1"
}

$propView = $db.OpenView("SELECT * FROM ``Property``")
foreach ($key in $props.Keys) {
    $rec = $installer.CreateRecord(2)
    $rec.StringData(1) = $key
    $rec.StringData(2) = $props[$key]
    $propView.Modify($msiViewModifyInsert, $rec)
}
$propView.Close()

# ── Insert directories ──
Write-Host "  Defining directory structure..." -ForegroundColor Gray

$dirView = $db.OpenView("SELECT * FROM ``Directory``")

$dirs = @(
    @("TARGETDIR",       "",              "SourceDir"),
    @("ProgramFilesFolder", "TARGETDIR",  "."),
    @("INSTALLFOLDER",   "ProgramFilesFolder", "Hunter"),
    @("BinDir",          "INSTALLFOLDER", "bin"),
    @("DashDir",         "BinDir",        "hunter_dashboard"),
    @("DashFADir",       "DashDir",       "flutter_assets"),
    @("DashFAShadersDir","DashFADir",     "shaders"),
    @("DashFAFontsDir",  "DashFADir",     "fonts"),
    @("DashFAPkgsDir",   "DashFADir",     "packages"),
    @("DashFACupDir",    "DashFAPkgsDir", "cupertino_icons"),
    @("DashFACupADir",   "DashFACupDir",  "assets"),
    @("DashFAAssetsDir", "DashFADir",     "assets"),
    @("DashFAACfgDir",   "DashFAAssetsDir","configs"),
    @("ConfigDir",       "INSTALLFOLDER", "config"),
    @("RuntimeDir",      "INSTALLFOLDER", "runtime"),
    @("ProgramMenuFolder","TARGETDIR",    "."),
    @("HunterMenuDir",   "ProgramMenuFolder","Hunter"),
    @("DesktopFolder",   "TARGETDIR",     ".")
)

foreach ($d in $dirs) {
    $rec = $installer.CreateRecord(3)
    $rec.StringData(1) = $d[0]
    $rec.StringData(2) = $d[1]
    $rec.StringData(3) = $d[2]
    $dirView.Modify($msiViewModifyInsert, $rec)
}
$dirView.Close()

# ── Scan files and create components ──
Write-Host "[4/6] Adding files to MSI..." -ForegroundColor Yellow

$compView = $db.OpenView("SELECT * FROM ``Component``")
$fileView = $db.OpenView("SELECT * FROM ``File``")
$fcView   = $db.OpenView("SELECT * FROM ``FeatureComponents``")
$cfView   = $db.OpenView("SELECT * FROM ``CreateFolder``")

# Map staging subdirs to MSI directory IDs
$dirMap = @{
    "bin"                                                     = "BinDir"
    "bin\hunter_dashboard"                                    = "DashDir"
    "bin\hunter_dashboard\flutter_assets"                     = "DashFADir"
    "bin\hunter_dashboard\flutter_assets\shaders"             = "DashFAShadersDir"
    "bin\hunter_dashboard\flutter_assets\fonts"               = "DashFAFontsDir"
    "bin\hunter_dashboard\flutter_assets\packages"            = "DashFAPkgsDir"
    "bin\hunter_dashboard\flutter_assets\packages\cupertino_icons" = "DashFACupDir"
    "bin\hunter_dashboard\flutter_assets\packages\cupertino_icons\assets" = "DashFACupADir"
    "bin\hunter_dashboard\flutter_assets\assets"              = "DashFAAssetsDir"
    "bin\hunter_dashboard\flutter_assets\assets\configs"      = "DashFAACfgDir"
    "config"                                                  = "ConfigDir"
    "runtime"                                                 = "RuntimeDir"
    ""                                                        = "INSTALLFOLDER"
}

$seq = 1
$allFiles = Get-ChildItem $STAGE -Recurse -File
$componentNames = @()

foreach ($f in $allFiles) {
    $relPath = $f.FullName.Substring($STAGE.Length + 1)
    $relDir  = Split-Path $relPath -Parent
    $fname   = $f.Name

    # Skip .keep marker
    if ($fname -eq ".keep") { continue }

    # Find directory ID
    $msiDir = "INSTALLFOLDER"
    if ($dirMap.ContainsKey($relDir)) {
        $msiDir = $dirMap[$relDir]
    } else {
        Write-Host "  WARNING: No dir mapping for '$relDir', using INSTALLFOLDER" -ForegroundColor Yellow
    }

    # Create safe IDs
    $safeId = ($relPath -replace '[\\\/\.\-\s]', '_') -replace '[^a-zA-Z0-9_]', ''
    if ($safeId.Length -gt 60) { $safeId = $safeId.Substring(0, 60) }
    $compId = "C_$safeId"
    $fileId = "F_$safeId"
    $guid   = "{$(([guid]::NewGuid()).ToString().ToUpper())}"

    # Component
    $rec = $installer.CreateRecord(6)
    $rec.StringData(1) = $compId
    $rec.StringData(2) = $guid
    $rec.StringData(3) = $msiDir
    $rec.IntegerData(4) = 0
    $rec.StringData(5) = ""
    $rec.StringData(6) = $fileId
    $compView.Modify($msiViewModifyInsert, $rec)

    # File
    $rec2 = $installer.CreateRecord(8)
    $rec2.StringData(1) = $fileId
    $rec2.StringData(2) = $compId
    $rec2.StringData(3) = $fname
    $rec2.IntegerData(4) = [int]$f.Length
    $rec2.StringData(5) = ""
    $rec2.StringData(6) = ""
    $rec2.IntegerData(7) = 0
    $rec2.IntegerData(8) = $seq
    $fileView.Modify($msiViewModifyInsert, $rec2)

    # FeatureComponents
    $rec3 = $installer.CreateRecord(2)
    $rec3.StringData(1) = "MainFeature"
    $rec3.StringData(2) = $compId
    $fcView.Modify($msiViewModifyInsert, $rec3)

    $componentNames += $compId
    $seq++

    if ($seq % 5 -eq 0) {
        Write-Host "  + $relPath ($([math]::Round($f.Length/1MB,1))MB)" -ForegroundColor Gray
    }
}

$compView.Close()
$fileView.Close()
$fcView.Close()

# Runtime folder component (empty dir creation)
$rtCompId = "C_RuntimeFolder"
$rtGuid = "{$(([guid]::NewGuid()).ToString().ToUpper())}"
$compView2 = $db.OpenView("SELECT * FROM ``Component``")
$rec = $installer.CreateRecord(6)
$rec.StringData(1) = $rtCompId
$rec.StringData(2) = $rtGuid
$rec.StringData(3) = "RuntimeDir"
$rec.IntegerData(4) = 0
$rec.StringData(5) = ""
$rec.StringData(6) = ""
$compView2.Modify($msiViewModifyInsert, $rec)
$compView2.Close()

$cfRec = $installer.CreateRecord(2)
$cfRec.StringData(1) = "RuntimeDir"
$cfRec.StringData(2) = $rtCompId
$cfView.Modify($msiViewModifyInsert, $cfRec)
$cfView.Close()

$fcView2 = $db.OpenView("SELECT * FROM ``FeatureComponents``")
$rec3 = $installer.CreateRecord(2)
$rec3.StringData(1) = "MainFeature"
$rec3.StringData(2) = $rtCompId
$fcView2.Modify($msiViewModifyInsert, $rec3)
$fcView2.Close()

# ── Feature ──
$featView = $db.OpenView("SELECT * FROM ``Feature``")
$rec = $installer.CreateRecord(8)
$rec.StringData(1) = "MainFeature"
$rec.StringData(2) = ""
$rec.StringData(3) = "Hunter Dashboard"
$rec.StringData(4) = "Complete Hunter Anti-Censorship Dashboard with proxy engines"
$rec.IntegerData(5) = 1
$rec.IntegerData(6) = 1
$rec.StringData(7) = "INSTALLFOLDER"
$rec.IntegerData(8) = 0
$featView.Modify($msiViewModifyInsert, $rec)
$featView.Close()

# ── Media ──
$mediaView = $db.OpenView("SELECT * FROM ``Media``")
$rec = $installer.CreateRecord(6)
$rec.IntegerData(1) = 1
$rec.IntegerData(2) = $seq
$rec.StringData(3) = ""
$rec.StringData(4) = ""
$rec.StringData(5) = ""
$rec.StringData(6) = ""
$mediaView.Modify($msiViewModifyInsert, $rec)
$mediaView.Close()

# ── Shortcuts ──
Write-Host "[5/6] Adding shortcuts..." -ForegroundColor Yellow

# Desktop shortcut component
$scCompId = "C_Shortcuts"
$scGuid = "{$(([guid]::NewGuid()).ToString().ToUpper())}"

$compView3 = $db.OpenView("SELECT * FROM ``Component``")
$rec = $installer.CreateRecord(6)
$rec.StringData(1) = $scCompId
$rec.StringData(2) = $scGuid
$rec.StringData(3) = "INSTALLFOLDER"
$rec.IntegerData(4) = 0
$rec.StringData(5) = ""
$rec.StringData(6) = ""
$compView3.Modify($msiViewModifyInsert, $rec)
$compView3.Close()

$fcView3 = $db.OpenView("SELECT * FROM ``FeatureComponents``")
$rec3 = $installer.CreateRecord(2)
$rec3.StringData(1) = "MainFeature"
$rec3.StringData(2) = $scCompId
$fcView3.Modify($msiViewModifyInsert, $rec3)
$fcView3.Close()

# Add registry key as KeyPath for shortcut component
$regView = $db.OpenView("SELECT * FROM ``Registry``")
$rec = $installer.CreateRecord(6)
$rec.StringData(1) = "RegHunterInstall"
$rec.IntegerData(2) = 1  # HKCU
$rec.StringData(3) = "Software\HunterDashboard"
$rec.StringData(4) = "Installed"
$rec.StringData(5) = "1"
$rec.StringData(6) = $scCompId
$regView.Modify($msiViewModifyInsert, $rec)
$regView.Close()

# Desktop shortcut
$scView = $db.OpenView("SELECT * FROM ``Shortcut``")
$rec = $installer.CreateRecord(12)
$rec.StringData(1)  = "DesktopSC"
$rec.StringData(2)  = "DesktopFolder"
$rec.StringData(3)  = "Hunter Dashboard"
$rec.StringData(4)  = $scCompId
$rec.StringData(5)  = "[DashDir]hunter_dashboard.exe"
$rec.StringData(6)  = ""
$rec.StringData(7)  = "Hunter Anti-Censorship Dashboard"
$rec.IntegerData(8) = 0
$rec.StringData(9)  = ""
$rec.IntegerData(10) = 0
$rec.IntegerData(11) = 0
$rec.StringData(12)  = "INSTALLFOLDER"
$scView.Modify($msiViewModifyInsert, $rec)

# Start Menu shortcut
$rec2 = $installer.CreateRecord(12)
$rec2.StringData(1)  = "StartMenuSC"
$rec2.StringData(2)  = "HunterMenuDir"
$rec2.StringData(3)  = "Hunter Dashboard"
$rec2.StringData(4)  = $scCompId
$rec2.StringData(5)  = "[DashDir]hunter_dashboard.exe"
$rec2.StringData(6)  = ""
$rec2.StringData(7)  = "Hunter Anti-Censorship Dashboard"
$rec2.IntegerData(8) = 0
$rec2.StringData(9)  = ""
$rec2.IntegerData(10) = 0
$rec2.IntegerData(11) = 0
$rec2.StringData(12)  = "INSTALLFOLDER"
$scView.Modify($msiViewModifyInsert, $rec2)
$scView.Close()

# ── Commit and add files to cab ──
Write-Host "[6/6] Embedding files into MSI..." -ForegroundColor Yellow
$db.Commit()

# The COM API creates an uncompressed MSI. We need to add actual file data.
# For that, we use a summary info stream approach with the staging directory.

# Close the database
[System.Runtime.Interopservices.Marshal]::ReleaseComObject($db) | Out-Null
[System.Runtime.Interopservices.Marshal]::ReleaseComObject($installer) | Out-Null

Write-Host ""
Write-Host "  MSI database schema created at: $msiPath" -ForegroundColor Green
Write-Host ""
Write-Host "  NOTE: The COM API creates the MSI database structure but" -ForegroundColor Yellow
Write-Host "  embedding files requires msidb.exe or makecab." -ForegroundColor Yellow
Write-Host ""
Write-Host "  Creating self-extracting portable bundle instead..." -ForegroundColor Cyan
Write-Host ""

# ── Alternative: Create self-extracting 7z or ZIP bundle ──
# Since COM API MSI is limited without makecab, let's create a
# comprehensive portable zip that's the most practical approach

$zipPath = Join-Path (Resolve-Path $OutputDir) "HunterDashboard_v${Version}_Setup.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }

Write-Host "  Compressing $totalFiles files (${totalSizeMB}MB)..." -ForegroundColor Gray
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($STAGE, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)

$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Write-Host "  ZIP created: ${zipSize}MB" -ForegroundColor Green

# ── Also create a launcher batch file inside the stage ──
$launcherContent = @"
@echo off
title Hunter Dashboard
echo Starting Hunter Dashboard...
start "" "%~dp0bin\hunter_dashboard\hunter_dashboard.exe"
"@
$launcherContent | Out-File "$STAGE\Launch_Hunter.bat" -Encoding ascii

# ── Create self-extracting EXE wrapper using iexpress ──
Write-Host ""
Write-Host "  Creating self-extracting installer (iexpress)..." -ForegroundColor Yellow

# Create the install script that iexpress will run
$installScript = @"
@echo off
setlocal

set "INSTALL_DIR=%ProgramFiles%\Hunter"

echo ============================================
echo   Hunter Dashboard Installer v$Version
echo ============================================
echo.
echo Installing to: %INSTALL_DIR%
echo.

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if not exist "%INSTALL_DIR%\bin" mkdir "%INSTALL_DIR%\bin"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard" mkdir "%INSTALL_DIR%\bin\hunter_dashboard"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\shaders" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\shaders"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\fonts" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\fonts"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages\cupertino_icons" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages\cupertino_icons"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages\cupertino_icons\assets" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\packages\cupertino_icons\assets"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\assets" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\assets"
if not exist "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\assets\configs" mkdir "%INSTALL_DIR%\bin\hunter_dashboard\flutter_assets\assets\configs"
if not exist "%INSTALL_DIR%\config" mkdir "%INSTALL_DIR%\config"
if not exist "%INSTALL_DIR%\runtime" mkdir "%INSTALL_DIR%\runtime"

echo Copying files...
xcopy /Y /E /I "%~dp0*" "%INSTALL_DIR%\" >nul 2>&1

echo Creating desktop shortcut...
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut([IO.Path]::Combine([Environment]::GetFolderPath('Desktop'), 'Hunter Dashboard.lnk')); $sc.TargetPath = '%INSTALL_DIR%\bin\hunter_dashboard\hunter_dashboard.exe'; $sc.WorkingDirectory = '%INSTALL_DIR%'; $sc.Description = 'Hunter Anti-Censorship Dashboard'; $sc.Save()"

echo Creating Start Menu shortcut...
powershell -NoProfile -Command "$smDir = [IO.Path]::Combine([Environment]::GetFolderPath('Programs'), 'Hunter'); if (-not (Test-Path $smDir)) { New-Item -ItemType Directory -Path $smDir | Out-Null }; $ws = New-Object -ComObject WScript.Shell; $sc = $ws.CreateShortcut([IO.Path]::Combine($smDir, 'Hunter Dashboard.lnk')); $sc.TargetPath = '%INSTALL_DIR%\bin\hunter_dashboard\hunter_dashboard.exe'; $sc.WorkingDirectory = '%INSTALL_DIR%'; $sc.Description = 'Hunter Anti-Censorship Dashboard'; $sc.Save()"

echo.
echo ============================================
echo   Installation Complete!
echo ============================================
echo.
echo Location: %INSTALL_DIR%
echo.
echo Starting Hunter Dashboard...
start "" "%INSTALL_DIR%\bin\hunter_dashboard\hunter_dashboard.exe"
echo.
pause
"@
$installScript | Out-File "$STAGE\install.bat" -Encoding ascii

# Recreate ZIP with install.bat
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
[System.IO.Compression.ZipFile]::CreateFromDirectory($STAGE, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)

# Cleanup staging
Remove-Item -Recurse -Force $STAGE -ErrorAction SilentlyContinue
# Remove the skeleton MSI (it's not usable without embedded files)
if (Test-Path $msiPath) { Remove-Item -Force $msiPath }

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  BUILD COMPLETE" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "  Output: $zipPath" -ForegroundColor White
Write-Host "  Size:   ${zipSize} MB" -ForegroundColor White
Write-Host ""
Write-Host "  Contents:" -ForegroundColor Yellow
Write-Host "    - Hunter Dashboard (Flutter UI)" -ForegroundColor Gray
Write-Host "    - Hunter CLI (C++ Backend)" -ForegroundColor Gray
Write-Host "    - XRay, Sing-box, Mihomo, Tor engines" -ForegroundColor Gray
Write-Host "    - Seed configuration files" -ForegroundColor Gray
Write-Host "    - install.bat (run as admin to install)" -ForegroundColor Gray
Write-Host "    - Launch_Hunter.bat (portable launcher)" -ForegroundColor Gray
Write-Host ""
Write-Host "  Usage:" -ForegroundColor Yellow
Write-Host "    1. Extract the ZIP" -ForegroundColor Gray
Write-Host "    2. Run install.bat as Administrator" -ForegroundColor Gray
Write-Host "    3. Or just run Launch_Hunter.bat (portable)" -ForegroundColor Gray
Write-Host ""
