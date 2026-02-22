"""
Hunter orchestrator - Main autonomous hunting workflow.

This module coordinates the full hunter workflow including
scraping, benchmarking, caching, and load balancing.
"""

import asyncio
import logging
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from queue import SimpleQueue
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from tqdm import tqdm

try:
    from hunter.core.config import HunterConfig
    from hunter.core.models import HunterBenchResult
    from hunter.network.http_client import HTTPClientManager, ConfigFetcher
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
    from parsers import UniversalParser
    from testing.benchmark import ProxyBenchmark
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
        
        # DPI Evasion Orchestrator (with strict timeout)
        self.dpi_evasion: Optional[DPIEvasionOrchestrator] = None
        if config.get("dpi_evasion_enabled", True) and DPIEvasionOrchestrator is not None:
            try:
                import threading
                self.dpi_evasion = DPIEvasionOrchestrator()
                
                # Run start() in a thread with strict 5s timeout
                dpi_done = threading.Event()
                def _start_dpi():
                    try:
                        self.dpi_evasion.start()
                    except Exception:
                        pass
                    finally:
                        dpi_done.set()
                
                dpi_thread = threading.Thread(target=_start_dpi, daemon=True)
                dpi_thread.start()
                
                if dpi_done.wait(timeout=5.0):
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
                import threading as _threading
                stealth_done = _threading.Event()
                def _start_stealth():
                    try:
                        self.stealth_engine.start()
                    except Exception:
                        pass
                    finally:
                        stealth_done.set()
                
                stealth_thread = _threading.Thread(target=_start_stealth, daemon=True)
                stealth_thread.start()
                
                if stealth_done.wait(timeout=5.0):
                    self.logger.info("Stealth engine started")
                else:
                    self.logger.info("Stealth engine: init timed out, continuing without it")
            except Exception as e:
                self.logger.info(f"Stealth engine start skipped: {e}")
        
        # UI progress system
        self.ui = HunterUI()
        
        # Real-time dashboard (replaces verbose console logging)
        self.dashboard: Optional[Any] = None
        if HunterDashboard is not None:
            try:
                self.dashboard = HunterDashboard()
            except Exception:
                pass
        
        self.logger.info("Hunter Orchestrator initialized successfully")

    async def scrape_configs(self) -> List[str]:
        """Scrape proxy configurations from ALL sources in parallel.
        
        Memory-aware: caps per-source config count based on adaptive max_total
        so we never accumulate 80K+ configs in RAM at 95% memory.
        """
        import gc as _gc
        configs = []
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        gemini_port = self.config.get("gemini_port", 10809)
        if gemini_port not in proxy_ports and self.gemini_balancer:
            proxy_ports.append(gemini_port)
        
        # Memory-aware caps: each source gets a share of max_total
        max_total = self.config.get("max_total", 1000)
        per_source_cap = max(100, max_total // 3)  # e.g. 80→26, 400→133, 1000→333
        
        # Build source list for progress tracker
        source_names = []
        tasks = []
        task_names = []
        
        # 1. Telegram configs — reduce limit under memory pressure
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
        
        # 2-4. HTTP config fetches — pass per_source_cap as max_configs
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
                        result = await loop.run_in_executor(
                            None, lambda: m(proxy_ports, max_configs=cap)
                        )
                        return list(result)[:cap]
                    except Exception as e:
                        self.logger.info(f"{n} fetch skipped: {e}")
                        return []
                
                tasks.append(_fetch())
        
        # Add cache sources to progress
        source_names.extend(["Cache", "BalancerCache"])
        
        # Execute all online tasks in parallel with progress tracking
        if self.dashboard:
            self.dashboard.update(phase="scraping")
            for sn in source_names:
                self.dashboard.source_update(sn, status="waiting")
        with self.ui.scrape_progress(source_names) as tracker:
            self.ui.status.update(phase="scraping")
            
            results = await asyncio.gather(*tasks, return_exceptions=True)
            
            # Collect results and update progress per source
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
            
            # Aggressive GC after fetching if memory is critical
            try:
                import psutil as _ps
                if _ps.virtual_memory().percent >= 90:
                    _gc.collect()
                    self.logger.info("[Memory] GC after scrape phase")
            except Exception:
                pass
            
            # 5. Load cached working configs
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

            # 6. Load balancer cache as last-resort source
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

        self.logger.info(f"Total raw configs after parallel fetch: {len(configs)}")

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

        # Final cap: never return more than max_total * 2 (dedup happens later)
        if len(configs) > max_total * 2:
            import random as _rnd
            _rnd.shuffle(configs)
            configs = configs[:max_total * 2]
            self.logger.info(f"Capped raw configs to {len(configs)} (max_total={max_total})")

        self.logger.info(f"Total configs after cache merge: {len(configs)}")
        return configs

    def validate_configs(self, configs: Iterable[str], max_workers: int = 50) -> List[HunterBenchResult]:
        """Validate and benchmark configurations with adaptive thread management."""
        from hunter.core.utils import prioritize_configs
        
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
        from hunter.proxy.load_balancer import XRAY_SUPPORTED_SS_CIPHERS
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
        reporter = self.bot_reporter or self.telegram_reporter
        if reporter is None or self.balancer is None:
            return
        try:
            status = self.balancer.get_status()
            await reporter.report_status(status)
        except Exception as e:
            self.logger.info(f"Status report skipped: {e}")

    async def run_cycle(self):
        """Run one complete hunter cycle."""
        import gc
        try:
            import psutil
        except ImportError:
            psutil = None
        
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

        # Report to Telegram (Bot API primary, Telethon fallback)
        reporter = self.bot_reporter or self.telegram_reporter
        all_validated = gold_configs + silver_configs
        if reporter is not None and all_validated:
            try:
                await reporter.report_gold_configs([
                    {
                        "ps": r.ps,
                        "latency_ms": r.latency_ms,
                        "region": r.region,
                        "tier": r.tier
                    } for r in all_validated
                ])
            except Exception as e:
                self.logger.info(f"Config report skipped: {e}")

            try:
                await reporter.report_config_files(
                    gold_uris=[r.uri for r in all_validated],
                    gemini_uris=[uri for uri, _ in gemini_configs] if gemini_configs else None,
                )
            except Exception as e:
                self.logger.info(f"Config file report skipped: {e}")

        # Save to files
        self._save_to_files(gold_configs, silver_configs)

        cycle_time = time.time() - cycle_start
        self.logger.info(f"Cycle completed in {cycle_time:.1f} seconds")
        self.last_cycle = time.time()
        
        # Restore original config values
        if psutil and original_max_total is not None:
            self.config.set("max_total", original_max_total)
            self.config.set("max_workers", original_max_workers)
            self.config.set("scan_limit", original_scan_limit)
        
        # Final memory status
        if psutil:
            final_mem = psutil.virtual_memory()
            self.logger.info(
                f"Cycle end memory: {final_mem.percent:.1f}% "
                f"({final_mem.used / (1024**3):.1f}GB / {final_mem.total / (1024**3):.1f}GB)"
            )
        
        # Log DPI evasion status
        if self.dpi_evasion:
            try:
                self.logger.info(f"DPI Evasion: {self.dpi_evasion.get_status_summary()}")
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
        # Initial cycle
        await self.run_cycle()

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
        """Start the orchestrator."""
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
        
        # Start balancer
        if self.balancer is not None:
            seed = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
            self.balancer.start(initial_configs=seed if seed else None)
            self.ui.status.update(balancer=len(seed) if seed else 0)
            self._sync_dashboard_balancer()

        if self.gemini_balancer:
            gemini_seed = self._load_balancer_cache(name="HUNTER_gemini_balancer_cache.json")
            self.gemini_balancer.start(initial_configs=gemini_seed if gemini_seed else None)
            self._sync_dashboard_balancer()

        # Start autonomous loop
        await self.run_autonomous_loop()

    async def stop(self):
        """Stop the orchestrator."""
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
        if self.balancer:
            self.balancer.stop()
        if self.gemini_balancer:
            self.gemini_balancer.stop()
        if self.telegram_scraper:
            await self.telegram_scraper.disconnect()

    def get_stealth_metrics(self) -> Dict[str, Any]:
        """Get stealth obfuscation metrics."""
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

    def get_dns_status(self) -> Dict[str, Any]:
        """Get DNS manager status."""
        return self.dns_manager.get_dns_status()

    def get_best_dns_server(self) -> Tuple[str, str]:
        """Get best DNS server for Iranian censorship bypass."""
        return self.dns_manager.get_best_dns_server()

    def test_dns_servers(self, max_tests: int = 5) -> Tuple[str, str]:
        """Test DNS servers and return the best one."""
        return self.dns_manager.auto_select_best_dns(max_tests)
