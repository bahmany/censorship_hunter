@echo off
REM Set up MSVC environment and build Flutter Windows app
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
echo === MSVC environment set up ===
where cl.exe
where rc.exe
echo === Building Flutter Windows Release ===
cd /d "D:\projects\v2ray\pythonProject1\hunter\hunter_flutter_ui"
C:\flutter\bin\flutter.bat build windows --release
