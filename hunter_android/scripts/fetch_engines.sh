#!/usr/bin/env bash
# Download pinned xray / sing-box (android arm64) and install them as
# app/src/main/jniLibs/arm64-v8a/lib{xray,singbox}.so so Android extracts them
# into nativeLibraryDir, the only app-reachable location where exec is allowed
# (W^X / SELinux blocks exec from filesDir on targetSdk >= 29).
# Hashes were computed from the official release assets at pin time.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$HERE/app/src/main/jniLibs/arm64-v8a"
XRAY_VERSION="v26.3.27"
XRAY_SHA256="57149ffd48b629c07bf76938e73ab2729fde5910091497eab3e93d1c190f4c1b"
SINGBOX_VERSION="v1.13.16"
SINGBOX_SHA256="0a5a2b11f7dd32c3584f3d2d0962c2a1f6e08fc0bcef13efdff0f6e7770f5194"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

fetch() { # url file sha
  curl -fL --retry 3 -o "$2" "$1"
  echo "$3  $2" | sha256sum -c - >/dev/null || { echo "SHA256 mismatch for $1" >&2; exit 1; }
}
if [[ ! -f "$OUT/libxray.so" ]]; then
  fetch "https://github.com/XTLS/Xray-core/releases/download/${XRAY_VERSION}/Xray-android-arm64-v8a.zip" "$TMP/x.zip" "$XRAY_SHA256"
  unzip -oq "$TMP/x.zip" xray -d "$TMP" && install -m 755 "$TMP/xray" "$OUT/libxray.so"
fi
if [[ ! -f "$OUT/libsingbox.so" ]]; then
  fetch "https://github.com/SagerNet/sing-box/releases/download/${SINGBOX_VERSION}/sing-box-${SINGBOX_VERSION#v}-android-arm64.tar.gz" "$TMP/s.tgz" "$SINGBOX_SHA256"
  tar xzf "$TMP/s.tgz" -C "$TMP"
  install -m 755 "$(find "$TMP" -name sing-box -type f | head -1)" "$OUT/libsingbox.so"
fi
echo "engines installed in $OUT"
