@echo off
REM Hunter Project Launcher
REM Run the Hunter autonomous proxy hunting system

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

REM Load secrets from hunter_secrets.env if it exists
if exist "hunter_secrets.env" (
    echo Loading secrets from hunter_secrets.env...
    for /f "tokens=1,* delims==" %%a in (hunter_secrets.env) do (
        call :set_var %%a %%b
    )
    echo Secrets loaded successfully.
) else (
    echo Warning: hunter_secrets.env not found.
)

goto :main

:set_var
set "key=%~1"
set "value=%~2"
if "%value:~0,1%"=="""" set "value=%value:~1%"
if "%value:~-1%"=="""" set "value=%value:~0,-1%"
set "%key%=%value%"
goto :eof

:main

REM Check for required environment variables
if "%HUNTER_API_ID%"=="" (
    echo Warning: HUNTER_API_ID not set
    echo Set it with: set HUNTER_API_ID=your_api_id
)

if "%HUNTER_API_HASH%"=="" (
    echo Warning: HUNTER_API_HASH not set
    echo Set it with: set HUNTER_API_HASH=your_api_hash
)

if "%HUNTER_PHONE%"=="" (
    echo Warning: HUNTER_PHONE not set
    echo Set it with: set HUNTER_PHONE=+1234567890
)

REM Activate virtual environment
call .venv\Scripts\activate

REM Ensure runtime directory exists
if not exist "runtime" (
    mkdir runtime
    echo Created runtime directory.
)

REM Run the hunter system
echo Starting Hunter...
python main.py

REM Deactivate virtual environment
call .venv\Scripts\deactivate

pause
