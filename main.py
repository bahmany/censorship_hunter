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
import gc
import logging
import sys
import os
import time
import warnings
from pathlib import Path
from colorama import Fore, Style, init

try:
    import psutil
except ImportError:
    psutil = None

# Initialize colorama for Windows
init(autoreset=True)

# Add current directory to path for imports
sys.path.insert(0, str(Path(__file__).parent))

try:
    from cryptography.utils import CryptographyDeprecationWarning

    warnings.filterwarnings(
        "ignore",
        message=r".*TripleDES.*",
        category=CryptographyDeprecationWarning,
    )
except Exception:
    pass

if sys.platform == "win32":
    try:
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    except Exception:
        pass

try:
    from hunter.orchestrator import HunterOrchestrator
    from hunter.core.config import HunterConfig
except ImportError:
    # Fallback for direct execution
    try:
        from orchestrator import HunterOrchestrator
        from core.config import HunterConfig
    except ImportError:
        print("Error: Required modules not found. Please run from the correct directory.")
        sys.exit(1)


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


class _NoisyNetworkLogFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        try:
            msg = record.getMessage()
            
            # Suppress connection read errors
            if "0 bytes read on a total of 8 expected bytes" in msg:
                return False
            if "[WinError 10053]" in msg and "connection was aborted" in msg.lower():
                return False
            
            # Suppress noisy Telethon reconnect/download messages
            if record.levelno <= logging.INFO:
                _NOISY_SUBSTRINGS = (
                    "Closing current connection to begin reconnect",
                    "Disconnecting borrowed sender for DC",
                    "Disconnecting from",
                    "Connecting to",
                    "Connection to",
                    "Disconnection from",
                    "Starting direct file download",
                    "File lives in another DC",
                    "Exporting auth for new borrowed sender",
                    "Automatic reconnection failed",
                    "Got a request to send a request to DC",
                )
                for sub in _NOISY_SUBSTRINGS:
                    if sub in msg:
                        return False
        except Exception:
            pass
        return True


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
    console_handler.addFilter(_NoisyNetworkLogFilter())
    
    # Get root logger and configure
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    logger.addHandler(console_handler)
    
    # Suppress noisy Telethon internal loggers at source
    logging.getLogger("telethon").setLevel(logging.WARNING)
    logging.getLogger("telethon.network").setLevel(logging.WARNING)
    logging.getLogger("telethon.client.downloads").setLevel(logging.WARNING)
    logging.getLogger("telethon.client.uploads").setLevel(logging.WARNING)


def print_banner():
    """Print startup banner"""
    print("=" * 60)
    print("  HUNTER - Advanced V2Ray Proxy Hunting System")
    print("  Autonomous censorship circumvention tool")
    print("=" * 60)


def check_and_cleanup_memory(logger):
    """Check memory usage at startup and force cleanup if needed."""
    if not psutil:
        logger.warning("psutil not available, skipping memory check")
        return
    
    mem = psutil.virtual_memory()
    mem_percent = mem.percent
    
    logger.info(f"Startup memory check: {mem_percent:.1f}% used ({mem.used / (1024**3):.1f}GB / {mem.total / (1024**3):.1f}GB)")
    
    # If memory is already high at startup, force aggressive cleanup
    if mem_percent >= 80:
        logger.warning(f"High memory at startup ({mem_percent:.1f}%), forcing aggressive cleanup...")
        
        # Multiple GC passes
        for i in range(3):
            gc.collect()
            time.sleep(0.5)
        
        # Check again
        mem_after = psutil.virtual_memory()
        logger.info(f"After cleanup: {mem_after.percent:.1f}% used")
        
        if mem_after.percent >= 85:
            logger.error(
                f"CRITICAL: Memory still at {mem_after.percent:.1f}% after cleanup. "
                f"Please close other applications or restart your system before running Hunter."
            )
            return False
    
    return True


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
        loop = asyncio.get_running_loop()

        def _loop_exception_handler(loop_obj, context):
            exc = context.get("exception")
            if isinstance(exc, ConnectionResetError):
                try:
                    if getattr(exc, "winerror", None) == 10054:
                        return
                except Exception:
                    pass
            loop_obj.default_exception_handler(context)

        loop.set_exception_handler(_loop_exception_handler)
    except Exception:
        pass

    try:
        # CRITICAL: Check and cleanup memory BEFORE starting
        if not check_and_cleanup_memory(logger):
            logger.error("Cannot start Hunter due to insufficient memory. Exiting.")
            return 1
        
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
