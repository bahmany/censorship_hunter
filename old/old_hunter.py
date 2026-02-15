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

# Web server for proxy management (optional)
_web_server_thread = None
_web_server_enabled = os.getenv("HUNTER_WEB_SERVER", "true").lower() == "true"
_web_server_port = int(os.getenv("HUNTER_WEB_PORT", "8080"))
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

try:
    import paramiko
except Exception:
    paramiko = None

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

import logging as _logging

_logging.getLogger("urllib3.connectionpool").setLevel(_logging.ERROR)

from tqdm import tqdm


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


def _curl_null_output() -> str:
    if sys.platform == "win32":
        return "NUL"
    return "/dev/null"


try:
    from telethon import TelegramClient
except Exception:
    TelegramClient = None

try:
    from telegram_reporter import get_reporter, send_report as persistent_send_report
except:
    get_reporter = None
    persistent_send_report = None

init(autoreset=True)

if sys.platform == "win32":
    import codecs

    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')

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
    # NapsternetV configs
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_STARTER.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix",
    "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray",
    "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt",
    "https://raw.githubusercontent.com/freefq/free/master/v2",
    "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2",
    # Tor-friendly mirrors
    "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt",
    "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub",
    "https://raw.githubusercontent.com/vveg26/get_proxy/main/dist/v2ray.txt",
]

NAPSTERV_SUBSCRIPTION_URLS = [
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt",
    "https://raw.githubusercontent.com/AzadNetCH/Clash/main/V2Ray.txt",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless",
    "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan",
]

_HOME_DIR = Path.home()

if sys.platform == "win32":
    TOR_PATH = r"C:\Users\mohammad\Desktop\Tor Browser\Browser\TorBrowser\Tor\tor.exe"
    TOR_SOCKS_PORT = int(os.getenv("HUNTER_TOR_SOCKS_PORT", "9150"))

    XRAY_PATH = r"C:\Users\mohammad\Documents\v2rayN-windows-64-SelfContained\bin\xray\xray.exe"
    MIHOMO_PATH = r"C:\Users\mohammad\Documents\v2rayN-windows-64-SelfContained\bin\mihomo\mihomo.exe"
    SINGBOX_PATH = r"C:\Users\mohammad\Documents\v2rayN-windows-64-SelfContained\bin\sing_box\sing-box.exe"

    TOR_PATH_FALLBACKS = [
        r"C:\Users\mbahmani\Desktop\Tor Browser\Browser\TorBrowser\Tor\tor.exe",
        "tor.exe",
    ]

    XRAY_PATH_FALLBACKS = [
        r"D:\v2rayN\bin\xray\xray.exe",
        "xray.exe",
    ]

    MIHOMO_PATH_FALLBACKS = [
        r"D:\v2rayN\bin\mihomo\mihomo.exe",
        "mihomo.exe",
    ]

    SINGBOX_PATH_FALLBACKS = [
        r"D:\v2rayN\bin\sing_box\sing-box.exe",
        "sing-box.exe",
        "singbox.exe",
    ]
else:
    TOR_PATH = os.getenv("HUNTER_TOR_PATH", "tor")
    TOR_SOCKS_PORT = int(os.getenv("HUNTER_TOR_SOCKS_PORT", "9050"))

    XRAY_PATH = os.getenv("HUNTER_XRAY_PATH_DEFAULT", "xray")
    MIHOMO_PATH = os.getenv("HUNTER_MIHOMO_PATH_DEFAULT", "mihomo")
    SINGBOX_PATH = os.getenv("HUNTER_SINGBOX_PATH_DEFAULT", "sing-box")

    TOR_PATH_FALLBACKS = ["tor"]
    XRAY_PATH_FALLBACKS = [
        str(_HOME_DIR / "bin" / "xray"),
        "/usr/local/bin/xray",
        "/usr/bin/xray",
        "xray",
    ]
    MIHOMO_PATH_FALLBACKS = [
        str(_HOME_DIR / "bin" / "mihomo"),
        "/usr/local/bin/mihomo",
        "/usr/bin/mihomo",
        "mihomo",
    ]
    SINGBOX_PATH_FALLBACKS = [
        str(_HOME_DIR / "bin" / "sing-box"),
        "/usr/local/bin/sing-box",
        "/usr/bin/sing-box",
        "sing-box",
        "singbox",
    ]

SSH_SERVERS: List[Dict[str, Any]] = []


def _load_ssh_servers_from_env() -> List[Dict[str, Any]]:
    raw = (os.getenv("HUNTER_SSH_SERVERS_JSON", "") or "").strip()
    if not raw:
        return list(SSH_SERVERS)
    try:
        data = json.loads(raw)
        if isinstance(data, list):
            return [d for d in data if isinstance(d, dict)]
    except Exception:
        return list(SSH_SERVERS)
    return list(SSH_SERVERS)


