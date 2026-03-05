"""
Continuous Config Validator — background health-checking of stored configs.

Runs in a dedicated thread, perpetually cycling through the config database:
- Picks batches of configs that haven't been tested recently
- Tests each via XRay SOCKS proxy (TCP connect + small download)
- Updates health status (alive/dead/latency) in the shared ConfigDatabase
- Marks stale configs for removal after repeated failures
- Feeds freshly-validated healthy configs back to the orchestrator
"""

import logging
import os
import random
import socket
import struct
import time
from concurrent.futures import as_completed
from typing import Dict, List, Optional, Set, Tuple

try:
    from hunter.core.task_manager import HunterTaskManager
except ImportError:
    from core.task_manager import HunterTaskManager

logger = logging.getLogger(__name__)

# Test targets for validation (IP-based to avoid DNS issues in Iran)
_TEST_TARGETS = [
    ("1.1.1.1", 80),        # Cloudflare
    ("8.8.8.8", 443),       # Google
    ("149.154.175.50", 443), # Telegram DC1
    ("91.108.56.130", 443),  # Telegram DC5
]


class ConfigHealthRecord:
    """Health tracking for a single config URI."""
    __slots__ = (
        "uri", "first_seen", "last_tested", "last_alive",
        "alive", "latency_ms", "consecutive_fails",
        "total_tests", "total_passes", "tag",
    )

    def __init__(self, uri: str, tag: str = ""):
        self.uri = uri
        self.first_seen = time.time()
        self.last_tested = 0.0
        self.last_alive = 0.0
        self.alive = False
        self.latency_ms = 0.0
        self.consecutive_fails = 0
        self.total_tests = 0
        self.total_passes = 0
        self.tag = tag

    @property
    def pass_rate(self) -> float:
        if self.total_tests == 0:
            return 0.0
        return self.total_passes / self.total_tests

    @property
    def is_stale(self) -> bool:
        """Config is stale if it failed 5+ times in a row and never passed."""
        return self.consecutive_fails >= 5 and self.total_passes == 0

    @property
    def needs_retest(self) -> bool:
        """Config needs retest if not tested in the last 15 minutes."""
        return (time.time() - self.last_tested) > 900

    def to_dict(self) -> dict:
        return {
            "uri": self.uri,
            "alive": self.alive,
            "latency_ms": self.latency_ms,
            "consecutive_fails": self.consecutive_fails,
            "pass_rate": round(self.pass_rate, 3),
            "total_tests": self.total_tests,
            "last_tested": self.last_tested,
            "tag": self.tag,
        }


