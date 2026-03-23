@echo off
setlocal enabledelayedexpansion

echo ========================================
echo huntercensor Setup v1.3.0
echo ========================================
echo.
echo [INFO] Running in user mode. Administrator rights are optional.
echo.

:: Create required directories
echo [INFO] Creating directories...
if not exist "bin" mkdir bin
if not exist "runtime" mkdir runtime
if not exist "runtime\xray_tmp" mkdir runtime\xray_tmp
if not exist "runtime\engine_tmp" mkdir runtime\engine_tmp
if not exist "config" mkdir config
if not exist "config\import" mkdir config\import
if not exist "logs" mkdir logs
if not exist "cache" mkdir cache

:: Copy runtime engines from release_package if available
echo [INFO] Setting up proxy engines...
if exist "release_package\bin\xray.exe" (
    copy /y "release_package\bin\xray.exe" "bin\xray.exe" >nul
    echo [OK] xray.exe copied from release_package
) else if exist "bin\xray.exe" (
    echo [OK] xray.exe already exists
) else (
    echo [WARN] xray.exe not found. Please place it in bin\ manually.
)

if exist "release_package\bin\sing-box.exe" (
    copy /y "release_package\bin\sing-box.exe" "bin\sing-box.exe" >nul
    echo [OK] sing-box.exe copied from release_package
) else if exist "bin\sing-box.exe" (
    echo [OK] sing-box.exe already exists
) else (
    echo [WARN] sing-box.exe not found.
)

if exist "release_package\bin\mihomo-windows-amd64-compatible.exe" (
    copy /y "release_package\bin\mihomo-windows-amd64-compatible.exe" "bin\mihomo-windows-amd64-compatible.exe" >nul
    echo [OK] mihomo-windows-amd64-compatible.exe copied from release_package
) else if exist "bin\mihomo-windows-amd64-compatible.exe" (
    echo [OK] mihomo-windows-amd64-compatible.exe already exists
) else (
    echo [WARN] mihomo-windows-amd64-compatible.exe not found.
)

if exist "release_package\bin\tor.exe" (
    copy /y "release_package\bin\tor.exe" "bin\tor.exe" >nul
    echo [OK] tor.exe copied from release_package
) else if exist "bin\tor.exe" (
    echo [OK] tor.exe already exists
) else (
    echo [WARN] tor.exe not found.
)

:: Copy geosite data if exists
echo [INFO] Setting up geosite data...
if exist "release_package\bin\geosite.dat" (
    copy /y "release_package\bin\geosite.dat" "bin\geosite.dat" >nul
    echo [OK] Geosite data copied from release_package
) else if exist "bin\geosite.dat" (
    echo [OK] Geosite data already exists
) else (
    echo [WARN] Geosite data not found. Iranian domain routing will use fallback method.
)

:: Copy huntercensor.exe from available sources
echo [INFO] Setting up huntercensor binary...
if exist "hunter_cpp\build\huntercensor.exe" (
    copy /y "hunter_cpp\build\huntercensor.exe" "bin\huntercensor.exe" >nul
    echo [OK] huntercensor binary copied from build directory
) else if exist "hunter_cpp\build\hountersansor.exe" (
    copy /y "hunter_cpp\build\hountersansor.exe" "bin\huntercensor.exe" >nul
    echo [OK] huntercensor binary copied from legacy build output
) else if exist "release_package\huntercensor.exe" (
    copy /y "release_package\huntercensor.exe" "bin\huntercensor.exe" >nul
    echo [OK] huntercensor binary copied from release_package
) else if exist "release_package\hountersansor.exe" (
    copy /y "release_package\hountersansor.exe" "bin\huntercensor.exe" >nul
    echo [OK] huntercensor binary copied from legacy release_package name
) else if exist "bin\huntercensor.exe" (
    echo [OK] huntercensor binary already exists
) else (
    echo [ERROR] huntercensor binary not found. Please build the project first.
    echo   cd hunter_cpp ^&^& mkdir build ^&^& cd build
    echo   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
    echo   ninja
    pause
    exit /b 1
)

