"""
Core utilities and helper functions.

This module contains common utility functions used throughout
the Hunter system.
"""

import base64
import json
import logging
import os
import re
import socket
import struct
import time
from typing import Any, Dict, List, Optional, Set


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


def get_country_code(ip: Optional[str]) -> Optional[str]:
    """Get country code for IP address."""
    if not ip or ip == "0.0.0.0":
        return None
    try:
        import requests
        resp = requests.get(f"https://ipapi.co/{ip}/country_code/", timeout=5)
        if resp.status_code >= 400:
            return None
        return resp.text.strip() or None
    except Exception:
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


def write_lines(path: str, lines: List[str]) -> None:
    """Write lines to file."""
    try:
        with open(path, "w", encoding="utf-8") as f:
            for line in lines:
                if line:
                    f.write(line + "\n")
    except Exception:
        pass


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
    for m in re.finditer(r"(?i)(vmess|vless|trojan|ss|shadowsocks)://[^\s\"'<>\[\]]+", text):
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
