"""
Proxy benchmarking and connectivity testing.

This module provides comprehensive testing capabilities for proxy
configurations including latency measurement and connectivity checks.
"""

import gc
import json
import logging
import multiprocessing
import os
import socket
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Dict, List, Optional, Tuple

import psutil
import yaml

try:
    from hunter.core.models import HunterParsedConfig, HunterBenchResult
    from hunter.core.utils import resolve_executable_path, kill_process_on_port, tier_for_latency, resolve_ip, get_country_code, get_region
    from hunter.performance.adaptive_thread_manager import AdaptiveThreadPool, create_optimized_validator
except ImportError:
    # Fallback for direct execution
    try:
        from core.models import HunterParsedConfig, HunterBenchResult
        from core.utils import resolve_executable_path, kill_process_on_port, tier_for_latency, resolve_ip, get_country_code, get_region
        from performance.adaptive_thread_manager import AdaptiveThreadPool, create_optimized_validator
    except ImportError:
        # Final fallback - define basic classes if imports fail
        HunterParsedConfig = None
        HunterBenchResult = None
        resolve_executable_path = None
        kill_process_on_port = None
        tier_for_latency = None
        resolve_ip = None
        get_country_code = None
        get_region = None
        AdaptiveThreadPool = None
        create_optimized_validator = None

# Test URLs for connectivity testing
MULTI_TEST_URLS = [
    ("https://cp.cloudflare.com/", "cloudflare_cp"),
    ("https://1.1.1.1/cdn-cgi/trace", "cf_trace"),
    ("https://www.gstatic.com/generate_204", "google204"),
    ("https://www.msftconnecttest.com/connecttest.txt", "microsoft"),
    ("https://azure.microsoft.com/", "azure"),
    ("https://detectportal.firefox.com/success.txt", "firefox"),
    ("https://www.apple.com/library/test/success.html", "apple"),
    ("https://connectivity-check.ubuntu.com/", "ubuntu"),
    ("https://api.ipify.org/", "ipify"),
    ("https://ifconfig.me/ip", "ifconfig"),
    ("https://www.arvancloud.ir/", "arvancloud"),
    ("https://panel.arvancloud.ir/", "arvan_panel"),
]

# Executable paths
XRAY_PATH = os.getenv("XRAY_PATH", "") or resolve_executable_path("xray", "", [
    os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "xray.exe")
])

MIHOMO_PATH = os.getenv("MIHOMO_PATH", "") or resolve_executable_path("mihomo", "", [
    os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "mihomo-windows-amd64-compatible.exe")
])

SINGBOX_PATH = os.getenv("SINGBOX_PATH", "") or resolve_executable_path("sing-box", "", [
    os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "sing-box.exe")
])


class ProxyTester:
    """Tests proxy configurations for connectivity and performance."""
    
    def __init__(self, iran_fragment_enabled: bool = False):
        self.iran_fragment_enabled = iran_fragment_enabled
        self.logger = logging.getLogger(__name__)
    
    def get_random_test_url(self) -> Tuple[str, str]:
        """Get a random test URL from whitelisted domains."""
        import random
        if random.random() < 0.7:
            return random.choice(MULTI_TEST_URLS[:5])
        return random.choice(MULTI_TEST_URLS)
    
    def test_single_url(self, socks_port: int, test_url: str, timeout_seconds: int) -> Optional[float]:
        """Test a single URL through the proxy."""
        curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
        try:
            start = time.monotonic()
            result = subprocess.run(
                [
                    curl_cmd,
                    "-x", f"socks5h://127.0.0.1:{socks_port}",
                    "-s", "-o", "nul" if sys.platform == "win32" else "/dev/null",
                    "-w", "%{http_code}",
                    "-m", str(timeout_seconds),
                    "--connect-timeout", str(min(5, timeout_seconds)),
                    "-k",
                    "-A", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
                    test_url
                ],
                capture_output=True,
                text=True,
                timeout=timeout_seconds + 3
            )
            elapsed = (time.monotonic() - start) * 1000.0
            
            if result.returncode == 0:
                try:
                    status_code = int(result.stdout.strip())
                    if status_code < 400 or status_code == 204:
                        return elapsed
                except ValueError:
                    pass
        except Exception:
            pass
        return None
    
    def check_xray_path(self, xray_path: str) -> bool:
        """Check if xray executable is valid."""
        resolved = resolve_executable_path("xray", xray_path, [XRAY_PATH] + [
            r"D:\v2rayN\bin\xray\xray.exe",
            r"C:\v2rayN\bin\xray\xray.exe", 
            r"C:\Program Files\v2rayN\bin\xray\xray.exe",
            "xray.exe"
        ])
        if not resolved:
            return False
        try:
            subprocess.run([resolved, "-version"], capture_output=True, text=True, timeout=5)
            return True
        except Exception:
            return False


