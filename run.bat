@echo off
REM Hunter Project Launcher - Uses Python launcher

echo ============================================
echo   HUNTER - Advanced V2Ray Proxy Hunting System
echo   Autonomous censorship circumvention tool
echo ============================================

REM Check if virtual environment exists
if not exist ".venv\Scripts\activate" (
    echo Error: Virtual environment not found at .venv
    echo Please run setup first.
    pause
    exit /b 1
)

REM Activate virtual environment
call .venv\Scripts\activate

REM Run the Python launcher
python launcher.py

pause
