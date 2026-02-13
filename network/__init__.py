"""
Network module initialization.

This module provides access to network operations including
HTTP client management and configuration fetching.
"""

from .http_client import (
    HTTPClientManager,
    ConfigFetcher,
    GITHUB_REPOS,
    ANTI_CENSORSHIP_SOURCES,
    IRAN_PRIORITY_SOURCES,
    NAPSTERV_SUBSCRIPTION_URLS
)

__all__ = [
    "HTTPClientManager",
    "ConfigFetcher",
    "GITHUB_REPOS",
    "ANTI_CENSORSHIP_SOURCES", 
    "IRAN_PRIORITY_SOURCES",
    "NAPSTERV_SUBSCRIPTION_URLS"
]
