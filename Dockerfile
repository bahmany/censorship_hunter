# Multi-stage Dockerfile for Hunter C++ Backend
FROM alpine:3.19 AS builder

# Install build dependencies
RUN apk add --no-cache \
    build-base \
    cmake \
    ninja \
    git \
    wget \
    curl-dev \
    openssl-dev \
    zlib-dev \
    pkgconfig \
    python3 \
    go \
    linux-headers \
    autoconf \
    automake \
    libevent-dev \
    tor

# Copy source code
WORKDIR /app
COPY hunter_cpp/ ./hunter_cpp/

# Build C++ backend
WORKDIR /app/hunter_cpp
RUN rm -rf build && mkdir -p build && cd build \
    && cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && ninja

# Build proxy engines (skip if network fails)
RUN mkdir -p /app/bin && python3 scripts/bootstrap_core_engines.py || echo "WARNING: proxy engines build skipped"

# Runtime stage
FROM alpine:3.19

RUN apk add --no-cache \
    libcurl \
    openssl \
    zlib \
    ca-certificates \
    netcat-openbsd \
    openssh-client

WORKDIR /app

# Copy binaries and dependencies
COPY --from=builder /app/hunter_cpp/build/hunter_backend /app/hunter_backend
COPY --from=builder /app/hunter_cpp/build/libhunter_core.a /app/
COPY --from=builder /app/bin/ /app/bin/

# Install runtime engines that may not have been built by bootstrap
RUN apk add --no-cache tor \
    && chmod +x /app/bin/* 2>/dev/null || true \
    && cp /usr/bin/tor /app/bin/tor 2>/dev/null || true

# Create runtime directories
RUN mkdir -p /app/runtime /app/config /app/logs /app/runtime/ssh

# Expose ports
# 7801: REST API / WebSocket control
# 7802: WebSocket monitor
# 7803: Main SOCKS5 proxy
# 7804: Gemini SOCKS5 proxy
# 7810-7899: Provisioned proxy ports
EXPOSE 7801 7802 7803 7804 7810-7820 3100-3120

# Set environment variables
ENV HUNTER_REST_API_PORT=7801
ENV HUNTER_WS_MONITOR_PORT=7802
ENV HUNTER_MULTIPROXY_PORT=7803
ENV HUNTER_GEMINI_PORT=7804
ENV HUNTER_XRAY_PATH=/app/bin/xray
ENV HUNTER_SINGBOX_PATH=/app/bin/sing-box
ENV HUNTER_MIHOMO_PATH=/app/bin/mihomo
ENV HUNTER_TOR_PATH=/app/bin/tor

CMD ["/app/hunter_backend"]
