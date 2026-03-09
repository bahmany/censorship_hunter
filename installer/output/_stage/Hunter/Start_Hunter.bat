@echo off
title Hunter Dashboard
echo ============================================
echo   Hunter Anti-Censorship Dashboard v1.0.0
echo ============================================
echo.

:: Create runtime directory
if not exist "runtime" mkdir runtime
if not exist "config\import" mkdir config\import

:: Start the Flutter dashboard
if exist "hunter_dashboard.exe" (
    echo Starting Hunter Dashboard...
    start "" "hunter_dashboard.exe"
) else (
    echo [ERROR] hunter_dashboard.exe not found!
    echo Please ensure all files are extracted correctly.
    pause
)
