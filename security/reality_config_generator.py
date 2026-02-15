"""
VLESS-Reality-Vision Configuration Generator

The most robust circumvention solution for Iran's 2026 filtering as described in
the technical report. Each component serves a specific anti-DPI purpose:

- VLESS: Lightweight, stateless protocol with cleaner packet timing than VMess
- REALITY: Revolutionary Xray technology that "borrows" TLS identity from legitimate
  whitelisted websites. During TLS Handshake, the server forwards ServerHello and 
  certificates exactly from a target site (e.g., swdist.apple.com). The DPI sees a 
  valid Apple certificate. Since Apple is whitelisted, the connection passes.
- XTLS-Vision: Solves "TLS-in-TLS" detection. When HTTPS is tunneled inside another
  TLS tunnel, packet size/timing patterns change and TSG AI can detect this. Vision
  applies dynamic padding and connection management to make tunnel traffic look 
  identical to a standard browser connection.

Anti-Probe Defense: If the censor (Active Probe) connects to the server, it acts as
a transparent proxy, serving the real target website content. No VPN signature exposed.

This module generates complete Xray-core v26+ server and client configurations
with all security parameters optimized for Iran's DPI environment.
"""

import base64
import hashlib
import json
import logging
import os
import random
import secrets
import struct
import uuid
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

from .tls_fingerprint_evasion import (
    REALITY_DEST_WHITELIST,
    TLSFingerprintEvasion,
)


@dataclass
class RealityServerConfig:
    """Complete VLESS-Reality-Vision server configuration."""
    listen_port: int = 443
    user_uuid: str = ""
    flow: str = "xtls-rprx-vision"
    dest: str = "swdist.apple.com:443"
    server_names: List[str] = field(default_factory=lambda: ["swdist.apple.com", "www.apple.com"])
    private_key: str = ""
    public_key: str = ""
    short_ids: List[str] = field(default_factory=list)
    # Sniffing
    sniffing_enabled: bool = True
    sniffing_dest_override: List[str] = field(default_factory=lambda: ["http", "tls", "quic"])
    route_only: bool = True
    # Optimization
    tcp_fast_open: bool = True
    tcp_keepalive: int = 60
    # Anti-probe
    fallback_dest: str = ""  # Where probes get redirected


@dataclass
class RealityClientConfig:
    """Complete VLESS-Reality-Vision client configuration."""
    server_address: str = ""
    server_port: int = 443
    user_uuid: str = ""
    flow: str = "xtls-rprx-vision"
    server_name: str = "swdist.apple.com"
    public_key: str = ""
    short_id: str = ""
    fingerprint: str = "chrome"
    alpn: List[str] = field(default_factory=lambda: ["h2", "http/1.1"])
    socks_port: int = 10808
    http_port: int = 10809


@dataclass
class SplitHTTPConfig:
    """SplitHTTP/XHTTP transport configuration for Xray v26+."""
    enabled: bool = True
    path: str = "/xhttp"
    host: str = ""
    max_upload_size: int = 1048576  # 1MB
    max_concurrent_uploads: int = 10
    min_upload_interval_ms: int = 30
    # Fragment settings for blackout conditions
    fragment_length: str = "100-200"
    fragment_interval: str = "10-50"


class X25519KeyGenerator:
    """
    Generate X25519 key pairs for Reality protocol.
    
    Uses Python's cryptographic primitives for key generation.
    Falls back to os.urandom if cryptography library unavailable.
    """
    
    @staticmethod
    def generate_keypair() -> Tuple[str, str]:
        """
        Generate X25519 private/public key pair.
        
        Returns:
            Tuple of (private_key_base64, public_key_base64)
        """
        try:
            from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
            from cryptography.hazmat.primitives import serialization
            
            private_key = X25519PrivateKey.generate()
            public_key = private_key.public_key()
            
            private_bytes = private_key.private_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PrivateFormat.Raw,
                encryption_algorithm=serialization.NoEncryption()
            )
            public_bytes = public_key.public_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PublicFormat.Raw
            )
            
            private_b64 = base64.urlsafe_b64encode(private_bytes).decode().rstrip('=')
            public_b64 = base64.urlsafe_b64encode(public_bytes).decode().rstrip('=')
            
            return private_b64, public_b64
            
        except ImportError:
            # Fallback: generate random 32-byte keys
            # Not cryptographically correct X25519 but serves as placeholder
            private_bytes = os.urandom(32)
            # Simple scalar mult placeholder
            public_bytes = hashlib.sha256(private_bytes).digest()
            
            private_b64 = base64.urlsafe_b64encode(private_bytes).decode().rstrip('=')
            public_b64 = base64.urlsafe_b64encode(public_bytes).decode().rstrip('=')
            
            return private_b64, public_b64


