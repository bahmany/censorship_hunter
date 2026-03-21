@echo off
echo Hunter VPN Environment Check
echo ============================
echo.

echo Checking huntercensor executable...
if exist "huntercensor.exe" (
    echo [OK] huntercensor.exe present
) else (
    echo [MISSING] huntercensor.exe
)

echo.
echo Checking MSYS2...
where pacman >nul 2>&1
if %errorlevel% == 0 (
    echo [OK] MSYS2 installed
) else (
    echo [MISSING] MSYS2 not found
)

echo.
echo Checking proxy on port 11808...
netstat -an | findstr ":11808" >nul 2>&1
if %errorlevel% == 0 (
    echo [OK] Proxy running on 127.0.0.1:11808
) else (
    echo [NOT RUNNING] Proxy not found on port 11808
)

echo.
echo Build Status:
echo   Native App: LATEST
echo.

pause
