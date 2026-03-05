"""
Aggressive Config Harvester — mass-collects proxy configs every 30 minutes.

Designed for heavy Iranian censorship periods:
- Fetches from 100+ GitHub/subscription sources in parallel
- Uses ALL available local proxy ports for fetching
- Rotates through proxy ports with round-robin load balancing
- No per-source caps — collects everything available
- Targets 200,000+ configs per cycle
- Base64-aware decoding and URI extraction
- Deduplication via hash set
"""

import base64
import hashlib
import logging
import os
import random
import re
import socket
import subprocess
import sys
import time
from concurrent.futures import as_completed, Future
from typing import Dict, List, Optional, Set, Tuple

import requests
import urllib3
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

try:
    from hunter.core.task_manager import HunterTaskManager
    from hunter.core.utils import extract_raw_uris_from_text
except ImportError:
    from core.task_manager import HunterTaskManager
    from core.utils import extract_raw_uris_from_text

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
logging.getLogger("urllib3.util.retry").setLevel(logging.ERROR)
logging.getLogger("urllib3.connectionpool").setLevel(logging.ERROR)

logger = logging.getLogger(__name__)

# ─── All proxy ports to use for fetching (round-robin) ───────────────────────
ALL_PROXY_PORTS = [1080, 1081, 10808, 10809, 11808, 11809]

# ─── Browser fingerprints for stealth ────────────────────────────────────────
_USER_AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:125.0) Gecko/20100101 Firefox/125.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_4) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0.0.0 Safari/537.36 Edg/123.0.0.0",
]

