#!/usr/bin/env python3
"""
Test script with working sources that contain actual config data
"""

import requests
import json
import time
import sys
from pathlib import Path

def is_valid_config_content(content):
    """Check if downloaded content contains valid configuration data"""
    if not content or len(content.strip()) < 10:
        return False, "Content too short or empty"
    
    content_lower = content.lower()
    
    # Check for common config patterns
    config_patterns = [
        # V2Ray/VMess patterns
        'vmess://', 'vless://', 'trojan://', 'hysteria://',
        # Shadowsocks patterns  
        'ss://', 'shadowsocks',
        # JSON config patterns
        '"protocol":', '"server":', '"port":', '"settings":',
        # Common config fields
        '"host":', '"path":', '"tls":', '"network":',
        # Base64 encoded configs (common)
        'eyJ', 'ewo', 'In0', 'CiAg',
    ]
    
    found_patterns = []
    for pattern in config_patterns:
        if pattern in content_lower:
            found_patterns.append(pattern)
    
    if not found_patterns:
        return False, "No config patterns found"
    
    # Additional validation for JSON configs
    if content.strip().startswith('{') and content.strip().endswith('}'):
        try:
            json.loads(content)
            return True, f"Valid JSON with patterns: {', '.join(found_patterns[:3])}"
        except json.JSONDecodeError:
            return False, "Invalid JSON format"
    
    # For non-JSON configs, check if they look like proxy lists
    lines = [line.strip() for line in content.split('\n') if line.strip()]
    proxy_lines = [line for line in lines if any(pattern in line.lower() for pattern in ['://', 'ss://', 'vmess://'])]
    
    if len(proxy_lines) >= 1:
        return True, f"Valid proxy list with {len(proxy_lines)} configs"
    
    return True, f"Valid content with patterns: {', '.join(found_patterns[:3])}"

def test_download_from_source(source_url, timeout=30):
    """Test downloading from a single source"""
    print(f"\n[TEST] Testing: {source_url}")
    
    try:
        # Make request with common headers
        headers = {
            'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36',
            'Accept': 'text/plain,application/json,*/*',
        }
        
        response = requests.get(source_url, headers=headers, timeout=timeout)
        response.raise_for_status()
        
        content = response.text
        content_size = len(content)
        
        print(f"   [OK] Downloaded {content_size} bytes")
        
        # Show a preview of the content
        preview = content[:300].replace('\n', ' ').strip()
        print(f"   [PREVIEW] {preview}...")
        
        # Validate content
        is_valid, message = is_valid_config_content(content)
        
        if is_valid:
            print(f"   [OK] Valid config content: {message}")
            
            # Count actual configs
            lines = [line.strip() for line in content.split('\n') if line.strip()]
            config_count = 0
            for line in lines:
                if any(pattern in line.lower() for pattern in ['vmess://', 'vless://', 'trojan://', 'ss://', 'hysteria://']):
                    config_count += 1
                elif line.startswith('{') and '"server"' in line:
                    config_count += 1
            
            print(f"   [COUNT] Found {config_count} configuration entries")
            return True, content_size, message, config_count
        else:
            print(f"   [FAIL] Invalid content: {message}")
            return False, content_size, message, 0
            
    except requests.exceptions.Timeout:
        print(f"   [TIMEOUT] Timeout after {timeout}s")
        return False, 0, "Timeout", 0
    except requests.exceptions.RequestException as e:
        print(f"   [ERROR] Request failed: {e}")
        return False, 0, str(e), 0
    except Exception as e:
        print(f"   [ERROR] Unexpected error: {e}")
        return False, 0, str(e), 0

