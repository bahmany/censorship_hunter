# Hunter - Standalone V2Ray Proxy Hunting System

A comprehensive proxy hunting and testing system designed for circumventing internet censorship with advanced anti-DPI capabilities.

## Quick Start

### Prerequisites
- Python 3.8+
- Windows (required for xray.exe)

### Running the Hunter

1. **Double-click the launcher:**
   ```batch
   run.bat
   ```

2. **Or run manually:**
   ```powershell
   # Activate virtual environment
   & ".venv\Scripts\activate"

   # Run the hunter
   python main.py
   ```

## Configuration

The hunter requires Telegram API credentials for scraping proxy configurations. Set these environment variables:

### Required Environment Variables

```batch
# Telegram API credentials (get from https://my.telegram.org/)
set HUNTER_API_ID=your_telegram_api_id
set HUNTER_API_HASH=your_telegram_api_hash
set HUNTER_PHONE=+1234567890

# Optional: Bot for reporting results
set TOKEN=your_bot_token
set CHAT_ID=@your_channel_or_chat_id
```

### Optional Configuration

```batch
# Proxy server port (default: 10808)
set HUNTER_MULTIPROXY_PORT=10808

# XRay executable path
set HUNTER_XRAY_PATH=C:\path\to\xray.exe

# Cycle interval in seconds (default: 300)
set HUNTER_SLEEP=300

# Enable Iran-specific features
set IRAN_FRAGMENT_ENABLED=true
set ADEE_ENABLED=true
```

## Project Structure

```
hunter/
├── main.py              # Standalone entry point
├── run.bat              # Windows launcher
├── runtime/             # All cache and temporary files
│   ├── HUNTER_state.json    # Session state
│   ├── HUNTER_raw.txt       # Raw harvested configs
│   ├── HUNTER_gold.txt      # Validated gold-tier configs
│   ├── HUNTER_silver.txt    # Validated silver-tier configs
│   ├── HUNTER_bridge_pool.txt # Bridge configs for next cycle
│   ├── HUNTER_validated.jsonl # Benchmark results
│   ├── subscriptions_cache.txt # Cached working configs
│   ├── working_configs_cache.txt # Working configs cache
│   └── *.json/*.yaml         # Temporary test config files
├── orchestrator.py      # Main workflow coordinator
├── core/                # Configuration & models
├── testing/            # Benchmarking engines
├── proxy/              # Load balancer
├── network/            # HTTP clients
├── telegram/           # Telegram integration
├── parsers/            # URI parsers
├── security/           # Anti-DPI obfuscation
├── config/             # Caching system
└── bin/               # Executables
```

## Features

- **Autonomous Operation**: Runs continuously, hunting for new proxy configs
- **Multi-Engine Testing**: Supports XRay, SingBox, and Mihomo engines
- **Load Balancing**: Single-port proxy server with automatic failover
- **Anti-DPI**: Iran-specific fragmentation and obfuscation techniques
- **Telegram Integration**: Scrapes configs from channels and reports results
- **Web Interface**: Monitoring dashboard at http://localhost:8080
- **Persistent Caching**: Remembers working configs between runs

## Command Line Options

```bash
python main.py --help    # Show help
python main.py --version # Show version
python main.py           # Start hunting
```

## Stopping the Hunter

Press `Ctrl+C` to gracefully shut down all services and save state.

## Troubleshooting

### Virtual Environment Issues
If the virtual environment is missing:
```batch
python -m venv .venv
.venv\Scripts\activate
pip install -r ../requirements.txt
```

### Configuration Errors
The hunter will show specific error messages for missing configuration. Set the required environment variables as shown above.

### Permission Issues
Run as administrator if you encounter port binding issues.

## Development

To run tests:
```batch
python -m pytest ../tests/ -v
```

To import hunter in your scripts:
```python
from hunter.core.config import HunterConfig
from hunter.orchestrator import HunterOrchestrator
```
