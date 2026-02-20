"""
Core utilities and helper functions.

This module contains common utility functions used throughout
the Hunter system.
"""

import base64
import json
import logging
import os
import random
import re
import socket
import struct
import time
from typing import Any, Dict, List, Optional, Set

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry


# Global HTTP session for connection pooling
_HTTP_SESSION: Optional[requests.Session] = None

BROWSER_USER_AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:122.0) Gecko/20100101 Firefox/122.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15",
]

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
    # 2026 Iran DPI evasion (ordered by effectiveness)
    "reality", "pbk=",
    "flow=xtls-rprx-vision",  # XTLS-Vision anti TLS-in-TLS
    "splithttp", "xhttp",    # SplitHTTP v26 transport
    "grpc", "gun",
    "h2", "http/2",
    "ws", "websocket",
    "httpupgrade",
    "quic", "kcp",
    "fp=chrome", "fp=firefox", "fp=safari", "fp=edge", "fp=ios", "fp=android",
    "fp=random", "fp=randomized",
    "alpn=h2", "alpn=http",
]

DPI_EVASION_FINGERPRINTS = [
    "chrome", "firefox", "safari", "edge", "ios", "android", "random", "randomized",
    "chrome_random", "ios_random", "firefox_random",
]

# 2026 protocol indicators (Hysteria2, TUIC, SplitHTTP)
UDP_PROTOCOL_INDICATORS = [
    "hysteria2://", "hy2://", "tuic://",
]

VISION_FLOW_INDICATOR = "flow=xtls-rprx-vision"
SPLITHTTP_INDICATORS = ["splithttp", "xhttp", "type=splithttp"]

IRAN_BLOCKED_PATTERNS = [
    "ir.", ".ir", "iran",
    "0.0.0.0", "127.0.0.1", "localhost",
    "10.10.34.", "192.168.",
]


def create_session_with_pool(pool_connections: int = 20, pool_maxsize: int = 50, retries: int = 2) -> requests.Session:
    """Create a requests session with connection pooling for faster HTTP requests."""
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


def get_http_session() -> requests.Session:
    """Get or create the global HTTP session with connection pooling."""
    global _HTTP_SESSION
    if _HTTP_SESSION is None:
        _HTTP_SESSION = create_session_with_pool()
    return _HTTP_SESSION


def random_user_agent() -> str:
    """Get a random browser user agent."""
    return random.choice(BROWSER_USER_AGENTS)


def safe_b64decode(data: str) -> bytes:
    """Safely decode base64 data with proper padding."""
    padded = data + "=" * (-len(data) % 4)
    return base64.b64decode(padded)


def clean_ps_string(ps: str) -> str:
    """Clean proxy name string by removing non-ASCII characters."""
    import re
    return re.sub(r'[^\x00-\x7F]+', '', ps).strip() or "Unknown"


def now_ts() -> int:
    """Get current timestamp as integer."""
    return int(time.time())


def tier_for_latency(latency_ms: float) -> str:
    """Determine performance tier based on latency."""
    if latency_ms < 200.0:
        return "gold"
    if latency_ms < 800.0:
        return "silver"
    if latency_ms > 2000.0:
        return "dead"
    return "silver"


def resolve_ip(host: str) -> Optional[str]:
    """Resolve hostname to IP address."""
    try:
        return socket.gethostbyname(host)
    except Exception:
        return None


_country_code_cache: Dict[str, Optional[str]] = {}

def get_country_code(ip: Optional[str]) -> Optional[str]:
    """Get country code for IP address (cached to avoid API hammering)."""
    if not ip or ip == "0.0.0.0":
        return None
    if ip in _country_code_cache:
        return _country_code_cache[ip]
    try:
        import requests
        resp = requests.get(f"https://ipapi.co/{ip}/country_code/", timeout=3)
        if resp.status_code >= 400:
            _country_code_cache[ip] = None
            return None
        cc = resp.text.strip() or None
        _country_code_cache[ip] = cc
        return cc
    except Exception:
        _country_code_cache[ip] = None
        return None