AVAILABLE_ENGINES = []
CDN_WHITELIST_DOMAINS = [
    "cloudflare.com", "cdn.cloudflare.com", "cloudflare-dns.com",
    "fastly.net", "fastly.com", "global.fastly.net",
    "akamai.net", "akamaiedge.net", "akamaihd.net",
    "azureedge.net", "azure.com", "microsoft.com",
    "amazonaws.com", "cloudfront.net", "awsglobalaccelerator.com",
    "googleusercontent.com", "googleapis.com", "gstatic.com",
    "edgecastcdn.net", "stackpathdns.com",
    "cdn77.org", "cdnjs.cloudflare.com",
    "jsdelivr.net", "unpkg.com",
    "workers.dev", "pages.dev",
    "vercel.app", "netlify.app",
    "arvancloud.ir", "arvancloud.com", "r2.dev",
    "arvan.run", "arvanstorage.ir", "arvancdn.ir",
    "arvancdn.com", "cdn.arvancloud.ir",
]
WHITELIST_PORTS = [443, 8443, 2053, 2083, 2087, 2096, 80, 8080]
ANTI_DPI_INDICATORS = [
    "reality", "pbk=",
    "grpc", "gun",
    "h2", "http/2",
    "ws", "websocket",
    "splithttp", "httpupgrade",
    "quic", "kcp",
    "fp=chrome", "fp=firefox", "fp=safari", "fp=edge",
    "alpn=h2", "alpn=http",
]
DPI_EVASION_FINGERPRINTS = [
    "chrome", "firefox", "safari", "edge", "ios", "android", "random", "randomized"
]
IRAN_BLOCKED_PATTERNS = [
    "ir.", ".ir", "iran",
    "0.0.0.0", "127.0.0.1", "localhost",
    "10.10.34.", "192.168.",
]
ANTI_CENSORSHIP_SOURCES = [
    # GitHub raw - most reliable
    "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub1.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub2.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/Sub3.txt",
    "https://raw.githubusercontent.com/barry-far/V2ray-Configs/main/All_Configs_Sub.txt",
    # Reality-focused sources (best for Iran)
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
    # Additional resilient sources
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
# Fallback DNS servers for when Iran's DNS is poisoned
FALLBACK_DNS_SERVERS = [
    "1.1.1.1",  # Cloudflare
    "8.8.8.8",  # Google
    "9.9.9.9",  # Quad9
    "208.67.222.222",  # OpenDNS
]


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


console_handler = logging.StreamHandler(sys.stdout)
console_handler.setLevel(logging.INFO)
console_handler.setFormatter(ColoredFormatter())
logging.basicConfig(level=logging.DEBUG, handlers=[console_handler])
logger = logging.getLogger(__name__)

logging.getLogger("telethon").setLevel(logging.WARNING)
logging.getLogger("telethon.network").setLevel(logging.WARNING)
logging.getLogger("telethon.client.downloads").setLevel(logging.WARNING)

european_codes = ['AL', 'AD', 'AT', 'BY', 'BE', 'BA', 'BG', 'HR', 'CY', 'CZ', 'DK', 'EE', 'FO', 'FI', 'FR', 'DE', 'GI',
                  'GR', 'HU', 'IS', 'IE', 'IT', 'XK', 'LV', 'LI', 'LT', 'LU', 'MK', 'MT', 'MD', 'MC', 'ME', 'NL', 'NO',
                  'PL', 'PT', 'RO', 'RU', 'SM', 'RS', 'SK', 'SI', 'ES', 'SE', 'CH', 'UA', 'GB', 'VA']
asian_codes = ['AF', 'AM', 'AZ', 'BH', 'BD', 'BT', 'BN', 'KH', 'CN', 'GE', 'HK', 'IN', 'ID', 'IR', 'IQ', 'IL', 'JP',
               'JO', 'KZ', 'KW', 'KG', 'LA', 'LB', 'MO', 'MY', 'MV', 'MN', 'MM', 'NP', 'KP', 'OM', 'PK', 'PS', 'PH',
               'QA', 'SA', 'SG', 'KR', 'LK', 'SY', 'TW', 'TJ', 'TH', 'TL', 'TR', 'TM', 'AE', 'UZ', 'VN', 'YE']
african_codes = ['DZ', 'AO', 'BJ', 'BW', 'BF', 'BI', 'CV', 'CM', 'CF', 'TD', 'KM', 'CD', 'CG', 'DJ', 'EG', 'GQ', 'ER',
                 'SZ', 'ET', 'GA', 'GM', 'GH', 'GN', 'GW', 'CI', 'KE', 'LS', 'LR', 'LY', 'MG', 'MW', 'ML', 'MR', 'MU',
                 'YT', 'MA', 'MZ', 'NA', 'NE', 'NG', 'RE', 'RW', 'SH', 'ST', 'SN', 'SC', 'SL', 'SO', 'ZA', 'SS', 'SD',
                 'TZ', 'TG', 'TN', 'UG', 'EH', 'ZM', 'ZW']


def get_region(country_code: Optional[str]) -> str:
    if country_code == 'US':
        return 'USA'
    elif country_code == 'CA':
        return 'Canada'
    elif country_code in european_codes:
        return 'Europe'
    elif country_code in asian_codes:
        return 'Asia'
    elif country_code in african_codes:
        return 'Africa'
    else:
        return 'Other'


def clean_ps_string(ps: str) -> str:
    return re.sub(r'[^\x00-\x7F]+', '', ps).strip() or "Unknown"


@dataclass
class HunterParsedConfig:
    uri: str
    outbound: Dict[str, Any]
    host: str
    port: int
    identity: str
    ps: str


@dataclass
class HunterBenchResult:
    uri: str
    outbound: Dict[str, Any]
    host: str
    port: int
    identity: str
    ps: str
    latency_ms: float
    ip: Optional[str]
    country_code: Optional[str]
    region: str
    tier: str


def _safe_b64decode(data: str) -> bytes:
    padded = data + "=" * (-len(data) % 4)
    return base64.b64decode(padded)


def _fetch_single_url_fast(url: str, proxy_ports: List[int], timeout: int = 8) -> Set[str]:
    """Fast fetch using requests session with connection pooling"""
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


def _fetch_single_url(url: str, proxy_ports: List[int], timeout: int = 12) -> Set[str]:
    """Fetch a single URL, trying fast method first then curl fallback"""
    # Try fast method first
    result = _fetch_single_url_fast(url, proxy_ports, timeout=min(timeout, 8))
    if result:
        return result

    # Fallback to curl for stubborn URLs
    configs = set()
    curl_cmd = "curl"

    attempts = [None] + proxy_ports[:3]

    for proxy_port in attempts:
        try:
            cmd = [
                curl_cmd,
                "-s",
                "-m",
                str(timeout),
                "-k",
                "--connect-timeout",
                "4",
                "-A",
                _random_user_agent(),
            ]
            if proxy_port:
                cmd.extend(["-x", f"socks5://127.0.0.1:{proxy_port}"])
            cmd.append(url)

            result = subprocess.run(cmd, capture_output=True, timeout=timeout + 2)
            if result.returncode == 0 and result.stdout and len(result.stdout) > 50:
                text = result.stdout.decode('utf-8', errors='ignore')
                try:
                    decoded = base64.b64decode(text.strip()).decode('utf-8', errors='ignore')
                    if '://' in decoded:
                        text = decoded
                except:
                    pass
                found = _extract_raw_uris_from_text(text)
                if found:
                    return found
        except Exception:
            continue
    return configs


def _fetch_github_configs_parallel(proxy_ports: List[int], max_workers: int = 25) -> Set[str]:
    """Fetch configs from GitHub repos in parallel with proxy fallback"""
    configs = set()

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = {executor.submit(_fetch_single_url, url, proxy_ports, 10): url for url in GITHUB_REPOS}
        try:
            for future in as_completed(futures, timeout=90):
                try:
                    found = future.result(timeout=1)
                    if found:
                        configs.update(found)
                except Exception:
                    pass
        except TimeoutError:
            logger.warning(f"GitHub fetch timeout, got {len(configs)} configs so far")
            for f in futures:
                f.cancel()
    return configs


def _fetch_anti_censorship_configs(proxy_ports: List[int], max_workers: int = 20) -> Set[str]:
    """
    Fetch configs from anti-censorship sources (mirrors, CDN-hosted, Reality-focused).
    These sources are designed to bypass Iran's filtering infrastructure.
    """
    configs = set()
    logger.info(f"Fetching from {len(ANTI_CENSORSHIP_SOURCES)} anti-censorship sources...")

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = {executor.submit(_fetch_single_url, url, proxy_ports, 15): url for url in ANTI_CENSORSHIP_SOURCES}
        try:
            for future in as_completed(futures, timeout=120):
                try:
                    found = future.result(timeout=1)
                    if found:
                        configs.update(found)
                except Exception:
                    pass
        except TimeoutError:
            logger.warning(f"Anti-censorship fetch timeout, got {len(configs)} configs so far")
            for f in futures:
                f.cancel()

    if configs:
        logger.info(f"Anti-censorship sources provided {len(configs)} configs")
    return configs


def _fetch_github_configs(use_tor: bool = False, proxy_port: Optional[int] = None,
                          all_proxy_ports: Optional[List[int]] = None) -> Set[str]:
    """Fetch configs from GitHub repos with smart proxy fallback"""
    # Build proxy list for fallback
    proxy_ports_list: List[int] = []
    if use_tor:
        proxy_ports_list.append(TOR_SOCKS_PORT)
    if proxy_port:
        proxy_ports_list.append(proxy_port)
    if all_proxy_ports:
        for p in all_proxy_ports:
            if p not in proxy_ports_list:
                proxy_ports_list.append(p)

    # Add standard local proxy ports as fallback
    for p in [1080, 10808, 7890, 2080]:
        if p not in proxy_ports_list:
            proxy_ports_list.append(p)

    return _fetch_github_configs_parallel(proxy_ports_list, max_workers=15)


def _fetch_napsterv_configs(use_tor: bool = False, proxy_port: Optional[int] = None,
                            all_proxy_ports: Optional[List[int]] = None) -> Set[str]:
    """Fetch configs from NapsternetV subscription URLs with proxy fallback"""
    # Build proxy list for fallback
    proxy_ports_list: List[int] = []
    if use_tor:
        proxy_ports_list.append(TOR_SOCKS_PORT)
    if proxy_port:
        proxy_ports_list.append(proxy_port)
    if all_proxy_ports:
        for p in all_proxy_ports:
            if p not in proxy_ports_list:
                proxy_ports_list.append(p)

    for p in [1080, 10808, 7890, 2080]:
        if p not in proxy_ports_list:
            proxy_ports_list.append(p)

    configs = set()
    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = {executor.submit(_fetch_single_url, url, proxy_ports_list, 12): url for url in
                   NAPSTERV_SUBSCRIPTION_URLS}
        for future in as_completed(futures, timeout=45):
            try:
                found = future.result()
                if found:
                    configs.update(found)
            except Exception:
                pass
    return configs


def _fetch_with_tor_engine(xray_path: str, tor_configs: List[str], target_urls: List[str], socks_port: int = 19050) -> \
        Set[str]:
    """Use Tor-like configs through xray engine to fetch subscription URLs"""
    configs = set()
    resolved_xray = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not tor_configs or not resolved_xray or not _check_xray_path(resolved_xray):
        return configs

    for tor_uri in tor_configs[:3]:
        parsed = _parse_any_uri(tor_uri)
        if not parsed:
            continue

        xray_config = {
            "log": {"loglevel": "warning"},
            "inbounds": [
                {"port": socks_port, "listen": "127.0.0.1", "protocol": "socks",
                 "settings": {"auth": "noauth", "udp": True}}
            ],
            "outbounds": [parsed.outbound],
        }
        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix=f"TOR_ENGINE_{socks_port}_", suffix=".json")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(xray_config, f, ensure_ascii=False)
            process = subprocess.Popen(
                [resolved_xray, "run", "-c", temp_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(2.5)

            if process.poll() is not None:
                continue

            curl_cmd = "curl"
            for url in target_urls:
                try:
                    result = subprocess.run(
                        [curl_cmd, "-x", f"socks5://127.0.0.1:{socks_port}", "-s", "-m", "15", "-k", url],
                        capture_output=True, timeout=20
                    )
                    if result.returncode == 0 and result.stdout:
                        text = result.stdout.decode('utf-8', errors='ignore')
                        try:
                            decoded = base64.b64decode(text.strip()).decode('utf-8', errors='ignore')
                            if '://' in decoded:
                                text = decoded
                        except:
                            pass
                        found = _extract_raw_uris_from_text(text)
                        if found:
                            configs.update(found)
                except Exception:
                    pass

            if configs:
                logger.info(f"Tor engine fetched {len(configs)} configs via {parsed.ps[:20]}")
                break
        except Exception as e:
            logger.debug(f"Tor engine error: {e}")
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=2)
                except:
                    try:
                        process.kill()
                    except:
                        pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except:
                    pass

    return configs


class SmartCache:
    """Smart caching for configs with failure tracking"""

    def __init__(self, cache_file: str = "subscriptions_cache.txt",
                 working_cache_file: str = "working_configs_cache.txt"):
        self.cache_file = cache_file
        self.working_cache_file = working_cache_file
        self._last_successful_fetch = 0
        self._consecutive_failures = 0

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
            logger.debug(f"Cache save error: {e}")
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
            logger.debug(f"Cache load error: {e}")
        return configs

    def record_failure(self):
        """Record a connection failure"""
        self._consecutive_failures += 1

    def should_use_cache(self) -> bool:
        """Check if cache should be used due to failures"""
        return self._consecutive_failures >= 2

    def get_failure_count(self) -> int:
        return self._consecutive_failures


class ResilientHeartbeat:
    """Resilient heartbeat for Telegram connection monitoring"""

    def __init__(self, check_interval: int = 30):
        self.check_interval = check_interval
        self._last_heartbeat = _now_ts()
        self._is_connected = False
        self._reconnect_attempts = 0
        self._max_reconnect_attempts = 5

    async def check_connection(self, client: Optional['TelegramClient']) -> bool:
        """Check connection status"""
        if client is None:
            self._is_connected = False
            return False

        try:
            if not client.is_connected():
                self._is_connected = False
                return False

            me = await asyncio.wait_for(client.get_me(), timeout=10)
            if me:
                self._is_connected = True
                self._last_heartbeat = _now_ts()
                self._reconnect_attempts = 0
                return True
        except asyncio.TimeoutError:
            logger.warning("Heartbeat timeout")
        except Exception as e:
            logger.warning(f"Heartbeat failed: {e}")

        self._is_connected = False
        return False

    async def try_reconnect(self, client: Optional['TelegramClient']) -> bool:
        """Try to reconnect"""
        if self._reconnect_attempts >= self._max_reconnect_attempts:
            logger.error(f"Max reconnect attempts ({self._max_reconnect_attempts}) reached")
            return False

        self._reconnect_attempts += 1
        logger.info(f"Reconnect attempt {self._reconnect_attempts}/{self._max_reconnect_attempts}")

        try:
            if client:
                try:
                    await client.disconnect()
                except:
                    pass
                await asyncio.sleep(2)
                await client.connect()
                if await client.is_user_authorized():
                    self._is_connected = True
                    logger.info("Reconnected successfully")
                    return True
        except Exception as e:
            logger.warning(f"Reconnect failed: {e}")

        return False

    def is_connected(self) -> bool:
        return self._is_connected

    def time_since_heartbeat(self) -> int:
        return _now_ts() - self._last_heartbeat


class SSHStableSocksProxy:
    def __init__(
            self,
            local_host: str,
            local_port: int,
            servers: List[Dict[str, Any]],
            refresh_interval: int,
            connect_timeout: int,
    ):
        self.local_host = str(local_host)
        self.local_port = int(local_port)
        self.servers = list(servers or [])
        self.refresh_interval = int(refresh_interval)
        self.connect_timeout = int(connect_timeout)
        self._running = False
        self._lock = threading.RLock()

        self._client = None
        self._transport = None
        self._active_server: Optional[Dict[str, Any]] = None

        self._server: Optional[socketserver.TCPServer] = None
        self._serve_thread: Optional[threading.Thread] = None
        self._refresh_thread: Optional[threading.Thread] = None

        self._stats: Dict[str, Dict[str, Any]] = {}
        for s in self.servers:
            key = f"{s.get('host')}:{int(s.get('port') or 22)}"
            self._stats[key] = {"ok": 0, "fail": 0, "last_ok": 0, "last_ms": None}

    def _server_key(self, server: Dict[str, Any]) -> str:
        return f"{server.get('host')}:{int(server.get('port') or 22)}"

    def _is_transport_ok(self) -> bool:
        with self._lock:
            tr = self._transport
        if tr is None:
            return False
        try:
            return bool(tr.is_active())
        except Exception:
            return False

    def _transport_probe(self) -> bool:
        with self._lock:
            tr = self._transport
        if tr is None:
            return False
        try:
            if not tr.is_active():
                return False
            chan = tr.open_channel("direct-tcpip", ("1.1.1.1", 443), ("127.0.0.1", 0))
            try:
                chan.close()
            except Exception:
                pass
            return True
        except Exception:
            return False

    def _close_client(self) -> None:
        try:
            if self._client is not None:
                self._client.close()
        except Exception:
            pass
        self._client = None
        self._transport = None
        self._active_server = None

    def _connect_server(self, server: Dict[str, Any]) -> bool:
        if paramiko is None:
            return False
        host = server.get("host")
        port = int(server.get("port") or 22)
        username = server.get("username")
        password = server.get("password")
        if not password:
            penv = server.get("password_env")
            if penv:
                try:
                    password = os.getenv(str(penv), "")
                except Exception:
                    password = ""
        if not host or not username or not password:
            return False

        start = time.monotonic()
        client = None
        try:
            client = paramiko.SSHClient()
            client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            client.connect(
                hostname=str(host),
                port=int(port),
                username=str(username),
                password=str(password),
                timeout=self.connect_timeout,
                auth_timeout=self.connect_timeout,
                banner_timeout=self.connect_timeout,
                allow_agent=False,
                look_for_keys=False,
            )
            transport = client.get_transport()
            if transport is None or not transport.is_active():
                client.close()
                return False
            transport.set_keepalive(30)

            try:
                chan = transport.open_channel("direct-tcpip", ("1.1.1.1", 443), ("127.0.0.1", 0))
                try:
                    chan.close()
                except Exception:
                    pass
            except Exception:
                client.close()
                return False

            elapsed_ms = (time.monotonic() - start) * 1000.0
            key = self._server_key(server)
            st = self._stats.setdefault(key, {"ok": 0, "fail": 0, "last_ok": 0, "last_ms": None})
            st["ok"] = int(st.get("ok", 0)) + 1
            st["last_ok"] = _now_ts()
            st["last_ms"] = elapsed_ms

            with self._lock:
                self._close_client()
                self._client = client
                self._transport = transport
                self._active_server = dict(server)
            return True
        except Exception:
            try:
                if client is not None:
                    client.close()
            except Exception:
                pass
            key = self._server_key(server)
            st = self._stats.setdefault(key, {"ok": 0, "fail": 0, "last_ok": 0, "last_ms": None})
            st["fail"] = int(st.get("fail", 0)) + 1
            return False

    def _select_best_server(self) -> Optional[Dict[str, Any]]:
        best = None
        best_tuple = None
        for s in self.servers:
            key = self._server_key(s)
            st = self._stats.get(key, {})
            fail = int(st.get("fail", 0))
            ok = int(st.get("ok", 0))
            last_ok = int(st.get("last_ok", 0))
            last_ms = st.get("last_ms")
            last_ms_val = float(last_ms) if isinstance(last_ms, (int, float)) else 1e9
            score = (-ok, fail, -last_ok, last_ms_val)
            if best_tuple is None or score < best_tuple:
                best_tuple = score
                best = s
        return best

    def ensure_connected(self, force_reconnect: bool = False) -> bool:
        if paramiko is None or not self.servers:
            return False

        if not force_reconnect and self._is_transport_ok() and self._transport_probe():
            return True

        for _ in range(max(1, len(self.servers))):
            cand = self._select_best_server() or (self.servers[0] if self.servers else None)
            if cand is None:
                return False
            if self._connect_server(cand):
                logger.info(
                    f"[SSH] Active on {self.local_host}:{self.local_port} via {cand.get('host')}:{int(cand.get('port') or 22)}"
                )
                return True
            try:
                self.servers.remove(cand)
                self.servers.append(cand)
            except Exception:
                pass
        return False

    def get_transport(self):
        with self._lock:
            tr = self._transport
        if tr is None:
            return None
        try:
            return tr if tr.is_active() else None
        except Exception:
            return None

    def _refresh_loop(self) -> None:
        while self._running:
            time.sleep(max(5, self.refresh_interval))
            if not self._running:
                break
            try:
                self.ensure_connected(force_reconnect=False)
            except Exception:
                pass

    def start(self) -> bool:
        if self._running:
            return True
        if paramiko is None:
            logger.warning("[SSH] paramiko missing; SSH SOCKS disabled")
            return False
        if not self.servers:
            logger.warning("[SSH] No SSH servers configured; SSH SOCKS disabled")
            return False

        self._running = True
        proxy = self

        class _ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
            allow_reuse_address = True
            daemon_threads = True

        class _Socks5Handler(socketserver.BaseRequestHandler):
            def _recv_exact(self, n: int) -> Optional[bytes]:
                buf = b""
                while len(buf) < n:
                    chunk = self.request.recv(n - len(buf))
                    if not chunk:
                        return None
                    buf += chunk
                return buf

            def _reply(self, rep: int) -> None:
                try:
                    self.request.sendall(
                        b"\x05" + bytes([rep]) + b"\x00\x01" + socket.inet_aton("0.0.0.0") + struct.pack("!H", 0)
                    )
                except Exception:
                    pass

            def handle(self):
                chan = None
                try:
                    self.request.settimeout(15)
                    head = self._recv_exact(2)
                    if not head or head[0] != 5:
                        return
                    nmethods = head[1]
                    _ = self._recv_exact(nmethods)
                    self.request.sendall(b"\x05\x00")

                    req = self._recv_exact(4)
                    if not req or req[0] != 5:
                        return
                    cmd = req[1]
                    atyp = req[3]
                    if cmd != 1:
                        self._reply(7)
                        return

                    if atyp == 1:
                        addr_raw = self._recv_exact(4)
                        if not addr_raw:
                            return
                        dst_addr = socket.inet_ntoa(addr_raw)
                    elif atyp == 3:
                        ln_raw = self._recv_exact(1)
                        if not ln_raw:
                            return
                        ln = ln_raw[0]
                        name_raw = self._recv_exact(ln)
                        if not name_raw:
                            return
                        dst_addr = name_raw.decode("utf-8", errors="ignore")
                    elif atyp == 4:
                        addr_raw = self._recv_exact(16)
                        if not addr_raw:
                            return
                        dst_addr = socket.inet_ntop(socket.AF_INET6, addr_raw)
                    else:
                        self._reply(8)
                        return

                    port_raw = self._recv_exact(2)
                    if not port_raw:
                        return
                    dst_port = struct.unpack("!H", port_raw)[0]

                    tr = proxy.get_transport()
                    if tr is None:
                        proxy.ensure_connected(force_reconnect=True)
                        tr = proxy.get_transport()
                    if tr is None:
                        self._reply(1)
                        return

                    try:
                        chan = tr.open_channel("direct-tcpip", (dst_addr, int(dst_port)), ("127.0.0.1", 0))
                    except Exception:
                        self._reply(5)
                        return

                    self._reply(0)

                    self.request.settimeout(None)
                    try:
                        chan.settimeout(None)
                    except Exception:
                        pass

                    while True:
                        r, _, _ = select.select([self.request, chan], [], [], 120)
                        if self.request in r:
                            data = self.request.recv(65536)
                            if not data:
                                break
                            chan.sendall(data)
                        if chan in r:
                            data = chan.recv(65536)
                            if not data:
                                break
                            self.request.sendall(data)
                except Exception:
                    return
                finally:
                    try:
                        if chan is not None:
                            chan.close()
                    except Exception:
                        pass

        try:
            self._server = _ThreadedTCPServer((self.local_host, self.local_port), _Socks5Handler)
        except Exception as e:
            self._running = False
            logger.error(f"[SSH] Failed to bind {self.local_host}:{self.local_port}: {e}")
            return False

        self._serve_thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._serve_thread.start()

        self._refresh_thread = threading.Thread(target=self._refresh_loop, daemon=True)
        self._refresh_thread.start()

        self.ensure_connected(force_reconnect=True)
        logger.info(f"[SSH] SOCKS5 listening on {self.local_host}:{self.local_port}")
        return True

    def stop(self) -> None:
        self._running = False
        try:
            if self._server is not None:
                self._server.shutdown()
                self._server.server_close()
        except Exception:
            pass
        self._server = None
        with self._lock:
            self._close_client()


def _is_tor_running() -> bool:
    """Check if Tor SOCKS proxy is available"""
    try:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2)
        result = s.connect_ex(('127.0.0.1', TOR_SOCKS_PORT))
        s.close()
        return result == 0
    except:
        return False


