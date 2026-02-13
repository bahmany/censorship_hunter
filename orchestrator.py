"""
Hunter orchestrator - Main autonomous hunting workflow.

This module coordinates the full hunter workflow including
scraping, benchmarking, caching, and load balancing.
"""

import asyncio
import logging
import time
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from tqdm import tqdm

from hunter.core.config import HunterConfig
from hunter.core.models import HunterBenchResult
from hunter.network.http_client import HTTPClientManager, ConfigFetcher
from hunter.parsers import UniversalParser
from hunter.testing.benchmark import ProxyBenchmark
from hunter.proxy.load_balancer import MultiProxyServer
from hunter.telegram.scraper import TelegramScraper, TelegramReporter
from hunter.config.cache import SmartCache
from hunter.security.obfuscation import StealthObfuscationEngine


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

        # Load balancer
        obfuscation = StealthObfuscationEngine(enabled=True)
        self.balancer = MultiProxyServer(
            xray_path=config.get("xray_path", ""),
            port=config.get("multiproxy_port", 10808),
            num_backends=config.get("multiproxy_backends", 5),
            health_check_interval=config.get("multiproxy_health_interval", 60),
            obfuscation_engine=obfuscation
        )

        # State
        self.validated_configs: List[Tuple[str, float]] = []
        self.last_cycle = 0
        self.cycle_count = 0

    async def scrape_configs(self) -> List[str]:
        """Scrape proxy configurations from all sources."""
        configs = []

        # Telegram configs - highest priority
        telegram_channels = self.config.get("targets", [])
        if telegram_channels:
            telegram_configs = await self.telegram_scraper.scrape_configs(telegram_channels)
            configs.extend(telegram_configs)
            self.logger.info(f"Telegram sources: {len(telegram_configs)} configs")

        # GitHub configs
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        github_configs = self.config_fetcher.fetch_github_configs(proxy_ports)
        configs.extend(github_configs)
        self.logger.info(f"GitHub sources: {len(github_configs)} configs")

        # Anti-censorship configs
        anti_censorship = self.config_fetcher.fetch_anti_censorship_configs(proxy_ports)
        configs.extend(anti_censorship)
        self.logger.info(f"Anti-censorship sources: {len(anti_censorship)} configs")

        # Iran priority configs
        iran_priority = self.config_fetcher.fetch_iran_priority_configs(proxy_ports)
        configs.extend(iran_priority)
        self.logger.info(f"Iran priority sources: {len(iran_priority)} configs")

        return configs

    def validate_configs(self, configs: Iterable[str], max_workers: int = 50) -> List[HunterBenchResult]:
        """Validate and benchmark configurations."""
        results = []
        test_url = self.config.get("test_url", "https://www.cloudflare.com/cdn-cgi/trace")
        timeout = self.config.get("timeout_seconds", 10)

        # Limit to max_total
        limited_configs = list(configs)[:self.config.get("max_total", 3000)]

        # Use tqdm for progress bar
        with tqdm(total=len(limited_configs), desc="Validating configs", unit="config") as pbar:
            # For simplicity, batch process (in real impl, use ThreadPoolExecutor)
            for uri in limited_configs:
                parsed = self.parser.parse(uri)
                if not parsed:
                    pbar.update(1)
                    continue

                latency = self.benchmarker.benchmark_config(
                    parsed,
                    self.config.get("multiproxy_port", 10808) + 1000,  # Use separate port
                    test_url,
                    timeout
                )
                if latency:
                    result = self.benchmarker.create_bench_result(parsed, latency)
                    results.append(result)

                pbar.update(1)

        # Sort by latency
        results.sort(key=lambda x: x.latency_ms)
        return results

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
        self.balancer.update_available_configs(configs)

    async def report_status(self):
        """Report current status to Telegram."""
        status = self.balancer.get_status()
        await self.telegram_reporter.report_status(status)

    async def run_cycle(self):
        """Run one complete hunter cycle."""
        self.cycle_count += 1
        cycle_start = time.time()

        self.logger.info(f"Starting hunter cycle #{self.cycle_count}")

        # Scrape configs
        raw_configs = await self.scrape_configs()
        self.logger.info(f"Total raw configs: {len(raw_configs)}")

        # Cache new configs
        self.cache.save_configs(raw_configs, working=False)

        # Validate configs
        validated = self.validate_configs(raw_configs)
        self.logger.info(f"Validated configs: {len(validated)}")

        # Tier configs
        tiered = self.tier_configs(validated)
        gold_configs = tiered["gold"]
        silver_configs = tiered["silver"]

        self.logger.info(f"Gold tier: {len(gold_configs)}, Silver tier: {len(silver_configs)}")

        # Update balancer
        all_configs = [(r.uri, r.latency_ms) for r in gold_configs + silver_configs]
        await self.update_balancer(all_configs)

        # Report to Telegram
        await self.telegram_reporter.report_gold_configs([
            {
                "ps": r.ps,
                "latency_ms": r.latency_ms,
                "region": r.region,
                "tier": r.tier
            } for r in gold_configs
        ])

        # Save to files
        self._save_to_files(gold_configs, silver_configs)

        cycle_time = time.time() - cycle_start
        self.logger.info(f"Cycle completed in {cycle_time:.1f} seconds")
        self.last_cycle = time.time()

    def _save_to_files(self, gold: List[HunterBenchResult], silver: List[HunterBenchResult]):
        """Save tiered configs to files."""
        # Implementation would write to gold.txt, silver.txt, etc.

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
        self.balancer.start()

        # Start autonomous loop
        await self.run_autonomous_loop()

    async def stop(self):
        """Stop the orchestrator."""
        self.balancer.stop()
        await self.telegram_scraper.disconnect()
