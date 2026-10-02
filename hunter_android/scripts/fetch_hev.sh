#!/usr/bin/env bash
# Fetch hev-socks5-tunnel (MIT) at a pinned commit and build libhev-socks5-tunnel.so
# with ndk-build into app/src/main/jniLibs/arm64-v8a. The JNI class is
# com.hunter.app.TProxyService (PKGNAME/CLSNAME defines).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG=2.9.3; COMMIT=b9b9b7b9b0febe32bb5d8cdb9ffa414d94242b75
SRC="$HERE/.deps/hev-socks5-tunnel"
OUT="$HERE/app/src/main/jniLibs"
[[ -f "$OUT/arm64-v8a/libhev-socks5-tunnel.so" ]] && exit 0
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$NDK" ]]; then
  SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
  NDK="$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)"
fi
[[ -x "$NDK/ndk-build" ]] || { echo "NDK not found (set ANDROID_NDK_HOME)" >&2; exit 1; }
if [[ ! -d "$SRC" ]]; then
  mkdir -p "$HERE/.deps"
  git clone -q --branch "$TAG" --recurse-submodules --shallow-submodules --depth 1 \
    https://github.com/heiher/hev-socks5-tunnel "$SRC"
fi
[[ "$(git -C "$SRC" rev-parse HEAD)" == "$COMMIT" ]] || { echo "hev commit mismatch" >&2; exit 1; }
"$NDK/ndk-build" -C "$SRC" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
  APP_ABI=arm64-v8a APP_PLATFORM=android-24 \
  "APP_CFLAGS=-O3 -DPKGNAME=com/hunter/app -DCLSNAME=TProxyService" \
  NDK_LIBS_OUT="$SRC/libs" NDK_OUT="$SRC/obj" -j"$(nproc)"
# (ndk-build wipes its libs dir, so build elsewhere then copy)
mkdir -p "$OUT/arm64-v8a" && cp "$SRC/libs/arm64-v8a/libhev-socks5-tunnel.so" "$OUT/arm64-v8a/"
echo "built $OUT/arm64-v8a/libhev-socks5-tunnel.so"
