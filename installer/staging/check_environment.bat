@echo off
echo huntercensor Release Environment Check
echo =====================================
echo.

echo Checking huntercensor executable...
if exist "huntercensor.exe" (
    echo [OK] huntercensor.exe present
) else if exist "hountersansor.exe" (
    echo [OK] legacy-named executable present ^(will be renamed in installer^)
) else (
    echo [MISSING] huntercensor.exe
)

echo.
echo Checking bundled proxy runtimes...
if exist "bin\xray.exe" (
    echo [OK] xray.exe present
) else (
    echo [MISSING] bin\xray.exe
)
if exist "bin\sing-box.exe" (
    echo [OK] sing-box.exe present
) else (
    echo [MISSING] bin\sing-box.exe
)
if exist "bin\mihomo-windows-amd64-compatible.exe" (
    echo [OK] mihomo runtime present
) else (
    echo [MISSING] bin\mihomo-windows-amd64-compatible.exe
)
if exist "bin\tor.exe" (
    echo [OK] tor.exe present
) else (
    echo [MISSING] bin\tor.exe
)

echo.
echo Checking bundled runtime data...
if exist "runtime\HUNTER_config_db.tsv" (
    echo [OK] runtime database present
) else (
    echo [WARN] runtime\HUNTER_config_db.tsv missing
)
if exist "runtime\HUNTER_balancer_cache.json" (
    echo [OK] balancer cache present
) else (
    echo [WARN] runtime\HUNTER_balancer_cache.json missing
)
if exist "config\All_Configs_Sub.txt" (
    echo [OK] bundled config sources present
) else (
    echo [WARN] bundled config sources missing
)

echo.
echo Release Status:
echo   Package: self-contained desktop release
echo   Local proxy endpoint after launch: 127.0.0.1:10808 ^(mixed HTTP+SOCKS^)
echo.

pause
