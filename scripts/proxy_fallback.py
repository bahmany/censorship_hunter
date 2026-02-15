"""
Proxy fallback mechanism for Telegram connection.

When SSH tunnel fails, uses validated V2Ray proxies to establish
a SOCKS5 tunnel for Telegram connection.
"""

import asyncio
import json
import logging
import os
import socket
import subprocess
import sys
import tempfile
import time
from typing import Optional, Tuple, List
from pathlib import Path

from hunter.core.models import HunterBenchResult
from hunter.core.utils import resolve_executable_path


class ProxyTunnelManager:
    """Manages V2Ray proxy tunnels for Telegram fallback."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.tunnel_process = None
        self.tunnel_port = None
        self.tunnel_config_path = None
    
    def establish_tunnel(
        self,
        validated_configs: List[HunterBenchResult],
        max_latency_ms: float = 200,
        socks_port: int = 11088
    ) -> Optional[int]:
        """
        Establish a SOCKS5 tunnel using validated V2Ray configs.
        
        Args:
            validated_configs: List of validated proxy configs
            max_latency_ms: Maximum acceptable latency (200ms default)
            socks_port: Local SOCKS5 port to use
        
        Returns:
            Port number if successful, None otherwise
        """
        # Filter configs by latency
        fast_configs = [
            c for c in validated_configs
            if c.latency_ms <= max_latency_ms
        ]
        
        if not fast_configs:
            self.logger.warning(f"No configs faster than {max_latency_ms}ms")
            return None
        
        self.logger.info(f"Found {len(fast_configs)} fast configs for tunnel")
        
        # Try each config until one works
        for config in fast_configs[:5]:  # Try top 5 fastest
            try:
                port = self._try_config(config, socks_port)
                if port:
                    self.logger.info(f"[SUCCESS] Proxy tunnel established on port {port}")
                    self.logger.info(f"Using: {config.ps or config.host}:{config.port} (latency: {config.latency_ms:.0f}ms)")
                    return port
            except Exception as e:
                self.logger.debug(f"Failed to establish tunnel with {config.host}: {e}")
                continue
        
        self.logger.error("Failed to establish tunnel with any validated config")
        return None
    
    def _try_config(self, config: HunterBenchResult, socks_port: int) -> Optional[int]:
        """Try to establish tunnel with a single config."""
        xray_path = resolve_executable_path("xray", "", [
            "D:\\v2rayN\\bin\\xray\\xray.exe",
            "C:\\v2rayN\\bin\\xray\\xray.exe",
            "xray.exe"
        ])
        
        if not xray_path:
            return None
        
        try:
            # Create XRay config
            xray_config = {
                "log": {"loglevel": "warning"},
                "inbounds": [
                    {
                        "port": socks_port,
                        "listen": "127.0.0.1",
                        "protocol": "socks",
                        "settings": {"auth": "noauth", "udp": True}
                    }
                ],
                "outbounds": [
                    {
                        "tag": "proxy",
                        "protocol": config.outbound.get("protocol", "vmess"),
                        **config.outbound
                    }
                ]
            }
            
            # Write config to temp file
            fd, self.tunnel_config_path = tempfile.mkstemp(
                prefix="HUNTER_TELEGRAM_", suffix=".json"
            )
            os.close(fd)
            
            with open(self.tunnel_config_path, "w", encoding="utf-8") as f:
                json.dump(xray_config, f, ensure_ascii=False)
            
            # Start XRay process
            self.tunnel_process = subprocess.Popen(
                [xray_path, "run", "-c", self.tunnel_config_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            )
            
            # Wait for process to start
            time.sleep(1.5)
            
            # Check if process is still running
            if self.tunnel_process.poll() is not None:
                return None
            
            # Verify SOCKS5 port is listening
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(2)
                result = sock.connect_ex(("127.0.0.1", socks_port))
                sock.close()
                
                if result == 0:
                    self.tunnel_port = socks_port
                    return socks_port
            except Exception:
                pass
            
            return None
            
        except Exception as e:
            self.logger.debug(f"Config error: {e}")
            self.close_tunnel()
            return None
    
    def close_tunnel(self):
        """Close the proxy tunnel."""
        if self.tunnel_process:
            try:
                self.tunnel_process.terminate()
                self.tunnel_process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    self.tunnel_process.kill()
                    self.tunnel_process.wait(timeout=1)
                except Exception:
                    pass
            except Exception:
                pass
            self.tunnel_process = None
        
        if self.tunnel_config_path and os.path.exists(self.tunnel_config_path):
            try:
                os.remove(self.tunnel_config_path)
            except Exception:
                pass
            self.tunnel_config_path = None
        
        self.tunnel_port = None
        self.logger.info("Proxy tunnel closed")


class TelegramProxyFallback:
    """Handles Telegram connection with proxy fallback."""
    
    def __init__(self, telegram_scraper, orchestrator):
        self.telegram_scraper = telegram_scraper
        self.orchestrator = orchestrator
        self.logger = logging.getLogger(__name__)
        self.tunnel_manager = ProxyTunnelManager()
    
    async def connect_with_fallback(self) -> bool:
        """
        Attempt Telegram connection with fallback to proxy tunnel.
        
        Strategy:
        1. Try SSH tunnel first
        2. If SSH fails, try validated V2Ray proxies
        3. If proxy tunnel succeeds, retry Telegram connection
        """
        self.logger.info("Attempting Telegram connection with fallback strategy")
        
        # Step 1: Try SSH tunnel
        self.logger.info("Step 1: Attempting SSH tunnel...")
        ssh_port = self.telegram_scraper.establish_ssh_tunnel()
        if ssh_port:
            self.logger.info(f"[SUCCESS] SSH tunnel established on port {ssh_port}")
            try:
                connected = await self.telegram_scraper.connect()
                if connected:
                    return True
            except Exception as e:
                self.logger.warning(f"SSH tunnel connection failed: {e}")
                self.telegram_scraper.close_ssh_tunnel()
        
        # Step 2: Try proxy tunnel fallback
        self.logger.info("Step 2: SSH failed, attempting proxy tunnel fallback...")
        
        # Get validated configs from cache or current cycle
        validated_configs = self._get_validated_configs()
        if not validated_configs:
            self.logger.error("No validated configs available for proxy fallback")
            return False
        
        self.logger.info(f"Using {len(validated_configs)} validated configs for fallback")
        
        # Try to establish proxy tunnel
        tunnel_port = self.tunnel_manager.establish_tunnel(
            validated_configs,
            max_latency_ms=200,  # Use only fast proxies
            socks_port=11088
        )
        
        if not tunnel_port:
            self.logger.error("Failed to establish proxy tunnel")
            return False
        
        # Step 3: Retry Telegram through proxy tunnel
        self.logger.info(f"Step 3: Retrying Telegram through proxy tunnel on port {tunnel_port}...")
        try:
            # Set proxy environment for Telegram connection
            original_proxy_host = os.getenv("HUNTER_TELEGRAM_PROXY_HOST")
            original_proxy_port = os.getenv("HUNTER_TELEGRAM_PROXY_PORT")
            
            os.environ["HUNTER_TELEGRAM_PROXY_HOST"] = "127.0.0.1"
            os.environ["HUNTER_TELEGRAM_PROXY_PORT"] = str(tunnel_port)
            
            # Reset Telegram client to use new proxy
            await self.telegram_scraper._reset_client()
            
            # Attempt connection
            connected = await self.telegram_scraper.connect()
            
            if connected:
                self.logger.info("[SUCCESS] Connected to Telegram through proxy tunnel")
                return True
            else:
                self.logger.warning("Telegram connection through proxy tunnel failed")
                return False
                
        except Exception as e:
            self.logger.error(f"Proxy tunnel connection error: {e}")
            return False
        finally:
            # Restore original proxy settings
            if original_proxy_host:
                os.environ["HUNTER_TELEGRAM_PROXY_HOST"] = original_proxy_host
            else:
                os.environ.pop("HUNTER_TELEGRAM_PROXY_HOST", None)
            
            if original_proxy_port:
                os.environ["HUNTER_TELEGRAM_PROXY_PORT"] = original_proxy_port
            else:
                os.environ.pop("HUNTER_TELEGRAM_PROXY_PORT", None)
            
            # Close tunnel if connection failed
            if not connected:
                self.tunnel_manager.close_tunnel()
    
    def _get_validated_configs(self) -> List[HunterBenchResult]:
        """Get validated configs from cache or orchestrator."""
        # Try to get from orchestrator's current cycle
        if hasattr(self.orchestrator, 'validated_configs'):
            if self.orchestrator.validated_configs:
                return self.orchestrator.validated_configs
        
        # Try to load from cache
        try:
            from hunter.config.cache import SmartCache
            cache = SmartCache()
            cached = cache.load_cached_configs(max_count=50, working_only=True)
            if cached:
                # Parse cached URIs into mock configs
                from hunter.parsers import UniversalParser
                parser = UniversalParser()
                configs = []
                for uri in cached[:20]:  # Use top 20 cached
                    parsed = parser.parse(uri)
                    if parsed:
                        from hunter.core.models import HunterBenchResult
                        result = HunterBenchResult(
                            uri=uri,
                            outbound=parsed.outbound,
                            host=parsed.host,
                            port=parsed.port,
                            identity=parsed.identity,
                            ps=parsed.ps,
                            latency_ms=100.0,  # Assume cached configs are reasonably fast
                            ip=None,
                            country_code=None,
                            region="Unknown",
                            tier="silver"
                        )
                        configs.append(result)
                return configs
        except Exception as e:
            self.logger.debug(f"Failed to load cached configs: {e}")
        
        return []
    
    def close(self):
        """Close all connections."""
        self.tunnel_manager.close_tunnel()
        self.telegram_scraper.close_ssh_tunnel()
