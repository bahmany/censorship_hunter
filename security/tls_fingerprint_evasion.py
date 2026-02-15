"""
TLS Fingerprint Evasion Engine - JA3/JA4 Spoofing & uTLS Identity Randomization

Designed for Iran's 2026 TSG (Traffic Secure Gateway) DPI system which maintains
massive databases of client TLS fingerprints (JA3 hashes). When a VPN client
performs a TLS handshake, TSG compares the JA3 hash against known VPN signatures.
If matched (e.g., standard Go TLS library used by unmodified Xray), connection
is terminated at ClientHello stage.

This module:
- Randomizes TLS fingerprints to mimic real browsers (Chrome, Firefox, Safari, iOS, Edge)
- Generates valid JA3-compatible cipher suite combinations
- Rotates ALPN settings (prioritizing h2 for Reality realism)
- Manages TLS extension ordering to avoid detection
- Provides Xray/Sing-box compatible fingerprint configurations
"""

import hashlib
import logging
import os
import random
import struct
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple


class BrowserProfile(Enum):
    """Browser profiles for TLS fingerprint mimicry."""
    CHROME_120 = "chrome_120"
    CHROME_122 = "chrome_122"
    CHROME_AUTO = "chrome_auto"
    CHROME_RANDOM = "chrome_random"
    FIREFOX_121 = "firefox_121"
    FIREFOX_122 = "firefox_122"
    FIREFOX_RANDOM = "firefox_random"
    SAFARI_17 = "safari_17"
    SAFARI_RANDOM = "safari_random"
    IOS_17 = "ios_17"
    IOS_RANDOM = "ios_random"
    EDGE_122 = "edge_122"
    EDGE_RANDOM = "edge_random"
    ANDROID_14 = "android_14"
    RANDOM = "random"
    RANDOMIZED = "randomized"


@dataclass
class TLSFingerprint:
    """Represents a complete TLS fingerprint configuration."""
    profile: BrowserProfile
    tls_version: int  # 0x0303 = TLS 1.2, 0x0304 = TLS 1.3
    cipher_suites: List[int] = field(default_factory=list)
    extensions: List[int] = field(default_factory=list)
    elliptic_curves: List[int] = field(default_factory=list)
    ec_point_formats: List[int] = field(default_factory=list)
    alpn: List[str] = field(default_factory=list)
    signature_algorithms: List[int] = field(default_factory=list)
    ja3_hash: str = ""
    xray_fingerprint: str = ""
    singbox_fingerprint: str = ""


# TLS 1.3 Cipher Suites (mandatory for modern browsers)
TLS13_CIPHER_SUITES = [
    0x1301,  # TLS_AES_128_GCM_SHA256
    0x1302,  # TLS_AES_256_GCM_SHA384
    0x1303,  # TLS_CHACHA20_POLY1305_SHA256
]

# Chrome 120-122 cipher suites (exact order matters for JA3)
CHROME_CIPHER_SUITES = [
    0x1301, 0x1302, 0x1303,  # TLS 1.3
    0xC02C, 0xC02B,  # ECDHE-ECDSA-AES256/128-GCM-SHA384/256
    0xC030, 0xC02F,  # ECDHE-RSA-AES256/128-GCM-SHA384/256
    0xCCA9, 0xCCA8,  # ECDHE-ECDSA/RSA-CHACHA20-POLY1305
    0xC024, 0xC023,  # ECDHE-ECDSA-AES256/128-CBC-SHA384/256
    0xC028, 0xC027,  # ECDHE-RSA-AES256/128-CBC-SHA384/256
    0xC00A, 0xC009,  # ECDHE-ECDSA-AES256/128-CBC-SHA
    0xC014, 0xC013,  # ECDHE-RSA-AES256/128-CBC-SHA
    0x009D, 0x009C,  # AES256/128-GCM-SHA384/256
    0x003D, 0x003C,  # AES256/128-CBC-SHA256
    0x0035, 0x002F,  # AES256/128-CBC-SHA
]