echo [INFO] Copying bundled runtime data if available...
if exist "release_package\runtime\HUNTER_config_db.tsv" (
    copy /y "release_package\runtime\HUNTER_config_db.tsv" "runtime\HUNTER_config_db.tsv" >nul
    echo [OK] Runtime config database copied
)
if exist "release_package\runtime\HUNTER_balancer_cache.json" (
    copy /y "release_package\runtime\HUNTER_balancer_cache.json" "runtime\HUNTER_balancer_cache.json" >nul
    echo [OK] Main balancer cache copied
)
if exist "release_package\runtime\HUNTER_gemini_balancer_cache.json" (
    copy /y "release_package\runtime\HUNTER_gemini_balancer_cache.json" "runtime\HUNTER_gemini_balancer_cache.json" >nul
    echo [OK] Gemini balancer cache copied
)
if exist "release_package\runtime\HUNTER_gold.txt" (
    copy /y "release_package\runtime\HUNTER_gold.txt" "runtime\HUNTER_gold.txt" >nul
    echo [OK] Gold cache copied
)
if exist "release_package\runtime\HUNTER_silver.txt" (
    copy /y "release_package\runtime\HUNTER_silver.txt" "runtime\HUNTER_silver.txt" >nul
    echo [OK] Silver cache copied
)
if exist "release_package\runtime\hunter_config.json" (
    copy /y "release_package\runtime\hunter_config.json" "runtime\hunter_config.json" >nul
    echo [OK] Runtime configuration copied
)

echo [INFO] Copying bundled config sources if available...
if exist "release_package\config\All_Configs_Sub.txt" (
    copy /y "release_package\config\All_Configs_Sub.txt" "config\All_Configs_Sub.txt" >nul
    echo [OK] All_Configs_Sub.txt copied
)
if exist "release_package\config\all_extracted_configs.txt" (
    copy /y "release_package\config\all_extracted_configs.txt" "config\all_extracted_configs.txt" >nul
    echo [OK] all_extracted_configs.txt copied
)
if exist "release_package\config\sub.txt" (
    copy /y "release_package\config\sub.txt" "config\sub.txt" >nul
    echo [OK] sub.txt copied
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
net session >nul 2>&1
if %errorlevel% equ 0 (
    netsh advfirewall firewall delete rule name="huntercensor" >nul 2>&1
    netsh advfirewall firewall add rule name="huntercensor" dir=in action=allow program="%~dp0bin\huntercensor.exe" enable=yes >nul
    netsh advfirewall firewall add rule name="huntercensor" dir=out action=allow program="%~dp0bin\huntercensor.exe" enable=yes >nul
    if %errorlevel% equ 0 (
        echo [OK] Firewall rules configured
    ) else (
        echo [WARN] Failed to configure firewall rules
    )
) else (
    echo [INFO] Skipping firewall rules because setup is not elevated
)

:: Create desktop shortcut
echo [INFO] Creating desktop shortcut...
powershell -Command "$WshShell = New-Object -comObject WScript.Shell; $Shortcut = $WshShell.CreateShortcut('%USERPROFILE%\Desktop\huntercensor.lnk'); $Shortcut.TargetPath = '%~dp0bin\huntercensor.exe'; $Shortcut.WorkingDirectory = '%~dp0'; $Shortcut.IconLocation = '%~dp0bin\huntercensor.exe,0'; $Shortcut.Save()"
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
echo echo Starting huntercensor...
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
echo huntercensor v1.3.0 has been configured.
echo.
echo Features:
echo - Integrated native Windows application
echo - Bundled XRay, sing-box, mihomo, and Tor runtimes
echo - Imported config database and balancer caches when bundled
echo - Local mixed proxy listener on 127.0.0.1:10808
echo - Offline censorship tooling and discovery workflow
echo.
echo Files created:
echo - bin\huntercensor.exe (main application)
echo - bin\xray.exe (proxy engine)
echo - bin\sing-box.exe / mihomo-windows-amd64-compatible.exe / tor.exe
echo - runtime\HUNTER_config_db.tsv (bundled config database if present)
echo - .env (configuration file)
echo - start_hunter.bat (startup script)
echo.
echo To start huntercensor:
echo   1. Double-click start_hunter.bat
echo   2. Or run: bin\huntercensor.exe
echo   3. Or use desktop shortcut
echo.
echo Configuration file: .env
echo Logs directory: logs\
echo Import directory: config\import\
echo.
echo Proxy will be available at: 127.0.0.1:10808 (mixed HTTP+SOCKS)
echo.
echo For DPI bypass settings, edit .env file or use the GUI.
echo.
pause
