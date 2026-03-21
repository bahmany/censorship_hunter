@echo off
setlocal enabledelayedexpansion

echo ========================================
echo Hunter DPI Bypass Setup v1.3.0
echo ========================================
echo.

:: Check if running as administrator
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Please run this setup as Administrator
    echo Right-click setup.bat and select "Run as administrator"
    pause
    exit /b 1
)

echo [INFO] Running with administrator privileges
echo.

:: Create required directories
echo [INFO] Creating directories...
if not exist "bin" mkdir bin
if not exist "runtime" mkdir runtime
if not exist "runtime\xray_tmp" mkdir runtime\xray_tmp
if not exist "config" mkdir config
if not exist "config\import" mkdir config\import
if not exist "logs" mkdir logs
if not exist "cache" mkdir cache

:: Download XRay binary if not exists
echo [INFO] Checking XRay binary...
if not exist "bin\xray.exe" (
    echo [INFO] Downloading XRay binary...
    powershell -Command "Invoke-WebRequest -Uri 'https://github.com/XTLS/Xray-core/releases/download/v1.8.6/Xray-windows-64.zip' -OutFile 'xray.zip'"
    if exist "xray.zip" (
        powershell -Command "Expand-Archive -Path 'xray.zip' -DestinationPath 'bin' -Force"
        del xray.zip
        if exist "bin\xray.exe" (
            echo [OK] XRay binary downloaded successfully
        ) else (
            echo [ERROR] Failed to extract XRay binary
            pause
            exit /b 1
        )
    ) else (
        echo [ERROR] Failed to download XRay binary
        pause
        exit /b 1
    )
) else (
    echo [OK] XRay binary already exists
)

:: Download geosite data if not exists
echo [INFO] Checking geosite data...
if not exist "bin\geosite.dat" (
    echo [INFO] Downloading geosite data...
    powershell -Command "Invoke-WebRequest -Uri 'https://github.com/v2fly/domain-list-community/releases/download/20240329122116/geosite.dat' -OutFile 'bin\geosite.dat'"
    if exist "bin\geosite.dat" (
        echo [OK] Geosite data downloaded successfully
    ) else (
        echo [ERROR] Failed to download geosite data
        pause
        exit /b 1
    )
) else (
    echo [OK] Geosite data already exists
)

:: Check if huntercensor.exe exists
echo [INFO] Checking Hunter binary...
if exist "hunter_cpp\build\huntercensor.exe" (
    copy "hunter_cpp\build\huntercensor.exe" "bin\huntercensor.exe" >nul
    echo [OK] Hunter binary copied to bin directory
) else if exist "bin\huntercensor.exe" (
    echo [OK] Hunter binary already exists
) else (
    echo [ERROR] Hunter binary not found
    echo Please build the project first:
    echo   cd hunter_cpp
    echo   mkdir build && cd build
    echo   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
    echo   ninja
    pause
    exit /b 1
)

:: Create configuration files
echo [INFO] Creating configuration files...

:: Create .env file if not exists
if not exist ".env" (
    echo # Hunter Configuration > .env
    echo # DPI Bypass Settings >> .env
    echo DPI_BYPASS_ENABLED=true >> .env
    echo DPI_TLS_FRAGMENT=true >> .env
    echo DPI_FRAGMENT_SIZE=100 >> .env
    echo DPI_FRAGMENT_DELAY=10 >> .env
    echo DPI_TTL_TRICK=false >> .env
    echo DPI_TTL_VALUE=8 >> .env
    echo. >> .env
    echo # Network Settings >> .env
    echo DISCOVERY_THREADS=9 >> .env
    echo VALIDATION_TIMEOUT=2000 >> .env
    echo BALANCER_PORT=10808 >> .env
    echo. >> .env
    echo # Logging >> .env
    echo LOG_LEVEL=info >> .env
    echo LOG_FILE=logs\hunter.log >> .env
    echo [OK] Created .env configuration file
) else (
    echo [OK] .env file already exists
)

