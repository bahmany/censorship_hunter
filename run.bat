@echo off
REM Hunter Project Launcher - Uses Python launcher

echo ============================================
echo   HUNTER - Advanced V2Ray Proxy Hunting System
echo   Autonomous censorship circumvention tool
echo ============================================

REM Check if virtual environment exists
if not exist ".venv\Scripts\python.exe" (
    echo Error: Virtual environment not found at .venv
    echo Please run setup first.
    pause
    exit /b 1
)

REM Activate virtual environment and run launcher
call .venv\Scripts\activate
echo.
echo   Web Admin Dashboard: http://localhost:8585
echo.
python launcher.py

pause
