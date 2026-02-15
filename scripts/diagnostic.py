#!/usr/bin/env python3
"""
Comprehensive diagnostic script for Hunter validation system.

Tests:
1. SSH tunnel connectivity to all configured servers
2. Multi-engine validation (XRay, Sing-box, Mihomo)
3. Config parsing and prioritization
4. Validation pipeline end-to-end
5. Telegram connectivity (if available)
"""

import asyncio
import logging
import os
import sys
from pathlib import Path
from typing import Dict, List, Tuple

# Set test mode for mock validation
os.environ["HUNTER_TEST_MODE"] = "true"

sys.path.insert(0, str(Path(__file__).parent.parent))

from hunter.core.config import HunterConfig
from hunter.orchestrator import HunterOrchestrator
from hunter.telegram.scraper import TelegramScraper
from hunter.testing.benchmark import ProxyBenchmark


def setup_logging():
    """Configure logging for diagnostics."""
    logging.basicConfig(
        level=logging.DEBUG,
        format='%(asctime)s | %(levelname)-8s | %(name)-30s | %(message)s',
        handlers=[
            logging.StreamHandler(sys.stdout),
            logging.FileHandler("diagnostic.log")
        ]
    )


def print_section(title: str):
    """Print a diagnostic section header."""
    print(f"\n{'='*70}")
    print(f"  {title}")
    print(f"{'='*70}\n")


async def test_ssh_connectivity(config: HunterConfig) -> Dict[str, bool]:
    """Test SSH tunnel connectivity to all configured servers."""
    print_section("SSH TUNNEL CONNECTIVITY TEST")
    
    scraper = TelegramScraper(config)
    results = {}
    
    print(f"Testing {len(scraper.ssh_servers)} SSH servers:\n")
    
    for idx, server in enumerate(scraper.ssh_servers, 1):
        host = server['host']
        port = server['port']
        print(f"[{idx}/{len(scraper.ssh_servers)}] {host}:{port}...", end=" ", flush=True)
        
        try:
            port_result = scraper.establish_ssh_tunnel()
            if port_result:
                print(f"[OK] SUCCESS (port {port_result})")
                results[f"{host}:{port}"] = True
                scraper.close_ssh_tunnel()
                break
            else:
                print("[FAILED]")
                results[f"{host}:{port}"] = False
        except Exception as e:
            print(f"[ERROR] {e}")
            results[f"{host}:{port}"] = False
    
    success_count = sum(1 for v in results.values() if v)
    print(f"\nSSH Results: {success_count}/{len(results)} servers reachable")
    
    return results


async def test_telegram_connectivity(config: HunterConfig) -> bool:
    """Test Telegram connectivity."""
    print_section("TELEGRAM CONNECTIVITY TEST")
    
    scraper = TelegramScraper(config)
    
    print("Attempting Telegram connection...")
    try:
        connected = await scraper.connect()
        if connected:
            print("[OK] Telegram connected successfully")
            await scraper.disconnect()
            return True
        else:
            print("[NO] Telegram connection failed (expected without API credentials)")
            return False
    except Exception as e:
        print(f"[ERROR] Telegram error: {e}")
        return False


def test_proxy_engines() -> Dict[str, bool]:
    """Test availability of proxy engines."""
    print_section("PROXY ENGINE AVAILABILITY TEST")
    
    from hunter.core.utils import resolve_executable_path
    
    engines = {
        "XRay": ("xray", [
            "D:\\v2rayN\\bin\\xray\\xray.exe",
            "C:\\v2rayN\\bin\\xray\\xray.exe",
            "xray.exe"
        ]),
        "Sing-box": ("sing-box", [
            "D:\\v2rayN\\bin\\sing_box\\sing-box.exe",
            "C:\\v2rayN\\bin\\sing_box\\sing-box.exe",
            "sing-box.exe"
        ]),
        "Mihomo": ("mihomo", [
            "D:\\v2rayN\\bin\\mihomo\\mihomo.exe",
            "C:\\v2rayN\\bin\\mihomo\\mihomo.exe",
            "mihomo.exe"
        ])
    }
    
    results = {}
    for name, (cmd, paths) in engines.items():
        print(f"Checking {name}...", end=" ", flush=True)
        resolved = resolve_executable_path(cmd, "", paths)
        if resolved:
            print(f"[OK] Found: {resolved}")
            results[name] = True
        else:
            print("[MISSING]")
            results[name] = False
    
    available = sum(1 for v in results.values() if v)
    print(f"\nProxy Engines: {available}/{len(results)} available")
    
    return results