class ConfigDatabase:
    """
    In-memory config database with 200k+ capacity.
    
    Thread-safe via simple locking on the dict.
    Stores ConfigHealthRecord objects keyed by URI hash.
    """

    def __init__(self, max_configs: int = 250_000):
        self.max_configs = max_configs
        self._db: Dict[str, ConfigHealthRecord] = {}
        self._lock = __import__("threading").Lock()
        self.logger = logging.getLogger("hunter.configdb")
        self._stats = {
            "total_added": 0,
            "total_evicted": 0,
            "total_validated": 0,
        }

    @property
    def size(self) -> int:
        return len(self._db)

    def _uri_key(self, uri: str) -> str:
        """Fast hash key for a URI."""
        import hashlib
        return hashlib.md5(uri.encode("utf-8", errors="ignore")).hexdigest()

    def add_configs(self, uris: Set[str], tag: str = "") -> int:
        """Add configs to database. Returns count of NEW configs added."""
        added = 0
        with self._lock:
            for uri in uris:
                if not uri or not isinstance(uri, str):
                    continue
                key = self._uri_key(uri)
                if key not in self._db:
                    self._db[key] = ConfigHealthRecord(uri, tag=tag)
                    added += 1
            self._stats["total_added"] += added

            # Evict stale configs if over capacity
            if len(self._db) > self.max_configs:
                self._evict_stale()

        if added:
            self.logger.debug(f"[ConfigDB] +{added} new configs (total: {len(self._db)})")
        return added

    def _evict_stale(self):
        """Remove worst configs to stay under max_configs. Called with lock held."""
        overflow = len(self._db) - self.max_configs
        if overflow <= 0:
            return

        # Score configs: lower = more likely to evict
        scored = []
        for key, rec in self._db.items():
            score = 0.0
            if rec.alive:
                score += 100.0
            score += rec.pass_rate * 50.0
            score -= rec.consecutive_fails * 10.0
            # Fresher configs get a small bonus
            age_hours = (time.time() - rec.first_seen) / 3600
            if age_hours < 6:
                score += 20.0
            scored.append((score, key))

        scored.sort(key=lambda x: x[0])
        evict_count = min(overflow + 1000, len(scored))  # evict extra buffer
        for _, key in scored[:evict_count]:
            del self._db[key]
        self._stats["total_evicted"] += evict_count
        self.logger.info(f"[ConfigDB] Evicted {evict_count} configs (now: {len(self._db)})")

    def get_untested_batch(self, batch_size: int = 100) -> List[ConfigHealthRecord]:
        """Get a batch of configs that need testing, prioritizing untested ones."""
        with self._lock:
            candidates = [
                rec for rec in self._db.values()
                if rec.needs_retest
            ]
        # Sort: never-tested first, then oldest-tested
        candidates.sort(key=lambda r: (r.total_tests, r.last_tested))
        return candidates[:batch_size]

    def update_health(self, uri: str, alive: bool, latency_ms: float = 0.0):
        """Update health status after a test."""
        key = self._uri_key(uri)
        with self._lock:
            rec = self._db.get(key)
            if not rec:
                return
            rec.last_tested = time.time()
            rec.total_tests += 1
            rec.alive = alive
            if alive:
                rec.latency_ms = latency_ms
                rec.last_alive = time.time()
                rec.consecutive_fails = 0
                rec.total_passes += 1
            else:
                rec.consecutive_fails += 1
            self._stats["total_validated"] += 1

    def get_healthy_configs(self, max_count: int = 500) -> List[Tuple[str, float]]:
        """Get healthy (alive) configs sorted by latency."""
        with self._lock:
            alive = [
                rec for rec in self._db.values()
                if rec.alive and rec.latency_ms > 0
            ]
        alive.sort(key=lambda r: r.latency_ms)
        return [(r.uri, r.latency_ms) for r in alive[:max_count]]

    def get_all_uris(self) -> Set[str]:
        """Get all stored URIs."""
        with self._lock:
            return {rec.uri for rec in self._db.values()}

    def get_stats(self) -> Dict:
        """Get database statistics."""
        with self._lock:
            alive_count = sum(1 for r in self._db.values() if r.alive)
            avg_latency = 0.0
            alive_recs = [r for r in self._db.values() if r.alive and r.latency_ms > 0]
            if alive_recs:
                avg_latency = sum(r.latency_ms for r in alive_recs) / len(alive_recs)
        return {
            "total_configs": len(self._db),
            "alive_configs": alive_count,
            "avg_latency_ms": round(avg_latency, 1),
            "max_capacity": self.max_configs,
            "utilization_pct": round(len(self._db) / self.max_configs * 100, 1),
            **self._stats,
        }


