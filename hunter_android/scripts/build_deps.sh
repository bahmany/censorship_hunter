#!/usr/bin/env bash
# Build static zlib + mbedTLS + libcurl for Android (default arm64-v8a) into
# .deps/install/<abi>. Pinned, SHA256-verified sources. Needs cmake, ninja/make, curl, tar, xz, bzip2.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ABI="${1:-arm64-v8a}"; API=24
PREFIX="$HERE/.deps/install/$ABI"; W="$HERE/.deps/work/$ABI"
[[ -f "$PREFIX/lib/libcurl.a" ]] && exit 0
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
[[ -z "$NDK" ]] && NDK="$(ls -d "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/nonexistent}}"/ndk/* 2>/dev/null | sort -V | tail -1)"
[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "NDK not found" >&2; exit 1; }
mkdir -p "$W" "$PREFIX"; cd "$W"
get() { [[ -f "$2" ]] || curl -fL --retry 3 -o "$2" "$1"; echo "$3  $2" | sha256sum -c - >/dev/null || { echo "bad sha: $2" >&2; exit 1; }; }
get https://zlib.net/fossils/zlib-1.3.1.tar.gz zlib.tgz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
get https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2 mbed.tbz 8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca
get https://curl.se/download/curl-8.11.1.tar.xz curl.txz c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56
tar xzf zlib.tgz; tar xjf mbed.tbz; tar xJf curl.txz
TC=(-GNinja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI"
    -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
cmake -S zlib-1.3.1 -B b-zlib "${TC[@]}" -DBUILD_SHARED_LIBS=OFF >/dev/null && cmake --build b-zlib --target install >/dev/null
rm -f "$PREFIX"/lib/libz.so*
cmake -S mbedtls-3.6.2 -B b-mbed "${TC[@]}" -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON >/dev/null
cmake --build b-mbed --target install >/dev/null
cmake -S curl-8.11.1 -B b-curl "${TC[@]}" -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_ZLIB=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
  -DUSE_NGHTTP2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DENABLE_ARES=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DZLIB_ROOT="$PREFIX" >/dev/null
cmake --build b-curl --target install >b-curl.log 2>&1 || { tail -30 b-curl.log >&2; exit 1; }
echo "deps installed in $PREFIX"
