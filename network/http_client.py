"""
Network operations and HTTP client management.

This module handles HTTP requests, connection pooling, and network
operations for fetching proxy configurations.
"""

import base64
import logging
import random
import subprocess
import sys
import time
from concurrent.futures import as_completed
from typing import List, Optional, Set

try:
    from hunter.core.task_manager import HunterTaskManager
except ImportError:
    from core.task_manager import HunterTaskManager

import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

try:
    from hunter.core.utils import extract_raw_uris_from_text, safe_b64decode
except ImportError:
    from core.utils import extract_raw_uris_from_text, safe_b64decode

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Browser user agents for rotation
BROWSER_USER_AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15",
]

# GitHub repositories for configuration sources
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

# Anti-censorship sources (Reality-focused, CDN-hosted)
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

# Iran priority sources (Reality-focused - best for bypassing DPI)
IRAN_PRIORITY_SOURCES = [
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality",
    "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt",
    "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt",
    "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/vless.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt",
    "https://raw.githubusercontent.com/sarinaesmailzadeh/V2Hub/main/merged_base64",
    "https://raw.githubusercontent.com/LalatinaHub/Starter/main/Starter",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless",
]

# NapsternetV subscription URLs
NAPSTERV_SUBSCRIPTION_URLS = [
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/V2Ray.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
]


class HTTPClientManager:
    """Manages HTTP sessions with memory-aware connection pooling."""
    
    def __init__(self):
        self._session: Optional[requests.Session] = None
        self._pool_size: int = 10  # default conservative
    
    def _get_memory_pct(self) -> float:
        """Get current memory usage percentage."""
        try:
            import psutil
            return psutil.virtual_memory().percent
        except Exception:
            return 50.0
    
    def _create_session(self, pool_connections: int = 10, pool_maxsize: int = 10, retries: int = 2) -> requests.Session:
        """Create a requests session with connection pooling."""
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
    
    def get_session(self) -> requests.Session:
        """Get or create the global HTTP session with memory-aware pooling."""
        if self._session is None:
            mem = self._get_memory_pct()
            if mem >= 90:
                pool_conn, pool_max = 3, 5
            elif mem >= 80:
                pool_conn, pool_max = 6, 15
            elif mem >= 70:
                pool_conn, pool_max = 8, 15
            else:
                pool_conn, pool_max = 10, 20
            self._pool_size = pool_max
            self._session = self._create_session(pool_conn, pool_max)
        return self._session
    
    def reset_session(self):
        """Close and reset session (e.g. after memory pressure)."""
        if self._session:
            try:
                self._session.close()
            except Exception:
                pass
            self._session = None
    
    def random_user_agent(self) -> str:
        """Get a random user agent."""
        try:
            return random.choice(BROWSER_USER_AGENTS)
        except Exception:
            return BROWSER_USER_AGENTS[0]


