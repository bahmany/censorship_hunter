@echo off
setlocal enabledelayedexpansion

echo === Building Hunter C++ with MSYS2 UCRT64 ===

REM Add UCRT64 toolchain to PATH
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

REM Verify tools exist
where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake not found. Install mingw-w64-ucrt-x86_64-cmake
    exit /b 1
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: ninja not found. Install mingw-w64-ucrt-x86_64-ninja
    exit /b 1
)

where gcc >nul 2>&1
if errorlevel 1 (
    echo ERROR: gcc not found. Install mingw-w64-ucrt-x86_64-gcc
    exit /b 1
)

echo Tools found.

REM Clean previous build if requested
if /i "%1"=="clean" (
    echo Cleaning build directory...
    if exist build rmdir /s /q build
)

REM Ensure no running GUI instance keeps huntercensor.exe locked
echo Closing running huntercensor processes if any...
taskkill /F /IM huntercensor.exe >nul 2>&1
if exist build\huntercensor.exe del /f /q build\huntercensor.exe >nul 2>&1
if exist ..\bin\huntercensor.exe del /f /q ..\bin\huntercensor.exe >nul 2>&1

REM Configure
echo Configuring with CMake...
cmake -S . -B build -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="C:/msys64/ucrt64/bin/ninja.exe" ^
  -DCMAKE_C_COMPILER="C:/msys64/ucrt64/bin/gcc.exe" ^
  -DCMAKE_CXX_COMPILER="C:/msys64/ucrt64/bin/g++.exe" ^
  -DCMAKE_BUILD_TYPE=Release

if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

REM Build
echo Building...
set "CMAKE_BUILD_PARALLEL_LEVEL=1"
cmake --build build -j 1

if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

if exist build\hountersansor_cli.exe del /f /q build\hountersansor_cli.exe >nul 2>&1
if exist ..\bin\hountersansor_cli.exe del /f /q ..\bin\hountersansor_cli.exe >nul 2>&1

echo.
echo === Build succeeded ===
echo Executable: build\huntercensor.exe
echo.
echo To run:
echo   build\huntercensor.exe
echo.
pause
