#!/usr/bin/env python3
"""
prepare_android_engines.py — Copy Android-native engine binaries to APK assets.

Android requires ARM64 (or x86_64 for emulator) builds of xray and sing-box.
These must be built separately (e.g. via Go cross-compilation for android/arm64)
and placed in bin/android/. This script copies them into the APK assets directory.

Usage:
  prepare_android_engines.py --bin <dir_with_android_engines> --assets <apk_assets_dir>
"""

import argparse
import shutil
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description="Copy Android engine binaries to APK assets.")
    ap.add_argument("--bin", required=True, help="Directory containing android-native xray/sing-box")
    ap.add_argument("--assets", required=True, help="APK assets directory (app/src/main/assets/engines)")
    args = ap.parse_args()

    bin_dir = Path(args.bin)
    assets_dir = Path(args.assets)
    assets_dir.mkdir(parents=True, exist_ok=True)

    for name in ["xray", "sing-box"]:
        src = bin_dir / name
        if not src.exists():
            print(f"[android-engines] WARNING: {src} not found — APK will lack {name}", file=sys.stderr)
            continue
        dst = assets_dir / name
        shutil.copy2(src, dst)
        print(f"[android-engines] {name}: {src.stat().st_size} bytes -> {dst}")

    print(f"[android-engines] Done. Engines in {assets_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