class ConfigFetcher:
    """Fetches proxy configurations from various sources with circuit breaker."""
    
    def __init__(self, http_manager: HTTPClientManager):
        self.http_manager = http_manager
        self.logger = logging.getLogger(__name__)
        self._failed_urls: dict = {}  # url -> (fail_count, last_fail_time)
        self._circuit_breaker_threshold = 3  # failures before skipping
        self._circuit_breaker_reset = 600  # seconds before retrying failed URL
    
    def _is_circuit_open(self, url: str) -> bool:
        """Check if circuit breaker is open (URL should be skipped)."""
        if url not in self._failed_urls:
            return False
        fail_count, last_fail = self._failed_urls[url]
        if fail_count >= self._circuit_breaker_threshold:
            if time.time() - last_fail < self._circuit_breaker_reset:
                return True
            # Reset after cooldown
            del self._failed_urls[url]
        return False
    
    def _record_failure(self, url: str):
        """Record a URL fetch failure for circuit breaker."""
        if url in self._failed_urls:
            count, _ = self._failed_urls[url]
            self._failed_urls[url] = (count + 1, time.time())
        else:
            self._failed_urls[url] = (1, time.time())
    
    def _record_success(self, url: str):
        """Reset circuit breaker on success."""
        self._failed_urls.pop(url, None)

    def _try_decode_and_extract(self, text: str) -> Set[str]:
        """Try base64 decode then extract URIs."""
        try:
            decoded = base64.b64decode(text.strip()).decode('utf-8', errors='ignore')
            if '://' in decoded:
                text = decoded
        except:
            pass
        return extract_raw_uris_from_text(text)

    def _expand_mirrors(self, url: str) -> List[str]:
        if not isinstance(url, str) or not url:
            return []
        if url.startswith("https://raw.githubusercontent.com/"):
            return [
                url,
                f"https://ghproxy.com/{url}",
                f"https://ghproxy.net/{url}",
            ]
        return [url]

    def _is_port_alive(self, port: int) -> bool:
        """Quick check if a local port is accepting TCP connections."""
        import socket
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except Exception:
            return False

    def _is_proxy_fast_enough(self, port: int, max_latency_ms: float = 800) -> bool:
        """Check if local proxy port responds fast enough to be worth using."""
        import socket
        try:
            t0 = time.time()
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                pass
            latency_ms = (time.time() - t0) * 1000
            return latency_ms < max_latency_ms
        except Exception:
            return False

    def _fetch_single_url_fast(self, url: str, proxy_ports: List[int], timeout: int = 8) -> Set[str]:
        """Fast fetch using requests session with connection pooling."""
        if self._is_circuit_open(url):
            return set()
        
        session = self.http_manager.get_session()
        headers = {"User-Agent": self.http_manager.random_user_agent()}
        
        for candidate in self._expand_mirrors(url):
            try:
                resp = session.get(candidate, headers=headers, timeout=timeout, verify=False)
                if resp.status_code == 200 and resp.text:
                    found = self._try_decode_and_extract(resp.text)
                    if found:
                        self._record_success(url)
                        return found
            except Exception:
                continue
        
        live_ports = [p for p in proxy_ports[:1] if self._is_port_alive(p)]
        if live_ports and self._is_proxy_fast_enough(live_ports[0]):
            proxy_port = live_ports[0]
            proxies = {"http": f"socks5h://127.0.0.1:{proxy_port}", "https": f"socks5h://127.0.0.1:{proxy_port}"}
            for candidate in self._expand_mirrors(url):
                try:
                    resp = session.get(candidate, headers=headers, timeout=min(timeout, 6), verify=False, proxies=proxies)
                    if resp.status_code == 200 and resp.text:
                        found = self._try_decode_and_extract(resp.text)
                        if found:
                            self._record_success(url)
                            return found
                except Exception:
                    continue
        
        self._record_failure(url)
        return set()
    
    def _fetch_single_url(self, url: str, proxy_ports: List[int], timeout: int = 12) -> Set[str]:
        """Fetch a single URL, trying fast method first then curl fallback."""
        if self._is_circuit_open(url):
            return set()
        
        # Try fast method first
        result = self._fetch_single_url_fast(url, proxy_ports, timeout=min(timeout, 8))
        if result:
            return result
        
        # Fallback to curl for stubborn URLs
        curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
        
        for candidate in self._expand_mirrors(url):
            try:
                cmd = [
                    curl_cmd,
                    "-s",
                    "-m", str(timeout),
                    "-k",
                    "--connect-timeout", "4",
                    "-A", self.http_manager.random_user_agent(),
                    candidate,
                ]
                proc_result = subprocess.run(cmd, capture_output=True, timeout=timeout + 2)
                if proc_result.returncode == 0 and proc_result.stdout:
                    text = proc_result.stdout.decode('utf-8', errors='ignore')
                    found = self._try_decode_and_extract(text)
                    if found:
                        self._record_success(url)
                        return found
            except Exception:
                continue
        
        self._record_failure(url)
        return set()
    
    def _get_task_manager(self) -> HunterTaskManager:
        """Get the shared HunterTaskManager instance."""
        return HunterTaskManager.get_instance()

    def _fetch_urls_parallel(self, urls: list, proxy_ports: List[int], timeout_per: int, overall_timeout: float, cap: int, label: str) -> Set[str]:
        """Shared parallel fetch logic using HunterTaskManager IO pool."""
        configs: Set[str] = set()
        mgr = self._get_task_manager()

        futures = {mgr.submit_io(self._fetch_single_url, url, proxy_ports, timeout_per): url for url in urls}
        try:
            for future in as_completed(futures, timeout=overall_timeout):
                try:
                    found = future.result(timeout=1)
                    if found:
                        if cap > 0 and len(configs) + len(found) > cap:
                            remaining = cap - len(configs)
                            configs.update(list(found)[:remaining])
                        else:
                            configs.update(found)
                        if cap > 0 and len(configs) >= cap:
                            self.logger.info(f"{label}: hit cap {cap}, stopping early")
                            for f in futures:
                                f.cancel()
                            break
                except Exception:
                    pass
        except TimeoutError:
            self.logger.warning(f"{label} fetch timeout, got {len(configs)} configs so far")
            for f in futures:
                f.cancel()

        if cap > 0 and len(configs) > cap:
            configs = set(list(configs)[:cap])
        return configs

    def fetch_github_configs(self, proxy_ports: List[int], max_workers: int = 10, max_configs: int = 0) -> Set[str]:
        """Fetch configs from GitHub repos in parallel with proxy fallback."""
        cap = max_configs if max_configs > 0 else 0
        return self._fetch_urls_parallel(GITHUB_REPOS, proxy_ports, 7, 25.0, cap, "GitHub")
    
    def fetch_anti_censorship_configs(self, proxy_ports: List[int], max_workers: int = 10, max_configs: int = 0) -> Set[str]:
        """Fetch configs from anti-censorship sources."""
        cap = max_configs if max_configs > 0 else 0
        self.logger.info(f"Fetching from {len(ANTI_CENSORSHIP_SOURCES)} anti-censorship sources (cap={cap})...")
        configs = self._fetch_urls_parallel(ANTI_CENSORSHIP_SOURCES, proxy_ports, 9, 25.0, cap, "Anti-censorship")
        if configs:
            self.logger.info(f"Anti-censorship sources provided {len(configs)} configs")
        return configs
    
    def fetch_iran_priority_configs(self, proxy_ports: List[int], max_workers: int = 8, max_configs: int = 0) -> Set[str]:
        """Fetch configs from Iran priority sources (Reality-focused)."""
        cap = max_configs if max_configs > 0 else 0
        self.logger.info(f"Fetching from {len(IRAN_PRIORITY_SOURCES)} Iran priority sources (cap={cap})...")
        configs = self._fetch_urls_parallel(IRAN_PRIORITY_SOURCES, proxy_ports, 10, 25.0, cap, "Iran priority")
        if configs:
            self.logger.info(f"Iran priority sources provided {len(configs)} configs (Reality-focused)")
        return configs
    
    def fetch_napsterv_configs(self, proxy_ports: List[int]) -> Set[str]:
        """Fetch configs from NapsternetV subscription URLs."""
        return self._fetch_urls_parallel(NAPSTERV_SUBSCRIPTION_URLS, proxy_ports, 12, 45.0, 0, "NapsterV")
