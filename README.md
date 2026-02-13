# Hunter 🏹 - Advanced Proxy Hunting System

> A powerful, autonomous tool for discovering, testing, and managing proxy configurations to bypass internet censorship.

[![Python Version](https://img.shields.io/badge/python-3.8+-blue.svg)](https://www.python.org/downloads/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Table of Contents

- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Project Structure](#project-structure)
- [Usage](#usage)
- [Troubleshooting](#troubleshooting)
- [Development](#development)
- [Contributing](#contributing)
- [License](#license)

Hunter is a standalone proxy hunting system designed to autonomously discover, validate, and manage V2Ray-compatible proxy configurations. It features advanced anti-DPI techniques, multi-engine testing, and seamless load balancing for reliable internet access in censored environments.

## Features

- **Autonomous Operation**: Continuous hunting for fresh proxy configs from Telegram channels.
- **Multi-Engine Support**: Compatible with XRay, SingBox, and Mihomo for diverse testing.
- **Load Balancing**: Dynamic proxy server with automatic failover and health checks.
- **Anti-DPI Capabilities**: Iran-specific fragmentation and obfuscation to evade detection.
- **Telegram Integration**: Scrapes configurations and reports results via bot.
- **Web Dashboard**: Monitor performance at http://localhost:8080.
- **Persistent Caching**: Saves working configs for faster restarts.
- **Security Obfuscation**: Built-in techniques to bypass censorship.

## Installation

### Prerequisites
- Python 3.8 or higher
- Windows 10/11 (required for bundled executables like xray.exe)

### Setup

1. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/hunter.git
   cd hunter
   ```

2. Create a virtual environment:
   ```bash
   python -m venv .venv
   .venv\Scripts\activate
   ```

3. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

## Quick Start

After installation, run the hunter as follows:

1. **Using the launcher:**
   Double-click `run.bat`.

2. **Manual execution:**
   ```powershell
   & ".venv\Scripts\activate"
   python main.py
   ```

The system will begin hunting for proxies and start the load balancer on port 10808.

## Configuration

Hunter uses environment variables for configuration. You can set them in your shell or create a `.env` file in the project root.

### Required

- `HUNTER_API_ID`: Your Telegram API ID (obtain from https://my.telegram.org/)
- `HUNTER_API_HASH`: Your Telegram API Hash
- `HUNTER_PHONE`: Your phone number for Telegram authentication (international format, e.g., +1234567890)

### Optional

- `TOKEN`: Bot token for sending reports (optional)
- `CHAT_ID`: Channel or chat ID for reports (e.g., @your_channel)
- `HUNTER_MULTIPROXY_PORT`: Port for the load balancer proxy server (default: 10808)
- `HUNTER_XRAY_PATH`: Path to XRay executable (default: bin/xray.exe)
- `HUNTER_SLEEP`: Time between hunting cycles in seconds (default: 300)
- `IRAN_FRAGMENT_ENABLED`: Enable Iran-specific fragmentation (default: false)
- `ADEE_ENABLED`: Enable additional obfuscation features (default: false)

### Example .env File

Create a file named `.env` with:

```
HUNTER_API_ID=12345678
HUNTER_API_HASH=abcdef1234567890
HUNTER_PHONE=+1234567890
TOKEN=your_bot_token_here
CHAT_ID=@your_channel
HUNTER_MULTIPROXY_PORT=10808
IRAN_FRAGMENT_ENABLED=true
ADEE_ENABLED=false
```

Note: Sensitive files like `.env` and session files are ignored by git.

## Project Structure

```
hunter/
├── main.py              # Main entry point
├── run.bat              # Windows launcher script
├── orchestrator.py      # Core workflow coordinator
├── .gitignore           # Git ignore rules
├── requirements.txt     # Python dependencies
├── __init__.py          # Package initialization
├── hunter_secrets.env   # Environment secrets (ignored)
├── hunter_session.session # Telegram session (ignored)
├── subscriptions_cache.txt # Cached subscriptions (ignored)
├── bin/                 # Executables and tools
│   ├── AmazTool.exe
│   ├── chromedriver.exe
│   ├── mihomo-windows-amd64-compatible.exe
│   ├── run_hunter.py
│   ├── sing-box.exe
│   ├── tor.exe
│   └── xray.exe
├── config/              # Configuration and caching
│   └── cache.py
├── core/                # Core modules
│   ├── __init__.py
│   ├── config.py
│   ├── models.py
│   └── utils.py
├── gateway/             # Gateway components
│   └── __init__.py
├── network/             # Network utilities
│   ├── __init__.py
│   └── http_client.py
├── parsers/             # URI and config parsers
│   ├── __init__.py
│   └── uri_parser.py
├── proxy/               # Load balancing
│   └── load_balancer.py
├── security/            # Obfuscation and security
│   └── obfuscation.py
├── telegram/            # Telegram integration
│   └── scraper.py
├── testing/             # Benchmarking and tests
│   └── benchmark.py
└── runtime/             # Runtime cache and temp files (created at runtime)
```

## Usage

### Command Line

```bash
python main.py --help    # Show help
python main.py --version # Show version
python main.py           # Start hunting
```

### Stopping

Press `Ctrl+C` to gracefully shut down all services and save state.

### Web Interface

Access the monitoring dashboard at http://localhost:8080 to view proxy status and performance.

## Troubleshooting

### Common Issues

1. **Virtual Environment Issues**
   If `.venv` is missing:
   ```batch
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r requirements.txt
   ```

2. **Telegram Authentication Errors**
   Ensure API credentials are correct and phone number is in international format.

3. **Port Conflicts**
   Change `HUNTER_MULTIPROXY_PORT` if 10808 is in use.

4. **Permission Errors**
   Run as administrator.

5. **Large Cache Files**
   The `subscriptions_cache.txt` is ignored by git; delete if needed for space.

## Development

### Running Tests

```bash
python -m pytest testing/ -v
```

### Importing in Code

```python
from core.config import HunterConfig
from orchestrator import HunterOrchestrator
```

### Building

No build process; ensure all dependencies are installed.

## Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests
5. Submit a pull request

For major changes, open an issue first.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
