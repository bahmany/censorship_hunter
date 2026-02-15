"""
SplitHTTP / XHTTP Transport Layer Engine

From article Section 3.2 (SplitHTTP Transport - Xray v26):
SplitHTTP splits the data into small HTTP requests that look like normal web
browsing activity. Instead of a single persistent connection, data is sent as
many independent short-lived HTTP requests/responses mimicking adaptive bitrate
streaming (like YouTube HLS/DASH). This makes it nearly indistinguishable from
normal web traffic to DPI systems.

Key DPI evasion properties:
- Each request looks like an independent web fetch (no persistent tunnel)
- Chunk sizes match common web content sizes (images, JS, CSS)
- Request timing mimics real browsing (burst + idle patterns)
- HTTP/2 multiplexing hides the tunnel within normal web streams
- CDN-fronted: traffic goes through Cloudflare/Gcore making it whitelisted

This module implements:
- SplitHTTP transport configuration for Xray-core v26+
- Adaptive chunk sizing that mimics web content (images, scripts, video segments)
- Request timing patterns that mimic real browsing behavior
- CDN integration (Cloudflare Workers, Gcore, ArvanCloud)
- Background padding traffic to maintain browsing-like patterns
- Complete Xray/Sing-box config generation for SplitHTTP transport
"""

import hashlib
import json
import logging
import os
import random
import string
import time
import threading
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple


class ChunkProfile(Enum):
    """Content type profiles for chunk sizing."""
    HTML_PAGE = "html"          # 2-50 KB typical
    CSS_STYLESHEET = "css"     # 5-100 KB
    JAVASCRIPT = "js"          # 10-300 KB
    IMAGE_SMALL = "img_small"  # 1-50 KB (icons, thumbnails)
    IMAGE_LARGE = "img_large"  # 50-500 KB (photos)
    VIDEO_SEGMENT = "video"    # 500 KB - 4 MB (HLS/DASH segments)
    API_RESPONSE = "api"       # 100 B - 10 KB (JSON responses)
    FONT = "font"              # 20-200 KB (WOFF2 fonts)


class BrowsingPattern(Enum):
    """Simulated browsing behavior patterns."""
    PAGE_LOAD = "page_load"           # Initial burst (many parallel requests)
    SCROLLING = "scrolling"           # Periodic image loads
    VIDEO_STREAMING = "video"         # Regular interval large chunks
    IDLE_KEEPALIVE = "idle"           # Occasional small requests
    INTERACTIVE = "interactive"       # Mix of API calls and content loads


@dataclass
class SplitHTTPConfig:
    """Configuration for SplitHTTP transport."""
    enabled: bool = True
    
    # Path configuration
    path: str = ""                          # Auto-generated if empty
    host: str = ""                          # CDN host header
    
    # Chunk sizing (mimics web content)
    min_chunk_bytes: int = 100              # Minimum chunk size
    max_chunk_bytes: int = 1048576          # Maximum chunk size (1MB)
    
    # Timing (mimics browsing patterns)
    min_request_interval_ms: float = 5.0    # Min delay between requests
    max_request_interval_ms: float = 200.0  # Max delay between requests
    
    # HTTP/2 settings
    max_concurrent_streams: int = 100       # HTTP/2 max concurrent uploads
    max_upload_size: int = 1048576          # Max upload per request (1MB)
    
    # CDN configuration
    cdn_provider: str = "cloudflare"        # cloudflare, gcore, arvancloud
    use_early_data: bool = True             # 0-RTT for reduced latency
    
    # Padding
    enable_padding: bool = True
    padding_ratio: float = 0.1             # 10% padding traffic
    
    # Browsing pattern simulation
    browsing_pattern: BrowsingPattern = BrowsingPattern.VIDEO_STREAMING


# Content type size distributions (bytes)
CHUNK_SIZE_PROFILES = {
    ChunkProfile.HTML_PAGE: (2048, 51200),
    ChunkProfile.CSS_STYLESHEET: (5120, 102400),
    ChunkProfile.JAVASCRIPT: (10240, 307200),
    ChunkProfile.IMAGE_SMALL: (1024, 51200),
    ChunkProfile.IMAGE_LARGE: (51200, 524288),
    ChunkProfile.VIDEO_SEGMENT: (524288, 4194304),
    ChunkProfile.API_RESPONSE: (100, 10240),
    ChunkProfile.FONT: (20480, 204800),
}