# ─── MASSIVE source list — every known public V2Ray config aggregator ────────
HARVEST_SOURCES: List[Dict] = [
    # === GitHub aggregators (largest volume) ===
    {"url": "https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/Epodonios/v2ray-configs/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge_base64.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/coldwater-10/V2ray-Config-Lite/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/MatinGhanbari/v2ray-configs/main/subscriptions/v2ray/all_sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/M-Mashreghi/Free-V2ray-Collector/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/NiREvil/vless/main/subscription.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/skywrt/v2ray-configs/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/longlon/v2ray-config/main/All_Configs_Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/ebrasha/free-v2ray-public-list/main/all_extracted_configs.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/hamed1124/port-based-v2ray-configs/main/all.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/mostafasadeghifar/v2ray-config/main/configs.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/Ashkan-m/v2ray/main/Sub.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_iOS.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/AzadNetCH/Clash/main/AzadNet_STARTER.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/AzadNetCH/Clash/main/V2Ray.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/mfuu/v2ray/master/v2ray", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/peasoft/NoMoreWalls/master/list_raw.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/freefq/free/master/v2", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/aiboboxx/v2rayfree/main/v2", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/ermaozi/get_subscribe/main/subscribe/v2ray.txt", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub", "tag": "github"},
    {"url": "https://raw.githubusercontent.com/vveg26/get_proxy/main/dist/v2ray.txt", "tag": "github"},
    # === Telegram-scraped aggregators ===
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/mix", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/mix", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vmess", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/vless", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/trojan", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/reality", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/base64/reality", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/yebekhe/TelegramV2rayCollector/main/sub/normal/ss", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/configtg.txt", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/Surfboardv2ray/TGParse/main/reality.txt", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vless", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/trojan", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/vmess", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/reality", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/soroushmirzaei/telegram-configs-collector/main/protocols/shadowsocks", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/all.txt", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/reality.txt", "tag": "tg_agg"},
    {"url": "https://raw.githubusercontent.com/MrMohebi/xray-proxy-grabber-telegram/master/collected-proxies/row-url/vless.txt", "tag": "tg_agg"},
    # === Iran-priority / Reality-focused ===
    {"url": "https://raw.githubusercontent.com/mahdibland/SSAggregator/master/sub/sub_merge.txt", "tag": "iran"},
    {"url": "https://raw.githubusercontent.com/sarinaesmailzadeh/V2Hub/main/merged_base64", "tag": "iran"},
    {"url": "https://raw.githubusercontent.com/LalatinaHub/Starter/main/Starter", "tag": "iran"},
    {"url": "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/vless", "tag": "iran"},
    {"url": "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/ss", "tag": "iran"},
    {"url": "https://raw.githubusercontent.com/Leon406/SubCrawler/master/sub/share/all", "tag": "iran"},
    # === Extra large aggregators ===
    {"url": "https://raw.githubusercontent.com/roosterkid/openproxylist/main/V2RAY_RAW.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/ShatakVPN/ConfigForge-V2Ray/main/configs/ir/all.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/miladtahanian/V2RayCFGDumper/main/sub.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/a2470982985/getNode/main/v2ray.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/ts-sf/fly/main/v2", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/chengaopan/AutoMergePublicNodes/master/list.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/tbbatbb/Proxy/master/dist/v2ray.config.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/mlabalern1/v2ray-node/main/nodev2ray.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/w1770946466/Auto_proxy/main/Long_term_subscription_num", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/Bardiafa/Free-V2ray-Config/main/All_Configs_Sub.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/resplus/default/main/sub.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/sashalsk/V2Ray/main/V2Config", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/itsyebekhe/HiN-VPN/main/subscription/normal/mix", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/itsyebekhe/HiN-VPN/main/subscription/normal/vless", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/itsyebekhe/HiN-VPN/main/subscription/normal/reality", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/lagzian/SS-Collector/main/ss.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/lagzian/SS-Collector/main/reality.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/lagzian/SS-Collector/main/vmess.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/lagzian/SS-Collector/main/trojan.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/mheidari98/.github/main/all", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/TheDudeIsHere/PROXY/main/proxy_list.txt", "tag": "extra"},
    {"url": "https://raw.githubusercontent.com/1386gg/V2ray-Configs/main/configs/subs.txt", "tag": "extra"},
    # === Subscription services ===
    {"url": "https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/socks5/data.txt", "tag": "sub"},
    {"url": "https://raw.githubusercontent.com/proxifly/free-proxy-list/main/proxies/protocols/socks4/data.txt", "tag": "sub"},
]

# GitHub mirror prefixes for censored networks
_GITHUB_MIRRORS = [
    "",  # direct
    "https://ghproxy.com/",
    "https://ghproxy.net/",
    "https://mirror.ghproxy.com/",
]


class AggressiveHarvester:
    """Mass config harvester using all available proxy ports."""

    def __init__(self, extra_proxy_ports: Optional[List[int]] = None):
        self.logger = logging.getLogger("hunter.harvester")
        self._proxy_ports = list(ALL_PROXY_PORTS)
        if extra_proxy_ports:
            for p in extra_proxy_ports:
                if p not in self._proxy_ports:
                    self._proxy_ports.append(p)
        self._alive_ports: List[int] = []
        self._port_idx = 0  # round-robin counter
        self._direct_session: Optional[requests.Session] = None
        self._proxy_session: Optional[requests.Session] = None
        self._direct_works: Optional[bool] = None  # None=unknown
        self._stats: Dict[str, int] = {
            "total_fetched": 0,
            "sources_ok": 0,
            "sources_failed": 0,
            "last_harvest_ts": 0,
            "last_harvest_count": 0,
        }
        # Additional user-provided source URLs (from env)
        self._extra_sources: List[Dict] = self._load_extra_sources()

    # ── Session management ───────────────────────────────────────────────

    def _create_session(self, retries: int = 0) -> requests.Session:
        session = requests.Session()
        retry = Retry(total=retries, backoff_factor=0.3, status_forcelist=[500, 502, 503, 504])
        adapter = HTTPAdapter(pool_connections=20, pool_maxsize=40, max_retries=retry)
        session.mount("http://", adapter)
        session.mount("https://", adapter)
        return session

    def _get_direct_session(self) -> requests.Session:
        if self._direct_session is None:
            self._direct_session = self._create_session(retries=0)
        return self._direct_session

    def _get_proxy_session(self) -> requests.Session:
        if self._proxy_session is None:
            self._proxy_session = self._create_session(retries=1)
        return self._proxy_session

    def reset_session(self):
        for s in (self._direct_session, self._proxy_session):
            if s:
                try:
                    s.close()
                except Exception:
                    pass
        self._direct_session = None
        self._proxy_session = None

    # ── Proxy port management ────────────────────────────────────────────

    def _probe_alive_ports(self) -> List[int]:
        """Find which proxy ports are accepting connections."""
        alive = []
        for port in self._proxy_ports:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(1.0)
                result = s.connect_ex(("127.0.0.1", port))
                s.close()
                if result == 0:
                    alive.append(port)
            except Exception:
                pass
        return alive

    def _next_proxy_port(self) -> Optional[int]:
        """Round-robin through alive proxy ports."""
        if not self._alive_ports:
            return None
        port = self._alive_ports[self._port_idx % len(self._alive_ports)]
        self._port_idx += 1
        return port

    def _get_proxies_for_port(self, port: int) -> Dict[str, str]:
        return {
            "http": f"socks5h://127.0.0.1:{port}",
            "https": f"socks5h://127.0.0.1:{port}",
        }

    # ── Extra sources from environment ───────────────────────────────────

    def _load_extra_sources(self) -> List[Dict]:
        """Load additional source URLs from HUNTER_EXTRA_HARVEST_URLS env var."""
        raw = os.getenv("HUNTER_EXTRA_HARVEST_URLS", "")
        sources = []
        if raw:
            for url in raw.split(","):
                url = url.strip()
                if url and url.startswith("http"):
                    sources.append({"url": url, "tag": "env_extra"})
        return sources

    # ── Direct access detection ──────────────────────────────────────────

    def _check_direct_access(self) -> bool:
        """Quick probe: can we reach raw.githubusercontent.com directly?"""
        if self._direct_works is not None:
            return self._direct_works
        session = self._get_direct_session()
        try:
            resp = session.head(
                "https://raw.githubusercontent.com",
                timeout=4, verify=False,
                headers={"User-Agent": random.choice(_USER_AGENTS)},
            )
            self._direct_works = resp.status_code < 500
        except Exception:
            self._direct_works = False
        if not self._direct_works:
            self.logger.info("[Harvester] Direct GitHub access blocked — proxy-first mode")
        else:
            self.logger.info("[Harvester] Direct GitHub access available")
        return self._direct_works

    # ── GitHub mirror expansion ──────────────────────────────────────────

    def _expand_mirrors(self, url: str) -> List[str]:
        # Skip mirrors entirely when direct is blocked (they're blocked too)
        if self._direct_works is False:
            return [url]
        if "raw.githubusercontent.com" not in url:
            return [url]
        path = url.replace("https://", "")
        candidates = [url]
        for mirror in _GITHUB_MIRRORS[1:]:
            candidates.append(f"{mirror}{path}")
        return candidates

    # ── Core fetch logic ─────────────────────────────────────────────────

    def _try_decode_base64(self, text: str) -> str:
        """If text looks like base64, decode it."""
        stripped = text.strip()
        if not stripped:
            return text
        # Heuristic: if no :// in first 500 chars but has base64 chars
        sample = stripped[:500]
        if "://" not in sample and re.match(r'^[A-Za-z0-9+/=\s]+$', sample):
            try:
                # Add padding if needed
                padded = stripped + "=" * (4 - len(stripped) % 4)
                decoded = base64.b64decode(padded).decode("utf-8", errors="ignore")
                if "://" in decoded:
                    return decoded
            except Exception:
                pass
        return text

    def _fetch_one_source(self, source: Dict) -> Tuple[str, Set[str], float]:
        """Fetch a single source URL. Returns (url, configs, elapsed_seconds).
        
        Strategy (proxy-first when direct is blocked):
        1. If direct works: try direct first, then proxy fallback
        2. If direct blocked: go straight to proxy (round-robin port)
        3. Last resort: curl with proxy
        """
        url = source["url"]
        ua = random.choice(_USER_AGENTS)
        headers = {"User-Agent": ua, "Accept": "*/*"}
        t0 = time.time()

        # --- Strategy A: Proxy-first (when direct is blocked) ---
        if self._direct_works is False:
            port = self._next_proxy_port()
            if port:
                proxy_session = self._get_proxy_session()
                proxies = self._get_proxies_for_port(port)
                try:
                    resp = proxy_session.get(
                        url, headers=headers, timeout=12,
                        verify=False, proxies=proxies,
                    )
                    if resp.status_code == 200 and resp.text and len(resp.text) > 20:
                        text = self._try_decode_base64(resp.text)
                        found = extract_raw_uris_from_text(text)
                        if found:
                            return (url, found, time.time() - t0)
                except Exception:
                    pass

            # curl fallback with proxy
            port2 = self._next_proxy_port()
            if port2:
                curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
                try:
                    cmd = [
                        curl_cmd, "-s", "-m", "15", "-k",
                        "--connect-timeout", "5",
                        "-A", ua,
                        "--socks5-hostname", f"127.0.0.1:{port2}",
                        url,
                    ]
                    result = subprocess.run(cmd, capture_output=True, timeout=18)
                    if result.returncode == 0 and result.stdout:
                        text = result.stdout.decode("utf-8", errors="ignore")
                        text = self._try_decode_base64(text)
                        found = extract_raw_uris_from_text(text)
                        if found:
                            return (url, found, time.time() - t0)
                except Exception:
                    pass
            return (url, set(), time.time() - t0)

        # --- Strategy B: Direct-first (when direct works) ---
        direct_session = self._get_direct_session()
        try:
            resp = direct_session.get(url, headers=headers, timeout=8, verify=False)
            if resp.status_code == 200 and resp.text and len(resp.text) > 20:
                text = self._try_decode_base64(resp.text)
                found = extract_raw_uris_from_text(text)
                if found:
                    return (url, found, time.time() - t0)
        except Exception:
            pass

        # Direct failed, try proxy
        port = self._next_proxy_port()
        if port:
            proxy_session = self._get_proxy_session()
            proxies = self._get_proxies_for_port(port)
            try:
                resp = proxy_session.get(
                    url, headers=headers, timeout=12,
                    verify=False, proxies=proxies,
                )
                if resp.status_code == 200 and resp.text and len(resp.text) > 20:
                    text = self._try_decode_base64(resp.text)
                    found = extract_raw_uris_from_text(text)
                    if found:
                        return (url, found, time.time() - t0)
            except Exception:
                pass

        return (url, set(), time.time() - t0)

    # ── Main harvest entry point ─────────────────────────────────────────

    def harvest(self, timeout_seconds: float = 300.0) -> Set[str]:
        """
        Run a full aggressive harvest cycle.
        
        Fetches from ALL sources in parallel using all proxy ports.
        Returns the set of all discovered config URIs (deduplicated).
        """
        self.logger.info("=" * 60)
        self.logger.info("[Harvester] Starting aggressive harvest cycle")
        t0 = time.time()

        # Probe alive proxy ports
        self._alive_ports = self._probe_alive_ports()
        self.logger.info(
            f"[Harvester] Alive proxy ports: {self._alive_ports} "
            f"(of {len(self._proxy_ports)} configured)"
        )

        # Check if direct GitHub access works
        self._check_direct_access()

        # Build source list
        all_sources = list(HARVEST_SOURCES) + self._extra_sources
        random.shuffle(all_sources)  # Randomize to spread load
        self.logger.info(f"[Harvester] Fetching from {len(all_sources)} sources...")

        # Parallel fetch using shared IO pool
        all_configs: Set[str] = set()
        mgr = HunterTaskManager.get_instance()
        ok_count = 0
        fail_count = 0

        futures: Dict[Future, Dict] = {}
        for src in all_sources:
            fut = mgr.submit_io(self._fetch_one_source, src)
            futures[fut] = src

        try:
            for future in as_completed(futures, timeout=timeout_seconds):
                try:
                    url, found, elapsed = future.result(timeout=2)
                    if found:
                        all_configs.update(found)
                        ok_count += 1
                        tag = futures[future].get("tag", "?")
                        self.logger.debug(
                            f"[Harvester] +{len(found)} from {tag} "
                            f"({elapsed:.1f}s) — total: {len(all_configs)}"
                        )
                    else:
                        fail_count += 1
                except Exception:
                    fail_count += 1
        except TimeoutError:
            self.logger.warning(
                f"[Harvester] Overall timeout ({timeout_seconds}s), "
                f"collected {len(all_configs)} so far"
            )
            for f in futures:
                f.cancel()

        elapsed_total = time.time() - t0
        self._stats.update({
            "total_fetched": self._stats["total_fetched"] + len(all_configs),
            "sources_ok": ok_count,
            "sources_failed": fail_count,
            "last_harvest_ts": time.time(),
            "last_harvest_count": len(all_configs),
        })

        self.logger.info(
            f"[Harvester] Cycle complete: {len(all_configs)} unique configs "
            f"from {ok_count}/{ok_count + fail_count} sources in {elapsed_total:.1f}s"
        )
        self.logger.info("=" * 60)

        return all_configs

    def get_stats(self) -> Dict[str, int]:
        return dict(self._stats)
