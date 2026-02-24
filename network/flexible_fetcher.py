"""
Flexible Config Fetcher - Multi-strategy config acquisition with Telegram priority.

This module implements an adaptive, resilient config fetching system that:
1. Prioritizes Telegram as the primary source
2. Has multiple fallback strategies
3. Adapts to network conditions
4. Uses parallel fetching with circuit breakers
"""

import asyncio
import logging
import time
import random
from typing import List, Set, Dict, Any, Optional, Callable
from dataclasses import dataclass, field
from enum import Enum, auto
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading

# Import existing components
try:
    from hunter.telegram.scraper import TelegramScraper
    from hunter.network.http_client import ConfigFetcher
    from hunter.core.config import HunterConfig
except ImportError:
    TelegramScraper = None
    ConfigFetcher = None
    HunterConfig = None


class FetchStrategy(Enum):
    """Available fetching strategies."""
    TELEGRAM_PRIMARY = auto()      # Telegram first, then others
    TELEGRAM_ONLY = auto()         # Only Telegram
    HTTP_FALLBACK = auto()         # HTTP sources only
    PARALLEL_ALL = auto()          # All sources in parallel
    ADAPTIVE = auto()              # Smart selection based on history
    SSH_TUNNEL = auto()            # Via SSH tunnel only
    DIRECT = auto()                # Direct connection only


@dataclass
class FetchResult:
    """Result of a fetch operation."""
    source: str
    configs: Set[str] = field(default_factory=set)
    success: bool = False
    duration_ms: float = 0.0
    error: Optional[str] = None
    strategy_used: FetchStrategy = FetchStrategy.ADAPTIVE


@dataclass
class SourceMetrics:
    """Metrics for a config source."""
    source_name: str
    success_count: int = 0
    failure_count: int = 0
    last_success_time: float = 0.0
    last_failure_time: float = 0.0
    avg_configs_per_success: float = 0.0
    avg_response_time_ms: float = 0.0
    total_configs_fetched: int = 0
    consecutive_failures: int = 0
    
    @property
    def success_rate(self) -> float:
        total = self.success_count + self.failure_count
        if total == 0:
            return 0.5  # Neutral default
        return self.success_count / total
    
    @property
    def is_healthy(self) -> bool:
        return self.consecutive_failures < 3
    
    @property
    def priority_score(self) -> float:
        """Calculate priority score for source ranking."""
        if not self.is_healthy:
            return 0.0
        
        # Base score from success rate
        score = self.success_rate * 100
        
        # Bonus for recent success
        time_since_success = time.time() - self.last_success_time
        if time_since_success < 300:  # 5 minutes
            score += 20
        elif time_since_success < 900:  # 15 minutes
            score += 10
        
        # Bonus for high config yield
        score += min(self.avg_configs_per_success / 10, 20)
        
        return score