# Browsing pattern timing distributions (ms between requests)
PATTERN_TIMING = {
    BrowsingPattern.PAGE_LOAD: {
        "burst_count": (5, 20),      # Parallel requests on page load
        "burst_interval_ms": (5, 50),
        "inter_page_ms": (2000, 10000),
        "chunk_profiles": [
            ChunkProfile.HTML_PAGE,
            ChunkProfile.CSS_STYLESHEET,
            ChunkProfile.JAVASCRIPT,
            ChunkProfile.IMAGE_SMALL,
            ChunkProfile.FONT,
        ],
    },
    BrowsingPattern.SCROLLING: {
        "burst_count": (1, 5),
        "burst_interval_ms": (100, 500),
        "inter_page_ms": (500, 3000),
        "chunk_profiles": [
            ChunkProfile.IMAGE_SMALL,
            ChunkProfile.IMAGE_LARGE,
            ChunkProfile.API_RESPONSE,
        ],
    },
    BrowsingPattern.VIDEO_STREAMING: {
        "burst_count": (1, 3),
        "burst_interval_ms": (10, 100),
        "inter_page_ms": (2000, 6000),  # Segment interval
        "chunk_profiles": [
            ChunkProfile.VIDEO_SEGMENT,
            ChunkProfile.API_RESPONSE,  # Manifest updates
        ],
    },
    BrowsingPattern.IDLE_KEEPALIVE: {
        "burst_count": (1, 1),
        "burst_interval_ms": (0, 0),
        "inter_page_ms": (10000, 30000),
        "chunk_profiles": [
            ChunkProfile.API_RESPONSE,
        ],
    },
    BrowsingPattern.INTERACTIVE: {
        "burst_count": (1, 8),
        "burst_interval_ms": (50, 300),
        "inter_page_ms": (500, 5000),
        "chunk_profiles": [
            ChunkProfile.API_RESPONSE,
            ChunkProfile.HTML_PAGE,
            ChunkProfile.JAVASCRIPT,
            ChunkProfile.IMAGE_SMALL,
        ],
    },
}

# Common web paths for realistic URL generation
WEB_PATHS = [
    "/assets/js/app", "/assets/css/style", "/assets/img/",
    "/api/v1/data", "/api/v2/stream", "/api/config",
    "/static/chunks/", "/static/media/", "/static/js/",
    "/cdn-cgi/", "/_next/data/", "/_next/static/",
    "/media/", "/images/", "/files/",
    "/video/hls/", "/video/dash/", "/manifest.json",
    "/sw.js", "/favicon.ico", "/robots.txt",
]

# CDN-specific configurations
CDN_CONFIGS = {
    "cloudflare": {
        "default_host": "your-worker.workers.dev",
        "max_upload": 104857600,  # 100MB free tier
        "supports_h2": True,
        "supports_early_data": True,
        "whitelisted_in_iran": True,
    },
    "gcore": {
        "default_host": "your-cdn.gcore.com",
        "max_upload": 52428800,
        "supports_h2": True,
        "supports_early_data": True,
        "whitelisted_in_iran": True,
    },
    "arvancloud": {
        "default_host": "your-site.arvancdn.ir",
        "max_upload": 52428800,
        "supports_h2": True,
        "supports_early_data": False,
        "whitelisted_in_iran": True,
    },
    "fastly": {
        "default_host": "your-site.fastly.net",
        "max_upload": 52428800,
        "supports_h2": True,
        "supports_early_data": True,
        "whitelisted_in_iran": False,
    },
}


