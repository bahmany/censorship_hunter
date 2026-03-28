# AGENTS.md

Guidelines for AI coding agents working on this project.

## Project Overview

Hunter is a C++ proxy configuration discovery and load balancing engine with a native Windows GUI (Dear ImGui + DirectX9) and a console orchestrator.

## Build & Test

### Prerequisites

- MSYS2 with UCRT64 environment
- CMake ≥ 3.16
- Ninja
- Visual Studio 2022+ (Windows SDK)

### Build Commands

```bash
# In MSYS2 UCRT64 shell
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,zlib}

cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja
ninja
```

### Run Tests

```bash
cd hunter_cpp/build
./hunter_tests.exe
```

### Quick Validation (Python)

```bash
pip3 install requests
python3 hunter_cpp/test_working_sources.py
```

## Project Structure

```
hunter_cpp/           # C++ backend
  include/            # Headers (core, network, proxy, testing, security, etc.)
  src/                # Implementation
  tests/              # Unit tests (21 tests in test_core.cpp)
config/               # Runtime config
  import/             # Drop .txt files for auto-import
.continue/            # Continue IDE configuration
```

## Code Style

- **Standard**: C++17
- **Naming**: `snake_case` for variables/functions, `PascalCase` for types, trailing underscore for members (`member_var_`)
- **Headers**: `#pragma once`, organized includes (system → third-party → project)
- **Namespaces**: `hunter::`, `hunter::network::`, `hunter::proxy::`, etc.
- **Documentation**: Doxygen-style `@brief` comments on public APIs
- **Concurrency**: Use `std::atomic` for flags, `std::mutex` for shared state, avoid raw `new`/`delete`

## Key Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HUNTER_MULTIPROXY_PORT` | `10808` | Main SOCKS5 balancer port |
| `HUNTER_GEMINI_PORT` | `10809` | Secondary balancer port |
| `HUNTER_WORKERS` | `10` | Worker thread count |
| `HUNTER_TEST_TIMEOUT` | `10` | Per-config test timeout (seconds) |

## Architecture Notes

- Native app: `hountersansor.exe` (GUI)
- Console app: `hountersansor_cli.exe` (orchestrator)
- 9 concurrent worker threads: ConfigScanner, GitHubDownloader, ContinuousValidator, AggressiveHarvester, BalancerMonitor, HealthMonitor, TelegramPublisher, DpiPressure, ImportWatcher
- Network operations use `libcurl`
- `ConfigDatabase` is in-memory; persistence via `SmartCache`

## Supported Protocols

VMess, VLESS, VLESS+Reality, Trojan, Shadowsocks, ShadowsocksR, Hysteria2, TUIC

## Network Ports

- `10808`: Main SOCKS5 balancer
- `10809`: Gemini SOCKS5 balancer
- `2901-2999`: Provisioned XRay proxies
- `11808+`: Benchmark ports (temporary)
