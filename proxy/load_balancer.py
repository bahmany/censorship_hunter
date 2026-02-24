"""
Load balancer that orchestrates multiple backends for the hunter gateway.
"""
import json
import logging
import os
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional, Set, Tuple

try:
    from hunter.core.models import HunterParsedConfig
    from hunter.core.utils import (resolve_executable_path, kill_process_on_port, now_ts, read_lines, append_unique_lines)
    from hunter.parsers import UniversalParser
    from hunter.security.obfuscation import StealthObfuscationEngine
except ImportError:
    from core.models import HunterParsedConfig
    from core.utils import (resolve_executable_path, kill_process_on_port, now_ts, read_lines, append_unique_lines)
    from parsers import UniversalParser
    from security.obfuscation import StealthObfuscationEngine

XRAY_PATH_FALLBACKS = [
    os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "xray.exe")
]

IRAN_FRAGMENT_ENABLED = os.getenv("IRAN_FRAGMENT_ENABLED", "false").lower() == "true"

XRAY_SUPPORTED_PROTOCOLS = {"vmess", "vless", "trojan", "shadowsocks"}

# Shadowsocks ciphers supported by XRay 25.x (AEAD only)
# Legacy stream ciphers (aes-*-cfb, chacha20-ietf, rc4-md5) are NOT supported
# and cause XRay to fail with "unknown cipher method" error.
XRAY_SUPPORTED_SS_CIPHERS = {
    "aes-128-gcm", "aes-256-gcm",
    "chacha20-poly1305", "chacha20-ietf-poly1305",
    "xchacha20-poly1305", "xchacha20-ietf-poly1305",
    "2022-blake3-aes-128-gcm", "2022-blake3-aes-256-gcm",
    "2022-blake3-chacha20-poly1305",
    "none", "plain",
}


