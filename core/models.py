"""
Core data models and structures for the Hunter system.

This module contains the fundamental data structures used throughout
the Hunter proxy hunting system.
"""

from dataclasses import dataclass
from typing import Any, Dict, Optional


@dataclass
class HunterParsedConfig:
    """Represents a parsed V2Ray configuration."""
    uri: str
    outbound: Dict[str, Any]
    host: str
    port: int
    identity: str
    ps: str


@dataclass
class HunterBenchResult:
    """Represents the result of benchmarking a proxy configuration."""
    uri: str
    outbound: Dict[str, Any]
    host: str
    port: int
    identity: str
    ps: str
    latency_ms: float
    ip: Optional[str]
    country_code: Optional[str]
    region: str
    tier: str


@dataclass
class ProxyStats:
    """Statistics for proxy performance."""
    total_configs: int = 0
    working_configs: int = 0
    gold_configs: int = 0
    silver_configs: int = 0
    average_latency: float = 0.0
    last_update: Optional[float] = None


@dataclass
class GatewayStats:
    """Statistics for gateway server."""
    running: bool = False
    current_config: Optional[str] = None
    uptime: int = 0
    socks_port: int = 0
    http_port: int = 0
    dns_port: int = 0
    requests: int = 0
    bytes_sent: int = 0
    bytes_received: int = 0


@dataclass
class BalancerStats:
    """Statistics for load balancer."""
    running: bool = False
    port: int = 0
    backends: int = 0
    total_backends: int = 0
    restarts: int = 0
    health_checks: int = 0
    backend_swaps: int = 0
    last_restart: Optional[float] = None
