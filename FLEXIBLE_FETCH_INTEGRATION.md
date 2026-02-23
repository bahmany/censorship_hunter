"""
Quick Integration Guide: Enable Flexible Fetching in Hunter

This file shows how to modify the orchestrator to use flexible fetching.
Apply these changes to orchestrator.py to enable Telegram-first fetching.
"""

# =============================================================================
# STEP 1: Add imports at the top of orchestrator.py
# =============================================================================

# Add these imports after the existing imports (around line 50):

"""
try:
    from hunter.network.flexible_fetcher import FlexibleConfigFetcher, FetchStrategy
    from hunter.network.strategy_selector import AdaptiveStrategySelector, FetchPriority
except ImportError:
    FlexibleConfigFetcher = None
    FetchStrategy = None
    AdaptiveStrategySelector = None
    FetchPriority = None
"""

# =============================================================================
# STEP 2: Initialize flexible fetcher in __init__ (after other components)
# =============================================================================

# Add this code at the end of __init__ method (before the final log line):

"""
        # Flexible fetching (optional enhancement)
        self.flexible_fetcher = None
        self.strategy_selector = None
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
                self.logger.info(f"Flexible fetcher init skipped: {e}")
        
        if AdaptiveStrategySelector is not None:
            try:
                self.strategy_selector = AdaptiveStrategySelector(logger=self.logger)
                self.logger.info("Strategy selector initialized")
            except Exception as e:
                self.logger.info(f"Strategy selector init skipped: {e}")
"""

# =============================================================================
# STEP 3: Replace scrape_configs method
# =============================================================================

# Replace the entire scrape_configs method with this flexible version:

"""
    async def scrape_configs(self) -> List[str]:
        \"\"\"Scrape configs with Telegram priority and flexible fallbacks.\"\"\"
        import gc as _gc
        
        # Use flexible fetcher if available
        if self.flexible_fetcher is not None:
            return await self._scrape_configs_flexible()
        
        # Fall back to original implementation
        return await self._scrape_configs_original()
    
    async def _scrape_configs_flexible(self) -> List[str]:
        \"\"\"Flexible config scraping with Telegram priority.\"\"\"
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
            
            # PHASE 1: Telegram First
            tracker.update_source("Telegram", status="fetching")
            
            if telegram_channels:
                telegram_limit = self.config.get("telegram_limit", 50)
                if max_total <= 150:
                    telegram_limit = min(telegram_limit, 10)
                elif max_total <= 400:
                    telegram_limit = min(telegram_limit, 25)
                
                try:
                    result = await self.flexible_fetcher.fetch_telegram_with_retry(
                        channels=telegram_channels,
                        limit=telegram_limit,
                        max_retries=3
                    )
                    
                    if result.success:
                        all_configs.update(result.configs)
                        sources_used.append("telegram")
                        tracker.source_done("Telegram", count=len(result.configs))
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="done", 
                                                        count=len(result.configs))
                        
                        # Skip HTTP if we got enough
                        if len(all_configs) >= per_source_cap * 2:
                            tracker.update_source("HTTP-Fallback", status="skipped")
                            tracker.source_done("HTTP-Fallback", count=0)
                    else:
                        tracker.source_failed("Telegram", reason=result.error or "failed")
                        if self.dashboard:
                            self.dashboard.source_update("Telegram", status="failed")
                        
                except Exception as e:
                    tracker.source_failed("Telegram", reason=str(e)[:40])
                    if self.dashboard:
                        self.dashboard.source_update("Telegram", status="failed")
            else:
                tracker.source_failed("Telegram", reason="no channels")
            
            # PHASE 2: HTTP Fallback (if needed)
            if len(all_configs) < per_source_cap:
                tracker.update_source("HTTP-Fallback", status="fetching")
                
                try:
                    http_results = await self.flexible_fetcher.fetch_http_sources_parallel(
                        proxy_ports=proxy_ports,
                        max_configs=per_source_cap
                    )
                    
                    for result in http_results:
                        if result.success:
                            all_configs.update(result.configs)
                            if result.source not in sources_used:
                                sources_used.append(result.source)
                    
                    tracker.source_done("HTTP-Fallback", count=len(all_configs))
                    if self.dashboard:
                        self.dashboard.source_update("HTTP-Fallback", status="done")
                        
                except Exception as e:
                    tracker.source_failed("HTTP-Fallback", reason=str(e)[:40])
                    if self.dashboard:
                        self.dashboard.source_update("HTTP-Fallback", status="failed")
            else:
                tracker.source_done("HTTP-Fallback", count=0)
            
            # PHASE 3: Cache
            cache_configs = []
            tracker.update_source("Cache", status="loading")
            
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
                except Exception as e:
                    tracker.source_failed("Cache", reason=str(e)[:30])
                    if self.dashboard:
                        self.dashboard.source_update("Cache", status="failed")
            else:
                tracker.source_done("Cache", count=0)
            
            # Load balancer cache
            balancer_uris = []
            try:
                bal_cache = self._load_balancer_cache(name="HUNTER_balancer_cache.json")
                balancer_uris = [uri for uri, _ in bal_cache]
            except Exception:
                pass
        
        # Handle failure
        if len(all_configs) == 0:
            self._consecutive_scrape_failures += 1
            self.logger.warning(
                f"[FlexibleFetch] All sources failed (streak: {self._consecutive_scrape_failures}). "
                f"Using cache."
            )
            all_configs.update(cache_configs)
            all_configs.update(balancer_uris)
        else:
            self._consecutive_scrape_failures = 0
            if len(all_configs) < 500:
                all_configs.update(cache_configs)
        
        # Cap results
        configs_list = list(all_configs)
        if len(configs_list) > max_total * 2:
            import random as _rnd
            _rnd.shuffle(configs_list)
            configs_list = configs_list[:max_total * 2]
        
        self.logger.info(f"[FlexibleFetch] Total: {len(configs_list)} from {sources_used}")
        return configs_list
    
    async def _scrape_configs_original(self) -> List[str]:
        \"\"\"Original scrape_configs implementation (keep existing code).\"\"\"
        # Copy your existing scrape_configs code here
        pass
"""

# =============================================================================
# STEP 4: Add metrics method (optional)
# =============================================================================

# Add this method to the orchestrator class:

"""
    def get_fetching_metrics(self) -> dict:
        \"\"\"Get flexible fetching metrics.\"\"\"
        if self.flexible_fetcher:
            return {
                'sources': self.flexible_fetcher.get_metrics_summary(),
                'strategy': self.strategy_selector.get_stats() if self.strategy_selector else None
            }
        return {}
"""

# =============================================================================
# Environment Variable Control
# =============================================================================

# You can also add environment variable control to enable/disable flexible fetching:

"""
# In __init__, check environment variable:
use_flexible = os.getenv("HUNTER_FLEXIBLE_FETCH", "true").lower() == "true"
if use_flexible and FlexibleConfigFetcher is not None:
    # Initialize flexible fetcher
    ...
"""

# Then you can control it:
# export HUNTER_FLEXIBLE_FETCH=true   # Enable
# export HUNTER_FLEXIBLE_FETCH=false  # Disable (use original)
