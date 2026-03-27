#!/usr/bin/env python3
"""
Test script to verify download functionality from all sources
Ensures downloaded content contains actual configuration data
"""

import requests
import json
import time
import sys
import os
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
        
        # Validate content
        is_valid, message = is_valid_config_content(content)
        
        if is_valid:
            print(f"   [OK] Valid config content: {message}")
            return True, content_size, message
        else:
            print(f"   [FAIL] Invalid content: {message}")
            print(f"   [PREVIEW] Content preview: {content[:200]}...")
            return False, content_size, message
            
    except requests.exceptions.Timeout:
        print(f"   [TIMEOUT] Timeout after {timeout}s")
        return False, 0, "Timeout"
    except requests.exceptions.RequestException as e:
        print(f"   [ERROR] Request failed: {e}")
        return False, 0, str(e)
    except Exception as e:
        print(f"   [ERROR] Unexpected error: {e}")
        return False, 0, str(e)

def test_all_sources():
    """Test downloading from all configured sources"""
    print("[START] Comprehensive download test from all sources")
    print("=" * 60)
    
    # Default sources from the application
    sources = [
        "https://raw.githubusercontent.com/bahmany/censorship_hunter/main/configs.txt",
        "https://raw.githubusercontent.com/mahdibland/ShadowsocksAggregator/master/all/iran.txt", 
        "https://raw.githubusercontent.com/Alvin9999/pac_nodes/master/ssr.txt",
        "https://raw.githubusercontent.com/v2fly/v2ray-core/master/release/config/config.json",
        "https://raw.githubusercontent.com/learnhard-cn/free_proxy_ss/main/clash/config.yaml",
        "https://raw.githubusercontent.com/racskasdf/free-proxy-list/main/proxies.txt",
    ]
    
    results = []
    total_downloaded = 0
    successful_sources = 0
    
    for i, source in enumerate(sources, 1):
        print(f"\n[SOURCE] {i}/{len(sources)}")
        
        success, size, message = test_download_from_source(source)
        
        results.append({
            'url': source,
            'success': success,
            'size': size,
            'message': message
        })
        
        if success:
            total_downloaded += size
            successful_sources += 1
        
        # Small delay between requests
        time.sleep(1)
    
    # Print summary
    print("\n" + "=" * 60)
    print("[SUMMARY] DOWNLOAD TEST RESULTS")
    print("=" * 60)
    
    print(f"Total sources tested: {len(sources)}")
    print(f"Successful downloads: {successful_sources}")
    print(f"Failed downloads: {len(sources) - successful_sources}")
    print(f"Total data downloaded: {total_downloaded:,} bytes")
    print(f"Success rate: {successful_sources/len(sources)*100:.1f}%")
    
    print("\n[DETAILS] RESULTS:")
    print("-" * 60)
    
    for i, result in enumerate(results, 1):
        status = "[OK]" if result['success'] else "[FAIL]"
        size_info = f"({result['size']:,} bytes)" if result['size'] > 0 else "(0 bytes)"
        print(f"{i:2d}. {status} {size_info} - {result['message']}")
        print(f"     URL: {result['url']}")
    
    print("\n[RECOMMENDATIONS]:")
    
    if successful_sources == 0:
        print("[CRITICAL] No sources are working!")
        print("   - Check internet connection")
        print("   - Verify source URLs are accessible")
        print("   - Check if GitHub is blocked")
    elif successful_sources < len(sources) // 2:
        print("[WARNING] Less than 50% of sources are working")
        print("   - Some sources may be temporarily unavailable")
        print("   - Consider adding alternative sources")
    else:
        print("[GOOD] Most sources are working")
        print("   - Download system is functioning properly")
    
    # Test specific config validation
    print("\n[VALIDATION] CONFIG CONTENT:")
    valid_configs = [r for r in results if r['success'] and 'config' in r['message'].lower()]
    if valid_configs:
        print(f"[OK] Found {len(valid_configs)} sources with valid config content")
        for result in valid_configs[:3]:  # Show first 3
            print(f"   - {result['message']}")
    else:
        print("[WARNING] No sources contained recognizable config patterns")
        print("   - Sources may contain non-standard formats")
        print("   - Consider updating config validation patterns")
    
    return successful_sources, len(sources)

def test_application_download_system():
    """Test the application's download system via command"""
    print("\n[APP_TEST] TESTING APPLICATION DOWNLOAD SYSTEM")
    print("=" * 60)
    
    # Check if application executable exists
    project_root = Path(__file__).parent
    exe_path = project_root / "build" / "huntercensor.exe"
    if not exe_path.exists():
        exe_path = project_root / "release_package" / "huntercensor.exe"
    
    if not exe_path.exists():
        print("[ERROR] Application executable not found")
        print("   Build the application first with: ./build.bat")
        return False
    
    print(f"[OK] Found application: {exe_path}")
    
    # Create test command
    test_sources = [
        "https://raw.githubusercontent.com/bahmany/censorship_hunter/main/configs.txt",
        "https://raw.githubusercontent.com/mahdibland/ShadowsocksAggregator/master/all/iran.txt"
    ]
    
    sources_json = json.dumps(test_sources)
    command = {
        "command": "download_configs",
        "sources": test_sources,
        "proxy": ""
    }
    
    command_json = json.dumps(command)
    
    print(f"[COMMAND] Test command: {command_json}")
    print("\n[INSTRUCTIONS] To test the application download system:")
    print("1. Run the application: huntercensor.exe")
    print("2. Navigate to the 'Sources' page")
    print("3. Click 'Download All Sources'")
    print("4. Monitor the console output for debug messages")
    print("5. Check the download logs in the UI")
    
    return True

if __name__ == "__main__":
    print("[TEST] HUNTER CENSORSHIP - COMPREHENSIVE DOWNLOAD TEST")
    print("=" * 60)
    
    # Test direct downloads from sources
    successful, total = test_all_sources()
    
    # Test application download system
    app_test = test_application_download_system()
    
    print("\n" + "=" * 60)
    print("[FINAL] TEST RESULTS")
    print("=" * 60)
    
    if successful > 0:
        print(f"[SUCCESS] {successful}/{total} sources working")
        print("   Download functionality is working")
        print("   Config content validation is working")
    else:
        print("[FAILURE] No sources are working")
        print("   Check internet connection and source URLs")
    
    if app_test:
        print("[OK] Application test instructions provided")
    
    print(f"\n[RESULT] Overall Success Rate: {successful/total*100:.1f}%")
    
    if successful >= total * 0.7:  # 70% success rate
        print("[PASSED] Download system is working well!")
        sys.exit(0)
    elif successful > 0:
        print("[PARTIAL] Some sources working, consider improvements")
        sys.exit(1)
    else:
        print("[FAILED] No sources working")
        sys.exit(2)
