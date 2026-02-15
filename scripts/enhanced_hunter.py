#!/usr/bin/env python3
"""
Enhanced Hunter with Adaptive Thread Management
Combines all features from old_hunter.py with new adaptive thread management system.
"""

import asyncio
import base64
import json
import logging
import os
import re
import shutil
import socket
import select
import struct
import socketserver
import subprocess
import sys
import tempfile
import time
from datetime import datetime
import threading
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple
from urllib.parse import parse_qs, unquote, urlparse

import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry
from urllib3.contrib.socks import SOCKSProxyManager
from colorama import Fore, Style, init

# Import adaptive thread management
from hunter.performance.adaptive_thread_manager import AdaptiveThreadPool, create_optimized_validator
from hunter.testing.benchmark import ProxyBenchmark
from hunter.core.config import HunterConfig
from hunter.orchestrator import HunterOrchestrator

# Import existing components
try:
    from hunter.telegram.interactive_auth import SmartTelegramAuth
    from hunter.config.dns_servers import DNSManager
    from hunter.security.obfuscation import AdversarialDPIExhaustionEngine
except ImportError:
    # Fallback if modules not available
    SmartTelegramAuth = None
    DNSManager = None
    AdversarialDPIExhaustionEngine = None

# Initialize colorama
init(autoreset=True)

# Configure logging
class ColoredFormatter(logging.Formatter):
    FORMAT = "%(asctime)s | %(levelname)s | %(message)s"
    DATEFMT = "%Y-%m-%d %H:%M:%S"
    FORMATS = {
        logging.DEBUG: Fore.CYAN + FORMAT + Style.RESET_ALL,
        logging.INFO: Fore.GREEN + FORMAT + Style.RESET_ALL,
        logging.WARNING: Fore.YELLOW + FORMAT + Style.RESET_ALL,
        logging.ERROR: Fore.RED + FORMAT + Style.RESET_ALL,
        logging.CRITICAL: Fore.RED + Style.BRIGHT + FORMAT + Style.RESET_ALL,
    }

    def format(self, record):
        log_fmt = self.FORMATS.get(record.levelno, self.FORMAT)
        formatter = logging.Formatter(log_fmt, datefmt=self.DATEFMT)
        return formatter.format(record)

# Setup logging
console_handler = logging.StreamHandler(sys.stdout)
console_handler.setLevel(logging.INFO)
console_handler.setFormatter(ColoredFormatter())
logging.basicConfig(level=logging.DEBUG, handlers=[console_handler])
logger = logging.getLogger(__name__)

# Suppress noisy libraries
logging.getLogger("urllib3.connectionpool").setLevel(logging.ERROR)
logging.getLogger("telethon").setLevel(logging.WARNING)
logging.getLogger("telethon.network").setLevel(logging.WARNING)
logging.getLogger("telethon.client.downloads").setLevel(logging.WARNING)

# Configuration from old hunter
GITHUB_REPOS = [
    "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt",
    "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt",
    "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt",
    "https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/main/all_extracted_configs.txt",
    "https://raw.githubusercontent.com/hamed1124/port-based-v2ray-configs/main/all.txt",
    "https://raw.githubusercontent.com/mostafasadeghifar/v2ray-config/main/configs.txt",
    "https://raw.githubusercontent.com/Ashkan-m/v2ray/main/Sub.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_STARTER.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix",
    "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/vveg26/get_proxy/main/dist/v2ray.txt",
]

ANTI_CENSORSHIP_SOURCES = [
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub1.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub2.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub3.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/All_Configs_Sub.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/configtg.txt",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vless",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/trojan",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vmess",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/all.txt",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
    "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/ss",
]

# Browser user agents
_BROWSER_USER_AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15",
]

def _random_user_agent() -> str:
    try:
        import random
        return random.choice(_BROWSER_USER_AGENTS)
    except Exception:
        return _BROWSER_USER_AGENTS[0]

