@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ========================================
echo huntercensor Setup Builder
echo ========================================
echo.

set "ISCC_PATH=C:\Program Files (x86)\Inno Setup 6\iscc.exe"
set "ISS_FILE=installer\hunter_setup.iss"

echo [1/4] Building native app and syncing staging artifacts...
call hunter_cpp\build.bat --no-pause
if errorlevel 1 (
    echo [ERROR] build.bat failed.
    exit /b 1
)

echo [2/4] Verifying required installer staging files...
set "MISSING=0"
if not exist "installer\staging\huntercensor.exe" (
    echo [ERROR] Missing installer\staging\huntercensor.exe
    set "MISSING=1"
)
if not exist "installer\staging\bin\sing-box.exe" (
    echo [ERROR] Missing installer\staging\bin\sing-box.exe
    set "MISSING=1"
)
if not exist "installer\staging\app_icon.ico" (
    echo [ERROR] Missing installer\staging\app_icon.ico
    set "MISSING=1"
)
if "%MISSING%"=="1" (
    echo [ERROR] Staging is incomplete. Setup build aborted.
    exit /b 1
)

echo [3/4] Running Inno Setup compiler...
if not exist "%ISCC_PATH%" goto :missing_iscc
goto :run_iscc

:missing_iscc
echo [ERROR] Inno Setup compiler not found:
echo         %ISCC_PATH%
echo         Install Inno Setup 6 or update ISCC_PATH in setup.bat.
exit /b 1

:run_iscc

"%ISCC_PATH%" "%ISS_FILE%"
if errorlevel 1 (
    echo [ERROR] Inno Setup compile failed.
    exit /b 1
)

echo [4/4] Done.
echo [OK] Setup executable created under installer\output\
echo.
pause