def get_region(country_code: Optional[str]) -> str:
    """Get region from country code."""
    european_codes = [
        'AL', 'AD', 'AT', 'BY', 'BE', 'BA', 'BG', 'HR', 'CY', 'CZ', 'DK', 'EE', 
        'FO', 'FI', 'FR', 'DE', 'GI', 'GR', 'HU', 'IS', 'IE', 'IT', 'XK', 'LV', 
        'LI', 'LT', 'LU', 'MK', 'MT', 'MD', 'MC', 'ME', 'NL', 'NO', 'PL', 'PT', 
        'RO', 'RU', 'SM', 'RS', 'SK', 'SI', 'ES', 'SE', 'CH', 'UA', 'GB', 'VA'
    ]
    asian_codes = [
        'AF', 'AM', 'AZ', 'BH', 'BD', 'BT', 'BN', 'KH', 'CN', 'GE', 'HK', 'IN', 
        'ID', 'IR', 'IQ', 'IL', 'JP', 'JO', 'KZ', 'KW', 'KG', 'LA', 'LB', 'MO', 
        'MY', 'MV', 'MN', 'MM', 'NP', 'KP', 'OM', 'PK', 'PS', 'PH', 'QA', 'SA', 
        'SG', 'KR', 'LK', 'SY', 'TW', 'TJ', 'TH', 'TL', 'TR', 'TM', 'AE', 'UZ', 
        'VN', 'YE'
    ]
    african_codes = [
        'DZ', 'AO', 'BJ', 'BW', 'BF', 'BI', 'CV', 'CM', 'CF', 'TD', 'KM', 'CD', 
        'CG', 'DJ', 'EG', 'GQ', 'ER', 'SZ', 'ET', 'GA', 'GM', 'GH', 'GN', 'GW', 
        'CI', 'KE', 'LS', 'LR', 'LY', 'MG', 'MW', 'ML', 'MR', 'MU', 'YT', 'MA', 
        'MZ', 'NA', 'NE', 'NG', 'RE', 'RW', 'SH', 'ST', 'SN', 'SC', 'SL', 'SO', 
        'ZA', 'SS', 'SD', 'TZ', 'TG', 'TN', 'UG', 'EH', 'ZM', 'ZW'
    ]
    
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


def resolve_executable_path(name: str, primary: Optional[str], fallbacks: List[str]) -> Optional[str]:
    """Resolve executable path with fallbacks."""
    import shutil
    
    candidates: List[str] = []
    if primary:
        candidates.append(primary)
    candidates.extend(fallbacks)
    
    seen = set()
    for path in candidates:
        if not path:
            continue
        
        try:
            normalized = os.path.expandvars(os.path.expanduser(path))
        except Exception:
            normalized = path
        
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
                resolved = shutil.which(normalized)
                if resolved and os.path.exists(resolved):
                    return resolved
            except Exception:
                pass
    
    return None


def read_lines(path: str) -> List[str]:
    """Read lines from file, stripping whitespace."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return [line.strip() for line in f if line.strip()]
    except Exception:
        return []


def write_lines(filepath: str, lines: List[str]) -> int:
    """Write lines to file."""
    try:
        with open(filepath, "w", encoding="utf-8") as f:
            for line in lines:
                if line:
                    f.write(line + "\n")
        return len(lines)
    except Exception:
        return 0


def is_cdn_based(uri: str) -> bool:
    """Check if config uses CDN/whitelisted domain (Domain Fronting)."""
    uri_lower = uri.lower()
    for domain in CDN_WHITELIST_DOMAINS:
        if domain.lower() in uri_lower:
            return True
    return False


def has_anti_dpi_features(uri: str) -> int:
    """Score config based on anti-DPI features (higher = better)."""
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
    if is_cdn_based(uri):
        score += 3
    
    return score


def is_likely_blocked(uri: str) -> bool:
    """Check if config is likely blocked by Iran's filtering infrastructure."""
    uri_lower = uri.lower()
    for pattern in IRAN_BLOCKED_PATTERNS:
        if pattern in uri_lower:
            return True
    return False


def is_ipv4_preferred(uri: str) -> bool:
    """Check if config uses IPv4 (preferred over IPv6 due to Iran's IPv6 filtering)."""
    if "[" in uri and "]" in uri:
        return False
    return True


