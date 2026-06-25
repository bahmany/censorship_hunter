#!/bin/sh
# Dev entrypoint: incremental compile + run
# Only recompiles changed files, then starts the backend

set -e

cd /app/hunter_cpp

echo "[dev-entrypoint] Starting incremental build..."

# Incremental build - ninja only recompiles changed files
if [ ! -f /app/build/build.ninja ]; then
    echo "[dev-entrypoint] First build - running cmake..."
    cd /app/build
    cmake /app/hunter_cpp -G Ninja -DCMAKE_BUILD_TYPE=Release
    cd /app/hunter_cpp
else
    echo "[dev-entrypoint] Incremental build - ninja will skip unchanged files"
fi

echo "[dev-entrypoint] Running ninja..."
ninja -C /app/build hunter_backend 2>&1

# Copy fresh binary
cp /app/build/hunter_backend /app/hunter_backend
echo "[dev-entrypoint] Build complete. Starting backend..."

# Forward signals properly
trap 'kill -TERM $PID 2>/dev/null; wait $PID; exit 0' TERM INT

/app/hunter_backend &
PID=$!
echo "[dev-entrypoint] Backend started with PID $PID"

wait $PID
exit $?
