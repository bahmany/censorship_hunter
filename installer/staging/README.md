# huntercensor Release Package v1.4.0

## Status

- **CLI**: up to date
- **Native App**: up to date
- **Installer**: built from `installer/hunter_setup.iss`

## Included Highlights

- Single native Windows application powered by C++ + Dear ImGui
- Sing-box-only runtime with mixed local proxy ports for HTTP + SOCKS auto-detection
- Hardened Telegram-only validation using Telegram DC, domain, CDN, and web reachability checks
- Redesigned Home dashboard with live provisioned-port activity and clearer Full / TG-only / History views
- Bundled proxy runtimes and runtime database for self-contained operation

## Package Contents

```text
huntercensor.exe
app_icon.ico
bin/
config/
runtime/
```

## Notes

- The native executable is the current Windows release build.
- Proxy core binaries are bundled under `bin/`.
- The bundled runtime folder may include `HUNTER_config_db.tsv`, balancer caches, and live config snapshots.
- The installer output is expected at `installer/output/huntercensor-Setup-v1.4.0.exe`.
- This package is intended for publication as the `v1.4.0` release line.
