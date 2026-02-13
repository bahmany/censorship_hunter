"""
URI parsers for different proxy protocols.

This module contains parsers for various proxy protocols including
VMess, VLESS, Trojan, and Shadowsocks.
"""

import base64
import json
from typing import Any, Dict, Optional
from urllib.parse import parse_qs, unquote, urlparse

from hunter.core.utils import safe_b64decode, clean_ps_string
from hunter.core.models import HunterParsedConfig


class VMessParser:
    """Parser for VMess protocol URIs."""
    
    @staticmethod
    def parse(uri: str) -> Optional[HunterParsedConfig]:
        """Parse VMess URI."""
        try:
            payload = uri[len("vmess://"):]
            decoded = safe_b64decode(payload).decode("utf-8", errors="ignore")
            j = json.loads(decoded)
            
            host = j.get("add")
            port = int(j.get("port", 0))
            uuid = j.get("id")
            ps = clean_ps_string(j.get("ps", "Unknown"))
            
            if not host or not port or not uuid or host == "0.0.0.0":
                return None
            
            outbound: Dict[str, Any] = {
                "protocol": "vmess",
                "settings": {
                    "vnext": [
                        {
                            "address": host,
                            "port": port,
                            "users": [
                                {
                                    "id": uuid,
                                    "alterId": int(j.get("aid", 0)),
                                    "security": j.get("scy", "auto"),
                                }
                            ],
                        }
                    ]
                },
                "streamSettings": {
                    "network": j.get("net", "tcp"),
                    "security": j.get("tls", "none"),
                },
            }
            
            if j.get("net") == "ws":
                outbound["streamSettings"]["wsSettings"] = {
                    "path": j.get("path", "/"),
                    "headers": {"Host": j.get("host", "")},
                }
            
            if j.get("tls") == "tls":
                outbound["streamSettings"]["tlsSettings"] = {
                    "serverName": j.get("sni", host),
                    "allowInsecure": False,
                }
            
            return HunterParsedConfig(uri=uri, outbound=outbound, host=host, port=port, identity=uuid, ps=ps)
        except Exception:
            return None


class VLESSParser:
    """Parser for VLESS protocol URIs."""
    
    @staticmethod
    def parse(uri: str) -> Optional[HunterParsedConfig]:
        """Parse VLESS URI."""
        try:
            parsed_url = urlparse(uri)
            uuid = parsed_url.username
            host = parsed_url.hostname
            port = parsed_url.port or 443
            params = parse_qs(parsed_url.query)
            ps = clean_ps_string(parsed_url.fragment or "Unknown")
            
            if not host or not uuid or host == "0.0.0.0":
                return None
            
            security = params.get("security", ["none"])[0]
            outbound: Dict[str, Any] = {
                "protocol": "vless",
                "settings": {
                    "vnext": [
                        {
                            "address": host,
                            "port": int(port),
                            "users": [{"id": uuid, "encryption": params.get("encryption", ["none"])[0]}],
                        }
                    ]
                },
                "streamSettings": {
                    "network": params.get("type", ["tcp"])[0],
                    "security": security,
                },
            }
            
            if security in ["tls", "reality"]:
                base_settings: Dict[str, Any] = {"serverName": params.get("sni", [host])[0], "allowInsecure": False}
                if security == "reality":
                    base_settings.update(
                        {
                            "fingerprint": params.get("fp", ["chrome"])[0],
                            "publicKey": params.get("pbk", [""])[0],
                            "shortId": params.get("sid", [""])[0],
                        }
                    )
                    outbound["streamSettings"]["realitySettings"] = base_settings
                else:
                    outbound["streamSettings"]["tlsSettings"] = base_settings
            
            transport = params.get("type", [""])[0]
            if transport == "ws":
                outbound["streamSettings"]["wsSettings"] = {
                    "path": params.get("path", ["/"])[0],
                    "headers": {"Host": params.get("host", [""])[0]},
                }
            elif transport == "grpc":
                outbound["streamSettings"]["grpcSettings"] = {"serviceName": params.get("serviceName", [""])[0]}
            
            return HunterParsedConfig(uri=uri, outbound=outbound, host=host, port=int(port), identity=uuid, ps=ps)
        except Exception:
            return None


