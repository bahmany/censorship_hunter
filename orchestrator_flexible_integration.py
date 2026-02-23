"""
Orchestrator Flexible Fetching Integration

This module provides the integration between the HunterOrchestrator
and the FlexibleConfigFetcher for Telegram-first adaptive fetching.
"""

import asyncio
import logging
from typing import List, Optional, Any

try:
    from hunter.network.flexible_fetcher import FlexibleConfigFetcher, FetchStrategy, fetch_configs_flexible
    from hunter.network.strategy_selector import AdaptiveStrategySelector, FetchPriority, select_fetch_strategy
except ImportError:
    FlexibleConfigFetcher = None
    FetchStrategy = None
    AdaptiveStrategySelector = None
    FetchPriority = None
    select_fetch_strategy = None


class FlexibleFetchingMixin:
    """
    Mixin to add flexible fetching capabilities to HunterOrchestrator.
    
    This replaces the standard scrape_configs with an adaptive,
    Telegram-first fetching strategy.
    """
    
    def _init_flexible_fetcher(self):
        """Initialize flexible fetching components."""
        self.flexible_fetcher: Optional[FlexibleConfigFetcher] = None
        self.strategy_selector: Optional[AdaptiveStrategySelector] = None
        
        if FlexibleConfigFetcher is not None:
            try:
                self.flexible_fetcher = FlexibleConfigFetcher(
                    config=self.config,
                    telegram_scraper=self.telegram_scraper,
                    http_fetcher=self.config_fetcher,
                    logger=self.logger
                )
                self.logger.info("Flexible fetcher initialized")
            except Exception as e:
                self.logger.warning(f"Flexible fetcher init skipped: {e}")
        
        if AdaptiveStrategySelector is not None:
            try:
                self.strategy_selector = AdaptiveStrategySelector(logger=self.logger)
                self.logger.info("Strategy selector initialized")
            except Exception as e:
                self.logger.warning(f"Strategy selector init skipped: {e}")
    
    async def scrape_configs_flexible(self) -> List[str]:
        """
        Flexible config scraping with Telegram priority and adaptive fallbacks.
        
        This method replaces scrape_configs() with:
        1. Telegram-first fetching with aggressive retry
        2. Adaptive strategy selection based on network conditions
        3. Multiple HTTP source fallbacks
        4. Smart circuit breakers and metrics tracking
        """
        import gc as _gc
        
        # Get proxy ports
        proxy_ports = [self.config.get("multiproxy_port", 10808)]
        gemini_port = self.config.get("gemini_port", 10809)
        if gemini_port not in proxy_ports and self.gemini_balancer:
            proxy_ports.append(gemini_port)
        
        # Get channels
        telegram_channels = self.config.get("targets", [])
        
        # Memory-aware configuration
        max_total = self.config.get("max_total", 1000)
        per_source_cap = max(100, max_total // 3)
        
        all_configs: set = set()
        sources_used = []
        
        # Update dashboard
        if self.dashboard:
            self.dashboard.update(phase="scraping")
        
        with self.ui.scrape_progress(["Telegram", "HTTP-Fallback", "Cache"]) as tracker:
            self.ui.status.update(phase="scraping")
            
            # ============================================================
            # PHASE 1: Telegram First (Primary Strategy)
            # ============================================================
            tracker.update_source("Telegram", status="fetching")
            
            if self.flexible_fetcher and telegram_channels:
                try:
                    # Use flexible fetcher for Telegram with retry
                    telegram_limit = self.config.get("telegram_limit", 50)
                    if max_total <= 150:
                        telegram_limit = min(telegram_limit, 10)
                    elif max_total <= 400:
                        telegram_limit = min(telegram_limit, 25)
                    
                    self.logger.info(
                        f"[FlexibleFetch] Phase 1: Telegram with "
                        f"limit={telegram_limit}, channels={len(telegram_channels)}"
                    )
                    
                    telegram_result = await self.flexible_fetcher.fetch_telegram_with_retry(
                        channels=telegram_channels,
                        limit=telegram_limit,
                        max_retries=3,
                        retry_delay=2.0
                    )
                    
                    if telegram_result.success:
                        all_configs.update(telegram_result.configs)
                        sources_used.append("telegram")
                        tracker.source_done("Telegram", count=len(telegram_result.configs))
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="done", 
                                                        count=len(telegram_result.configs))
                        
                        self.logger.info(
                            f"[FlexibleFetch] Telegram success: "
                            f"{len(telegram_result.configs)} configs in "
                            f"{telegram_result.duration_ms:.0f}ms"
                        )
                        
                        # If we got enough configs, we can skip HTTP fallback
                        if len(all_configs) >= per_source_cap * 2:
                            self.logger.info(
                                f"[FlexibleFetch] Sufficient configs from Telegram, "
                                f"skipping HTTP fallback"
                            )
                            tracker.update_source("HTTP-Fallback", status="skipped")
                            tracker.source_done("HTTP-Fallback", count=0)
                        
                    else:
                        # Telegram failed, will fall back to HTTP
                        tracker.source_failed("Telegram", 
                                            reason=telegram_result.error or "failed")
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="failed")
                        self.logger.warning(
                            f"[FlexibleFetch] Telegram failed: {telegram_result.error}"
                        )
                        
                except Exception as e:
                    tracker.source_failed("Telegram", reason=str(e)[:40])
                    if self.dashboard:
                        self.dashboard.source_update("Telegram", status="failed")
                    self.logger.warning(f"[FlexibleFetch] Telegram exception: {e}")
            else:
                tracker.source_failed("Telegram", reason="no scraper")
                if self.dashboard:
                    self.dashboard.source_update("Telegram", status="failed")
            
            # ============================================================
            # PHASE 2: HTTP Fallback (if needed)
            # ============================================================
            min_configs_needed = per_source_cap
            
            if len(all_configs) < min_configs_needed and self.flexible_fetcher:
                tracker.update_source("HTTP-Fallback", status="fetching")
                
                try:
                    self.logger.info(
                        f"[FlexibleFetch] Phase 2: HTTP fallback "
                        f"(have {len(all_configs)}, need {min_configs_needed})"
                    )
                    
                    http_results = await self.flexible_fetcher.fetch_http_sources_parallel(
                        proxy_ports=proxy_ports,
                        max_configs=per_source_cap,
                        timeout_per_source=15.0,
                        max_workers=10
                    )
                    
                    http_success_count = 0
                    for result in http_results:
                        if result.success:
                            all_configs.update(result.configs)
                            if result.source not in sources_used:
                                sources_used.append(result.source)
                            http_success_count += 1
                    
                    tracker.source_done("HTTP-Fallback", count=len(all_configs))
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
            
            # ============================================================
            # PHASE 3: Cache (always load as backup)
            # ============================================================
            cache_configs = []
            if self.cache is not None:
                try:
                    tracker.update_source("Cache", status="loading")
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
            
            # Aggressive GC if memory is critical
            try:
                import psutil as _ps
                if _ps.virtual_memory().percent >= 90:
                    _gc.collect()
                    self.logger.info("[Memory] GC after scrape phase")
            except Exception:
                pass
            
            # ============================================================
            # PHASE 4: Load balancer cache as last resort
            # ============================================================
            balancer_uris = []
            try:
                bal_cache = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
                balancer_uris = [uri for uri, _ in bal_cache]
            except Exception:
                pass
        
        self.logger.info(
            f"[FlexibleFetch] Total raw configs after fetch: {len(all_configs)} "
            f"from sources: {sources_used}"
        )
        
        # Handle complete fetch failure
        if len(all_configs) == 0:
            self._consecutive_scrape_failures += 1
            self.logger.warning(
                f"[FlexibleFetch] All online sources failed (streak: {self._consecutive_scrape_failures}). "
                f"Using cached configs as primary source."
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
        
        self.logger.info(f"[FlexibleFetch] Final config count: {len(configs_list)}")
        return configs_list
    
    def get_fetching_metrics(self) -> dict:
        """Get metrics from the flexible fetcher."""
        if self.flexible_fetcher:
            return {
                'source_metrics': self.flexible_fetcher.get_metrics_summary(),
                'strategy_selector': self.strategy_selector.get_stats() if self.strategy_selector else None
            }
        return {}


def patch_orchestrator_with_flexible_fetching(orchestrator_class):
    """
    Patch the HunterOrchestrator class with flexible fetching capabilities.
    
    Usage:
        from hunter.orchestrator import HunterOrchestrator
        from hunter.orchestrator_flexible_integration import patch_orchestrator_with_flexible_fetching
        
        HunterOrchestrator = patch_orchestrator_with_flexible_fetching(HunterOrchestrator)
    """
    original_init = orchestrator_class.__init__
    
    def patched_init(self, config):
        original_init(self, config)
        # Initialize flexible fetching
        mixin = FlexibleFetchingMixin()
        mixin.config = self.config
        mixin.telegram_scraper = getattr(self, 'telegram_scraper', None)
        mixin.config_fetcher = getattr(self, 'config_fetcher', None)
        mixin.logger = getattr(self, 'logger', logging.getLogger(__name__))
        mixin.dashboard = getattr(self, 'dashboard', None)
        mixin.ui = getattr(self, 'ui', None)
        mixin.cache = getattr(self, 'cache', None)
        mixin.gemini_balancer = getattr(self, 'gemini_balancer', None)
        mixin._consecutive_scrape_failures = 0
        mixin._load_balancer_cache = self._load_balancer_cache
        
        mixin._init_flexible_fetcher()
        
        # Attach to self
        self.flexible_fetcher = mixin.flexible_fetcher
        self.strategy_selector = mixin.strategy_selector
        self.scrape_configs_flexible = lambda: mixin.scrape_configs_flexible()
        self.get_fetching_metrics = lambda: mixin.get_fetching_metrics()
    
    orchestrator_class.__init__ = patched_init
    return orchestrator_class
