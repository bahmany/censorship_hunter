"""
Load balancer that orchestrates multiple backends for the hunter gateway.
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional, Set, Tuple

from hunter.core.models import HunterParsedConfig
from hunter.core.utils import (resolve_executable_path, kill_process_on_port, now_ts, read_lines, append_unique_lines)
from hunter.parsers import UniversalParser
from hunter.security.obfuscation import StealthObfuscationEngine

XRAY_PATH_FALLBACKS = [
    os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "xray.exe")
]

IRAN_FRAGMENT_ENABLED = os.getenv("IRAN_FRAGMENT_ENABLED", "false").lower() == "true"


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
        self._parser = UniversalParser()

    def _create_balanced_config(self, backends: List[Dict[str, Any]]) -> Dict[str, Any]:
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
            outbound = parsed.outbound.copy()
            outbound["tag"] = f"proxy-{idx}"
            if self.obfuscation_engine and self.obfuscation_engine.enabled:
                outbound = self.obfuscation_engine.apply_obfuscation_to_config(outbound)
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

        config = {
            "log": {"loglevel": "warning"},
            "inbounds": [{
                "port": self.port,
                "listen": "0.0.0.0",
                "protocol": "socks",
                "settings": {"auth": "noauth", "udp": True},
                "sniffing": {"enabled": True, "destOverride": ["http", "tls", "quic"], "routeOnly": False},
            }],
            "outbounds": outbounds,
            "routing": {
                "domainStrategy": "AsIs",
                "balancers": [{"tag": "balancer", "selector": selectors, "strategy": {"type": "random"}}],
                "rules": [{"type": "field", "inboundTag": ["socks"], "balancerTag": "balancer"}],
            },
            "dns": {"servers": ["https://cloudflare-dns.com/dns-query", "https://dns.google/dns-query", "1.1.1.1", "8.8.8.8"]},
        }
        return config

    def _write_and_start(self, backends: List[Dict[str, Any]]) -> bool:
        config = self._create_balanced_config(backends)
        kill_process_on_port(self.port)
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
                return False
            self._stats["restarts"] += 1
            self._stats["last_restart"] = now_ts()
            return True
        except Exception:
            return False

    def _test_backend(self, uri: str, timeout: int = 8) -> Optional[float]:
        parsed = self._parser.parse(uri)
        if not parsed:
            return None
        port = self.port + 100 + (hash(uri) % 50)
        config = {
            "log": {"loglevel": "warning"},
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
            time.sleep(2)
            if process.poll() is not None:
                return None
            curl_cmd = "curl.exe" if sys.platform == "win32" else "curl"
            result = subprocess.run(
                [curl_cmd, "-x", f"socks5h://127.0.0.1:{port}", "-s", "-o", "nul" if sys.platform == "win32" else "/dev/null", "-w", "%{http_code}", "-m", str(timeout), "-k", "https://cp.cloudflare.com/"],
                capture_output=True, text=True, timeout=timeout + 3,
            )
            if result.returncode == 0:
                try:
                    code = int(result.stdout.strip())
                    if code < 400 or code == 204:
                        return float(result.returncode)
                except ValueError:
                    pass
            return None
        except Exception:
            return None
        finally:
            if process:
                try:
                    process.terminate()
                    process.wait(timeout=1)
                except Exception:
                    pass
            if temp_path and os.path.exists(temp_path):
                try:
                    os.remove(temp_path)
                except Exception:
                    pass

    def _find_working_backends(self, count: int = 5) -> List[Dict[str, Any]]:
        with self._lock:
            configs = list(self._available_configs)
        configs.sort(key=lambda x: x[1])
        working = []
        for uri, latency in configs:
            if len(working) >= count:
                break
            if uri in self._failed_uris:
                continue
            latency = self._test_backend(uri)
            if latency:
                working.append({"uri": uri, "latency": latency, "healthy": True, "added_at": now_ts()})
            else:
                self._failed_uris.add(uri)
        return working

    def start(self, initial_configs: Optional[List[Tuple[str, float]]] = None) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
        if initial_configs:
            with self._lock:
                self._available_configs = list(initial_configs)
        backends = self._find_working_backends(self.num_backends)
        if backends:
            self._backends = backends
            self._write_and_start(backends)
        self._health_thread = threading.Thread(target=self._health_check_loop, daemon=True)
        self._health_thread.start()

    def _health_check_loop(self) -> None:
        while self._running:
            time.sleep(self.health_check_interval)
            with self._lock:
                backends = list(self._backends)
            healthy = sum(1 for b in backends if b.get("healthy"))
            if healthy == 0 and self._available_configs:
                new_backends = self._find_working_backends(self.num_backends)
                if new_backends:
                    with self._lock:
                        self._backends = new_backends
                    self._write_and_start(new_backends)
                        
    def update_available_configs(self, configs: List[Tuple[str, float]]) -> None:
        with self._lock:
            self._available_configs = configs
            if not self._running:
                return
        if not self._backends:
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
        return {
            "running": self._running,
            "port": self.port,
            "backends": sum(1 for b in backends if b.get("healthy")),
            "total_backends": len(backends),
            "stats": self._stats.copy(),
        }
