# Contributing to Hunter

Thank you for your interest in contributing to Hunter! This document provides guidelines for contributing to the project.

## Getting Started

1. Fork the repository
2. Clone your fork locally
3. Create a feature branch from `main`
4. Make your changes
5. Test your changes
6. Submit a pull request

## Development Setup

### C++ Backend

```bash
# MSYS2 UCRT64 shell
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,curl,zlib}

cd hunter_cpp
mkdir build && cd build
cmake .. -G Ninja
ninja

# Run tests
./hunter_tests.exe
```

### Flutter UI

```powershell
cd hunter_flutter_ui
flutter pub get
flutter analyze
flutter build windows --release
```

## Code Style

### C++

- **Standard**: C++17
- **Naming**: `snake_case` for variables and functions, `PascalCase` for types, trailing underscore for member variables (`member_var_`)
- **Headers**: `#pragma once`, organized includes (system → third-party → project)
- **Namespaces**: `hunter::`, `hunter::network::`, `hunter::proxy::`, etc.
- **Documentation**: Doxygen-style `@brief` comments on public APIs
- **Concurrency**: Use `std::atomic` for flags, `std::mutex` for shared state, avoid raw `new`/`delete`

### Dart/Flutter

- **Style**: Follow Dart effective style guide
- **Analysis**: Code must pass `flutter analyze` with zero issues
- **Widgets**: Prefer `StatelessWidget` with const constructors where possible

## Pull Request Guidelines

- Keep PRs focused on a single change
- Include a clear description of what changed and why
- Ensure all existing tests pass
- Add tests for new functionality
- Update documentation if behavior changes

## Reporting Issues

When reporting bugs, please include:

- OS version and architecture
- Steps to reproduce
- Expected vs actual behavior
- Relevant log output from `runtime/hunter_crash.log` or the Logs tab

## Architecture Notes

- The C++ backend (`hunter_cli`) is the core engine — it runs autonomously
- The Flutter UI (`hunter_dashboard`) is a thin monitoring/control layer
- Communication is via stdin/stdout JSON lines (primary) and file-based status (secondary)
- All network operations go through `libcurl` with configurable timeouts
- The `ConfigDatabase` is in-memory only — persistence is handled by `SmartCache` writing to files

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