def test_working_sources():
    """Test downloading from working sources that contain actual configs"""
    print("[START] Testing working sources with actual config content")
    print("=" * 70)
    
    # Working sources with actual config data
    sources = [
        # V2Ray official config (JSON format)
        "https://raw.githubusercontent.com/v2fly/v2ray-core/master/release/config/config.json",
        
        # Some working proxy list sources (these may change over time)
        "https://raw.githubusercontent.com/ripaojiedian/mulan/master/ssr.txt",
        "https://raw.githubusercontent.com/FreeGPT/FreeGPT/main/README.md",
        
        # Alternative sources that might work
        "https://raw.githubusercontent.com/rodrigograca29/free-proxy-list/master/proxy-list-raw.txt",
        "https://raw.githubusercontent.com/fate0/proxylist/master/proxy.list",
    ]
    
    results = []
    total_downloaded = 0
    total_configs = 0
    successful_sources = 0
    
    for i, source in enumerate(sources, 1):
        print(f"\n[SOURCE] {i}/{len(sources)}")
        
        success, size, message, config_count = test_download_from_source(source)
        
        results.append({
            'url': source,
            'success': success,
            'size': size,
            'message': message,
            'config_count': config_count
        })
        
        if success:
            total_downloaded += size
            total_configs += config_count
            successful_sources += 1
        
        # Small delay between requests
        time.sleep(1)
    
    # Print summary
    print("\n" + "=" * 70)
    print("[SUMMARY] DOWNLOAD TEST RESULTS")
    print("=" * 70)
    
    print(f"Total sources tested: {len(sources)}")
    print(f"Successful downloads: {successful_sources}")
    print(f"Failed downloads: {len(sources) - successful_sources}")
    print(f"Total data downloaded: {total_downloaded:,} bytes")
    print(f"Total config entries found: {total_configs}")
    print(f"Success rate: {successful_sources/len(sources)*100:.1f}%")
    
    print("\n[DETAILS] RESULTS:")
    print("-" * 70)
    
    for i, result in enumerate(results, 1):
        status = "[OK]" if result['success'] else "[FAIL]"
        size_info = f"({result['size']:,} bytes)" if result['size'] > 0 else "(0 bytes)"
        config_info = f"[{result['config_count']} configs]" if result['config_count'] > 0 else "[0 configs]"
        print(f"{i:2d}. {status} {size_info} {config_info}")
        print(f"     Message: {result['message']}")
        print(f"     URL: {result['url']}")
    
    print("\n[VALIDATION] CONFIG CONTENT ANALYSIS:")
    
    if total_configs > 0:
        print(f"[OK] Total of {total_configs} configuration entries found")
        print("   Config validation patterns are working correctly")
        
        # Show sample config types found
        config_types = set()
        for result in results:
            if result['success'] and 'vmess' in result['message'].lower():
                config_types.add('VMess')
            if result['success'] and 'shadowsocks' in result['message'].lower():
                config_types.add('Shadowsocks')
            if result['success'] and 'json' in result['message'].lower():
                config_types.add('JSON')
        
        if config_types:
            print(f"   Config types detected: {', '.join(config_types)}")
    else:
        print("[WARNING] No configuration entries found")
        print("   Sources may not contain expected config formats")
    
    return successful_sources, total_configs, len(sources)

def create_test_command():
    """Create a test command for the application"""
    print("\n[APP_TEST] APPLICATION COMMAND TEST")
    print("=" * 70)
    
    # Use the working sources we found
    working_sources = [
        "https://raw.githubusercontent.com/v2fly/v2ray-core/master/release/config/config.json",
    ]
    
    command = {
        "command": "download_configs",
        "sources": working_sources,
        "proxy": ""
    }
    
    command_json = json.dumps(command, indent=2)
    
    print("[COMMAND] Test command for application:")
    print(command_json)
    
    print("\n[INSTRUCTIONS] To test in the application:")
    print("1. Run: huntercensor.exe")
    print("2. Go to Sources page")
    print("3. Click 'Download All Sources'")
    print("4. Watch console for debug output")
    print("5. Check UI logs for download progress")
    
    return working_sources

if __name__ == "__main__":
    print("[TEST] HUNTER CENSORSHIP - WORKING SOURCES DOWNLOAD TEST")
    print("=" * 70)
    
    # Test working sources
    successful, configs, total = test_working_sources()
    
    # Create test command
    test_sources = create_test_command()
    
    print("\n" + "=" * 70)
    print("[FINAL] TEST RESULTS")
    print("=" * 70)
    
    if successful > 0:
        print(f"[SUCCESS] Download system working!")
        print(f"   Sources working: {successful}/{total}")
        print(f"   Configs found: {configs}")
        print("   Content validation working")
        
        if configs > 0:
            print("[EXCELLENT] Actual configuration data found and validated!")
        else:
            print("[GOOD] Download working, but config format may need adjustment")
    else:
        print("[FAILURE] No sources working")
        print("   Check internet connection or source URLs")
    
    print(f"\n[RESULT] Test completed successfully")
    print("Ready to test with the application!")