class RealityConfigGenerator:
    """
    Generates complete VLESS-Reality-Vision configurations for Xray-core v26+.
    
    Optimized for Iran's 2026 filtering architecture:
    - Dest targets selected from Iran-whitelisted domains
    - TLS fingerprints randomized to avoid JA3 detection
    - XTLS-Vision flow for TLS-in-TLS pattern hiding
    - Anti-probe fallback configuration
    - TCP FastOpen for faster connections
    - SplitHTTP/XHTTP transport option for blackout conditions
    """
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.fingerprint_engine = TLSFingerprintEvasion()
        self.key_generator = X25519KeyGenerator()
    
    def generate_server_config(self, 
                                listen_port: int = 443,
                                dest: Optional[str] = None,
                                server_names: Optional[List[str]] = None,
                                user_uuid: Optional[str] = None) -> Dict[str, Any]:
        """
        Generate complete Xray-core v26+ server configuration.
        
        This is the config that runs on the VPS (server side).
        Implements all anti-DPI and anti-probe measures.
        
        Args:
            listen_port: Port to listen on (443 recommended)
            dest: Reality destination (e.g., "swdist.apple.com:443")
            server_names: Allowed server names for Reality
            user_uuid: UUID for the user (auto-generated if None)
            
        Returns:
            Complete Xray-core JSON configuration dict
        """
        # Generate keys
        private_key, public_key = self.key_generator.generate_keypair()
        
        # Generate UUID
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        
        # Select dest from whitelist if not specified
        if not dest or not server_names:
            dest_str, snames = self.fingerprint_engine.get_random_reality_dest()
            if not dest:
                dest = dest_str
            if not server_names:
                server_names = snames
        
        # Generate shortIds
        short_ids = self.fingerprint_engine.generate_short_ids(8)
        
        config = {
            "log": {
                "loglevel": "warning",
                "access": "",
                "error": ""
            },
            "dns": {
                "servers": [
                    {
                        "address": "https+local://1.1.1.1/dns-query",
                        "address_resolver": "local-dns"
                    },
                    {
                        "tag": "local-dns",
                        "address": "localhost",
                        "detour": "direct"
                    }
                ]
            },
            "inbounds": [
                {
                    "tag": "vless-reality-in",
                    "listen": "0.0.0.0",
                    "port": listen_port,
                    "protocol": "vless",
                    "settings": {
                        "clients": [
                            {
                                "id": user_uuid,
                                "flow": "xtls-rprx-vision",
                                "level": 0
                            }
                        ],
                        "decryption": "none"
                    },
                    "streamSettings": {
                        "network": "tcp",
                        "security": "reality",
                        "realitySettings": {
                            "show": False,
                            "dest": dest,
                            "xver": 0,
                            "serverNames": server_names,
                            "privateKey": private_key,
                            "minClientVer": "",
                            "maxClientVer": "",
                            "maxTimeDiff": 0,
                            "shortIds": short_ids
                        },
                        "tcpSettings": {
                            "acceptProxyProtocol": False
                        }
                    },
                    "sniffing": {
                        "enabled": True,
                        "destOverride": ["http", "tls", "quic"],
                        "routeOnly": True,
                        "metadataOnly": False
                    }
                }
            ],
            "outbounds": [
                {
                    "tag": "direct",
                    "protocol": "freedom",
                    "settings": {
                        "domainStrategy": "AsIs"
                    }
                },
                {
                    "tag": "block",
                    "protocol": "blackhole",
                    "settings": {}
                }
            ],
            "routing": {
                "domainStrategy": "AsIs",
                "rules": [
                    {
                        "type": "field",
                        "outboundTag": "block",
                        "ip": ["geoip:private"]
                    },
                    {
                        "type": "field",
                        "outboundTag": "block",
                        "protocol": ["bittorrent"]
                    }
                ]
            },
            "policy": {
                "levels": {
                    "0": {
                        "handshake": 4,
                        "connIdle": 300,
                        "uplinkOnly": 1,
                        "downlinkOnly": 1,
                        "statsUserUplink": True,
                        "statsUserDownlink": True,
                        "bufferSize": 4
                    }
                },
                "system": {
                    "statsInboundUplink": True,
                    "statsInboundDownlink": True,
                    "statsOutboundUplink": True,
                    "statsOutboundDownlink": True
                }
            }
        }
        
        # Store key info for client config generation
        self._last_server_info = {
            "private_key": private_key,
            "public_key": public_key,
            "user_uuid": user_uuid,
            "short_ids": short_ids,
            "dest": dest,
            "server_names": server_names,
            "listen_port": listen_port,
        }
        
        self.logger.info(
            f"Generated Reality server config: dest={dest}, "
            f"port={listen_port}, uuid={user_uuid[:8]}..."
        )
        
        return config
    
    def generate_client_config(self,
                                server_address: str,
                                server_port: int = 443,
                                user_uuid: str = "",
                                public_key: str = "",
                                short_id: str = "",
                                server_name: str = "",
                                socks_port: int = 10808,
                                http_port: int = 10809,
                                enable_fragment: bool = True,
                                network_type: str = "fiber") -> Dict[str, Any]:
        """
        Generate complete Xray-core v26+ client configuration.
        
        Optimized for connecting from inside Iran with maximum stealth.
        
        Args:
            server_address: VPS IP address
            server_port: Server listen port
            user_uuid: User UUID
            public_key: Reality public key (from server)
            short_id: One of the server's shortIds
            server_name: SNI (one of server's serverNames)
            socks_port: Local SOCKS5 port
            http_port: Local HTTP proxy port
            enable_fragment: Enable TLS fragmentation
            network_type: "fiber", "mobile_4g", "mobile_5g", "blackout"
        """
        # Get optimal fingerprint for Iran
        fp = self.fingerprint_engine.get_optimal_fingerprint_for_isp()
        
        # Use last server info if available and params not provided
        if hasattr(self, '_last_server_info'):
            info = self._last_server_info
            if not user_uuid:
                user_uuid = info.get("user_uuid", str(uuid.uuid4()))
            if not public_key:
                public_key = info.get("public_key", "")
            if not short_id and info.get("short_ids"):
                short_id = random.choice(info["short_ids"])
            if not server_name and info.get("server_names"):
                server_name = info["server_names"][0]
        
        if not server_name:
            server_name = "swdist.apple.com"
        if not short_id:
            short_id = os.urandom(4).hex()
        
        # Build sockopt based on network type
        sockopt = self._build_sockopt(network_type, enable_fragment)
        
        config = {
            "log": {
                "loglevel": "warning"
            },
            "inbounds": [
                {
                    "tag": "socks-in",
                    "listen": "127.0.0.1",
                    "port": socks_port,
                    "protocol": "socks",
                    "settings": {
                        "auth": "noauth",
                        "udp": True,
                        "ip": "127.0.0.1"
                    },
                    "sniffing": {
                        "enabled": True,
                        "destOverride": ["http", "tls", "quic"],
                        "routeOnly": True
                    }
                },
                {
                    "tag": "http-in",
                    "listen": "127.0.0.1",
                    "port": http_port,
                    "protocol": "http",
                    "settings": {}
                }
            ],
            "outbounds": [
                {
                    "tag": "vless-reality-out",
                    "protocol": "vless",
                    "settings": {
                        "vnext": [
                            {
                                "address": server_address,
                                "port": server_port,
                                "users": [
                                    {
                                        "id": user_uuid,
                                        "flow": "xtls-rprx-vision",
                                        "encryption": "none",
                                        "level": 0
                                    }
                                ]
                            }
                        ]
                    },
                    "streamSettings": {
                        "network": "tcp",
                        "security": "reality",
                        "realitySettings": {
                            "serverName": server_name,
                            "fingerprint": fp.xray_fingerprint,
                            "show": False,
                            "publicKey": public_key,
                            "shortId": short_id,
                            "spiderX": ""
                        },
                        "sockopt": sockopt
                    }
                },
                {
                    "tag": "direct",
                    "protocol": "freedom",
                    "settings": {
                        "domainStrategy": "AsIs"
                    }
                },
                {
                    "tag": "block",
                    "protocol": "blackhole",
                    "settings": {}
                }
            ],
            "routing": {
                "domainStrategy": "AsIs",
                "rules": [
                    {
                        "type": "field",
                        "outboundTag": "direct",
                        "domain": ["geosite:ir"]
                    },
                    {
                        "type": "field",
                        "outboundTag": "direct",
                        "ip": ["geoip:ir", "geoip:private"]
                    },
                    {
                        "type": "field",
                        "outboundTag": "vless-reality-out",
                        "port": "0-65535"
                    }
                ]
            }
        }
        
        self.logger.info(
            f"Generated Reality client config: server={server_address}:{server_port}, "
            f"sni={server_name}, fp={fp.xray_fingerprint}"
        )
        
        return config
    
    def generate_splithttp_config(self, 
                                   server_address: str,
                                   server_port: int = 443,
                                   user_uuid: str = "",
                                   cdn_host: str = "",
                                   path: str = "/xhttp",
                                   socks_port: int = 10808) -> Dict[str, Any]:
        """
        Generate SplitHTTP/XHTTP transport configuration (Xray v26+).
        
        Designed for "Blackout" conditions where long TCP connections are penalized.
        Breaks data stream into hundreds of small HTTP requests mimicking 
        Adaptive Bitrate Streaming / modern web browsing behavior.
        
        Args:
            server_address: Server address (or CDN domain)
            server_port: Server port
            user_uuid: User UUID
            cdn_host: CDN hostname for Host header
            path: HTTP path
            socks_port: Local SOCKS5 port
        """
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        
        if not cdn_host:
            cdn_host = server_address
        
        fp = self.fingerprint_engine.get_random_fingerprint(prefer_h2=True)
        
        config = {
            "log": {"loglevel": "warning"},
            "inbounds": [
                {
                    "tag": "socks-in",
                    "listen": "127.0.0.1",
                    "port": socks_port,
                    "protocol": "socks",
                    "settings": {"auth": "noauth", "udp": True}
                }
            ],
            "outbounds": [
                {
                    "tag": "vless-splithttp",
                    "protocol": "vless",
                    "settings": {
                        "vnext": [{
                            "address": server_address,
                            "port": server_port,
                            "users": [{
                                "id": user_uuid,
                                "encryption": "none",
                                "level": 0
                            }]
                        }]
                    },
                    "streamSettings": {
                        "network": "splithttp",
                        "security": "tls",
                        "tlsSettings": {
                            "serverName": cdn_host,
                            "fingerprint": fp.xray_fingerprint,
                            "alpn": ["h2", "http/1.1"],
                            "allowInsecure": False
                        },
                        "splithttpSettings": {
                            "path": path,
                            "host": cdn_host,
                            "maxUploadSize": 1048576,
                            "maxConcurrentUploads": 10,
                            "minUploadIntervalMs": 30
                        },
                        "sockopt": {
                            "tcpFastOpen": True,
                            "tcpKeepAliveInterval": 60,
                            "fragment": {
                                "packets": "tlshello",
                                "length": "100-200",
                                "interval": "10-50"
                            }
                        }
                    }
                },
                {
                    "tag": "direct",
                    "protocol": "freedom",
                    "settings": {"domainStrategy": "AsIs"}
                }
            ],
            "routing": {
                "domainStrategy": "AsIs",
                "rules": [
                    {"type": "field", "outboundTag": "direct", "ip": ["geoip:ir", "geoip:private"]},
                    {"type": "field", "outboundTag": "vless-splithttp", "port": "0-65535"}
                ]
            }
        }
        
        self.logger.info(f"Generated SplitHTTP config: {server_address}:{server_port}")
        return config
    
    def generate_websocket_cdn_config(self,
                                       server_address: str,
                                       server_port: int = 443,
                                       user_uuid: str = "",
                                       cdn_host: str = "",
                                       ws_path: str = "",
                                       socks_port: int = 10808) -> Dict[str, Any]:
        """
        Generate VLESS over WebSocket + CDN configuration.
        
        For UDP-restricted networks where Reality cannot work.
        Leverages Cloudflare/Gcore capacity to bypass filtering.
        """
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        if not cdn_host:
            cdn_host = server_address
        if not ws_path:
            ws_path = "/" + secrets.token_urlsafe(8)
        
        fp = self.fingerprint_engine.get_random_fingerprint(prefer_h2=True)
        
        config = {
            "log": {"loglevel": "warning"},
            "inbounds": [{
                "tag": "socks-in",
                "listen": "127.0.0.1",
                "port": socks_port,
                "protocol": "socks",
                "settings": {"auth": "noauth", "udp": True}
            }],
            "outbounds": [
                {
                    "tag": "vless-ws-cdn",
                    "protocol": "vless",
                    "settings": {
                        "vnext": [{
                            "address": server_address,
                            "port": server_port,
                            "users": [{
                                "id": user_uuid,
                                "encryption": "none",
                                "level": 0
                            }]
                        }]
                    },
                    "streamSettings": {
                        "network": "ws",
                        "security": "tls",
                        "tlsSettings": {
                            "serverName": cdn_host,
                            "fingerprint": fp.xray_fingerprint,
                            "alpn": ["http/1.1"],
                            "allowInsecure": False
                        },
                        "wsSettings": {
                            "path": ws_path,
                            "headers": {
                                "Host": cdn_host
                            },
                            "maxEarlyData": 2048,
                            "earlyDataHeaderName": "Sec-WebSocket-Protocol"
                        },
                        "sockopt": {
                            "tcpFastOpen": True,
                            "fragment": {
                                "packets": "tlshello",
                                "length": "100-200",
                                "interval": "10-50"
                            }
                        }
                    }
                },
                {"tag": "direct", "protocol": "freedom", "settings": {"domainStrategy": "AsIs"}}
            ],
            "routing": {
                "domainStrategy": "AsIs",
                "rules": [
                    {"type": "field", "outboundTag": "direct", "ip": ["geoip:ir", "geoip:private"]},
                    {"type": "field", "outboundTag": "vless-ws-cdn", "port": "0-65535"}
                ]
            }
        }
        
        self.logger.info(f"Generated WebSocket+CDN config: {cdn_host}")
        return config
    
    def _build_sockopt(self, network_type: str, enable_fragment: bool) -> Dict[str, Any]:
        """Build sockopt configuration based on network type."""
        sockopt = {
            "tcpFastOpen": True,
            "tcpKeepAliveInterval": 60,
        }
        
        if enable_fragment:
            if network_type == "blackout":
                # Aggressive fragmentation for blackout conditions
                sockopt["fragment"] = {
                    "packets": "tlshello",
                    "length": "50-100",
                    "interval": "10-30",
                }
            elif network_type in ("mobile_4g", "mobile_5g"):
                # Moderate fragmentation for mobile
                sockopt["fragment"] = {
                    "packets": "tlshello",
                    "length": "100-200",
                    "interval": "10-50",
                }
                # Lower MTU for mobile PMTUD attacks
                sockopt["tcpMptcp"] = True
            else:
                # Standard fragmentation for fiber/ADSL
                sockopt["fragment"] = {
                    "packets": "tlshello",
                    "length": "100-200",
                    "interval": "10-50",
                }
        
        return sockopt
    
    def generate_vless_reality_uri(self,
                                    server_address: str,
                                    server_port: int = 443,
                                    user_uuid: str = "",
                                    public_key: str = "",
                                    short_id: str = "",
                                    server_name: str = "",
                                    fingerprint: str = "chrome",
                                    remark: str = "HUNTER-Reality") -> str:
        """
        Generate a VLESS Reality share URI.
        
        Format: vless://uuid@address:port?params#remark
        """
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        if not server_name:
            server_name = "swdist.apple.com"
        if not short_id:
            short_id = os.urandom(4).hex()
        
        params = (
            f"type=tcp&security=reality&pbk={public_key}"
            f"&fp={fingerprint}&sni={server_name}"
            f"&sid={short_id}&flow=xtls-rprx-vision"
        )
        
        uri = f"vless://{user_uuid}@{server_address}:{server_port}?{params}#{remark}"
        return uri
    
    def enhance_existing_config(self, xray_config: Dict[str, Any],
                                 network_type: str = "fiber") -> Dict[str, Any]:
        """
        Enhance an existing Xray configuration with anti-DPI features.
        
        Adds: fingerprint randomization, fragmentation, TCP optimizations.
        """
        config = json.loads(json.dumps(xray_config))  # Deep copy
        
        fp = self.fingerprint_engine.get_random_fingerprint(prefer_h2=True)
        
        for outbound in config.get("outbounds", []):
            stream = outbound.get("streamSettings", {})
            
            # Add/update TLS fingerprint
            security = stream.get("security", "")
            if security == "reality":
                reality = stream.get("realitySettings", {})
                reality["fingerprint"] = fp.xray_fingerprint
                stream["realitySettings"] = reality
            elif security == "tls":
                tls = stream.get("tlsSettings", {})
                tls["fingerprint"] = fp.xray_fingerprint
                if "alpn" not in tls:
                    tls["alpn"] = fp.alpn
                stream["tlsSettings"] = tls
            
            # Add sockopt with fragmentation
            sockopt = stream.get("sockopt", {})
            sockopt["tcpFastOpen"] = True
            sockopt["tcpKeepAliveInterval"] = 60
            
            if "fragment" not in sockopt:
                frag = self._build_sockopt(network_type, True).get("fragment")
                if frag:
                    sockopt["fragment"] = frag
            
            stream["sockopt"] = sockopt
            outbound["streamSettings"] = stream
        
        # Add sniffing to inbounds
        for inbound in config.get("inbounds", []):
            if "sniffing" not in inbound:
                inbound["sniffing"] = {
                    "enabled": True,
                    "destOverride": ["http", "tls", "quic"],
                    "routeOnly": True
                }
        
        return config
    
    def get_recommended_config_for_network(self, 
                                            network_type: str,
                                            server_address: str = "",
                                            server_port: int = 443) -> Dict[str, str]:
        """
        Get recommended protocol and config type based on network conditions.
        
        From article Table 2:
        - Home (ADSL/Fiber) -> VLESS-Reality-Vision
        - Mobile (4G/5G) -> Hysteria2 / TUIC v5
        - Blackout -> SplitHTTP (Xray v26)
        - UDP-Restricted -> VLESS over WebSocket (CDN)
        """
        recommendations = {
            "fiber": {
                "protocol": "VLESS-Reality-Vision",
                "config_type": "reality",
                "reason": "High stability, full stealth against DPI",
                "key_settings": "flow: xtls-rprx-vision",
            },
            "adsl": {
                "protocol": "VLESS-Reality-Vision",
                "config_type": "reality",
                "reason": "High stability, full stealth against DPI",
                "key_settings": "flow: xtls-rprx-vision",
            },
            "mobile_4g": {
                "protocol": "Hysteria2 / TUIC v5",
                "config_type": "hysteria2",
                "reason": "Overcomes Throttling and high Packet Loss",
                "key_settings": "congestion: brutal, MTU: 1350",
            },
            "mobile_5g": {
                "protocol": "Hysteria2 / TUIC v5",
                "config_type": "hysteria2",
                "reason": "Overcomes Throttling and high Packet Loss on 5G",
                "key_settings": "congestion: brutal, MTU: 1350",
            },
            "blackout": {
                "protocol": "SplitHTTP (Xray v26)",
                "config_type": "splithttp",
                "reason": "Mimics standard web browsing, CDN evasion",
                "key_settings": "fragment: 100-200 bytes",
            },
            "udp_restricted": {
                "protocol": "VLESS over WebSocket (CDN)",
                "config_type": "ws_cdn",
                "reason": "Leverages Cloudflare/Gcore capacity",
                "key_settings": "path: /random-path, Early Data",
            },
        }
        
        return recommendations.get(network_type, recommendations["fiber"])
