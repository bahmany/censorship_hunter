"""
Active Probe Defense & Entropy Normalization Engine

From article Section 2.3.3 (Active Probing):
When the DPI system cannot definitively identify a connection, the firewall sends
fake requests (Probes) to the destination server. These probes try to connect using
known VPN protocols (VMess, Shadowsocks, etc.). If the destination server responds
validly to any probe, its IP is immediately and permanently blacklisted.

From article Section 2.3.2 (Heuristic Entropy Analysis):
Encrypted traffic has high entropy (randomness). However, normal HTTPS has specific
patterns in headers and packet sizes. Circumvention tools hiding traffic inside 
tunnels may create abnormal entropy distributions. TSG sensors measure entropy to
identify non-standard protocols.

This module implements:
- Active probe detection and deflection
- Transparent proxy fallback (serve real website content to probes)
- Protocol signature detection (identify DPI probe patterns)
- Entropy normalization to match standard HTTPS distributions
- Traffic padding to normalize packet size patterns
- Connection behavior mimicry (symmetric ratio detection avoidance)
"""

import hashlib
import logging
import math
import os
import random
import socket
import struct
import time
import threading
from collections import Counter, deque
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Deque, Dict, List, Optional, Set, Tuple


class ProbeType(Enum):
    """Known DPI probe types used by Iranian filtering system."""
    VMESS_PROBE = "vmess"
    VLESS_PROBE = "vless"
    SHADOWSOCKS_PROBE = "shadowsocks"
    TROJAN_PROBE = "trojan"
    WIREGUARD_PROBE = "wireguard"
    OPENVPN_PROBE = "openvpn"
    TOR_PROBE = "tor"
    HTTP_PROBE = "http"
    UNKNOWN = "unknown"


@dataclass
class ProbeSignature:
    """Signature of a known DPI probe."""
    probe_type: ProbeType
    initial_bytes: bytes     # First N bytes pattern
    min_length: int = 0
    max_length: int = 0
    description: str = ""


@dataclass
class ProbeDefenseMetrics:
    """Metrics for active probe defense."""
    total_connections: int = 0
    probes_detected: int = 0
    probes_deflected: int = 0
    legitimate_connections: int = 0
    entropy_normalizations: int = 0
    padding_applied: int = 0
    probe_types_seen: Dict[str, int] = field(default_factory=dict)


# Known probe signatures
# These are the initial bytes that Iranian DPI probes send
KNOWN_PROBE_SIGNATURES = [
    # VMess probe: starts with auth byte pattern
    ProbeSignature(
        probe_type=ProbeType.VMESS_PROBE,
        initial_bytes=b'\x01',  # VMess version byte
        min_length=16, max_length=64,
        description="VMess authentication probe"
    ),
    # Shadowsocks probe: random-looking but specific length
    ProbeSignature(
        probe_type=ProbeType.SHADOWSOCKS_PROBE,
        initial_bytes=b'',  # No fixed prefix, detected by behavior
        min_length=7, max_length=300,
        description="Shadowsocks AEAD probe"
    ),
    # OpenVPN probe
    ProbeSignature(
        probe_type=ProbeType.OPENVPN_PROBE,
        initial_bytes=b'\x38',  # OpenVPN control channel
        min_length=14, max_length=100,
        description="OpenVPN control packet probe"
    ),
    # WireGuard probe
    ProbeSignature(
        probe_type=ProbeType.WIREGUARD_PROBE,
        initial_bytes=b'\x01\x00\x00\x00',  # WireGuard handshake init
        min_length=148, max_length=148,
        description="WireGuard handshake initiation"
    ),
    # Tor probe
    ProbeSignature(
        probe_type=ProbeType.TOR_PROBE,
        initial_bytes=b'\x00\x00',  # Tor VERSIONS cell
        min_length=7, max_length=512,
        description="Tor VERSIONS cell probe"
    ),
    # HTTP CONNECT probe (checks for proxy)
    ProbeSignature(
        probe_type=ProbeType.HTTP_PROBE,
        initial_bytes=b'CONNECT ',
        min_length=20, max_length=1000,
        description="HTTP CONNECT proxy probe"
    ),
    # HTTP GET probe (checks for web panel)
    ProbeSignature(
        probe_type=ProbeType.HTTP_PROBE,
        initial_bytes=b'GET / ',
        min_length=10, max_length=2000,
        description="HTTP GET web panel probe"
    ),
]

