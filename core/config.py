"""
Core configuration management for the Hunter system.

This module handles all configuration loading, validation, and management
including environment variables and settings files.
"""

import json
import logging
import os
import re
import sys
from typing import Any, Dict, List, Optional


class HunterConfig:
    """Central configuration manager for the Hunter system."""
    
    def __init__(self, secrets_file: Optional[str] = None):
        self.secrets_file = secrets_file or os.getenv("HUNTER_SECRETS_FILE", "hunter_secrets.env")
        self._config: Dict[str, Any] = {}
        self._load_default_config()
        self._load_env_file()
        self._load_from_environment()
    
    def _load_default_config(self):
        """Load default configuration values."""
        runtime_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "runtime")
        
        self._config = {
            # Telegram Configuration
            # Note: These will be overridden by _load_env_file() and _load_from_environment()
            "api_id": None,
            "api_hash": None,
            "phone": None,
            "bot_token": None,
            "report_channel": None,
            "session_name": "hunter_session",
            "telegram_limit": 50,
            
            # Target Channels
            "targets": [
                t.strip() for t in os.getenv("HUNTER_TARGETS", "").split(",") 
                if t.strip()
            ] or [
                "v2rayngvpn", "mitivpn", "proxymtprotoir", "Porteqal3", 
                "v2ray_configs_pool", "vmessorg", "V2rayNGn", "v2ray_swhil",
                "VmessProtocol", "PrivateVPNs", "DirectVPN", "v2rayNG_Matsuri",
                "FalconPolV2rayNG", "ShadowSocks_s", "napsternetv_config",
                "VlessConfig", "iP_CF", "ConfigsHUB"
            ],
            
            # Paths (use runtime directory)
            "xray_path": os.getenv("HUNTER_XRAY_PATH", ""),
            "state_file": os.getenv("HUNTER_STATE_FILE", os.path.join(runtime_dir, "HUNTER_state.json")),
            "raw_file": os.getenv("HUNTER_RAW_FILE", os.path.join(runtime_dir, "HUNTER_raw.txt")),
            "gold_file": os.getenv("HUNTER_GOLD_FILE", os.path.join(runtime_dir, "HUNTER_gold.txt")),
            "silver_file": os.getenv("HUNTER_SILVER_FILE", os.path.join(runtime_dir, "HUNTER_silver.txt")),
            "bridge_pool_file": os.getenv("HUNTER_BRIDGE_POOL_FILE", os.path.join(runtime_dir, "HUNTER_bridge_pool.txt")),
            "validated_jsonl": os.getenv("HUNTER_VALIDATED_JSONL", os.path.join(runtime_dir, "HUNTER_validated.jsonl")),
            
            # Testing Configuration
            "test_url": os.getenv("HUNTER_TEST_URL", "https://www.cloudflare.com/cdn-cgi/trace"),
            "google_test_url": os.getenv("HUNTER_GOOGLE_TEST_URL", "https://www.google.com/generate_204"),
            "scan_limit": int(os.getenv("HUNTER_SCAN_LIMIT", "50")),
            "latest_total": int(os.getenv("HUNTER_LATEST_URIS", "500")),
            "max_total": int(os.getenv("HUNTER_MAX_CONFIGS", "1500")),  # Reduced from 3000 to 1500
            "npvt_scan_limit": int(os.getenv("HUNTER_NPVT_SCAN", "50")),
            "max_workers": int(os.getenv("HUNTER_WORKERS", "10")),  # Reduced from 50 to 10
            "timeout_seconds": int(os.getenv("HUNTER_TEST_TIMEOUT", "10")),
            
            # Timing Configuration
            "sleep_seconds": int(os.getenv("HUNTER_SLEEP", "300")),
            "cleanup_interval": int(os.getenv("HUNTER_CLEANUP", str(24 * 3600))),
            "recursive_ratio": float(os.getenv("HUNTER_RECURSIVE_RATIO", "0.15")),
            
            # Bridge Configuration
            "max_bridges": int(os.getenv("HUNTER_MAX_BRIDGES", "8")),
            "bridge_base": int(os.getenv("HUNTER_BRIDGE_BASE", "11808")),
            "bench_base": int(os.getenv("HUNTER_BENCH_BASE", "12808")),
            
            # MultiProxy Configuration
            "multiproxy_port": int(os.getenv("HUNTER_MULTIPROXY_PORT", "10808")),
            "multiproxy_backends": int(os.getenv("HUNTER_MULTIPROXY_BACKENDS", "5")),
            "multiproxy_health_interval": int(os.getenv("HUNTER_MULTIPROXY_HEALTH_INTERVAL", "60")),
            "gemini_balancer_enabled": os.getenv("HUNTER_GEMINI_BALANCER", "true").lower() == "true",
            "gemini_port": int(os.getenv("HUNTER_GEMINI_PORT", "10809")),
            
            # Connection Configuration
            "connect_tries": int(os.getenv("HUNTER_CONNECT_TRIES", "4")),
            
            # Feature Flags
            "adee_enabled": os.getenv("ADEE_ENABLED", "true").lower() == "true",
            "iran_fragment_enabled": os.getenv("IRAN_FRAGMENT_ENABLED", "false").lower() == "true",
            "gateway_enabled": os.getenv("GATEWAY_ENABLED", "false").lower() == "true",
            "web_server_enabled": os.getenv("HUNTER_WEB_SERVER", "true").lower() == "true",
            "web_server_port": int(os.getenv("HUNTER_WEB_PORT", "8080")),
            
            # Gateway Configuration
            "gateway_socks_port": int(os.getenv("GATEWAY_SOCKS_PORT", "10808")),
            "gateway_http_port": int(os.getenv("GATEWAY_HTTP_PORT", "10809")),
            "gateway_dns_port": int(os.getenv("GATEWAY_DNS_PORT", "53")),
            
            # === 2026 DPI Evasion Configuration ===
            # DPI Evasion Orchestrator
            "dpi_evasion_enabled": os.getenv("HUNTER_DPI_EVASION", "true").lower() == "true",
            "dpi_evasion_adaptive": os.getenv("HUNTER_DPI_ADAPTIVE", "true").lower() == "true",
            
            # TLS Fingerprint Evasion (JA3/JA4 spoofing)
            "tls_fingerprint_enabled": os.getenv("HUNTER_TLS_FP_EVASION", "true").lower() == "true",
            "tls_fingerprint_prefer_h2": os.getenv("HUNTER_TLS_FP_H2", "true").lower() == "true",
            "tls_fingerprint_rotation_interval": int(os.getenv("HUNTER_TLS_FP_ROTATION", "120")),
            
            # TLS ClientHello Fragmentation
            "tls_fragment_enabled": os.getenv("HUNTER_TLS_FRAGMENT", "true").lower() == "true",
            "tls_fragment_strategy": os.getenv("HUNTER_TLS_FRAG_STRATEGY", "three_part"),
            "tls_fragment_min_delay": float(os.getenv("HUNTER_TLS_FRAG_MIN_DELAY", "10")),
            "tls_fragment_max_delay": float(os.getenv("HUNTER_TLS_FRAG_MAX_DELAY", "50")),
            "tls_fragment_min_size": int(os.getenv("HUNTER_TLS_FRAG_MIN_SIZE", "1")),
            "tls_fragment_max_size": int(os.getenv("HUNTER_TLS_FRAG_MAX_SIZE", "200")),
            
            # VLESS-Reality-Vision defaults
            "reality_dest": os.getenv("HUNTER_REALITY_DEST", "swdist.apple.com:443"),
            "reality_server_names": os.getenv("HUNTER_REALITY_SNI", "swdist.apple.com,www.apple.com"),
            
            # MTU Optimizer (5G PMTUD attack mitigation)
            "mtu_optimization_enabled": os.getenv("HUNTER_MTU_OPT", "true").lower() == "true",
            "mtu_mobile_4g": int(os.getenv("HUNTER_MTU_4G", "1350")),
            "mtu_mobile_5g": int(os.getenv("HUNTER_MTU_5G", "1280")),
            "mtu_fiber": int(os.getenv("HUNTER_MTU_FIBER", "1500")),
            
            # Active Probe Defense
            "probe_defense_enabled": os.getenv("HUNTER_PROBE_DEFENSE", "true").lower() == "true",
            "probe_fallback_site": os.getenv("HUNTER_PROBE_FALLBACK", "www.apple.com"),
            
            # Entropy Normalization
            "entropy_normalization_enabled": os.getenv("HUNTER_ENTROPY_NORM", "true").lower() == "true",
            
            # UDP Protocol support (Hysteria2/TUIC)
            "hysteria2_enabled": os.getenv("HUNTER_HY2_ENABLED", "true").lower() == "true",
            "tuic_enabled": os.getenv("HUNTER_TUIC_ENABLED", "true").lower() == "true",
            "udp_port_hopping": os.getenv("HUNTER_UDP_HOP", "true").lower() == "true",
            "udp_port_range": os.getenv("HUNTER_UDP_PORT_RANGE", "20000-40000"),
            "udp_hop_interval": int(os.getenv("HUNTER_UDP_HOP_INTERVAL", "30")),
        }
    
    def _load_env_file(self):
        """Load configuration from environment file."""
        try:
            if not self.secrets_file or not os.path.exists(self.secrets_file):
                return
            
            with open(self.secrets_file, "r", encoding="utf-8") as f:
                for raw_line in f:
                    line = raw_line.strip()
                    if not line or line.startswith("#"):
                        continue
                    
                    key = None
                    value = None
                    
                    # Handle PowerShell environment files
                    if line.lower().startswith("$env:"):
                        m = re.match(r"^\$env:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", line)
                        if m:
                            key = m.group(1).strip()
                            value = m.group(2).strip()
                    # Handle standard environment files
                    elif "=" in line:
                        left, right = line.split("=", 1)
                        key = left.strip()
                        value = right.strip()
                    
                    if key and value is not None:
                        # Remove quotes if present
                        if value.startswith('"') and value.endswith('"') and len(value) >= 2:
                            value = value[1:-1]
                        elif value.startswith("'") and value.endswith("'") and len(value) >= 2:
                            value = value[1:-1]
                        
                        os.environ.setdefault(key, value)
        except Exception as e:
            logging.getLogger(__name__).warning(f"Failed to load env file: {e}")
    
    def _load_from_environment(self):
        """Override configuration with environment variables."""
        env_mappings = {
            # Telegram credentials - support multiple env var names
            "HUNTER_API_ID": ("api_id", int),
            "TELEGRAM_API_ID": ("api_id", int),
            "HUNTER_API_HASH": ("api_hash", str),
            "TELEGRAM_API_HASH": ("api_hash", str),
            "HUNTER_PHONE": ("phone", str),
            "TELEGRAM_PHONE": ("phone", str),
            "TOKEN": ("bot_token", str),
            "TELEGRAM_BOT_TOKEN": ("bot_token", str),
            "CHAT_ID": ("report_channel", lambda x: int(x) if x.lstrip('-').isdigit() else x),
            "TELEGRAM_GROUP_ID": ("report_channel", lambda x: int(x) if x.lstrip('-').isdigit() else x),
            "HUNTER_SESSION": ("session_name", str),
            "TELEGRAM_SESSION": ("session_name", str),
            "HUNTER_TELEGRAM_LIMIT": ("telegram_limit", int),
            "HUNTER_XRAY_PATH": ("xray_path", str),
            "HUNTER_TEST_URL": ("test_url", str),
            "HUNTER_GOOGLE_TEST_URL": ("google_test_url", str),
            "HUNTER_SCAN_LIMIT": ("scan_limit", int),
            "HUNTER_LATEST_URIS": ("latest_total", int),
            "HUNTER_MAX_CONFIGS": ("max_total", int),
            "HUNTER_NPVT_SCAN": ("npvt_scan_limit", int),
            "HUNTER_WORKERS": ("max_workers", int),
            "HUNTER_TEST_TIMEOUT": ("timeout_seconds", int),
            "HUNTER_SLEEP": ("sleep_seconds", int),
            "HUNTER_CLEANUP": ("cleanup_interval", int),
            "HUNTER_RECURSIVE_RATIO": ("recursive_ratio", float),
            "HUNTER_MAX_BRIDGES": ("max_bridges", int),
            "HUNTER_BRIDGE_BASE": ("bridge_base", int),
            "HUNTER_BENCH_BASE": ("bench_base", int),
            "HUNTER_MULTIPROXY_PORT": ("multiproxy_port", int),
            "HUNTER_MULTIPROXY_BACKENDS": ("multiproxy_backends", int),
            "HUNTER_MULTIPROXY_HEALTH_INTERVAL": ("multiproxy_health_interval", int),
            "HUNTER_GEMINI_BALANCER": ("gemini_balancer_enabled", lambda x: x.lower() == "true"),
            "HUNTER_GEMINI_PORT": ("gemini_port", int),
            "HUNTER_CONNECT_TRIES": ("connect_tries", int),
            "ADEE_ENABLED": ("adee_enabled", lambda x: x.lower() == "true"),
            "IRAN_FRAGMENT_ENABLED": ("iran_fragment_enabled", lambda x: x.lower() == "true"),
            "GATEWAY_ENABLED": ("gateway_enabled", lambda x: x.lower() == "true"),
            "HUNTER_WEB_SERVER": ("web_server_enabled", lambda x: x.lower() == "true"),
            "HUNTER_WEB_PORT": ("web_server_port", int),
            "GATEWAY_SOCKS_PORT": ("gateway_socks_port", int),
            "GATEWAY_HTTP_PORT": ("gateway_http_port", int),
            "GATEWAY_DNS_PORT": ("gateway_dns_port", int),
        }
        
        for env_key, (config_key, converter) in env_mappings.items():
            if env_key in os.environ:
                try:
                    self._config[config_key] = converter(os.environ[env_key])
                except (ValueError, TypeError) as e:
                    logging.getLogger(__name__).warning(
                        f"Invalid value for {env_key}: {os.environ[env_key]}"
                    )
    
    def get(self, key: str, default: Any = None) -> Any:
        """Get configuration value."""
        return self._config.get(key, default)
    
    def set(self, key: str, value: Any):
        """Set configuration value."""
        self._config[key] = value
    
    def update(self, config_dict: Dict[str, Any]):
        """Update multiple configuration values."""
        self._config.update(config_dict)
    
    def to_dict(self) -> Dict[str, Any]:
        """Get configuration as dictionary."""
        return self._config.copy()
    
    def validate(self) -> List[str]:
        """Validate configuration and return list of errors."""
        errors = []
        warnings = []
        
        # Telegram credentials are now OPTIONAL - just warn if missing
        api_id = self.get("api_id")
        if not api_id:
            warnings.append("HUNTER_API_ID not set - Telegram features will be disabled")
        
        api_hash = self.get("api_hash", "")
        if not api_hash or api_hash == "":
            warnings.append("HUNTER_API_HASH not set - Telegram features will be disabled")
        
        phone = self.get("phone", "")
        if not phone or phone == "":
            warnings.append("HUNTER_PHONE not set - Telegram features will be disabled")
        
        # Log warnings but don't treat as errors
        if warnings:
            logger = logging.getLogger(__name__)
            logger.info("Configuration warnings (non-critical):")
            for warning in warnings:
                logger.info(f"  - {warning}")
        
        # Validate numeric values (these ARE required)
        numeric_fields = [
            ("scan_limit", 1, 1000),
            ("max_total", 1, 10000),
            ("max_workers", 1, 200),
            ("timeout_seconds", 1, 60),
            ("telegram_limit", 1, 500),
            ("sleep_seconds", 10, 3600),
        ]
        
        for field, min_val, max_val in numeric_fields:
            value = self.get(field)
            if not isinstance(value, int) or value < min_val or value > max_val:
                errors.append(f"{field} must be between {min_val} and {max_val}")
        
        return errors
    
    def is_windows(self) -> bool:
        """Check if running on Windows."""
        return sys.platform == "win32"
    
    def get_executable_paths(self) -> Dict[str, List[str]]:
        """Get fallback paths for executables."""
        hunter_bin = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "bin")
        return {
            "xray": [
                os.path.join(hunter_bin, "xray.exe")
            ],
            "tor": [
                os.path.join(hunter_bin, "tor.exe")
            ],
            "mihomo": [
                os.path.join(hunter_bin, "mihomo-windows-amd64-compatible.exe")
            ],
            "sing-box": [
                os.path.join(hunter_bin, "sing-box.exe")
            ],
            "chromedriver": [
                os.path.join(hunter_bin, "chromedriver.exe")
            ],
            "amaztool": [
                os.path.join(hunter_bin, "AmazTool.exe")
            ]
        }
