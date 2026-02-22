"""
Security helpers (e.g., ADEE and stealth engines).
This module focuses on DPI-resistant features such as SNI rotation,
fragmentation helpers, and dynamic obfuscation.
"""

import random
import socket
import time
from typing import Dict

try:
    from hunter.core.utils import now_ts
except ImportError:
    from core.utils import now_ts

CDN_WHITELIST_DOMAINS = [
    "cloudflare.com", "cdn.cloudflare.com", "cloudflare-dns.com",
    "fastly.net", "fastly.com", "global.fastly.net",
    "akamai.net", "akamaiedge.net", "akamaihd.net",
    "azureedge.net", "azure.com", "microsoft.com",
    "amazonaws.com", "cloudfront.net", "awsglobalaccelerator.com",
    "googleusercontent.com", "googleapis.com", "gstatic.com",
    "workers.dev", "pages.dev", "vercel.app", "r2.dev", "arvan.run",
    "arvancdn.com"
]

IRAN_FRAGMENT_ENABLED = False


class AdversarialDPIExhaustionEngine:
    """Aggressive DPI exhaustion engine for Iran environment."""

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.running = False
        self.current_sni_index = 0
        self.sni_rotation_interval = 300
        self._stats = {
            "stress_packets_sent": 0,
            "fragmented_packets": 0,
            "sni_rotations": 0,
            "cache_miss_induced": 0,
            "start_time": 0,
        }
        self.cdn_whitelist = CDN_WHITELIST_DOMAINS.copy()
        self.last_sni_rotation = now_ts()
        self.ac_patterns = self._generate_ac_patterns()

    def _generate_ac_patterns(self):
        base = [i for i in range(256)]
        patterns = []
        for _ in range(3):
            patterns.append(bytes(random.choice(base) for _ in range(128)))
        return patterns

    def start(self):
        if not self.enabled or self.running:
            return
        self.running = True
        self._stats["start_time"] = now_ts()

    def stop(self):
        self.running = False

    def get_current_sni(self) -> str:
        if not self.cdn_whitelist:
            return "cloudflare.com"
        return self.cdn_whitelist[self.current_sni_index % len(self.cdn_whitelist)]

    def rotate_sni(self):
        self.current_sni_index = (self.current_sni_index + 1) % len(self.cdn_whitelist)
        self._stats["sni_rotations"] += 1
        self.last_sni_rotation = now_ts()
        return self.get_current_sni()

    def get_stats(self) -> Dict[str, int]:
        stats = self._stats.copy()
        stats["uptime"] = now_ts() - stats.get("start_time", now_ts()) if stats.get("start_time") else 0
        return stats

    def apply_obfuscation_to_config(self, outbound: Dict, current_sni: str) -> Dict:
        if not self.enabled:
            return outbound
        conf = outbound.copy()
        if "streamSettings" not in conf:
            return conf
        settings = conf["streamSettings"].copy()
        if "tlsSettings" in settings:
            tls = settings["tlsSettings"].copy()
            tls["serverName"] = current_sni
            settings["tlsSettings"] = tls
        if "wsSettings" in settings:
            ws = settings["wsSettings"].copy()
            headers = ws.get("headers", {}).copy()
            grpc_settings = ws.get("grpcSettings", {})
            grpc_settings["authority"] = current_sni
            ws["grpcSettings"] = grpc_settings
            headers["Host"] = current_sni
            ws["headers"] = headers
            settings["wsSettings"] = ws
        conf["streamSettings"] = settings
        return conf


class StealthObfuscationEngine:
    """Stealthy obfuscation that prioritizes deniable traffic."""

    def __init__(self, enabled: bool = True):
        self.enabled = enabled
        self.current_sni_index = 0
        self.cdn_whitelist = CDN_WHITELIST_DOMAINS[:8]
        self._stats = {"configs_obfuscated": 0, "sni_rotations": 0}

    def get_current_sni(self) -> str:
        if not self.cdn_whitelist:
            return "cdn.cloudflare.com"
        return self.cdn_whitelist[self.current_sni_index % len(self.cdn_whitelist)]

    def rotate_sni(self) -> str:
        self.current_sni_index = (self.current_sni_index + 1) % len(self.cdn_whitelist)
        self._stats["sni_rotations"] += 1
        return self.get_current_sni()

    def apply_obfuscation_to_config(self, outbound: Dict) -> Dict:
        if not self.enabled or "streamSettings" not in outbound:
            return outbound
        sni = self.get_current_sni()
        conf = outbound.copy()
        settings = conf["streamSettings"].copy()
        if "tlsSettings" in settings:
            tls = settings["tlsSettings"].copy()
            tls["serverName"] = sni
            settings["tlsSettings"] = tls
        if "grpcSettings" in settings:
            grpc = settings["grpcSettings"].copy()
            grpc["authority"] = sni
            settings["grpcSettings"] = grpc
        if "wsSettings" in settings:
            ws = settings["wsSettings"].copy()
            headers = ws.get("headers", {}).copy()
            headers["Host"] = sni
            ws["headers"] = headers
            settings["wsSettings"] = ws
        conf["streamSettings"] = settings
        self._stats["configs_obfuscated"] += 1
        return conf

    def get_stats(self) -> Dict[str, int]:
        return self._stats.copy()