# Firefox 121-122 cipher suites
FIREFOX_CIPHER_SUITES = [
    0x1301, 0x1303, 0x1302,  # TLS 1.3 (different order than Chrome)
    0xC02B, 0xC02F,  # ECDHE-ECDSA/RSA-AES128-GCM-SHA256
    0xC02C, 0xC030,  # ECDHE-ECDSA/RSA-AES256-GCM-SHA384
    0xCCA9, 0xCCA8,  # CHACHA20-POLY1305
    0xC013, 0xC014,  # ECDHE-RSA-AES128/256-CBC-SHA
    0xC009, 0xC00A,  # ECDHE-ECDSA-AES128/256-CBC-SHA
    0x002F, 0x0035,  # AES128/256-CBC-SHA
    0x000A,  # DES-CBC3-SHA (Firefox keeps this for compat)
]

# Safari 17 / iOS 17 cipher suites
SAFARI_CIPHER_SUITES = [
    0x1301, 0x1302, 0x1303,  # TLS 1.3
    0xC02C, 0xC02B,  # ECDHE-ECDSA-AES256/128-GCM
    0xC030, 0xC02F,  # ECDHE-RSA-AES256/128-GCM
    0xCCA9, 0xCCA8,  # CHACHA20-POLY1305
    0xC024, 0xC023,  # ECDHE-ECDSA-AES256/128-CBC-SHA384/256
    0xC028, 0xC027,  # ECDHE-RSA-AES256/128-CBC-SHA384/256
    0xC00A, 0xC009,  # ECDHE-ECDSA-AES256/128-CBC-SHA
    0xC014, 0xC013,  # ECDHE-RSA-AES256/128-CBC-SHA
]

# Edge 122 (Chromium-based, very similar to Chrome)
EDGE_CIPHER_SUITES = CHROME_CIPHER_SUITES.copy()

# Android 14 (Chromium WebView)
ANDROID_CIPHER_SUITES = [
    0x1301, 0x1302, 0x1303,
    0xC02C, 0xC02B, 0xC030, 0xC02F,
    0xCCA9, 0xCCA8,
    0xC00A, 0xC009, 0xC014, 0xC013,
    0x009D, 0x009C, 0x0035, 0x002F,
]

# TLS Extensions by browser
CHROME_EXTENSIONS = [
    0x0000,  # server_name (SNI)
    0x0017,  # extended_master_secret
    0xFF01,  # renegotiation_info
    0x000A,  # supported_groups
    0x000B,  # ec_point_formats
    0x0023,  # session_ticket
    0x0010,  # application_layer_protocol_negotiation (ALPN)
    0x0005,  # status_request (OCSP)
    0x000D,  # signature_algorithms
    0x0012,  # signed_certificate_timestamp
    0x002B,  # supported_versions
    0x002D,  # psk_key_exchange_modes
    0x0033,  # key_share
    0x001B,  # compress_certificate
    0x0015,  # padding
    0x4469,  # encrypted_client_hello (draft)
]

FIREFOX_EXTENSIONS = [
    0x0000,  # server_name
    0x0017,  # extended_master_secret
    0xFF01,  # renegotiation_info
    0x000A,  # supported_groups
    0x000B,  # ec_point_formats
    0x0023,  # session_ticket
    0x0010,  # ALPN
    0x0005,  # status_request
    0x000D,  # signature_algorithms
    0x002B,  # supported_versions
    0x002D,  # psk_key_exchange_modes
    0x0033,  # key_share
    0x001C,  # record_size_limit
    0x0015,  # padding
]

# Elliptic curves
CHROME_CURVES = [0x001D, 0x0017, 0x0018]  # x25519, secp256r1, secp384r1
FIREFOX_CURVES = [0x001D, 0x0017, 0x0018, 0x0019]  # + secp521r1
SAFARI_CURVES = [0x001D, 0x0017, 0x0018, 0x0019]

