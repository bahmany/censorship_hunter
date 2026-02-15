"""
Hysteria2 & TUIC v5 UDP-Based Protocol Support with Port Hopping

Built on QUIC standards, these protocols are optimal for poor line quality
(high packet loss) common on Iranian mobile networks (MCI, Irancell).

Key features from article Section 4.4:
- Congestion Control: Algorithms like Brutal or BBR aggressively occupy bandwidth,
  preventing the filtering system from using Throttling to degrade service.
- Challenge: Since Iran throttles UDP (QUIC throttling), "Port Hopping" is required
  to evade detection.

This module:
- Generates Hysteria2 server/client configurations
- Generates TUIC v5 server/client configurations
- Implements port hopping strategies for UDP throttling evasion
- Provides congestion control configuration (Brutal/BBR)
- Manages MTU settings for 5G PMTUD attack mitigation
- Generates Sing-box compatible configs for Hysteria2/TUIC
"""

import json
import logging
import os
import random
import secrets
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple


class CongestionControl(Enum):
    """Congestion control algorithms for UDP protocols."""
    BRUTAL = "brutal"       # Aggressive bandwidth occupation
    BBR = "bbr"             # Google's BBR algorithm
    CUBIC = "cubic"         # Standard TCP CUBIC adapted for QUIC
    NEW_RENO = "new_reno"   # Conservative fallback


class PortHoppingStrategy(Enum):
    """Port hopping strategies to evade QUIC throttling."""
    SEQUENTIAL = "sequential"       # Increment ports sequentially
    RANDOM = "random"               # Random port selection
    RANGE_SWEEP = "range_sweep"     # Sweep through port ranges
    TIME_BASED = "time_based"       # Change port based on time intervals


@dataclass
class Hysteria2Config:
    """Hysteria2 protocol configuration."""
    server_address: str = ""
    server_port: int = 443
    auth_password: str = ""
    # Bandwidth settings (Mbps)
    up_mbps: int = 50
    down_mbps: int = 100
    # Congestion control
    congestion: CongestionControl = CongestionControl.BRUTAL
    # TLS
    sni: str = ""
    insecure: bool = False
    # Port hopping
    port_hopping_enabled: bool = True
    port_hopping_range: str = "20000-40000"
    port_hopping_interval: int = 30  # seconds
    # MTU (critical for 5G PMTUD attacks)
    mtu: int = 1350
    # Obfuscation
    obfs_type: str = "salamander"
    obfs_password: str = ""
    # Local proxy
    socks_port: int = 10808
    http_port: int = 10809


@dataclass
class TUICConfig:
    """TUIC v5 protocol configuration."""
    server_address: str = ""
    server_port: int = 443
    user_uuid: str = ""
    auth_password: str = ""
    # Congestion control
    congestion: CongestionControl = CongestionControl.BBR
    # QUIC settings
    alpn: List[str] = field(default_factory=lambda: ["h3"])
    udp_relay_mode: str = "native"  # native or quic
    # TLS
    sni: str = ""
    insecure: bool = False
    # Port hopping
    port_hopping_enabled: bool = True
    port_hopping_range: str = "20000-40000"
    # MTU
    mtu: int = 1350
    # Reduce RTT
    zero_rtt_handshake: bool = True
    # Local proxy
    socks_port: int = 10808


@dataclass
class PortHoppingConfig:
    """Port hopping configuration."""
    enabled: bool = True
    strategy: PortHoppingStrategy = PortHoppingStrategy.RANDOM
    port_range_start: int = 20000
    port_range_end: int = 40000
    hop_interval_seconds: int = 30
    max_ports: int = 100