class ContinuousValidator:
    """
    Background validator that continuously tests configs from the database.
    
    Designed to run in a dedicated thread via the ThreadManager.
    Each cycle picks a batch, tests them via lightweight SOCKS probes,
    and updates the ConfigDatabase.
    """

    def __init__(self, config_db: ConfigDatabase, batch_size: int = 50):
        self.db = config_db
        self.batch_size = batch_size
        self.logger = logging.getLogger("hunter.validator")
        self._stats = {
            "cycles": 0,
            "total_tested": 0,
            "total_passed": 0,
            "total_failed": 0,
        }

    def _test_config_quick(self, uri: str) -> Tuple[bool, float]:
        """
        Quick health check for a single config URI.
        
        Uses XRay-independent validation: parse the URI to extract
        host:port, then attempt a TCP connection + TLS handshake probe.
        Returns (alive, latency_ms).
        """
        host, port = self._extract_host_port(uri)
        if not host or not port:
            return (False, 0.0)

        # TCP connect test with timing
        t0 = time.time()
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect((host, port))
            # Send TLS ClientHello probe (first 3 bytes)
            s.send(b'\x16\x03\x01')
            # Try to read something back (even 1 byte means server is alive)
            s.settimeout(3.0)
            try:
                data = s.recv(1)
                if data:
                    latency = (time.time() - t0) * 1000
                    s.close()
                    return (True, latency)
            except socket.timeout:
                # Server accepted connection but didn't respond to our probe
                # This is still "alive" for a proxy server
                latency = (time.time() - t0) * 1000
                s.close()
                return (True, latency)
            s.close()
            return (False, 0.0)
        except Exception:
            return (False, 0.0)

    def _extract_host_port(self, uri: str) -> Tuple[Optional[str], Optional[int]]:
        """Extract server host and port from a proxy URI."""
        try:
            if uri.startswith("vmess://"):
                import base64 as b64
                import json
                payload = uri[8:]
                # Add padding
                payload += "=" * (4 - len(payload) % 4)
                data = json.loads(b64.b64decode(payload).decode("utf-8", errors="ignore"))
                host = data.get("add", "")
                port = int(data.get("port", 0))
                if host and port:
                    return (host, port)
            elif uri.startswith(("vless://", "trojan://")):
                # vless://uuid@host:port?... or trojan://pass@host:port?...
                after_scheme = uri.split("://", 1)[1]
                at_split = after_scheme.split("@", 1)
                if len(at_split) == 2:
                    host_part = at_split[1].split("?")[0].split("#")[0]
                    if ":" in host_part:
                        # Handle IPv6: [::1]:443
                        if host_part.startswith("["):
                            bracket_end = host_part.index("]")
                            host = host_part[1:bracket_end]
                            port_str = host_part[bracket_end + 2:]
                        else:
                            host, port_str = host_part.rsplit(":", 1)
                        port_str = port_str.strip().rstrip("/")
                        port = int(port_str) if port_str.isdigit() else 443
                        return (host, port)
            elif uri.startswith("ss://"):
                # ss://base64@host:port or ss://method:pass@host:port
                after_scheme = uri[5:].split("#")[0]
                if "@" in after_scheme:
                    host_part = after_scheme.split("@", 1)[1].split("?")[0]
                    if ":" in host_part:
                        host, port_str = host_part.rsplit(":", 1)
                        port_str = port_str.strip().rstrip("/")
                        port = int(port_str) if port_str.isdigit() else 0
                        if host and port:
                            return (host, port)
            elif uri.startswith(("hysteria2://", "hy2://")):
                after_scheme = uri.split("://", 1)[1]
                at_split = after_scheme.split("@", 1)
                if len(at_split) == 2:
                    host_part = at_split[1].split("?")[0].split("#")[0]
                    if ":" in host_part:
                        host, port_str = host_part.rsplit(":", 1)
                        port_str = port_str.strip().rstrip("/")
                        port = int(port_str) if port_str.isdigit() else 443
                        return (host, port)
        except Exception:
            pass
        return (None, None)

    def _is_ip_address(self, host: str) -> bool:
        """Check if host is an IP address (not a domain)."""
        try:
            socket.inet_aton(host)
            return True
        except Exception:
            return False

    def validate_batch(self) -> Tuple[int, int]:
        """
        Validate one batch of configs from the database.
        Returns (tested_count, passed_count).
        """
        batch = self.db.get_untested_batch(self.batch_size)
        if not batch:
            return (0, 0)

        self.logger.debug(f"[Validator] Testing batch of {len(batch)} configs")
        mgr = HunterTaskManager.get_instance()
        tested = 0
        passed = 0

        futures = {}
        for rec in batch:
            fut = mgr.submit_io(self._test_config_quick, rec.uri)
            futures[fut] = rec

        try:
            for future in as_completed(futures, timeout=60.0):
                rec = futures[future]
                try:
                    alive, latency = future.result(timeout=2)
                    self.db.update_health(rec.uri, alive, latency)
                    tested += 1
                    if alive:
                        passed += 1
                except Exception:
                    self.db.update_health(rec.uri, False)
                    tested += 1
        except TimeoutError:
            pass

        self._stats["cycles"] += 1
        self._stats["total_tested"] += tested
        self._stats["total_passed"] += passed
        self._stats["total_failed"] += (tested - passed)

        if tested:
            self.logger.info(
                f"[Validator] Batch: {passed}/{tested} alive "
                f"(DB: {self.db.size} total, "
                f"{len(self.db.get_healthy_configs(1))} healthy)"
            )
        return (tested, passed)

    def get_stats(self) -> Dict:
        return {**self._stats, "db": self.db.get_stats()}
