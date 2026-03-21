# Hunter VPN Release Package v1.2.2

## Status

- **CLI**: up to date
- **Native App**: up to date
- **Installer**: built from `installer/hunter_setup.iss`

## Included Highlights

- Single native Windows application powered by C++ + Dear ImGui
- Simple main pages for overview, configs, censorship tools, and logs
- Technical runtime, source, Telegram, and provisioning controls grouped into `Advanced`
- Integrated offline censorship probe and exit-IP discovery inside the native app

## Package Contents

```text
hountersansor.exe
hountersansor_cli.exe
app_icon.ico
runtime/
```

## Notes

- The native executable is the current Windows release build.
- The CLI binary is the current C++ console build.
- Proxy core binaries are bundled in the installer staging under `bin/`.
- This package is intended for publication as the `v1.2.2` release line.