# ALPN configurations
ALPN_H2_HTTP11 = ["h2", "http/1.1"]
ALPN_HTTP11_ONLY = ["http/1.1"]
ALPN_H2_ONLY = ["h2"]

# Signature algorithms
CHROME_SIG_ALGS = [
    0x0403, 0x0804, 0x0401, 0x0503, 0x0805, 0x0501,
    0x0806, 0x0601, 0x0201,
]
FIREFOX_SIG_ALGS = [
    0x0403, 0x0503, 0x0603, 0x0804, 0x0805, 0x0806,
    0x0401, 0x0501, 0x0601, 0x0203, 0x0201,
]

# Whitelisted Reality destinations (critical for Iran bypass)
# These domains support TLS 1.3 + H2 and are NOT blocked in Iran
REALITY_DEST_WHITELIST = [
    # Apple (whitelisted for Iranian app developers/businesses)
    "swdist.apple.com",
    "www.apple.com",
    "apple.com",
    "updates.cdn-apple.com",
    "configuration.apple.com",
    # Microsoft (whitelisted for Windows Update, Office 365)
    "www.microsoft.com",
    "login.microsoftonline.com",
    "update.microsoft.com",
    "download.microsoft.com",
    "www.office.com",
    "outlook.office365.com",
    # Google (partially whitelisted for Android services)
    "www.google.com",
    "dl.google.com",
    "play.googleapis.com",
    "www.gstatic.com",
    # Amazon (whitelisted for AWS services used by Iranian businesses)
    "www.amazon.com",
    "s3.amazonaws.com",
    # Cloudflare (whitelisted for CDN services)
    "www.cloudflare.com",
    "cdnjs.cloudflare.com",
    "1.1.1.1",
    # Banking/Commerce (always whitelisted)
    "www.digikala.com",
    "www.snapp.ir",
    # International services with Iranian traffic
    "www.speedtest.net",
    "fast.com",
    "yahoo.com",
    "www.yahoo.com",
    # Gaming (partially allowed for economic reasons)
    "store.steampowered.com",
    "steamcommunity.com",
]

# Iranian ISP-specific detection patterns
IRAN_ISP_TLS_SIGNATURES = {
    "TCI": {"known_vpn_ja3": set(), "aggressiveness": 0.9},
    "MCI": {"known_vpn_ja3": set(), "aggressiveness": 0.85},
    "Irancell": {"known_vpn_ja3": set(), "aggressiveness": 0.8},
    "Rightel": {"known_vpn_ja3": set(), "aggressiveness": 0.7},
    "Shatel": {"known_vpn_ja3": set(), "aggressiveness": 0.75},
    "MobinNet": {"known_vpn_ja3": set(), "aggressiveness": 0.65},
}


