# Core engine bootstrap (source-first)

This project now bootstraps proxy cores from upstream source repositories during build:

- `v2ray-core` from `https://github.com/v2ray/v2ray-core`
- `sing-box` from `https://github.com/sagernet/sing-box`

Script:

- `hunter_cpp/scripts/bootstrap_core_engines.py`

Build integration:

- `hunter_cpp/build.bat` runs bootstrap before packaging sync.

Outputs:

- `bin/v2ray.exe`
- `bin/xray.exe` (compatibility alias to `v2ray.exe` for current runtime paths)
- `bin/sing-box.exe`

## Requirements

- `git`
- `go` (toolchain in `PATH`)

## Notes

- This removes the need to manually provide prebuilt binaries in `bin/` before packaging.
- Existing runtime paths still reference `xray.exe`; alias is created automatically for compatibility.