def _create_session_with_pool(pool_connections: int = 20, pool_maxsize: int = 50, retries: int = 2) -> requests.Session:
    """Create a requests session with connection pooling for faster HTTP requests"""
    session = requests.Session()
    retry_strategy = Retry(
        total=retries,
        backoff_factor=0.3,
        status_forcelist=[500, 502, 503, 504],
    )
    adapter = HTTPAdapter(
        pool_connections=pool_connections,
        pool_maxsize=pool_maxsize,
        max_retries=retry_strategy
    )
    session.mount("http://", adapter)
    session.mount("https://", adapter)
    return session

# Global session for connection pooling
_HTTP_SESSION: Optional[requests.Session] = None

def _get_http_session() -> requests.Session:
    """Get or create the global HTTP session with connection pooling"""
    global _HTTP_SESSION
    if _HTTP_SESSION is None:
        _HTTP_SESSION = _create_session_with_pool()
    return _HTTP_SESSION

def _safe_b64decode(data: str) -> bytes:
    padded = data + "=" * (-len(data) % 4)
    return base64.b64decode(padded)

def _extract_raw_uris_from_text(text: str) -> Set[str]:
    """Extract raw URIs from text content"""
    uris = set()
    # Look for common proxy patterns
    patterns = [
        r'(vmess://[^\s\n]+)',
        r'(vless://[^\s\n]+)',
        r'(trojan://[^\s\n]+)',
        r'(ss://[^\s\n]+)',
        r'(ssr://[^\s\n]+)',
    ]
    
    for pattern in patterns:
        matches = re.findall(pattern, text, re.IGNORECASE)
        uris.update(matches)
    
    return uris

def _now_ts() -> int:
    """Get current timestamp"""
    return int(time.time())

def _read_lines(file_path: str) -> List[str]:
    """Read lines from file"""
    try:
        if os.path.exists(file_path):
            with open(file_path, "r", encoding="utf-8") as f:
                return [line.strip() for line in f if line.strip()]
    except Exception as e:
        logger.debug(f"Error reading {file_path}: {e}")
    return []

def _write_lines(file_path: str, lines: List[str]) -> None:
    """Write lines to file"""
    try:
        with open(file_path, "w", encoding="utf-8") as f:
            for line in lines:
                f.write(line + "\n")
    except Exception as e:
        logger.debug(f"Error writing {file_path}: {e}")

def _append_unique_lines(file_path: str, lines: List[str]) -> int:
    """Append unique lines to file"""
    try:
        existing = set(_read_lines(file_path))
        new_lines = [line for line in lines if line and line not in existing]
        if new_lines:
            with open(file_path, "a", encoding="utf-8") as f:
                for line in new_lines:
                    f.write(line + "\n")
        return len(new_lines)
    except Exception as e:
        logger.debug(f"Error appending to {file_path}: {e}")
    return 0

def _load_json(file_path: str, default: Any = None) -> Any:
    """Load JSON from file"""
    try:
        if os.path.exists(file_path):
            with open(file_path, "r", encoding="utf-8") as f:
                return json.load(f)
    except Exception as e:
        logger.debug(f"Error loading JSON {file_path}: {e}")
    return default or {}