class XRayBenchmark:
    """Benchmark using XRay engine."""
    
    def __init__(self, tester: ProxyTester):
        self.tester = tester
        self.logger = logging.getLogger(__name__)
    
    def benchmark_config(self, outbound: Dict[str, Any], socks_port: int, test_url: str, 
                        timeout_seconds: int, use_fragment: bool = True) -> Optional[float]:
        """Benchmark configuration using XRay."""
        # Prepare outbound with proper tag
        proxy_outbound = outbound.copy()
        proxy_outbound["tag"] = "proxy"
        
        # Build outbounds list
        outbounds_list = [proxy_outbound]
        
        # Add fragment outbound for Iran DPI bypass
        if use_fragment and self.tester.iran_fragment_enabled:
            fragment_outbound = {
                "tag": "fragment",
                "protocol": "freedom",
                "settings": {
                    "domainStrategy": "AsIs",
                    "fragment": {
                        "packets": "tlshello",
                        "length": "10-20",
                        "interval": "10-20"
                    }
                }
            }
            outbounds_list.insert(0, fragment_outbound)
            # Update proxy outbound to use fragment
            if "streamSettings" not in proxy_outbound:
                proxy_outbound["streamSettings"] = {}
            if "sockopt" not in proxy_outbound["streamSettings"]:
                proxy_outbound["streamSettings"]["sockopt"] = {}
            proxy_outbound["streamSettings"]["sockopt"]["dialerProxy"] = "fragment"
        
        xray_config = {
            "log": {"loglevel": "warning"},
            "inbounds": [
                {"port": socks_port, "listen": "127.0.0.1", "protocol": "socks",
                 "settings": {"auth": "noauth", "udp": True}}
            ],
            "outbounds": outbounds_list,
        }
        
        resolved_xray = resolve_executable_path("xray", XRAY_PATH, [XRAY_PATH] + [
            r"D:\v2rayN\bin\xray\xray.exe",
            r"C:\v2rayN\bin\xray\xray.exe",
            r"C:\Program Files\v2rayN\bin\xray\xray.exe",
            "xray.exe"
        ])
        
        if not resolved_xray:
            return None

        # Best-effort validation; do not hard-fail (helps in mocked tests and some PATH setups)
        try:
            if not self.tester.check_xray_path(resolved_xray):
                self.logger.debug("XRay path validation failed; continuing anyway")
        except Exception:
            pass
        
        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix=f"HUNTER_{socks_port}_", suffix=".json")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(xray_config, f, ensure_ascii=False)
            
            process = subprocess.Popen(
                [resolved_xray, "run", "-c", temp_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            )
            time.sleep(2.0)
            
            if process.poll() is not None:
                return None
            
            latency = self.tester.test_single_url(socks_port, test_url, timeout_seconds)
            if latency:
                return latency
            
            fallback_url, _ = self.tester.get_random_test_url()
            if fallback_url and fallback_url != test_url:
                latency = self.tester.test_single_url(socks_port, fallback_url, min(timeout_seconds, 8))
                if latency:
                    return latency
            
            return None
        except subprocess.TimeoutExpired:
            return None
        except FileNotFoundError:
            self.logger.error("curl not found. Please install curl.")
            return None
        except Exception as e:
            self.logger.debug(f"Benchmark error on port {socks_port}: {e}")
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                        process.wait(timeout=1)
                    except Exception:
                        pass
                except Exception:
                    pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except Exception:
                    pass


class MihomoBenchmark:
    """Benchmark using Mihomo (Clash Meta) engine."""
    
    def __init__(self, tester: ProxyTester):
        self.tester = tester
        self.logger = logging.getLogger(__name__)
    
    def benchmark_config(self, parsed: HunterParsedConfig, socks_port: int, test_url: str, 
                        timeout_seconds: int) -> Optional[float]:
        """Benchmark configuration using Mihomo."""
        resolved_mihomo = resolve_executable_path("mihomo", MIHOMO_PATH, [MIHOMO_PATH] + [
            r"D:\v2rayN\bin\mihomo\mihomo.exe",
            r"C:\v2rayN\bin\mihomo\mihomo.exe",
            r"C:\Program Files\v2rayN\bin\mihomo\mihomo.exe",
            "mihomo.exe"
        ])
        
        if not resolved_mihomo:
            return None
        
        try:
            outbound = parsed.outbound
            protocol = outbound.get("protocol", "")
            settings = outbound.get("settings", {})
            stream = outbound.get("streamSettings", {})
            
            proxy = None
            if protocol == "vmess":
                vnext = settings.get("vnext", [{}])[0]
                users = vnext.get("users", [{}])[0]
                proxy = {
                    "name": "test",
                    "type": "vmess",
                    "server": vnext.get("address", parsed.host),
                    "port": vnext.get("port", parsed.port),
                    "uuid": users.get("id", ""),
                    "alterId": users.get("alterId", 0),
                    "cipher": users.get("security", "auto"),
                }
                network = stream.get("network", "tcp")
                if network == "ws":
                    proxy["network"] = "ws"
                    ws_settings = stream.get("wsSettings", {})
                    proxy["ws-opts"] = {"path": ws_settings.get("path", "/")}
                if stream.get("security") == "tls":
                    proxy["tls"] = True
                    proxy["skip-cert-verify"] = True
            
            elif protocol == "vless":
                vnext = settings.get("vnext", [{}])[0]
                users = vnext.get("users", [{}])[0]
                proxy = {
                    "name": "test",
                    "type": "vless",
                    "server": vnext.get("address", parsed.host),
                    "port": vnext.get("port", parsed.port),
                    "uuid": users.get("id", ""),
                    "network": stream.get("network", "tcp"),
                }
                if users.get("flow"):
                    proxy["flow"] = users.get("flow")
                if stream.get("security") == "tls":
                    proxy["tls"] = True
                    proxy["skip-cert-verify"] = True
                elif stream.get("security") == "reality":
                    reality = stream.get("realitySettings", {})
                    proxy["tls"] = True
                    proxy["reality-opts"] = {
                        "public-key": reality.get("publicKey", ""),
                        "short-id": reality.get("shortId", ""),
                    }
                    proxy["servername"] = reality.get("serverName", "")
                    proxy["client-fingerprint"] = reality.get("fingerprint", "chrome")
            
            elif protocol == "trojan":
                servers = settings.get("servers", [{}])[0]
                proxy = {
                    "name": "test",
                    "type": "trojan",
                    "server": servers.get("address", parsed.host),
                    "port": servers.get("port", parsed.port),
                    "password": servers.get("password", ""),
                    "skip-cert-verify": True,
                }
            
            elif protocol == "shadowsocks":
                servers = settings.get("servers", [{}])[0]
                proxy = {
                    "name": "test",
                    "type": "ss",
                    "server": servers.get("address", parsed.host),
                    "port": servers.get("port", parsed.port),
                    "cipher": servers.get("method", "aes-256-gcm"),
                    "password": servers.get("password", ""),
                }
            
            if not proxy:
                return None
            
            mihomo_config = {
                "mixed-port": socks_port,
                "mode": "global",
                "log-level": "silent",
                "proxies": [proxy],
                "proxy-groups": [{"name": "GLOBAL", "type": "select", "proxies": ["test"]}],
            }
            
            temp_path = None
            process = None
            try:
                fd, temp_path = tempfile.mkstemp(prefix=f"MIHOMO_{socks_port}_", suffix=".yaml")
                os.close(fd)
                with open(temp_path, "w", encoding="utf-8") as f:
                    yaml.dump(mihomo_config, f, allow_unicode=True)
                
                process = subprocess.Popen(
                    [resolved_mihomo, "-f", temp_path],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
                )
                time.sleep(2.0)
                
                if process.poll() is not None:
                    return None
                
                latency = self.tester.test_single_url(socks_port, test_url, timeout_seconds)
                if latency:
                    return latency
                
                for url, _ in MULTI_TEST_URLS[:3]:
                    if url != test_url:
                        latency = self.tester.test_single_url(socks_port, url, 12)
                        if latency:
                            return latency
                return None
            finally:
                if process:
                    try:
                        process.terminate()
                        process.wait(timeout=2)
                    except:
                        try:
                            process.kill()
                        except:
                            pass
                if temp_path and os.path.exists(temp_path):
                    try:
                        os.remove(temp_path)
                    except:
                        pass
        except Exception as e:
            self.logger.debug(f"Mihomo benchmark error: {e}")
            return None


class SingBoxBenchmark:
    """Benchmark using sing-box engine."""
    
    def __init__(self, tester: ProxyTester):
        self.tester = tester
        self.logger = logging.getLogger(__name__)
    
    def benchmark_config(self, parsed: HunterParsedConfig, socks_port: int, test_url: str, 
                        timeout_seconds: int) -> Optional[float]:
        """Benchmark configuration using sing-box."""
        resolved_singbox = resolve_executable_path("sing-box", SINGBOX_PATH, [SINGBOX_PATH] + [
            r"D:\v2rayN\bin\sing_box\sing-box.exe",
            r"C:\v2rayN\bin\sing_box\sing-box.exe",
            r"C:\Program Files\v2rayN\bin\sing_box\sing-box.exe",
            "sing-box.exe",
            "singbox.exe"
        ])
        
        if not resolved_singbox:
            return None
        
        try:
            outbound = parsed.outbound
            protocol = outbound.get("protocol", "")
            settings = outbound.get("settings", {})
            stream = outbound.get("streamSettings", {})
            
            sb_outbound = None
            if protocol == "vmess":
                vnext = settings.get("vnext", [{}])[0]
                users = vnext.get("users", [{}])[0]
                sb_outbound = {
                    "type": "vmess",
                    "tag": "proxy",
                    "server": vnext.get("address", parsed.host),
                    "server_port": vnext.get("port", parsed.port),
                    "uuid": users.get("id", ""),
                    "security": users.get("security", "auto"),
                    "alter_id": users.get("alterId", 0),
                }
                network = stream.get("network", "tcp")
                if network == "ws":
                    ws = stream.get("wsSettings", {})
                    sb_outbound["transport"] = {"type": "ws", "path": ws.get("path", "/")}
                if stream.get("security") == "tls":
                    sb_outbound["tls"] = {"enabled": True, "insecure": True}
            
            elif protocol == "vless":
                vnext = settings.get("vnext", [{}])[0]
                users = vnext.get("users", [{}])[0]
                sb_outbound = {
                    "type": "vless",
                    "tag": "proxy",
                    "server": vnext.get("address", parsed.host),
                    "server_port": vnext.get("port", parsed.port),
                    "uuid": users.get("id", ""),
                }
                if users.get("flow"):
                    sb_outbound["flow"] = users.get("flow")
                network = stream.get("network", "tcp")
                if network != "tcp":
                    sb_outbound["transport"] = {"type": network}
                if stream.get("security") == "tls":
                    sb_outbound["tls"] = {"enabled": True, "insecure": True}
                elif stream.get("security") == "reality":
                    reality = stream.get("realitySettings", {})
                    sb_outbound["tls"] = {
                        "enabled": True,
                        "server_name": reality.get("serverName", ""),
                        "utls": {"enabled": True, "fingerprint": reality.get("fingerprint", "chrome")},
                        "reality": {
                            "enabled": True,
                            "public_key": reality.get("publicKey", ""),
                            "short_id": reality.get("shortId", ""),
                        }
                    }
            
            elif protocol == "trojan":
                servers = settings.get("servers", [{}])[0]
                sb_outbound = {
                    "type": "trojan",
                    "tag": "proxy",
                    "server": servers.get("address", parsed.host),
                    "server_port": servers.get("port", parsed.port),
                    "password": servers.get("password", ""),
                    "tls": {"enabled": True, "insecure": True},
                }
            
            elif protocol == "shadowsocks":
                servers = settings.get("servers", [{}])[0]
                sb_outbound = {
                    "type": "shadowsocks",
                    "tag": "proxy",
                    "server": servers.get("address", parsed.host),
                    "server_port": servers.get("port", parsed.port),
                    "method": servers.get("method", "aes-256-gcm"),
                    "password": servers.get("password", ""),
                }
            
            if not sb_outbound:
                return None
            
            singbox_config = {
                "log": {"level": "error"},
                "inbounds": [{
                    "type": "mixed",
                    "tag": "mixed-in",
                    "listen": "127.0.0.1",
                    "listen_port": socks_port,
                }],
                "outbounds": [sb_outbound, {"type": "direct", "tag": "direct"}],
            }
            
            temp_path = None
            process = None
            try:
                fd, temp_path = tempfile.mkstemp(prefix=f"SINGBOX_{socks_port}_", suffix=".json")
                os.close(fd)
                with open(temp_path, "w", encoding="utf-8") as f:
                    json.dump(singbox_config, f, ensure_ascii=False)
                
                process = subprocess.Popen(
                    [resolved_singbox, "run", "-c", temp_path],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
                )
                time.sleep(2.0)
                
                if process.poll() is not None:
                    return None
                
                latency = self.tester.test_single_url(socks_port, test_url, timeout_seconds)
                if latency:
                    return latency
                
                for url, _ in MULTI_TEST_URLS[:3]:
                    if url != test_url:
                        latency = self.tester.test_single_url(socks_port, url, 12)
                        if latency:
                            return latency
                return None
            finally:
                if process:
                    try:
                        process.terminate()
                        process.wait(timeout=2)
                    except:
                        try:
                            process.kill()
                        except:
                            pass
                if temp_path and os.path.exists(temp_path):
                    try:
                        os.remove(temp_path)
                    except:
                        pass
        except Exception as e:
            self.logger.debug(f"Sing-box benchmark error: {e}")
            return None


class ProxyBenchmark:
    """Main proxy benchmark orchestrator with optimized thread management."""
    
    def __init__(self, iran_fragment_enabled: bool = False):
        self.tester = ProxyTester(iran_fragment_enabled)
        self.xray_benchmark = XRayBenchmark(self.tester)
        self.mihomo_benchmark = MihomoBenchmark(self.tester)
        self.singbox_benchmark = SingBoxBenchmark(self.tester)
        self.logger = logging.getLogger(__name__)
        self.test_mode = os.getenv("HUNTER_TEST_MODE", "").lower() == "true"
        
        # Initialize adaptive thread pool with REDUCED threads to prevent memory leak
        # Memory-aware: 8 max threads instead of 32 to prevent 97% memory usage
        cpu_count = multiprocessing.cpu_count()
        memory_gb = psutil.virtual_memory().total / (1024**3)
        
        # Calculate safe max_threads based on available memory
        # Each thread can use ~500MB during benchmark (xray process + overhead)
        safe_max_threads = max(4, min(int(memory_gb / 2), 8))  # Max 8 threads
        
        self.thread_pool = AdaptiveThreadPool(
            min_threads=2,
            max_threads=safe_max_threads,
            target_cpu_utilization=0.70,  # Lower CPU to reduce memory pressure
            target_queue_size=50,         # Smaller queue to prevent memory buildup
            enable_work_stealing=True,
            enable_cpu_affinity=False
        )
        
        # Memory management settings (LOWERED to prevent issues when starting with high memory)
        self.memory_emergency_threshold = 0.85  # Stop at 85% memory (was 90%)
        self.memory_warning_threshold = 0.80    # Warn at 80% memory (was 85%)
        self.batch_chunk_size = 50              # Process max 50 configs per batch
        
        # Performance tracking
        self.benchmark_count = 0
        self.start_time = time.time()
        
        self.logger.info("ProxyBenchmark initialized with adaptive thread management")
    
    def benchmark_config(self, parsed: HunterParsedConfig, socks_port: int, test_url: str, 
                        timeout_seconds: int, try_all_engines: bool = False) -> Optional[float]:
        """Benchmark configuration using multiple engines with optimized timeouts."""
        if self.test_mode:
            import random
            return random.uniform(50, 300)
        
        # Optimize timeout for faster validation
        # Use shorter timeout for first attempt, longer for fallback
        primary_timeout = max(3, timeout_seconds // 2)  # Half timeout for primary
        fallback_timeout = timeout_seconds
        
        # Try XRay first (fastest startup)
        latency = self.xray_benchmark.benchmark_config(
            parsed.outbound, socks_port, test_url, primary_timeout
        )
        if latency:
            return latency
        
        if not try_all_engines:
            return None
        
        # Try sing-box (fallback)
        latency = self.singbox_benchmark.benchmark_config(
            parsed, socks_port + 1000, test_url, min(fallback_timeout, 8)
        )
        if latency:
            return latency
        
        # Try mihomo (last resort)
        latency = self.mihomo_benchmark.benchmark_config(
            parsed, socks_port + 2000, test_url, min(fallback_timeout, 8)
        )
        if latency:
            return latency
        
        return None
    
    def benchmark_configs_batch(self, configs: List[Tuple[HunterParsedConfig, int]], 
                              test_url: str, timeout_seconds: int = 10) -> List[HunterBenchResult]:
        """Benchmark multiple configurations with MEMORY-AWARE batch chunking to prevent leak."""
        if not configs:
            return []
        
        total_configs = len(configs)
        self.logger.info(f"Starting batch benchmark of {total_configs} configs with memory-safe chunking")
        
        # Start thread pool if not already running
        if not self.thread_pool.running:
            self.thread_pool.start()
        
        # Split into smaller chunks to prevent memory leak
        all_results = []
        chunk_size = self.batch_chunk_size
        
        for chunk_idx in range(0, total_configs, chunk_size):
            chunk = configs[chunk_idx:chunk_idx + chunk_size]
            chunk_num = (chunk_idx // chunk_size) + 1
            total_chunks = (total_configs + chunk_size - 1) // chunk_size
            
            # Check memory before processing chunk
            mem_percent = psutil.virtual_memory().percent
            if mem_percent >= self.memory_emergency_threshold * 100:
                self.logger.error(
                    f"EMERGENCY STOP: Memory at {mem_percent:.1f}%, stopping benchmark. "
                    f"Processed {len(all_results)}/{total_configs} configs."
                )
                break
            
            if mem_percent >= self.memory_warning_threshold * 100:
                self.logger.warning(
                    f"High memory ({mem_percent:.1f}%), forcing aggressive cleanup before chunk {chunk_num}/{total_chunks}"
                )
                gc.collect()
                time.sleep(0.5)  # Brief pause to let GC finish
            
            self.logger.info(f"Processing chunk {chunk_num}/{total_chunks} ({len(chunk)} configs)")
            
            # Submit benchmark tasks for this chunk
            futures = []
            for i, (parsed, socks_port) in enumerate(chunk):
                future = self.thread_pool.submit(
                    self._benchmark_config_task, parsed, socks_port, test_url, timeout_seconds
                )
                futures.append((future, parsed))
            
            # Collect results for this chunk
            chunk_results = []
            completed = 0
            failed = 0
            
            for future, parsed in futures:
                try:
                    latency = future.result(timeout=timeout_seconds + 5)
                    if latency:
                        result = self.create_bench_result(parsed, latency)
                        chunk_results.append(result)
                        completed += 1
                    else:
                        failed += 1
                except Exception as e:
                    self.logger.debug(f"Benchmark task failed: {e}")
                    failed += 1
            
            all_results.extend(chunk_results)
            self.benchmark_count += len(chunk)
            
            # CRITICAL: Force garbage collection after each chunk
            gc.collect()
            
            # Log chunk performance
            mem_after = psutil.virtual_memory().percent
            self.logger.info(
                f"Chunk {chunk_num}/{total_chunks} done: {completed} OK, {failed} failed, "
                f"memory: {mem_after:.1f}%"
            )
            
            # Brief pause between chunks to let system stabilize
            if chunk_idx + chunk_size < total_configs:
                time.sleep(0.2)
        
        # Final cleanup
        gc.collect()
        
        # Log final performance metrics
        metrics = self.thread_pool.get_metrics()
        elapsed_time = time.time() - self.start_time
        
        self.logger.info(
            f"Batch benchmark completed: {len(all_results)} successful out of {total_configs} total"
        )
        self.logger.info(
            f"Performance: {metrics.tasks_per_second:.1f} tasks/sec, "
            f"CPU: {metrics.cpu_utilization:.1f}%, "
            f"Memory: {metrics.memory_utilization:.1f}%"
        )
        
        return all_results
    
    def _benchmark_config_task(self, parsed: HunterParsedConfig, socks_port: int, 
                            test_url: str, timeout_seconds: int) -> Optional[float]:
        """Benchmark a single configuration (task function for thread pool)."""
        return self.benchmark_config(parsed, socks_port, test_url, timeout_seconds, try_all_engines=True)
    
    def get_performance_metrics(self) -> Dict[str, Any]:
        """Get performance metrics for the benchmark system."""
        if not self.thread_pool.running:
            return {"status": "Thread pool not running"}
        
        metrics = self.thread_pool.get_metrics()
        elapsed_time = time.time() - self.start_time
        
        return {
            "thread_pool_metrics": {
                "total_tasks": metrics.total_tasks,
                "completed_tasks": metrics.completed_tasks,
                "failed_tasks": metrics.failed_tasks,
                "tasks_per_second": metrics.tasks_per_second,
                "cpu_utilization": metrics.cpu_utilization,
                "memory_utilization": metrics.memory_utilization,
                "thread_utilization": metrics.thread_utilization,
                "queue_size": metrics.queue_size
            },
            "benchmark_metrics": {
                "total_benchmarks": self.benchmark_count,
                "elapsed_time": elapsed_time,
                "benchmarks_per_second": self.benchmark_count / elapsed_time if elapsed_time > 0 else 0
            }
        }
    
    def start_thread_pool(self):
        """Start the adaptive thread pool."""
        if not self.thread_pool.running:
            self.thread_pool.start()
            self.logger.info("Adaptive thread pool started")
    
    def stop_thread_pool(self):
        """Stop the adaptive thread pool."""
        if self.thread_pool.running:
            self.thread_pool.stop()
            self.logger.info("Adaptive thread pool stopped")
    
    def create_bench_result(self, parsed: HunterParsedConfig, latency_ms: float) -> HunterBenchResult:
        """Create benchmark result from parsed config and latency."""
        ip = resolve_ip(parsed.host)
        cc = get_country_code(ip)
        region = get_region(cc) if cc else "Other"
        tier = tier_for_latency(latency_ms)
        
        return HunterBenchResult(
            uri=parsed.uri,
            outbound=parsed.outbound,
            host=parsed.host,
            port=parsed.port,
            identity=parsed.identity,
            ps=parsed.ps,
            latency_ms=latency_ms,
            ip=ip,
            country_code=cc,
            region=region,
            tier=tier,
        )
