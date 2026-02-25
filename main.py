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

import json
import re

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
    from hunter.ui.progress import HunterUI, ServiceManager
except ImportError:
    # Fallback for direct execution
    try:
        from orchestrator import HunterOrchestrator
        from core.config import HunterConfig
        from ui.progress import HunterUI, ServiceManager
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
            
            # Suppress noisy Telethon reconnect/download messages (INFO and WARNING)
            if record.levelno <= logging.WARNING:
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
                    "Attempt ",
                    "at connecting failed",
                    "Connection refused by destination",
                    "Automatic reconnection",
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
    
    # File handler: persist all logs even after dashboard removes console handlers
    try:
        runtime_dir = Path(__file__).parent / "runtime"
        runtime_dir.mkdir(exist_ok=True)
        file_handler = logging.FileHandler(
            str(runtime_dir / "hunter.log"), encoding="utf-8", mode="a"
        )
        file_handler.setLevel(logging.INFO)
        file_handler.setFormatter(logging.Formatter(
            '%(asctime)s | %(levelname)-7s | %(message)s',
            datefmt='%Y-%m-%d %H:%M:%S'
        ))
        file_handler.addFilter(_NoisyNetworkLogFilter())
        logger.addHandler(file_handler)
    except Exception:
        pass
    
    # Suppress noisy Telethon internal loggers at source
    logging.getLogger("telethon").setLevel(logging.WARNING)
    logging.getLogger("telethon.network").setLevel(logging.ERROR)
    logging.getLogger("telethon.network.mtproto").setLevel(logging.ERROR)
    logging.getLogger("telethon.network.connection").setLevel(logging.ERROR)
    logging.getLogger("telethon.client.downloads").setLevel(logging.WARNING)
    logging.getLogger("telethon.client.uploads").setLevel(logging.WARNING)


def print_banner():
    """Print startup banner using HunterUI."""
    ui = HunterUI()
    ui.print_banner()
    # Check tools in bin/
    bin_dir = str(Path(__file__).parent / "bin")
    ui.print_tools_check(bin_dir)


def check_and_cleanup_memory(logger):
    """Check memory usage at startup and force cleanup if needed."""
    if not psutil:
        logger.warning("psutil not available, skipping memory check")
        return True
    
    mem = psutil.virtual_memory()
    mem_percent = mem.percent
    
    logger.info(f"Startup memory check: {mem_percent:.1f}% used ({mem.used / (1024**3):.1f}GB / {mem.total / (1024**3):.1f}GB)")
    
    # Always force aggressive cleanup at startup if memory > 70% (silent)
    if mem_percent >= 70:
        # Multiple aggressive GC passes (silent)
        for i in range(5):
            gc.collect()
            time.sleep(0.3)
        
        # Check again
        mem_after = psutil.virtual_memory()
        mem_percent = mem_after.percent
    
    # Adaptive startup based on memory level (silent - no warnings)
    if mem_percent >= 95:
        logger.info(f"Starting in ULTRA-MINIMAL mode (memory: {mem_percent:.1f}%)")
    elif mem_percent >= 90:
        logger.info(f"Starting in MINIMAL mode (memory: {mem_percent:.1f}%)")
    elif mem_percent >= 85:
        logger.info(f"Starting in REDUCED mode (memory: {mem_percent:.1f}%)")
    elif mem_percent >= 80:
        logger.info(f"Starting in CONSERVATIVE mode (memory: {mem_percent:.1f}%)")
    else:
        logger.info(f"Starting in NORMAL mode (memory: {mem_percent:.1f}%)")
    
    # Always allow startup - let cycle-level adaptation handle it
    return True


def print_config_info(config: HunterConfig):
    """Print configuration information"""
    print(f"Python {sys.version.split()[0]} on {sys.platform}")
    print(f"Iran Fragment: {'ENABLED' if config.get('iran_fragment_enabled') else 'DISABLED'}")
    print(f"Iran Priority Sources: {len(config.get('targets', []))} Reality-focused sources")
    print(f"ADEE: {'ENABLED' if config.get('adee_enabled') else 'DISABLED'}")
    print()


def kill_existing_hunter_processes(logger):
    """Kill only processes occupying Hunter's balancer ports (10808, 10809).

    Other xray/v2ray instances (e.g. user-launched) are left untouched.
    """
    from core.utils import kill_process_on_port
    killed_count = 0

    for port in (10808, 10809):
        try:
            if kill_process_on_port(port):
                killed_count += 1
        except Exception:
            pass

    if killed_count > 0:
        logger.info(f"Killed {killed_count} process(es) on Hunter ports (10808/10809)")
        import time
        time.sleep(2)  # Wait for processes to fully terminate

    # Remove PID file if exists
    try:
        runtime_dir = Path(__file__).parent / "runtime"
        pid_file = runtime_dir / "hunter_service.pid"
        if pid_file.exists():
            pid_file.unlink()
    except Exception:
        pass