def _resolve_executable_path(name: str, primary: Optional[str], fallbacks: Iterable[str]) -> Optional[str]:
    candidates: List[str] = []
    if primary:
        candidates.append(primary)
    for p in fallbacks:
        if p:
            candidates.append(p)

    seen = set()
    for p in candidates:
        if not p:
            continue
        try:
            normalized = os.path.expandvars(os.path.expanduser(p))
        except Exception:
            normalized = p

        if normalized in seen:
            continue
        seen.add(normalized)

        if os.path.isabs(normalized):
            if os.path.exists(normalized):
                return normalized
        else:
            if os.path.exists(normalized):
                return normalized
            try:
                import shutil

                resolved = shutil.which(normalized)
                if resolved and os.path.exists(resolved):
                    return resolved
            except Exception:
                pass

    return None


def _extract_raw_uris_from_text(text: str) -> Set[str]:
    if not text:
        return set()
    uris = set()
    for m in re.finditer(r"(?i)(vmess|vless|trojan|ss|shadowsocks)://[^\s\"'<>\[\]]+", text):
        uri = m.group(0).strip().rstrip(")],.;:!?")
        if len(uri) > 20:
            uris.add(uri)
    return uris


def _extract_raw_uris_from_bytes(data: bytes) -> Set[str]:
    if not data:
        return set()
    text = data.decode("utf-8", errors="ignore")
    uris = _extract_raw_uris_from_text(text)
    if uris:
        return uris
    for candidate in re.findall(r"[A-Za-z0-9+/=]{100,}", text)[:20]:
        try:
            decoded = _safe_b64decode(candidate).decode("utf-8", errors="ignore")
            extracted = _extract_raw_uris_from_text(decoded)
            if extracted:
                uris.update(extracted)
        except Exception:
            continue
    return uris


def _parse_vmess(uri: str) -> Optional[HunterParsedConfig]:
    try:
        payload = uri[len("vmess://"):]
        decoded = _safe_b64decode(payload).decode("utf-8", errors="ignore")
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


def _parse_vless(uri: str) -> Optional[HunterParsedConfig]:
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


def _parse_trojan(uri: str) -> Optional[HunterParsedConfig]:
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


def _parse_ss(uri: str) -> Optional[HunterParsedConfig]:
    try:
        parsed = uri
        if "#" in parsed:
            parsed, tag = parsed.split("#", 1)
            ps = clean_ps_string(unquote(tag))
        else:
            ps = "Unknown"
        core = parsed[len("ss://"):]
        if "@" not in core:
            core = _safe_b64decode(core.split("?", 1)[0]).decode("utf-8", errors="ignore")
        if "@" not in core:
            return None
        userinfo, hostport = core.split("@", 1)
        if ":" not in userinfo or ":" not in hostport:
            return None
        method, password = userinfo.split(":", 1)
        host, port_str = hostport.split(":", 1)
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


def _parse_any_uri(uri: str) -> Optional[HunterParsedConfig]:
    if not uri or "://" not in uri:
        return None
    proto = uri.split("://", 1)[0].lower()
    if proto == "vmess":
        return _parse_vmess(uri)
    if proto == "vless":
        return _parse_vless(uri)
    if proto == "trojan":
        return _parse_trojan(uri)
    if proto == "ss" or proto == "shadowsocks":
        return _parse_ss(uri)
    return None


def _check_xray_path(xray_path: str) -> bool:
    resolved = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not resolved:
        return False
    try:
        subprocess.run([resolved, "-version"], capture_output=True, text=True, timeout=5)
        return True
    except Exception:
        return False


MULTI_TEST_URLS = [
    ("https://cp.cloudflare.com/", "cloudflare_cp"),
    ("https://1.1.1.1/cdn-cgi/trace", "cf_trace"),
    ("https://www.gstatic.com/generate_204", "google204"),
    ("https://www.msftconnecttest.com/connecttest.txt", "microsoft"),
    ("https://azure.microsoft.com/", "azure"),
    ("https://detectportal.firefox.com/success.txt", "firefox"),
    ("https://www.apple.com/library/test/success.html", "apple"),
    ("https://connectivity-check.ubuntu.com/", "ubuntu"),
    ("https://api.ipify.org/", "ipify"),
    ("https://ifconfig.me/ip", "ifconfig"),
    ("https://www.arvancloud.ir/", "arvancloud"),
    ("https://panel.arvancloud.ir/", "arvan_panel"),
]
DNS_TEST_DOMAINS = [
    "cloudflare.com",
    "microsoft.com",
    "google.com",
    "azure.com",
    "akamai.com",
    "arvancloud.ir",
    "arvancloud.com",
]


def _get_random_test_url() -> Tuple[str, str]:
    """Get a random test URL from whitelisted domains to avoid DPI pattern detection"""
    import random
    if random.random() < 0.7:
        return random.choice(MULTI_TEST_URLS[:5])
    return random.choice(MULTI_TEST_URLS)


def _test_single_url(socks_port: int, test_url: str, timeout_seconds: int) -> Optional[float]:
    """Test a single URL through the proxy"""
    curl_cmd = "curl"
    try:
        start = time.monotonic()
        result = subprocess.run(
            [
                curl_cmd,
                "-x", f"socks5h://127.0.0.1:{socks_port}",
                "-s", "-o", _curl_null_output(),
                "-w", "%{http_code}",
                "-m", str(timeout_seconds),
                "--connect-timeout", str(min(5, timeout_seconds)),
                "-k",
                "-A", _random_user_agent(),
                test_url
            ],
            capture_output=True,
            text=True,
            timeout=timeout_seconds + 3
        )
        elapsed = (time.monotonic() - start) * 1000.0

        if result.returncode == 0:
            try:
                status_code = int(result.stdout.strip())
                if status_code < 400 or status_code == 204:
                    return elapsed
            except ValueError:
                pass
    except Exception:
        pass
    return None


def _test_dns_resolution(socks_port: int, domain: str, timeout_seconds: int = 10) -> bool:
    """Test DNS resolution through the proxy using curl"""
    curl_cmd = "curl"
    try:
        result = subprocess.run(
            [
                curl_cmd,
                "-x", f"socks5h://127.0.0.1:{socks_port}",
                "-s", "-o", _curl_null_output(),
                "-w", "%{http_code}",
                "-m", str(timeout_seconds),
                "--connect-timeout", str(min(5, timeout_seconds)),
                "-k",
                "-A", _random_user_agent(),
                f"https://{domain}/"
            ],
            capture_output=True,
            text=True,
            timeout=timeout_seconds + 5
        )
        if result.returncode == 0:
            try:
                status_code = int(result.stdout.strip())
                return status_code < 500
            except ValueError:
                pass
    except Exception:
        pass
    return False


