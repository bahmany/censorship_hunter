"""
SSH Configuration Manager - Manages SSH server credentials and fallback.

Features:
- Load SSH credentials from .env file
- Support multiple SSH servers with fallback
- Track SSH server health/availability
- Fallback to cached V2Ray proxies when SSH fails
"""

import json
import logging
import os
from pathlib import Path
from typing import List, Dict, Any, Optional
from datetime import datetime, timedelta


class SSHConfigManager:
    """Manages SSH server configurations and credentials."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.ssh_servers: List[Dict[str, Any]] = []
        self.server_health: Dict[str, Dict[str, Any]] = {}
        self.load_ssh_servers()
    
    def load_ssh_servers(self):
        """Load SSH servers from .env file, hunter_secrets.env, or environment variables."""
        # Try to load from .env file first
        env_path = Path(__file__).parent / ".env"
        if env_path.exists():
            self._load_from_env_file(env_path)
        else:
            # Try hunter_secrets.env
            secrets_path = Path(__file__).parent / "hunter_secrets.env"
            if secrets_path.exists():
                self._load_from_env_file(secrets_path)
            else:
                self._load_from_environment()
        
        if not self.ssh_servers:
            self.logger.warning("No SSH servers configured")
        else:
            self.logger.info(f"Loaded {len(self.ssh_servers)} SSH servers")
            for server in self.ssh_servers:
                self.logger.info(f"  - {server['host']}:{server['port']}")
    
    def _load_from_env_file(self, env_path: Path):
        """Load SSH servers from .env file."""
        try:
            with open(env_path, 'r', encoding='utf-8') as f:
                content = f.read()
            
            # Parse SSH_SERVERS_JSON (multi-line)
            if 'SSH_SERVERS_JSON=' in content:
                # Find the start of SSH_SERVERS_JSON
                start_idx = content.find('SSH_SERVERS_JSON=')
                if start_idx != -1:
                    # Extract from start to end of JSON array
                    start_idx += len('SSH_SERVERS_JSON=')
                    remaining = content[start_idx:]
                    
                    # Remove leading/trailing whitespace and quotes
                    remaining = remaining.strip()
                    if remaining.startswith('"') and remaining.endswith('"'):
                        remaining = remaining[1:-1]
                    elif remaining.startswith("'") and remaining.endswith("'"):
                        remaining = remaining[1:-1]
                    
                    # Find the end of the JSON array
                    bracket_count = 0
                    json_str = ""
                    for char in remaining:
                        json_str += char
                        if char == '[':
                            bracket_count += 1
                        elif char == ']':
                            bracket_count -= 1
                            if bracket_count == 0:
                                break
                    
                    try:
                        servers = json.loads(json_str)
                        if isinstance(servers, list):
                            self.ssh_servers = servers
                            self.logger.info(f"Loaded {len(servers)} SSH servers from {env_path.name}")
                            return
                    except json.JSONDecodeError as e:
                        self.logger.warning(f"Failed to parse SSH_SERVERS_JSON: {e}")
            
            # Parse individual SSH_N_* variables
            lines = content.split('\n')
            for line in lines:
                line = line.strip()
                if line.startswith('SSH_') and '_HOST=' in line:
                    self._parse_ssh_env_line(line)
        
        except Exception as e:
            self.logger.warning(f"Failed to load SSH servers from {env_path.name}: {e}")
    
    def _parse_ssh_env_line(self, line: str):
        """Parse SSH_N_* environment variable lines."""
        try:
            key, value = line.split('=', 1)
            key = key.strip()
            value = value.strip().strip('"\'')
            
            # Extract server number and field
            parts = key.split('_')
            if len(parts) < 3:
                return
            
            server_num = parts[1]
            field = parts[2].lower()
            
            # Find or create server entry
            server_idx = None
            for i, server in enumerate(self.ssh_servers):
                if server.get('_index') == server_num:
                    server_idx = i
                    break
            
            if server_idx is None:
                server_idx = len(self.ssh_servers)
                self.ssh_servers.append({'_index': server_num})
            
            # Set field
            if field == 'host':
                self.ssh_servers[server_idx]['host'] = value
            elif field == 'port':
                try:
                    self.ssh_servers[server_idx]['port'] = int(value)
                except ValueError:
                    self.ssh_servers[server_idx]['port'] = 22
            elif field == 'user':
                self.ssh_servers[server_idx]['username'] = value
            elif field == 'pass':
                self.ssh_servers[server_idx]['password'] = value
        
        except Exception as e:
            self.logger.debug(f"Failed to parse SSH env line: {e}")
    
    def _load_from_environment(self):
        """Load SSH servers from environment variables."""
        # Try JSON format first
        ssh_json = os.getenv('SSH_SERVERS_JSON')
        if ssh_json:
            try:
                servers = json.loads(ssh_json)
                if isinstance(servers, list):
                    self.ssh_servers = servers
                    return
            except json.JSONDecodeError:
                pass
        
        # Try individual SSH_N_* variables
        for i in range(1, 10):
            host = os.getenv(f'SSH{i}_HOST')
            if not host:
                continue
            
            port = int(os.getenv(f'SSH{i}_PORT', '22'))
            user = os.getenv(f'SSH{i}_USER')
            password = os.getenv(f'SSH{i}_PASS')
            
            server = {'host': host, 'port': port}
            if user:
                server['username'] = user
            if password:
                server['password'] = password
            
            self.ssh_servers.append(server)
    
    def get_servers(self) -> List[Dict[str, Any]]:
        """Get all SSH servers sorted by health (best first)."""
        if not self.ssh_servers:
            return []
        
        # Sort by health score (successful connections first)
        return sorted(
            self.ssh_servers,
            key=lambda s: self._get_health_score(s),
            reverse=True
        )
    
    def _get_health_score(self, server: Dict[str, Any]) -> float:
        """Calculate health score for a server."""
        key = f"{server['host']}:{server['port']}"
        health = self.server_health.get(key, {})
        
        # Calculate score based on success rate and recency
        success_count = health.get('success_count', 0)
        fail_count = health.get('fail_count', 0)
        last_success = health.get('last_success')
        
        # Success rate (0-1)
        total = success_count + fail_count
        if total == 0:
            success_rate = 0.5  # Unknown servers get neutral score
        else:
            success_rate = success_count / total
        
        # Recency bonus (recent successes are better)
        recency_bonus = 0
        if last_success:
            age = datetime.now() - last_success
            if age < timedelta(hours=1):
                recency_bonus = 1.0
            elif age < timedelta(hours=24):
                recency_bonus = 0.5
        
        return success_rate + recency_bonus
    
    def mark_success(self, server: Dict[str, Any]):
        """Mark a server as successfully connected."""
        key = f"{server['host']}:{server['port']}"
        if key not in self.server_health:
            self.server_health[key] = {
                'success_count': 0,
                'fail_count': 0,
                'last_success': None,
                'last_fail': None
            }
        
        health = self.server_health[key]
        health['success_count'] += 1
        health['last_success'] = datetime.now()
        
        self.logger.debug(f"SSH server {key}: success (total: {health['success_count']})")
    
    def mark_failure(self, server: Dict[str, Any], error: str = ""):
        """Mark a server as failed."""
        key = f"{server['host']}:{server['port']}"
        if key not in self.server_health:
            self.server_health[key] = {
                'success_count': 0,
                'fail_count': 0,
                'last_success': None,
                'last_fail': None
            }
        
        health = self.server_health[key]
        health['fail_count'] += 1
        health['last_fail'] = datetime.now()
        health['last_error'] = error
        
        self.logger.debug(f"SSH server {key}: failed ({error}) (total: {health['fail_count']})")
    
    def get_health_report(self) -> Dict[str, Any]:
        """Get health report for all servers."""
        report = {}
        for server in self.ssh_servers:
            key = f"{server['host']}:{server['port']}"
            health = self.server_health.get(key, {})
            report[key] = {
                'success_count': health.get('success_count', 0),
                'fail_count': health.get('fail_count', 0),
                'last_success': health.get('last_success'),
                'last_fail': health.get('last_fail'),
                'score': self._get_health_score(server)
            }
        return report


class ConfigCacheManager:
    """Manages comprehensive caching of all discovered V2Ray configs."""
    
    def __init__(self, cache_dir: Optional[str] = None):
        self.logger = logging.getLogger(__name__)
        self.cache_dir = Path(cache_dir or Path.home() / '.hunter' / 'cache')
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        
        self.all_configs_file = self.cache_dir / 'all_configs.json'
        self.active_configs_file = self.cache_dir / 'active_configs.json'
        self.validated_configs_file = self.cache_dir / 'validated_configs.json'
    
    def save_all_configs(self, configs: List[str]):
        """Save all discovered configs (for offline use)."""
        try:
            data = {
                'timestamp': datetime.now().isoformat(),
                'count': len(configs),
                'configs': configs
            }
            with open(self.all_configs_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            
            self.logger.info(f"Cached {len(configs)} configs to {self.all_configs_file}")
            return True
        except Exception as e:
            self.logger.error(f"Failed to save all configs: {e}")
            return False
    
    def load_all_configs(self) -> List[str]:
        """Load all cached configs (for offline use)."""
        try:
            if not self.all_configs_file.exists():
                return []
            
            with open(self.all_configs_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            configs = data.get('configs', [])
            self.logger.info(f"Loaded {len(configs)} cached configs")
            return configs
        except Exception as e:
            self.logger.error(f"Failed to load all configs: {e}")
            return []
    
    def save_active_configs(self, configs: List[Dict[str, Any]]):
        """Save active/alive proxies (validated and working)."""
        try:
            data = {
                'timestamp': datetime.now().isoformat(),
                'count': len(configs),
                'configs': configs
            }
            with open(self.active_configs_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            
            self.logger.info(f"Cached {len(configs)} active configs")
            return True
        except Exception as e:
            self.logger.error(f"Failed to save active configs: {e}")
            return False
    
    def load_active_configs(self) -> List[Dict[str, Any]]:
        """Load active/alive proxies."""
        try:
            if not self.active_configs_file.exists():
                return []
            
            with open(self.active_configs_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            configs = data.get('configs', [])
            self.logger.info(f"Loaded {len(configs)} active configs")
            return configs
        except Exception as e:
            self.logger.error(f"Failed to load active configs: {e}")
            return []
    
    def save_validated_configs(self, configs: List[Dict[str, Any]]):
        """Save validated configs with latency info."""
        try:
            data = {
                'timestamp': datetime.now().isoformat(),
                'count': len(configs),
                'configs': configs
            }
            with open(self.validated_configs_file, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            
            self.logger.info(f"Cached {len(configs)} validated configs")
            return True
        except Exception as e:
            self.logger.error(f"Failed to save validated configs: {e}")
            return False
    
    def load_validated_configs(self) -> List[Dict[str, Any]]:
        """Load validated configs."""
        try:
            if not self.validated_configs_file.exists():
                return []
            
            with open(self.validated_configs_file, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            configs = data.get('configs', [])
            self.logger.info(f"Loaded {len(configs)} validated configs")
            return configs
        except Exception as e:
            self.logger.error(f"Failed to load validated configs: {e}")
            return []
    
    def get_cache_status(self) -> Dict[str, Any]:
        """Get status of all caches."""
        return {
            'all_configs': {
                'file': str(self.all_configs_file),
                'exists': self.all_configs_file.exists(),
                'count': len(self.load_all_configs())
            },
            'active_configs': {
                'file': str(self.active_configs_file),
                'exists': self.active_configs_file.exists(),
                'count': len(self.load_active_configs())
            },
            'validated_configs': {
                'file': str(self.validated_configs_file),
                'exists': self.validated_configs_file.exists(),
                'count': len(self.load_validated_configs())
            }
        }