class FlexibleConfigFetcher:
    """
    Flexible config fetcher with Telegram priority and multiple fallback strategies.
    
    Features:
    - Telegram-first fetching with aggressive retry
    - Multiple parallel HTTP fallback sources
    - Adaptive strategy selection based on success history
    - Circuit breaker pattern for failing sources
    - Configurable timeouts and retry logic
    """
    
    def __init__(
        self,
        config: Any,
        telegram_scraper: Optional[TelegramScraper] = None,
        http_fetcher: Optional[ConfigFetcher] = None,
        logger: Optional[logging.Logger] = None
    ):
        self.config = config
        self.telegram_scraper = telegram_scraper
        self.http_fetcher = http_fetcher
        self.logger = logger or logging.getLogger(__name__)
        
        # Metrics tracking
        self._metrics: Dict[str, SourceMetrics] = {}
        self._metrics_lock = threading.Lock()
        
        # Circuit breakers
        self._circuit_breakers: Dict[str, Dict[str, Any]] = {}
        self._circuit_lock = threading.Lock()
        
        # Fetch history for adaptive strategy
        self._fetch_history: List[FetchResult] = []
        self._max_history_size = 50
        
        # Default configuration
        self._default_timeouts = {
            'telegram_connect': 20,
            'telegram_fetch': 60,
            'http_fetch': 12,
            'circuit_breaker_reset': 120,  # 2 minutes
            'circuit_breaker_threshold': 3,
        }
        
        # Channel prioritization (most reliable first)
        self._priority_channels = [
            # High-priority, reliable channels
            "v2rayngvpn",
            "mitivpn", 
            "proxymtprotoir",
            "Porteqal3",
            "v2ray_configs_pool",
            "VpnProSec",
            "proxy_mtm",
            # Secondary channels
            "vmessorg",
            "V2rayNGn",
            "v2ray_swhil",
            "VmessProtocol",
            "PrivateVPNs",
            # Additional channels
            "DirectVPN",
            "v2rayNG_Matsuri",
            "FalconPolV2rayNG",
            "ShadowSocks_s",
            "napsternetv_config",
            "VlessConfig",
            "iP_CF",
            "ConfigsHUB",
        ]
    
    def _get_or_create_metrics(self, source: str) -> SourceMetrics:
        """Get or create metrics for a source."""
        with self._metrics_lock:
            if source not in self._metrics:
                self._metrics[source] = SourceMetrics(source_name=source)
            return self._metrics[source]
    
    def _record_success(self, source: str, configs_count: int, duration_ms: float):
        """Record a successful fetch."""
        metrics = self._get_or_create_metrics(source)
        metrics.success_count += 1
        metrics.consecutive_failures = 0
        metrics.last_success_time = time.time()
        metrics.total_configs_fetched += configs_count
        
        # Update averages
        n = metrics.success_count
        metrics.avg_configs_per_success = (
            (metrics.avg_configs_per_success * (n - 1) + configs_count) / n
        )
        metrics.avg_response_time_ms = (
            (metrics.avg_response_time_ms * (n - 1) + duration_ms) / n
        )
        
        # Reset circuit breaker
        with self._circuit_lock:
            if source in self._circuit_breakers:
                del self._circuit_breakers[source]
    
    def _record_failure(self, source: str, error: str):
        """Record a failed fetch."""
        metrics = self._get_or_create_metrics(source)
        metrics.failure_count += 1
        metrics.consecutive_failures += 1
        metrics.last_failure_time = time.time()
        
        # Update circuit breaker
        with self._circuit_lock:
            if source not in self._circuit_breakers:
                self._circuit_breakers[source] = {
                    'failures': 0,
                    'last_failure': 0,
                    'open': False
                }
            cb = self._circuit_breakers[source]
            cb['failures'] += 1
            cb['last_failure'] = time.time()
            
            threshold = self._default_timeouts['circuit_breaker_threshold']
            if cb['failures'] >= threshold:
                cb['open'] = True
                self.logger.warning(f"Circuit breaker OPEN for {source}")
    
    def _is_circuit_open(self, source: str) -> bool:
        """Check if circuit breaker is open for a source."""
        with self._circuit_lock:
            if source not in self._circuit_breakers:
                return False
            
            cb = self._circuit_breakers[source]
            if not cb['open']:
                return False
            
            # Check if we should reset
            reset_time = self._default_timeouts['circuit_breaker_reset']
            if time.time() - cb['last_failure'] > reset_time:
                cb['open'] = False
                cb['failures'] = 0
                self.logger.info(f"Circuit breaker CLOSED for {source}")
                return False
            
            return True
    
    def _update_history(self, result: FetchResult):
        """Update fetch history."""
        self._fetch_history.append(result)
        if len(self._fetch_history) > self._max_history_size:
            self._fetch_history.pop(0)
    
    def get_source_rankings(self) -> List[tuple]:
        """Get sources ranked by priority score."""
        with self._metrics_lock:
            sources = list(self._metrics.items())
        
        # Sort by priority score descending
        ranked = [(name, metrics) for name, metrics in sources]
        ranked.sort(key=lambda x: x[1].priority_score, reverse=True)
        return ranked
    
    async def fetch_telegram_with_retry(
        self,
        channels: List[str],
        limit: int = 100,
        max_retries: int = 3,
        retry_delay: float = 2.0
    ) -> FetchResult:
        """
        Fetch configs from Telegram with aggressive retry logic.
        
        This is the PRIMARY fetching method - it will try very hard
        to get configs from Telegram before falling back.
        """
        if not self.telegram_scraper:
            return FetchResult(
                source="telegram",
                success=False,
                error="Telegram scraper not available",
                strategy_used=FetchStrategy.TELEGRAM_PRIMARY
            )
        
        start_time = time.time()
        all_configs: Set[str] = set()
        last_error = None
        
        # Sort channels by priority (if known)
        sorted_channels = sorted(
            channels,
            key=lambda c: self._priority_channels.index(c)
            if c in self._priority_channels else 999
        )

        for attempt in range(max_retries):
            try:
                self.logger.info(
                    f"[Telegram] Fetch attempt {attempt + 1}/{max_retries} "
                    f"from {len(sorted_channels)} channels (limit={limit})"
                )

                try:
                    deadline_s = float(os.getenv("HUNTER_TG_DEADLINE", "90"))
                except Exception:
                    deadline_s = 90.0
                deadline_s = max(15.0, min(240.0, deadline_s))
                outer_timeout = max(
                    float(self._default_timeouts.get('telegram_fetch', 60)) + 20.0,
                    deadline_s + 15.0,
                )
                configs = await asyncio.wait_for(
                    self.telegram_scraper.scrape_configs(sorted_channels, limit=limit),
                    timeout=outer_timeout,
                )

                if configs:
                    if isinstance(configs, set):
                        all_configs.update(configs)
                    elif isinstance(configs, list):
                        all_configs.update(configs)
                
                if all_configs:
                    duration_ms = (time.time() - start_time) * 1000
                    
                    self.logger.info(
                        f"[Telegram] SUCCESS: Got {len(all_configs)} configs "
                        f"in {duration_ms:.0f}ms"
                    )
                    
                    self._record_success("telegram", len(all_configs), duration_ms)
                    
                    result = FetchResult(
                        source="telegram",
                        configs=all_configs,
                        success=True,
                        duration_ms=duration_ms,
                        strategy_used=FetchStrategy.TELEGRAM_PRIMARY
                    )
                    self._update_history(result)
                    return result
                
                # No configs, try again
                last_error = "No configs found"
                
            except asyncio.TimeoutError:
                last_error = "Timeout"
                self.logger.warning(f"[Telegram] Attempt {attempt + 1} timed out")
                
            except Exception as e:
                last_error = str(e)
                self.logger.warning(f"[Telegram] Attempt {attempt + 1} failed: {e}")
            
            # Wait before retry with exponential backoff
            if attempt < max_retries - 1:
                delay = retry_delay * (2 ** attempt) + random.uniform(0, 1)
                self.logger.info(f"[Telegram] Waiting {delay:.1f}s before retry...")
                await asyncio.sleep(delay)
        
        # All retries exhausted
        duration_ms = (time.time() - start_time) * 1000
        self._record_failure("telegram", f"All retries failed: {last_error}")
        
        result = FetchResult(
            source="telegram",
            configs=all_configs,
            success=len(all_configs) > 0,
            duration_ms=duration_ms,
            error=f"Failed after {max_retries} attempts: {last_error}",
            strategy_used=FetchStrategy.TELEGRAM_PRIMARY
        )
        self._update_history(result)
        return result
    
    async def fetch_http_sources_parallel(
        self,
        proxy_ports: List[int],
        max_configs: int = 200,
        timeout_per_source: float = 15.0,
        max_workers: int = 10
    ) -> List[FetchResult]:
        """
        Fetch from HTTP sources in parallel with circuit breakers.
        
        Multiple fallback strategies:
        1. Direct connection (no proxy)
        2. Via SOCKS proxy
        3. Via HTTP proxy
        4. Curl fallback
        """
        if not self.http_fetcher:
            return []
        
        results = []
        
        # Define HTTP source categories
        source_methods = [
            ("github", self.http_fetcher.fetch_github_configs),
            ("anti_censorship", self.http_fetcher.fetch_anti_censorship_configs),
            ("iran_priority", self.http_fetcher.fetch_iran_priority_configs),
        ]
        
        async def fetch_single(name: str, method) -> FetchResult:
            start_time = time.time()
            
            if self._is_circuit_open(name):
                return FetchResult(
                    source=name,
                    success=False,
                    error="Circuit breaker open",
                    duration_ms=0,
                    strategy_used=FetchStrategy.HTTP_FALLBACK
                )
            
            try:
                # Run in thread pool
                loop = asyncio.get_running_loop()
                configs = await asyncio.wait_for(
                    loop.run_in_executor(
                        None,
                        lambda: method(proxy_ports, max_workers=max_workers, max_configs=max_configs)
                    ),
                    timeout=timeout_per_source
                )
                
                duration_ms = (time.time() - start_time) * 1000
                
                if configs:
                    count = len(configs) if isinstance(configs, (set, list)) else 0
                    self._record_success(name, count, duration_ms)
                    
                    return FetchResult(
                        source=name,
                        configs=configs if isinstance(configs, set) else set(configs),
                        success=True,
                        duration_ms=duration_ms,
                        strategy_used=FetchStrategy.HTTP_FALLBACK
                    )
                else:
                    self._record_failure(name, "No configs returned")
                    return FetchResult(
                        source=name,
                        success=False,
                        error="No configs returned",
                        duration_ms=duration_ms,
                        strategy_used=FetchStrategy.HTTP_FALLBACK
                    )
                    
            except asyncio.TimeoutError:
                duration_ms = (time.time() - start_time) * 1000
                self._record_failure(name, "Timeout")
                return FetchResult(
                    source=name,
                    success=False,
                    error="Timeout",
                    duration_ms=duration_ms,
                    strategy_used=FetchStrategy.HTTP_FALLBACK
                )
                
            except Exception as e:
                duration_ms = (time.time() - start_time) * 1000
                self._record_failure(name, str(e))
                return FetchResult(
                    source=name,
                    success=False,
                    error=str(e),
                    duration_ms=duration_ms,
                    strategy_used=FetchStrategy.HTTP_FALLBACK
                )
        
        # Fetch all in parallel
        tasks = [fetch_single(name, method) for name, method in source_methods]
        results = await asyncio.gather(*tasks, return_exceptions=True)
        
        # Handle exceptions in results
        processed_results = []
        for r in results:
            if isinstance(r, Exception):
                processed_results.append(FetchResult(
                    source="unknown",
                    success=False,
                    error=str(r),
                    strategy_used=FetchStrategy.HTTP_FALLBACK
                ))
            else:
                processed_results.append(r)
                self._update_history(r)
        
        return processed_results
    
    async def fetch_with_flexible_strategy(
        self,
        channels: List[str],
        proxy_ports: List[int],
        strategy: FetchStrategy = FetchStrategy.ADAPTIVE,
        min_configs: int = 50,
        telegram_limit: int = 100,
        http_max_configs: int = 200
    ) -> Dict[str, Any]:
        """
        Main entry point: Fetch configs using the specified strategy.
        
        Args:
            channels: Telegram channels to scrape
            proxy_ports: Local proxy ports for HTTP fetching
            strategy: Fetching strategy to use
            min_configs: Minimum configs needed (triggers fallback if not met)
            telegram_limit: Max configs per Telegram channel
            http_max_configs: Max configs from HTTP sources
        
        Returns:
            Dict with 'configs', 'sources', 'strategy', 'success'
        """
        start_time = time.time()
        all_configs: Set[str] = set()
        sources_used = []
        
        self.logger.info(
            f"[FlexibleFetcher] Starting with strategy={strategy.name}, "
            f"min_configs={min_configs}"
        )
        
        # Strategy 1: TELEGRAM_PRIMARY or ADAPTIVE (default)
        if strategy in (FetchStrategy.TELEGRAM_PRIMARY, FetchStrategy.ADAPTIVE, FetchStrategy.TELEGRAM_ONLY):
            telegram_result = await self.fetch_telegram_with_retry(
                channels=channels,
                limit=telegram_limit,
                max_retries=2
            )
            
            if telegram_result.success:
                all_configs.update(telegram_result.configs)
                sources_used.append("telegram")
                
                # If we got enough configs from Telegram, we're done
                if len(all_configs) >= min_configs and strategy != FetchStrategy.TELEGRAM_ONLY:
                    self.logger.info(
                        f"[FlexibleFetcher] Got {len(all_configs)} configs from Telegram, "
                        f"satisfied min_configs requirement"
                    )
                    return {
                        'configs': all_configs,
                        'sources': sources_used,
                        'strategy': strategy.name,
                        'success': True,
                        'duration_ms': (time.time() - start_time) * 1000,
                        'primary_source': 'telegram'
                    }
            
            # Telegram failed or didn't give enough, continue to fallback
            if strategy == FetchStrategy.TELEGRAM_ONLY:
                return {
                    'configs': all_configs,
                    'sources': sources_used,
                    'strategy': strategy.name,
                    'success': len(all_configs) > 0,
                    'duration_ms': (time.time() - start_time) * 1000,
                    'primary_source': 'telegram',
                    'error': telegram_result.error if not telegram_result.success else None
                }
        
        # Strategy 2: HTTP fallback (if Telegram failed or strategy requires it)
        if strategy in (FetchStrategy.TELEGRAM_PRIMARY, FetchStrategy.ADAPTIVE, 
                       FetchStrategy.HTTP_FALLBACK, FetchStrategy.PARALLEL_ALL):
            
            needed = min_configs - len(all_configs)
            if needed > 0 or strategy in (FetchStrategy.HTTP_FALLBACK, FetchStrategy.PARALLEL_ALL):
                self.logger.info(
                    f"[FlexibleFetcher] Falling back to HTTP sources "
                    f"(need {needed} more configs)"
                )
                
                http_results = await self.fetch_http_sources_parallel(
                    proxy_ports=proxy_ports,
                    max_configs=http_max_configs
                )
                
                for result in http_results:
                    if result.success:
                        all_configs.update(result.configs)
                        if result.source not in sources_used:
                            sources_used.append(result.source)
                
                # Check if we have enough now
                if len(all_configs) >= min_configs:
                    self.logger.info(
                        f"[FlexibleFetcher] Got {len(all_configs)} total configs "
                        f"from {len(sources_used)} sources"
                    )
                    return {
                        'configs': all_configs,
                        'sources': sources_used,
                        'strategy': strategy.name,
                        'success': True,
                        'duration_ms': (time.time() - start_time) * 1000,
                        'primary_source': sources_used[0] if sources_used else 'unknown'
                    }
        
        # Strategy 3: Cache fallback (last resort)
        if len(all_configs) < min_configs:
            self.logger.warning(
                f"[FlexibleFetcher] Only got {len(all_configs)} configs, "
                f"checking cache as last resort"
            )
            # Cache loading would be done by the orchestrator
        
        final_success = len(all_configs) > 0
        
        return {
            'configs': all_configs,
            'sources': sources_used,
            'strategy': strategy.name,
            'success': final_success,
            'duration_ms': (time.time() - start_time) * 1000,
            'primary_source': sources_used[0] if sources_used else 'unknown',
            'config_count': len(all_configs)
        }
    
    def get_metrics_summary(self) -> Dict[str, Any]:
        """Get summary of all source metrics."""
        with self._metrics_lock:
            return {
                name: {
                    'success_rate': m.success_rate,
                    'is_healthy': m.is_healthy,
                    'priority_score': m.priority_score,
                    'total_configs': m.total_configs_fetched,
                    'avg_response_ms': m.avg_response_time_ms,
                    'consecutive_failures': m.consecutive_failures
                }
                for name, m in self._metrics.items()
            }
    
    def reset_circuit_breakers(self):
        """Reset all circuit breakers (e.g., on new network connection)."""
        with self._circuit_lock:
            self._circuit_breakers.clear()
        self.logger.info("[FlexibleFetcher] All circuit breakers reset")


