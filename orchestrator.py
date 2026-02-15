"""
Hunter orchestrator - Main autonomous hunting workflow.

This module coordinates the full hunter workflow including
scraping, benchmarking, caching, and load balancing.
"""

import asyncio
import logging
import os
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
    from hunter.telegram.scraper import TelegramScraper, TelegramReporter
    from hunter.config.cache import SmartCache
    from hunter.security.stealth_obfuscation import StealthObfuscationEngine, ObfuscationConfig, ProxyStealthWrapper
    from hunter.security.dpi_evasion_orchestrator import DPIEvasionOrchestrator
    from hunter.core.utils import load_json, now_ts, save_json, write_lines
    from hunter.telegram_config_reporter import ConfigReportingService
    from hunter.config.dns_servers import DNSManager, get_dns_config
except ImportError:
    # Fallback for direct execution
    try:
        from core.config import HunterConfig
        from core.models import HunterBenchResult
        from network.http_client import HTTPClientManager, ConfigFetcher
        from parsers import UniversalParser
        from testing.benchmark import ProxyBenchmark
        from proxy.load_balancer import MultiProxyServer
        from telegram.scraper import TelegramScraper, TelegramReporter
        from config.cache import SmartCache
        from security.stealth_obfuscation import StealthObfuscationEngine, ObfuscationConfig, ProxyStealthWrapper
        from security.dpi_evasion_orchestrator import DPIEvasionOrchestrator
        from core.utils import load_json, now_ts, save_json, write_lines
        from telegram_config_reporter import ConfigReportingService
        from config.dns_servers import DNSManager, get_dns_config
    except ImportError:
        # Final fallback - set to None if imports fail
        HunterConfig = None
        HunterBenchResult = None
        HTTPClientManager = None
        ConfigFetcher = None
        UniversalParser = None
        ProxyBenchmark = None
        MultiProxyServer = None
        TelegramScraper = None
        TelegramReporter = None
        SmartCache = None
        StealthObfuscationEngine = None
        ObfuscationConfig = None
        ProxyStealthWrapper = None
        DPIEvasionOrchestrator = None
        load_json = None
        now_ts = None
        save_json = None
        write_lines = None
        ConfigReportingService = None
        DNSManager = None
        get_dns_config = None


