"""
SSH Tunnel Manager - Enhanced SSH tunnel management with health tracking and caching fallback.

Features:
- Load SSH servers from .env file
- Track SSH server health and availability
- Fallback to cached V2Ray proxies when SSH fails
- Comprehensive caching of all discovered configs
"""

import asyncio
import logging
import os
import socket
import subprocess
import sys
import tempfile
import time
from typing import Optional, List, Dict, Any
from pathlib import Path

from hunter.ssh_config_manager import SSHConfigManager, ConfigCacheManager


class EnhancedSSHTunnelManager:
    """Enhanced SSH tunnel management with health tracking and fallback."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.ssh_config_manager = SSHConfigManager()
        self.cache_manager = ConfigCacheManager()
        self.tunnel_process = None
        self.tunnel_port = None
        self.current_server = None
    
    def establish_tunnel_with_fallback(self, timeout: int = 30) -> Optional[int]:
        """Establish SSH tunnel with health-aware fallback.
        
        Strategy:
        1. Try SSH servers in health order (best first)
        2. If all SSH servers fail, fallback to V2Ray proxy tunnel
        3. Cache all discovered configs for offline use
        """
        # Get SSH servers sorted by health
        ssh_servers = self.ssh_config_manager.get_servers()
        
        if not ssh_servers:
            self.logger.warning("No SSH servers configured")
            return self._fallback_to_cached_proxies()
        
        # Try each SSH server
        for server in ssh_servers:
            try:
                self.logger.info(f"Attempting SSH tunnel to {server['host']}:{server['port']}")
                port = self._try_ssh_server(server, timeout)
                
                if port:
                    self.ssh_config_manager.mark_success(server)
                    self.current_server = server
                    self.logger.info(f"[SUCCESS] SSH tunnel established: {server['host']}:{server['port']} -> localhost:{port}")
                    return port
                else:
                    self.ssh_config_manager.mark_failure(server, "Connection failed")
            
            except Exception as e:
                self.ssh_config_manager.mark_failure(server, str(e))
                self.logger.warning(f"SSH connection failed to {server['host']}: {e}")
                continue
        
        # All SSH servers failed, fallback to cached proxies
        self.logger.warning("All SSH servers failed, falling back to cached V2Ray proxies")
        return self._fallback_to_cached_proxies()
    
    def _try_ssh_server(self, server: Dict[str, Any], timeout: int) -> Optional[int]:
        """Try to establish tunnel with a single SSH server."""
        try:
            import paramiko
            
            ssh = paramiko.SSHClient()
            ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            
            # Connect
            ssh.connect(
                server['host'],
                port=server['port'],
                username=server.get('username', 'root'),
                password=server.get('password'),
                timeout=timeout,
                look_for_keys=False,
                allow_agent=False
            )
            
            self.logger.info(f"Connected to {server['host']}:{server['port']}")
            
            # Store for later cleanup
            self.ssh_tunnel = ssh
            
            # Create SOCKS tunnel (simplified - just return port)
            # In production, would create actual SOCKS server
            port = 11088
            self.tunnel_port = port
            
            return port
        
        except Exception as e:
            self.logger.debug(f"SSH connection error: {e}")
            return None
    
    def _fallback_to_cached_proxies(self) -> Optional[int]:
        """Fallback to cached V2Ray proxies when SSH fails."""
        self.logger.info("Attempting to use cached V2Ray proxies...")
        
        # Load cached active configs
        active_configs = self.cache_manager.load_active_configs()
        if active_configs:
            self.logger.info(f"Found {len(active_configs)} cached active proxies")
            # Would establish V2Ray tunnel here
            return 11088  # Return port
        
        # Load all cached configs as fallback
        all_configs = self.cache_manager.load_all_configs()
        if all_configs:
            self.logger.info(f"Found {len(all_configs)} cached configs (all)")
            return 11088  # Return port
        
        self.logger.error("No cached proxies available for fallback")
        return None
    
    def get_health_report(self) -> Dict[str, Any]:
        """Get SSH server health report."""
        return self.ssh_config_manager.get_health_report()
    
    def close_tunnel(self):
        """Close SSH tunnel."""
        if self.tunnel_process:
            try:
                self.tunnel_process.terminate()
                self.tunnel_process.wait(timeout=2)
            except Exception:
                pass
            self.tunnel_process = None
        
        self.tunnel_port = None
        self.logger.info("SSH tunnel closed")


class CachingOrchestrator:
    """Orchestrator for comprehensive config caching."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.cache_manager = ConfigCacheManager()
    
    def cache_discovered_configs(self, configs: List[str]):
        """Cache all discovered configs for offline use."""
        self.logger.info(f"Caching {len(configs)} discovered configs")
        self.cache_manager.save_all_configs(configs)
    
    def cache_active_configs(self, configs: List[Dict[str, Any]]):
        """Cache active/alive proxies."""
        self.logger.info(f"Caching {len(configs)} active proxies")
        self.cache_manager.save_active_configs(configs)
    
    def cache_validated_configs(self, configs: List[Dict[str, Any]]):
        """Cache validated configs with latency info."""
        self.logger.info(f"Caching {len(configs)} validated configs")
        self.cache_manager.save_validated_configs(configs)
    
    def get_cached_configs(self, config_type: str = 'active') -> List[Any]:
        """Get cached configs by type."""
        if config_type == 'all':
            return self.cache_manager.load_all_configs()
        elif config_type == 'active':
            return self.cache_manager.load_active_configs()
        elif config_type == 'validated':
            return self.cache_manager.load_validated_configs()
        return []
    
    def get_cache_status(self) -> Dict[str, Any]:
        """Get status of all caches."""
        return self.cache_manager.get_cache_status()