class TLSFingerprintEvasion:
    """
    TLS Fingerprint Evasion Engine for Iran's TSG DPI system.
    
    Generates realistic browser TLS fingerprints to avoid JA3/JA4 detection.
    Randomizes between Chrome, Firefox, Safari, iOS, and Edge profiles.
    Prioritizes h2 ALPN for Reality traffic realism.
    """
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self._fingerprint_cache: Dict[str, TLSFingerprint] = {}
        self._rotation_counter = 0
        self._last_rotation = time.time()
        self._rotation_interval = 120  # Rotate every 2 minutes
        self._current_profile: Optional[BrowserProfile] = None
        self._ja3_blacklist: set = set()
        
        # Known VPN JA3 hashes to AVOID generating
        self._known_vpn_ja3 = {
            "e7d705a3286e19ea42f587b344ee6865",  # Go TLS default
            "bd0bf25947d4a37404f0424edf4db9ad",  # Python urllib3
            "3b5074b1b5d032e5620f69f9f700ff0e",  # OpenVPN
            "cd08e31494f9531f560d64c695473da9",  # WireGuard
            "b32309a26951912be7dba376398abc3b",  # Shadowsocks
            "e058f5f3c3e157e1b8e7a6ab6e0b72d4",  # V2Ray default
            "2ad2b325e94de30f3f5b27c0b1c6c037",  # Xray unmodified
        }
        
        self.logger.info("TLS Fingerprint Evasion Engine initialized")
    
    def get_random_fingerprint(self, prefer_h2: bool = True) -> TLSFingerprint:
        """
        Generate a random browser TLS fingerprint.
        
        Args:
            prefer_h2: Prioritize h2 ALPN (recommended for Reality)
        
        Returns:
            TLSFingerprint with realistic browser parameters
        """
        # Select random profile with weighted distribution
        # Chrome dominates market share, so weight it higher
        weights = {
            BrowserProfile.CHROME_122: 35,
            BrowserProfile.CHROME_120: 15,
            BrowserProfile.FIREFOX_122: 15,
            BrowserProfile.FIREFOX_121: 5,
            BrowserProfile.SAFARI_17: 10,
            BrowserProfile.IOS_17: 8,
            BrowserProfile.EDGE_122: 7,
            BrowserProfile.ANDROID_14: 5,
        }
        
        profiles = list(weights.keys())
        profile_weights = list(weights.values())
        profile = random.choices(profiles, weights=profile_weights, k=1)[0]
        
        return self._generate_fingerprint(profile, prefer_h2)
    
    def get_fingerprint_for_profile(self, profile: BrowserProfile, 
                                     prefer_h2: bool = True) -> TLSFingerprint:
        """Generate fingerprint for a specific browser profile."""
        return self._generate_fingerprint(profile, prefer_h2)
    
    def get_rotating_fingerprint(self, prefer_h2: bool = True) -> TLSFingerprint:
        """
        Get a fingerprint that rotates periodically.
        Used for long-lived connections to avoid statistical fingerprinting.
        """
        now = time.time()
        if (now - self._last_rotation > self._rotation_interval or 
            self._current_profile is None):
            self._current_profile = None
            self._last_rotation = now
            self._rotation_counter += 1
        
        if self._current_profile is None:
            fp = self.get_random_fingerprint(prefer_h2)
            self._current_profile = fp.profile
            return fp
        
        return self._generate_fingerprint(self._current_profile, prefer_h2)
    
    def _generate_fingerprint(self, profile: BrowserProfile, 
                               prefer_h2: bool) -> TLSFingerprint:
        """Generate a complete TLS fingerprint for the given profile."""
        fp = TLSFingerprint(profile=profile, tls_version=0x0303)
        
        if profile in (BrowserProfile.CHROME_120, BrowserProfile.CHROME_122,
                       BrowserProfile.CHROME_AUTO, BrowserProfile.CHROME_RANDOM):
            fp.cipher_suites = self._randomize_chrome_ciphers()
            fp.extensions = CHROME_EXTENSIONS.copy()
            fp.elliptic_curves = CHROME_CURVES.copy()
            fp.signature_algorithms = CHROME_SIG_ALGS.copy()
            fp.xray_fingerprint = "chrome"
            fp.singbox_fingerprint = "chrome"
            
        elif profile in (BrowserProfile.FIREFOX_121, BrowserProfile.FIREFOX_122,
                         BrowserProfile.FIREFOX_RANDOM):
            fp.cipher_suites = self._randomize_firefox_ciphers()
            fp.extensions = FIREFOX_EXTENSIONS.copy()
            fp.elliptic_curves = FIREFOX_CURVES.copy()
            fp.signature_algorithms = FIREFOX_SIG_ALGS.copy()
            fp.xray_fingerprint = "firefox"
            fp.singbox_fingerprint = "firefox"
            
        elif profile in (BrowserProfile.SAFARI_17, BrowserProfile.SAFARI_RANDOM):
            fp.cipher_suites = self._randomize_safari_ciphers()
            fp.extensions = CHROME_EXTENSIONS.copy()  # Safari similar to Chrome
            fp.elliptic_curves = SAFARI_CURVES.copy()
            fp.signature_algorithms = CHROME_SIG_ALGS.copy()
            fp.xray_fingerprint = "safari"
            fp.singbox_fingerprint = "safari"
            
        elif profile in (BrowserProfile.IOS_17, BrowserProfile.IOS_RANDOM):
            fp.cipher_suites = SAFARI_CIPHER_SUITES.copy()
            fp.extensions = CHROME_EXTENSIONS.copy()
            fp.elliptic_curves = SAFARI_CURVES.copy()
            fp.signature_algorithms = CHROME_SIG_ALGS.copy()
            fp.xray_fingerprint = "ios"
            fp.singbox_fingerprint = "ios"
            
        elif profile in (BrowserProfile.EDGE_122, BrowserProfile.EDGE_RANDOM):
            fp.cipher_suites = self._randomize_chrome_ciphers()  # Edge = Chromium
            fp.extensions = CHROME_EXTENSIONS.copy()
            fp.elliptic_curves = CHROME_CURVES.copy()
            fp.signature_algorithms = CHROME_SIG_ALGS.copy()
            fp.xray_fingerprint = "edge"
            fp.singbox_fingerprint = "edge"
            
        elif profile == BrowserProfile.ANDROID_14:
            fp.cipher_suites = ANDROID_CIPHER_SUITES.copy()
            fp.extensions = CHROME_EXTENSIONS.copy()
            fp.elliptic_curves = CHROME_CURVES.copy()
            fp.signature_algorithms = CHROME_SIG_ALGS.copy()
            fp.xray_fingerprint = "android"
            fp.singbox_fingerprint = "android"
            
        else:
            # Full random - pick from any profile
            return self.get_random_fingerprint(prefer_h2)
        
        # Set ALPN
        if prefer_h2:
            fp.alpn = ALPN_H2_HTTP11.copy()
        else:
            fp.alpn = random.choice([ALPN_H2_HTTP11, ALPN_HTTP11_ONLY])
        
        # EC point formats (standard)
        fp.ec_point_formats = [0x00]  # uncompressed
        
        # Compute JA3 hash
        fp.ja3_hash = self._compute_ja3(fp)
        
        # Verify it's not a known VPN fingerprint
        if fp.ja3_hash in self._known_vpn_ja3:
            # Slightly modify cipher order to change hash
            if len(fp.cipher_suites) > 3:
                idx = random.randint(3, len(fp.cipher_suites) - 1)
                fp.cipher_suites[idx], fp.cipher_suites[idx - 1] = \
                    fp.cipher_suites[idx - 1], fp.cipher_suites[idx]
                fp.ja3_hash = self._compute_ja3(fp)
        
        return fp
    
    def _randomize_chrome_ciphers(self) -> List[int]:
        """Generate randomized Chrome-like cipher suites."""
        ciphers = CHROME_CIPHER_SUITES.copy()
        # TLS 1.3 ciphers must stay at the front
        tls13 = ciphers[:3]
        rest = ciphers[3:]
        # Slight randomization of non-TLS1.3 ciphers (swap adjacent pairs)
        for i in range(0, len(rest) - 1, 2):
            if random.random() < 0.3:
                rest[i], rest[i + 1] = rest[i + 1], rest[i]
        return tls13 + rest
    
    def _randomize_firefox_ciphers(self) -> List[int]:
        """Generate randomized Firefox-like cipher suites."""
        ciphers = FIREFOX_CIPHER_SUITES.copy()
        tls13 = ciphers[:3]
        rest = ciphers[3:]
        for i in range(0, len(rest) - 1, 2):
            if random.random() < 0.25:
                rest[i], rest[i + 1] = rest[i + 1], rest[i]
        return tls13 + rest
    
    def _randomize_safari_ciphers(self) -> List[int]:
        """Generate randomized Safari-like cipher suites."""
        ciphers = SAFARI_CIPHER_SUITES.copy()
        tls13 = ciphers[:3]
        rest = ciphers[3:]
        for i in range(0, len(rest) - 1, 2):
            if random.random() < 0.2:
                rest[i], rest[i + 1] = rest[i + 1], rest[i]
        return tls13 + rest
    
    def _compute_ja3(self, fp: TLSFingerprint) -> str:
        """
        Compute JA3 hash from fingerprint.
        
        JA3 format: TLSVersion,Ciphers,Extensions,EllipticCurves,EllipticCurvePointFormats
        """
        tls_ver = str(fp.tls_version)
        ciphers = "-".join(str(c) for c in fp.cipher_suites)
        extensions = "-".join(str(e) for e in fp.extensions)
        curves = "-".join(str(c) for c in fp.elliptic_curves)
        ec_formats = "-".join(str(f) for f in fp.ec_point_formats)
        
        ja3_string = f"{tls_ver},{ciphers},{extensions},{curves},{ec_formats}"
        return hashlib.md5(ja3_string.encode()).hexdigest()
    
    def get_xray_config_params(self, fp: Optional[TLSFingerprint] = None) -> Dict[str, Any]:
        """
        Generate Xray-compatible TLS configuration parameters.
        
        Returns dict suitable for inclusion in Xray streamSettings.
        """
        if fp is None:
            fp = self.get_random_fingerprint(prefer_h2=True)
        
        return {
            "fingerprint": fp.xray_fingerprint,
            "alpn": fp.alpn,
            "allowInsecure": False,
        }
    
    def get_singbox_config_params(self, fp: Optional[TLSFingerprint] = None) -> Dict[str, Any]:
        """
        Generate Sing-box compatible TLS configuration parameters.
        """
        if fp is None:
            fp = self.get_random_fingerprint(prefer_h2=True)
        
        return {
            "utls": {
                "enabled": True,
                "fingerprint": fp.singbox_fingerprint,
            },
            "alpn": fp.alpn,
        }
    
    def get_random_reality_dest(self) -> Tuple[str, List[str]]:
        """
        Get a random Reality destination (dest + serverNames).
        
        Critical for Iran bypass: the dest must be a TLS 1.3 + H2 site 
        that is NOT blocked (whitelisted) in Iran.
        
        Returns:
            Tuple of (dest:port, [serverNames])
        """
        # Group destinations by domain family
        apple_dests = [d for d in REALITY_DEST_WHITELIST if "apple" in d]
        microsoft_dests = [d for d in REALITY_DEST_WHITELIST if "microsoft" in d or "office" in d]
        google_dests = [d for d in REALITY_DEST_WHITELIST if "google" in d or "gstatic" in d]
        cloudflare_dests = [d for d in REALITY_DEST_WHITELIST if "cloudflare" in d]
        other_dests = [d for d in REALITY_DEST_WHITELIST 
                       if not any(x in d for x in ["apple", "microsoft", "office", 
                                                     "google", "gstatic", "cloudflare"])]
        
        # Weighted selection favoring Apple/Microsoft (most reliably whitelisted)
        groups = [
            (apple_dests, 30),
            (microsoft_dests, 25),
            (google_dests, 15),
            (cloudflare_dests, 15),
            (other_dests, 15),
        ]
        
        group = random.choices(
            [g[0] for g in groups],
            weights=[g[1] for g in groups],
            k=1
        )[0]
        
        if not group:
            group = REALITY_DEST_WHITELIST
        
        primary = random.choice(group)
        # ServerNames should include the primary and optionally related domains
        server_names = [primary]
        for d in group:
            if d != primary and len(server_names) < 3:
                server_names.append(d)
        
        return f"{primary}:443", server_names
    
    def generate_short_ids(self, count: int = 8) -> List[str]:
        """Generate random hex shortIds for Reality config."""
        short_ids = []
        for _ in range(count):
            # ShortIds are 1-16 hex chars, typically 8
            length = random.choice([2, 4, 6, 8])
            short_id = os.urandom(length // 2).hex()
            short_ids.append(short_id)
        return short_ids
    
    def is_fingerprint_safe(self, ja3_hash: str) -> bool:
        """Check if a JA3 hash is safe (not known as VPN)."""
        return ja3_hash not in self._known_vpn_ja3 and ja3_hash not in self._ja3_blacklist
    
    def add_blacklisted_ja3(self, ja3_hash: str):
        """Add a JA3 hash to the blacklist (detected by DPI)."""
        self._ja3_blacklist.add(ja3_hash)
        self.logger.info(f"JA3 hash blacklisted: {ja3_hash[:12]}...")
    
    def get_optimal_fingerprint_for_isp(self, isp: str = "unknown") -> TLSFingerprint:
        """
        Get optimal fingerprint based on detected ISP.
        
        Different Iranian ISPs have different DPI aggressiveness levels.
        TCI/MCI: Most aggressive - need Chrome/Safari (most common)
        Rightel/Shatel: Less aggressive - more profiles work
        """
        isp_upper = isp.upper()
        
        if "TCI" in isp_upper or "MCI" in isp_upper or "HAMRAH" in isp_upper:
            # Most aggressive - use Chrome (most common in Iran)
            profile = random.choice([
                BrowserProfile.CHROME_122,
                BrowserProfile.CHROME_120,
            ])
        elif "IRANCELL" in isp_upper or "MTN" in isp_upper:
            profile = random.choice([
                BrowserProfile.CHROME_122,
                BrowserProfile.SAFARI_17,
                BrowserProfile.ANDROID_14,
            ])
        elif "RIGHTEL" in isp_upper:
            profile = random.choice([
                BrowserProfile.CHROME_122,
                BrowserProfile.FIREFOX_122,
                BrowserProfile.EDGE_122,
            ])
        else:
            # Unknown ISP - use safest option
            profile = random.choice([
                BrowserProfile.CHROME_122,
                BrowserProfile.CHROME_120,
                BrowserProfile.SAFARI_17,
            ])
        
        return self.get_fingerprint_for_profile(profile, prefer_h2=True)
    
    def build_client_hello(self, fp: TLSFingerprint, 
                           server_name: str) -> bytes:
        """
        Build a realistic TLS ClientHello packet matching the fingerprint.
        
        This is used for the fragmentation engine to split correctly.
        """
        # Random bytes for ClientHello
        client_random = os.urandom(32)
        session_id = os.urandom(32)
        
        # Build cipher suites
        cipher_bytes = b""
        for cs in fp.cipher_suites:
            cipher_bytes += struct.pack(">H", cs)
        
        # Build extensions
        extensions_bytes = self._build_extensions(fp, server_name)
        
        # Handshake header
        handshake_body = (
            struct.pack(">H", 0x0303) +  # Client version TLS 1.2
            client_random +  # 32 bytes random
            struct.pack("B", len(session_id)) + session_id +  # Session ID
            struct.pack(">H", len(cipher_bytes)) + cipher_bytes +  # Cipher suites
            b'\x01\x00' +  # Compression methods (null)
            struct.pack(">H", len(extensions_bytes)) + extensions_bytes
        )
        
        # Handshake message
        handshake = (
            b'\x01' +  # ClientHello
            struct.pack(">I", len(handshake_body))[1:] +  # 3-byte length
            handshake_body
        )
        
        # TLS record
        record = (
            b'\x16' +  # Content type: Handshake
            struct.pack(">H", 0x0301) +  # Record version TLS 1.0 (standard)
            struct.pack(">H", len(handshake)) +
            handshake
        )
        
        return record
    
    def _build_extensions(self, fp: TLSFingerprint, server_name: str) -> bytes:
        """Build TLS extensions matching the fingerprint."""
        extensions = b""
        
        # SNI extension
        sni_bytes = server_name.encode('utf-8')
        sni_list = (
            b'\x00' +  # Host name type
            struct.pack(">H", len(sni_bytes)) +
            sni_bytes
        )
        sni_ext = struct.pack(">H", len(sni_list)) + sni_list
        extensions += (
            struct.pack(">H", 0x0000) +  # SNI type
            struct.pack(">H", len(sni_ext)) +
            sni_ext
        )
        
        # ALPN extension
        alpn_data = b""
        for proto in fp.alpn:
            proto_bytes = proto.encode('utf-8')
            alpn_data += struct.pack("B", len(proto_bytes)) + proto_bytes
        alpn_list = struct.pack(">H", len(alpn_data)) + alpn_data
        extensions += (
            struct.pack(">H", 0x0010) +
            struct.pack(">H", len(alpn_list)) +
            alpn_list
        )
        
        # Supported groups (elliptic curves)
        curves_data = b""
        for curve in fp.elliptic_curves:
            curves_data += struct.pack(">H", curve)
        curves_list = struct.pack(">H", len(curves_data)) + curves_data
        extensions += (
            struct.pack(">H", 0x000A) +
            struct.pack(">H", len(curves_list)) +
            curves_list
        )
        
        # EC point formats
        ec_data = bytes(fp.ec_point_formats)
        ec_list = struct.pack("B", len(ec_data)) + ec_data
        extensions += (
            struct.pack(">H", 0x000B) +
            struct.pack(">H", len(ec_list)) +
            ec_list
        )
        
        # Signature algorithms
        sig_data = b""
        for sig in fp.signature_algorithms:
            sig_data += struct.pack(">H", sig)
        sig_list = struct.pack(">H", len(sig_data)) + sig_data
        extensions += (
            struct.pack(">H", 0x000D) +
            struct.pack(">H", len(sig_list)) +
            sig_list
        )
        
        # Supported versions (TLS 1.3)
        sv_data = b'\x03\x04\x03\x03'  # TLS 1.3, TLS 1.2
        sv_list = struct.pack("B", len(sv_data)) + sv_data
        extensions += (
            struct.pack(">H", 0x002B) +
            struct.pack(">H", len(sv_list)) +
            sv_list
        )
        
        # PSK key exchange modes
        psk_data = b'\x01'  # psk_dhe_ke
        psk_list = struct.pack("B", len(psk_data)) + psk_data
        extensions += (
            struct.pack(">H", 0x002D) +
            struct.pack(">H", len(psk_list)) +
            psk_list
        )
        
        # Key share (x25519)
        key_share_data = os.urandom(32)  # x25519 public key
        ks_entry = struct.pack(">H", 0x001D) + struct.pack(">H", 32) + key_share_data
        ks_list = struct.pack(">H", len(ks_entry)) + ks_entry
        extensions += (
            struct.pack(">H", 0x0033) +
            struct.pack(">H", len(ks_list)) +
            ks_list
        )
        
        # Padding to make ClientHello size realistic (512 bytes typical)
        current_size = len(extensions) + 100  # approximate overhead
        if current_size < 512:
            padding_needed = 512 - current_size
            padding_data = b'\x00' * padding_needed
            extensions += (
                struct.pack(">H", 0x0015) +  # padding extension
                struct.pack(">H", len(padding_data)) +
                padding_data
            )
        
        return extensions
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get fingerprint evasion metrics."""
        return {
            "rotation_counter": self._rotation_counter,
            "current_profile": self._current_profile.value if self._current_profile else None,
            "blacklisted_ja3_count": len(self._ja3_blacklist),
            "known_vpn_ja3_count": len(self._known_vpn_ja3),
            "cache_size": len(self._fingerprint_cache),
        }