class SplitHTTPTransport:
    """
    SplitHTTP/XHTTP Transport Engine.
    
    Generates configurations and manages the SplitHTTP transport layer
    that makes tunnel traffic look like normal web browsing.
    """
    
    def __init__(self, config: Optional[SplitHTTPConfig] = None):
        self.logger = logging.getLogger(__name__)
        self.config = config or SplitHTTPConfig()
        self._lock = threading.Lock()
        
        # Metrics
        self._total_chunks = 0
        self._total_bytes = 0
        self._pattern_switches = 0
        self._current_pattern = self.config.browsing_pattern
        
        # Auto-generate path if not set
        if not self.config.path:
            self.config.path = self._generate_realistic_path()
        
        self.logger.info(
            f"SplitHTTP transport initialized: path={self.config.path}, "
            f"cdn={self.config.cdn_provider}, pattern={self._current_pattern.value}"
        )
    
    def _generate_realistic_path(self) -> str:
        """Generate a realistic-looking URL path for the SplitHTTP endpoint."""
        base = random.choice(WEB_PATHS)
        # Add a random hash-like suffix (mimics webpack chunk names)
        suffix = hashlib.md5(os.urandom(16)).hexdigest()[:8]
        return f"{base}{suffix}"
    
    def get_chunk_size(self, profile: Optional[ChunkProfile] = None) -> int:
        """
        Get a realistic chunk size based on content type profile.
        
        Mimics real web content sizes to avoid statistical detection.
        """
        if profile is None:
            pattern = PATTERN_TIMING.get(self._current_pattern, {})
            profiles = pattern.get("chunk_profiles", [ChunkProfile.API_RESPONSE])
            profile = random.choice(profiles)
        
        min_size, max_size = CHUNK_SIZE_PROFILES.get(
            profile, (self.config.min_chunk_bytes, self.config.max_chunk_bytes)
        )
        
        # Use log-normal distribution for more realistic sizes
        # (most requests small, few large - matches real web traffic)
        import math
        mean = math.log(min_size + (max_size - min_size) / 3)
        sigma = 0.5
        size = int(random.lognormvariate(mean, sigma))
        return max(min_size, min(max_size, size))
    
    def get_request_interval(self) -> float:
        """
        Get the delay before the next request.
        
        Mimics browsing timing patterns to avoid periodic timing detection.
        """
        pattern = PATTERN_TIMING.get(self._current_pattern, {})
        min_ms = pattern.get("burst_interval_ms", (50, 200))[0]
        max_ms = pattern.get("burst_interval_ms", (50, 200))[1]
        
        # Add jitter with exponential distribution
        base_delay = random.uniform(min_ms, max_ms)
        jitter = random.expovariate(1.0 / max(1, base_delay * 0.2))
        
        return base_delay + jitter
    
    def get_burst_size(self) -> int:
        """Get the number of concurrent requests in a burst."""
        pattern = PATTERN_TIMING.get(self._current_pattern, {})
        min_burst, max_burst = pattern.get("burst_count", (1, 5))
        return random.randint(min_burst, max_burst)
    
    def switch_pattern(self, pattern: Optional[BrowsingPattern] = None):
        """
        Switch the active browsing pattern.
        
        This should be called periodically to make the traffic pattern
        more realistic (users switch between reading, scrolling, watching).
        """
        if pattern is None:
            # Weighted random selection (video streaming most common for VPN)
            weights = {
                BrowsingPattern.VIDEO_STREAMING: 0.4,
                BrowsingPattern.SCROLLING: 0.2,
                BrowsingPattern.PAGE_LOAD: 0.15,
                BrowsingPattern.INTERACTIVE: 0.15,
                BrowsingPattern.IDLE_KEEPALIVE: 0.1,
            }
            patterns = list(weights.keys())
            w = list(weights.values())
            pattern = random.choices(patterns, weights=w, k=1)[0]
        
        with self._lock:
            old = self._current_pattern
            self._current_pattern = pattern
            self._pattern_switches += 1
        
        self.logger.debug(f"Pattern switch: {old.value} -> {pattern.value}")
    
    def generate_xray_splithttp_outbound(
        self,
        server_address: str,
        server_port: int = 443,
        user_uuid: str = "",
        cdn_host: str = "",
        path: str = "",
        sni: str = "",
    ) -> Dict[str, Any]:
        """
        Generate Xray-core v26+ SplitHTTP outbound config.
        
        This is the recommended transport for blackout conditions in Iran.
        """
        if not user_uuid:
            import uuid
            user_uuid = str(uuid.uuid4())
        
        if not path:
            path = self.config.path
        
        if not cdn_host:
            cdn_host = CDN_CONFIGS.get(
                self.config.cdn_provider, {}
            ).get("default_host", server_address)
        
        if not sni:
            sni = cdn_host
        
        outbound = {
            "tag": "proxy-splithttp",
            "protocol": "vless",
            "settings": {
                "vnext": [{
                    "address": server_address,
                    "port": server_port,
                    "users": [{
                        "id": user_uuid,
                        "encryption": "none",
                    }]
                }]
            },
            "streamSettings": {
                "network": "splithttp",
                "security": "tls",
                "tlsSettings": {
                    "serverName": sni,
                    "fingerprint": random.choice([
                        "chrome", "firefox", "safari", "ios"
                    ]),
                    "alpn": ["h2", "http/1.1"],
                    "allowInsecure": False,
                },
                "splithttpSettings": {
                    "path": path,
                    "host": cdn_host,
                    "maxUploadSize": self.config.max_upload_size,
                    "maxConcurrentUploads": self.config.max_concurrent_streams,
                },
            },
        }
        
        return outbound
    
    def generate_xray_splithttp_inbound(
        self,
        listen_port: int = 443,
        user_uuid: str = "",
        path: str = "",
        cert_file: str = "/etc/ssl/certs/cert.pem",
        key_file: str = "/etc/ssl/private/key.pem",
    ) -> Dict[str, Any]:
        """Generate Xray-core SplitHTTP server inbound config."""
        if not user_uuid:
            import uuid
            user_uuid = str(uuid.uuid4())
        
        if not path:
            path = self.config.path
        
        inbound = {
            "tag": "splithttp-in",
            "listen": "0.0.0.0",
            "port": listen_port,
            "protocol": "vless",
            "settings": {
                "clients": [{
                    "id": user_uuid,
                    "email": "user@splithttp.local",
                }],
                "decryption": "none",
            },
            "streamSettings": {
                "network": "splithttp",
                "security": "tls",
                "tlsSettings": {
                    "certificates": [{
                        "certificateFile": cert_file,
                        "keyFile": key_file,
                    }],
                    "alpn": ["h2", "http/1.1"],
                },
                "splithttpSettings": {
                    "path": path,
                },
            },
        }
        
        return inbound
    
    def generate_singbox_splithttp_outbound(
        self,
        server_address: str,
        server_port: int = 443,
        user_uuid: str = "",
        cdn_host: str = "",
        path: str = "",
    ) -> Dict[str, Any]:
        """Generate Sing-box compatible SplitHTTP outbound."""
        if not user_uuid:
            import uuid
            user_uuid = str(uuid.uuid4())
        
        if not path:
            path = self.config.path
        
        if not cdn_host:
            cdn_host = server_address
        
        return {
            "type": "vless",
            "tag": "proxy-splithttp",
            "server": server_address,
            "server_port": server_port,
            "uuid": user_uuid,
            "transport": {
                "type": "httpupgrade",  # Sing-box uses httpupgrade for similar behavior
                "host": cdn_host,
                "path": path,
            },
            "tls": {
                "enabled": True,
                "server_name": cdn_host,
                "utls": {
                    "enabled": True,
                    "fingerprint": random.choice(["chrome", "firefox", "safari"]),
                },
                "alpn": ["h2", "http/1.1"],
            },
        }
    
    def generate_cloudflare_worker_script(self, upstream_address: str,
                                           upstream_port: int = 443) -> str:
        """
        Generate Cloudflare Worker script for SplitHTTP CDN fronting.
        
        This worker acts as a reverse proxy, making the tunnel traffic
        appear as legitimate CDN-served web content.
        """
        return f"""// Cloudflare Worker for SplitHTTP CDN Fronting
// Deploy to: your-worker.workers.dev
// Upstream: {upstream_address}:{upstream_port}

export default {{
  async fetch(request, env) {{
    const url = new URL(request.url);
    
    // Forward to upstream Xray server
    const upstream = new URL(request.url);
    upstream.hostname = '{upstream_address}';
    upstream.port = '{upstream_port}';
    upstream.protocol = 'https:';
    
    // Preserve headers for SplitHTTP compatibility
    const headers = new Headers(request.headers);
    headers.set('Host', upstream.hostname);
    
    // Add realistic response headers
    const response = await fetch(upstream.toString(), {{
      method: request.method,
      headers: headers,
      body: request.body,
      redirect: 'follow',
    }});
    
    // Clone response and add CDN headers
    const newResponse = new Response(response.body, response);
    newResponse.headers.set('Server', 'cloudflare');
    newResponse.headers.set('CF-Cache-Status', 'DYNAMIC');
    newResponse.headers.set('X-Content-Type-Options', 'nosniff');
    
    return newResponse;
  }},
}};
"""
    
    def generate_nginx_config(self, upstream_port: int = 8443,
                               path: str = "") -> str:
        """
        Generate Nginx reverse proxy config for SplitHTTP.
        
        Nginx sits in front of Xray, providing legitimate web server
        behavior and TLS termination.
        """
        if not path:
            path = self.config.path
        
        return f"""# Nginx config for SplitHTTP reverse proxy
# Place at: /etc/nginx/conf.d/splithttp.conf

server {{
    listen 443 ssl http2;
    server_name your-domain.com;
    
    ssl_certificate /etc/ssl/certs/cert.pem;
    ssl_certificate_key /etc/ssl/private/key.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_ciphers HIGH:!aNULL:!MD5;
    
    # SplitHTTP endpoint
    location {path} {{
        proxy_pass https://127.0.0.1:{upstream_port};
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        
        # Important: disable buffering for streaming
        proxy_buffering off;
        proxy_request_buffering off;
        
        # Timeouts for long-lived connections
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }}
    
    # Serve real website for non-SplitHTTP requests (anti-probe)
    location / {{
        root /var/www/html;
        index index.html;
        try_files $uri $uri/ =404;
    }}
    
    # Health check
    location /health {{
        return 200 'OK';
        add_header Content-Type text/plain;
    }}
}}
"""
    
    def generate_padding_headers(self) -> Dict[str, str]:
        """
        Generate realistic HTTP headers for padding requests.
        
        These headers make SplitHTTP requests look like real browser requests.
        """
        user_agents = [
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/121.0.0.0 Safari/537.36",
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
            "Mozilla/5.0 (iPhone; CPU iPhone OS 17_2 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Mobile/15E148 Safari/604.1",
        ]
        
        accept_types = [
            "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
            "application/json, text/plain, */*",
            "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8",
            "*/*",
        ]
        
        return {
            "User-Agent": random.choice(user_agents),
            "Accept": random.choice(accept_types),
            "Accept-Language": "en-US,en;q=0.9",
            "Accept-Encoding": "gzip, deflate, br",
            "Cache-Control": random.choice(["no-cache", "max-age=0", ""]),
            "Sec-Fetch-Dest": random.choice(["document", "script", "image", "empty"]),
            "Sec-Fetch-Mode": random.choice(["navigate", "cors", "no-cors"]),
            "Sec-Fetch-Site": random.choice(["same-origin", "cross-site", "none"]),
            "Sec-Ch-Ua": '"Not_A Brand";v="8", "Chromium";v="121", "Google Chrome";v="121"',
            "Sec-Ch-Ua-Mobile": "?0",
            "Sec-Ch-Ua-Platform": random.choice(['"Windows"', '"macOS"', '"Linux"']),
        }
    
    def get_optimal_settings_for_network(self, network_type: str) -> Dict[str, Any]:
        """
        Get optimal SplitHTTP settings based on network conditions.
        
        From article Table 2:
        - Blackout: fragment 100-200 bytes, SplitHTTP primary
        - Fiber: larger chunks allowed, higher concurrency
        - Mobile: smaller chunks, conservative concurrency
        """
        settings = {
            "fiber": {
                "max_upload_size": 1048576,      # 1MB
                "max_concurrent": 100,
                "pattern": BrowsingPattern.VIDEO_STREAMING,
                "chunk_range": (10240, 524288),  # 10KB-512KB
            },
            "mobile_4g": {
                "max_upload_size": 524288,        # 512KB
                "max_concurrent": 50,
                "pattern": BrowsingPattern.SCROLLING,
                "chunk_range": (5120, 102400),   # 5KB-100KB
            },
            "mobile_5g": {
                "max_upload_size": 1048576,       # 1MB
                "max_concurrent": 80,
                "pattern": BrowsingPattern.VIDEO_STREAMING,
                "chunk_range": (10240, 524288),  # 10KB-512KB
            },
            "blackout": {
                "max_upload_size": 204800,        # 200KB (conservative)
                "max_concurrent": 20,
                "pattern": BrowsingPattern.INTERACTIVE,
                "chunk_range": (100, 51200),     # 100B-50KB (small fragments)
            },
            "throttled": {
                "max_upload_size": 262144,        # 256KB
                "max_concurrent": 30,
                "pattern": BrowsingPattern.SCROLLING,
                "chunk_range": (2048, 102400),   # 2KB-100KB
            },
        }
        
        return settings.get(network_type, settings["fiber"])
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get SplitHTTP transport metrics."""
        with self._lock:
            return {
                "total_chunks": self._total_chunks,
                "total_bytes": self._total_bytes,
                "pattern_switches": self._pattern_switches,
                "current_pattern": self._current_pattern.value,
                "path": self.config.path,
                "cdn_provider": self.config.cdn_provider,
                "config": {
                    "max_upload_size": self.config.max_upload_size,
                    "max_concurrent_streams": self.config.max_concurrent_streams,
                    "padding_enabled": self.config.enable_padding,
                    "padding_ratio": self.config.padding_ratio,
                },
            }
