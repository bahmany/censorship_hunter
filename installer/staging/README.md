# huntercensor Release Package v1.3.0

## Status

- **CLI**: up to date
- **Native App**: up to date
- **Installer**: built from `installer/hunter_setup.iss`

## Included Highlights

- Single native Windows application powered by C++ + Dear ImGui
- Simple main pages for overview, configs, censorship tools, and logs
- Technical runtime, source, Telegram, and provisioning controls grouped into `Advanced`
- Integrated offline censorship probe and exit-IP discovery inside the native app
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
- The installer output is expected at `installer/output/huntercensor-Setup-v1.3.0.exe`.
- This package is intended for publication as the `v1.3.0` release line.
