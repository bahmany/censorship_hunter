#!/bin/sh
export HUNTER_REST_API_PORT=7801
export HUNTER_WS_MONITOR_PORT=7802
export HUNTER_MULTIPROXY_PORT=7803
export HUNTER_GEMINI_PORT=7804
gdb -batch -ex "set pagination off" -ex run -ex bt -ex "info registers" /app/hunter_backend 2>&1 | tail -60
