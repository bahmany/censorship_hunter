@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

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

REM Copy to release_package
echo Copying executables to release_package...
if not exist ..\release_package mkdir ..\release_package
copy /y build\huntercensor.exe ..\release_package\huntercensor.exe >nul
copy /y build\hunter_tests.exe ..\release_package\hunter_tests.exe >nul
if exist build\hunter_tests.exe (
    echo Copied huntercensor.exe and hunter_tests.exe to release_package
) else (
    echo Copied huntercensor.exe to release_package
)

echo Syncing latest runtime/config data into release_package and installer staging...
if not exist ..\release_package\runtime mkdir ..\release_package\runtime
if not exist ..\release_package\config mkdir ..\release_package\config
if not exist ..\release_package\bin mkdir ..\release_package\bin
if not exist ..\installer\staging\runtime mkdir ..\installer\staging\runtime
if not exist ..\installer\staging\config mkdir ..\installer\staging\config
if not exist ..\installer\staging\bin mkdir ..\installer\staging\bin

copy /y build\huntercensor.exe ..\installer\staging\huntercensor.exe >nul
echo [OK] Synced installer staging executable

if exist ..\bin\sing-box.exe (
    copy /y ..\bin\sing-box.exe ..\release_package\bin\sing-box.exe >nul
    copy /y ..\bin\sing-box.exe ..\installer\staging\bin\sing-box.exe >nul
    echo [OK] Synced bin\sing-box.exe
)
if exist ..\bin\tor.exe (
    copy /y ..\bin\tor.exe ..\release_package\bin\tor.exe >nul
    copy /y ..\bin\tor.exe ..\installer\staging\bin\tor.exe >nul
    echo [OK] Synced bin\tor.exe
)
if exist ..\bin\geosite.dat (
    copy /y ..\bin\geosite.dat ..\release_package\bin\geosite.dat >nul
    copy /y ..\bin\geosite.dat ..\installer\staging\bin\geosite.dat >nul
    echo [OK] Synced bin\geosite.dat
)

if exist ..\runtime\HUNTER_config_db.tsv (
    copy /y ..\runtime\HUNTER_config_db.tsv ..\release_package\runtime\HUNTER_config_db.tsv >nul
    copy /y ..\runtime\HUNTER_config_db.tsv ..\installer\staging\runtime\HUNTER_config_db.tsv >nul
    echo [OK] Synced runtime\HUNTER_config_db.tsv
)
if exist ..\runtime\HUNTER_config_db_export.txt (
    copy /y ..\runtime\HUNTER_config_db_export.txt ..\release_package\runtime\HUNTER_config_db_export.txt >nul
    copy /y ..\runtime\HUNTER_config_db_export.txt ..\installer\staging\runtime\HUNTER_config_db_export.txt >nul
    echo [OK] Synced runtime\HUNTER_config_db_export.txt
)
if exist ..\runtime\sources_manager.tsv (
    copy /y ..\runtime\sources_manager.tsv ..\release_package\runtime\sources_manager.tsv >nul
    copy /y ..\runtime\sources_manager.tsv ..\installer\staging\runtime\sources_manager.tsv >nul
    echo [OK] Synced runtime\sources_manager.tsv
)
if exist ..\runtime\source_history.tsv (
    copy /y ..\runtime\source_history.tsv ..\release_package\runtime\source_history.tsv >nul
    copy /y ..\runtime\source_history.tsv ..\installer\staging\runtime\source_history.tsv >nul
    echo [OK] Synced runtime\source_history.tsv
)
if exist ..\config\All_Configs_Sub.txt (
    copy /y ..\config\All_Configs_Sub.txt ..\release_package\config\All_Configs_Sub.txt >nul
    copy /y ..\config\All_Configs_Sub.txt ..\installer\staging\config\All_Configs_Sub.txt >nul
    echo [OK] Synced config\All_Configs_Sub.txt
)
if exist ..\config\all_extracted_configs.txt (
    copy /y ..\config\all_extracted_configs.txt ..\release_package\config\all_extracted_configs.txt >nul
    copy /y ..\config\all_extracted_configs.txt ..\installer\staging\config\all_extracted_configs.txt >nul
    echo [OK] Synced config\all_extracted_configs.txt
)
if exist ..\config\sub.txt (
    copy /y ..\config\sub.txt ..\release_package\config\sub.txt >nul
    copy /y ..\config\sub.txt ..\installer\staging\config\sub.txt >nul
    echo [OK] Synced config\sub.txt
)

echo.
echo === Build succeeded ===
echo Executable: build\huntercensor.exe
echo Also available in: ..\release_package\huntercensor.exe
echo.
echo To run:
echo   build\huntercensor.exe
echo   or
echo   ..\release_package\huntercensor.exe
echo.
if /i "%~1"=="--no-pause" exit /b 0
pause
