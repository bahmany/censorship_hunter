#!/usr/bin/env python3
"""
Example: Using Flexible Config Fetcher with Telegram Priority

This example demonstrates how to use the new flexible fetching system
with Telegram as the primary source and multiple fallback strategies.
"""

import asyncio
import logging

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


async def example_flexible_fetch():
    """
    Example: Fetch configs using the flexible fetcher with Telegram priority.
    """
    from hunter.core.config import HunterConfig
    from hunter.telegram.scraper import TelegramScraper
    from hunter.network.http_client import ConfigFetcher, HTTPClientManager
    from hunter.network.flexible_fetcher import FlexibleConfigFetcher, FetchStrategy
    from hunter.network.strategy_selector import AdaptiveStrategySelector, FetchPriority
    
    # Initialize components
    config = HunterConfig()
    
    # Initialize Telegram scraper
    telegram_scraper = None
    try:
        telegram_scraper = TelegramScraper(config)
        logger.info("Telegram scraper initialized")
    except Exception as e:
        logger.warning(f"Telegram scraper not available: {e}")
    
    # Initialize HTTP fetcher
    http_fetcher = None
    try:
        http_manager = HTTPClientManager()
        http_fetcher = ConfigFetcher(http_manager)
        logger.info("HTTP fetcher initialized")
    except Exception as e:
        logger.warning(f"HTTP fetcher not available: {e}")
    
    # Create flexible fetcher
    fetcher = FlexibleConfigFetcher(
        config=config,
        telegram_scraper=telegram_scraper,
        http_fetcher=http_fetcher,
        logger=logger
    )
    
    # Get channels from config
    channels = config.get("targets", [
        "v2rayngvpn", "mitivpn", "proxymtprotoir", "Porteqal3",
        "v2ray_configs_pool", "vmessorg", "V2rayNGn"
    ])
    
    proxy_ports = [10808, 10809]  # Your proxy ports
    
    # ============================================================
    # Strategy 1: Telegram First (Default)
    # ============================================================
    logger.info("=" * 60)
    logger.info("Strategy 1: Telegram First with HTTP Fallback")
    logger.info("=" * 60)
    
    result = await fetcher.fetch_with_flexible_strategy(
        channels=channels,
        proxy_ports=proxy_ports,
        strategy=FetchStrategy.TELEGRAM_PRIMARY,
        min_configs=100,
        telegram_limit=50,
        http_max_configs=200
    )
    
    logger.info(f"Success: {result['success']}")
    logger.info(f"Configs: {result['config_count']}")
    logger.info(f"Sources: {result['sources']}")
    logger.info(f"Duration: {result['duration_ms']:.0f}ms")
    
    # ============================================================
    # Strategy 2: Using Strategy Selector for Adaptive Fetching
    # ============================================================
    logger.info("\n" + "=" * 60)
    logger.info("Strategy 2: Adaptive Strategy Selection")
    logger.info("=" * 60)
    
    selector = AdaptiveStrategySelector(logger=logger)
    
    # Simulate some fetch history
    selector.record_fetch_result(True, "telegram", 45, 5000)
    selector.record_fetch_result(True, "github", 30, 8000)
    selector.record_fetch_result(False, "anti_censorship", 0, 0)
    
    # Get decision
    decision = selector.decide_strategy(
        available_channels=channels,
        memory_pressure=50.0  # Current memory usage
    )
    
    logger.info(f"Priority: {decision.priority.value}")
    logger.info(f"Telegram channels: {len(decision.telegram_channels)}")
    logger.info(f"HTTP sources: {decision.http_sources}")
    logger.info(f"Parallel mode: {decision.parallel_mode}")
    logger.info(f"Reason: {decision.reason}")
    
    # ============================================================
    # Get Metrics Summary
    # ============================================================
    logger.info("\n" + "=" * 60)
    logger.info("Source Metrics Summary")
    logger.info("=" * 60)
    
    metrics = fetcher.get_metrics_summary()
    for source, m in metrics.items():
        logger.info(f"\n{source}:")
        logger.info(f"  Success rate: {m['success_rate']:.1%}")
        logger.info(f"  Healthy: {m['is_healthy']}")
        logger.info(f"  Priority score: {m['priority_score']:.1f}")
        logger.info(f"  Total configs: {m['total_configs']}")
        logger.info(f"  Avg response: {m['avg_response_ms']:.0f}ms")
    
    return result


async def example_direct_telegram_fetch():
    """
    Example: Direct Telegram fetching with retry.
    """
    from hunter.core.config import HunterConfig
    from hunter.telegram.scraper import TelegramScraper
    from hunter.network.flexible_fetcher import FlexibleConfigFetcher
    
    config = HunterConfig()
    
    telegram_scraper = None
    try:
        telegram_scraper = TelegramScraper(config)
    except Exception as e:
        logger.error(f"Cannot initialize Telegram: {e}")
        return
    
    fetcher = FlexibleConfigFetcher(
        config=config,
        telegram_scraper=telegram_scraper
    )
    
    channels = ["v2rayngvpn", "mitivpn", "proxymtprotoir"]
    
    logger.info("Fetching from Telegram with 3 retries...")
    
    result = await fetcher.fetch_telegram_with_retry(
        channels=channels,
        limit=50,
        max_retries=3,
        retry_delay=2.0
    )
    
    if result.success:
        logger.info(f"Got {len(result.configs)} configs from Telegram")
        # Print first 5 configs
        for uri in list(result.configs)[:5]:
            logger.info(f"  - {uri[:80]}...")
    else:
        logger.error(f"Failed: {result.error}")
    
    return result


async def main():
    """Run all examples."""
    logger.info("\n" + "=" * 60)
    logger.info("Flexible Config Fetcher Examples")
    logger.info("=" * 60)
    
    try:
        # Run flexible fetch example
        await example_flexible_fetch()
    except Exception as e:
        logger.error(f"Flexible fetch example failed: {e}")
        import traceback
        traceback.print_exc()
    
    logger.info("\n" + "=" * 60)
    logger.info("Examples completed")
    logger.info("=" * 60)


if __name__ == "__main__":
    asyncio.run(main())