class TrojanParser:
    """Parser for Trojan protocol URIs."""
    
    @staticmethod
    def parse(uri: str) -> Optional[HunterParsedConfig]:
        """Parse Trojan URI."""
        try:
            parsed_url = urlparse(uri)
            password = parsed_url.username
            host = parsed_url.hostname
            port = parsed_url.port or 443
            params = parse_qs(parsed_url.query)
            ps = clean_ps_string(parsed_url.fragment or "Unknown")
            
            if not host or not password or host == "0.0.0.0":
                return None
            
            outbound: Dict[str, Any] = {
                "protocol": "trojan",
                "settings": {"servers": [{"address": host, "port": int(port), "password": password}]},
                "streamSettings": {
                    "network": params.get("type", ["tcp"])[0],
                    "security": "tls",
                    "tlsSettings": {
                        "serverName": params.get("sni", [host])[0],
                        "allowInsecure": params.get("allowInsecure", ["0"])[0] == "1",
                    },
                },
            }
            
            return HunterParsedConfig(uri=uri, outbound=outbound, host=host, port=int(port), identity=password, ps=ps)
        except Exception:
            return None


class ShadowsocksParser:
    """Parser for Shadowsocks protocol URIs."""
    
    @staticmethod
    def parse(uri: str) -> Optional[HunterParsedConfig]:
        """Parse Shadowsocks URI."""
        try:
            parsed = uri
            if "#" in parsed:
                parsed, tag = parsed.split("#", 1)
                ps = clean_ps_string(unquote(tag))
            else:
                ps = "Unknown"
            
            core = parsed[len("ss://"):]
            if "@" not in core:
                core = safe_b64decode(core.split("?", 1)[0]).decode("utf-8", errors="ignore")
            
            if "@" not in core:
                return None
            
            userinfo, hostport = core.split("@", 1)
            if ":" not in hostport:
                return None
            
            # Try to decode userinfo as base64 (standard SS format)
            method = None
            password = None
            try:
                decoded_userinfo = safe_b64decode(userinfo).decode("utf-8", errors="ignore")
                if ":" in decoded_userinfo:
                    method, password = decoded_userinfo.split(":", 1)
            except Exception:
                pass
            
            # Fallback to direct split if userinfo is not base64-encoded
            if (not method) or (password is None):
                if ":" in userinfo:
                    method, password = userinfo.split(":", 1)
                else:
                    return None
            host, port_str = hostport.split(":", 1)
            
            import re
            port = int(re.split(r"[^0-9]", port_str)[0])
            
            if not host or not port or host == "0.0.0.0":
                return None
            
            outbound: Dict[str, Any] = {
                "protocol": "shadowsocks",
                "settings": {"servers": [{"address": host, "port": port, "method": method, "password": password}]},
            }
            
            identity = f"{method}:{password}"
            return HunterParsedConfig(uri=uri, outbound=outbound, host=host, port=port, identity=identity, ps=ps)
        except Exception:
            return None


class UniversalParser:
    """Universal parser that can handle all supported protocols."""
    
    def __init__(self):
        self.parsers = {
            "vmess": VMessParser(),
            "vless": VLESSParser(),
            "trojan": TrojanParser(),
            "ss": ShadowsocksParser(),
            "shadowsocks": ShadowsocksParser(),
        }
    
    def parse(self, uri: str) -> Optional[HunterParsedConfig]:
        """Parse URI using appropriate parser."""
        if not uri or "://" not in uri:
            return None
        
        proto = uri.split("://", 1)[0].lower()
        parser = self.parsers.get(proto)
        
        if parser:
            return parser.parse(uri)
        
        return None