:: Create sample import file
if not exist "config\import\sample.txt" (
    echo # Sample proxy configurations for import >> config\import\sample.txt
    echo vmess://eyJ2IjogIjIiLCAicHMiOiAidGVzdCIsICJhZWRzIjogIjEyNy4wLjAuMSIsICJwb3J0IjogNDQzLCAiaWQiOiAiMTIzNDU2NzgtYWJjZC1lZmZoLTEyMzQtNDU2Nzg5MGFiY2QiLCAiYWlkIjogMCwgInNjeSI6ICJhdXRvIiwgInRscyI6ICJ0bHMiLCAidHlwZSI6ICJub25lIiwgImhvc3QiOiAidGVzdC5leGFtcGxlLmNvbSIsICJwYXRoIjogIi8ifQ== >> config\import\sample.txt
    echo trojan://password@example.com:443?sni=example.com&type=tcp >> config\import\sample.txt
    echo ss://YWVzLTI1Ni1nY206cGFzc3dvcmQ@server.example.com:8388 >> config\import\sample.txt
    echo [OK] Created sample import file
) else (
    echo [OK] Sample import file already exists
)

:: Set Windows Firewall rules
echo [INFO] Configuring Windows Firewall...
netsh advfirewall firewall delete rule name="Hunter DPI Bypass" >nul 2>&1
netsh advfirewall firewall add rule name="Hunter DPI Bypass" dir=in action=allow program="%~dp0bin\huntercensor.exe" enable=yes >nul
netsh advfirewall firewall add rule name="Hunter DPI Bypass" dir=out action=allow program="%~dp0bin\huntercensor.exe" enable=yes >nul
if %errorlevel% equ 0 (
    echo [OK] Firewall rules configured
) else (
    echo [WARN] Failed to configure firewall rules (may require manual setup)
)

:: Create desktop shortcut
echo [INFO] Creating desktop shortcut...
powershell -Command "$WshShell = New-Object -comObject WScript.Shell; $Shortcut = $WshShell.CreateShortcut('%USERPROFILE%\Desktop\Hunter.lnk'); $Shortcut.TargetPath = '%~dp0bin\huntercensor.exe'; $Shortcut.WorkingDirectory = '%~dp0'; $Shortcut.IconLocation = '%~dp0bin\huntercensor.exe,0'; $Shortcut.Save()"
if %errorlevel% equ 0 (
    echo [OK] Desktop shortcut created
) else (
    echo [WARN] Failed to create desktop shortcut
)

:: Create startup script
echo [INFO] Creating startup script...
(
echo @echo off
echo cd /d "%~dp0"
echo echo Starting Hunter DPI Bypass...
echo bin\huntercensor.exe
echo pause
) > "start_hunter.bat"
echo [OK] Created start_hunter.bat

:: Display final information
echo.
echo ========================================
echo Setup Complete!
echo ========================================
echo.
echo Hunter DPI Bypass v1.3.0 has been configured.
echo.
echo Features:
echo - TLS ClientHello Fragmentation (enabled by default)
echo - TTL Manipulation (optional)
echo - TCP Fragmentation
echo - Domain Fronting
echo - Edge Router Bypass (domestic zones only)
echo - MAC Address Display for network hops
echo - Manual bypass testing API
echo.
echo Files created:
echo - bin\huntercensor.exe (main application)
echo - bin\xray.exe (proxy engine)
echo - bin\geosite.dat (domain database)
echo - .env (configuration file)
echo - start_hunter.bat (startup script)
echo.
echo To start Hunter:
echo   1. Double-click start_hunter.bat
echo   2. Or run: bin\huntercensor.exe
echo   3. Or use desktop shortcut
echo.
echo Configuration file: .env
echo Logs directory: logs\
echo Import directory: config\import\
echo.
echo Proxy will be available at: 127.0.0.1:10808 (SOCKS5)
echo.
echo For DPI bypass settings, edit .env file or use the GUI.
echo.
pause
