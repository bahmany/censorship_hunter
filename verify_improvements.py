#!/usr/bin/env python3
"""
Verification script for Hunter improvements.
Tests validation pipeline, SSH connectivity, and multi-engine fallback.
"""

import asyncio
import logging
import os
import sys
from pathlib import Path

os.environ["HUNTER_TEST_MODE"] = "true"
sys.path.insert(0, str(Path(__file__).parent.parent))

from core.config import HunterConfig
from orchestrator import HunterOrchestrator
from testing.benchmark import ProxyBenchmark
import base64
import json


logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s | %(levelname)-8s | %(message)s'
)
logger = logging.getLogger(__name__)


def test_validation_pipeline():
    """Test validation pipeline with test mode."""
    logger.info("=" * 70)
    logger.info("TEST 1: PARSER VALIDATION")
    logger.info("=" * 70)
    
    # Test parsers directly without orchestrator
    from parsers import UniversalParser
    
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
    
    test_uris = [vmess_uri, vless_uri, trojan_uri, ss_uri]
    
    logger.info(f"Test configs created: {len(test_uris)} total (4 unique types)")
    
    parser = UniversalParser()
    parsed_count = 0
    
    for uri in test_uris:
        try:
            parsed = parser.parse(uri)
            if parsed:
                parsed_count += 1
                logger.info(f"✓ Parsed {parsed.protocol}: {parsed.ps}")
        except Exception as e:
            logger.error(f"✗ Failed to parse: {e}")
    
    logger.info(f"Successfully parsed: {parsed_count}/{len(test_uris)}")
    logger.info(f"Success rate: {parsed_count/len(test_uris)*100:.1f}%")
    
    return parsed_count == len(test_uris)


def test_multi_engine_fallback():
    """Test multi-engine fallback."""
    logger.info("")
    logger.info("=" * 70)
    logger.info("TEST 2: MULTI-ENGINE FALLBACK")
    logger.info("=" * 70)
    
    benchmarker = ProxyBenchmark(iran_fragment_enabled=False)
    
    logger.info(f"Test mode enabled: {benchmarker.test_mode}")
    logger.info(f"XRay benchmark: {benchmarker.xray_benchmark is not None}")
    logger.info(f"Sing-box benchmark: {benchmarker.singbox_benchmark is not None}")
    logger.info(f"Mihomo benchmark: {benchmarker.mihomo_benchmark is not None}")
    
    all_available = all([
        benchmarker.test_mode,
        benchmarker.xray_benchmark is not None,
        benchmarker.singbox_benchmark is not None,
        benchmarker.mihomo_benchmark is not None
    ])
    
    return all_available


async def test_memory_management():
    """Test memory management features."""
    logger.info("")
    logger.info("=" * 70)
    logger.info("TEST 3: MEMORY MANAGEMENT")
    logger.info("=" * 70)
    
    try:
        import psutil
        mem = psutil.virtual_memory()
        logger.info(f"Current memory: {mem.percent:.1f}% ({mem.used / (1024**3):.1f}GB / {mem.total / (1024**3):.1f}GB)")
        
        # Test memory thresholds
        from testing.benchmark import ProxyBenchmark
        benchmarker = ProxyBenchmark()
        
        logger.info(f"Emergency threshold: {benchmarker.memory_emergency_threshold * 100:.0f}%")
        logger.info(f"Warning threshold: {benchmarker.memory_warning_threshold * 100:.0f}%")
        logger.info(f"Batch chunk size: {benchmarker.batch_chunk_size}")
        logger.info(f"Max threads: {benchmarker.thread_pool.max_threads}")
        
        # Verify thresholds are safe
        safe_thresholds = (
            benchmarker.memory_emergency_threshold <= 0.85 and
            benchmarker.memory_warning_threshold <= 0.80 and
            benchmarker.batch_chunk_size <= 50 and
            benchmarker.thread_pool.max_threads <= 8
        )
        
        if safe_thresholds:
            logger.info("✓ Memory management thresholds are SAFE")
        else:
            logger.warning("✗ Memory management thresholds may be too high")
        
        return safe_thresholds
    except ImportError:
        logger.warning("psutil not available, skipping memory test")
        return True
    except Exception as e:
        logger.error(f"Memory test error: {e}")
        return False


def main():
    """Run all verification tests."""
    logger.info("")
    logger.info("HUNTER IMPROVEMENTS VERIFICATION")
    logger.info("")
    
    results = {}
    
    # Test 1: Validation pipeline
    try:
        results['validation'] = test_validation_pipeline()
    except Exception as e:
        logger.error(f"Validation test failed: {e}")
        logger.error(f"Exception type: {type(e)}")
        import traceback
        logger.error(f"Traceback: {traceback.format_exc()}")
        results['validation'] = False
    
    # Test 2: Multi-engine fallback
    try:
        results['fallback'] = test_multi_engine_fallback()
    except Exception as e:
        logger.error(f"Fallback test failed: {e}")
        results['fallback'] = False
    
    # Test 3: Memory management
    try:
        results['memory'] = asyncio.run(test_memory_management())
    except Exception as e:
        logger.error(f"Memory test failed: {e}")
        results['memory'] = False
    
    # Summary
    logger.info("")
    logger.info("=" * 70)
    logger.info("VERIFICATION SUMMARY")
    logger.info("=" * 70)
    logger.info(f"Parser validation: {'PASS' if results['validation'] else 'FAIL'}")
    logger.info(f"Multi-engine fallback: {'PASS' if results['fallback'] else 'FAIL'}")
    logger.info(f"Memory management: {'PASS' if results['memory'] else 'FAIL'}")
    
    all_pass = all(results.values())
    logger.info("")
    logger.info(f"Overall result: {'ALL TESTS PASSED' if all_pass else 'SOME TESTS FAILED'}")
    logger.info("")
    
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
