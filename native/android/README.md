# تلاش برای عبور از فیلترینگ ایران (Iran Filter Bypass)

Advanced V2Ray VPN System with 2026 DPI Evasion — native C++ rewrite for Android 8.0+ (API 26+).

## 🎯 Features

- **Modern Material3 UI** with dark theme
- **Android VPN Service** with load balancing (up to 10 concurrent configs)
- **Per-app VPN (Split Tunneling)** - select which apps use VPN
- **Auto config discovery** from Telegram channels and GitHub repos
- **2026 DPI Evasion** implementing techniques from the Iranian filtering report:
  - VLESS-Reality-Vision support
  - TLS fingerprint randomization (JA3/JA4 spoofing)
  - SplitHTTP/XHTTP transport
  - TLS fragmentation for DPI bypass
  - MTU optimization for 5G networks
- **Google Play compliant** with proper permissions and policies
- **Persian (Farsi) localization**

## Architecture

```
┌─────────────────────────────────────────────┐
│              Java/Android Layer              │
│  MainActivity ← HunterService (Foreground)  │
│         HunterCallbackImpl (OkHttp,         │
│         V2Ray engine, Telegram Bot API)     │
├─────────────────────────────────────────────┤
│                  JNI Bridge                  │
│              hunter_jni.cpp                  │
├─────────────────────────────────────────────┤
│            C++ Native Core Engine            │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │  Config   │  │ Parsers  │  │  Network  │ │
│  │  Models   │  │ VMess    │  │  HTTP     │ │
│  │  Utils    │  │ VLESS    │  │  Fetcher  │ │
│  │          │  │ Trojan   │  │           │ │
│  │          │  │ SS       │  │           │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │ Security │  │ Testing  │  │  Proxy    │ │
│  │ ADEE     │  │ Bench    │  │  Balancer │ │
│  │ Stealth  │  │          │  │           │ │
│  └──────────┘  └──────────┘  └───────────┘ │
│  ┌──────────┐  ┌──────────┐                 │
│  │ Telegram │  │  Cache   │                 │
│  │ Scraper  │  │  Smart   │                 │
│  │ Reporter │  │          │                 │
│  └──────────┘  └──────────┘                 │
│         ┌─────────────────┐                 │
│         │  Orchestrator   │                 │
│         │  (Main Loop)    │                 │
│         └─────────────────┘                 │
└─────────────────────────────────────────────┘
```

## Features (1:1 parity with Python version)

- **Multi-source config scraping**: Telegram channels, GitHub repos, anti-censorship sources, Iran priority (Reality-focused)
- **Protocol support**: VMess, VLESS, Trojan, Shadowsocks — full URI parsing
- **Anti-DPI prioritization**: 8-tier config ranking based on Iran's filtering techniques (BGP, DPI, Protocol Suppression)
- **Iran fragment support**: TLS Hello fragmentation for DPI bypass
- **Multi-engine benchmarking**: XRay/V2Ray core integration via JNI
- **Load balancer**: Multi-backend XRay balancer with health checking and auto-rotation
- **Stealth obfuscation**: SNI rotation, CDN domain fronting
- **ADEE engine**: Adversarial DPI Exhaustion Engine
- **Smart caching**: Persistent config cache with failure tracking
- **Telegram integration**: Channel scraping (public) + Bot API reporting
- **Gemini balancer**: Optional secondary balancer for Gemini-tagged configs
- **Foreground service**: Runs reliably on Android 8+ with proper notification
- **Boot auto-start**: Optional restart after device reboot

## Android Compatibility

| Android Version | API Level | Status |
|----------------|-----------|--------|
| Android 8.0    | 26        | ✅ Supported |
| Android 9.0    | 28        | ✅ Supported |
| Android 10     | 29        | ✅ Supported |
| Android 11     | 30        | ✅ Supported |
| Android 12     | 31        | ✅ Supported |
| Android 13     | 33        | ✅ Supported |
| Android 14     | 34        | ✅ Supported (Target) |

## ABIs Supported

- `armeabi-v7a` (32-bit ARM)
- `arm64-v8a` (64-bit ARM)
- `x86` (Intel 32-bit, emulators)
- `x86_64` (Intel 64-bit, emulators)

## Prerequisites

1. **Android Studio** (latest stable)
2. **Android NDK** r26+ (configured in `build.gradle`)
3. **CMake** 3.22.1+ (via Android Studio SDK Manager)
4. **XRay/V2Ray core** binary for Android (ARM/x86 .so or executable)

## Build

```bash
# Clone and open in Android Studio
# OR build from command line:

cd native/android

# Debug build
./gradlew assembleDebug

# Release build
./gradlew assembleRelease

# Install on connected device
./gradlew installDebug
```

## Configuration

Configuration can be set via:
1. **UI**: MainActivity settings fields
2. **SharedPreferences**: `hunter_config`
3. **Environment file**: `{filesDir}/hunter_secrets.env`

### Required Settings

| Setting | Description |
|---------|-------------|
| `api_id` | Telegram API ID |
| `api_hash` | Telegram API Hash |
| `phone` | Phone number for Telegram |
| `bot_token` | Telegram Bot token (for reporting) |
| `chat_id` | Telegram report channel ID |

### Optional Settings

| Setting | Default | Description |
|---------|---------|-------------|
| `max_workers` | 30 | Concurrent benchmark workers |
| `sleep_seconds` | 300 | Seconds between cycles |
| `timeout_seconds` | 10 | Proxy test timeout |
| `iran_fragment` | false | Enable TLS fragmentation |
| `multiproxy_port` | 10808 | Local SOCKS proxy port |

## V2Ray Integration

The app requires an XRay/V2Ray core binary. Options:
1. Bundle `libxray.so` in `jniLibs/` for each ABI
2. Download at runtime from a trusted source
3. Use [AmazTool](https://github.com/) or similar Android V2Ray library

Set the path via `xray_path` in SharedPreferences.

## Project Structure

```
app/src/main/
├── cpp/                          # C++ native core
│   ├── CMakeLists.txt
│   ├── core/                     # Config, models, utilities
│   │   ├── config.h / .cpp
│   │   ├── models.h
│   │   └── utils.h / .cpp
│   ├── parsers/                  # Protocol URI parsers
│   │   └── uri_parser.h / .cpp
│   ├── network/                  # HTTP client & config fetcher
│   │   └── http_client.h / .cpp
│   ├── testing/                  # Proxy benchmarking
│   │   └── benchmark.h / .cpp
│   ├── proxy/                    # Load balancer
│   │   └── load_balancer.h / .cpp
│   ├── telegram/                 # Telegram scraper & reporter
│   │   └── scraper.h / .cpp
│   ├── security/                 # Obfuscation engines
│   │   └── obfuscation.h / .cpp
│   ├── cache/                    # Smart caching
│   │   └── cache.h / .cpp
│   ├── orchestrator.h / .cpp     # Main workflow
│   └── hunter_jni.cpp            # JNI bridge
├── java/com/hunter/app/          # Java/Android layer
│   ├── HunterApplication.java
│   ├── HunterNative.java         # JNI interface
│   ├── HunterCallbackImpl.java   # Android API implementations
│   ├── HunterService.java        # Foreground service
│   ├── MainActivity.java         # UI
│   └── BootReceiver.java         # Auto-start on boot
└── res/
    ├── layout/activity_main.xml
    ├── values/strings.xml
    └── xml/network_security_config.xml
```

## License

Same license as the parent Hunter project.