def test_validation_pipeline(config: HunterConfig) -> Tuple[int, int, int]:
    """Test the validation pipeline with test mode."""
    print_section("VALIDATION PIPELINE TEST (TEST MODE)")
    
    import base64
    import json
    
    # Create test configs
    vmess_config = {
        "add": "1.2.3.4",
        "port": 8080,
        "ps": "Test VMess",
        "id": "12345678-abcd-1234-abcd-12345678abcd",
        "aid": 0,
        "scy": "auto",
        "net": "tcp"
    }
    vmess_b64 = base64.b64encode(json.dumps(vmess_config).encode()).decode()
    vmess_uri = f"vmess://{vmess_b64}"
    
    vless_uri = "vless://12345678-abcd-1234-abcd-12345678abcd@2.3.4.5:443?encryption=none&security=tls&sni=example.com&type=tcp&ps=Test%20VLESS"
    trojan_uri = "trojan://password123@3.4.5.6:443?security=tls&sni=example.com&type=tcp&ps=Test%20Trojan"
    
    ss_config = "aes-256-gcm:password123"
    ss_b64 = base64.b64encode(ss_config.encode()).decode()
    ss_uri = f"ss://{ss_b64}@4.5.6.7:8388?ps=Test%20SS"
    
    test_uris = [vmess_uri, vless_uri, trojan_uri, ss_uri] * 25
    
    print(f"Created {len(test_uris)} test configs (4 unique types × 25)")
    print(f"Test mode: {os.getenv('HUNTER_TEST_MODE')}")
    
    orchestrator = HunterOrchestrator(config)
    
    print("\nValidating configs...")
    validated = orchestrator.validate_configs(test_uris, max_workers=20)
    
    tiered = orchestrator.tier_configs(validated)
    gold = len(tiered['gold'])
    silver = len(tiered['silver'])
    
    print(f"\nValidation Results:")
    print(f"  Total configs: {len(test_uris)}")
    print(f"  Validated: {len(validated)} ({len(validated)/len(test_uris)*100:.1f}%)")
    print(f"  Gold tier: {gold}")
    print(f"  Silver tier: {silver}")
    
    return len(test_uris), len(validated), gold + silver


def test_multi_engine_fallback() -> Dict[str, bool]:
    """Test multi-engine fallback logic."""
    print_section("MULTI-ENGINE FALLBACK TEST")
    
    benchmarker = ProxyBenchmark(iran_fragment_enabled=False)
    
    print(f"Test mode enabled: {benchmarker.test_mode}")
    print(f"XRay benchmark available: {benchmarker.xray_benchmark is not None}")
    print(f"Sing-box benchmark available: {benchmarker.singbox_benchmark is not None}")
    print(f"Mihomo benchmark available: {benchmarker.mihomo_benchmark is not None}")
    
    results = {
        "XRay": benchmarker.xray_benchmark is not None,
        "Sing-box": benchmarker.singbox_benchmark is not None,
        "Mihomo": benchmarker.mihomo_benchmark is not None,
        "Test mode": benchmarker.test_mode
    }
    
    for engine, available in results.items():
        status = "[OK]" if available else "[NO]"
        print(f"{status} {engine}")
    
    return results


async def run_diagnostics():
    """Run all diagnostic tests."""
    print("\n" + "="*70)
    print("  HUNTER DIAGNOSTIC SUITE")
    print("="*70)
    
    config = HunterConfig({
        "test_url": "https://www.cloudflare.com/cdn-cgi/trace",
        "timeout_seconds": 10,
        "max_workers": 20,
        "max_total": 3000,
        "iran_fragment_enabled": False,
        "multiproxy_port": 10808,
        "multiproxy_backends": 5,
        "targets": [],
    })
    
    # Test 1: Proxy engines
    engines = test_proxy_engines()
    
    # Test 2: Multi-engine fallback
    fallback = test_multi_engine_fallback()
    
    # Test 3: Validation pipeline
    total, validated, tiered = test_validation_pipeline(config)
    
    # Test 4: SSH connectivity
    ssh_results = await test_ssh_connectivity(config)
    
    # Test 5: Telegram connectivity (optional)
    telegram_ok = await test_telegram_connectivity(config)
    
    # Summary
    print_section("DIAGNOSTIC SUMMARY")
    
    print("Proxy Engines:")
    for engine, available in engines.items():
        status = "[OK]" if available else "[NO]"
        print(f"  {status} {engine}")
    
    print("\nMulti-Engine Fallback:")
    for engine, available in fallback.items():
        status = "[OK]" if available else "[NO]"
        print(f"  {status} {engine}")
    
    print("\nValidation Pipeline:")
    print(f"  [OK] Test mode: Enabled")
    print(f"  [OK] Configs tested: {total}")
    print(f"  [OK] Configs validated: {validated} ({validated/total*100:.1f}%)")
    print(f"  [OK] Tiered configs: {tiered}")
    
    print("\nSSH Connectivity:")
    ssh_success = sum(1 for v in ssh_results.values() if v)
    status = "[OK]" if ssh_success > 0 else "[NO]"
    print(f"  {status} Servers reachable: {ssh_success}/{len(ssh_results)}")
    
    print("\nTelegram Connectivity:")
    status = "[OK]" if telegram_ok else "[NO]"
    print(f"  {status} Connected: {telegram_ok}")
    
    print("\n" + "="*70)
    print("  DIAGNOSTIC COMPLETE")
    print("="*70 + "\n")
    
    return validated > 0 and fallback.get("Test mode", False)


async def main():
    """Main entry point."""
    setup_logging()
    
    try:
        success = await run_diagnostics()
        sys.exit(0 if success else 1)
    except Exception as e:
        logging.getLogger(__name__).exception(f"Diagnostic failed: {e}")
        sys.exit(1)


if __name__ == "__main__":
    asyncio.run(main())