class HunterOrchestrator:
    """Main orchestrator for the autonomous hunter workflow."""

    def __init__(self, config: HunterConfig):
        self.config = config
        self.logger = logging.getLogger(__name__)

        # Initialize components
        self.http_manager = HTTPClientManager()
        self.config_fetcher = ConfigFetcher(self.http_manager)
        self.parser = UniversalParser()
        self.benchmarker = ProxyBenchmark(config.get("iran_fragment_enabled", False))
        self.cache = SmartCache()
        self.telegram_scraper = TelegramScraper(config)
        self.telegram_reporter = TelegramReporter(self.telegram_scraper)
        self.config_reporting_service = ConfigReportingService(self)

        # Initialize ADEE stealth obfuscation
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
        
        # Initialize DNS manager for Iranian censorship bypass
        self.dns_manager = DNSManager()
        
        # Load balancer
        obfuscation = self.stealth_engine
        self.balancer = MultiProxyServer(
            xray_path=config.get("xray_path", ""),
            port=config.get("multiproxy_port", 10808),
            num_backends=config.get("multiproxy_backends", 5),
            health_check_interval=config.get("multiproxy_health_interval", 60),
            obfuscation_engine=obfuscation
        )

        self.gemini_balancer: Optional[MultiProxyServer] = None
        if self.config.get("gemini_balancer_enabled", False):
            self.gemini_balancer = MultiProxyServer(
                xray_path=config.get("xray_path", ""),
                port=config.get("gemini_port", 10809),
                num_backends=config.get("multiproxy_backends", 5),
                health_check_interval=config.get("multiproxy_health_interval", 60),
                obfuscation_engine=obfuscation,
            )

        # State
        self.validated_configs: List[Tuple[str, float]] = []
        self.last_cycle = 0
        self.cycle_count = 0
        
        # Initialize 2026 DPI Evasion Orchestrator
        self.dpi_evasion: Optional[DPIEvasionOrchestrator] = None
        if config.get("dpi_evasion_enabled", True) and DPIEvasionOrchestrator is not None:
            try:
                self.dpi_evasion = DPIEvasionOrchestrator()
                self.dpi_evasion.start()
                self.logger.info(
                    f"DPI Evasion Orchestrator started: "
                    f"strategy={self.dpi_evasion.get_optimal_strategy().value}, "
                    f"network={self.dpi_evasion.state.network_type.value}"
                )
            except Exception as e:
                self.logger.warning(f"DPI Evasion Orchestrator init failed (non-fatal): {e}")
                self.dpi_evasion = None
        
        # Start stealth engine
        self.stealth_engine.start()

    async def scrape_configs(self) -> List[str]:
        """Scrape proxy configurations from ALL sources in parallel.
        
        Optimized for speed:
        - Telegram authentication runs in parallel with config fetching
        - All sources (Telegram, GitHub, anti-censorship, Iran priority) fetch simultaneously
        - Non-blocking operations for maximum throughput
        """
        configs = []
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        
        # Create tasks for parallel execution
        tasks = []
        
        # 1. Telegram configs - run in parallel with other sources
        telegram_channels = self.config.get("targets", [])
        if telegram_channels:
            telegram_limit = self.config.get("telegram_limit", 50)
            
            async def fetch_telegram():
                try:
                    telegram_configs = await self.telegram_scraper.scrape_configs(telegram_channels, limit=telegram_limit)
                    self.logger.info(f"Telegram sources: {len(telegram_configs)} configs")
                    # Ensure it's a list, not a set
                    if isinstance(telegram_configs, set):
                        return list(telegram_configs)
                    return telegram_configs if isinstance(telegram_configs, list) else []
                except Exception as e:
                    self.logger.warning(f"Telegram scrape failed: {e}")
                    return []
            
            tasks.append(fetch_telegram())
        
        # 2. GitHub configs - parallel fetch
        async def fetch_github():
            try:
                github_configs = self.config_fetcher.fetch_github_configs(proxy_ports)
                self.logger.info(f"GitHub sources: {len(github_configs)} configs")
                return list(github_configs)  # Convert set to list
            except Exception as e:
                self.logger.warning(f"GitHub fetch failed: {e}")
                return []
        
        tasks.append(fetch_github())
        
        # 3. Anti-censorship configs - parallel fetch
        async def fetch_anti_censorship():
            try:
                anti_censorship = self.config_fetcher.fetch_anti_censorship_configs(proxy_ports)
                self.logger.info(f"Anti-censorship sources: {len(anti_censorship)} configs")
                return list(anti_censorship)  # Convert set to list
            except Exception as e:
                self.logger.warning(f"Anti-censorship fetch failed: {e}")
                return []
        
        tasks.append(fetch_anti_censorship())
        
        # 4. Iran priority configs - parallel fetch
        async def fetch_iran_priority():
            try:
                iran_priority = self.config_fetcher.fetch_iran_priority_configs(proxy_ports)
                self.logger.info(f"Iran priority sources: {len(iran_priority)} configs")
                return list(iran_priority)  # Convert set to list
            except Exception as e:
                self.logger.warning(f"Iran priority fetch failed: {e}")
                return []
        
        tasks.append(fetch_iran_priority())
        
        # Execute all tasks in parallel
        self.logger.info(f"Fetching from {len(tasks)} sources in parallel...")
        results = await asyncio.gather(*tasks, return_exceptions=True)
        
        # Collect all configs with detailed logging
        for idx, result in enumerate(results):
            if isinstance(result, list):
                self.logger.debug(f"Task {idx} returned {len(result)} configs")
                configs.extend(result)
            elif isinstance(result, Exception):
                self.logger.warning(f"Task {idx} failed: {result}")
            else:
                self.logger.warning(f"Task {idx} returned unexpected type: {type(result)}")
        
        self.logger.info(f"Total raw configs after parallel fetch: {len(configs)}")

        # 5. Load cached working configs if total is still low
        if len(configs) < 500:
            cached = self.cache.load_cached_configs(max_count=500, working_only=True)
            if cached:
                configs.extend(cached)
                self.logger.info(f"Cached working configs: {len(cached)} added")

        return configs

    def validate_configs(self, configs: Iterable[str], max_workers: int = 50) -> List[HunterBenchResult]:
        """Validate and benchmark configurations with adaptive thread management."""
        import gc
        try:
            import psutil
        except ImportError:
            psutil = None
        
        from hunter.core.utils import prioritize_configs
        
        # CRITICAL: Check memory BEFORE starting benchmark
        if psutil:
            mem_percent = psutil.virtual_memory().percent
            if mem_percent >= 85:
                self.logger.error(
                    f"CANNOT START BENCHMARK: Memory already at {mem_percent:.1f}%. "
                    f"Forcing cleanup and reducing config count..."
                )
                gc.collect()
                import time
                time.sleep(1)
                
                mem_after = psutil.virtual_memory().percent
                if mem_after >= 85:
                    self.logger.error(
                        f"Memory still at {mem_after:.1f}% after cleanup. "
                        f"Skipping benchmark this cycle to prevent system crash."
                    )
                    return []
        
        results: List[HunterBenchResult] = []
        test_url = self.config.get("test_url", "https://www.cloudflare.com/cdn-cgi/trace")
        timeout = self.config.get("timeout_seconds", 10)

        configured_workers = self.config.get("max_workers")
        if isinstance(configured_workers, int) and configured_workers > 0:
            max_workers = configured_workers

        # Limit to max_total and deduplicate
        # CRITICAL: Dynamically adjust based on current memory
        base_max_total = self.config.get("max_total", 800)
        
        # Further reduce if memory is already elevated
        if psutil:
            mem_percent = psutil.virtual_memory().percent
            if mem_percent >= 80:
                max_total = min(base_max_total, 400)  # Cut in half if memory high
                self.logger.warning(f"Memory at {mem_percent:.1f}%, reducing max_total to {max_total}")
            elif mem_percent >= 70:
                max_total = min(base_max_total, 600)  # Reduce by 25%
                self.logger.info(f"Memory at {mem_percent:.1f}%, reducing max_total to {max_total}")
            else:
                max_total = base_max_total
        else:
            max_total = base_max_total
        
        deduped_configs = list(dict.fromkeys(list(configs)))[:max_total]
        
        # Check if we have any configs to validate
        if not deduped_configs:
            self.logger.warning("No configs to validate - returning empty results")
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
        self.benchmarker.start_thread_pool()
        
        # Parse configurations for batch benchmarking
        parsed_configs = []
        for uri in limited_configs:
            try:
                parsed = self.parser.parse(uri)
                if parsed:
                    parsed_configs.append(parsed)
            except Exception as e:
                self.logger.debug(f"Failed to parse {uri}: {e}")
        
        # Prepare configs for batch benchmarking
        benchmark_configs = []
        base_port = int(self.config.get("multiproxy_port", 10808)) + 1000
        
        for i, parsed in enumerate(parsed_configs):
            socks_port = base_port + i
            benchmark_configs.append((parsed, socks_port))
        
        # Use optimized batch benchmarking with adaptive thread management
        self.logger.info(f"Starting batch benchmark of {len(benchmark_configs)} configs with adaptive thread pool")
        results = self.benchmarker.benchmark_configs_batch(
            benchmark_configs, test_url, timeout
        )
        
        # Get performance metrics
        metrics = self.benchmarker.get_performance_metrics()
        self.logger.info(f"Thread pool performance: {metrics['thread_pool_metrics']['tasks_per_second']:.1f} tasks/sec")
        self.logger.info(f"CPU utilization: {metrics['thread_pool_metrics']['cpu_utilization']:.1f}%")
        self.logger.info(f"Memory utilization: {metrics['thread_pool_metrics']['memory_utilization']:.1f}%")
        
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
        """Update load balancer with new configurations."""
        # Apply stealth obfuscation to configs
        obfuscated_configs = []
        for uri, latency in configs:
            protocol = uri.split('://')[0] if '://' in uri else 'unknown'
            obfuscated_uri = self.stealth_engine.get_obfuscated_uri(uri, protocol)
            obfuscated_configs.append((obfuscated_uri, latency))
        
        self.balancer.update_available_configs(obfuscated_configs)

    async def update_gemini_balancer(self, configs: List[Tuple[str, float]]):
        """Update Gemini balancer with new configurations."""
        if not self.gemini_balancer:
            return
        self.gemini_balancer.update_available_configs(configs)

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
        items = payload.get("configs") if isinstance(payload, dict) else None
        if not isinstance(items, list):
            return []
        seed: List[Tuple[str, float]] = []
        for item in items:
            try:
                if isinstance(item, dict) and isinstance(item.get("uri"), str):
                    latency = float(item.get("latency_ms", 0.0))
                    seed.append((item["uri"], latency))
            except Exception:
                continue
        return seed

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
        status = self.balancer.get_status()
        await self.telegram_reporter.report_status(status)

    async def run_cycle(self):
        """Run one complete hunter cycle."""
        import gc
        try:
            import psutil
        except ImportError:
            psutil = None
        
        self.cycle_count += 1
        cycle_start = time.time()

        self.logger.info(f"Starting hunter cycle #{self.cycle_count}")
        
        # ============================================================
        # CRITICAL: Memory check and adaptive configuration at cycle start
        # ============================================================
        if psutil:
            mem = psutil.virtual_memory()
            mem_percent = mem.percent
            mem_gb_used = mem.used / (1024**3)
            mem_gb_total = mem.total / (1024**3)
            
            self.logger.info(
                f"Memory status: {mem_percent:.1f}% used "
                f"({mem_gb_used:.1f}GB / {mem_gb_total:.1f}GB)"
            )
            
            # Aggressive cleanup if memory is high
            if mem_percent >= 75:
                self.logger.warning(f"High memory detected ({mem_percent:.1f}%), forcing cleanup...")
                gc.collect()
                import time as time_module
                time_module.sleep(0.5)
                
                mem_after = psutil.virtual_memory()
                self.logger.info(f"After cleanup: {mem_after.percent:.1f}% used")
                mem_percent = mem_after.percent
            
            # Adaptive configuration based on memory pressure
            if mem_percent >= 85:
                self.logger.error(
                    f"CRITICAL MEMORY: {mem_percent:.1f}% - Skipping this cycle to prevent crash"
                )
                self.logger.info("Please close other applications or wait for memory to free up")
                return
            elif mem_percent >= 80:
                # Severe memory pressure - minimal operation
                adaptive_max_total = 200
                adaptive_max_workers = 5
                adaptive_scan_limit = 20
                self.logger.warning(
                    f"SEVERE memory pressure ({mem_percent:.1f}%) - "
                    f"Reducing to minimal: max_total={adaptive_max_total}, workers={adaptive_max_workers}"
                )
            elif mem_percent >= 70:
                # High memory pressure - reduced operation
                adaptive_max_total = 400
                adaptive_max_workers = 8
                adaptive_scan_limit = 30
                self.logger.warning(
                    f"HIGH memory pressure ({mem_percent:.1f}%) - "
                    f"Reducing: max_total={adaptive_max_total}, workers={adaptive_max_workers}"
                )
            elif mem_percent >= 60:
                # Moderate memory pressure - conservative operation
                adaptive_max_total = 600
                adaptive_max_workers = 10
                adaptive_scan_limit = 40
                self.logger.info(
                    f"MODERATE memory pressure ({mem_percent:.1f}%) - "
                    f"Conservative: max_total={adaptive_max_total}, workers={adaptive_max_workers}"
                )
            else:
                # Normal operation - use configured values
                adaptive_max_total = self.config.get("max_total", 800)
                adaptive_max_workers = self.config.get("max_workers", 10)
                adaptive_scan_limit = self.config.get("scan_limit", 50)
                self.logger.info(
                    f"NORMAL memory ({mem_percent:.1f}%) - "
                    f"Full operation: max_total={adaptive_max_total}, workers={adaptive_max_workers}"
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

        # Scrape configs
        raw_configs = await self.scrape_configs()
        self.logger.info(f"Total raw configs: {len(raw_configs)}")

        # Cache new configs
        self.cache.save_configs(raw_configs, working=False)

        # Validate configs
        validated = self.validate_configs(raw_configs)
        self.logger.info(f"Validated configs: {len(validated)}")

        # Save working configs to cache for future cycles
        if validated:
            working_uris = [r.uri for r in validated]
            self.cache.save_configs(working_uris, working=True)

        # Tier configs
        tiered = self.tier_configs(validated)
        gold_configs = tiered["gold"]
        silver_configs = tiered["silver"]

        self.logger.info(f"Gold tier: {len(gold_configs)}, Silver tier: {len(silver_configs)}")

        # Update balancer
        all_configs = [(r.uri, r.latency_ms) for r in gold_configs + silver_configs]
        await self.update_balancer(all_configs)
        self._save_balancer_cache(all_configs, name="HUNTER_balancer_cache.json")

        gemini_configs: List[Tuple[str, float]] = []
        if self.gemini_balancer:
            for r in gold_configs + silver_configs:
                ps = (r.ps or "").lower()
                if "gemini" in ps or "gmn" in ps:
                    gemini_configs.append((r.uri, r.latency_ms))
            await self.update_gemini_balancer(gemini_configs)
            self._save_balancer_cache(gemini_configs, name="HUNTER_gemini_balancer_cache.json")

        # Report to Telegram
        await self.telegram_reporter.report_gold_configs([
            {
                "ps": r.ps,
                "latency_ms": r.latency_ms,
                "region": r.region,
                "tier": r.tier
            } for r in gold_configs
        ])

        try:
            await self.telegram_reporter.report_config_files(
                gold_uris=[r.uri for r in gold_configs],
                gemini_uris=[uri for uri, _ in gemini_configs] if gemini_configs else None,
            )
        except Exception:
            pass

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

    def _save_to_files(self, gold: List[HunterBenchResult], silver: List[HunterBenchResult]):
        """Save tiered configs to files."""
        gold_file = str(self.config.get("gold_file", "") or "")
        silver_file = str(self.config.get("silver_file", "") or "")
        try:
            if gold_file:
                write_lines(gold_file, [r.uri for r in gold])
        except Exception:
            pass
        try:
            if silver_file:
                write_lines(silver_file, [r.uri for r in silver])
        except Exception:
            pass

    async def run_autonomous_loop(self):
        """Main autonomous loop."""
        sleep_seconds = self.config.get("sleep_seconds", 300)

        # Initial cycle
        await self.run_cycle()

        while True:
            try:
                await asyncio.sleep(sleep_seconds)

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
                await asyncio.sleep(60)  # Back off on error

    async def start(self):
        """Start the orchestrator."""
        # Start balancer
        seed = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
        self.balancer.start(initial_configs=seed if seed else None)

        if self.gemini_balancer:
            gemini_seed = self._load_balancer_cache(name="HUNTER_gemini_balancer_cache.json")
            self.gemini_balancer.start(initial_configs=gemini_seed if gemini_seed else None)

        # Start autonomous loop
        await self.run_autonomous_loop()

    async def stop(self):
        """Stop the orchestrator."""
        # Stop DPI evasion orchestrator
        if self.dpi_evasion:
            try:
                self.dpi_evasion.stop()
                self.logger.info("DPI Evasion Orchestrator stopped")
            except Exception:
                pass
        
        # Stop stealth engine
        self.stealth_engine.stop()
        self.balancer.stop()
        if self.gemini_balancer:
            self.gemini_balancer.stop()
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
