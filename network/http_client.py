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
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Optional, Set

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
                pool_conn, pool_max = 5, 10
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

    def _is_port_alive(self, port: int) -> bool:
        """Quick check if a local port is accepting TCP connections."""
        import socket
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except Exception:
            return False

    def _fetch_single_url_fast(self, url: str, proxy_ports: List[int], timeout: int = 8) -> Set[str]:
        """Fast fetch using requests session with connection pooling."""
        if self._is_circuit_open(url):
            return set()
        
        session = self.http_manager.get_session()
        headers = {"User-Agent": self.http_manager.random_user_agent()}
        
        # Try direct first
        try:
            resp = session.get(url, headers=headers, timeout=timeout, verify=False)
            if resp.status_code == 200 and resp.text:
                found = self._try_decode_and_extract(resp.text)
                if found:
                    self._record_success(url)
                    return found
        except:
            pass
        
        # Filter to only alive proxy ports to avoid flooding pool with failed connections
        live_ports = [p for p in proxy_ports[:2] if self._is_port_alive(p)]
        
        # Try with SOCKS proxies (use session to stay within pool limits)
        for proxy_port in live_ports[:1]:
            try:
                proxies = {"http": f"socks5h://127.0.0.1:{proxy_port}", "https": f"socks5h://127.0.0.1:{proxy_port}"}
                resp = session.get(url, headers=headers, timeout=timeout, verify=False, proxies=proxies)
                if resp.status_code == 200 and resp.text:
                    found = self._try_decode_and_extract(resp.text)
                    if found:
                        self._record_success(url)
                        return found
            except:
                continue
        
        # Try HTTP proxy on port+1 (use session to stay within pool limits)
        for proxy_port in live_ports[:1]:
            try:
                http_port = proxy_port + 100
                proxies = {"http": f"http://127.0.0.1:{http_port}", "https": f"http://127.0.0.1:{http_port}"}
                resp = session.get(url, headers=headers, timeout=timeout, verify=False, proxies=proxies)
                if resp.status_code == 200 and resp.text:
                    found = self._try_decode_and_extract(resp.text)
                    if found:
                        self._record_success(url)
                        return found
            except:
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
        
        # Build attempt list: direct, then only alive proxy ports
        attempts = [(None, None)]
        live_ports = [p for p in proxy_ports[:2] if self._is_port_alive(p)]
        for p in live_ports[:1]:
            attempts.append(("socks5", p))
        for p in live_ports[:1]:
            attempts.append(("http", p + 100))
        
        for proxy_type, proxy_port in attempts:
            try:
                cmd = [
                    curl_cmd,
                    "-s",
                    "-m", str(timeout),
                    "-k",
                    "--connect-timeout", "4",
                    "-A", self.http_manager.random_user_agent(),
                ]
                if proxy_type == "socks5":
                    cmd.extend(["-x", f"socks5://127.0.0.1:{proxy_port}"])
                elif proxy_type == "http":
                    cmd.extend(["-x", f"http://127.0.0.1:{proxy_port}"])
                cmd.append(url)
                
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
    
    def _adaptive_workers(self, default: int) -> int:
        """Return thread count based on memory pressure."""
        try:
            import psutil
            mem = psutil.virtual_memory().percent
            if mem >= 95:
                return max(2, default // 5)
            elif mem >= 90:
                return max(3, default // 4)
            elif mem >= 85:
                return max(4, default // 3)
            elif mem >= 80:
                return max(5, default // 2)
        except Exception:
            pass
        return default

    def fetch_github_configs(self, proxy_ports: List[int], max_workers: int = 10, max_configs: int = 0) -> Set[str]:
        """Fetch configs from GitHub repos in parallel with proxy fallback."""
        configs: Set[str] = set()
        workers = self._adaptive_workers(max_workers)
        cap = max_configs if max_configs > 0 else 0
        
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {executor.submit(self._fetch_single_url, url, proxy_ports, 10): url for url in GITHUB_REPOS}
            try:
                for future in as_completed(futures, timeout=90):
                    try:
                        found = future.result(timeout=1)
                        if found:
                            if cap > 0 and len(configs) + len(found) > cap:
                                remaining = cap - len(configs)
                                configs.update(list(found)[:remaining])
                            else:
                                configs.update(found)
                            if cap > 0 and len(configs) >= cap:
                                self.logger.info(f"GitHub: hit cap {cap}, stopping early")
                                for f in futures:
                                    f.cancel()
                                break
                    except Exception:
                        pass
            except TimeoutError:
                self.logger.warning(f"GitHub fetch timeout, got {len(configs)} configs so far")
                for f in futures:
                    f.cancel()
        if cap > 0 and len(configs) > cap:
            configs = set(list(configs)[:cap])
        return configs
    
    def fetch_anti_censorship_configs(self, proxy_ports: List[int], max_workers: int = 10, max_configs: int = 0) -> Set[str]:
        """Fetch configs from anti-censorship sources."""
        configs: Set[str] = set()
        workers = self._adaptive_workers(max_workers)
        cap = max_configs if max_configs > 0 else 0
        self.logger.info(f"Fetching from {len(ANTI_CENSORSHIP_SOURCES)} anti-censorship sources ({workers} workers, cap={cap})...")
        
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {executor.submit(self._fetch_single_url, url, proxy_ports, 15): url for url in ANTI_CENSORSHIP_SOURCES}
            try:
                for future in as_completed(futures, timeout=120):
                    try:
                        found = future.result(timeout=1)
                        if found:
                            if cap > 0 and len(configs) + len(found) > cap:
                                remaining = cap - len(configs)
                                configs.update(list(found)[:remaining])
                            else:
                                configs.update(found)
                            if cap > 0 and len(configs) >= cap:
                                self.logger.info(f"Anti-censorship: hit cap {cap}, stopping early")
                                for f in futures:
                                    f.cancel()
                                break
                    except Exception:
                        pass
            except TimeoutError:
                self.logger.warning(f"Anti-censorship fetch timeout, got {len(configs)} configs so far")
                for f in futures:
                    f.cancel()
        
        if cap > 0 and len(configs) > cap:
            configs = set(list(configs)[:cap])
        if configs:
            self.logger.info(f"Anti-censorship sources provided {len(configs)} configs")
        return configs
    
    def fetch_iran_priority_configs(self, proxy_ports: List[int], max_workers: int = 8, max_configs: int = 0) -> Set[str]:
        """Fetch configs from Iran priority sources (Reality-focused)."""
        configs: Set[str] = set()
        workers = self._adaptive_workers(max_workers)
        cap = max_configs if max_configs > 0 else 0
        self.logger.info(f"Fetching from {len(IRAN_PRIORITY_SOURCES)} Iran priority sources ({workers} workers, cap={cap})...")
        
        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {executor.submit(self._fetch_single_url, url, proxy_ports, 20): url for url in IRAN_PRIORITY_SOURCES}
            try:
                for future in as_completed(futures, timeout=90):
                    try:
                        found = future.result(timeout=1)
                        if found:
                            if cap > 0 and len(configs) + len(found) > cap:
                                remaining = cap - len(configs)
                                configs.update(list(found)[:remaining])
                            else:
                                configs.update(found)
                            if cap > 0 and len(configs) >= cap:
                                self.logger.info(f"Iran priority: hit cap {cap}, stopping early")
                                for f in futures:
                                    f.cancel()
                                break
                    except Exception:
                        pass
            except TimeoutError:
                self.logger.warning(f"Iran priority fetch timeout, got {len(configs)} configs so far")
                for f in futures:
                    f.cancel()
        
        if cap > 0 and len(configs) > cap:
            configs = set(list(configs)[:cap])
        if configs:
            self.logger.info(f"Iran priority sources provided {len(configs)} configs (Reality-focused)")
        return configs
    
    def fetch_napsterv_configs(self, proxy_ports: List[int]) -> Set[str]:
        """Fetch configs from NapsternetV subscription URLs."""
        configs = set()
        with ThreadPoolExecutor(max_workers=8) as executor:
            futures = {executor.submit(self._fetch_single_url, url, proxy_ports, 12): url for url in NAPSTERV_SUBSCRIPTION_URLS}
            for future in as_completed(futures, timeout=45):
                try:
                    found = future.result()
                    if found:
                        configs.update(found)
                except Exception:
                    pass
        return configs
