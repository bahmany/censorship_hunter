"""
Hunter Package - Advanced V2Ray Proxy Hunting System

A sophisticated proxy hunting and testing system designed for 
circumventing internet censorship with advanced anti-DPI capabilities.
"""

__version__ = "2.0.0"
__author__ = "Hunter Project"

from .core.config import HunterConfig
from .core.models import HunterParsedConfig, HunterBenchResult
from .testing.benchmark import ProxyBenchmark
from .proxy.load_balancer import MultiProxyServer

__all__ = [
    "HunterConfig",
    "HunterParsedConfig", 
    "HunterBenchResult",
    "ProxyBenchmark",
    "MultiProxyServer"
]