class UDPProtocolManager:
    """
    Manages Hysteria2 and TUIC v5 protocol configurations.
    
    Optimized for Iran's mobile networks where:
    - UDP/QUIC is throttled by DPI
    - High packet loss on 4G/5G
    - PMTUD attacks cause connection drops
    - Port-based blocking requires hopping
    """
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self._port_hop_counter = 0
        self._current_ports: Dict[str, int] = {}
    
    def generate_hysteria2_server_config(self,
                                          listen_port: int = 443,
                                          domain: str = "",
                                          cert_path: str = "/etc/ssl/certs/cert.pem",
                                          key_path: str = "/etc/ssl/private/key.pem",
                                          auth_password: str = "",
                                          up_mbps: int = 100,
                                          down_mbps: int = 200,
                                          obfs_password: str = "") -> Dict[str, Any]:
        """
        Generate Hysteria2 server configuration.
        
        Server-side config for VPS running Hysteria2.
        """
        if not auth_password:
            auth_password = secrets.token_urlsafe(24)
        if not obfs_password:
            obfs_password = secrets.token_urlsafe(16)
        
        config = {
            "listen": f":{listen_port}",
            "tls": {
                "cert": cert_path,
                "key": key_path,
            },
            "auth": {
                "type": "password",
                "password": auth_password,
            },
            "masquerade": {
                "type": "proxy",
                "proxy": {
                    "url": "https://www.apple.com",
                    "rewriteHost": True,
                }
            },
            "bandwidth": {
                "up": f"{up_mbps} mbps",
                "down": f"{down_mbps} mbps",
            },
            "quic": {
                "initStreamReceiveWindow": 8388608,
                "maxStreamReceiveWindow": 8388608,
                "initConnReceiveWindow": 20971520,
                "maxConnReceiveWindow": 20971520,
                "maxIdleTimeout": "30s",
                "maxIncomingStreams": 1024,
                "disablePathMTUDiscovery": True,
            },
            "obfs": {
                "type": "salamander",
                "salamander": {
                    "password": obfs_password,
                }
            },
        }
        
        self.logger.info(f"Generated Hysteria2 server config on port {listen_port}")
        return config
    
    def generate_hysteria2_client_config(self,
                                          server_address: str,
                                          server_port: int = 443,
                                          auth_password: str = "",
                                          sni: str = "",
                                          up_mbps: int = 50,
                                          down_mbps: int = 100,
                                          obfs_password: str = "",
                                          socks_port: int = 10808,
                                          http_port: int = 10809,
                                          enable_port_hopping: bool = True) -> Dict[str, Any]:
        """
        Generate Hysteria2 client configuration.
        
        Optimized for Iranian mobile networks with:
        - Brutal congestion control to overcome throttling
        - Port hopping to evade QUIC blocking
        - Low MTU to prevent PMTUD attacks
        - Salamander obfuscation
        """
        if not sni:
            sni = server_address
        
        # Build server string with port hopping
        if enable_port_hopping:
            server_str = f"{server_address}:{server_port},20000-40000"
        else:
            server_str = f"{server_address}:{server_port}"
        
        config = {
            "server": server_str,
            "auth": auth_password,
            "tls": {
                "sni": sni,
                "insecure": False,
            },
            "bandwidth": {
                "up": f"{up_mbps} mbps",
                "down": f"{down_mbps} mbps",
            },
            "quic": {
                "initStreamReceiveWindow": 4194304,
                "maxStreamReceiveWindow": 4194304,
                "initConnReceiveWindow": 8388608,
                "maxConnReceiveWindow": 8388608,
                "maxIdleTimeout": "30s",
                "disablePathMTUDiscovery": True,
            },
            "socks5": {
                "listen": f"127.0.0.1:{socks_port}",
            },
            "http": {
                "listen": f"127.0.0.1:{http_port}",
            },
            "transport": {
                "type": "udp",
                "udp": {
                    "hopInterval": "30s",
                }
            },
        }
        
        # Add obfuscation if password provided
        if obfs_password:
            config["obfs"] = {
                "type": "salamander",
                "salamander": {
                    "password": obfs_password,
                }
            }
        
        self.logger.info(
            f"Generated Hysteria2 client config: {server_address}:{server_port}, "
            f"port_hopping={enable_port_hopping}"
        )
        return config
    
    def generate_tuic_server_config(self,
                                     listen_port: int = 443,
                                     cert_path: str = "/etc/ssl/certs/cert.pem",
                                     key_path: str = "/etc/ssl/private/key.pem",
                                     user_uuid: str = "",
                                     auth_password: str = "") -> Dict[str, Any]:
        """Generate TUIC v5 server configuration."""
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        if not auth_password:
            auth_password = secrets.token_urlsafe(24)
        
        config = {
            "server": f"[::]:{ listen_port}",
            "users": {
                user_uuid: auth_password,
            },
            "certificate": cert_path,
            "private_key": key_path,
            "congestion_control": "bbr",
            "alpn": ["h3"],
            "zero_rtt_handshake": True,
            "udp_relay_ipv6": True,
            "max_idle_time": "15s",
            "max_external_packet_size": 1350,
            "gc_interval": "3s",
            "gc_lifetime": "15s",
            "auth_timeout": "3s",
            "log_level": "warn",
        }
        
        self.logger.info(f"Generated TUIC v5 server config on port {listen_port}")
        return config
    
    def generate_tuic_client_config(self,
                                     server_address: str,
                                     server_port: int = 443,
                                     user_uuid: str = "",
                                     auth_password: str = "",
                                     sni: str = "",
                                     socks_port: int = 10808) -> Dict[str, Any]:
        """
        Generate TUIC v5 client configuration.
        
        Optimized for high packet loss environments with:
        - BBR congestion control
        - Zero-RTT handshake for faster connections
        - Native UDP relay mode
        """
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        if not sni:
            sni = server_address
        
        config = {
            "relay": {
                "server": f"{server_address}:{server_port}",
                "uuid": user_uuid,
                "password": auth_password,
                "ip": server_address,
                "congestion_control": "bbr",
                "alpn": ["h3"],
                "zero_rtt_handshake": True,
                "udp_relay_mode": "native",
                "disable_sni": False,
                "reduce_rtt": True,
            },
            "local": {
                "server": f"127.0.0.1:{socks_port}",
            },
            "log_level": "warn",
        }
        
        self.logger.info(f"Generated TUIC v5 client config: {server_address}:{server_port}")
        return config
    
    def generate_singbox_hysteria2_outbound(self,
                                             server_address: str,
                                             server_port: int = 443,
                                             auth_password: str = "",
                                             sni: str = "",
                                             up_mbps: int = 50,
                                             down_mbps: int = 100,
                                             obfs_password: str = "",
                                             enable_port_hopping: bool = True) -> Dict[str, Any]:
        """Generate Sing-box Hysteria2 outbound configuration."""
        if not sni:
            sni = server_address
        
        outbound = {
            "type": "hysteria2",
            "tag": "hysteria2-out",
            "server": server_address,
            "server_port": server_port,
            "password": auth_password,
            "up_mbps": up_mbps,
            "down_mbps": down_mbps,
            "tls": {
                "enabled": True,
                "server_name": sni,
                "alpn": ["h3"],
                "insecure": False,
            },
        }
        
        if obfs_password:
            outbound["obfs"] = {
                "type": "salamander",
                "password": obfs_password,
            }
        
        if enable_port_hopping:
            outbound["server_port_range"] = "20000:40000"
            outbound["hop_interval"] = "30s"
        
        return outbound
    
    def generate_singbox_tuic_outbound(self,
                                        server_address: str,
                                        server_port: int = 443,
                                        user_uuid: str = "",
                                        auth_password: str = "",
                                        sni: str = "") -> Dict[str, Any]:
        """Generate Sing-box TUIC v5 outbound configuration."""
        if not user_uuid:
            user_uuid = str(uuid.uuid4())
        if not sni:
            sni = server_address
        
        outbound = {
            "type": "tuic",
            "tag": "tuic-out",
            "server": server_address,
            "server_port": server_port,
            "uuid": user_uuid,
            "password": auth_password,
            "congestion_control": "bbr",
            "udp_relay_mode": "native",
            "zero_rtt_handshake": True,
            "tls": {
                "enabled": True,
                "server_name": sni,
                "alpn": ["h3"],
                "insecure": False,
            },
        }
        
        return outbound
    
    def get_port_hop_range(self, strategy: PortHoppingStrategy = PortHoppingStrategy.RANDOM,
                           port_start: int = 20000, port_end: int = 40000,
                           count: int = 10) -> List[int]:
        """
        Generate a list of ports for hopping.
        
        Different strategies for different ISP behaviors:
        - RANDOM: Best for general use
        - SEQUENTIAL: When ISP blocks random high ports
        - RANGE_SWEEP: When ISP learns and blocks specific ports
        - TIME_BASED: Change on fixed schedule
        """
        ports = []
        
        if strategy == PortHoppingStrategy.RANDOM:
            ports = [random.randint(port_start, port_end) for _ in range(count)]
        
        elif strategy == PortHoppingStrategy.SEQUENTIAL:
            step = (port_end - port_start) // count
            ports = [port_start + i * step for i in range(count)]
        
        elif strategy == PortHoppingStrategy.RANGE_SWEEP:
            # Divide range into sub-ranges and pick from each
            sub_range = (port_end - port_start) // count
            for i in range(count):
                sub_start = port_start + i * sub_range
                sub_end = sub_start + sub_range
                ports.append(random.randint(sub_start, sub_end))
        
        elif strategy == PortHoppingStrategy.TIME_BASED:
            import time
            base = int(time.time()) // 30  # Change every 30 seconds
            for i in range(count):
                port = port_start + ((base + i) * 7919) % (port_end - port_start)
                ports.append(port)
        
        return ports
    
    def get_optimal_config_for_network(self, 
                                        network_type: str,
                                        packet_loss_percent: float = 0.0) -> Dict[str, Any]:
        """
        Get optimal UDP protocol configuration for network conditions.
        
        Returns recommended protocol and settings.
        """
        if network_type in ("mobile_5g", "mobile_4g"):
            if packet_loss_percent > 5.0:
                return {
                    "protocol": "hysteria2",
                    "congestion": "brutal",
                    "mtu": 1280 if network_type == "mobile_5g" else 1350,
                    "port_hopping": True,
                    "hop_interval": 20 if packet_loss_percent > 10 else 30,
                    "reason": f"High packet loss ({packet_loss_percent}%) - Brutal congestion overcomes throttling",
                }
            else:
                return {
                    "protocol": "tuic",
                    "congestion": "bbr",
                    "mtu": 1350,
                    "port_hopping": True,
                    "hop_interval": 30,
                    "reason": "Moderate conditions - BBR provides good throughput",
                }
        
        elif network_type == "udp_restricted":
            return {
                "protocol": "hysteria2",
                "congestion": "brutal",
                "mtu": 1350,
                "port_hopping": True,
                "hop_interval": 15,
                "obfs": "salamander",
                "reason": "UDP restricted - aggressive hopping + obfuscation needed",
            }
        
        else:
            return {
                "protocol": "hysteria2",
                "congestion": "bbr",
                "mtu": 1400,
                "port_hopping": False,
                "reason": "Fixed network - BBR sufficient, no hopping needed",
            }
    
    def get_metrics(self) -> Dict[str, Any]:
        """Get UDP protocol metrics."""
        return {
            "port_hop_counter": self._port_hop_counter,
            "active_ports": dict(self._current_ports),
        }
