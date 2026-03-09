@echo off
echo Building Hunter MSI Installer...

REM Check if WiX Toolset is installed
where heat >nul 2>nul
if errorlevel 1 (
    echo WiX Toolset not found. Please install WiX Toolset.
    echo Download from: https://wixtoolset.org/releases/
    pause
    exit /b 1
)

REM Create necessary directories
if not exist "output" mkdir output
if not exist "wixobj" mkdir wixobj

REM Download Visual C++ Redistributable if not exists
if not exist "vc_redist.x64.exe" (
    echo Downloading Visual C++ Redistributable...
    powershell -Command "Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile 'vc_redist.x64.exe'"
)

REM Copy icon files if they exist
if exist "..\hunter_flutter_ui\windows\runner\resources\app_icon.ico" (
    copy "..\hunter_flutter_ui\windows\runner\resources\app_icon.ico" "hunter_icon.ico"
) else (
    echo Warning: Icon file not found. Using default placeholder.
)

REM Create license RTF
echo Creating license file...
echo {\rtf1\ansi\deff0 {\fonttbl {\f0\fnil\fcharset0 Calibri;}} > license.rtf
echo {\colortbl ;\red0\green0\blue0;} >> license.rtf
echo \f0\fs24 Hunter Anti-Censorship Dashboard\par >> license.rtf
echo \par >> license.rtf
echo Copyright (C) 2026 Hunter Project\par >> license.rtf
echo \par >> license.rtf
echo This software is provided "as-is" for educational and research purposes.\par >> license.rtf
echo \par >> license.rtf
echo The user is responsible for compliance with local laws and regulations.\par >> license.rtf
echo \par >> license.rtf
echo For more information, visit: https://github.com/bahmany/censorship_hunter\par >> license.rtf
echo } >> license.rtf

REM Compile WiX source
echo Compiling WiX source...
candle -out wixobj\ hunter_installer.wxs
if errorlevel 1 (
    echo Compilation failed!
    pause
    exit /b 1
)

REM Link to create MSI
echo Linking to create MSI...
light -out output\hunter_dashboard.msi wixobj\hunter_installer.wobj
if errorlevel 1 (
    echo Linking failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo Hunter MSI Installer created successfully!
echo Location: output\hunter_dashboard.msi
echo ========================================
echo.
echo Installer includes:
echo - Flutter Dashboard Application
echo - C++ CLI with all dependencies
echo - Proxy Engines (XRay, sing-box, mihomo, Tor)
echo - Pre-loaded configuration files
echo - Visual C++ Redistributable
echo.
pause
