"""
Hunter orchestrator - Main autonomous hunting workflow.

This module coordinates the full hunter workflow including
scraping, benchmarking, caching, and load balancing.
"""

import asyncio
import hashlib
import logging
import os
import sys
import threading
import time
from concurrent.futures import as_completed

try:
    from hunter.core.task_manager import HunterTaskManager
except ImportError:
    from core.task_manager import HunterTaskManager
from queue import SimpleQueue
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from tqdm import tqdm

ProxyBenchmark = None
HunterConfig = None
HunterBenchResult = None
HTTPClientManager = None
ConfigFetcher = None
FlexibleConfigFetcher = None
FetchStrategy = None
UniversalParser = None
MultiProxyServer = None
TelegramScraper = None
TelegramReporter = None
BotReporter = None
SmartCache = None
StealthObfuscationEngine = None
ObfuscationConfig = None
ProxyStealthWrapper = None
DPIEvasionOrchestrator = None
load_json = None
now_ts = None
read_lines = None
save_json = None
write_lines = None
ConfigReportingService = None
DNSManager = None
get_dns_config = None
HunterUI = None
LiveStatus = None
ServiceManager = None
HunterDashboard = None

try:
    from hunter.core.config import HunterConfig
    from hunter.core.models import HunterBenchResult
    from hunter.network.http_client import HTTPClientManager, ConfigFetcher
    from hunter.network.flexible_fetcher import FlexibleConfigFetcher, FetchStrategy
    from hunter.parsers import UniversalParser
    from hunter.testing.benchmark import ProxyBenchmark
    from hunter.proxy.load_balancer import MultiProxyServer
    from hunter.telegram.scraper import TelegramScraper, TelegramReporter, BotReporter
    from hunter.config.cache import SmartCache
    from hunter.security.stealth_obfuscation import StealthObfuscationEngine, ObfuscationConfig, ProxyStealthWrapper
    from hunter.security.dpi_evasion_orchestrator import DPIEvasionOrchestrator
    from hunter.core.utils import load_json, now_ts, read_lines, save_json, write_lines
    from hunter.telegram_config_reporter import ConfigReportingService
    from hunter.config.dns_servers import DNSManager, get_dns_config
    from hunter.ui.progress import HunterUI, LiveStatus, ServiceManager
    from hunter.ui.dashboard import HunterDashboard
except ImportError:
    # Fallback for direct execution
    from core.config import HunterConfig
    from core.models import HunterBenchResult
    from network.http_client import HTTPClientManager, ConfigFetcher
    from network.flexible_fetcher import FlexibleConfigFetcher, FetchStrategy
    from parsers import UniversalParser
    try:
        from testing.benchmark import ProxyBenchmark
    except Exception:
        ProxyBenchmark = None
    from proxy.load_balancer import MultiProxyServer
    from telegram.scraper import TelegramScraper, TelegramReporter, BotReporter
    from config.cache import SmartCache
    from security.stealth_obfuscation import StealthObfuscationEngine, ObfuscationConfig, ProxyStealthWrapper
    from security.dpi_evasion_orchestrator import DPIEvasionOrchestrator
    from core.utils import load_json, now_ts, read_lines, save_json, write_lines
    from telegram_config_reporter import ConfigReportingService
    from ui.progress import HunterUI, LiveStatus, ServiceManager
    from ui.dashboard import HunterDashboard
    try:
        from config.dns_servers import DNSManager, get_dns_config
    except ImportError:
        DNSManager = None
        get_dns_config = None


