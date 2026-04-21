# Hunter C++ CLion Configuration

## Overview
This project uses C++17 with CMake build system and requires MSYS2 UCRT64 toolchain on Windows.

## Prerequisites
1. Install MSYS2 from https://www.msys2.org/
2. In MSYS2 UCRT64 shell, install required packages:
   ```bash
   pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,zlib}
   ```

## CLion Setup Instructions

### 1. Toolchain Configuration
- Open CLion Settings/Preferences → Build, Execution, Deployment → Toolchains
- Add new toolchain: "MSYS2 UCRT64"
- C Compiler: `C:\msys64\ucrt64\bin\gcc.exe`
- C++ Compiler: `C:\msys64\ucrt64\bin\g++.exe`
- Debugger: `C:\msys64\ucrt64\bin\gdb.exe`
- Make: `C:\msys64\ucrt64\bin\mingw32-make.exe` (or use ninja)

### 2. CMake Profiles
- Open Settings → Build, Execution, Deployment → CMake
- Add two profiles:

**Release Profile:**
- Name: "Release"
- Toolchain: "MSYS2 UCRT64"
- Build type: "Release"
- CMake options: `-G Ninja`
- Build directory: `$PROJECT_DIR$/build`

**Debug Profile:**
- Name: "Debug" 
- Toolchain: "MSYS2 UCRT64"
- Build type: "Debug"
- CMake options: `-G Ninja`
- Build directory: `$PROJECT_DIR$/cmake-build-debug`

### 3. Build Configuration
The project creates:
- Static library: `hunter_core`
- GUI executable: `huntercensor` (Windows only)
- Test executable: `hunter_tests`

### 4. Run Configurations
Three run configurations are pre-configured:
- **Build Release**: Builds and runs `huntercensor` in release mode
- **Run Tests**: Builds and runs `hunter_tests` 
- **Debug Build**: Builds and runs `huntercensor` in debug mode

### 5. Code Style
Project follows C++17 standards with:
- 120 character line limit
- snake_case naming for variables/functions
- PascalCase for types
- Trailing underscore for member variables

## Build Targets
- `hunter_core`: Static library containing all core functionality
- `huntercensor`: Windows GUI application with Dear ImGui + DirectX9
- `hunter_tests`: Unit tests (22 tests)

## Dependencies
- Threads: `Threads::Threads`
- Compression: `ZLIB`
- Network: `libcurl` (static linking on Windows)
- Windows libraries: `ws2_32`, `winhttp`, `wininet`, `crypt32`, etc.

## Testing
Run the test executable to verify all components work:
```bash
./build/hunter_tests.exe
```

## Alternative Build (Command Line)
You can also build using the provided batch script:
```bash
# In project root
.\build.bat
```

This script handles MSYS2 toolchain setup, CMake configuration, and building with proper static linking.