class MultiProxyServer:
    """Balanced proxy server running a single SOCKS/HTTP entry point."""

    def __init__(
        self,
        xray_path: str,
        port: int = 10808,
        num_backends: int = 5,
        health_check_interval: int = 60,
        obfuscation_engine: Optional[StealthObfuscationEngine] = None,
    ) -> None:
        self.xray_path = resolve_executable_path("xray", xray_path, [xray_path] + XRAY_PATH_FALLBACKS) or xray_path
        self.port = port
        self.num_backends = num_backends
        self.health_check_interval = health_check_interval
        self._parser = UniversalParser()
        self.obfuscation_engine = obfuscation_engine

        self.logger = logging.getLogger(__name__)
        self._lock = threading.RLock()
        self._running = False
        self._process: Optional[subprocess.Popen] = None
        self._config_path: Optional[str] = None
        self._backends: List[Dict[str, Any]] = []
        self._available_configs: List[Tuple[str, float]] = []
        self._failed_uris: Set[str] = set()
        self._stats: Dict[str, Any] = {
            "restarts": 0,
            "health_checks": 0,
            "backend_swaps": 0,
            "last_restart": None,
        }
        self._health_thread: Optional[threading.Thread] = None

        # Force backend support
        self._forced_backend: Optional[str] = None  # URI of forced backend
        self._force_permanent: bool = False  # True = permanent, False = temporary (cleared on next refresh)

    def _is_xray_supported(self, parsed: HunterParsedConfig) -> bool:
        try:
            proto = str(parsed.outbound.get("protocol", "") or "").lower()
            if proto not in XRAY_SUPPORTED_PROTOCOLS:
                return False
            # Reject Shadowsocks with unsupported (legacy) ciphers
            if proto == "shadowsocks":
                settings = parsed.outbound.get("settings", {})
                servers = settings.get("servers", [])
                for srv in servers:
                    method = str(srv.get("method", "") or "").lower()
                    if method and method not in XRAY_SUPPORTED_SS_CIPHERS:
                        return False
            return True
        except Exception:
            return False

    def _filter_supported_configs(self, configs: List[Tuple[str, float]]) -> List[Tuple[str, float]]:
        filtered: List[Tuple[str, float]] = []
        for uri, latency in configs:
            if not isinstance(uri, str):
                continue
            proto = uri.split("://", 1)[0].lower()
            if proto not in {"vmess", "vless", "trojan", "ss", "shadowsocks"}:
                continue
            # For SS configs, also check cipher compatibility
            if proto in {"ss", "shadowsocks"}:
                parsed = self._parser.parse(uri)
                if not parsed or not self._is_xray_supported(parsed):
                    continue
            filtered.append((uri, latency))
        return filtered

    def set_forced_backend(self, uri: str, permanent: bool = False) -> bool:
        """Force a specific backend URI. If permanent=True, survives refreshes."""
        parsed = self._parser.parse(uri)
        if not parsed or not self._is_xray_supported(parsed):
            return False
        with self._lock:
            self._forced_backend = uri
            self._force_permanent = permanent
        self.logger.info(f"[Balancer] Forced backend set ({'permanent' if permanent else 'temporary'}): {uri[:60]}")
        # Rebuild with forced backend
        forced = [{"uri": uri, "latency": 0, "healthy": True, "added_at": now_ts(), "forced": True}]
        self._write_and_start(forced)
        with self._lock:
            self._backends = forced
        return True

    def clear_forced_backend(self) -> None:
        """Remove forced backend and revert to normal balancing."""
        with self._lock:
            self._forced_backend = None
            self._force_permanent = False
        self.logger.info("[Balancer] Forced backend cleared, reverting to normal")
        self._refresh_backends()

    def set_manual_backends(self, uris: List[str]) -> int:
        """Manually set specific backend URIs. Returns count of accepted backends."""
        backends = []
        for uri in uris:
            parsed = self._parser.parse(uri)
            if parsed and self._is_xray_supported(parsed):
                backends.append({"uri": uri, "latency": 0, "healthy": True, "added_at": now_ts(), "manual": True})
        if not backends:
            return 0
        if self._write_and_start(backends):
            with self._lock:
                self._backends = backends
            self.logger.info(f"[Balancer] Manual backends set: {len(backends)}")
            return len(backends)
        return 0

    def get_all_backends_detail(self) -> List[Dict[str, Any]]:
        """Get detailed info for all current backends."""
        with self._lock:
            backends = list(self._backends)
            forced = self._forced_backend
            force_perm = self._force_permanent
        result = []
        for b in backends:
            result.append({
                "uri": b.get("uri", ""),
                "uri_short": b.get("uri", "")[:80],
                "latency": round(b.get("latency", 0)),
                "healthy": b.get("healthy", False),
                "added_at": b.get("added_at", ""),
                "forced": b.get("forced", False),
                "manual": b.get("manual", False),
            })
        return result

    def get_available_configs_list(self) -> List[Dict[str, Any]]:
        """Get available config pool for UI selection."""
        with self._lock:
            configs = list(self._available_configs)
        return [{"uri": uri, "uri_short": uri[:80], "latency": round(lat)} for uri, lat in configs[:100]]

    def _create_balanced_config(self, backends: List[Dict[str, Any]]) -> Dict[str, Any]:
        """Create xray config with load balancer across multiple backends."""
        outbounds = []
        selectors = []

        if IRAN_FRAGMENT_ENABLED:
            fragment_outbound = {
                "tag": "fragment",
                "protocol": "freedom",
                "settings": {"domainStrategy": "AsIs", "fragment": {"packets": "tlshello", "length": "10-20", "interval": "10-20"}},
            }
            outbounds.append(fragment_outbound)

        for idx, backend in enumerate(backends):
            if not backend.get("healthy", True):
                continue
            parsed: Optional[HunterParsedConfig] = self._parser.parse(backend["uri"])
            if not parsed:
                continue
            if not self._is_xray_supported(parsed):
                continue
            outbound = parsed.outbound.copy()
            outbound["tag"] = f"proxy-{idx}"
            if IRAN_FRAGMENT_ENABLED:
                settings = outbound.setdefault("streamSettings", {})
                sockopt = settings.setdefault("sockopt", {})
                sockopt["dialerProxy"] = "fragment"
            outbounds.append(outbound)
            selectors.append(outbound["tag"])

        if not selectors:
            outbounds.append({"tag": "direct", "protocol": "freedom", "settings": {"domainStrategy": "AsIs"}})
            selectors = ["direct"]

        outbounds.append({"protocol": "blackhole", "tag": "block", "settings": {}})

        has_proxies = any(s.startswith("proxy-") for s in selectors)

        config = {
            "log": {"loglevel": "warning"},
            "inbounds": [
                {
                    "port": self.port,
                    "listen": "0.0.0.0",
                    "protocol": "socks",
                    "tag": "socks-in",
                    "settings": {"auth": "noauth", "udp": True},
                    "sniffing": {"enabled": True, "destOverride": ["http", "tls", "quic"], "routeOnly": False},
                },
                {
                    "port": self.port + 100,
                    "listen": "0.0.0.0",
                    "protocol": "http",
                    "tag": "http-in",
                    "settings": {"allowTransparent": False},
                },
            ],
            "outbounds": outbounds,
            "routing": {
                "domainStrategy": "AsIs",
                "balancers": [{
                    "tag": "balancer",
                    "selector": selectors,
                    "strategy": {"type": "leastPing"} if has_proxies else {"type": "random"},
                }],
                "rules": [
                    {"type": "field", "inboundTag": ["socks-in", "http-in"], "balancerTag": "balancer"},
                ],
            },
            "dns": {
                "servers": [
                    {"address": "https://cloudflare-dns.com/dns-query", "skipFallback": False},
                    {"address": "https://dns.google/dns-query", "skipFallback": False},
                    "1.1.1.1",
                    "8.8.8.8",
                ],
            },
        }

        if has_proxies:
            config["observatory"] = {
                "subjectSelector": [s for s in selectors if s.startswith("proxy-")],
                "probeURL": "https://cp.cloudflare.com/",
                "probeInterval": "60s",
                "enableConcurrency": True,
            }
        return config

    def _test_xray_config(self, config_path: str) -> Tuple[bool, str]:
        """Test xray config validity. Returns (ok, error_text)."""
        try:
            result = subprocess.run(
                [self.xray_path, "run", "-test", "-c", config_path],
                capture_output=True, timeout=10,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            err = (result.stdout + result.stderr).decode("utf-8", errors="ignore").strip()
            if result.returncode == 0:
                return True, ""
            return False, err[-500:] if len(err) > 500 else err
        except Exception as e:
            return False, str(e)

    def _write_and_start(self, backends: List[Dict[str, Any]]) -> bool:
        config = self._create_balanced_config(backends)
        # Kill only our own XRay process first, then clear our port
        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except Exception:
                try:
                    self._process.kill()
                except Exception:
                    pass
            self._process = None
        kill_process_on_port(self.port)
        kill_process_on_port(self.port + 100)
        try:
            if self._config_path and os.path.exists(self._config_path):
                os.remove(self._config_path)
        except Exception:
            pass
        fd, path = tempfile.mkstemp(prefix="balancer_", suffix=".json")
        os.close(fd)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(config, f, ensure_ascii=False, indent=2)
        self._config_path = path

        # Pre-validate config before starting XRay
        ok, err_text = self._test_xray_config(path)
        if not ok:
            self.logger.warning(f"[Balancer] Config test failed on port {self.port}: {err_text}")
            # Try removing backends one by one to find the bad one
            if backends:
                for i in range(len(backends) - 1, -1, -1):
                    reduced = backends[:i] + backends[i+1:]
                    config2 = self._create_balanced_config(reduced)
                    with open(path, "w", encoding="utf-8") as f:
                        json.dump(config2, f, ensure_ascii=False, indent=2)
                    ok2, _ = self._test_xray_config(path)
                    if ok2:
                        self.logger.info(f"[Balancer] Removed bad backend {i}, retrying with {len(reduced)}")
                        config = config2
                        backends = reduced
                        break
                else:
                    # All backends bad, fall back to direct
                    config = self._create_balanced_config([])
                    with open(path, "w", encoding="utf-8") as f:
                        json.dump(config, f, ensure_ascii=False, indent=2)
                    backends = []

        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except Exception:
                try:
                    self._process.kill()
                except Exception:
                    pass
        try:
            self._process = subprocess.Popen(
                [self.xray_path, "run", "-c", self._config_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            time.sleep(1.5)
            if self._process.poll() is not None:
                rc = self._process.returncode
                stderr_out = b""
                try:
                    _stdout, stderr_out = self._process.communicate(timeout=0.5)
                except Exception:
                    pass
                err_msg = stderr_out.decode("utf-8", errors="ignore").strip() if stderr_out else ""
                if err_msg:
                    if len(err_msg) > 800:
                        err_msg = err_msg[-800:]
                    self.logger.warning(f"[Balancer] XRay exited early (code {rc}) on port {self.port}: {err_msg}")
                else:
                    self.logger.warning(f"[Balancer] XRay exited early (code {rc}) on port {self.port}")
                return False
            self._stats["restarts"] += 1
            self._stats["last_restart"] = now_ts()
            self.logger.info(
                f"Balancer started on port {self.port} with {len(backends)} backends "
                f"(latencies: {[round(b['latency'],0) for b in backends]}ms)"
            )
            return True
        except Exception as e:
            self.logger.warning(f"Balancer XRay start failed: {e}")
            return False

    def _test_port(self, socks_port: int, test_url: str, timeout: int = 8) -> Optional[float]:
        """Test a SOCKS port with curl and return latency in ms, or None on failure."""
        curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
        null_out = "nul" if sys.platform == "win32" else "/dev/null"
        try:
            start = time.monotonic()
            result = subprocess.run(
                [curl_cmd, "-x", f"socks5h://127.0.0.1:{socks_port}", "-s",
                 "-o", null_out, "-w", "%{http_code}", "-m", str(timeout), "-k", test_url],
                capture_output=True, text=True, timeout=timeout + 3,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            elapsed_ms = (time.monotonic() - start) * 1000.0
            if result.returncode == 0:
                try:
                    code = int(result.stdout.strip())
                    if code < 400 or code == 204:
                        return elapsed_ms
                except ValueError:
                    pass
        except Exception:
            pass
        return None

    def _test_backend(self, uri: str, timeout: int = 8) -> Optional[float]:
        """Test a backend proxy and return latency in ms."""
        parsed = self._parser.parse(uri)
        if not parsed:
            return None
        if not self._is_xray_supported(parsed):
            return None
        port = self.port + 200 + (abs(hash(uri)) % 50)
        config = {
            "log": {"loglevel": "none"},
            "inbounds": [{"port": port, "listen": "127.0.0.1", "protocol": "socks", "settings": {"auth": "noauth", "udp": False}}],
            "outbounds": [parsed.outbound],
        }
        temp_path = None
        process = None
        try:
            fd, temp_path = tempfile.mkstemp(prefix="test_", suffix=".json")
            os.close(fd)
            with open(temp_path, "w", encoding="utf-8") as f:
                json.dump(config, f, ensure_ascii=False)
            process = subprocess.Popen(
                [self.xray_path, "run", "-c", temp_path],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
            )
            time.sleep(0.8)
            if process.poll() is not None:
                return None
            # Try primary URL, then fallback
            latency = self._test_port(port, "https://1.1.1.1/cdn-cgi/trace", timeout)
            if not latency:
                latency = self._test_port(port, "https://www.google.com/generate_204", timeout)
            return latency
        except Exception:
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=1)
                except Exception:
                    try:
                        process.kill()
                    except Exception:
                        pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except Exception:
                    pass

    def _find_working_backends(self, count: int = 5) -> List[Dict[str, Any]]:
        """Find working backends from available configs by testing each."""
        with self._lock:
            configs = list(self._available_configs)
        configs.sort(key=lambda x: x[1])
        working = []
        tried = set()
        for uri, orig_latency in configs:
            if len(working) >= count:
                break
            if uri in tried or uri in self._failed_uris:
                continue
            tried.add(uri)
            latency = self._test_backend(uri, timeout=10)
            if latency:
                working.append({"uri": uri, "latency": latency, "healthy": True, "added_at": now_ts()})
                self.logger.info(f"[Balancer] Backend {len(working)}: {latency:.0f}ms")
            else:
                with self._lock:
                    self._failed_uris.add(uri)
        return working

    def _backends_from_configs(self, configs: List[Tuple[str, float]], count: int) -> List[Dict[str, Any]]:
        """Build backend list directly from pre-validated configs without re-testing."""
        backends = []
        reject_parse = 0
        reject_xray = 0
        for uri, latency in sorted(configs, key=lambda x: x[1]):
            if len(backends) >= count:
                break
            parsed = self._parser.parse(uri)
            if not parsed:
                reject_parse += 1
                if reject_parse <= 3:
                    short = uri[:80] if isinstance(uri, str) else str(uri)[:80]
                    self.logger.debug(f"[Balancer] parse failed: {short}")
                continue
            if not self._is_xray_supported(parsed):
                reject_xray += 1
                proto = parsed.outbound.get("protocol", "?")
                if reject_xray <= 3:
                    self.logger.debug(f"[Balancer] xray unsupported: proto={proto}")
                continue
            backends.append({"uri": uri, "latency": latency, "healthy": True, "added_at": now_ts()})
        if not backends:
            self.logger.warning(
                f"[Balancer] _backends_from_configs: 0/{len(configs)} accepted "
                f"(parse_fail={reject_parse}, xray_unsupported={reject_xray})"
            )
        return backends

    def start(self, initial_configs: Optional[List[Tuple[str, float]]] = None) -> None:
        """Start the load balancer."""
        with self._lock:
            if self._running:
                return
            self._running = True

        if initial_configs:
            with self._lock:
                self._available_configs = self._filter_supported_configs(list(initial_configs))

        self.logger.info(f"[Balancer] Starting on port {self.port} with {self.num_backends} backend slots")

        started = False

        if initial_configs:
            # Seed from cache without re-testing (cache entries were already validated)
            backends = self._backends_from_configs(self._available_configs, self.num_backends)
            if backends:
                if self._write_and_start(backends):
                    with self._lock:
                        self._backends = backends
                    self.logger.info(f"[Balancer] Started with {len(backends)} cached backends")
                    started = True
                else:
                    with self._lock:
                        self._backends = []
                    self.logger.warning("[Balancer] Failed to start with cached backends, falling back to direct")
                    started = self._write_and_start([])
            else:
                self.logger.warning("[Balancer] No cached backends available, will retry...")
                started = self._write_and_start([])  # Start with direct fallback
        else:
            started = self._write_and_start([])  # Start with direct fallback

        # Start health check thread
        self._health_thread = threading.Thread(target=self._health_check_loop, daemon=True)
        self._health_thread.start()
        if started:
            self.logger.info(f"[Balancer] Server ready | SOCKS on :{self.port}")
        else:
            self.logger.warning(f"[Balancer] Server failed to start | port :{self.port}")

    def _health_check_loop(self) -> None:
        """Background health check - tests the main balancer port directly."""
        self.logger.info(f"[Balancer] Health monitor started (interval: {self.health_check_interval}s)")
        while self._running:
            time.sleep(self.health_check_interval)
            if not self._running:
                break

            self._stats["health_checks"] += 1

            # Test the main balancer port directly (the real health indicator)
            latency = self._test_port(self.port, "https://1.1.1.1/cdn-cgi/trace", 10)
            if not latency:
                latency = self._test_port(self.port, "https://www.google.com/generate_204", 10)

            if latency:
                backend_count = len(self._backends)
                self.logger.info(f"[Balancer] OK {latency:.0f}ms | {backend_count} backends active")
            else:
                self.logger.warning("[Balancer] Main port not responding, refreshing backends...")
                self._refresh_backends()

    def _refresh_backends(self) -> None:
        """Restart XRay - first try existing backends, then find new ones."""
        # If permanent forced backend, don't refresh
        with self._lock:
            if self._forced_backend and self._force_permanent:
                forced = [{"uri": self._forced_backend, "latency": 0, "healthy": True, "added_at": now_ts(), "forced": True}]
                self._write_and_start(forced)
                self._backends = forced
                return
            # Clear temporary forced backend on refresh
            if self._forced_backend and not self._force_permanent:
                self._forced_backend = None

        # Clear stale failed URIs periodically
        with self._lock:
            if len(self._failed_uris) > 100:
                self._failed_uris.clear()

        # Strategy 1: Try restarting with current backends (fast, no re-testing)
        with self._lock:
            current = list(self._backends)
        if current:
            if self._write_and_start(current):
                self.logger.info(f"[Balancer] Restarted with {len(current)} existing backends")
                return

        # Strategy 2: Try building from available configs pool (no re-testing)
        with self._lock:
            avail = list(self._available_configs)
        if avail:
            new_backends = self._backends_from_configs(avail, self.num_backends)
            if new_backends:
                if self._write_and_start(new_backends):
                    with self._lock:
                        self._backends = new_backends
                    self.logger.info(f"[Balancer] Restarted with {len(new_backends)} backends from pool")
                    self._stats["backend_swaps"] += 1
                    return

        # Strategy 3: Re-test configs individually (slow, last resort)
        new_backends = self._find_working_backends(self.num_backends)
        if new_backends:
            if self._write_and_start(new_backends):
                with self._lock:
                    self._backends = new_backends
                self.logger.info(f"[Balancer] Restarted with {len(new_backends)} re-tested backends")
                self._stats["backend_swaps"] += 1
            else:
                with self._lock:
                    self._backends = []
                self.logger.error("[Balancer] Failed to restart with new backends, falling back to direct")
                self._write_and_start([])
        else:
            self.logger.warning("[Balancer] No working backends found")

    def update_available_configs(self, configs: List[Tuple[str, float]], trusted: bool = False) -> None:
        """Update the pool of available configs.
        If trusted=True, configs are pre-validated by benchmarker and used directly.
        """
        with self._lock:
            self._available_configs = self._filter_supported_configs(list(configs))
            self._failed_uris.clear()  # Reset failures on fresh configs
            if not self._running:
                return
        self.logger.info(f"[Balancer] Config pool updated: {len(self._available_configs)} configs (trusted={trusted})")
        if not self._available_configs:
            return
        if trusted:
            # Benchmarker already validated these - use directly without re-testing
            new_backends = self._backends_from_configs(self._available_configs, self.num_backends)
            if new_backends:
                if self._write_and_start(new_backends):
                    with self._lock:
                        self._backends = new_backends
                    self.logger.info(f"[Balancer] Updated with {len(new_backends)} trusted backends")
                else:
                    with self._lock:
                        self._backends = []
                    self.logger.warning("[Balancer] Failed to start with trusted backends, falling back to direct")
                    self._write_and_start([])
            else:
                self.logger.warning(f"[Balancer] Trusted configs provided but none parsed successfully")
        elif not self._backends:
            new_backends = self._find_working_backends(self.num_backends)
            if new_backends:
                with self._lock:
                    self._backends = new_backends
                self._write_and_start(new_backends)

    def stop(self) -> None:
        with self._lock:
            self._running = False
        if self._process:
            try:
                self._process.terminate()
                self._process.wait(timeout=3)
            except Exception:
                pass
        if self._config_path and os.path.exists(self._config_path):
            try:
                os.remove(self._config_path)
            except Exception:
                pass

    def get_status(self) -> Dict[str, Any]:
        with self._lock:
            backends = list(self._backends)
            forced = self._forced_backend
            force_perm = self._force_permanent
        return {
            "running": self._running,
            "port": self.port,
            "backends": sum(1 for b in backends if b.get("healthy")),
            "total_backends": len(backends),
            "stats": self._stats.copy(),
            "forced_backend": forced[:80] if forced else None,
            "force_permanent": force_perm,
            "available_pool": len(self._available_configs),
        }
