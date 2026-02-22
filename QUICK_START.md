# Hunter - Quick Start Guide

## Prerequisites

- Python 3.8+
- Windows 10/11 (for bundled `.exe` tools) or Linux/macOS (bring your own binaries)
- Telegram account with API credentials ([my.telegram.org](https://my.telegram.org/apps))

## 1. Install

```bash
git clone https://github.com/yourusername/hunter.git
cd hunter
python -m venv .venv

# Windows
.venv\Scripts\activate

# Linux / macOS
source .venv/bin/activate

pip install -r requirements.txt
```

## 2. Configure

Copy the example environment file and fill in your values:

```bash
cp .env.example .env
```

Minimum required settings in `.env`:

```env
HUNTER_API_ID=12345678
HUNTER_API_HASH=abcdef1234567890abcdef1234567890
HUNTER_PHONE=+1234567890
```

Optional — Telegram bot for reporting results:

```env
TOKEN=your_bot_token_here
CHAT_ID=@your_channel_or_-100xxxxxxxxxx
```

## 3. Add Proxy Engines

Place at least one of the following executables in the `bin/` directory:

| File | Source |
|------|--------|
| `xray.exe` / `xray` | [github.com/XTLS/Xray-core/releases](https://github.com/XTLS/Xray-core/releases) |
| `sing-box.exe` / `sing-box` | [github.com/SagerNet/sing-box/releases](https://github.com/SagerNet/sing-box/releases) |
| `mihomo.exe` / `mihomo` | [github.com/MetaCubeX/mihomo/releases](https://github.com/MetaCubeX/mihomo/releases) |

Hunter tries engines in the order: XRay → Sing-box → Mihomo.

## 4. Run

```bash
# Windows — double-click or:
run.bat

# Any platform:
python main.py
```

The load balancer starts on `127.0.0.1:10808` (SOCKS5).  
The web dashboard is available at `http://localhost:8080`.

## 5. Verify

```bash
python verify_improvements.py
```

Expected output:

```
Validation pipeline: PASS
Multi-engine fallback: PASS
Overall result: ALL TESTS PASSED
```

## Key Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `HUNTER_MULTIPROXY_PORT` | `10808` | Local SOCKS5 proxy port |
| `HUNTER_SLEEP` | `300` | Seconds between hunting cycles |
| `HUNTER_WORKERS` | `10` | Concurrent config testers |
| `HUNTER_TEST_MODE` | `false` | Dry-run without real proxies |
| `IRAN_FRAGMENT_ENABLED` | `false` | TLS fragmentation for DPI bypass |
| `HUNTER_DPI_EVASION` | `true` | Enable full DPI evasion suite |
| `HUNTER_WEB_PORT` | `8080` | Web dashboard port |

See `.env.example` for the full list of options.

## Troubleshooting

**No working configs found** — Ensure at least one proxy engine binary is in `bin/` and Telegram credentials are correct.

**Port 10808 in use** — Set `HUNTER_MULTIPROXY_PORT` to another port in `.env`.

**Telegram auth loop** — Delete `*.session` files and re-authenticate.

**Permission errors on Windows** — Run the terminal as Administrator.