def prioritize_configs(uris: List[str]) -> List[str]:
    """"Prioritize configs for testing - most resilient against Iran's 2026 filtering.
    
    Priority based on Iran's 2026 TSG/Whitelist filtering architecture:
    1. VLESS Reality+Vision + CDN (best: invisible to DPI, borrows whitelisted TLS identity)
    2. VLESS Reality+Vision (excellent: mimics real sites, Vision hides TLS-in-TLS)
    3. VLESS Reality (no Vision) (very good: still borrows TLS identity)
    4. Hysteria2/TUIC (excellent for mobile: overcomes throttling via UDP)
    5. VLESS/Trojan + SplitHTTP/XHTTP (v26: mimics web browsing in blackout)
    6. VLESS/Trojan + gRPC/H2 + TLS 443 (good: looks like HTTP/2)
    7. VLESS/Trojan + WebSocket + TLS 443 + CDN (good: CDN fronting)
    8. VLESS/Trojan + WebSocket + TLS 443 (decent: looks like HTTPS)
    9. VMess + WebSocket + TLS + CDN (legacy but usable with CDN)
    10. Other TLS-based on port 443
    11. IPv6 (deprioritized: ~98.5% IPv6 suppression in Iran)
    12. Everything else (likely blocked)
    
    New in 2026: Vision flow, SplitHTTP, Hysteria2/TUIC, fingerprint awareness.
    """
    # Filter out likely-blocked configs first
    filtered_uris = [uri for uri in uris if not is_likely_blocked(uri)]
    
    # Priority buckets (12 tiers for 2026 architecture)
    tier_01_reality_vision_cdn = []
    tier_02_reality_vision = []
    tier_03_reality_no_vision = []
    tier_04_udp_protocols = []      # Hysteria2, TUIC
    tier_05_splithttp = []          # SplitHTTP/XHTTP v26
    tier_06_grpc_h2 = []
    tier_07_ws_tls_cdn = []         # WebSocket + CDN
    tier_08_ws_tls_443 = []
    tier_09_vmess_ws_cdn = []
    tier_10_tls_443 = []
    tier_11_ipv6 = []
    tier_12_other = []
    
    for uri in filtered_uris:
        uri_lower = uri.lower()
        is_ipv4 = is_ipv4_preferred(uri)
        is_cdn = is_cdn_based(uri)
        
        # Deprioritize IPv6 due to Iran's IPv6 suppression
        if not is_ipv4:
            tier_11_ipv6.append(uri)
            continue
        
        # Hysteria2 / TUIC (UDP-based protocols - great for mobile)
        if any(uri_lower.startswith(p) for p in ("hysteria2://", "hy2://", "tuic://")):
            tier_04_udp_protocols.append(uri)
            continue
        
        # VLESS configs
        if uri_lower.startswith("vless://"):
            is_reality = "reality" in uri_lower or "pbk=" in uri_lower
            has_vision = VISION_FLOW_INDICATOR in uri_lower
            is_splithttp = any(ind in uri_lower for ind in SPLITHTTP_INDICATORS)
            is_grpc = "grpc" in uri_lower or "gun" in uri_lower
            is_h2 = "h2" in uri_lower or "http/2" in uri_lower
            is_ws = "ws" in uri_lower or "websocket" in uri_lower
            is_tls_443 = ":443" in uri and ("security=tls" in uri_lower or "tls" in uri_lower)
            has_fingerprint = "fp=" in uri_lower
            
            if is_reality and has_vision and is_cdn:
                tier_01_reality_vision_cdn.append(uri)
            elif is_reality and has_vision:
                tier_02_reality_vision.append(uri)
            elif is_reality:
                tier_03_reality_no_vision.append(uri)
            elif is_splithttp:
                tier_05_splithttp.append(uri)
            elif is_grpc or is_h2:
                tier_06_grpc_h2.append(uri)
            elif is_ws and is_cdn and is_tls_443:
                tier_07_ws_tls_cdn.append(uri)
            elif is_ws and is_tls_443:
                tier_08_ws_tls_443.append(uri)
            elif is_tls_443:
                tier_10_tls_443.append(uri)
            else:
                tier_12_other.append(uri)
        
        # Trojan configs
        elif uri_lower.startswith("trojan://"):
            is_grpc = "grpc" in uri_lower or "gun" in uri_lower
            is_splithttp = any(ind in uri_lower for ind in SPLITHTTP_INDICATORS)
            is_ws = "ws" in uri_lower or "websocket" in uri_lower
            is_443 = ":443" in uri
            
            if is_splithttp:
                tier_05_splithttp.append(uri)
            elif is_grpc:
                tier_06_grpc_h2.append(uri)
            elif is_ws and is_cdn and is_443:
                tier_07_ws_tls_cdn.append(uri)
            elif is_ws and is_443:
                tier_08_ws_tls_443.append(uri)
            elif is_443:
                tier_10_tls_443.append(uri)
            else:
                tier_12_other.append(uri)
        
        # VMess configs
        elif uri_lower.startswith("vmess://"):
            try:
                payload = uri[len("vmess://"):]
                decoded = safe_b64decode(payload).decode("utf-8", errors="ignore")
                is_ws = '"net":"ws"' in decoded
                is_tls = '"tls":"tls"' in decoded
                is_grpc = '"net":"grpc"' in decoded or '"net":"gun"' in decoded
                port_443 = '"port":"443"' in decoded or '"port":443' in decoded
                
                if is_grpc and is_tls:
                    tier_06_grpc_h2.append(uri)
                elif is_ws and is_tls and is_cdn:
                    tier_09_vmess_ws_cdn.append(uri)
                elif is_ws and is_tls and port_443:
                    tier_08_ws_tls_443.append(uri)
                elif is_tls and port_443:
                    tier_10_tls_443.append(uri)
                else:
                    tier_12_other.append(uri)
            except:
                tier_12_other.append(uri)
        else:
            tier_12_other.append(uri)
    
    # Shuffle within each tier for load balancing
    all_tiers = [
        tier_01_reality_vision_cdn, tier_02_reality_vision, tier_03_reality_no_vision,
        tier_04_udp_protocols, tier_05_splithttp, tier_06_grpc_h2,
        tier_07_ws_tls_cdn, tier_08_ws_tls_443, tier_09_vmess_ws_cdn,
        tier_10_tls_443, tier_11_ipv6, tier_12_other,
    ]
    for tier in all_tiers:
        random.shuffle(tier)
    
    result = []
    for tier in all_tiers:
        result.extend(tier)
    return result