def load_env_files():
    """Load all environment files into os.environ BEFORE any config init.
    
    Priority: hunter_secrets.env > .env (first found wins per key).
    Properly handles SSH_SERVERS_JSON with JSON array values.
    """
    hunter_dir = Path(__file__).parent
    env_files = [
        hunter_dir / "hunter_secrets.env",
        hunter_dir / ".env",
    ]
    
    loaded_count = 0
    for env_file in env_files:
        if not env_file.exists():
            continue
        try:
            content = env_file.read_text(encoding="utf-8")
            
            # Handle SSH_SERVERS_JSON specially (contains JSON with = and , chars)
            if "SSH_SERVERS_JSON=" in content:
                start = content.find("SSH_SERVERS_JSON=") + len("SSH_SERVERS_JSON=")
                remaining = content[start:].strip()
                # Find the complete JSON array
                bracket_count = 0
                json_str = ""
                for char in remaining:
                    json_str += char
                    if char == '[':
                        bracket_count += 1
                    elif char == ']':
                        bracket_count -= 1
                        if bracket_count == 0:
                            break
                if json_str:
                    os.environ.setdefault("SSH_SERVERS_JSON", json_str)
            
            # Parse all other key=value lines
            for line in content.splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("SSH_SERVERS_JSON"):
                    continue  # Already handled above
                
                # Handle PowerShell $env: format
                if line.lower().startswith("$env:"):
                    m = re.match(r"^\$env:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", line)
                    if m:
                        key, value = m.group(1).strip(), m.group(2).strip()
                    else:
                        continue
                elif "=" in line:
                    key, value = line.split("=", 1)
                    key, value = key.strip(), value.strip()
                else:
                    continue
                
                # Remove quotes
                if len(value) >= 2:
                    if (value[0] == '"' and value[-1] == '"') or (value[0] == "'" and value[-1] == "'"):
                        value = value[1:-1]
                
                os.environ.setdefault(key, value)
                loaded_count += 1
            
        except Exception as e:
            print(f"Warning: Failed to load {env_file.name}: {e}")
    
    # Also set SSH_SERVERS from SSH_SERVERS_JSON for components that check SSH_SERVERS
    if "SSH_SERVERS_JSON" in os.environ and "SSH_SERVERS" not in os.environ:
        os.environ["SSH_SERVERS"] = os.environ["SSH_SERVERS_JSON"]
    
    # Map HUNTER_* to TELEGRAM_* for components that expect TELEGRAM_* vars
    _alias_map = {
        "HUNTER_API_ID": "TELEGRAM_API_ID",
        "HUNTER_API_HASH": "TELEGRAM_API_HASH",
        "HUNTER_PHONE": "TELEGRAM_PHONE",
        "CHAT_ID": "TELEGRAM_GROUP_ID",
    }
    for src, dst in _alias_map.items():
        if src in os.environ and dst not in os.environ:
            os.environ[dst] = os.environ[src]


async def main():
    """Main entry point for the hunter system"""
    # Load env files FIRST - before any config or component init
    load_env_files()
    
    print_banner()
    setup_logging()

    logger = logging.getLogger(__name__)
    
    # Kill any existing Hunter/XRay processes before starting
    kill_existing_hunter_processes(logger)

    try:
        loop = asyncio.get_running_loop()

        def _loop_exception_handler(loop_obj, context):
            exc = context.get("exception")
            msg = context.get("message", "")
            # Suppress ConnectionResetError for winerror 10054
            if isinstance(exc, ConnectionResetError):
                try:
                    if getattr(exc, "winerror", None) == 10054:
                        return
                except Exception:
                    pass
            # Suppress "Task was destroyed but it is pending" warnings from Telethon
            if "Task was destroyed but it is pending" in msg:
                return
            # Suppress GeneratorExit errors from Telethon connection loops
            if isinstance(exc, RuntimeError) and "coroutine ignored GeneratorExit" in str(exc):
                return
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
        
        # Service management: check for existing instance, register PID
        svc = ServiceManager(str(runtime_dir))
        if svc.is_running():
            logger.warning("Another Hunter instance is already running. Exiting.")
            return 1
        svc.register()
        svc.setup_signal_handlers()
        
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
            svc.unregister()
            return 1

        # Create and start orchestrator
        logger.info("Initializing Hunter Orchestrator...")
        orchestrator = HunterOrchestrator(config)

        # Start web admin dashboard
        try:
            from web.server import start_server as start_web_server
            web_port = int(os.environ.get('HUNTER_WEB_PORT', '8585'))
            start_web_server(
                dashboard=orchestrator.dashboard,
                orchestrator=orchestrator,
                host='0.0.0.0',
                port=web_port
            )
        except Exception as e:
            logger.warning(f"Web dashboard failed to start: {e}")

        logger.info("Starting autonomous hunting service...")
        await orchestrator.start()

    except KeyboardInterrupt:
        logger.info("Shutting down gracefully...")
    except Exception as e:
        logger.error(f"Fatal error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        # Always clean up resources on exit
        try:
            if 'orchestrator' in dir():
                await orchestrator.stop()
                logger.info("Orchestrator stopped successfully")
        except Exception as stop_err:
            logger.warning(f"Error during shutdown: {stop_err}")
        try:
            if 'svc' in dir():
                svc.unregister()
        except Exception:
            pass


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