# Known Iranian probe source IP ranges (periodically updated)
KNOWN_PROBE_IP_RANGES = [
    # TIC (Telecommunication Infrastructure Company) ranges
    "10.202.",  # Internal TIC
    "10.201.",  # Internal TIC
    # Known Iranian datacenter IPs used for probing
    "2.176.", "2.177.", "2.178.",  # TCI ranges
    "5.200.", "5.201.", "5.202.",  # IRANCELL
    "31.24.232.", "31.24.233.",   # Shatel
    "37.191.", "37.192.",         # MCI
    "46.32.", "46.33.",           # ADSL providers
    "78.38.", "78.39.",           # Pishgaman
    "85.185.", "85.186.",         # IRC ranges
]


class ActiveProbeDefender:
    """
    Active Probe Defense System.
    
    Detects and deflects DPI active probes to prevent server IP blacklisting.
    When a probe is detected, the server serves legitimate website content
    instead of VPN protocol responses.
    """
    
    def __init__(self, fallback_site: str = "www.apple.com"):
        self.logger = logging.getLogger(__name__)
        self.metrics = ProbeDefenseMetrics()
        self._lock = threading.Lock()
        self.fallback_site = fallback_site
        
        # Track connection patterns
        self._connection_history: Deque[Dict] = deque(maxlen=1000)
        self._ip_reputation: Dict[str, float] = {}  # IP -> suspicion score
        self._blacklisted_ips: Set[str] = set()
        
        # Timing analysis
        self._connection_timing: Dict[str, List[float]] = {}
        
        self.logger.info("Active Probe Defender initialized")
    
    def analyze_connection(self, client_ip: str, initial_data: bytes,
                           connection_time: float = 0.0) -> Tuple[bool, ProbeType]:
        """
        Analyze an incoming connection to determine if it's a DPI probe.
        
        Returns:
            Tuple of (is_probe, probe_type)
        """
        with self._lock:
            self.metrics.total_connections += 1
        
        # Check 1: Known probe IP ranges
        if self._is_known_probe_ip(client_ip):
            with self._lock:
                self.metrics.probes_detected += 1
                self.metrics.probe_types_seen["ip_based"] = \
                    self.metrics.probe_types_seen.get("ip_based", 0) + 1
            return True, ProbeType.UNKNOWN
        
        # Check 2: Signature matching
        probe_type = self._match_probe_signature(initial_data)
        if probe_type != ProbeType.UNKNOWN:
            with self._lock:
                self.metrics.probes_detected += 1
                key = probe_type.value
                self.metrics.probe_types_seen[key] = \
                    self.metrics.probe_types_seen.get(key, 0) + 1
            return True, probe_type
        
        # Check 3: Behavioral analysis
        if self._is_suspicious_behavior(client_ip, initial_data, connection_time):
            with self._lock:
                self.metrics.probes_detected += 1
                self.metrics.probe_types_seen["behavioral"] = \
                    self.metrics.probe_types_seen.get("behavioral", 0) + 1
            return True, ProbeType.UNKNOWN
        
        # Legitimate connection
        with self._lock:
            self.metrics.legitimate_connections += 1
        return False, ProbeType.UNKNOWN
    
    def _is_known_probe_ip(self, ip: str) -> bool:
        """Check if IP is from known probe ranges."""
        if ip in self._blacklisted_ips:
            return True
        
        for prefix in KNOWN_PROBE_IP_RANGES:
            if ip.startswith(prefix):
                return True
        
        return False
    
    def _match_probe_signature(self, data: bytes) -> ProbeType:
        """Match incoming data against known probe signatures."""
        if not data:
            return ProbeType.UNKNOWN
        
        for sig in KNOWN_PROBE_SIGNATURES:
            # Check initial bytes match
            if sig.initial_bytes and not data.startswith(sig.initial_bytes):
                continue
            
            # Check length range
            if sig.min_length and len(data) < sig.min_length:
                continue
            if sig.max_length and len(data) > sig.max_length:
                continue
            
            # Special Shadowsocks detection: high entropy + specific lengths
            if sig.probe_type == ProbeType.SHADOWSOCKS_PROBE:
                if self._looks_like_ss_probe(data):
                    return ProbeType.SHADOWSOCKS_PROBE
                continue
            
            return sig.probe_type
        
        return ProbeType.UNKNOWN
    
    def _looks_like_ss_probe(self, data: bytes) -> bool:
        """Detect Shadowsocks probe by entropy and structure."""
        if len(data) < 7 or len(data) > 300:
            return False
        
        # Shadowsocks probes have very high entropy
        entropy = self._calculate_entropy(data)
        if entropy < 7.0:  # Near-maximum entropy
            return False
        
        # Probes often send exact method+password combinations
        # Check for common probe patterns
        return len(data) in (7, 32, 48, 64, 128, 256)  # Common SS probe sizes
    
    def _is_suspicious_behavior(self, client_ip: str, data: bytes, 
                                 conn_time: float) -> bool:
        """
        Detect probes by behavioral patterns.
        
        Iranian DPI probes exhibit:
        - Rapid sequential connections from same IP
        - Very short connection durations
        - Exact same payload sent to multiple servers
        - No follow-up data after initial bytes
        """
        # Track connection history for this IP
        if client_ip not in self._connection_timing:
            self._connection_timing[client_ip] = []
        
        now = time.time()
        self._connection_timing[client_ip].append(now)
        
        # Clean old entries
        self._connection_timing[client_ip] = [
            t for t in self._connection_timing[client_ip]
            if now - t < 60  # Last 60 seconds
        ]
        
        recent_count = len(self._connection_timing[client_ip])
        
        # Suspicious: many connections in short time
        if recent_count > 10:
            self.logger.debug(f"Suspicious: {client_ip} made {recent_count} connections in 60s")
            self._update_ip_reputation(client_ip, 0.3)
            return True
        
        # Suspicious: very small initial data (< 10 bytes)
        if 0 < len(data) < 10:
            self._update_ip_reputation(client_ip, 0.1)
        
        # Check accumulated suspicion score
        if self._ip_reputation.get(client_ip, 0) > 0.8:
            return True
        
        return False
    
    def _update_ip_reputation(self, ip: str, increase: float):
        """Update suspicion score for an IP."""
        current = self._ip_reputation.get(ip, 0.0)
        new_score = min(1.0, current + increase)
        self._ip_reputation[ip] = new_score
        
        if new_score > 0.9:
            self._blacklisted_ips.add(ip)
            self.logger.info(f"IP blacklisted as probe source: {ip}")
    
    def _calculate_entropy(self, data: bytes) -> float:
        """Calculate Shannon entropy of data."""
        if not data:
            return 0.0
        
        counter = Counter(data)
        length = len(data)
        entropy = 0.0
        
        for count in counter.values():
            probability = count / length
            if probability > 0:
                entropy -= probability * math.log2(probability)
        
        return entropy
    
    def generate_fallback_response(self, probe_type: ProbeType) -> bytes:
        """
        Generate a legitimate-looking response to deflect probes.
        
        When a probe is detected, respond as if this is a normal web server
        serving the fallback site's content.
        """
        with self._lock:
            self.metrics.probes_deflected += 1
        
        if probe_type in (ProbeType.HTTP_PROBE,):
            return self._generate_http_response()
        else:
            return self._generate_tls_alert()
    
    def _generate_http_response(self) -> bytes:
        """Generate a legitimate HTTP response."""
        body = f"""<!DOCTYPE html>
<html><head><title>{self.fallback_site}</title></head>
<body><h1>Welcome</h1><p>This server is operating normally.</p></body></html>"""
        
        response = (
            f"HTTP/1.1 200 OK\r\n"
            f"Server: nginx/1.24.0\r\n"
            f"Content-Type: text/html; charset=utf-8\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: close\r\n"
            f"\r\n"
            f"{body}"
        )
        return response.encode()
    
    def _generate_tls_alert(self) -> bytes:
        """Generate a TLS alert (connection refused) to deflect non-HTTP probes."""
        # TLS Alert: handshake_failure (40)
        return b'\x15\x03\x03\x00\x02\x02\x28'
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get probe defense metrics."""
        with self._lock:
            total = max(1, self.metrics.total_connections)
            return {
                "total_connections": self.metrics.total_connections,
                "probes_detected": self.metrics.probes_detected,
                "probes_deflected": self.metrics.probes_deflected,
                "legitimate_connections": self.metrics.legitimate_connections,
                "probe_rate": round(self.metrics.probes_detected / total * 100, 1),
                "probe_types_seen": dict(self.metrics.probe_types_seen),
                "blacklisted_ips": len(self._blacklisted_ips),
                "tracked_ips": len(self._ip_reputation),
            }


class EntropyNormalizer:
    """
    Entropy Normalization Engine.
    
    Defeats Iran's TSG heuristic entropy analysis (Section 2.3.2) by making
    encrypted tunnel traffic match the entropy distribution of standard HTTPS.
    
    Standard HTTPS characteristics:
    - Entropy ~7.2-7.8 (not maximum 8.0)
    - Specific packet size patterns (headers add structure)
    - Periodic low-entropy content (HTTP headers between encrypted data)
    - Asymmetric upload/download ratios
    """
    
    # Standard HTTPS entropy range
    HTTPS_ENTROPY_MIN = 7.0
    HTTPS_ENTROPY_MAX = 7.8
    HTTPS_ENTROPY_TARGET = 7.5
    
    # Standard HTTPS packet sizes
    HTTPS_COMMON_SIZES = [
        64, 128, 256, 512, 576, 1024, 1460, 1500,
        # HTTP header sizes
        200, 300, 400, 500,
        # TLS record sizes
        16384,  # Max TLS record
    ]
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self._normalization_count = 0
        self._padding_count = 0
        self._lock = threading.Lock()
        
        # Track traffic patterns
        self._upload_bytes = 0
        self._download_bytes = 0
        self._packet_sizes: Deque[int] = deque(maxlen=1000)
    
    def normalize_packet(self, data: bytes, direction: str = "upload") -> bytes:
        """
        Normalize a packet's entropy to match standard HTTPS.
        
        Adds structured padding to reduce pure randomness and match
        the entropy distribution of normal web traffic.
        """
        if not data:
            return data
        
        entropy = self._calculate_entropy(data)
        
        # If entropy is too high (looks like pure encrypted data)
        if entropy > self.HTTPS_ENTROPY_MAX:
            data = self._add_structure_padding(data)
            with self._lock:
                self._normalization_count += 1
        
        # Track traffic for ratio analysis
        with self._lock:
            if direction == "upload":
                self._upload_bytes += len(data)
            else:
                self._download_bytes += len(data)
            self._packet_sizes.append(len(data))
        
        return data
    
    def normalize_packet_size(self, data: bytes) -> bytes:
        """
        Pad packet to a common HTTPS packet size.
        
        Prevents statistical analysis of packet sizes which can
        reveal tunnel traffic patterns.
        """
        current_size = len(data)
        
        # Find next common size
        target_size = current_size
        for size in sorted(self.HTTPS_COMMON_SIZES):
            if size >= current_size:
                target_size = size
                break
        
        if target_size > current_size:
            # Add padding that looks like HTTP headers
            padding = self._generate_http_like_padding(target_size - current_size)
            data = data + padding
            with self._lock:
                self._padding_count += 1
        
        return data
    
    def generate_traffic_padding(self) -> bytes:
        """
        Generate background traffic padding.
        
        To defeat "proxy behavior" detection (Section 2.2):
        - Creates asymmetric upload/download ratios
        - Generates keep-alive like patterns
        - Mimics normal web browsing behavior
        """
        # Choose a common HTTP-like padding size
        size = random.choice([64, 128, 200, 256, 300, 400, 512])
        return self._generate_http_like_padding(size)
    
    def get_upload_download_ratio(self) -> float:
        """
        Get current upload/download ratio.
        
        Symmetric ratios (close to 1.0) are flagged as suspicious tunnels.
        Normal web browsing: 0.05-0.3 (much more download than upload)
        """
        with self._lock:
            if self._download_bytes == 0:
                return 0.0
            return self._upload_bytes / max(1, self._download_bytes)
    
    def needs_ratio_correction(self) -> bool:
        """
        Check if upload/download ratio is suspicious.
        
        Returns True if ratio looks like a tunnel (too symmetric).
        """
        ratio = self.get_upload_download_ratio()
        # Normal web: 0.05-0.3, suspicious if > 0.5
        return ratio > 0.5
    
    def generate_ratio_correction_traffic(self, target_ratio: float = 0.15) -> int:
        """
        Calculate how many dummy download bytes to inject to fix ratio.
        
        Returns number of bytes of dummy download traffic needed.
        """
        with self._lock:
            if self._upload_bytes == 0:
                return 0
            
            current_ratio = self._upload_bytes / max(1, self._download_bytes)
            if current_ratio <= target_ratio:
                return 0
            
            # Calculate needed download bytes
            needed_download = self._upload_bytes / target_ratio
            extra_bytes = int(needed_download - self._download_bytes)
            return max(0, extra_bytes)
    
    def _add_structure_padding(self, data: bytes) -> bytes:
        """
        Add low-entropy structured data to reduce overall entropy.
        
        Mimics HTTP header patterns mixed into data.
        """
        # Create HTTP-header-like low-entropy padding
        padding_size = max(16, len(data) // 10)  # ~10% padding
        padding = self._generate_http_like_padding(padding_size)
        
        # Insert padding at intervals throughout the data
        # This creates a mixed entropy pattern like real HTTPS
        result = bytearray()
        chunk_size = max(1, len(data) // 4)
        padding_chunk_size = max(1, len(padding) // 3)
        
        for i in range(0, len(data), chunk_size):
            result.extend(data[i:i + chunk_size])
            if i + chunk_size < len(data):
                p_start = (i // chunk_size) * padding_chunk_size
                p_end = p_start + padding_chunk_size
                if p_start < len(padding):
                    result.extend(padding[p_start:min(p_end, len(padding))])
        
        return bytes(result)
    
    def _generate_http_like_padding(self, size: int) -> bytes:
        """Generate padding that looks like HTTP headers (low entropy)."""
        # HTTP-like header strings
        headers = [
            b"Content-Type: text/html; charset=utf-8\r\n",
            b"Cache-Control: max-age=3600\r\n",
            b"Server: nginx/1.24.0\r\n",
            b"Accept-Encoding: gzip, deflate, br\r\n",
            b"Connection: keep-alive\r\n",
            b"X-Request-Id: ",
            b"Date: Mon, 15 Feb 2026 12:00:00 GMT\r\n",
            b"Content-Length: 0\r\n",
            b"Vary: Accept-Encoding\r\n",
            b"ETag: W/\"",
        ]
        
        result = bytearray()
        while len(result) < size:
            header = random.choice(headers)
            result.extend(header)
            # Add some random but printable characters
            if random.random() < 0.3:
                result.extend(os.urandom(4).hex().encode())
                result.extend(b"\r\n")
        
        return bytes(result[:size])
    
    def _calculate_entropy(self, data: bytes) -> float:
        """Calculate Shannon entropy."""
        if not data:
            return 0.0
        
        counter = Counter(data)
        length = len(data)
        entropy = 0.0
        
        for count in counter.values():
            probability = count / length
            if probability > 0:
                entropy -= probability * math.log2(probability)
        
        return entropy
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get entropy normalization metrics."""
        with self._lock:
            return {
                "normalizations": self._normalization_count,
                "padding_applied": self._padding_count,
                "upload_bytes": self._upload_bytes,
                "download_bytes": self._download_bytes,
                "upload_download_ratio": round(self.get_upload_download_ratio(), 3),
                "ratio_suspicious": self.needs_ratio_correction(),
                "tracked_packets": len(self._packet_sizes),
            }
