@echo off
echo Hunter VPN Environment Check
echo ============================
echo.

echo Checking Visual Studio 2022...
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" (
    echo [OK] Visual Studio C++ compiler found
) else (
    echo [MISSING] Visual Studio C++ compiler
    echo   Need: Desktop development with C++ workload
)

echo.
echo Checking Flutter...
where flutter >nul 2>&1
if %errorlevel% == 0 (
    echo [OK] Flutter installed
    flutter --version | head -1
) else (
    echo [MISSING] Flutter not in PATH
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
echo   C++ CLI: LATEST (March 11, 2026)
echo   Flutter UI: OLD (March 10, 2026)
echo.

pause
