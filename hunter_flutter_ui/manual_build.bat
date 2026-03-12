@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  Manual Flutter Windows Release Build (bypasses flutter build)
REM  Uses vcvarsall to set up MSVC env, then VS cmake+ninja
REM ============================================================

set "PROJECT_DIR=D:\projects\v2ray\pythonProject1\hunter\hunter_flutter_ui"
set "BUILD_DIR=%PROJECT_DIR%\build\windows\x64"
set "FLUTTER_ROOT=C:\flutter"
set "TOOLCHAIN=%PROJECT_DIR%\msvc_toolchain.cmake"
set "NINJA=C:\msys64\ucrt64\bin\ninja.exe"
set "CMAKE=C:\msys64\ucrt64\bin\cmake.exe"

echo [1/4] Setting up MSVC environment...
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo FAILED: vcvarsall.bat
    exit /b 1
)
echo     cl.exe found at:
where cl.exe
echo     rc.exe found at:
where rc.exe

echo.
echo [2/4] Building Dart AOT snapshot and assets...
cd /d "%PROJECT_DIR%"
call "%FLUTTER_ROOT%\bin\flutter.bat" assemble --output "%BUILD_DIR%\flutter_assemble" -dTargetPlatform=windows-x64 -dBuildMode=release -dDartObfuscation=false -dTreeShakeIcons=true -dTrackWidgetCreation=false release_bundle_windows-x64_assets release_windows-x64
if errorlevel 1 (
    echo FAILED: flutter assemble
    exit /b 1
)

echo.
echo [3/4] Running CMake generation with Ninja...
if exist "%BUILD_DIR%\CMakeCache.txt" del /f "%BUILD_DIR%\CMakeCache.txt"
if exist "%BUILD_DIR%\CMakeFiles" rmdir /s /q "%BUILD_DIR%\CMakeFiles"

"%CMAKE%" -S "%PROJECT_DIR%\windows" -B "%BUILD_DIR%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DFLUTTER_TARGET_PLATFORM=windows-x64 ^
  -DPROJECT_BUILD_DIR="%BUILD_DIR%/" ^
  -DPROJECT_DIR="%PROJECT_DIR%"
if errorlevel 1 (
    echo FAILED: CMake generation
    exit /b 1
)

echo.
echo [4/4] Building with Ninja...
"%CMAKE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 (
    echo FAILED: Ninja build
    exit /b 1
)

echo.
echo === Assembling release package ===
set "OUT=%BUILD_DIR%\runner\Release"
if not exist "%OUT%" mkdir "%OUT%"
copy /y "%BUILD_DIR%\runner\hunter_dashboard.exe" "%OUT%\" >nul 2>&1
copy /y "%PROJECT_DIR%\windows\flutter\ephemeral\flutter_windows.dll" "%OUT%\" >nul 2>&1
copy /y "%PROJECT_DIR%\windows\flutter\ephemeral\icudtl.dat" "%OUT%\data\" >nul 2>&1
xcopy /s /y /i "%BUILD_DIR%\flutter_assemble\flutter_assets" "%OUT%\data\flutter_assets" >nul 2>&1
copy /y "%BUILD_DIR%\flutter_assemble\windows\app.so" "%OUT%\data\app.so" >nul 2>&1

echo.
echo ============================================================
echo  BUILD SUCCESSFUL
echo  Output: %OUT%
echo ============================================================