# Convenience function for quick usage
async def fetch_configs_flexible(
    config: Any,
    telegram_scraper: Optional[TelegramScraper] = None,
    http_fetcher: Optional[ConfigFetcher] = None,
    channels: Optional[List[str]] = None,
    proxy_ports: Optional[List[int]] = None,
    strategy: FetchStrategy = FetchStrategy.ADAPTIVE,
    min_configs: int = 50
) -> Dict[str, Any]:
    """
    Convenience function to fetch configs with flexible strategy.
    
    Example:
        result = await fetch_configs_flexible(
            config=my_config,
            telegram_scraper=my_scraper,
            http_fetcher=my_fetcher,
            channels=["v2rayngvpn", "mitivpn"],
            proxy_ports=[10808],
            strategy=FetchStrategy.TELEGRAM_PRIMARY,
            min_configs=100
        )
        
        if result['success']:
            configs = result['configs']
            print(f"Got {len(configs)} configs from {result['sources']}")
    """
    fetcher = FlexibleConfigFetcher(
        config=config,
        telegram_scraper=telegram_scraper,
        http_fetcher=http_fetcher
    )
    
    return await fetcher.fetch_with_flexible_strategy(
        channels=channels or [],
        proxy_ports=proxy_ports or [10808],
        strategy=strategy,
        min_configs=min_configs
    )
