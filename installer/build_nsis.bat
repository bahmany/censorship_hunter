@echo off
echo Building Hunter Setup with NSIS...

REM Check if NSIS is installed
where makensis >nul 2>nul
if errorlevel 1 (
    echo NSIS not found. Please install NSIS (Nullsoft Scriptable Install System).
    echo Download from: https://nsis.sourceforge.io/Download
    pause
    exit /b 1
)

REM Create license file
echo Creating license file...
echo Hunter Anti-Censorship Dashboard > license.txt
echo Copyright (C) 2026 Hunter Project >> license.txt
echo. >> license.txt
echo This software is provided "as-is" for educational and research purposes. >> license.txt
echo The user is responsible for compliance with local laws and regulations. >> license.txt
echo. >> license.txt
echo For more information, visit: https://github.com/bahmany/censorship_hunter >> license.txt

REM Copy icon if available
if exist "..\hunter_flutter_ui\windows\runner\resources\app_icon.ico" (
    copy "..\hunter_flutter_ui\windows\runner\resources\app_icon.ico" "hunter_icon.ico"
) else (
    echo Warning: Icon file not found. Using default.
)

REM Create output directory
if not exist "output" mkdir output

REM Build the installer
echo Building NSIS installer...
makensis hunter_setup.nsi
if errorlevel 1 (
    echo Build failed!
    pause
    exit /b 1
)

REM Move to output folder
if exist "hunter_dashboard_setup.exe" (
    move "hunter_dashboard_setup.exe" "output\"
    echo.
    echo ========================================
    echo Hunter Setup created successfully!
    echo Location: output\hunter_dashboard_setup.exe
    echo File size:
    dir "output\hunter_dashboard_setup.exe" | findstr "hunter_dashboard_setup.exe"
    echo ========================================
    echo.
    echo Installer includes:
    echo - Flutter Dashboard Application
    echo - C++ CLI with all dependencies  
    echo - Proxy Engines ^(XRay, sing-box, mihomo, Tor^)
    echo - Pre-loaded configuration files ^(10MB+^)
    echo - Visual C++ Redistributable ^(auto-download^)
    echo - Desktop and Start Menu shortcuts
    echo.
) else (
    echo Installer file not found!
    pause
    exit /b 1
)

pause
