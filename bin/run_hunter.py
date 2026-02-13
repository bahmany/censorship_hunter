#!/usr/bin/env python3
"""
Hunter Entry Point - Autonomous V2Ray Proxy Hunting System

This script orchestrates the full hunter workflow:
1. Loads configuration and secrets
2. Initializes components (Telegram scraper, benchmarker, load balancer, etc.)
3. Runs the autonomous harvesting and validation cycle
4. Manages gateway and proxy servers

Usage:
    python run_hunter.py [options]

Options:
    --config FILE    Path to hunter secrets config file
    --help           Show this help message
"""

import asyncio
import logging
import os
import signal
import sys
from typing import Any, Dict, List, Optional

from hunter.core.config import HunterConfig
from hunter.core.utils import setup_logging

# Global components
_orchestrator: Optional[HunterOrchestrator] = None


def init_components(config: HunterConfig):
    """Initialize the hunter orchestrator."""
    global _orchestrator
    
    # Setup logging
    setup_logging()
    
    # Create orchestrator
    _orchestrator = HunterOrchestrator(config)


async def run_autonomous_hunter():
    """Run the autonomous hunter using the orchestrator."""
    if not _orchestrator:
        logger.error("Orchestrator not initialized")
        return
    
    logger.info("Starting Hunter autonomous mode...")
    
    try:
        await _orchestrator.start()
    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    except Exception as e:
        logger.error(f"Error in autonomous hunter: {e}")
    finally:
        await _orchestrator.stop()


def main():
    """Main entry point."""
    # Parse arguments
    config_file = None
    if len(sys.argv) > 1:
        if sys.argv[1] in ("--help", "-h"):
            print(__doc__)
            return
        elif sys.argv[1] == "--config" and len(sys.argv) > 2:
            config_file = sys.argv[2]

    # Load configuration
    _config = HunterConfig(config_file)

    # Validate config
    errors = _config.validate()
    if errors:
        print("Configuration errors:")
        for error in errors:
            print(f"  - {error}")
        return 1

    # Initialize components
    init_components(_config)

    # Setup signal handlers
    def signal_handler(signum, frame):
        _logger.info(f"Received signal {signum}, shutting down...")
        if _balancer:
            _balancer.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Run the autonomous hunter
    try:
        asyncio.run(run_autonomous_hunter())
    except Exception as e:
        _logger.error(f"Fatal error: {e}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