def _save_json(file_path: str, data: Any) -> None:
    """Save data to JSON file"""
    try:
        with open(file_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    except Exception as e:
        logger.debug(f"Error saving JSON {file_path}: {e}")

def _load_env_file(file_path: str) -> None:
    """Load environment variables from file"""
    try:
        if os.path.exists(file_path):
            with open(file_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        key, value = line.split("=", 1)
                        os.environ[key.strip()] = value.strip()
    except Exception as e:
        logger.debug(f"Error loading env file {file_path}: {e}")

def _resolve_executable_path(name: str, preferred: str, fallbacks: List[str]) -> Optional[str]:
    """Resolve executable path"""
    paths = [preferred] + fallbacks
    for path in paths:
        if path and os.path.exists(path):
            return path
        if path and shutil.which(path):
            return path
    return None

def _is_tor_running() -> bool:
    """Check if Tor is running"""
    try:
        import socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        result = sock.connect_ex(("127.0.0.1", 9050))
        sock.close()
        return result == 0
    except:
        return False

class EnhancedConfigFetcher:
    """Enhanced config fetcher with adaptive thread management"""
    
    def __init__(self, thread_pool: AdaptiveThreadPool):
        self.thread_pool = thread_pool
        self.logger = logging.getLogger(__name__ + ".EnhancedConfigFetcher")
    
    def fetch_single_url(self, url: str, proxy_ports: List[int], timeout: int = 8) -> Set[str]:
        """Fetch a single URL with proxy fallback"""
        session = _get_http_session()
        headers = {"User-Agent": _random_user_agent()}

        # Try direct first
        try:
            resp = session.get(url, headers=headers, timeout=timeout, verify=False)
            if resp.status_code == 200 and len(resp.text) > 50:
                text = resp.text
                try:
                    decoded = base64.b64decode(text.strip()).decode('utf-8', errors='ignore')
                    if '://' in decoded:
                        text = decoded
                except:
                    pass
                found = _extract_raw_uris_from_text(text)
                if found:
                    return found
        except:
            pass

        # Try with SOCKS proxies
        for proxy_port in proxy_ports[:3]:
            try:
                proxies = {"http": f"socks5h://127.0.0.1:{proxy_port}", "https": f"socks5h://127.0.0.1:{proxy_port}"}
                resp = requests.get(url, headers=headers, timeout=timeout, verify=False, proxies=proxies)
                if resp.status_code == 200 and len(resp.text) > 50:
                    text = resp.text
                    try:
                        decoded = base64.b64decode(text.strip()).decode('utf-8', errors='ignore')
                        if '://' in decoded:
                            text = decoded
                    except:
                        pass
                    found = _extract_raw_uris_from_text(text)
                    if found:
                        return found
            except:
                continue
        return set()
    
    def fetch_github_configs_parallel(self, proxy_ports: List[int]) -> Set[str]:
        """Fetch configs from GitHub repos in parallel using adaptive thread pool"""
        configs = set()
        
        # Submit fetch tasks to thread pool
        futures = []
        for url in GITHUB_REPOS:
            future = self.thread_pool.submit(self.fetch_single_url, url, proxy_ports, 10)
            futures.append(future)
        
        # Collect results
        for future in futures:
            try:
                result = future.result(timeout=15)
                if result:
                    configs.update(result)
            except Exception as e:
                self.logger.debug(f"GitHub fetch task failed: {e}")
        
        return configs
    
    def fetch_anti_censorship_configs(self, proxy_ports: List[int]) -> Set[str]:
        """Fetch configs from anti-censorship sources using adaptive thread pool"""
        configs = set()
        self.logger.info(f"Fetching from {len(ANTI_CENSORSHIP_SOURCES)} anti-censorship sources...")
        
        # Submit fetch tasks to thread pool
        futures = []
        for url in ANTI_CENSORSHIP_SOURCES:
            future = self.thread_pool.submit(self.fetch_single_url, url, proxy_ports, 15)
            futures.append(future)
        
        # Collect results
        for future in futures:
            try:
                result = future.result(timeout=20)
                if result:
                    configs.update(result)
            except Exception as e:
                self.logger.debug(f"Anti-censorship fetch task failed: {e}")
        
        if configs:
            self.logger.info(f"Anti-censorship sources provided {len(configs)} configs")
        return configs

class EnhancedHunter:
    """Enhanced Hunter with adaptive thread management and all old features"""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__ + ".EnhancedHunter")
        
        # Initialize adaptive thread pool
        self.thread_pool = AdaptiveThreadPool(
            min_threads=8,
            max_threads=32,
            target_cpu_utilization=0.85,
            target_queue_size=200,
            enable_work_stealing=True,
            enable_cpu_affinity=False
        )
        
        # Initialize components
        self.config_fetcher = EnhancedConfigFetcher(self.thread_pool)
        self.benchmarker = ProxyBenchmark()
        self.orchestrator = HunterOrchestrator()
        
        # Initialize optional components
        self.dns_manager = DNSManager() if DNSManager else None
        self.stealth_engine = AdversarialDPIExhaustionEngine() if AdversarialDPIExhaustionEngine else None
        
        # State tracking
        self.state = {}
        self.smart_cache = SmartCache()
        
        # Performance metrics
        self.start_time = time.time()
        self.total_configs_processed = 0
        self.total_validations = 0
        
        self.logger.info("Enhanced Hunter initialized with adaptive thread management")
    
    def start(self):
        """Start the enhanced hunter"""
        self.thread_pool.start()
        self.benchmarker.start_thread_pool()
        
        if self.stealth_engine:
            self.stealth_engine.start()
        
        self.logger.info("Enhanced Hunter started")
    
    def stop(self):
        """Stop the enhanced hunter"""
        self.thread_pool.stop()
        self.benchmarker.stop_thread_pool()
        
        if self.stealth_engine:
            self.stealth_engine.stop()
        
        self.logger.info("Enhanced Hunter stopped")
    
    def fetch_all_configs(self, proxy_ports: List[int]) -> Set[str]:
        """Fetch configs from all sources using adaptive thread management"""
        all_configs = set()
        
        # Start thread pool if not running
        if not self.thread_pool.running:
            self.thread_pool.start()
        
        # Fetch from GitHub repos
        self.logger.info("Fetching configs from GitHub repos...")
        github_configs = self.config_fetcher.fetch_github_configs_parallel(proxy_ports)
        if github_configs:
            self.logger.info(f"Got {len(github_configs)} configs from GitHub")
            all_configs.update(github_configs)
        
        # Fetch from anti-censorship sources
        anti_censor_configs = self.config_fetcher.fetch_anti_censorship_configs(proxy_ports)
        if anti_censor_configs:
            all_configs.update(anti_censor_configs)
        
        # Load cached configs
        cached_configs = self.smart_cache.load_cached_configs(max_count=2000)
        if cached_configs:
            self.logger.info(f"Loaded {len(cached_configs)} configs from cache")
            all_configs.update(cached_configs)
        
        self.total_configs_processed += len(all_configs)
        return all_configs
    
    def validate_configs_enhanced(self, configs: List[str], timeout: int = 10) -> List:
        """Validate configs using enhanced thread management"""
        if not configs:
            return []
        
        self.logger.info(f"Validating {len(configs)} configs with enhanced thread management")
        
        # Use orchestrator's enhanced validation
        results = self.orchestrator.validate_configs(configs, timeout=timeout)
        self.total_validations += len(configs)
        
        # Get performance metrics
        metrics = self.benchmarker.get_performance_metrics()
        self.logger.info(f"Validation performance: {metrics['thread_pool_metrics']['tasks_per_second']:.1f} tasks/sec")
        
        return results
    
    def get_performance_metrics(self) -> Dict[str, Any]:
        """Get comprehensive performance metrics"""
        thread_metrics = self.thread_pool.get_metrics()
        benchmark_metrics = self.benchmarker.get_performance_metrics()
        
        elapsed_time = time.time() - self.start_time
        
        return {
            "thread_pool": {
                "total_tasks": thread_metrics.total_tasks,
                "completed_tasks": thread_metrics.completed_tasks,
                "failed_tasks": thread_metrics.failed_tasks,
                "tasks_per_second": thread_metrics.tasks_per_second,
                "cpu_utilization": thread_metrics.cpu_utilization,
                "memory_utilization": thread_metrics.memory_utilization,
                "thread_utilization": thread_metrics.thread_utilization,
                "queue_size": thread_metrics.queue_size
            },
            "benchmark": benchmark_metrics.get("thread_pool_metrics", {}),
            "hunter": {
                "total_configs_processed": self.total_configs_processed,
                "total_validations": self.total_validations,
                "elapsed_time": elapsed_time,
                "configs_per_second": self.total_configs_processed / elapsed_time if elapsed_time > 0 else 0,
                "validations_per_second": self.total_validations / elapsed_time if elapsed_time > 0 else 0
            },
            "dns": self.dns_manager.get_status() if self.dns_manager else {},
            "stealth": self.stealth_engine.get_metrics() if self.stealth_engine else {}
        }
    
    def run_cycle(self, config: Dict[str, Any]) -> Dict[str, Any]:
        """Run a single enhanced hunter cycle"""
        cycle_start = time.time()
        
        # Load configuration
        self._load_config(config)
        
        # Prepare proxy ports
        proxy_ports = self._prepare_proxy_ports()
        
        # Fetch configs
        configs = self.fetch_all_configs(proxy_ports)
        
        # Validate configs
        validated = self.validate_configs_enhanced(list(configs))
        
        # Categorize results
        gold = [r for r in validated if hasattr(r, 'tier') and r.tier == "gold"]
        silver = [r for r in validated if hasattr(r, 'tier') and r.tier == "silver"]
        
        # Save results
        self._save_results(gold, silver)
        
        # Get performance metrics
        metrics = self.get_performance_metrics()
        
        cycle_time = time.time() - cycle_start
        
        results = {
            "cycle_time": cycle_time,
            "configs_fetched": len(configs),
            "configs_validated": len(validated),
            "gold_configs": len(gold),
            "silver_configs": len(silver),
            "performance_metrics": metrics
        }
        
        self.logger.info(f"Cycle completed in {cycle_time:.1f}s: {len(validated)} validated configs")
        return results
    
    def _load_config(self, config: Dict[str, Any]):
        """Load configuration"""
        # Implementation for loading config
        pass
    
    def _prepare_proxy_ports(self) -> List[int]:
        """Prepare proxy ports for fetching"""
        proxy_ports = [1080, 10808, 7890, 2080]
        
        if _is_tor_running():
            proxy_ports.append(9050)
            self.logger.info("Tor detected, added to proxy ports")
        
        return proxy_ports
    
    def _save_results(self, gold: List, silver: List):
        """Save validation results"""
        # Save gold configs
        if gold:
            gold_uris = [r.uri for r in gold if hasattr(r, 'uri')]
            _write_lines("HUNTER_gold.txt", gold_uris)
            self.logger.info(f"Saved {len(gold)} gold configs")
        
        # Save silver configs
        if silver:
            silver_uris = [r.uri for r in silver if hasattr(r, 'uri')]
            _write_lines("HUNTER_silver.txt", silver_uris)
            self.logger.info(f"Saved {len(silver)} silver configs")
        
        # Save to cache
        working_configs = set()
        for r in gold + silver:
            if hasattr(r, 'uri'):
                working_configs.add(r.uri)
        
        if working_configs:
            self.smart_cache.save_configs(working_configs, working=True)

class SmartCache:
    """Smart caching for configs with failure tracking"""
    
    def __init__(self, cache_file: str = "subscriptions_cache.txt",
                 working_cache_file: str = "working_configs_cache.txt"):
        self.cache_file = cache_file
        self.working_cache_file = working_cache_file
        self._last_successful_fetch = 0
        self._consecutive_failures = 0
        self.logger = logging.getLogger(__name__ + ".SmartCache")
    
    def save_configs(self, configs: Set[str], working: bool = False) -> int:
        """Save configs to cache file"""
        target_file = self.working_cache_file if working else self.cache_file
        try:
            existing = set(_read_lines(target_file))
            new_configs = configs - existing
            if new_configs:
                with open(target_file, "a", encoding="utf-8") as f:
                    for cfg in new_configs:
                        if cfg:
                            f.write(cfg + "\n")
            self._last_successful_fetch = _now_ts()
            self._consecutive_failures = 0
            return len(new_configs)
        except Exception as e:
            self.logger.debug(f"Cache save error: {e}")
            return 0
    
    def load_cached_configs(self, max_count: int = 1000, working_only: bool = False) -> Set[str]:
        """Load cached configs from file"""
        configs = set()
        try:
            if working_only:
                lines = _read_lines(self.working_cache_file)
            else:
                lines = _read_lines(self.cache_file)
                lines.extend(_read_lines(self.working_cache_file))
            
            for line in lines[-max_count:]:
                if line and '://' in line:
                    configs.add(line)
        except Exception as e:
            self.logger.debug(f"Cache load error: {e}")
        return configs
    
    def record_failure(self):
        """Record a connection failure"""
        self._consecutive_failures += 1
    
    def should_use_cache(self) -> bool:
        """Check if cache should be used due to failures"""
        return self._consecutive_failures >= 2
    
    def get_failure_count(self) -> int:
        return self._consecutive_failures

async def run_enhanced_hunter():
    """Run the enhanced hunter with all features"""
    print(f"Python {sys.version} on {sys.platform}")
    print("Enhanced Hunter with Adaptive Thread Management")
    print("=" * 60)
    
    # Load environment
    base_dir = Path(__file__).resolve().parent
    secrets_file = str(base_dir / "hunter_secrets.env")
    _load_env_file(secrets_file)
    
    # Create enhanced hunter
    hunter = EnhancedHunter()
    
    try:
        # Start hunter
        hunter.start()
        
        # Configuration
        config = {
            "max_configs": int(os.getenv("HUNTER_MAX_CONFIGS", "3000")),
            "timeout": int(os.getenv("HUNTER_TEST_TIMEOUT", "10")),
            "sleep_seconds": int(os.getenv("HUNTER_SLEEP", "300")),
        }
        
        # Main loop
        cycle_count = 0
        while True:
            cycle_count += 1
            logger.info(f"\n{'=' * 80}")
            logger.info(f"ENHANCED CYCLE {cycle_count} STARTING")
            logger.info(f"{'=' * 80}")
            
            # Run cycle
            results = hunter.run_cycle(config)
            
            # Log results
            logger.info(f"Cycle Results:")
            logger.info(f"  Configs fetched: {results['configs_fetched']}")
            logger.info(f"  Configs validated: {results['configs_validated']}")
            logger.info(f"  Gold configs: {results['gold_configs']}")
            logger.info(f"  Silver configs: {results['silver_configs']}")
            logger.info(f"  Cycle time: {results['cycle_time']:.1f}s")
            
            # Log performance metrics
            metrics = results['performance_metrics']
            logger.info(f"Performance Metrics:")
            logger.info(f"  Tasks/sec: {metrics['thread_pool']['tasks_per_second']:.1f}")
            logger.info(f"  CPU utilization: {metrics['thread_pool']['cpu_utilization']:.1f}%")
            logger.info(f"  Memory utilization: {metrics['thread_pool']['memory_utilization']:.1f}%")
            logger.info(f"  Thread utilization: {metrics['thread_pool']['thread_utilization']:.1f}%")
            logger.info(f"  Configs/sec: {metrics['hunter']['configs_per_second']:.1f}")
            logger.info(f"  Validations/sec: {metrics['hunter']['validations_per_second']:.1f}")
            
            # Sleep before next cycle
            logger.info(f"Sleeping for {config['sleep_seconds']}s before next cycle...")
            await asyncio.sleep(config['sleep_seconds'])
    
    except KeyboardInterrupt:
        logger.info("Received interrupt signal, shutting down...")
    except Exception as e:
        logger.error(f"Enhanced hunter error: {e}")
    finally:
        # Cleanup
        hunter.stop()
        logger.info("Enhanced hunter stopped")

if __name__ == "__main__":
    asyncio.run(run_enhanced_hunter())