def append_unique_lines(path: str, lines: List[str]) -> int:
    """Append unique lines to file."""
    existing = set(read_lines(path))
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


def load_json(path: str, default: Dict[str, Any]) -> Dict[str, Any]:
    """Load JSON file with default fallback."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
            if isinstance(data, dict):
                return data
            return default
    except Exception:
        return default


def save_json(path: str, data: Dict[str, Any]) -> None:
    """Save data to JSON file."""
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


def extract_raw_uris_from_text(text: str) -> Set[str]:
    """Extract proxy URIs from text."""
    if not text:
        return set()
    
    import re
    uris = set()
    for m in re.finditer(r"(?i)(vmess|vless|trojan|ss|shadowsocks|hysteria2|hy2|tuic)://[^\s\"'<>\[\]]+", text):
        uri = m.group(0).strip().rstrip(")],.;:!?")
        if len(uri) > 10:
            uris.add(uri)
    return uris


def extract_raw_uris_from_bytes(data: bytes) -> Set[str]:
    """Extract proxy URIs from bytes data."""
    if not data:
        return set()
    
    text = data.decode("utf-8", errors="ignore")
    uris = extract_raw_uris_from_text(text)
    if uris:
        return uris
    
    # Try to find and decode base64 content
    import re
    for candidate in re.findall(r"[A-Za-z0-9+/=]{100,}", text)[:20]:
        try:
            decoded = safe_b64decode(candidate).decode("utf-8", errors="ignore")
            extracted = extract_raw_uris_from_text(decoded)
            if extracted:
                uris.update(extracted)
        except Exception:
            continue
    
    return uris


def kill_process_on_port(port: int) -> bool:
    """Kill any process using the specified port."""
    import subprocess
    import sys
    
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
                            logging.getLogger(__name__).info(f"Killed process {pid} on port {port}")
                            time.sleep(0.5)
                            return True
                        except:
                            pass
        else:
            # Linux/Mac - use lsof
            result = subprocess.run(
                ["lsof", "-ti", f":{port}"],
                capture_output=True, text=True, timeout=5
            )
            if result.stdout.strip():
                for pid in result.stdout.strip().split("\n"):
                    try:
                        os.kill(int(pid), 9)
                        logging.getLogger(__name__).info(f"Killed process {pid} on port {port}")
                        time.sleep(0.5)
                        return True
                    except:
                        pass
    except:
        pass
    return False


def get_local_ip() -> str:
    """Get local IP address."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"


def setup_logging():
    """Setup colored logging for the application."""
    import sys
    from colorama import Fore, Style, init
    
    init(autoreset=True)
    
    if sys.platform == "win32":
        import codecs
        sys.stdout.reconfigure(encoding='utf-8')
        sys.stderr.reconfigure(encoding='utf-8')
    
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
    
    # Reduce noise from external libraries
    logging.getLogger("urllib3.connectionpool").setLevel(logging.ERROR)
    logging.getLogger("telethon").setLevel(logging.WARNING)
    logging.getLogger("telethon.network").setLevel(logging.WARNING)
    logging.getLogger("telethon.client.downloads").setLevel(logging.WARNING)
