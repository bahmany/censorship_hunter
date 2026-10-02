#!/bin/bash
# build_apk.sh — Build the Hunter Android APK with embedded ARM64 engines
# and an embedded proxy-config bundle.
#
# This script:
#   1. Downloads ARM64 (arm64-v8a) xray and sing-box binaries from upstream
#   2. Installs them as jniLibs/lib*.so (exec from nativeLibraryDir)
#   3. Bundles a zstd-compressed proxy-config bundle into APK assets/configs.zst
#   4. Builds the release APK via Gradle
#   5. Copies the final APK to bin/android/hunter.apk
#
# Usage:
#   ./build_apk.sh                          # Build with debug keystore;
#                                           # reuse existing assets/configs.zst
#                                           # or download+compress configs fresh
#   ./build_apk.sh --configs-bundle <path>  # Use a pre-built config bundle
#                                           # (raw text is zstd-compressed,
#                                           # a .zst file is used as-is)
#   HUNTER_KEYSTORE=... ./build_apk.sh      # Build with custom release keystore
#
# Any other arguments are passed through to Gradle.
#
# Requirements:
#   - Android SDK (ANDROID_HOME set)
#   - NDK (auto-installed by Gradle)
#   - CMake 3.22.1 (auto-installed by Gradle)
#   - Java 17+
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ASSETS_DIR="$SCRIPT_DIR/app/src/main/assets/engines"
CONFIGS_ASSET="$SCRIPT_DIR/app/src/main/assets/configs.zst"
CONFIGS_BUNDLE=""
GRADLE_ARGS=()
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# ─── Argument parsing ───
while [[ $# -gt 0 ]]; do
    case "$1" in
        --configs-bundle)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "ERROR: --configs-bundle requires a path argument"
                exit 1
            fi
            CONFIGS_BUNDLE="$2"
            shift 2
            ;;
        *)
            GRADLE_ARGS+=("$1")
            shift
            ;;
    esac
done

echo "=== Hunter Android APK Build ==="
echo "Project: $PROJECT_ROOT"
echo "Assets:  $ASSETS_DIR"
echo ""

# ─── Step 1: engines + native deps (pinned, SHA256-verified) ───
# Engines ship as jniLibs/lib{xray,singbox}.so so they run from nativeLibraryDir.
mkdir -p "$ASSETS_DIR/.."
"$SCRIPT_DIR/scripts/fetch_engines.sh"
"$SCRIPT_DIR/scripts/build_deps.sh" arm64-v8a
"$SCRIPT_DIR/scripts/fetch_hev.sh"

# ─── Step 2: Bundle proxy configs ───
# Produce APK assets/configs.zst (zstd-compressed, one URI per line).
# Priority: an explicit --configs-bundle path > an existing asset (fast
# rebuild) > a fresh download via download_configs.py.
prepare_configs() {
    local src="$1"
    local tmp_zst="$TMP_DIR/configs.zst"

    if [[ -n "$src" ]]; then
        if [[ ! -f "$src" ]]; then
            echo "ERROR: --configs-bundle file not found: $src"
            exit 1
        fi
        echo "[3/5] Bundling configs from $src ..."
        if [[ "$src" == *.zst ]]; then
            cp "$src" "$tmp_zst"
        else
            if ! command -v zstd >/dev/null 2>&1; then
                echo "ERROR: 'zstd' not found — pass a pre-compressed .zst bundle instead"
                exit 1
            fi
            zstd -19 -f "$src" -o "$tmp_zst" >/dev/null
        fi
    elif [[ -f "$CONFIGS_ASSET" ]]; then
        echo "[3/5] configs.zst already present ($(du -h "$CONFIGS_ASSET" | cut -f1)) — keeping it"
        return 0
    else
        echo "[3/5] No config bundle supplied; downloading fresh configs..."
        local raw="$TMP_DIR/configs_bundle.txt"
        python3 "$PROJECT_ROOT/hunter_cpp/tools/download_configs.py" \
            --outfile "$raw" --timeout 30 --workers 16 --min-configs 1000
        if ! command -v zstd >/dev/null 2>&1; then
            echo "ERROR: 'zstd' not found, cannot compress config bundle"
            exit 1
        fi
        zstd -19 -f "$raw" -o "$tmp_zst" >/dev/null
    fi

    mkdir -p "$(dirname "$CONFIGS_ASSET")"
    cp "$tmp_zst" "$CONFIGS_ASSET"
    echo "      Done: $(du -h "$CONFIGS_ASSET" | cut -f1)"
}

prepare_configs "$CONFIGS_BUNDLE"

# ─── Step 4: Build the APK ───
echo "[4/5] Building APK with Gradle..."
cd "$SCRIPT_DIR"
./gradlew assembleRelease --no-daemon "${GRADLE_ARGS[@]}"
APK="$SCRIPT_DIR/app/build/outputs/apk/release/app-release.apk"

if [[ ! -f "$APK" ]]; then
    echo "ERROR: APK not found at $APK"
    exit 1
fi

# ─── Step 5: Copy to bin/android ───
echo "[5/5] Copying APK to bin/android/"
mkdir -p "$PROJECT_ROOT/bin/android"
cp "$APK" "$PROJECT_ROOT/bin/android/hunter.apk"

echo ""
echo "=== Build Complete ==="
echo "APK: $PROJECT_ROOT/bin/android/hunter.apk"
echo "Size: $(du -h "$PROJECT_ROOT/bin/android/hunter.apk" | cut -f1)"
echo ""
echo "To install on a phone:"
echo "  adb install bin/android/hunter.apk"
echo ""
echo "For distribution with a real signing key:"
echo "  keytool -genkeypair -v -keystore hunter.keystore -alias hunter -keyalg RSA -keysize 2048 -validity 10000"
echo "  HUNTER_KEYSTORE=hunter.keystore HUNTER_KEYSTORE_PASSWORD=... \\"
echo "  HUNTER_KEY_ALIAS=hunter HUNTER_KEY_PASSWORD=... ./build_apk.sh"
