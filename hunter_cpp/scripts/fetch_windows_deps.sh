#!/usr/bin/env bash
# Build static Windows (MinGW-w64) dependencies: zlib, curl (Schannel TLS), GLFW.
# Pinned versions + SHA256; sources are verified before use. Idempotent.
#
#   scripts/fetch_windows_deps.sh [PREFIX]          (default: <repo>/build/win-deps)
# Cross (Linux):  needs x86_64-w64-mingw32-gcc (apt install mingw-w64), cmake, ninja|make, curl|wget, unzip, xz
# MSYS2 native:   run inside a MINGW64/UCRT64 shell (set HUNTER_NATIVE_MINGW=1).
# zstd: Hunter embeds the decompress-only amalgamation (third_party/zstd/zstddeclib.c),
#       so no separate libzstd is required.
set -euo pipefail

ZLIB_VER=1.3.1;   ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
CURL_VER=8.11.1;  CURL_URL="https://github.com/curl/curl/releases/download/curl-${CURL_VER//./_}/curl-${CURL_VER}.tar.xz"
CURL_SHA=c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
GLFW_VER=3.4;     GLFW_URL="https://github.com/glfw/glfw/releases/download/${GLFW_VER}/glfw-${GLFW_VER}.zip"
GLFW_SHA=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"  # repo root
PREFIX="${1:-${HUNTER_WIN_DEPS:-$ROOT/build/win-deps}}"
WORK="${HUNTER_DEPS_WORK:-$PREFIX/.work}"
TOOLCHAIN_ARGS=()
if [ -z "${HUNTER_NATIVE_MINGW:-}" ]; then
  TOOLCHAIN_ARGS=(-DCMAKE_TOOLCHAIN_FILE="$HERE/../cmake/mingw-w64-toolchain.cmake")
fi
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
mkdir -p "$PREFIX" "$WORK"; PREFIX="$(cd "$PREFIX" && pwd)"; WORK="$(cd "$WORK" && pwd)"

fetch() { # url sha file
  local f="$WORK/$3"
  if [ ! -f "$f" ] || ! echo "$2  $f" | sha256sum -c --status; then
    echo ">> download $1"
    if command -v curl >/dev/null; then curl -fsSL "$1" -o "$f"; else wget -q "$1" -O "$f"; fi
  fi
  echo "$2  $f" | sha256sum -c - || { echo "SHA256 MISMATCH for $3" >&2; rm -f "$f"; exit 1; }
}
common=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF "${TOOLCHAIN_ARGS[@]}" "${GEN[@]}")

# ---- zlib ----
if [ ! -f "$PREFIX/lib/libzlibstatic.a" ] && [ ! -f "$PREFIX/lib/libz.a" ]; then
  fetch "$ZLIB_URL" $ZLIB_SHA zlib.tar.gz
  rm -rf "$WORK/zlib-src"; mkdir -p "$WORK/zlib-src"; tar -xzf "$WORK/zlib.tar.gz" -C "$WORK/zlib-src" --strip-components=1
  cmake -S "$WORK/zlib-src" -B "$WORK/zlib-build" "${common[@]}" -DZLIB_BUILD_EXAMPLES=OFF -DINSTALL_PKGCONFIG_DIR="$PREFIX/lib/pkgconfig"
  cmake --build "$WORK/zlib-build" -j"$JOBS" && cmake --install "$WORK/zlib-build"
  # zlib's CMake names the static lib libzlibstatic.a on MinGW; provide libz.a too.
  [ -f "$PREFIX/lib/libz.a" ] || cp "$PREFIX/lib/libzlibstatic.a" "$PREFIX/lib/libz.a"
  rm -f "$PREFIX"/lib/libzlib.dll.a "$PREFIX"/bin/libzlib.dll "$PREFIX"/bin/zlib1.dll 2>/dev/null || true
fi
ZLIB_LIB="$PREFIX/lib/libz.a"

# ---- curl (Schannel, static, no optional deps) ----
if [ ! -f "$PREFIX/lib/libcurl.a" ]; then
  fetch "$CURL_URL" $CURL_SHA curl.tar.xz
  rm -rf "$WORK/curl-src"; mkdir -p "$WORK/curl-src"; tar -xJf "$WORK/curl.tar.xz" -C "$WORK/curl-src" --strip-components=1
  cmake -S "$WORK/curl-src" -B "$WORK/curl-build" "${common[@]}" \
    -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_STATIC_LIBS=ON -DENABLE_CURL_MANUAL=OFF \
    -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_WINDOWS_SSPI=ON \
    -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR="$PREFIX/include" -DZLIB_LIBRARY="$ZLIB_LIB" \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_LDAP=OFF -DCURL_DISABLE_LDAP=ON \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_DISABLE_INSTALL=OFF
  cmake --build "$WORK/curl-build" -j"$JOBS" && cmake --install "$WORK/curl-build"
fi

# ---- GLFW ----
if [ ! -f "$PREFIX/lib/libglfw3.a" ]; then
  fetch "$GLFW_URL" $GLFW_SHA glfw.zip
  rm -rf "$WORK/glfw-src"; mkdir -p "$WORK/glfw-src"; unzip -q "$WORK/glfw.zip" -d "$WORK/glfw-src"
  src="$(ls -d "$WORK"/glfw-src/glfw-*)"
  cmake -S "$src" -B "$WORK/glfw-build" "${common[@]}" -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF
  cmake --build "$WORK/glfw-build" -j"$JOBS" && cmake --install "$WORK/glfw-build"
fi

echo "OK: static Windows deps in $PREFIX"
