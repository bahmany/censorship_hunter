#!/usr/bin/env bash
# One-shot reproducible static Windows build (cross-compile from Linux, or MSYS2 with HUNTER_NATIVE_MINGW=1).
#   scripts/build_windows.sh [--no-engines] [--configs]
# Output: <repo>/bin/windows/hunter.exe  (single static exe; engines embedded by default)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"; ROOT="$(cd "$SRC/.." && pwd)"
export HUNTER_WIN_DEPS="${HUNTER_WIN_DEPS:-$ROOT/build/win-deps}"
BUILD="${HUNTER_WIN_BUILD:-$ROOT/build/win}"
ENG=ON; CFG=OFF
for a in "$@"; do case "$a" in --no-engines) ENG=OFF;; --configs) CFG=ON;; esac; done
[ -f "$SRC/third_party/imgui/imgui.cpp" ] || git -C "$ROOT" submodule update --init hunter_cpp/third_party/imgui
"$HERE/fetch_windows_deps.sh" "$HUNTER_WIN_DEPS"
TC=(); [ -n "${HUNTER_NATIVE_MINGW:-}" ] || TC=(-DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/mingw-w64-toolchain.cmake")
GEN=(); command -v ninja >/dev/null && GEN=(-G Ninja)
cmake -S "$SRC" -B "$BUILD" "${GEN[@]}" "${TC[@]}" -DCMAKE_BUILD_TYPE=Release \
  -DHUNTER_WIN_DEPS="$HUNTER_WIN_DEPS" -DHUNTER_EMBED_ENGINES=$ENG -DHUNTER_EMBED_CONFIGS=$CFG
cmake --build "$BUILD" -j"${JOBS:-$(nproc 2>/dev/null || echo 2)}"
echo "Built: $ROOT/bin/windows/hunter.exe"
