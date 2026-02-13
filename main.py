#!/usr/bin/env python3
"""
Hunter - Standalone V2Ray Proxy Hunting System

A comprehensive proxy hunting and testing system designed for
circumventing internet censorship with advanced anti-DPI capabilities.

Usage:
    python -m hunter
    or
    python main.py
"""

import asyncio
import logging
import sys
import os
from pathlib import Path
from colorama import Fore, Style, init

# Initialize colorama for Windows
init(autoreset=True)

# Add current directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

from hunter.orchestrator import HunterOrchestrator
from hunter.core.config import HunterConfig


class ColoredFormatter(logging.Formatter):
    """Custom formatter with colors for different log levels."""
    
    COLORS = {
        'DEBUG': Fore.CYAN,
        'INFO': Fore.GREEN,
        'WARNING': Fore.YELLOW,
        'ERROR': Fore.RED,
        'CRITICAL': Fore.MAGENTA + Style.BRIGHT,
    }
    
    def format(self, record):
        # Add color to level name
        if record.levelname in self.COLORS:
            colored_levelname = f"{self.COLORS[record.levelname]}{record.levelname}{Style.RESET_ALL}"
            record.levelname = colored_levelname
        
        return super().format(record)


def setup_logging():
    """Setup logging configuration"""
    # Create console handler
    console_handler = logging.StreamHandler()
    console_handler.setLevel(logging.INFO)
    
    # Create colored formatter
    formatter = ColoredFormatter(
        '%(asctime)s | %(levelname)s | %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    console_handler.setFormatter(formatter)
    
    # Get root logger and configure
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    logger.addHandler(console_handler)


def print_banner():
    """Print startup banner"""
    print("=" * 60)
    print("  HUNTER - Advanced V2Ray Proxy Hunting System")
    print("  Autonomous censorship circumvention tool")
    print("=" * 60)


def print_config_info(config: HunterConfig):
    """Print configuration information"""
    print(f"Python {sys.version.split()[0]} on {sys.platform}")
    print(f"Iran Fragment: {'ENABLED' if config.get('iran_fragment_enabled') else 'DISABLED'}")
    print(f"Iran Priority Sources: {len(config.get('targets', []))} Reality-focused sources")
    print(f"ADEE: {'ENABLED' if config.get('adee_enabled') else 'DISABLED'}")
    print()


async def main():
    """Main entry point for the hunter system"""
    print_banner()
    setup_logging()

    logger = logging.getLogger(__name__)

    try:
        # Ensure runtime directory exists
        runtime_dir = Path(__file__).parent / "runtime"
        runtime_dir.mkdir(exist_ok=True)
        logger.info(f"Runtime directory: {runtime_dir}")
        
        # Load configuration with secrets file
        config = HunterConfig(secrets_file="hunter_secrets.env")
        print_config_info(config)

        # Validate configuration
        errors = config.validate()
        if errors:
            logger.error("Configuration errors found:")
            for error in errors:
                logger.error(f"  - {error}")
            logger.error("Please fix configuration issues and try again.")
            return 1

        # Create and start orchestrator
        logger.info("Initializing Hunter Orchestrator...")
        orchestrator = HunterOrchestrator(config)

        logger.info("Starting autonomous hunting loop...")
        await orchestrator.run_autonomous_loop()

    except KeyboardInterrupt:
        logger.info("Shutting down gracefully...")
        return 0
    except Exception as e:
        logger.error(f"Fatal error: {e}")
        import traceback
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    # Handle command line arguments
    if len(sys.argv) > 1:
        if sys.argv[1] in ['--help', '-h']:
            print(__doc__)
            sys.exit(0)
        elif sys.argv[1] in ['--version', '-v']:
            print("Hunter v2.0.0")
            sys.exit(0)

    # Run the main function
    try:
        exit_code = asyncio.run(main())
        sys.exit(exit_code)
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        sys.exit(0)