def _benchmark_with_xray(
        xray_path: str,
        outbound: Dict[str, Any],
        socks_port: int,
        test_url: str,
        timeout_seconds: int,
) -> Optional[float]:
    """
    Enhanced benchmark with multiple test URLs and fallback methods.
    Tries multiple URLs to increase success rate against filtering.
    """
    xray_config = {
        "log": {"loglevel": "warning"},
        "inbounds": [
            {"port": socks_port, "listen": "127.0.0.1", "protocol": "socks",
             "settings": {"auth": "noauth", "udp": True}}
        ],
        "outbounds": [outbound],
    }
    resolved_xray = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not resolved_xray or not _check_xray_path(resolved_xray):
        return None
    temp_path = None
    process = None
    try:
        fd, temp_path = tempfile.mkstemp(prefix=f"HUNTER_{socks_port}_", suffix=".json")
        os.close(fd)
        with open(temp_path, "w", encoding="utf-8") as f:
            json.dump(xray_config, f, ensure_ascii=False)
        process = subprocess.Popen(
            [resolved_xray, "run", "-c", temp_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(1.0)

        if process.poll() is not None:
            return None

        latency = _test_single_url(socks_port, test_url, timeout_seconds)
        if latency:
            return latency

        fallback_url, _ = _get_random_test_url()
        if fallback_url and fallback_url != test_url:
            latency = _test_single_url(socks_port, fallback_url, min(timeout_seconds, 8))
            if latency:
                return latency

        return None
    except subprocess.TimeoutExpired:
        return None
    except FileNotFoundError:
        logger.error("curl not found. Please install curl.")
        return None
    except Exception as e:
        logger.debug(f"Benchmark error on port {socks_port}: {e}")
        return None
    finally:
        if process:
            try:
                process.terminate()
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                    process.wait(timeout=1)
                except Exception:
                    pass
            except Exception:
                pass
        if temp_path and os.path.exists(temp_path):
            try:
                os.remove(temp_path)
            except Exception:
                pass


def _benchmark_with_mihomo(
        mihomo_path: str,
        parsed,
        socks_port: int,
        test_url: str,
        timeout_seconds: int,
) -> Optional[float]:
    """Benchmark using Mihomo (Clash Meta) engine"""
    resolved_mihomo = _resolve_executable_path("mihomo", mihomo_path, [MIHOMO_PATH] + MIHOMO_PATH_FALLBACKS)
    if not resolved_mihomo:
        return None

    try:
        outbound = parsed.outbound
        protocol = outbound.get("protocol", "")
        settings = outbound.get("settings", {})
        stream = outbound.get("streamSettings", {})

        proxy = None
        if protocol == "vmess":
            vnext = settings.get("vnext", [{}])[0]
            users = vnext.get("users", [{}])[0]
            proxy = {
                "name": "test",
                "type": "vmess",
                "server": vnext.get("address", parsed.host),
                "port": vnext.get("port", parsed.port),
                "uuid": users.get("id", ""),
                "alterId": users.get("alterId", 0),
                "cipher": users.get("security", "auto"),
            }
            network = stream.get("network", "tcp")
            if network == "ws":
                proxy["network"] = "ws"
                ws_settings = stream.get("wsSettings", {})
                proxy["ws-opts"] = {"path": ws_settings.get("path", "/")}
            if stream.get("security") == "tls":
                proxy["tls"] = True
                proxy["skip-cert-verify"] = True

        elif protocol == "vless":
            vnext = settings.get("vnext", [{}])[0]
            users = vnext.get("users", [{}])[0]
            proxy = {
                "name": "test",
                "type": "vless",
                "server": vnext.get("address", parsed.host),
                "port": vnext.get("port", parsed.port),
                "uuid": users.get("id", ""),
                "network": stream.get("network", "tcp"),
            }
            if users.get("flow"):
                proxy["flow"] = users.get("flow")
            if stream.get("security") == "tls":
                proxy["tls"] = True
                proxy["skip-cert-verify"] = True
            elif stream.get("security") == "reality":
                reality = stream.get("realitySettings", {})
                proxy["tls"] = True
                proxy["reality-opts"] = {
                    "public-key": reality.get("publicKey", ""),
                    "short-id": reality.get("shortId", ""),
                }
                proxy["servername"] = reality.get("serverName", "")
                proxy["client-fingerprint"] = reality.get("fingerprint", "chrome")

        elif protocol == "trojan":
            servers = settings.get("servers", [{}])[0]
            proxy = {
                "name": "test",
                "type": "trojan",
                "server": servers.get("address", parsed.host),
                "port": servers.get("port", parsed.port),
                "password": servers.get("password", ""),
                "skip-cert-verify": True,
            }

        elif protocol == "shadowsocks":
            servers = settings.get("servers", [{}])[0]
            proxy = {
                "name": "test",
                "type": "ss",
                "server": servers.get("address", parsed.host),
                "port": servers.get("port", parsed.port),
                "cipher": servers.get("method", "aes-256-gcm"),
                "password": servers.get("password", ""),
            }

        if not proxy:
            return None

        mihomo_config = {
            "mixed-port": socks_port,
            "mode": "global",
            "log-level": "silent",
            "proxies": [proxy],
            "proxy-groups": [{"name": "GLOBAL", "type": "select", "proxies": ["test"]}],
        }

        import yaml
        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix=f"MIHOMO_{socks_port}_", suffix=".yaml")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                yaml.dump(mihomo_config, f, allow_unicode=True)

            process = subprocess.Popen(
                [resolved_mihomo, "-f", temp_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(2.0)

            if process.poll() is not None:
                return None

            latency = _test_single_url(socks_port, test_url, timeout_seconds)
            if latency:
                return latency

            for url, _ in MULTI_TEST_URLS[:3]:
                if url != test_url:
                    latency = _test_single_url(socks_port, url, 12)
                    if latency:
                        return latency
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=2)
                except:
                    try:
                        process.kill()
                    except:
                        pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except:
                    pass
    except Exception as e:
        logger.debug(f"Mihomo benchmark error: {e}")
        return None


def _benchmark_with_singbox(
        singbox_path: str,
        parsed,
        socks_port: int,
        test_url: str,
        timeout_seconds: int,
) -> Optional[float]:
    """Benchmark using sing-box engine"""
    resolved_singbox = _resolve_executable_path("sing-box", singbox_path, [SINGBOX_PATH] + SINGBOX_PATH_FALLBACKS)
    if not resolved_singbox:
        return None

    try:
        outbound = parsed.outbound
        protocol = outbound.get("protocol", "")
        settings = outbound.get("settings", {})
        stream = outbound.get("streamSettings", {})

        sb_outbound = None
        if protocol == "vmess":
            vnext = settings.get("vnext", [{}])[0]
            users = vnext.get("users", [{}])[0]
            sb_outbound = {
                "type": "vmess",
                "tag": "proxy",
                "server": vnext.get("address", parsed.host),
                "server_port": vnext.get("port", parsed.port),
                "uuid": users.get("id", ""),
                "security": users.get("security", "auto"),
                "alter_id": users.get("alterId", 0),
            }
            network = stream.get("network", "tcp")
            if network == "ws":
                ws = stream.get("wsSettings", {})
                sb_outbound["transport"] = {"type": "ws", "path": ws.get("path", "/")}
            if stream.get("security") == "tls":
                sb_outbound["tls"] = {"enabled": True, "insecure": True}

        elif protocol == "vless":
            vnext = settings.get("vnext", [{}])[0]
            users = vnext.get("users", [{}])[0]
            sb_outbound = {
                "type": "vless",
                "tag": "proxy",
                "server": vnext.get("address", parsed.host),
                "server_port": vnext.get("port", parsed.port),
                "uuid": users.get("id", ""),
            }
            if users.get("flow"):
                sb_outbound["flow"] = users.get("flow")
            network = stream.get("network", "tcp")
            if network != "tcp":
                sb_outbound["transport"] = {"type": network}
            if stream.get("security") == "tls":
                sb_outbound["tls"] = {"enabled": True, "insecure": True}
            elif stream.get("security") == "reality":
                reality = stream.get("realitySettings", {})
                sb_outbound["tls"] = {
                    "enabled": True,
                    "server_name": reality.get("serverName", ""),
                    "utls": {"enabled": True, "fingerprint": reality.get("fingerprint", "chrome")},
                    "reality": {
                        "enabled": True,
                        "public_key": reality.get("publicKey", ""),
                        "short_id": reality.get("shortId", ""),
                    }
                }

        elif protocol == "trojan":
            servers = settings.get("servers", [{}])[0]
            sb_outbound = {
                "type": "trojan",
                "tag": "proxy",
                "server": servers.get("address", parsed.host),
                "server_port": servers.get("port", parsed.port),
                "password": servers.get("password", ""),
                "tls": {"enabled": True, "insecure": True},
            }

        elif protocol == "shadowsocks":
            servers = settings.get("servers", [{}])[0]
            sb_outbound = {
                "type": "shadowsocks",
                "tag": "proxy",
                "server": servers.get("address", parsed.host),
                "server_port": servers.get("port", parsed.port),
                "method": servers.get("method", "aes-256-gcm"),
                "password": servers.get("password", ""),
            }

        if not sb_outbound:
            return None

        singbox_config = {
            "log": {"level": "error"},
            "inbounds": [{
                "type": "mixed",
                "tag": "mixed-in",
                "listen": "127.0.0.1",
                "listen_port": socks_port,
            }],
            "outbounds": [sb_outbound, {"type": "direct", "tag": "direct"}],
        }

        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix=f"SINGBOX_{socks_port}_", suffix=".json")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(singbox_config, f, ensure_ascii=False)

            process = subprocess.Popen(
                [resolved_singbox, "run", "-c", temp_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            time.sleep(2.0)

            if process.poll() is not None:
                return None

            latency = _test_single_url(socks_port, test_url, timeout_seconds)
            if latency:
                return latency

            for url, _ in MULTI_TEST_URLS[:3]:
                if url != test_url:
                    latency = _test_single_url(socks_port, url, 12)
                    if latency:
                        return latency
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=2)
                except:
                    try:
                        process.kill()
                    except:
                        pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except:
                    pass
    except Exception as e:
        logger.debug(f"Sing-box benchmark error: {e}")
        return None


def _benchmark_multi_engine(
        parsed,
        socks_port: int,
        test_url: str,
        timeout_seconds: int,
        xray_path: str = XRAY_PATH,
        mihomo_path: str = MIHOMO_PATH,
        singbox_path: str = SINGBOX_PATH,
        try_all_engines: bool = False,
) -> Optional[float]:
    """

    :  xray ()
     try_all_engines=True:
    """
    latency = _benchmark_with_xray(xray_path, parsed.outbound, socks_port, test_url, timeout_seconds)
    if latency:
        return latency

    if not try_all_engines:
        return None

    latency = _benchmark_with_singbox(singbox_path, parsed, socks_port + 1000, test_url, min(timeout_seconds, 15))
    if latency:
        return latency

    latency = _benchmark_with_mihomo(mihomo_path, parsed, socks_port + 2000, test_url, min(timeout_seconds, 15))
    if latency:
        return latency

    return None


def _resolve_ip(host: str) -> Optional[str]:
    try:
        return socket.gethostbyname(host)
    except Exception:
        return None


def _get_country_code(ip: Optional[str]) -> Optional[str]:
    if not ip or ip == "0.0.0.0":
        return None
    try:
        resp = requests.get(f"https://ipapi.co/{ip}/country_code/", timeout=5)
        if resp.status_code >= 400:
            return None
        return resp.text.strip() or None
    except Exception:
        return None


def _signature(result: HunterBenchResult) -> str:
    base = result.ip if result.ip else result.host
    return f"{base}:{result.port}:{result.identity}"


def _read_lines(path: str) -> List[str]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return [line.strip() for line in f if line.strip()]
    except Exception:
        return []


def _append_unique_lines(path: str, lines: List[str]) -> int:
    existing = set(_read_lines(path))
    new_lines = [line for line in lines if line and line not in existing]
    if not new_lines:
        return 0
    try:
        with open(path, "a", encoding="utf-8") as f:
            for line in new_lines:
                f.write(line + "\n")
        return len(new_lines)
    except Exception:
        return 0


def _write_lines(path: str, lines: List[str]) -> None:
    try:
        with open(path, "w", encoding="utf-8") as f:
            for line in lines:
                if line:
                    f.write(line + "\n")
    except Exception:
        pass


def _load_json(path: str, default: Dict[str, Any]) -> Dict[str, Any]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                return data
            return default
    except Exception:
        return default


def _save_json(path: str, data: Dict[str, Any]) -> None:
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


def _load_env_file(path: str) -> None:
    try:
        if not path or not os.path.exists(path):
            return
        with open(path, "r", encoding="utf-8") as f:
            for raw_line in f:
                line = raw_line.strip()
                if not line or line.startswith("#"):
                    continue
                key = None
                value = None
                if line.lower().startswith("$env:"):
                    m = re.match(r"^\$env:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", line)
                    if not m:
                        continue
                    key = m.group(1).strip()
                    value = m.group(2).strip()
                elif "=" in line:
                    left, right = line.split("=", 1)
                    key = left.strip()
                    value = right.strip()
                if not key or value is None:
                    continue
                if value.startswith('"') and value.endswith('"') and len(value) >= 2:
                    value = value[1:-1]
                elif value.startswith("'") and value.endswith("'") and len(value) >= 2:
                    value = value[1:-1]
                os.environ.setdefault(key, value)
    except Exception:
        return


def _now_ts() -> int:
    return int(time.time())


def _tier_for_latency(latency_ms: float) -> str:
    if latency_ms < 200.0:
        return "gold"
    if latency_ms < 800.0:
        return "silver"
    if latency_ms > 2000.0:
        return "dead"
    return "silver"


def _kill_process_on_port(port: int) -> bool:
    """Kill any process using the specified port"""
    try:
        if sys.platform == "win32":
            # Find PID using netstat
            result = subprocess.run(
                ["netstat", "-ano"],
                capture_output=True, text=True, timeout=5
            )
            for line in result.stdout.split("\n"):
                if f":{port}" in line and "LISTENING" in line:
                    parts = line.split()
                    if parts:
                        pid = parts[-1]
                        try:
                            subprocess.run(["taskkill", "/F", "/PID", pid],
                                           capture_output=True, timeout=5)
                            logger.info(f"Killed process {pid} on port {port}")
                            time.sleep(0.5)
                            return True
                        except:
                            pass
        else:
            pids: Set[int] = set()

            if shutil.which("lsof"):
                result = subprocess.run(
                    ["lsof", "-ti", f":{port}"],
                    capture_output=True, text=True, timeout=5
                )
                for pid in (result.stdout or "").strip().split("\n"):
                    try:
                        if pid.strip():
                            pids.add(int(pid.strip()))
                    except Exception:
                        pass
            elif shutil.which("ss"):
                result = subprocess.run(
                    ["ss", "-ltnp", f"sport = :{port}"],
                    capture_output=True, text=True, timeout=5
                )
                for m in re.finditer(r"pid=(\d+)", result.stdout or ""):
                    try:
                        pids.add(int(m.group(1)))
                    except Exception:
                        pass
            elif shutil.which("fuser"):
                subprocess.run(["fuser", "-k", f"{port}/tcp"], capture_output=True, text=True, timeout=5)
                time.sleep(0.5)
                return True

            for pid in pids:
                try:
                    os.kill(pid, 9)
                    logger.info(f"Killed process {pid} on port {port}")
                    time.sleep(0.5)
                    return True
                except Exception:
                    pass
    except:
        pass
    return False


class MultiProxyServer:
    """
    Load-Balanced Multi-Proxy Server

    Single port (10808) with multiple backend proxies for high availability.
    Uses xray's built-in load balancer for traffic distribution.

    Features:
    - Single entry point on port 10808 (HTTP+SOCKS)
    - Multiple backend proxies with automatic failover
    - Hot-swap backends without dropping connections
    - Round-robin load balancing across working proxies
    """

    def __init__(self, xray_path: str, port: int = 10808, num_backends: int = 5,
                 health_check_interval: int = 60, fallback_to_direct: bool = True):
        self.xray_path = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS) or xray_path
        self.port = port
        self.num_backends = num_backends
        self.health_check_interval = health_check_interval
        self.fallback_to_direct = bool(fallback_to_direct)

        # Backend proxies: list of (uri, latency, healthy)
        self._backends: List[Dict[str, Any]] = []
        self._lock = threading.RLock()
        self._running = False
        self._process: Optional[subprocess.Popen] = None
        self._config_path: Optional[str] = None
        self._health_thread: Optional[threading.Thread] = None

        # Config pool
        self._available_configs: List[Tuple[str, float]] = []
        self._failed_uris: Set[str] = set()

        # Stats
        self._stats = {
            "restarts": 0,
            "health_checks": 0,
            "backend_swaps": 0,
            "last_restart": None,
        }

    def _create_balanced_config(self, backends: List[Dict[str, Any]]) -> Dict[str, Any]:
        """Create xray config with load balancer across multiple backends"""
        outbounds = []
        balancer_selectors = []

        # Add each backend as an outbound
        for i, backend in enumerate(backends):
            if not backend.get("healthy", True):
                continue
            parsed = _parse_any_uri(backend["uri"])
            if not parsed:
                continue

            outbound = parsed.outbound.copy()
            outbound["tag"] = f"proxy-{i}"
            outbounds.append(outbound)
            balancer_selectors.append(f"proxy-{i}")

        # If no working backends, optionally fall back to direct
        if not outbounds:
            if self.fallback_to_direct:
                outbounds.append({
                    "protocol": "freedom",
                    "tag": "direct",
                    "settings": {"domainStrategy": "AsIs"}
                })
                balancer_selectors = ["direct"]
            else:
                balancer_selectors = ["block"]

        # Add block outbound for routing
        outbounds.append({
            "protocol": "blackhole",
            "tag": "block",
            "settings": {}
        })

        config = {
            "log": {"loglevel": "warning"},
            "inbounds": [{
                "port": self.port,
                "listen": "0.0.0.0",
                "protocol": "socks",
                "tag": "socks-in",
                "settings": {"auth": "noauth", "udp": True},
                "sniffing": {
                    "enabled": True,
                    "destOverride": ["http", "tls", "quic"],
                    "routeOnly": False
                }
            }],
            "outbounds": outbounds,
            "routing": {
                "domainStrategy": "AsIs",
                "balancers": [{
                    "tag": "balancer",
                    "selector": balancer_selectors,
                    "strategy": {"type": "random"}
                }],
                "rules": [
                    {
                        "type": "field",
                        "inboundTag": ["socks-in"],
                        "balancerTag": "balancer"
                    }
                ]
            }
        }

        return config

    def _write_and_start(self, backends: List[Dict[str, Any]]) -> bool:
        """Write config and start/restart xray process"""
        config = self._create_balanced_config(backends)

        # Kill any existing process on our port
        _kill_process_on_port(self.port)

        # Write config to temp file
        try:
            if self._config_path and os.path.exists(self._config_path):
                os.remove(self._config_path)
        except:
            pass

        fd, self._config_path = tempfile.mkstemp(prefix="BALANCER_", suffix=".json")
        os.close(fd)
        with open(self._config_path, "w", encoding="utf-8") as f:
            json.dump(config, f, ensure_ascii=False, indent=2)
        logger.debug(f"[Balancer] Config written to: {self._config_path}")

        # Stop existing process
        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except:
                try:
                    self._process.kill()
                except:
                    pass

        # Start new process
        try:
            self._process = subprocess.Popen(
                [self.xray_path, "run", "-c", self._config_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            time.sleep(1.5)  # Wait longer for xray to start

            if self._process.poll() is not None:
                # Get stderr for debugging
                stdout_output = ""
                stderr_output = ""
                try:
                    stdout, stderr = self._process.communicate(timeout=2)
                    stdout_output = stdout.decode('utf-8', errors='ignore')[:300]
                    stderr_output = stderr.decode('utf-8', errors='ignore')[:300]
                except:
                    pass
                logger.error(f"[Balancer] xray died - stdout: {stdout_output}")
                logger.error(f"[Balancer] xray died - stderr: {stderr_output}")
                logger.error(f"[Balancer] Config file: {self._config_path}")
                # Don't delete config file so we can inspect it
                return False

            self._stats["restarts"] += 1
            self._stats["last_restart"] = _now_ts()
            return True

        except Exception as e:
            logger.error(f"[Balancer] Failed to start xray: {e}")
            return False

    def _test_backend(self, uri: str, timeout: int = 8) -> Optional[float]:
        """Test a backend proxy and return latency"""
        parsed = _parse_any_uri(uri)
        if not parsed:
            return None

        # Use a test port
        test_port = self.port + 100 + (hash(uri) % 50)

        config = {
            "log": {"loglevel": "none"},
            "inbounds": [{
                "port": test_port,
                "listen": "127.0.0.1",
                "protocol": "socks",
                "settings": {"auth": "noauth", "udp": False}
            }],
            "outbounds": [parsed.outbound]
        }

        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix="TEST_", suffix=".json")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(config, f, ensure_ascii=False)

            process = subprocess.Popen(
                [self.xray_path, "run", "-c", temp_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            time.sleep(0.8)

            if process.poll() is not None:
                return None

            latency = _test_single_url(test_port, "https://cp.cloudflare.com/", timeout)
            if not latency:
                latency = _test_single_url(test_port, "https://www.google.com/generate_204", timeout)
            return latency

        except:
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=1)
                except:
                    try:
                        process.kill()
                    except:
                        pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except:
                    pass

    def _find_working_backends(self, count: int = 5) -> List[Dict[str, Any]]:
        """Find working backends from available configs"""
        working = []
        tried = set()

        with self._lock:
            configs = list(self._available_configs)

        # Sort by latency (fastest first)
        configs.sort(key=lambda x: x[1])

        for uri, orig_latency in configs:
            if len(working) >= count:
                break
            if uri in tried or uri in self._failed_uris:
                continue
            tried.add(uri)

            latency = self._test_backend(uri, timeout=10)
            if latency:
                parsed = _parse_any_uri(uri)
                ps = parsed.ps[:25] if parsed and parsed.ps else "Unknown"
                working.append({
                    "uri": uri,
                    "latency": latency,
                    "healthy": True,
                    "ps": ps,
                    "added_at": _now_ts()
                })
                logger.info(f"[Balancer] Backend {len(working)}: {ps} ({latency:.0f}ms)")
            else:
                with self._lock:
                    self._failed_uris.add(uri)

        return working

    def _health_check_loop(self) -> None:
        """Background health check thread"""
        logger.info(f"[Balancer] Health monitor started (interval: {self.health_check_interval}s)")

        while self._running:
            time.sleep(self.health_check_interval)
            if not self._running:
                break

            self._stats["health_checks"] += 1

            # Test the main port
            latency = _test_single_url(self.port, "https://cp.cloudflare.com/", 10)
            if not latency:
                latency = _test_single_url(self.port, "https://www.google.com/generate_204", 10)

            if latency:
                healthy_count = sum(1 for b in self._backends if b.get("healthy"))
                logger.info(f"[Balancer] OK {latency:.0f}ms | {healthy_count} backends active")
            else:
                logger.warning("[Balancer] Main port not responding, refreshing backends...")
                self._refresh_backends()

    def _refresh_backends(self) -> None:
        """Find new working backends and restart"""
        # Clear failed URIs periodically
        with self._lock:
            if len(self._failed_uris) > 100:
                self._failed_uris.clear()

        new_backends = self._find_working_backends(self.num_backends)

        if new_backends:
            with self._lock:
                self._backends = new_backends

            if self._write_and_start(new_backends):
                logger.info(f"[Balancer] Restarted with {len(new_backends)} backends")
                self._stats["backend_swaps"] += 1
            else:
                logger.error("[Balancer] Failed to restart with new backends")
        else:
            logger.warning("[Balancer] No working backends found")

    def update_available_configs(self, configs: List[Tuple[str, float]]) -> None:
        """Update the pool of available configs"""
        with self._lock:
            self._available_configs = list(configs)
        logger.info(f"[Balancer] Config pool updated: {len(configs)} configs")

    def start(self, initial_configs: List[Tuple[str, float]] = None) -> None:
        """Start the load balancer"""
        if self._running:
            return

        self._running = True

        if initial_configs:
            self.update_available_configs(initial_configs)

        logger.info(f"[Balancer] Starting on port {self.port} with {self.num_backends} backend slots")

        # Find initial working backends
        backends = self._find_working_backends(self.num_backends)

        if backends:
            with self._lock:
                self._backends = backends

            if self._write_and_start(backends):
                logger.info(f"[Balancer] Started with {len(backends)} working backends")
            else:
                logger.error("[Balancer] Failed to start initial process")
        else:
            logger.warning("[Balancer] No working backends found at startup, will retry...")
            # Start with direct connection as placeholder
            self._write_and_start([])

        # Start health check thread
        self._health_thread = threading.Thread(target=self._health_check_loop, daemon=True)
        self._health_thread.start()

        logger.info(f"[Balancer] Server ready | HTTP+SOCKS on :{self.port}")

    def stop(self) -> None:
        """Stop the load balancer"""
        self._running = False

        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except:
                try:
                    self._process.kill()
                except:
                    pass

        if self._config_path and os.path.exists(self._config_path):
            try:
                os.remove(self._config_path)
            except:
                pass

        logger.info("[Balancer] Stopped")

    def get_ports(self) -> List[int]:
        """Get the single port being served"""
        return [self.port] if self._running else []

    def get_status(self) -> Dict[str, Any]:
        """Get current status"""
        with self._lock:
            backends = list(self._backends)

        healthy_count = sum(1 for b in backends if b.get("healthy"))

        return {
            "running": self._running,
            "port": self.port,
            "backends": healthy_count,
            "total_backends": len(backends),
            "stats": self._stats.copy(),
            "backend_list": [
                {
                    "ps": b.get("ps", "Unknown"),
                    "latency_ms": b.get("latency", 0),
                    "healthy": b.get("healthy", False),
                }
                for b in backends
            ]
        }


class BridgeManager:
    def __init__(self, xray_path: str, socks_port_base: int, max_bridges: int) -> None:
        self.xray_path = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS) or xray_path
        self.socks_port_base = socks_port_base
        self.max_bridges = max_bridges
        self._bridges: List[Tuple[int, subprocess.Popen, str]] = []

    def stop_all(self) -> None:
        for port, proc, cfg_path in self._bridges:
            try:
                proc.terminate()
                proc.wait(timeout=1)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
            if cfg_path and os.path.exists(cfg_path):
                try:
                    os.remove(cfg_path)
                except Exception:
                    pass
        self._bridges = []

    def ports(self) -> List[int]:
        return [p for p, _, _ in self._bridges]

    def spawn_from_uris(self, uris: List[str]) -> None:
        self.stop_all()
        if not _check_xray_path(self.xray_path):
            return
        port = self.socks_port_base
        for uri in uris:
            if len(self._bridges) >= self.max_bridges:
                break
            parsed = _parse_any_uri(uri)
            if not parsed:
                continue
            xray_config = {
                "log": {"loglevel": "warning"},
                "inbounds": [
                    {"port": port, "listen": "127.0.0.1", "protocol": "socks",
                     "settings": {"auth": "noauth", "udp": True}}
                ],
                "outbounds": [parsed.outbound],
            }
            cfg_path = None
            try:
                fd, cfg_path = tempfile.mkstemp(prefix=f"bridge_{port}_", suffix=".json")
                os.close(fd)
                with open(cfg_path, "w", encoding="utf-8") as f:
                    json.dump(xray_config, f, ensure_ascii=False)
                proc = subprocess.Popen([self.xray_path, "run", "-c", cfg_path], stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL)
                time.sleep(0.4)
                self._bridges.append((port, proc, cfg_path))
                port += 1
            except Exception:
                if cfg_path and os.path.exists(cfg_path):
                    try:
                        os.remove(cfg_path)
                    except Exception:
                        pass


async def _try_connect_telegram(
        session_name: str,
        api_id: int,
        api_hash: str,
        phone: str,
        proxy_ports: List[int],
) -> Optional[TelegramClient]:
    if TelegramClient is None:
        logger.error("Telethon is not available. Install telethon to use hunter mode.")
        return None
    candidate_proxies: List[Optional[Tuple[str, str, int]]] = []
    for port in proxy_ports:
        candidate_proxies.append(("socks5", "127.0.0.1", port))
    candidate_proxies.append(None)

    last_error = None
    client: Optional[TelegramClient] = None
    for proxy in candidate_proxies:
        try:
            if proxy is None:
                logger.info("Trying DIRECT connection")
            else:
                logger.info(f"Trying SOCKS5 127.0.0.1:{proxy[2]}")
            client = TelegramClient(session_name, api_id, api_hash, proxy=proxy, connection_retries=0)
            await client.connect()
            if not await client.is_user_authorized():
                await client.start(phone=phone)
            if await client.is_user_authorized():
                logger.info("Telegram connected and authorized")
                return client
            await client.disconnect()
        except Exception as exc:
            last_error = exc
            try:
                if client is not None:
                    await client.disconnect()
            except Exception:
                pass
            continue
    if last_error:
        logger.error(f"Telegram connection failed: {last_error}")
    return None


async def _scrape_telegram_configs(
        client: TelegramClient,
        targets: List[str],
        scan_limit: int,
        max_total: int,
        npvt_scan_limit: int,
        last_seen: Dict[str, int],
        heartbeat: Optional[ResilientHeartbeat] = None,
) -> Tuple[Set[str], bool]:
    """
    Scrape configs from Telegram channels with resilient connection handling.
    Returns: (harvested_configs, connection_lost)
    """
    harvested: Set[str] = set()
    connection_lost = False
    consecutive_errors = 0
    max_consecutive_errors = 3

    for target in targets:
        if len(harvested) >= max_total:
            break

        if consecutive_errors >= max_consecutive_errors:
            logger.warning(f"Too many consecutive errors ({consecutive_errors}), stopping scrape")
            connection_lost = True
            break

        logger.info(f"Scraping {target} (target={max_total})")

        try:
            if not client.is_connected():
                logger.warning(f"Connection lost before scraping {target}")
                connection_lost = True
                if heartbeat:
                    reconnected = await heartbeat.try_reconnect(client)
                    if not reconnected:
                        break
                    connection_lost = False
                else:
                    break
        except Exception:
            pass

        try:
            try:
                from telethon.tl.functions.channels import JoinChannelRequest
            except Exception:
                JoinChannelRequest = None
            if JoinChannelRequest is not None:
                try:
                    await asyncio.wait_for(client(JoinChannelRequest(target)), timeout=15)
                    logger.debug(f"Joined {target}")
                except asyncio.TimeoutError:
                    logger.debug(f"Join timeout for {target}")
                except Exception:
                    pass
        except Exception:
            pass

        try:
            files_downloaded = 0
            async for msg in client.iter_messages(target, limit=npvt_scan_limit):
                if len(harvested) >= max_total:
                    break
                file_obj = getattr(msg, "file", None)
                if not file_obj or not getattr(file_obj, "name", None):
                    continue
                name = (file_obj.name or "").lower()
                if not name.endswith((".npvt", ".npv", ".txt", ".json", ".yaml", ".yml", ".conf")):
                    continue
                if file_obj.size and file_obj.size > 10 * 1024 * 1024:
                    continue
                temp_path = None
                try:
                    fd, temp_path = tempfile.mkstemp(prefix="tg_", suffix="_" + os.path.basename(name))
                    os.close(fd)
                    await asyncio.wait_for(client.download_media(msg, file=temp_path), timeout=30)
                    with open(temp_path, "rb") as f:
                        data = f.read()
                    before = len(harvested)
                    harvested.update(_extract_raw_uris_from_bytes(data))
                    added = len(harvested) - before
                    if added > 0:
                        logger.info(f"Downloaded {name} from {target} (+{added} configs)")
                        files_downloaded += 1
                except asyncio.TimeoutError:
                    logger.debug(f"Download timeout for {name}")
                except Exception as e:
                    logger.debug(f"Failed to download {name}: {e}")
                finally:
                    try:
                        if temp_path and os.path.exists(temp_path):
                            os.remove(temp_path)
                    except Exception:
                        pass
                if files_downloaded >= 3:
                    break
            consecutive_errors = 0
        except asyncio.CancelledError:
            logger.warning(f"Operation cancelled during file scraping for {target}")
            connection_lost = True
            break
        except Exception as e:
            error_str = str(e).lower()
            is_connection_error = (
                    "disconnected" in error_str or
                    "connection" in error_str or
                    "security error" in error_str or
                    "too many messages" in error_str
            )
            if is_connection_error:
                consecutive_errors += 1
                logger.warning(f"Connection error during file scraping for {target}: {e}")
                if heartbeat:
                    reconnected = await heartbeat.try_reconnect(client)
                    if reconnected:
                        consecutive_errors = 0
                    else:
                        connection_lost = True
            else:
                logger.debug(f"File scraping error for {target}: {e}")

        try:
            min_id = int(last_seen.get(target, 0) or 0)
            last_id_processed = min_id
            msg_count = 0
            async for msg in client.iter_messages(target, limit=scan_limit, min_id=min_id, reverse=True):
                if len(harvested) >= max_total:
                    break
                msg_id = int(getattr(msg, "id", 0) or 0)
                if msg_id > last_id_processed:
                    last_id_processed = msg_id
                text = getattr(msg, "text", None) or ""
                before = len(harvested)
                harvested.update(_extract_raw_uris_from_text(text))
                if len(harvested) != before:
                    logger.debug(f"Found {len(harvested) - before} configs in message from {target}")
                msg_count += 1
            if last_id_processed > min_id:
                last_seen[target] = last_id_processed
            logger.info(f"Scanned {msg_count} messages from {target}")
            consecutive_errors = 0
        except asyncio.CancelledError:
            logger.warning(f"Operation cancelled for {target} - session sync issue detected")
            connection_lost = True
            break
        except Exception as e:
            error_str = str(e).lower()
            is_connection_error = (
                    "disconnected" in error_str or
                    "connection" in error_str or
                    "cannot send" in error_str or
                    "security error" in error_str or
                    "too many messages" in error_str or
                    "cancelled" in error_str
            )
            if is_connection_error:
                consecutive_errors += 1
                logger.warning(f"Connection error for {target}: {e}")
                if heartbeat:
                    reconnected = await heartbeat.try_reconnect(client)
                    if reconnected:
                        consecutive_errors = 0
                    else:
                        connection_lost = True
            else:
                logger.warning(f"Message scraping error for {target}: {e}")
            continue

    return harvested, connection_lost


def _quick_connectivity_test(xray_path: str, uri: str, socks_port: int, timeout: int = 8) -> bool:
    """
       -      xray
    """
    parsed = _parse_any_uri(uri)
    if not parsed:
        return False

    resolved_xray = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not resolved_xray or not _check_xray_path(resolved_xray):
        return False

    xray_config = {
        "log": {"loglevel": "none"},
        "inbounds": [
            {"port": socks_port, "listen": "127.0.0.1", "protocol": "socks",
             "settings": {"auth": "noauth", "udp": False}}
        ],
        "outbounds": [parsed.outbound],
    }
    temp_path = None
    process = None
    try:
        fd, temp_path = tempfile.mkstemp(prefix=f"QUICK_{socks_port}_", suffix=".json")
        os.close(fd)
        with open(temp_path, "w", encoding="utf-8") as f:
            json.dump(xray_config, f, ensure_ascii=False)
        process = subprocess.Popen(
            [resolved_xray, "run", "-c", temp_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        time.sleep(1.5)

        if process.poll() is not None:
            return False

        curl_cmd = "curl"
        result = subprocess.run(
            [curl_cmd, "-x", f"socks5h://127.0.0.1:{socks_port}",
             "-s", "-o", _curl_null_output(),
             "-w", "%{http_code}", "-m", str(timeout), "--connect-timeout", "5",
             "-k", "https://1.1.1.1/cdn-cgi/trace"],
            capture_output=True, text=True, timeout=timeout + 3
        )
        if result.returncode == 0:
            try:
                code = int(result.stdout.strip())
                return code < 400 or code == 204
            except:
                pass
        return False
    except:
        return False
    finally:
        if process:
            try:
                process.terminate()
                process.wait(timeout=1)
            except:
                try:
                    process.kill()
                except:
                    pass
        if temp_path and os.path.exists(temp_path):
            try:
                os.remove(temp_path)
            except:
                pass


def _is_cdn_based(uri: str) -> bool:
    """Check if config uses CDN/whitelisted domain (Domain Fronting)"""
    uri_lower = uri.lower()
    for domain in CDN_WHITELIST_DOMAINS:
        if domain.lower() in uri_lower:
            return True
    return False


def _has_anti_dpi_features(uri: str) -> int:
    """Score config based on anti-DPI features (higher = better)"""
    uri_lower = uri.lower()
    score = 0
    for indicator in ANTI_DPI_INDICATORS:
        if indicator in uri_lower:
            score += 1
    # Bonus for whitelisted ports
    for port in WHITELIST_PORTS:
        if f":{port}" in uri:
            score += 1
            break
    # Bonus for TLS fingerprint evasion
    for fp in DPI_EVASION_FINGERPRINTS:
        if f"fp={fp}" in uri_lower:
            score += 2
            break
    # Bonus for CDN-based configs
    if _is_cdn_based(uri):
        score += 3
    return score


def _is_likely_blocked(uri: str) -> bool:
    """Check if config is likely blocked by Iran's filtering infrastructure"""
    uri_lower = uri.lower()
    for pattern in IRAN_BLOCKED_PATTERNS:
        if pattern in uri_lower:
            return True
    # Check for non-standard ports that are often blocked
    try:
        parsed = urlparse(uri)
        port = parsed.port
        if port and port not in WHITELIST_PORTS and port not in [80, 443, 8080, 8443]:
            # Non-standard ports are more likely to be blocked
            pass
    except:
        pass
    return False


def _filter_blocked_configs(uris: List[str]) -> List[str]:
    """Filter out configs that are likely blocked by Iran's DPI"""
    return [uri for uri in uris if not _is_likely_blocked(uri)]


def _is_ipv4_preferred(uri: str) -> bool:
    """Check if config uses IPv4 (preferred over IPv6 due to Iran's IPv6 filtering)"""
    # IPv6 addresses contain colons in brackets like [2001:db8::1]
    if "[" in uri and "]" in uri:
        return False  # IPv6
    return True


def _prioritize_configs(uris: List[str]) -> List[str]:
    """
    Prioritize configs for testing - most resilient against Iran's filtering first.

    Priority based on Iran's filtering techniques (BGP, DPI, Protocol Suppression):
    1. VLESS Reality + CDN + IPv4 (best: invisible to DPI, uses whitelisted domains)
    2. VLESS Reality + IPv4 (excellent: TLS fingerprint mimics real sites)
    3. VLESS/Trojan + gRPC/H2 + TLS 443 (very good: looks like HTTP/2)
    4. VLESS/Trojan + WebSocket + TLS 443 (good: looks like HTTPS)
    5. VMess + WebSocket + TLS + CDN (decent: obfuscated but detectable)
    6. Other TLS-based on port 443
    7. Everything else (likely blocked)

    IPv6 configs are deprioritized due to ~98.5% IPv6 suppression in Iran.
    """
    # Filter out likely-blocked configs first
    filtered_uris = _filter_blocked_configs(uris)

    # Priority buckets
    tier_1_reality_cdn = []  # VLESS Reality + CDN (ultimate)
    tier_2_reality = []  # VLESS Reality
    tier_3_grpc_h2 = []  # gRPC/H2 based
    tier_4_ws_tls_443 = []  # WebSocket + TLS on 443
    tier_5_vmess_ws_cdn = []  # VMess WS TLS with CDN
    tier_6_tls_443 = []  # Other TLS on 443
    tier_7_ipv6 = []  # IPv6 configs (deprioritized)
    tier_8_other = []  # Everything else

    for uri in filtered_uris:
        uri_lower = uri.lower()
        is_ipv4 = _is_ipv4_preferred(uri)
        is_cdn = _is_cdn_based(uri)
        anti_dpi_score = _has_anti_dpi_features(uri)

        # Deprioritize IPv6 due to Iran's IPv6 suppression
        if not is_ipv4:
            tier_7_ipv6.append(uri)
            continue

        # VLESS configs
        if uri_lower.startswith("vless://"):
            is_reality = "reality" in uri_lower or "pbk=" in uri_lower
            is_grpc = "grpc" in uri_lower or "gun" in uri_lower
            is_h2 = "h2" in uri_lower or "http/2" in uri_lower
            is_ws = "ws" in uri_lower or "websocket" in uri_lower
            is_tls_443 = ":443" in uri and ("security=tls" in uri_lower or "tls" in uri_lower)

            if is_reality and is_cdn:
                tier_1_reality_cdn.append(uri)
            elif is_reality:
                tier_2_reality.append(uri)
            elif is_grpc or is_h2:
                tier_3_grpc_h2.append(uri)
            elif is_ws and is_tls_443:
                tier_4_ws_tls_443.append(uri)
            elif is_tls_443:
                tier_6_tls_443.append(uri)
            else:
                tier_8_other.append(uri)

        # Trojan configs
        elif uri_lower.startswith("trojan://"):
            is_grpc = "grpc" in uri_lower or "gun" in uri_lower
            is_ws = "ws" in uri_lower or "websocket" in uri_lower
            is_443 = ":443" in uri

            if is_grpc:
                tier_3_grpc_h2.append(uri)
            elif is_ws and is_443:
                tier_4_ws_tls_443.append(uri)
            elif is_443:
                tier_6_tls_443.append(uri)
            else:
                tier_8_other.append(uri)

        # VMess configs
        elif uri_lower.startswith("vmess://"):
            try:
                payload = uri[len("vmess://"):]
                decoded = _safe_b64decode(payload).decode("utf-8", errors="ignore")
                is_ws = '"net":"ws"' in decoded
                is_tls = '"tls":"tls"' in decoded
                is_grpc = '"net":"grpc"' in decoded or '"net":"gun"' in decoded
                port_443 = '"port":"443"' in decoded or '"port":443' in decoded

                if is_grpc and is_tls:
                    tier_3_grpc_h2.append(uri)
                elif is_ws and is_tls and is_cdn:
                    tier_5_vmess_ws_cdn.append(uri)
                elif is_ws and is_tls and port_443:
                    tier_4_ws_tls_443.append(uri)
                elif is_tls and port_443:
                    tier_6_tls_443.append(uri)
                else:
                    tier_8_other.append(uri)
            except:
                tier_8_other.append(uri)
        else:
            tier_8_other.append(uri)

    # Shuffle within each tier for load balancing
    import random
    for tier in [tier_1_reality_cdn, tier_2_reality, tier_3_grpc_h2,
                 tier_4_ws_tls_443, tier_5_vmess_ws_cdn, tier_6_tls_443,
                 tier_7_ipv6, tier_8_other]:
        random.shuffle(tier)

    logger.info(f"Anti-DPI Priority: Reality+CDN={len(tier_1_reality_cdn)}, Reality={len(tier_2_reality)}, "
                f"gRPC/H2={len(tier_3_grpc_h2)}, WS+TLS={len(tier_4_ws_tls_443)}, "
                f"VMess+CDN={len(tier_5_vmess_ws_cdn)}, TLS443={len(tier_6_tls_443)}, "
                f"IPv6={len(tier_7_ipv6)}, Other={len(tier_8_other)}")

    return (tier_1_reality_cdn + tier_2_reality + tier_3_grpc_h2 +
            tier_4_ws_tls_443 + tier_5_vmess_ws_cdn + tier_6_tls_443 +
            tier_7_ipv6 + tier_8_other)


def _bench_one(
        uri: str,
        idx: int,
        xray_path: str,
        socks_base: int,
        test_url: str,
        google_test_url: str,
        timeout_seconds: int,
        use_multi_engine: bool = False,
) -> Optional[HunterBenchResult]:
    try:
        parsed = _parse_any_uri(uri)
        if not parsed:
            return None
        port = socks_base + (idx % 2000)

        latency = _benchmark_with_xray(xray_path, parsed.outbound, port, test_url, timeout_seconds)

        if latency is None and use_multi_engine:
            latency = _benchmark_with_singbox(SINGBOX_PATH, parsed, port + 1000, test_url, min(timeout_seconds, 12))

        if latency is None and use_multi_engine:
            latency = _benchmark_with_mihomo(MIHOMO_PATH, parsed, port + 2000, test_url, min(timeout_seconds, 12))

        if latency is None:
            return None
        ip = _resolve_ip(parsed.host)
        cc = _get_country_code(ip)
        region = get_region(cc) if cc else "Other"
        tier = _tier_for_latency(latency)
        return HunterBenchResult(
            uri=parsed.uri,
            outbound=parsed.outbound,
            host=parsed.host,
            port=parsed.port,
            identity=parsed.identity,
            ps=parsed.ps,
            latency_ms=latency,
            ip=ip,
            country_code=cc,
            region=region,
            tier=tier,
        )
    except Exception as e:
        logger.debug(f"Bench error: {e}")
        return None


def _validate_and_tier(
        uris: List[str],
        xray_path: str,
        socks_base: int,
        max_workers: int,
        timeout_seconds: int,
        test_url: str,
        google_test_url: str,
) -> List[HunterBenchResult]:
    if not _check_xray_path(xray_path):
        logger.error("Xray core not found at '%s'", xray_path)
        return []

    prioritized_uris = _prioritize_configs(uris)
    logger.info(f"Prioritized configs: testing most resilient protocols first")

    results: List[HunterBenchResult] = []
    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        futures = {
            executor.submit(_bench_one, uri, i, xray_path, socks_base, test_url, google_test_url, timeout_seconds): uri
            for i, uri in enumerate(prioritized_uris)
        }
        for future in tqdm(
                as_completed(futures),
                total=len(futures),
                desc="Testing",
                unit="cfg",
                ncols=80,
        ):
            try:
                res = future.result()
                if res and res.tier != "dead":
                    results.append(res)
                    logger.info(f"{res.ps[:30]} | {res.latency_ms:.0f}ms | {res.tier} | {res.region}")
            except Exception:
                continue
    dedup: Dict[str, HunterBenchResult] = {}
    for res in results:
        key = _signature(res)
        if key not in dedup or res.latency_ms < dedup[key].latency_ms:
            dedup[key] = res
    return sorted(dedup.values(), key=lambda r: r.latency_ms)


def _gemini_probe_config(
        xray_path: str,
        outbound: Dict[str, Any],
        socks_port: int,
        google_test_url: str,
        gemini_test_url: str,
        timeout_seconds: int,
) -> bool:
    resolved_xray = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not resolved_xray or not _check_xray_path(resolved_xray):
        return False

    xray_config = {
        "log": {"loglevel": "warning"},
        "inbounds": [
            {"port": int(socks_port), "listen": "127.0.0.1", "protocol": "socks", "settings": {"auth": "noauth", "udp": True}}
        ],
        "outbounds": [outbound],
    }

    temp_path = None
    process = None
    try:
        fd, temp_path = tempfile.mkstemp(prefix=f"GEMINI_{socks_port}_", suffix=".json")
        os.close(fd)
        with open(temp_path, "w", encoding="utf-8") as f:
            json.dump(xray_config, f, ensure_ascii=False)

        process = subprocess.Popen(
            [resolved_xray, "run", "-c", temp_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
        )
        time.sleep(0.8)
        if process.poll() is not None:
            return False

        if not _test_single_url(int(socks_port), str(google_test_url), max(8, min(int(timeout_seconds), 12))):
            return False

        proxies = {
            "http": f"socks5h://127.0.0.1:{int(socks_port)}",
            "https": f"socks5h://127.0.0.1:{int(socks_port)}",
        }
        headers = {"User-Agent": _random_user_agent()}
        try:
            resp = requests.get(
                str(gemini_test_url),
                headers=headers,
                timeout=max(8, min(int(timeout_seconds), 15)),
                proxies=proxies,
                verify=False,
            )
            if resp.status_code in (200, 302):
                text = (resp.text or "").lower()
                if "gemini" in text:
                    return True
                if "gemini.google.com" in (resp.url or "") or "accounts.google.com" in (resp.url or ""):
                    return True
        except Exception:
            return False
        return False
    except Exception:
        return False
    finally:
        if process:
            try:
                process.terminate()
                process.wait(timeout=2)
            except Exception:
                try:
                    process.kill()
                except Exception:
                    pass
        if temp_path and os.path.exists(temp_path):
            try:
                os.remove(temp_path)
            except Exception:
                pass


def _select_gemini_allowed_configs(
        validated: List[HunterBenchResult],
        xray_path: str,
        socks_base: int,
        google_test_url: str,
        gemini_test_url: str,
        timeout_seconds: int,
        max_candidates: int,
        max_workers: int,
) -> List[Tuple[str, float]]:
    if not validated:
        return []

    candidates = validated[: max(1, int(max_candidates))]

    def _probe_one(i: int, r: HunterBenchResult) -> Optional[Tuple[str, float]]:
        port = int(socks_base) + (i % 200)
        ok = _gemini_probe_config(
            xray_path=xray_path,
            outbound=r.outbound,
            socks_port=port,
            google_test_url=google_test_url,
            gemini_test_url=gemini_test_url,
            timeout_seconds=timeout_seconds,
        )
        if ok:
            return (r.uri, float(r.latency_ms))
        return None

    out: List[Tuple[str, float]] = []
    with ThreadPoolExecutor(max_workers=max(1, int(max_workers))) as ex:
        futs = [ex.submit(_probe_one, i, r) for i, r in enumerate(candidates)]
        for f in as_completed(futs):
            try:
                got = f.result()
                if got:
                    out.append(got)
            except Exception:
                continue
    out.sort(key=lambda x: x[1])
    return out


async def _send_report(
        api_id: int,
        api_hash: str,
        bot_token: str,
        channel_id: int,
        text: str,
        proxy_ports: Optional[List[int]] = None,
        validated_configs: Optional[List] = None,
        xray_path: str = "",
        bridge_base: int = 15808,
) -> None:
    if TelegramClient is None:
        return
    if not bot_token or ":" not in bot_token:
        return

    MAX_MSG_LEN = 4000
    messages = []
    if len(text) <= MAX_MSG_LEN:
        messages = [text]
    else:
        chunks = text.split("\n\n")
        current = ""
        for chunk in chunks:
            if len(current) + len(chunk) + 2 <= MAX_MSG_LEN:
                current += chunk + "\n\n"
            else:
                if current:
                    messages.append(current.strip())
                current = chunk + "\n\n"
        if current:
            messages.append(current.strip())

    try:
        candidate_proxies: List[Optional[Tuple[str, str, int]]] = []

        standard_ports = [1080, 1083, 10808, 10809, 2080, 2081, 7890, 7891, 8080, 8118]
        for port in standard_ports:
            candidate_proxies.append(("socks5", "127.0.0.1", port))

        if _is_tor_running():
            candidate_proxies.append(("socks5", "127.0.0.1", TOR_SOCKS_PORT))
            logger.info(f"Tor available on port {TOR_SOCKS_PORT}")

        if proxy_ports:
            for port in proxy_ports:
                if port not in standard_ports:
                    candidate_proxies.append(("socks5", "127.0.0.1", int(port)))

        candidate_proxies.append(None)

        last_error = None
        for proxy in candidate_proxies:
            client: Optional[TelegramClient] = None
            try:
                client = TelegramClient("HUNTER_reporter_session", api_id, api_hash, proxy=proxy, connection_retries=0)
                await client.start(bot_token=bot_token)
                for msg in messages:
                    await client.send_message(channel_id, msg)
                    await asyncio.sleep(0.5)
                await client.disconnect()
                logger.info(f"Report sent to Telegram ({len(messages)} messages)")
                return
            except Exception as exc:
                last_error = exc
                try:
                    if client is not None:
                        await client.disconnect()
                except Exception:
                    pass
                continue
        if last_error:
            logger.warning(f"Report failed: {last_error}")
    except Exception as exc:
        logger.warning(f"Report failed: {exc}")


async def run_autonomous_hunter() -> None:
    base_dir = Path(__file__).resolve().parent

    def _norm_file_path(p: str) -> str:
        if not p:
            return p
        if os.path.isabs(p):
            return p
        return str(base_dir / p)

    def _norm_exec_path(p: str) -> str:
        if not p:
            return p
        if os.path.isabs(p):
            return p
        if "/" in p or "\\" in p:
            return str(base_dir / p)
        return p

    secrets_file = _norm_file_path(os.getenv("HUNTER_SECRETS_FILE", "hunter_secrets.env"))
    _load_env_file(secrets_file)

    api_id = int(os.getenv("HUNTER_API_ID", "31828870"))
    api_hash = os.getenv("HUNTER_API_HASH", "")
    phone = os.getenv("HUNTER_PHONE", "")
    bot_token = os.getenv("TOKEN", "")
    report_channel_str = os.getenv("CHAT_ID", "-1002567385742")
    report_channel = int(report_channel_str)
    session_name = _norm_file_path(os.getenv("HUNTER_SESSION", "HUNTER_session"))

    if not api_id or not api_hash:
        logger.error("Telegram API credentials missing. Set HUNTER_API_ID + HUNTER_API_HASH")
        return

    targets = [t.strip() for t in os.getenv("HUNTER_TARGETS", "").split(",") if t.strip()]
    if not targets:
        targets = [
            "v2rayngvpn", "mitivpn", "proxymtprotoir", "Porteqal3", "v2ray_configs_pool", "vmessorg",
            "V2rayNGn", "v2ray_swhil", "VmessProtocol", "PrivateVPNs", "DirectVPN",
            "v2rayNG_Matsuri", "FalconPolV2rayNG", "ShadowSocks_s", "napsternetv_config",
            "VpnProSec", "VlessConfig", "proxy_mtm", "iP_CF", "ConfigsHUB"
        ]

    xray_path = _norm_exec_path(os.getenv("HUNTER_XRAY_PATH", XRAY_PATH))
    resolved_xray_path = _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS)
    if not resolved_xray_path:
        logger.error("Xray core not found in any configured path")
        return
    xray_path = resolved_xray_path

    test_url = os.getenv("HUNTER_TEST_URL", "https://www.cloudflare.com/cdn-cgi/trace")
    google_test_url = os.getenv("HUNTER_GOOGLE_TEST_URL", "https://www.google.com/generate_204")
    scan_limit = int(os.getenv("HUNTER_SCAN_LIMIT", "50"))
    latest_total = int(os.getenv("HUNTER_LATEST_URIS", "500"))
    max_total = int(os.getenv("HUNTER_MAX_CONFIGS", "3000"))
    npvt_scan_limit = int(os.getenv("HUNTER_NPVT_SCAN", "50"))
    max_workers = int(os.getenv("HUNTER_WORKERS", "50"))  # Increased for faster scanning
    timeout_seconds = int(os.getenv("HUNTER_TEST_TIMEOUT", "10"))  # Reduced for faster scanning
    sleep_seconds = int(os.getenv("HUNTER_SLEEP", "300"))
    cleanup_interval = int(os.getenv("HUNTER_CLEANUP", str(24 * 3600)))
    recursive_ratio = float(os.getenv("HUNTER_RECURSIVE_RATIO", "0.15"))
    max_bridges = int(os.getenv("HUNTER_MAX_BRIDGES", "8"))
    bridge_base = int(os.getenv("HUNTER_BRIDGE_BASE", "11808"))
    bench_base = int(os.getenv("HUNTER_BENCH_BASE", "13808"))

    ssh_socks_host = os.getenv("HUNTER_SSH_LISTEN", "127.0.0.1")
    ssh_socks_port = int(os.getenv("HUNTER_SSH_SOCKS_PORT", "12808"))
    ssh_refresh = int(os.getenv("HUNTER_SSH_REFRESH", "60"))
    ssh_connect_timeout = int(os.getenv("HUNTER_SSH_CONNECT_TIMEOUT", "15"))
    ssh_servers = _load_ssh_servers_from_env()

    ssh_proxy = SSHStableSocksProxy(
        local_host=ssh_socks_host,
        local_port=ssh_socks_port,
        servers=ssh_servers,
        refresh_interval=ssh_refresh,
        connect_timeout=ssh_connect_timeout,
    )
    ssh_proxy_started = bool(ssh_proxy.start())

    state_file = _norm_file_path(os.getenv("HUNTER_STATE_FILE", "HUNTER_state.json"))
    raw_file = _norm_file_path(os.getenv("HUNTER_RAW_FILE", "HUNTER_raw.txt"))
    gold_file = _norm_file_path(os.getenv("HUNTER_GOLD_FILE", "HUNTER_gold.txt"))
    silver_file = _norm_file_path(os.getenv("HUNTER_SILVER_FILE", "HUNTER_silver.txt"))
    bridge_pool_file = _norm_file_path(os.getenv("HUNTER_BRIDGE_POOL_FILE", "HUNTER_bridge_pool.txt"))
    validated_jsonl = _norm_file_path(os.getenv("HUNTER_VALIDATED_JSONL", "HUNTER_validated.jsonl"))

    smart_cache = SmartCache(
        cache_file=str(base_dir / "subscriptions_cache.txt"),
        working_cache_file=str(base_dir / "working_configs_cache.txt"),
    )

    heartbeat = ResilientHeartbeat(check_interval=30)

    state = _load_json(state_file,
                       {"last_cleanup": 0, "cycle": 0, "last_seen": {}, "backoff": 30, "ever_online": False})
    bridge_manager = BridgeManager(xray_path=xray_path, socks_port_base=bridge_base, max_bridges=max_bridges)

    # Initialize MultiProxyServer with load balancing on single port 10808
    multi_proxy_port = int(os.getenv("HUNTER_MULTIPROXY_PORT", "10808"))
    multi_proxy_backends = int(os.getenv("HUNTER_MULTIPROXY_BACKENDS", "5"))
    multi_proxy_health_interval = int(os.getenv("HUNTER_MULTIPROXY_HEALTH_INTERVAL", "60"))
    multi_proxy_server = MultiProxyServer(
        xray_path=xray_path,
        port=multi_proxy_port,
        num_backends=multi_proxy_backends,
        health_check_interval=multi_proxy_health_interval
    )
    multi_proxy_started = False

    gemini_port = int(os.getenv("HUNTER_GEMINI_PORT", "10809"))
    gemini_backends = int(os.getenv("HUNTER_GEMINI_BACKENDS", "3"))
    gemini_test_url = os.getenv("HUNTER_GEMINI_TEST_URL", "https://gemini.google.com/")
    gemini_max_candidates = int(os.getenv("HUNTER_GEMINI_MAX_CANDIDATES", "25"))
    gemini_workers = int(os.getenv("HUNTER_GEMINI_WORKERS", "6"))
    gemini_bench_base = int(os.getenv("HUNTER_GEMINI_BENCH_BASE", str(bench_base + 5000)))
    gemini_file = _norm_file_path(os.getenv("HUNTER_GEMINI_APPROVED_FILE", "gemini_approved_configs.txt"))

    gemini_proxy_server = MultiProxyServer(
        xray_path=xray_path,
        port=gemini_port,
        num_backends=gemini_backends,
        health_check_interval=multi_proxy_health_interval,
        fallback_to_direct=False,
    )
    gemini_proxy_started = False

    bootstrap_ports = [1080, 1083, 10808, gemini_port, 2080, 7890, 8080]
    if ssh_proxy_started:
        bootstrap_ports.insert(0, ssh_socks_port)
    if _is_tor_running():
        bootstrap_ports.append(TOR_SOCKS_PORT)
        logger.info(f"Tor detected on port {TOR_SOCKS_PORT}, added to fallback list")

    startup_gemini = _read_lines(gemini_file)[:50]
    if startup_gemini:
        gemini_proxy_server.start([(u, 1000.0) for u in startup_gemini])
        gemini_proxy_started = True
        logger.info(f"[Gemini] Load balancer ready on port {gemini_port} (HTTP+SOCKS)")

    engines_available = []
    if _resolve_executable_path("xray", xray_path, [XRAY_PATH] + XRAY_PATH_FALLBACKS):
        engines_available.append("xray")
    if _resolve_executable_path("sing-box", SINGBOX_PATH, [SINGBOX_PATH] + SINGBOX_PATH_FALLBACKS):
        engines_available.append("sing-box")
    if _resolve_executable_path("mihomo", MIHOMO_PATH, [MIHOMO_PATH] + MIHOMO_PATH_FALLBACKS):
        engines_available.append("mihomo")
    logger.info(f"Available engines: {', '.join(engines_available) if engines_available else 'NONE!'}")

    # Start proxies immediately with last known configs
    startup_configs = []
    for config_file in [gold_file, silver_file, bridge_pool_file]:
        if os.path.exists(config_file):
            lines = _read_lines(config_file)
            for uri in lines[:20]:  # Take top 20 from each file
                if uri and uri not in [c[0] for c in startup_configs]:
                    startup_configs.append((uri, 1000))  # Default latency, will be updated

    if startup_configs:
        logger.info(f"[MultiProxy] Loading {len(startup_configs)} configs from previous session...")
        multi_proxy_server.start(startup_configs[:50])  # Start with top 50
        multi_proxy_started = True
        logger.info(f"[Balancer] Load balancer ready on port {multi_proxy_port} (HTTP+SOCKS)")

    # Start web server in background thread
    if _web_server_enabled:
        try:
            from proxy_web_server import run_web_server, app_state, proxy_server as web_proxy_server
            import proxy_web_server
            proxy_web_server.proxy_server = multi_proxy_server
            proxy_web_server.app_state["gold_count"] = len(_read_lines(gold_file))
            proxy_web_server.app_state["silver_count"] = len(_read_lines(silver_file))

            def start_web():
                run_web_server(host="0.0.0.0", port=_web_server_port)

            _web_server_thread = threading.Thread(target=start_web, daemon=True)
            _web_server_thread.start()
            logger.info(f"[WebServer] Started on http://0.0.0.0:{_web_server_port}")
        except Exception as e:
            logger.warning(f"[WebServer] Failed to start: {e}")

    while True:
        state["cycle"] = int(state.get("cycle", 0)) + 1
        cycle_id = state["cycle"]

        logger.info(f"\n{'=' * 80}")
        logger.info(f"CYCLE {cycle_id} STARTING")
        logger.info(f"{'=' * 80}")
        connect_tries = int(os.getenv("HUNTER_CONNECT_TRIES", "4"))
        sources = [
            ("bridge_pool", bridge_pool_file),
            ("gold", gold_file),
            ("silver", silver_file),
            ("mixed", ""),
        ]

        client: Optional[TelegramClient] = None
        telegram_available = False
        for attempt in range(max(1, connect_tries)):
            source_name, source_file = sources[min(attempt, len(sources) - 1)]
            try:
                bridge_manager.stop_all()
            except Exception:
                pass

            if source_name == "mixed":
                pool = _read_lines(bridge_pool_file)
                pool.extend(_read_lines(gold_file))
                pool.extend(_read_lines(silver_file))
                seen = set()
                bridge_candidates = []
                for u in pool:
                    if u and u not in seen:
                        seen.add(u)
                        bridge_candidates.append(u)
                bridge_candidates = bridge_candidates[: max_bridges * 3]
            else:
                bridge_candidates = _read_lines(source_file)[: max_bridges * 2]

            if bridge_candidates:
                logger.info(
                    f" Attempt {attempt + 1}/{connect_tries}: Using {source_name} ({len(bridge_candidates)} candidates)")
                bridge_manager.spawn_from_uris(bridge_candidates)
            else:
                logger.info(f"Attempt {attempt + 1}/{connect_tries}: Using {source_name} (no bridges)")

            proxy_ports = bootstrap_ports + bridge_manager.ports()
            client = await _try_connect_telegram(session_name, api_id, api_hash, phone, proxy_ports)
            if client:
                telegram_available = True
                break
            await asyncio.sleep(1)

        start_ts = _now_ts()
        harvested = set()
        connection_lost = False
        last_seen = state.get("last_seen")
        if not isinstance(last_seen, dict):
            last_seen = {}

        if client and telegram_available:
            state["backoff"] = 30
            state["ever_online"] = True
            try:
                async with client:
                    harvested, connection_lost = await _scrape_telegram_configs(
                        client,
                        targets,
                        scan_limit=scan_limit,
                        max_total=latest_total,
                        npvt_scan_limit=npvt_scan_limit,
                        last_seen=last_seen,
                        heartbeat=heartbeat,
                    )
            except asyncio.CancelledError:
                logger.warning("Telegram scraping cancelled - session sync issue, will reset session on next run")
                connection_lost = True
                # Mark session for reset by removing session file
                try:
                    session_file = session_name + ".session"
                    if os.path.exists(session_file):
                        os.remove(session_file)
                        logger.info(f"Removed stale session file: {session_file}")
                except Exception:
                    pass
            except Exception as e:
                logger.warning(f"Telegram scraping error: {e}")
                connection_lost = True
            state["last_seen"] = last_seen

            if connection_lost:
                logger.warning("Connection was lost during scraping, will use fallback sources")
                smart_cache.record_failure()
        else:
            logger.warning("Telegram not available, using fallback sources")
            smart_cache.record_failure()
            connection_lost = True

        logger.info(f"Fetching configs from GitHub repos (parallel + proxy fallback)...")
        use_tor = _is_tor_running()
        working_proxy_port = None

        # Collect all available proxy ports for fallback
        all_available_proxies = list(bridge_manager.ports()) if bridge_manager else []
        if use_tor:
            all_available_proxies.append(TOR_SOCKS_PORT)
        all_available_proxies.extend(bootstrap_ports)

        if bridge_manager.ports():
            working_proxy_port = bridge_manager.ports()[0]

        if use_tor:
            logger.info(f"Tor detected on port {TOR_SOCKS_PORT}, added to proxy fallback list")

        logger.info(f"Using {len(all_available_proxies)} proxy ports for fallback")

        github_configs = _fetch_github_configs(use_tor=use_tor, proxy_port=working_proxy_port,
                                               all_proxy_ports=all_available_proxies)
        if github_configs:
            logger.info(f"Got {len(github_configs)} configs from GitHub")
            harvested.update(github_configs)

        logger.info(f"Fetching NapsternetV configs (parallel)...")
        napsterv_configs = _fetch_napsterv_configs(use_tor=use_tor, proxy_port=working_proxy_port,
                                                   all_proxy_ports=all_available_proxies)
        if napsterv_configs:
            logger.info(f"Got {len(napsterv_configs)} configs from NapsternetV")
            harvested.update(napsterv_configs)

        anti_censor_configs = _fetch_anti_censorship_configs(all_available_proxies, max_workers=12)
        if anti_censor_configs:
            harvested.update(anti_censor_configs)

        if connection_lost or smart_cache.should_use_cache() or len(harvested) < 100:
            logger.info(f"Loading cached configs (failures: {smart_cache.get_failure_count()})...")
            cached_configs = smart_cache.load_cached_configs(max_count=2000)
            if cached_configs:
                logger.info(f"Loaded {len(cached_configs)} configs from cache")
                harvested.update(cached_configs)

            gold_cached = set(_read_lines(gold_file))
            silver_cached = set(_read_lines(silver_file))
            if gold_cached or silver_cached:
                logger.info(f"Adding {len(gold_cached)} gold + {len(silver_cached)} silver from previous runs")
                harvested.update(gold_cached)
                harvested.update(silver_cached)

        working_configs = set(_read_lines(gold_file)) | set(_read_lines(silver_file))
        if working_configs and len(harvested) < 500:
            tor_engine_configs = _fetch_with_tor_engine(
                xray_path=xray_path,
                tor_configs=list(working_configs)[:5],
                target_urls=GITHUB_REPOS[:5] + NAPSTERV_SUBSCRIPTION_URLS,
                socks_port=19050
            )
            if tor_engine_configs:
                logger.info(f"Tor engine fetched {len(tor_engine_configs)} additional configs")
                harvested.update(tor_engine_configs)

        if harvested:
            smart_cache.save_configs(harvested, working=False)

        harvested_count = _append_unique_lines(raw_file, list(harvested))

        logger.info(f"Total harvested: {len(harvested)} configs ({harvested_count} new)")

        validate_list = list(harvested)
        validate_list = validate_list[:max_total]

        if not validate_list:
            logger.warning("No configs to validate, trying to use cached working configs")
            validate_list = list(set(_read_lines(gold_file)) | set(_read_lines(silver_file)))[:max_total]

        if validate_list:
            logger.info(f"Validating {len(validate_list)} configs (Cloudflare + Google test)")
            validated = _validate_and_tier(
                validate_list,
                xray_path=xray_path,
                socks_base=bench_base,
                max_workers=max_workers,
                timeout_seconds=timeout_seconds,
                test_url=test_url,
                google_test_url=google_test_url,
            )
        else:
            logger.warning("No configs to validate at all")
            validated = []

        gold = [r for r in validated if r.tier == "gold"]
        silver = [r for r in validated if r.tier == "silver"]

        gold_added = _append_unique_lines(gold_file, [r.uri for r in gold])
        silver_added = _append_unique_lines(silver_file, [r.uri for r in silver])

        logger.info(f"Gold: {len(gold)} ({gold_added} new) |  Silver: {len(silver)} ({silver_added} new)")

        # Start or update MultiProxyServer with best configs
        if validated:
            # Prepare configs sorted by latency (best first)
            proxy_configs = [(r.uri, r.latency_ms) for r in validated]

            if not multi_proxy_started:
                # First time: start the server
                logger.info(f"[Balancer] Starting load balancer with {len(proxy_configs)} configs...")
                multi_proxy_server.start(proxy_configs)
                multi_proxy_started = True
                logger.info(f"[Balancer] Server running on 0.0.0.0:{multi_proxy_port} (HTTP+SOCKS)")
            else:
                # Update available configs for health check replacements
                multi_proxy_server.update_available_configs(proxy_configs)
                logger.info(f"[Balancer] Updated available configs pool with {len(proxy_configs)} configs")

            gemini_allowed = _select_gemini_allowed_configs(
                validated=validated,
                xray_path=xray_path,
                socks_base=gemini_bench_base,
                google_test_url=google_test_url,
                gemini_test_url=gemini_test_url,
                timeout_seconds=timeout_seconds,
                max_candidates=gemini_max_candidates,
                max_workers=gemini_workers,
            )

            if gemini_allowed:
                _write_lines(gemini_file, [u for u, _ in gemini_allowed])
                if not gemini_proxy_started:
                    gemini_proxy_server.start(gemini_allowed)
                    gemini_proxy_started = True
                    logger.info(f"[Gemini] Server running on 0.0.0.0:{gemini_port} (HTTP+SOCKS)")
                else:
                    gemini_proxy_server.update_available_configs(gemini_allowed)
                    logger.info(f"[Gemini] Updated pool with {len(gemini_allowed)} configs")

            # Update web server state
            if _web_server_enabled:
                try:
                    import proxy_web_server
                    proxy_web_server.app_state["gold_count"] = len(gold)
                    proxy_web_server.app_state["silver_count"] = len(silver)
                    proxy_web_server.app_state["last_scan"] = datetime.now().strftime("%H:%M:%S")
                    proxy_web_server.app_state["scan_status"] = "idle"
                except:
                    pass

        if validated:
            working_uris = {r.uri for r in validated}
            smart_cache.save_configs(working_uris, working=True)

        try:
            with open(validated_jsonl, "a", encoding="utf-8") as f:
                for r in validated:
                    f.write(
                        json.dumps(
                            {
                                "ts": start_ts,
                                "uri": r.uri,
                                "latency_ms": r.latency_ms,
                                "tier": r.tier,
                                "ip": r.ip,
                                "country_code": r.country_code,
                                "region": r.region,
                                "ps": r.ps,
                            },
                            ensure_ascii=False,
                        )
                        + "\n"
                    )
        except Exception:
            pass

        fast_for_recursive = gold if gold else validated
        top_n = max(1, int(len(fast_for_recursive) * recursive_ratio)) if fast_for_recursive else 0
        next_pool = [r.uri for r in fast_for_recursive[:top_n]]
        _write_lines(bridge_pool_file, next_pool)
        logger.info(f"Bridge pool updated: {len(next_pool)} configs for next cycle")

        all_cached = set(_read_lines(gold_file)) | set(_read_lines(silver_file))
        all_cached.discard("")
        if all_cached and cycle_id % 3 == 0:
            logger.info(f"Re-testing {min(len(all_cached), 100)} cached configs...")
            cached_list = list(all_cached)[:100]
            retest_results = _validate_and_tier(
                cached_list,
                xray_path=xray_path,
                socks_base=bench_base + 2000,
                max_workers=max_workers,
                timeout_seconds=timeout_seconds,
                test_url=test_url,
                google_test_url=google_test_url,
            )
            if retest_results:
                logger.info(f" {len(retest_results)} cached configs still working")
                for r in retest_results:
                    if r not in validated:
                        validated.append(r)
                validated.sort(key=lambda x: x.latency_ms)
            else:
                logger.warning("No cached configs passed retest")

        if validated:
            report_text = f" {len(validated)} Configs\n\n"
            for r in validated[:20]:
                report_text += f" {r.latency_ms:.0f}ms\n```\n{r.uri}\n```\n\n"

            if persistent_send_report:
                extra_ports = list(bridge_manager.ports()) if bridge_manager else []
                await persistent_send_report(api_id, api_hash, bot_token, report_channel, report_text, extra_ports)
            else:
                await _send_report(api_id, api_hash, bot_token, report_channel, report_text, proxy_ports=proxy_ports)
        else:
            logger.warning("No validated configs to report this cycle")

        now_ts = _now_ts()
        last_cleanup = int(state.get("last_cleanup", 0))
        if now_ts - last_cleanup >= cleanup_interval:
            state["last_cleanup"] = now_ts
            gold_set = set(_read_lines(gold_file))
            silver_set = set(_read_lines(silver_file))
            bridge_set = set(_read_lines(bridge_pool_file))
            gold_set.discard("")
            silver_set.discard("")
            bridge_set.discard("")
            _write_lines(gold_file, sorted(gold_set))
            _write_lines(silver_file, sorted(silver_set))
            _write_lines(bridge_pool_file, list(bridge_set)[: max_bridges * 5])
            logger.info("Cleanup completed")

        _save_json(state_file, state)
        # Note: MultiProxyServer keeps running between cycles for continuous service
        bridge_manager.stop_all()

        # Log Balancer status
        if multi_proxy_started:
            status = multi_proxy_server.get_status()
            logger.info(f"[Balancer] Status: {status.get('backends', 0)} backends on port {multi_proxy_port}")

        logger.info(f"Sleeping for {sleep_seconds}s before next cycle...\n")
        await asyncio.sleep(sleep_seconds)


if __name__ == "__main__":
    print(f"Python {sys.version} on {sys.platform}")
    asyncio.run(run_autonomous_hunter())