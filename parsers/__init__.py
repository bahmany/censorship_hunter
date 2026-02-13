"""
Parsers module initialization.

This module provides access to all URI parsers for different
proxy protocols.
"""

from .uri_parser import (
    VMessParser,
    VLESSParser, 
    TrojanParser,
    ShadowsocksParser,
    UniversalParser
)

__all__ = [
    "VMessParser",
    "VLESSParser",
    "TrojanParser", 
    "ShadowsocksParser",
    "UniversalParser"
]