class HunterOrchestrator:
    """Main orchestrator for the autonomous hunter workflow."""

    def __init__(self, config: HunterConfig):
        self.config = config
        self.logger = logging.getLogger(__name__)

        # Initialize all components with robust error handling
        # Every component is optional - if it fails, we continue without it
        
        # HTTP client
        self.http_manager = None
        self.config_fetcher = None
        if HTTPClientManager is not None:
            try:
                self.http_manager = HTTPClientManager()
                self.config_fetcher = ConfigFetcher(self.http_manager)
            except Exception as e:
                self.logger.info(f"HTTP client init skipped: {e}")
        
        # Parser
        self.parser = None
        if UniversalParser is not None:
            try:
                self.parser = UniversalParser()
            except Exception as e:
                self.logger.info(f"Parser init skipped: {e}")
        
        # Benchmarker
        self.benchmarker = None
        if ProxyBenchmark is not None:
            try:
                self.benchmarker = ProxyBenchmark(config.get("iran_fragment_enabled", False))
            except Exception as e:
                self.logger.info(f"Benchmarker init skipped: {e}")
        
        # Cache
        self.cache = None
        if SmartCache is not None:
            try:
                self.cache = SmartCache()
            except Exception as e:
                self.logger.info(f"Cache init skipped: {e}")
        
        # Telegram (optional - works without credentials)
        self.telegram_scraper = None
        self.telegram_reporter = None
        self.bot_reporter = None
        self.config_reporting_service = None
        
        # Bot API reporter (primary - uses TOKEN + CHAT_ID via HTTP, no SSH needed)
        if BotReporter is not None:
            try:
                self.bot_reporter = BotReporter()
                if self.bot_reporter.enabled:
                    self.logger.info("Bot reporter initialized (Bot API)")
                else:
                    self.bot_reporter = None
            except Exception as e:
                self.logger.info(f"Bot reporter init skipped: {e}")
        
        # Telethon-based scraper/reporter (for scraping channels + fallback reporting)
        if TelegramScraper is not None:
            try:
                self.telegram_scraper = TelegramScraper(config)
                if TelegramReporter is not None:
                    self.telegram_reporter = TelegramReporter(self.telegram_scraper)
                if ConfigReportingService is not None:
                    self.config_reporting_service = ConfigReportingService(self)
            except Exception as e:
                self.logger.info(f"Telegram init skipped: {e}")

        # Stealth obfuscation
        self.stealth_engine = None
        if StealthObfuscationEngine is not None and ObfuscationConfig is not None:
            try:
                stealth_config = ObfuscationConfig(
                    enabled=config.get("stealth_enabled", True),
                    use_async=config.get("stealth_async", True),
                    micro_fragmentation=config.get("micro_fragmentation", True),
                    sni_rotation=config.get("sni_rotation", True),
                    noise_generation=config.get("noise_generation", True),
                    ac_stress=config.get("ac_stress", True),
                    exhaustion_level=config.get("exhaustion_level", 0.8),
                    noise_intensity=config.get("noise_intensity", 0.7),
                    cdn_fronting=config.get("cdn_fronting", True)
                )
                self.stealth_engine = StealthObfuscationEngine(stealth_config)
            except Exception as e:
                self.logger.info(f"Stealth engine init skipped: {e}")
        
        # DNS manager
        self.dns_manager = None
        if DNSManager is not None:
            try:
                self.dns_manager = DNSManager()
            except Exception as e:
                self.logger.info(f"DNS manager init skipped: {e}")
        
        # Load balancer
        self.balancer = None
        if MultiProxyServer is not None:
            try:
                self.balancer = MultiProxyServer(
                    xray_path=config.get("xray_path", ""),
                    port=config.get("multiproxy_port", 10808),
                    num_backends=config.get("multiproxy_backends", 5),
                    health_check_interval=config.get("multiproxy_health_interval", 60),
                    obfuscation_engine=self.stealth_engine
                )
            except Exception as e:
                self.logger.info(f"Load balancer init skipped: {e}")

        self.gemini_balancer: Optional[MultiProxyServer] = None
        if self.config.get("gemini_balancer_enabled", False) and MultiProxyServer is not None:
            try:
                self.gemini_balancer = MultiProxyServer(
                    xray_path=config.get("xray_path", ""),
                    port=config.get("gemini_port", 10809),
                    num_backends=config.get("multiproxy_backends", 5),
                    health_check_interval=config.get("multiproxy_health_interval", 60),
                    obfuscation_engine=self.stealth_engine,
                )
            except Exception as e:
                self.logger.info(f"Gemini balancer init skipped: {e}")

        # State
        self.validated_configs: List[Tuple[str, float]] = []
        self.last_cycle = 0
        self.cycle_count = 0
        self._last_validated_count = 0
        self._consecutive_scrape_failures = 0
        self._last_good_configs: List[Tuple[str, float]] = []
        self._cycle_lock = threading.Lock()
        self._tg_publish_lock = threading.Lock()
        self._tg_publish_task: Optional[asyncio.Task] = None
        self._last_tg_publish_ts = 0.0
        try:
            self._tg_publish_interval_s = int(os.getenv("HUNTER_TELEGRAM_PUBLISH_INTERVAL", "1200"))
        except Exception:
            self._tg_publish_interval_s = 1200
        self._tg_publish_interval_s = max(60, min(24 * 3600, self._tg_publish_interval_s))
        try:
            self._last_tg_publish_ts = time.time() - float(self._tg_publish_interval_s)
        except Exception:
            self._last_tg_publish_ts = 0.0
        
        # DPI Evasion Orchestrator (with strict timeout)
        self.dpi_evasion: Optional[DPIEvasionOrchestrator] = None
        _task_mgr = HunterTaskManager.get_instance()
        if config.get("dpi_evasion_enabled", True) and DPIEvasionOrchestrator is not None:
            try:
                self.dpi_evasion = DPIEvasionOrchestrator()
                
                # Run start() via shared IO pool with 5s timeout
                ok = _task_mgr.run_with_timeout(self.dpi_evasion.start, 5.0)
                if ok is not None:
                    self.logger.info(
                        f"DPI Evasion: strategy={self.dpi_evasion.get_optimal_strategy().value}, "
                        f"network={self.dpi_evasion.state.network_type.value}"
                    )
                else:
                    self.logger.info("DPI Evasion: init timed out, using defaults")
            except Exception as e:
                self.logger.info(f"DPI Evasion init skipped: {e}")
                self.dpi_evasion = None
        
        # Start stealth engine (if available, with strict timeout)
        if self.stealth_engine is not None:
            try:
                ok = _task_mgr.run_with_timeout(self.stealth_engine.start, 5.0)
                if ok is not None:
                    self.logger.info("Stealth engine started")
                else:
                    self.logger.info("Stealth engine: init timed out, continuing without it")
            except Exception as e:
                self.logger.info(f"Stealth engine start skipped: {e}")
        
        # UI progress system
        self.ui = HunterUI()
        
        # Flexible config fetcher (Telegram priority with fallback strategies)
        self.flexible_fetcher = None
        if FlexibleConfigFetcher is not None and self.telegram_scraper is not None:
            try:
                self.flexible_fetcher = FlexibleConfigFetcher(
                    config=self.config,
                    telegram_scraper=self.telegram_scraper,
                    http_fetcher=self.config_fetcher,
                    logger=self.logger
                )
                self.logger.info("Flexible config fetcher initialized (Telegram priority)")
            except Exception as e:
                self.logger.info(f"Flexible fetcher init skipped: {e}")
        
        # Real-time dashboard (replaces verbose console logging)
        self.dashboard: Optional[Any] = None
        if HunterDashboard is not None:
            try:
                self.dashboard = HunterDashboard()
            except Exception:
                pass
        
        self.logger.info("Hunter Orchestrator initialized successfully")

        # Expose orchestrator globally so subsystems (e.g. TelegramScraper)
        # can discover the running balancer for proxy fallback.
        import hunter.orchestrator as _self_mod
        _self_mod._global_orchestrator = self

    async def scrape_configs(self) -> List[str]:
        """Scrape proxy configurations with Telegram priority and flexible fallbacks."""
        import gc as _gc
        
        # Use flexible fetcher if available
        if self.flexible_fetcher is not None:
            return await self._scrape_configs_flexible()
        
        # Fall back to original implementation
        return await self._scrape_configs_original()
    
    async def _scrape_configs_flexible(self) -> List[str]:
        """Flexible config scraping with Telegram priority and circuit breakers."""
        import gc as _gc
        
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        gemini_port = self.config.get("gemini_port", 10809)
        if gemini_port not in proxy_ports and self.gemini_balancer:
            proxy_ports.append(gemini_port)
        
        telegram_channels = self.config.get("targets", [])
        max_total = self.config.get("max_total", 1000)
        per_source_cap = max(100, max_total // 3)
        
        all_configs: set = set()
        sources_used = []
        
        if self.dashboard:
            self.dashboard.update(phase="scraping")
        
        with self.ui.scrape_progress(["Telegram", "HTTP-Fallback", "Cache"]) as tracker:
            self.ui.status.update(phase="scraping")
            
            # PHASE 1: Telegram First (Primary Strategy)
            if telegram_channels and self.flexible_fetcher:
                telegram_limit = self.config.get("telegram_limit", 50)
                if max_total <= 150:
                    telegram_limit = min(telegram_limit, 10)
                elif max_total <= 400:
                    telegram_limit = min(telegram_limit, 25)
                
                try:
                    result = await self.flexible_fetcher.fetch_telegram_with_retry(
                        channels=telegram_channels,
                        limit=telegram_limit,
                        max_retries=2,
                        retry_delay=2.0
                    )
                    
                    if result.success:
                        all_configs.update(result.configs)
                        sources_used.append("telegram")
                        tracker.source_done("Telegram", count=len(result.configs))
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="done", 
                                                        count=len(result.configs))
                        
                        self.logger.info(
                            f"[FlexibleFetch] Telegram success: {len(result.configs)} configs "
                            f"in {result.duration_ms:.0f}ms"
                        )
                        
                        # Skip HTTP if we got enough configs
                        if len(all_configs) >= per_source_cap * 2:
                            self.logger.info(
                                f"[FlexibleFetch] Sufficient configs ({len(all_configs)}), "
                                f"skipping HTTP fallback"
                            )
                            tracker.source_done("HTTP-Fallback", count=0)
                            if self.dashboard:
                                self.dashboard.source_update("HTTP-Fallback", status="done", count=0)
                    else:
                        tracker.source_failed("Telegram", reason=result.error or "failed")
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="failed")
                        self.logger.warning(
                            f"[FlexibleFetch] Telegram failed: {result.error}"
                        )
                        
                except Exception as e:
                    tracker.source_failed("Telegram", reason=str(e)[:40])
                    if self.dashboard:
                        self.dashboard.source_update("Telegram", status="failed")
                    self.logger.warning(f"[FlexibleFetch] Telegram exception: {e}")
            else:
                tracker.source_failed("Telegram", reason="no channels or fetcher")
                if self.dashboard:
                    self.dashboard.source_update("Telegram", status="failed")
            
            # PHASE 2: HTTP / GitHub Fallback
            # Always fetch from GitHub when Telegram failed; also fetch if we need more configs
            telegram_failed = "telegram" not in sources_used
            need_more = len(all_configs) < per_source_cap
            if (telegram_failed or need_more) and self.flexible_fetcher:
                try:
                    http_cap = max(per_source_cap, max_total // 2) if telegram_failed else per_source_cap
                    self.logger.info(
                        f"[FlexibleFetch] GitHub/HTTP fetch "
                        f"({'Telegram failed — GitHub is PRIMARY' if telegram_failed else 'supplementing'}): "
                        f"have {len(all_configs)}, cap {http_cap}"
                    )
                    
                    http_results = await self.flexible_fetcher.fetch_http_sources_parallel(
                        proxy_ports=proxy_ports,
                        max_configs=http_cap,
                        timeout_per_source=35.0 if telegram_failed else 25.0,
                        max_workers=8 if telegram_failed else 6
                    )
                    
                    http_success_count = 0
                    for result in http_results:
                        if result.success:
                            all_configs.update(result.configs)
                            if result.source not in sources_used:
                                sources_used.append(result.source)
                            http_success_count += 1
                    
                    tracker.source_done("HTTP-Fallback", count=http_success_count)
                    if self.dashboard:
                        self.dashboard.source_update("HTTP-Fallback", status="done", 
                                                    count=http_success_count)
                    
                    self.logger.info(
                        f"[FlexibleFetch] HTTP fallback: {http_success_count} sources, "
                        f"total configs now {len(all_configs)}"
                    )
                    
                except Exception as e:
                    tracker.source_failed("HTTP-Fallback", reason=str(e)[:40])
                    if self.dashboard:
                        self.dashboard.source_update("HTTP-Fallback", status="failed")
                    self.logger.warning(f"[FlexibleFetch] HTTP fallback exception: {e}")
            else:
                tracker.source_done("HTTP-Fallback", count=0)
                if self.dashboard:
                    self.dashboard.source_update("HTTP-Fallback", status="done", count=0)
            
            # PHASE 3: Cache (always load as backup)
            cache_configs = []
            
            if self.cache is not None:
                try:
                    cache_limit = min(500, per_source_cap)
                    cached = self.cache.load_cached_configs(max_count=cache_limit, working_only=True)
                    if cached:
                        cache_configs = list(cached) if isinstance(cached, set) else cached
                        tracker.source_done("Cache", count=len(cache_configs))
                        if self.dashboard:
                            self.dashboard.source_update("Cache", status="done", 
                                                        count=len(cache_configs))
                    else:
                        tracker.source_done("Cache", count=0)
                        if self.dashboard:
                            self.dashboard.source_update("Cache", status="done", count=0)
                except Exception as e:
                    tracker.source_failed("Cache", reason=str(e)[:30])
                    if self.dashboard:
                        self.dashboard.source_update("Cache", status="failed")
            else:
                tracker.source_done("Cache", count=0)
                if self.dashboard:
                    self.dashboard.source_update("Cache", status="done", count=0)
            
            # Load balancer cache as last resort
            balancer_uris = []
            try:
                bal_cache = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
                balancer_uris = [uri for uri, _ in bal_cache]
            except Exception:
                pass
            
            # Aggressive GC if memory is critical
            try:
                import psutil as _ps
                if _ps.virtual_memory().percent >= 90:
                    _gc.collect()
                    self.logger.info("[Memory] GC after scrape phase")
            except Exception:
                pass
        
        # Handle complete failure
        if len(all_configs) == 0:
            self._consecutive_scrape_failures += 1
            self.logger.warning(
                f"[FlexibleFetch] All online sources failed (streak: {self._consecutive_scrape_failures}). "
                f"Using cache."
            )
            all_configs.update(cache_configs)
            all_configs.update(balancer_uris)
        else:
            self._consecutive_scrape_failures = 0
            # Supplement with cache if we got some but not enough
            if len(all_configs) < 500:
                all_configs.update(cache_configs)
        
        # Final cap
        configs_list = list(all_configs)
        if len(configs_list) > max_total * 2:
            import random as _rnd
            _rnd.shuffle(configs_list)
            configs_list = configs_list[:max_total * 2]
            self.logger.info(f"[FlexibleFetch] Capped raw configs to {len(configs_list)}")
        
        self.logger.info(
            f"[FlexibleFetch] Total: {len(configs_list)} configs from sources: {sources_used}"
        )
        return configs_list
    
    async def _scrape_configs_original(self) -> List[str]:
        """Original scrape_configs implementation (fallback)."""
        import gc as _gc
        configs = []
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        gemini_port = self.config.get("gemini_port", 10809)
        if gemini_port not in proxy_ports and self.gemini_balancer:
            proxy_ports.append(gemini_port)
        
        max_total = self.config.get("max_total", 1000)
        per_source_cap = max(100, max_total // 3)
        
        source_names = []
        tasks = []
        task_names = []
        
        # Telegram configs
        telegram_channels = self.config.get("targets", [])
        if telegram_channels and self.telegram_scraper is not None:
            telegram_limit = self.config.get("telegram_limit", 50)
            if max_total <= 150:
                telegram_limit = min(telegram_limit, 10)
            elif max_total <= 400:
                telegram_limit = min(telegram_limit, 25)
            source_names.append("Telegram")
            task_names.append("Telegram")
            
            async def fetch_telegram(_lim=telegram_limit):
                try:
                    telegram_configs = await asyncio.wait_for(
                        self.telegram_scraper.scrape_configs(telegram_channels, limit=_lim),
                        timeout=120
                    )
                    if isinstance(telegram_configs, set):
                        return list(telegram_configs)[:per_source_cap]
                    return (telegram_configs if isinstance(telegram_configs, list) else [])[:per_source_cap]
                except asyncio.TimeoutError:
                    self.logger.warning("Telegram scrape timed out after 120s")
                    return []
                except Exception as e:
                    self.logger.info(f"Telegram scrape skipped: {e}")
                    return []
            
            tasks.append(fetch_telegram())
        
        # HTTP config fetches
        if self.config_fetcher is not None:
            loop = asyncio.get_running_loop()
            
            for name, method in [
                ("GitHub", self.config_fetcher.fetch_github_configs),
                ("AntiCensor", self.config_fetcher.fetch_anti_censorship_configs),
                ("IranPriority", self.config_fetcher.fetch_iran_priority_configs),
            ]:
                source_names.append(name)
                task_names.append(name)
                
                async def _fetch(m=method, n=name, cap=per_source_cap):
                    try:
                        _mgr = HunterTaskManager.get_instance()
                        result = await loop.run_in_executor(
                            _mgr._io_pool, lambda: m(proxy_ports, max_configs=cap)
                        )
                        return list(result)[:cap]
                    except Exception as e:
                        self.logger.info(f"{n} fetch skipped: {e}")
                        return []
                
                tasks.append(_fetch())
        
        source_names.extend(["Cache", "BalancerCache"])
        
        if self.dashboard:
            self.dashboard.update(phase="scraping")
            for sn in source_names:
                self.dashboard.source_update(sn, status="waiting")
        
        with self.ui.scrape_progress(source_names) as tracker:
            self.ui.status.update(phase="scraping")
            
            results = await asyncio.gather(*tasks, return_exceptions=True)
            
            for idx, result in enumerate(results):
                name = task_names[idx] if idx < len(task_names) else f"Source-{idx}"
                if isinstance(result, list):
                    configs.extend(result)
                    tracker.source_done(name, count=len(result))
                    if self.dashboard:
                        self.dashboard.source_update(name, status="done", count=len(result))
                elif isinstance(result, Exception):
                    tracker.source_failed(name, reason=str(result)[:40])
                    if self.dashboard:
                        self.dashboard.source_update(name, status="failed")
                else:
                    tracker.source_failed(name, reason="unexpected")
                    if self.dashboard:
                        self.dashboard.source_update(name, status="failed")
            
            try:
                import psutil as _ps
                if _ps.virtual_memory().percent >= 90:
                    _gc.collect()
                    self.logger.info("[Memory] GC after scrape phase")
            except Exception:
                pass
            
            cache_limit = min(500, per_source_cap)
            cache_configs = []
            if self.cache is not None:
                try:
                    cached = self.cache.load_cached_configs(max_count=cache_limit, working_only=True)
                    if cached:
                        cache_configs = list(cached) if isinstance(cached, set) else cached
                        tracker.source_done("Cache", count=len(cache_configs))
                        if self.dashboard:
                            self.dashboard.source_update("Cache", status="done", count=len(cache_configs))
                    else:
                        tracker.source_done("Cache", count=0)
                        if self.dashboard:
                            self.dashboard.source_update("Cache", status="done", count=0)
                except Exception as e:
                    tracker.source_failed("Cache", reason=str(e)[:30])
                    if self.dashboard:
                        self.dashboard.source_update("Cache", status="failed")
            else:
                tracker.source_done("Cache", count=0)
                if self.dashboard:
                    self.dashboard.source_update("Cache", status="done", count=0)

            balancer_uris = []
            try:
                bal_cache = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
                balancer_uris = [uri for uri, _ in bal_cache]
                tracker.source_done("BalancerCache", count=len(balancer_uris))
                if self.dashboard:
                    self.dashboard.source_update("BalancerCache", status="done", count=len(balancer_uris))
            except Exception:
                tracker.source_done("BalancerCache", count=0)
                if self.dashboard:
                    self.dashboard.source_update("BalancerCache", status="done", count=0)

        if len(configs) == 0:
            self._consecutive_scrape_failures += 1
            self.logger.warning(
                f"[Resilience] All online sources failed (streak: {self._consecutive_scrape_failures}). "
                f"Using cached configs as primary source."
            )
            configs.extend(cache_configs)
            configs.extend(balancer_uris)
        else:
            self._consecutive_scrape_failures = 0
            if len(configs) < 500:
                configs.extend(cache_configs)

        if len(configs) > max_total * 2:
            import random as _rnd
            _rnd.shuffle(configs)
            configs = configs[:max_total * 2]
            self.logger.info(f"Capped raw configs to {len(configs)} (max_total={max_total})")

        return configs

    def validate_configs(self, configs: Iterable[str], max_workers: int = 50) -> List[HunterBenchResult]:
        """Validate and benchmark configurations with adaptive thread management."""
        try:
            from hunter.core.utils import prioritize_configs
        except ImportError:
            from core.utils import prioritize_configs
        
        results: List[HunterBenchResult] = []
        test_url = self.config.get("test_url", "https://www.cloudflare.com/cdn-cgi/trace")
        timeout = self.config.get("timeout_seconds", 10)

        # Use adaptive max_workers from cycle configuration
        configured_workers = self.config.get("max_workers")
        if isinstance(configured_workers, int) and configured_workers > 0:
            max_workers = configured_workers

        # Use adaptive max_total from cycle configuration (already set in run_cycle)
        max_total = self.config.get("max_total", 800)
        deduped_configs = list(dict.fromkeys(list(configs)))[:max_total]
        
        # Check if we have any configs to validate
        if not deduped_configs:
            self.logger.info("No configs to validate")
            return []
        
        # Check required components
        if self.parser is None or self.benchmarker is None:
            self.logger.info("Parser or benchmarker not available - skipping validation")
            return []
        
        self.logger.info(f"Processing {len(deduped_configs)} unique configs for validation (max: {max_total})")
        
        # Prioritize configs by anti-censorship features (Reality, gRPC, CDN, etc.)
        # In test mode, skip prioritization to validate all configs
        test_mode = os.getenv("HUNTER_TEST_MODE", "").lower() == "true"
        self.logger.debug(f"Test mode check: HUNTER_TEST_MODE={os.getenv('HUNTER_TEST_MODE')} -> {test_mode}")
        if test_mode:
            limited_configs = deduped_configs
            self.logger.info(f"TEST MODE: Skipping prioritization, using all {len(deduped_configs)} configs")
        else:
            # Use DPI evasion orchestrator for strategy-aware prioritization if available
            if self.dpi_evasion:
                limited_configs = self.dpi_evasion.prioritize_configs_for_strategy(deduped_configs)
                strategy = self.dpi_evasion.get_optimal_strategy()
                self.logger.info(
                    f"DPI-aware prioritization: strategy={strategy.value}, "
                    f"network={self.dpi_evasion.state.network_type.value}"
                )
            else:
                limited_configs = prioritize_configs(deduped_configs)
        self.logger.info(f"Prioritized {len(limited_configs)} configs by anti-DPI features")

        # Start adaptive thread pool
        try:
            self.benchmarker.start_thread_pool()
        except Exception as e:
            self.logger.info(f"Thread pool start issue: {e}")
        
        # Parse configurations with progress
        parsed_configs = []
        with self.ui.task_progress(len(limited_configs), f"Parsing {len(limited_configs)} configs") as pbar:
            for uri in limited_configs:
                try:
                    parsed = self.parser.parse(uri)
                    if parsed:
                        parsed_configs.append(parsed)
                except Exception as e:
                    self.logger.debug(f"Failed to parse {uri}: {e}")
                pbar.advance(1)
        
        # Pre-filter: remove configs that the balancer will reject (e.g. legacy SS ciphers)
        # This avoids wasting benchmark time on configs XRay 25.x can't use in balancer mode
        try:
            from hunter.proxy.load_balancer import XRAY_SUPPORTED_SS_CIPHERS
        except ImportError:
            from proxy.load_balancer import XRAY_SUPPORTED_SS_CIPHERS
        pre_filter_count = len(parsed_configs)
        filtered_parsed = []
        for parsed in parsed_configs:
            proto = str(parsed.outbound.get("protocol", "") or "").lower()
            if proto == "shadowsocks":
                servers = parsed.outbound.get("settings", {}).get("servers", [])
                dominated_by_legacy = False
                for srv in servers:
                    method = str(srv.get("method", "") or "").lower()
                    if method and method not in XRAY_SUPPORTED_SS_CIPHERS:
                        dominated_by_legacy = True
                        break
                if dominated_by_legacy:
                    continue
            filtered_parsed.append(parsed)
        parsed_configs = filtered_parsed
        if pre_filter_count != len(parsed_configs):
            self.logger.info(
                f"Pre-filter: {pre_filter_count} -> {len(parsed_configs)} "
                f"(removed {pre_filter_count - len(parsed_configs)} unsupported SS ciphers)"
            )
        
        # Prepare configs for batch benchmarking
        benchmark_configs = []
        base_port = int(self.config.get("multiproxy_port", 10808)) + 1000
        
        for i, parsed in enumerate(parsed_configs):
            socks_port = base_port + i
            benchmark_configs.append((parsed, socks_port))
        
        # Attach UI callback to benchmarker for live progress updates
        self.ui.status.update(phase="validating")
        if self.dashboard:
            self.dashboard.update(phase="validating")
            self.dashboard.reset_bench(len(benchmark_configs))
        self.benchmarker._ui_callback = None
        bench_tracker = None
        try:
            bench_ctx = self.ui.benchmark_progress(len(benchmark_configs))
            bench_tracker = bench_ctx.__enter__()
            
            def _on_result(parsed_cfg, latency, tier_str):
                """Called by benchmarker for each completed config."""
                name = getattr(parsed_cfg, 'ps', '') or ''
                if latency:
                    bench_tracker.tested(
                        success=True,
                        name=name,
                        latency=latency,
                        tier=tier_str
                    )
                    if self.dashboard:
                        self.dashboard.bench_result(True, name=name, latency=latency, tier=tier_str)
                else:
                    bench_tracker.tested(success=False)
                    if self.dashboard:
                        self.dashboard.bench_result(False)
            
            self.benchmarker._ui_callback = _on_result
        except Exception:
            pass
        
        # Use optimized batch benchmarking with adaptive thread management
        self.logger.info(f"Starting batch benchmark of {len(benchmark_configs)} configs")
        results = self.benchmarker.benchmark_configs_batch(
            benchmark_configs, test_url, timeout
        )
        
        # Close progress bar
        try:
            if bench_tracker is not None:
                bench_ctx.__exit__(None, None, None)
            self.benchmarker._ui_callback = None
        except Exception:
            pass
        
        # Get performance metrics
        try:
            metrics = self.benchmarker.get_performance_metrics()
            self.logger.info(f"Thread pool: {metrics['thread_pool_metrics']['tasks_per_second']:.1f} tasks/sec")
        except Exception:
            pass
        
        return results

    async def report_configs_to_telegram(self, results: List[HunterBenchResult]):
        """Report validated configs to Telegram group.
        
        Sends:
        - Top 10 configs as copyable text messages
        - Top 50 configs as a file for import
        """
        if not results:
            self.logger.warning("No configs to report")
            return False
        
        try:
            self.logger.info(f"Reporting {len(results)} validated configs to Telegram...")
            success = await self.config_reporting_service.report_validated_configs(results)
            if success:
                self.logger.info("Successfully reported configs to Telegram")
            else:
                self.logger.warning("Failed to report configs to Telegram")
            return success
        except Exception as e:
            self.logger.error(f"Config reporting error: {e}")
            return False

    def tier_configs(self, results: List[HunterBenchResult]) -> Dict[str, List[HunterBenchResult]]:
        """Tier configurations by performance."""
        gold = []
        silver = []

        for result in results:
            if result.tier == "gold":
                gold.append(result)
            elif result.tier == "silver":
                silver.append(result)

        return {
            "gold": gold[:100],  # Keep top 100
            "silver": silver[:200]  # Keep top 200
        }

    async def update_balancer(self, configs: List[Tuple[str, float]]):
        """Update load balancer with new configurations.
        
        Note: DO NOT obfuscate URIs here - the balancer's _create_balanced_config
        already applies outbound-level obfuscation. URI-level obfuscation corrupts
        the URIs making them unparseable by the balancer.
        """
        self.balancer.update_available_configs(configs, trusted=True)

    async def update_gemini_balancer(self, configs: List[Tuple[str, float]]):
        """Update Gemini balancer with new configurations."""
        if not self.gemini_balancer:
            return
        self.gemini_balancer.update_available_configs(configs, trusted=True)

    def _balancer_cache_path(self, name: str = "HUNTER_balancer_cache.json") -> str:
        try:
            state_file = str(self.config.get("state_file", "") or "")
            if state_file:
                base_dir = os.path.dirname(state_file)
                if base_dir:
                    return os.path.join(base_dir, name)
        except Exception:
            pass
        return os.path.join(os.path.dirname(__file__), "runtime", name)

    def _load_balancer_cache(self, name: str = "HUNTER_balancer_cache.json") -> List[Tuple[str, float]]:
        path = self._balancer_cache_path(name=name)
        payload = load_json(path, default={})
        if not isinstance(payload, dict):
            return []
        items = payload.get("configs")
        if not isinstance(items, list):
            return []
        # Age-awareness: warn if cache is stale but still use it
        saved_at = payload.get("saved_at", 0)
        if saved_at:
            try:
                age_hours = (time.time() - float(saved_at)) / 3600
                if age_hours > 24:
                    self.logger.warning(f"[Cache] {name} is {age_hours:.0f}h old - very stale, will still try")
                elif age_hours > 6:
                    self.logger.info(f"[Cache] {name} is {age_hours:.1f}h old - somewhat stale")
                else:
                    self.logger.info(f"[Cache] {name} is {age_hours:.1f}h old - fresh")
            except Exception:
                pass
        seed: List[Tuple[str, float]] = []
        for item in items:
            try:
                if isinstance(item, dict) and isinstance(item.get("uri"), str):
                    latency = float(item.get("latency_ms", 0.0))
                    seed.append((item["uri"], latency))
            except Exception:
                continue
        return seed

    def _sync_dashboard_balancer(self):
        """Push current balancer status to the dashboard."""
        if not self.dashboard:
            return
        try:
            if self.balancer:
                st = self.balancer.get_status()
                self.dashboard.update(
                    bal_main_port=self.balancer.port,
                    bal_main_backends=st.get("backends", 0),
                    bal_main_status="OK" if st.get("running") else "down",
                )
        except Exception:
            pass
        try:
            if self.gemini_balancer:
                gs = self.gemini_balancer.get_status()
                self.dashboard.update(
                    bal_gemini_port=self.gemini_balancer.port,
                    bal_gemini_backends=gs.get("backends", 0),
                    bal_gemini_status="OK" if gs.get("running") else "down",
                )
        except Exception:
            pass

    def _save_balancer_cache(self, configs: List[Tuple[str, float]], name: str = "HUNTER_balancer_cache.json") -> None:
        path = self._balancer_cache_path(name=name)
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
        except Exception:
            pass
        payload = {
            "saved_at": now_ts(),
            "configs": [{"uri": uri, "latency_ms": float(lat)} for uri, lat in configs[:1000]],
        }
        save_json(path, payload)

    async def report_status(self):
        """Report current status to Telegram."""
        if self.balancer is None:
            return
        reporters = [r for r in (self.bot_reporter, self.telegram_reporter) if r is not None]
        if not reporters:
            return
        try:
            status = self.balancer.get_status()
        except Exception:
            return
        for reporter in reporters:
            try:
                ok = await reporter.report_status(status)
                if ok is not False:
                    return
            except Exception as e:
                self.logger.info(f"Status report skipped: {e}")

    def _telegram_outbox_path(self) -> str:
        try:
            state_file = str(self.config.get("state_file", "") or "")
            if state_file:
                base_dir = os.path.dirname(state_file)
                if base_dir:
                    return os.path.join(base_dir, "HUNTER_telegram_outbox.json")
        except Exception:
            pass
        return os.path.join(os.path.dirname(__file__), "runtime", "HUNTER_telegram_outbox.json")

    def _get_tg_publish_targets(self) -> List[Any]:
        raw = (
            os.getenv("HUNTER_TELEGRAM_PUBLISH_CHATS")
            or os.getenv("HUNTER_TELEGRAM_PUBLISH_TARGETS")
            or os.getenv("TELEGRAM_PUBLISH_CHATS")
            or ""
        )
        parts: List[Any] = []
        if raw:
            for p in raw.split(","):
                s = (p or "").strip()
                if s:
                    parts.append(s)
        else:
            rc = self.config.get("report_channel")
            if rc:
                parts.append(rc)
            else:
                cid = os.getenv("CHAT_ID") or os.getenv("TELEGRAM_GROUP_ID")
                if cid:
                    parts.append(str(cid).strip())

        out: List[Any] = []
        seen: Set[str] = set()
        for it in parts:
            x: Any = it
            if isinstance(x, str):
                s = x.strip()
                if s.lstrip("-").isdigit():
                    try:
                        x = int(s)
                    except Exception:
                        x = s
                else:
                    x = s
            key = str(x)
            if key in seen:
                continue
            seen.add(key)
            out.append(x)
        return out

    def _collect_publish_uris(self, max_lines: int) -> List[str]:
        gold_file = str(self.config.get("gold_file", "") or "")
        silver_file = str(self.config.get("silver_file", "") or "")
        all_uris: List[str] = []
        for fpath in (gold_file, silver_file):
            if not fpath:
                continue
            try:
                all_uris.extend(read_lines(fpath))
            except Exception:
                pass

        seen: Set[str] = set()
        deduped: List[str] = []
        for u in all_uris:
            if not isinstance(u, str):
                continue
            s = u.strip()
            if not s:
                continue
            if s in seen:
                continue
            seen.add(s)
            deduped.append(s)

        if max_lines > 0 and len(deduped) > max_lines:
            deduped = deduped[-max_lines:]
        return deduped

    async def _publish_uris_to_targets(self, uris: List[str], gemini_uris: Optional[List[str]] = None, force: bool = False) -> bool:
        reporters = [r for r in (self.bot_reporter, self.telegram_reporter) if r is not None]
        if not reporters:
            return False

        now = time.time()
        due = (now - float(self._last_tg_publish_ts or 0.0)) >= float(self._tg_publish_interval_s)
        if not force and not due:
            return False

        acquired = self._tg_publish_lock.acquire(blocking=False)
        if not acquired:
            return False

        try:
            targets = self._get_tg_publish_targets()
            if not targets:
                return False

            try:
                max_lines = int(os.getenv("HUNTER_TELEGRAM_PUBLISH_MAX_LINES", "50"))
            except Exception:
                max_lines = 50
            max_lines = max(5, min(400, max_lines))

            send_uris = (uris or [])[:max_lines]
            if not send_uris:
                return False

            if self.dashboard:
                try:
                    self.dashboard.update(tg_status=f"publishing:{len(send_uris)}")
                except Exception:
                    pass
            try:
                self.ui.status.update(tg_status=f"publishing:{len(send_uris)}")
            except Exception:
                pass
            try:
                self.logger.info(f"[Telegram] Publishing {len(send_uris)} configs to {len(targets)} chat(s)")
            except Exception:
                pass

            ok_any = False
            for chat_id in targets:
                ok_target = False
                for reporter in reporters:
                    try:
                        try:
                            await self._flush_telegram_outbox(reporter, chat_id=chat_id)
                        except Exception:
                            pass
                        
                        # Get current working configs for BotReporter fallback
                        if hasattr(reporter, 'fallback_sender') and reporter.fallback_sender:
                            # Get current working configs from balancer
                            working_configs = []
                            if self.balancer:
                                try:
                                    working_configs = [b.uri for b in self.balancer.get_working_backends()[:10]]
                                except Exception:
                                    pass
                            # Pass configs to the reporter
                            reporter.fallback_sender.current_configs = working_configs
                        
                        ok_target = bool(await reporter.report_config_files(
                            gold_uris=send_uris,
                            gemini_uris=(gemini_uris or None),
                            max_lines=max_lines,
                            chat_id=chat_id,
                        ))
                    except Exception:
                        ok_target = False
                    if ok_target:
                        break

                if not ok_target:
                    try:
                        self._enqueue_telegram_outbox(
                            gold_uris=send_uris,
                            gemini_uris=(gemini_uris or None),
                            chat_id=chat_id,
                        )
                    except Exception:
                        pass
                ok_any = ok_any or ok_target

            if ok_any:
                self._last_tg_publish_ts = time.time()
                if self.dashboard:
                    try:
                        self.dashboard.update(tg_status="published")
                    except Exception:
                        pass
                try:
                    self.ui.status.update(tg_status="published")
                except Exception:
                    pass
            return ok_any
        finally:
            try:
                self._tg_publish_lock.release()
            except Exception:
                pass

    async def _maybe_periodic_publish(self) -> None:
        try:
            max_lines = int(os.getenv("HUNTER_TELEGRAM_PUBLISH_MAX_LINES", "50"))
        except Exception:
            max_lines = 50
        max_lines = max(5, min(400, max_lines))
        uris = self._collect_publish_uris(max_lines=max_lines)
        if not uris:
            return
        try:
            await self._publish_uris_to_targets(uris, force=False)
        except Exception:
            return

    async def _telegram_periodic_publish_loop(self) -> None:
        while True:
            try:
                await self._maybe_periodic_publish()
            except asyncio.CancelledError:
                break
            except Exception:
                pass
            await asyncio.sleep(60)

    def _load_telegram_outbox(self) -> List[Dict[str, Any]]:
        path = self._telegram_outbox_path()
        payload = load_json(path, default={})
        items = payload.get("items") if isinstance(payload, dict) else None
        if not isinstance(items, list):
            return []
        out: List[Dict[str, Any]] = []
        for it in items:
            if not isinstance(it, dict):
                continue
            if not isinstance(it.get("gold_uris"), list):
                continue
            out.append(it)
        out.sort(key=lambda x: int(x.get("created_at", 0)))
        return out

    def _save_telegram_outbox(self, items: List[Dict[str, Any]]) -> None:
        path = self._telegram_outbox_path()
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
        except Exception:
            pass
        max_items = int(os.getenv("HUNTER_TELEGRAM_OUTBOX_MAX", "20"))
        if max_items > 0 and len(items) > max_items:
            items = items[-max_items:]
        save_json(path, {"saved_at": now_ts(), "items": items})

    def _enqueue_telegram_outbox(self, gold_uris: List[str], gemini_uris: Optional[List[str]] = None, chat_id: Optional[Any] = None) -> str:
        max_lines = int(os.getenv("HUNTER_TELEGRAM_MAX_LINES", "200"))
        gold = [u for u in (gold_uris or []) if isinstance(u, str) and u][:max_lines]
        gem = [u for u in (gemini_uris or []) if isinstance(u, str) and u][:max_lines] if gemini_uris else []
        raw = ("\n".join(gold) + "\n---\n" + "\n".join(gem)).encode("utf-8", errors="ignore")
        item_id = hashlib.sha1(raw).hexdigest()[:16]
        items = self._load_telegram_outbox()
        if any(it.get("id") == item_id and it.get("chat_id") == chat_id for it in items):
            return item_id
        items.append({
            "id": item_id,
            "created_at": now_ts(),
            "attempts": 0,
            "chat_id": chat_id,
            "gold_uris": gold,
            "gemini_uris": gem,
        })
        self._save_telegram_outbox(items)
        return item_id

    async def _flush_telegram_outbox(self, reporter: Any, chat_id: Optional[Any] = None) -> Tuple[int, int]:
        items = self._load_telegram_outbox()
        if not items:
            return (0, 0)
        sent = 0
        kept: List[Dict[str, Any]] = []
        for idx, it in enumerate(items):
            it_chat = it.get("chat_id", None)
            if chat_id is not None and it_chat not in (None, chat_id):
                kept.append(it)
                continue
            try:
                it["attempts"] = int(it.get("attempts", 0)) + 1
            except Exception:
                it["attempts"] = 1
            ok = False
            try:
                ok = bool(await reporter.report_config_files(
                    gold_uris=it.get("gold_uris", []),
                    gemini_uris=it.get("gemini_uris", None) or None,
                    chat_id=chat_id,
                ))
            except Exception:
                ok = False
            if ok:
                sent += 1
                continue
            kept.append(it)
            for rest in items[idx + 1:]:
                kept.append(rest)
            break
        self._save_telegram_outbox(kept)
        return (sent, len(kept))

    async def run_cycle(self) -> bool:
        """Run one complete hunter cycle."""
        import gc
        try:
            import psutil
        except ImportError:
            psutil = None

        acquired = self._cycle_lock.acquire(blocking=False)
        if not acquired:
            self.logger.info("[CycleLock] Another cycle is already running, skipping")
            return False
        
        try:
            self.cycle_count += 1
            cycle_start = time.time()
            self.ui.status.update(cycle=self.cycle_count, phase="starting")
            if self.dashboard:
                self.dashboard.update(cycle=self.cycle_count, phase="starting")
                self.dashboard.reset_sources()
                self.dashboard.reset_bench(0)

            self.logger.info(f"Starting hunter cycle #{self.cycle_count}")
        
            # ============================================================
            # CRITICAL: Memory check and adaptive configuration at cycle start
            # ============================================================
            if psutil:
                mem = psutil.virtual_memory()
                mem_percent = mem.percent
                mem_gb_used = mem.used / (1024**3)
                mem_gb_total = mem.total / (1024**3)
                
                self.ui.status.update(memory_pct=mem_percent)
                if self.dashboard:
                    self.dashboard.update(
                        memory_pct=mem_percent,
                        memory_used_gb=mem_gb_used,
                        memory_total_gb=mem_gb_total,
                    )
                self.logger.info(
                    f"Memory status: {mem_percent:.1f}% used "
                    f"({mem_gb_used:.1f}GB / {mem_gb_total:.1f}GB)"
                )
                
                # Quick cleanup if memory is high (non-blocking)
                if mem_percent >= 85:
                    gc.collect(0)
                    gc.collect(1)
                    mem_percent = psutil.virtual_memory().percent
                
                # Auto-tune based on CPU cores - XRay test processes are I/O bound, not RAM bound
                cpu_cores = os.cpu_count() or 4
                base_workers = max(4, cpu_cores)  # At least 4, scale with cores
                
                # Adaptive configuration based on memory pressure
                # Workers are I/O bound (network tests), so we can run more than cores
                if mem_percent >= 95:
                    adaptive_max_total = 80
                    adaptive_max_workers = max(4, base_workers // 2)
                    adaptive_scan_limit = 15
                    mode = "ULTRA-MINIMAL"
                elif mem_percent >= 90:
                    adaptive_max_total = 150
                    adaptive_max_workers = max(6, base_workers)
                    adaptive_scan_limit = 25
                    mode = "MINIMAL"
                elif mem_percent >= 85:
                    adaptive_max_total = 250
                    adaptive_max_workers = max(8, base_workers)
                    adaptive_scan_limit = 30
                    mode = "REDUCED"
                elif mem_percent >= 80:
                    adaptive_max_total = 400
                    adaptive_max_workers = max(8, base_workers + 2)
                    adaptive_scan_limit = 35
                    mode = "CONSERVATIVE"
                elif mem_percent >= 70:
                    adaptive_max_total = 600
                    adaptive_max_workers = max(10, base_workers + 4)
                    adaptive_scan_limit = 40
                    mode = "SCALED"
                elif mem_percent >= 60:
                    adaptive_max_total = 800
                    adaptive_max_workers = max(12, base_workers * 2)
                    adaptive_scan_limit = 50
                    mode = "MODERATE"
                else:
                    adaptive_max_total = self.config.get("max_total", 1000)
                    adaptive_max_workers = max(12, base_workers * 2)
                    adaptive_scan_limit = self.config.get("scan_limit", 50)
                    mode = "NORMAL"
                
                if self.dashboard:
                    self.dashboard.update(mode=mode)
                self.logger.info(
                    f"Cycle mode: {mode} (memory: {mem_percent:.1f}%, "
                    f"configs: {adaptive_max_total}, workers: {adaptive_max_workers})"
                )
                
                # Temporarily override config for this cycle
                original_max_total = self.config.get("max_total")
                original_max_workers = self.config.get("max_workers")
                original_scan_limit = self.config.get("scan_limit")
                
                self.config.set("max_total", adaptive_max_total)
                self.config.set("max_workers", adaptive_max_workers)
                self.config.set("scan_limit", adaptive_scan_limit)
            else:
                self.logger.warning("psutil not available - running without memory monitoring")
                original_max_total = None
                original_max_workers = None
                original_scan_limit = None

            # Reset HTTP session to free stale connections from previous cycle
            if self.http_manager is not None:
                try:
                    self.http_manager.reset_session()
                except Exception:
                    pass

            # Scrape configs
            raw_configs = await self.scrape_configs()
            self.logger.info(f"Total raw configs: {len(raw_configs)}")

            # GC after scrape phase - critical for high memory situations
            gc.collect()

            # Cache new configs
            if self.cache is not None:
                try:
                    self.cache.save_configs(raw_configs, working=False)
                except Exception as e:
                    self.logger.info(f"Cache save skipped: {e}")

            # Validate configs
            validated = self.validate_configs(raw_configs)
            self.logger.info(f"Validated configs: {len(validated)}")

            # Free raw configs to release memory immediately
            del raw_configs
            gc.collect()

            # Save working configs to cache for future cycles
            if validated and self.cache is not None:
                try:
                    working_uris = [r.uri for r in validated]
                    self.cache.save_configs(working_uris, working=True)
                except Exception as e:
                    self.logger.info(f"Cache save skipped: {e}")

            # Incremental balancer feeding: feed balancer NOW with gold/silver only
            # Dead-tier configs are unreliable and cause XRay startup failures (code 23)
            if validated and self.balancer is not None:
                try:
                    usable = [(r.uri, r.latency_ms) for r in validated if r.tier in ("gold", "silver")]
                    if usable:
                        early_configs = usable[:20]
                        self.balancer.update_available_configs(early_configs, trusted=True)
                        self.logger.info(f"[Incremental] Fed balancer with {len(early_configs)} gold/silver configs early")
                except Exception:
                    pass

            # Re-test cached configs every 3 cycles — skip when memory is critical
            mem_now = psutil.virtual_memory().percent if psutil else 50
            if self.cycle_count % 3 == 0 and mem_now < 90:
                gold_file = str(self.config.get("gold_file", "") or "")
                silver_file = str(self.config.get("silver_file", "") or "")
                cached_uris = set()
                for fpath in [gold_file, silver_file]:
                    if fpath:
                        try:
                            cached_uris.update(read_lines(fpath))
                        except Exception:
                            pass
                # Also add balancer cache URIs
                try:
                    bal_cache = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
                    cached_uris.update(uri for uri, _ in bal_cache)
                except Exception:
                    pass
                cached_uris.discard("")
                # Remove already-validated URIs
                validated_uris = {r.uri for r in validated}
                retest_uris = [u for u in cached_uris if u not in validated_uris][:100]
                if retest_uris:
                    self.logger.info(f"[Retest] Re-testing {len(retest_uris)} cached configs from previous cycles...")
                    retest_results = self.validate_configs(retest_uris)
                    if retest_results:
                        self.logger.info(f"[Retest] {len(retest_results)} cached configs still working")
                        validated.extend(retest_results)
                        validated.sort(key=lambda r: r.latency_ms)
                    else:
                        self.logger.warning("[Retest] No cached configs passed retest")

            # Tier configs
            tiered = self.tier_configs(validated)
            gold_configs = tiered["gold"]
            silver_configs = tiered["silver"]

            self.logger.info(f"Gold tier: {len(gold_configs)}, Silver tier: {len(silver_configs)}")
            self.ui.status.update(
                gold=len(gold_configs), silver=len(silver_configs),
                phase="balancing"
            )
            if self.dashboard:
                self.dashboard.update(
                    gold=len(gold_configs), silver=len(silver_configs),
                    phase="balancing"
                )

            # Update balancer
            all_configs = [(r.uri, r.latency_ms) for r in gold_configs + silver_configs]
            self._last_validated_count = len(all_configs)
            if all_configs:
                self._last_good_configs = all_configs[:200]  # Keep best 200 for resilience
        
            if self.balancer is not None:
                if all_configs:
                    try:
                        await self.update_balancer(all_configs)
                    except Exception as e:
                        self.logger.info(f"Balancer update skipped: {e}")
                elif self._last_good_configs:
                    # No new configs validated - re-feed last known good configs
                    self.logger.warning("[Resilience] No new configs, re-feeding last known good configs to balancer")
                    try:
                        await self.update_balancer(self._last_good_configs)
                    except Exception as e:
                        self.logger.info(f"Balancer resilience update skipped: {e}")
            self._sync_dashboard_balancer()
            try:
                if all_configs:
                    self._save_balancer_cache(all_configs, name="HUNTER_balancer_cache.json")
            except Exception:
                pass

            # Feed gemini balancer: prefer name-matched, but fall back to ALL configs
            gemini_configs: List[Tuple[str, float]] = []
            if self.gemini_balancer:
                # First try name-matched configs
                for r in gold_configs + silver_configs:
                    ps = (r.ps or "").lower()
                    if "gemini" in ps or "gmn" in ps:
                        gemini_configs.append((r.uri, r.latency_ms))
                # If no name-matched, use ALL validated configs (different subset)
                if not gemini_configs and all_configs:
                    # Use second half of configs for diversity (balancer uses first half via leastPing)
                    half = max(1, len(all_configs) // 2)
                    gemini_configs = all_configs[half:] if len(all_configs) > 3 else list(all_configs)
                    self.logger.info(f"[Gemini] No name-matched configs, using {len(gemini_configs)} from validated pool")
                try:
                    if gemini_configs:
                        await self.update_gemini_balancer(gemini_configs)
                        self._save_balancer_cache(gemini_configs, name="HUNTER_gemini_balancer_cache.json")
                except Exception:
                    pass

            all_validated = gold_configs + silver_configs
            if all_validated:
                self.ui.status.update(phase="reporting")
                if self.dashboard:
                    self.dashboard.update(phase="reporting")
                try:
                    await self._publish_uris_to_targets(
                        [r.uri for r in all_validated],
                        gemini_uris=[uri for uri, _ in gemini_configs] if gemini_configs else None,
                        force=False,
                    )
                except Exception:
                    pass

            # Save to files
            self._save_to_files(gold_configs, silver_configs)

            cycle_time = time.time() - cycle_start
            self.logger.info(f"Cycle completed in {cycle_time:.1f} seconds")
            self.last_cycle = time.time()
        
            # Restore original config values (ALWAYS - even partial success)
            if psutil and original_max_total is not None:
                self.config.set("max_total", original_max_total)
                self.config.set("max_workers", original_max_workers)
                self.config.set("scan_limit", original_scan_limit)
        
            # Final memory status
            if psutil:
                try:
                    final_mem = psutil.virtual_memory()
                    self.logger.info(
                        f"Cycle end memory: {final_mem.percent:.1f}% "
                        f"({final_mem.used / (1024**3):.1f}GB / {final_mem.total / (1024**3):.1f}GB)"
                    )
                except Exception:
                    pass
        
            # Log DPI evasion status
            if self.dpi_evasion:
                try:
                    self.logger.info(f"DPI Evasion: {self.dpi_evasion.get_status_summary()}")
                except Exception:
                    pass

            return True
        finally:
            try:
                self._cycle_lock.release()
            except Exception:
                pass

    def _append_unique_lines(self, filepath: str, new_lines: List[str]) -> int:
        """Append unique lines to file (like old_hunter.py). Returns count of new lines added."""
        existing = set()
        try:
            lines = read_lines(filepath)
            existing = set(lines)
        except Exception:
            pass
        added = 0
        try:
            with open(filepath, "a", encoding="utf-8") as f:
                for line in new_lines:
                    if line and line not in existing:
                        f.write(line + "\n")
                        existing.add(line)
                        added += 1
        except Exception:
            pass
        return added

    def _save_to_files(self, gold: List[HunterBenchResult], silver: List[HunterBenchResult]):
        """Save tiered configs to files (append unique, building pool across cycles)."""
        gold_file = str(self.config.get("gold_file", "") or "")
        silver_file = str(self.config.get("silver_file", "") or "")
        try:
            if gold_file:
                added = self._append_unique_lines(gold_file, [r.uri for r in gold])
                if added:
                    self.logger.info(f"Gold file: {added} new configs appended")
        except Exception:
            pass
        try:
            if silver_file:
                added = self._append_unique_lines(silver_file, [r.uri for r in silver])
                if added:
                    self.logger.info(f"Silver file: {added} new configs appended")
        except Exception:
            pass

    def _compute_adaptive_sleep(self) -> int:
        """Compute adaptive sleep based on cycle results.
        
        - Few/no validated configs → retry sooner (120s)
        - Consecutive scrape failures → back off gradually (up to 600s)
        - Good results → normal interval
        """
        base_sleep = self.config.get("sleep_seconds", 300)
        
        if self._consecutive_scrape_failures >= 3:
            # Network is down, back off to avoid hammering
            sleep = min(base_sleep * 2, 600)
            self.logger.info(f"[Adaptive] Network issues (failures={self._consecutive_scrape_failures}), sleep={sleep}s")
            return sleep
        
        if self._last_validated_count == 0:
            # No configs validated - retry sooner
            sleep = max(120, base_sleep // 3)
            self.logger.info(f"[Adaptive] No validated configs, retry sooner: sleep={sleep}s")
            return sleep
        
        if self._last_validated_count < 5:
            # Very few configs - retry moderately soon
            sleep = max(150, base_sleep // 2)
            self.logger.info(f"[Adaptive] Few configs ({self._last_validated_count}), sleep={sleep}s")
            return sleep
        
        self.logger.info(f"[Adaptive] Good cycle ({self._last_validated_count} configs), sleep={base_sleep}s")
        return base_sleep

    async def run_autonomous_loop(self):
        """Main autonomous loop with adaptive sleep."""
        # Initial cycle (wrapped in try/except so first-cycle crash doesn't kill the service)
        try:
            await self.run_cycle()
        except Exception as e:
            self.logger.error(f"Initial cycle failed: {e}")
            import traceback
            self.logger.debug(traceback.format_exc())

        while True:
            try:
                sleep_seconds = self._compute_adaptive_sleep()
                self.ui.status.update(phase="sleeping", next_in=sleep_seconds)
                if self.dashboard:
                    self.dashboard.update(phase="sleeping", next_in=sleep_seconds)
                
                # Countdown sleep so status bar shows remaining time
                for remaining in range(sleep_seconds, 0, -5):
                    self.ui.status.update(next_in=remaining)
                    if self.dashboard:
                        self.dashboard.update(next_in=remaining)
                        if remaining % 30 == 0:
                            self._sync_dashboard_balancer()
                    await asyncio.sleep(min(5, remaining))

                # Check if enough time has passed
                if time.time() - self.last_cycle >= sleep_seconds:
                    await self.run_cycle()

                # Periodic status report
                if self.cycle_count % 10 == 0:
                    await self.report_status()

            except asyncio.CancelledError:
                break
            except Exception as e:
                self.logger.error(f"Error in autonomous loop: {e}")
                backoff = min(60 * (self._consecutive_scrape_failures + 1), 300)
                self.ui.status.update(phase="sleeping", next_in=backoff)
                if self.dashboard:
                    self.dashboard.update(phase="sleeping", next_in=backoff)
                await asyncio.sleep(backoff)

    async def start(self):
        """Start the orchestrator using ThreadManager.
        
        Each major operation runs in its own dedicated thread:
          - config_scanner:      discovers & validates configs every 30 min
          - telegram_publisher:  publishes configs to Telegram every 30 min
          - balancer:            keeps load-balancer alive on 10808 / 10809
          - health_monitor:      watches RAM / CPU and throttles workers
        """
        # Start real-time dashboard (replaces old status bar + verbose console logs)
        if self.dashboard is not None:
            self.dashboard.start()
            self.ui.dashboard_active = True  # suppress old progress bars
            # Attach dashboard log handler to root logger so events panel gets all logs
            root = logging.getLogger()
            root.addHandler(self.dashboard.log_handler)
            # Remove console StreamHandlers to stop scrolling log spam
            for h in list(root.handlers):
                if isinstance(h, logging.StreamHandler) and not isinstance(h, logging.FileHandler):
                    if h.stream in (sys.stderr, sys.stdout):
                        root.removeHandler(h)
        else:
            # Fallback to old status bar if dashboard unavailable
            self.ui.status.start()

        # Create and start ThreadManager — all tasks run in dedicated threads
        try:
            from hunter.core.thread_manager import ThreadManager
        except ImportError:
            from core.thread_manager import ThreadManager

        self.thread_manager = ThreadManager(self)
        self.thread_manager.start_all()
        self.logger.info(
            f"ThreadManager started — threads: "
            f"{', '.join(w.name for w in self.thread_manager._workers)}"
        )

        # Keep main coroutine alive; sync dashboard periodically
        try:
            while self.thread_manager.is_running():
                # Update dashboard with thread status
                if self.dashboard:
                    try:
                        status = self.thread_manager.get_status()
                        hw = status.get("hardware", {})
                        self.dashboard.update(
                            memory_pct=hw.get("ram_percent", 0),
                            mode=hw.get("mode", "?"),
                        )
                        self._sync_dashboard_balancer()
                    except Exception:
                        pass
                await asyncio.sleep(5)
        except asyncio.CancelledError:
            pass

    async def stop(self):
        """Stop the orchestrator and all managed threads."""
        # Stop ThreadManager first (stops scanner, publisher, balancer, health threads)
        if hasattr(self, 'thread_manager') and self.thread_manager:
            try:
                self.thread_manager.stop_all(timeout=15)
                self.logger.info("ThreadManager stopped all workers")
            except Exception as exc:
                self.logger.warning(f"ThreadManager stop error: {exc}")

        # Stop dashboard / status bar
        if self.dashboard:
            self.dashboard.stop()
        self.ui.status.stop()
        
        # Stop DPI evasion orchestrator
        if self.dpi_evasion:
            try:
                self.dpi_evasion.stop()
                self.logger.info("DPI Evasion Orchestrator stopped")
            except Exception:
                pass
        
        # Stop stealth engine
        if self.stealth_engine:
            self.stealth_engine.stop()
        if self._tg_publish_task is not None:
            try:
                self._tg_publish_task.cancel()
            except Exception:
                pass
            self._tg_publish_task = None
        if self.balancer:
            self.balancer.stop()
        if self.gemini_balancer:
            self.gemini_balancer.stop()
        if self.telegram_scraper:
            try:
                # Cancel all pending asyncio tasks to prevent GeneratorExit errors
                try:
                    tasks = [t for t in asyncio.all_tasks() 
                             if t is not asyncio.current_task() and not t.done()]
                    if tasks:
                        for task in tasks:
                            task.cancel()
                        # Wait briefly for tasks to acknowledge cancellation
                        await asyncio.wait(tasks, timeout=1.0, return_when=asyncio.ALL_COMPLETED)
                except Exception:
                    pass
                await asyncio.wait_for(self.telegram_scraper.disconnect(), timeout=5.0)
            except asyncio.TimeoutError:
                self.logger.debug("Telegram scraper disconnect timed out")
            except Exception as e:
                self.logger.debug(f"Telegram scraper disconnect error: {e}")

    def get_stealth_metrics(self) -> Dict[str, Any]:
        """Get stealth obfuscation metrics."""
        if self.stealth_engine is None:
            return {"enabled": False}
        return self.stealth_engine.get_stealth_metrics()
    
    def get_dpi_evasion_metrics(self) -> Dict[str, Any]:
        """Get comprehensive DPI evasion metrics from all 2026 modules."""
        if self.dpi_evasion:
            return self.dpi_evasion.get_all_metrics()
        return {"enabled": False}
    
    def get_dpi_evasion_status(self) -> str:
        """Get human-readable DPI evasion status."""
        if self.dpi_evasion:
            return self.dpi_evasion.get_status_summary()
        return "DPI Evasion: disabled"

    async def manual_telegram_fetch(self) -> Dict[str, Any]:
        """Manually trigger Telegram config scraping."""
        if self.telegram_scraper is None:
            return {"ok": False, "error": "Telegram scraper not available"}
        try:
            channels = self.config.get("targets", [])
            limit = self.config.get("telegram_limit", 50)
            self.logger.info(f"[Manual] Fetching configs from Telegram ({len(channels)} channels, limit={limit})...")
            if self.flexible_fetcher is not None:
                result = await self.flexible_fetcher.fetch_telegram_with_retry(
                    channels=channels,
                    limit=limit,
                    max_retries=1,
                    retry_delay=0.0,
                )
                configs = list(result.configs)
            else:
                configs = await asyncio.wait_for(
                    self.telegram_scraper.scrape_configs(channels, limit=limit),
                    timeout=180
                )
            if isinstance(configs, set):
                configs = list(configs)
            self.logger.info(f"[Manual] Fetched {len(configs)} configs from Telegram")
            # Save to cache
            if configs and self.cache is not None:
                try:
                    self.cache.save_configs(configs, working=False)
                except Exception:
                    pass
            return {"ok": True, "count": len(configs), "configs": configs[:20]}
        except asyncio.TimeoutError:
            return {"ok": False, "error": "Telegram fetch timed out"}
        except Exception as e:
            return {"ok": False, "error": str(e)}

    async def manual_rescan(self) -> Dict[str, Any]:
        """Manually trigger a full config scan cycle."""
        try:
            self.logger.info("[Manual] Re-scan triggered by user")
            ran = await self.run_cycle()
            if not ran:
                return {"ok": False, "error": "Cycle already running"}
            return {"ok": True, "message": f"Cycle #{self.cycle_count} completed", "validated": self._last_validated_count}
        except Exception as e:
            self.logger.error(f"[Manual] Re-scan failed: {e}")
            return {"ok": False, "error": str(e)}

    async def manual_publish_telegram(self) -> Dict[str, Any]:
        """Manually publish current configs to Telegram group."""
        reporters = [r for r in (self.bot_reporter, self.telegram_reporter) if r is not None]
        if not reporters:
            return {"ok": False, "error": "No Telegram reporter configured"}
        try:
            # Get current gold+silver from files
            gold_file = str(self.config.get("gold_file", "") or "")
            silver_file = str(self.config.get("silver_file", "") or "")
            all_uris = []
            for fpath in [gold_file, silver_file]:
                if fpath:
                    try:
                        all_uris.extend(read_lines(fpath))
                    except Exception:
                        pass
            if not all_uris:
                return {"ok": False, "error": "No configs to publish"}
            self.logger.info(f"[Manual] Publishing {len(all_uris)} configs to Telegram...")
            ok = bool(await self._publish_uris_to_targets(all_uris, force=True))
            if not ok:
                try:
                    for t in self._get_tg_publish_targets():
                        self._enqueue_telegram_outbox(gold_uris=all_uris, chat_id=t)
                except Exception:
                    pass
                return {"ok": False, "error": "Publish failed - queued to outbox", "count": len(all_uris)}
            return {"ok": True, "count": len(all_uris), "targets": len(self._get_tg_publish_targets())}
        except Exception as e:
            self.logger.error(f"[Manual] Publish failed: {e}")
            return {"ok": False, "error": str(e)}

    def test_config_access(self, uri: str, targets: List[str] = None) -> Dict[str, bool]:
        """Test which services a config can access.
        Returns dict of {service_name: reachable}.
        """
        if targets is None:
            targets = [
                ("telegram", "https://api.telegram.org/"),
                ("gemini", "https://gemini.google.com/"),
                ("windsurf", "https://codeium.com/"),
                ("aws", "https://aws.amazon.com/"),
            ]
        if self.balancer is None:
            return {name: False for name, _ in targets}
        # Parse and start a temp proxy for this config
        parsed = self.parser.parse(uri) if self.parser else None
        if not parsed:
            return {name: False for name, _ in targets}
        import subprocess, tempfile
        port = self.config.get("multiproxy_port", 10808) + 500 + (abs(hash(uri)) % 50)
        config = {
            "log": {"loglevel": "none"},
            "inbounds": [{"port": port, "listen": "127.0.0.1", "protocol": "socks", "settings": {"auth": "noauth", "udp": False}}],
            "outbounds": [parsed.outbound],
        }
        results = {name: False for name, _ in targets}
        xray_path = self.balancer.xray_path
        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix="access_test_", suffix=".json")
            os.close(fd)
            import json as _json
            with open(temp_path, "w", encoding="utf-8") as f:
                _json.dump(config, f, ensure_ascii=False)
            process = subprocess.Popen(
                [xray_path, "run", "-c", temp_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            import time as _time
            _time.sleep(1.0)
            if process.poll() is not None:
                return results
            curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
            null_out = "nul" if sys.platform == "win32" else "/dev/null"
            for name, url in targets:
                try:
                    r = subprocess.run(
                        [curl_cmd, "-x", f"socks5h://127.0.0.1:{port}", "-s",
                         "-o", null_out, "-w", "%{http_code}", "-m", "8", "-k", url],
                        capture_output=True, text=True, timeout=12,
                        creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
                    )
                    if r.returncode == 0:
                        try:
                            code = int(r.stdout.strip())
                            results[name] = code < 400 or code == 204
                        except ValueError:
                            pass
                except Exception:
                    pass
        except Exception:
            pass
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=1)
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
        return results

    def categorize_backends(self) -> Dict[str, List[Dict[str, Any]]]:
        """Categorize current backends by service access.
        Tests each backend against Telegram, Gemini, Windsurf, AWS.
        """
        categories = {
            "telegram": [],
            "gemini": [],
            "windsurf": [],
            "aws": [],
            "general": [],
        }
        # Gather backends from both balancers
        all_backends = []
        if self.balancer:
            all_backends.extend(self.balancer.get_all_backends_detail())
        if self.gemini_balancer:
            for b in self.gemini_balancer.get_all_backends_detail():
                b["balancer"] = "gemini"
                all_backends.append(b)
        for b in all_backends:
            b.setdefault("balancer", "main")
            categories["general"].append(b)
        return categories

    def get_dns_status(self) -> Dict[str, Any]:
        """Get DNS manager status."""
        if self.dns_manager is None:
            return {"enabled": False, "error": "DNS manager not available"}
        return self.dns_manager.get_dns_status()

    def get_best_dns_server(self) -> Tuple[str, str]:
        """Get best DNS server for Iranian censorship bypass."""
        if self.dns_manager is None:
            return ("1.1.1.1", "Cloudflare (default fallback)")
        return self.dns_manager.get_best_dns_server()

    def test_dns_servers(self, max_tests: int = 5) -> Tuple[str, str]:
        """Test DNS servers and return the best one."""
        if self.dns_manager is None:
            return ("1.1.1.1", "Cloudflare (default fallback)")
        return self.dns_manager.auto_select_best_dns(max_tests)
