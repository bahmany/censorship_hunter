# Build Instructions for Hunter Android

## Method 1: Android Studio (Recommended)

1. **Install Android Studio** from https://developer.android.com/studio
2. **Open Project**: File → Open → Select `D:\projects\v2ray\pythonProject1\hunter\native\android`
3. **Android Studio will automatically**:
   - Download correct Gradle version
   - Download Android Gradle Plugin
   - Setup NDK and CMake
   - Build native C++ code
4. **Build**: Build → Make Project (Ctrl+F9) or Build → Build APK(s)

## Method 2: Manual Build (Advanced)

### Prerequisites
- Java 11 (JDK 11)
- Android SDK with API 30
- Android NDK 21.4.7075529
- CMake 3.18.1

### Environment Setup
```powershell
$env:ANDROID_HOME = "D:\projects\v2ray\pythonProject1\hunter\native\android\android-sdk"
$env:JAVA_HOME = "D:\projects\v2ray\pythonProject1\hunter\native\android\jdk-11.0.23+9"
$env:PATH = "$env:JAVA_HOME\bin;$env:ANDROID_HOME\cmdline-tools\latest\bin;$env:PATH"
```

### Build Commands
```powershell
# Clean build
.\gradlew.bat clean

# Build debug APK
.\gradlew.bat assembleDebug

# Build release APK
.\gradlew.bat assembleRelease
```

## APK Location
After successful build:
- Debug APK: `app/build/outputs/apk/debug/app-debug.apk`
- Release APK: `app/build/outputs/apk/release/app-release.apk`

## Troubleshooting

### If build fails with Gradle/AGP issues:
1. Use Android Studio (handles all dependencies automatically)
2. Or update `build.gradle` to use compatible versions:
   - Gradle: 6.7.1
   - AGP: 4.2.2
   - Compile SDK: 30
   - NDK: 21.4.7075529

### If C++ compilation fails:
1. Ensure NDK is installed: `sdkmanager "ndk;21.4.7075529"`
2. Check CMake is installed: `sdkmanager "cmake;3.18.1"`
3. Verify `local.properties` paths are correct

## Project Structure
```
app/src/main/cpp/
├── CMakeLists.txt          # Native build configuration
├── core/                   # Core C++ modules
├── parsers/                # Protocol parsers
├── network/                # HTTP client
├── security/               # Obfuscation engines
├── cache/                  # Smart caching
├── testing/                # Benchmarking
├── proxy/                  # Load balancer
├── telegram/               # Telegram integration
├── orchestrator/           # Main workflow
└── hunter_jni.cpp          # JNI bridge
```

## Features
- ✅ All C++ modules compiled
- ✅ JNI bridge to Java layer
- ✅ Android Service integration
- ✅ UI for configuration and monitoring
- ✅ Full feature parity with Python version
