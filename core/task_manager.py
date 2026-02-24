"""
Unified Task Manager — Single source of truth for all concurrency in Hunter.

Replaces the fragmented threading architecture (3 separate thread pool systems)
with one shared, hardware-aware, adaptive pool that every module draws from.

Architecture:
    ┌─────────────────────────────────────┐
    │         HunterTaskManager           │
    │  (singleton — one per process)      │
    ├──────────┬──────────┬───────────────┤
    │ IO Pool  │ CPU Pool │  AsyncRunner  │
    │ (shared  │ (shared  │  (run sync    │
    │  TPE for │  TPE for │   code from   │
    │  network)│  parse)  │   async ctx)  │
    └──────────┴──────────┴───────────────┘

Design principles:
  - One shared ThreadPoolExecutor for I/O-bound work (network, XRay tests)
  - One shared ThreadPoolExecutor for CPU-bound work (parsing, obfuscation)
  - Hardware-aware pool sizing with live adaptive scaling
  - Memory pressure detection with automatic throttling
  - Semaphore-based concurrency limiting per task type
  - Circuit breaker for repeated failures
  - Clean shutdown with proper resource cleanup
"""

import asyncio
import gc
import logging
import os
import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple, TypeVar

try:
    import psutil
except ImportError:
    psutil = None

logger = logging.getLogger(__name__)

T = TypeVar("T")


# ---------------------------------------------------------------------------
# Hardware profiling
# ---------------------------------------------------------------------------

class ResourceMode(Enum):
    NORMAL = "NORMAL"
    MODERATE = "MODERATE"
    SCALED = "SCALED"
    CONSERVATIVE = "CONSERVATIVE"
    REDUCED = "REDUCED"
    MINIMAL = "MINIMAL"
    ULTRA_MINIMAL = "ULTRA-MINIMAL"


@dataclass
class HardwareSnapshot:
    """Point-in-time snapshot of system resources."""
    cpu_count: int = 4
    cpu_percent: float = 0.0
    ram_total_gb: float = 8.0
    ram_used_gb: float = 4.0
    ram_percent: float = 50.0
    mode: ResourceMode = ResourceMode.NORMAL
    io_pool_size: int = 12
    cpu_pool_size: int = 4
    max_configs: int = 1000
    scan_chunk: int = 50

    @staticmethod
    def detect() -> "HardwareSnapshot":
        snap = HardwareSnapshot()
        snap.cpu_count = os.cpu_count() or 4

        if psutil:
            try:
                mem = psutil.virtual_memory()
                snap.ram_total_gb = mem.total / (1024 ** 3)
                snap.ram_used_gb = mem.used / (1024 ** 3)
                snap.ram_percent = mem.percent
            except Exception:
                pass
            try:
                snap.cpu_percent = psutil.cpu_percent(interval=0.2)
            except Exception:
                pass

        base = max(4, snap.cpu_count)

        if snap.ram_percent >= 95:
            snap.mode = ResourceMode.ULTRA_MINIMAL
            snap.io_pool_size = max(2, base // 4)
            snap.cpu_pool_size = 2
            snap.max_configs = 80
            snap.scan_chunk = 20
        elif snap.ram_percent >= 90:
            snap.mode = ResourceMode.MINIMAL
            snap.io_pool_size = max(4, base // 2)
            snap.cpu_pool_size = 2
            snap.max_configs = 150
            snap.scan_chunk = 30
        elif snap.ram_percent >= 85:
            snap.mode = ResourceMode.REDUCED
            snap.io_pool_size = max(6, base)
            snap.cpu_pool_size = max(2, base // 2)
            snap.max_configs = 250
            snap.scan_chunk = 40
        elif snap.ram_percent >= 80:
            snap.mode = ResourceMode.CONSERVATIVE
            snap.io_pool_size = max(8, base + 2)
            snap.cpu_pool_size = max(2, base // 2)
            snap.max_configs = 400
            snap.scan_chunk = 50
        elif snap.ram_percent >= 70:
            snap.mode = ResourceMode.SCALED
            snap.io_pool_size = max(10, base + 4)
            snap.cpu_pool_size = max(3, base // 2)
            snap.max_configs = 600
            snap.scan_chunk = 50
        elif snap.ram_percent >= 60:
            snap.mode = ResourceMode.MODERATE
            snap.io_pool_size = max(12, base * 2)
            snap.cpu_pool_size = max(4, base)
            snap.max_configs = 800
            snap.scan_chunk = 50
        else:
            snap.mode = ResourceMode.NORMAL
            snap.io_pool_size = max(12, base * 2)
            snap.cpu_pool_size = max(4, base)
            snap.max_configs = 1000
            snap.scan_chunk = 50

        return snap


# ---------------------------------------------------------------------------
# Circuit Breaker
# ---------------------------------------------------------------------------

class CircuitBreaker:
    """Lightweight circuit breaker for task categories."""

    def __init__(self, threshold: int = 5, reset_timeout: float = 60.0):
        self.threshold = threshold
        self.reset_timeout = reset_timeout
        self._failures = 0
        self._last_failure: float = 0.0
        self._open = False
        self._lock = threading.Lock()

    def record_success(self):
        with self._lock:
            self._failures = 0
            self._open = False

    def record_failure(self) -> bool:
        with self._lock:
            self._failures += 1
            self._last_failure = time.time()
            if self._failures >= self.threshold:
                self._open = True
                return True
            return False

    def allow(self) -> bool:
        with self._lock:
            if not self._open:
                return True
            if (time.time() - self._last_failure) > self.reset_timeout:
                self._failures = 0
                self._open = False
                return True
            return False

    @property
    def is_open(self) -> bool:
        with self._lock:
            return self._open


# ---------------------------------------------------------------------------
# Main Task Manager (singleton per process)
# ---------------------------------------------------------------------------

class HunterTaskManager:
    """
    Unified concurrency manager for Hunter.

    Provides:
      - Shared I/O thread pool  (network fetches, XRay benchmark tests)
      - Shared CPU thread pool  (config parsing, obfuscation)
      - Adaptive pool resizing based on live hardware snapshot
      - Memory-pressure GC triggers
      - run_in_io / run_in_cpu / run_batch / run_async helpers
      - Named circuit breakers for task categories
      - Clean shutdown

    Usage:
        mgr = HunterTaskManager.get_instance()
        future = mgr.submit_io(fetch_url, url)
        result = future.result(timeout=15)

        # Or batch:
        results = mgr.run_batch_io(fetch_url, urls, timeout=25)

        # From async code:
        result = await mgr.run_in_io_async(fetch_url, url)
    """

    _instance: Optional["HunterTaskManager"] = None
    _instance_lock = threading.Lock()

    @classmethod
    def get_instance(cls) -> "HunterTaskManager":
        if cls._instance is None:
            with cls._instance_lock:
                if cls._instance is None:
                    cls._instance = cls()
        return cls._instance

    @classmethod
    def reset_instance(cls):
        """Tear down singleton (for tests)."""
        with cls._instance_lock:
            if cls._instance is not None:
                cls._instance.shutdown()
                cls._instance = None

    def __init__(self):
        self._hw: Optional[HardwareSnapshot] = None
        self._hw_ts: float = 0.0
        self._hw_ttl: float = 10.0  # refresh every 10s

        # Detect hardware once at init
        self._hw = HardwareSnapshot.detect()
        self._hw_ts = time.time()

        # I/O pool — network fetches, XRay subprocess benchmarks
        self._io_pool = ThreadPoolExecutor(
            max_workers=self._hw.io_pool_size,
            thread_name_prefix="hunter-io",
        )
        # CPU pool — parsing, obfuscation, compression
        self._cpu_pool = ThreadPoolExecutor(
            max_workers=self._hw.cpu_pool_size,
            thread_name_prefix="hunter-cpu",
        )

        # Metrics
        self._io_submitted = 0
        self._io_completed = 0
        self._io_failed = 0
        self._cpu_submitted = 0
        self._cpu_completed = 0
        self._metrics_lock = threading.Lock()

        # Circuit breakers keyed by category
        self._breakers: Dict[str, CircuitBreaker] = {}
        self._breakers_lock = threading.Lock()

        # GC bookkeeping
        self._last_gc: float = 0.0

        self._running = True

        logger.info(
            f"HunterTaskManager started — IO pool: {self._hw.io_pool_size}, "
            f"CPU pool: {self._hw.cpu_pool_size}, "
            f"mode: {self._hw.mode.value}, "
            f"RAM: {self._hw.ram_percent:.0f}%"
        )

    # ── Hardware snapshot (cached) ──────────────────────────────────────

    def get_hardware(self) -> HardwareSnapshot:
        now = time.time()
        if (now - self._hw_ts) > self._hw_ttl:
            self._hw = HardwareSnapshot.detect()
            self._hw_ts = now
        return self._hw

    # ── Pool resizing ───────────────────────────────────────────────────

    def maybe_resize(self):
        """Resize pools if hardware conditions changed significantly."""
        hw = self.get_hardware()
        io_target = hw.io_pool_size
        cpu_target = hw.cpu_pool_size

        # ThreadPoolExecutor._max_workers is writable in CPython 3.8+
        try:
            if self._io_pool._max_workers != io_target:
                self._io_pool._max_workers = io_target
                logger.debug(f"IO pool resized → {io_target}")
        except Exception:
            pass
        try:
            if self._cpu_pool._max_workers != cpu_target:
                self._cpu_pool._max_workers = cpu_target
                logger.debug(f"CPU pool resized → {cpu_target}")
        except Exception:
            pass

        # Trigger GC under memory pressure (at most once per 30s)
        if hw.ram_percent >= 85 and (time.time() - self._last_gc) > 30:
            gc.collect(0)
            gc.collect(1)
            self._last_gc = time.time()
            if hw.ram_percent >= 95:
                gc.collect(2)
                logger.warning(
                    f"[TaskManager] RAM critical: {hw.ram_percent:.0f}% — forced full GC"
                )

    # ── Circuit breakers ────────────────────────────────────────────────

    def get_breaker(self, name: str, threshold: int = 5, reset_s: float = 60.0) -> CircuitBreaker:
        with self._breakers_lock:
            if name not in self._breakers:
                self._breakers[name] = CircuitBreaker(threshold, reset_s)
            return self._breakers[name]

    # ── Submission helpers ──────────────────────────────────────────────

    def submit_io(self, fn: Callable[..., T], *args, **kwargs) -> Future:
        """Submit an I/O-bound task to the shared IO pool."""
        self.maybe_resize()
        with self._metrics_lock:
            self._io_submitted += 1
        return self._io_pool.submit(fn, *args, **kwargs)

    def submit_cpu(self, fn: Callable[..., T], *args, **kwargs) -> Future:
        """Submit a CPU-bound task to the shared CPU pool."""
        with self._metrics_lock:
            self._cpu_submitted += 1
        return self._cpu_pool.submit(fn, *args, **kwargs)

    def run_batch_io(
        self,
        fn: Callable,
        items: Sequence,
        *,
        timeout: float = 30.0,
        max_concurrent: Optional[int] = None,
        on_result: Optional[Callable[[Any, Any], None]] = None,
    ) -> List[Tuple[Any, Any]]:
        """
        Run *fn(item)* for each item in *items* on the IO pool.

        Returns list of (item, result) tuples.
        Failed items return (item, None).
        """
        self.maybe_resize()
        results: List[Tuple[Any, Any]] = []

        sem = threading.Semaphore(max_concurrent or self.get_hardware().io_pool_size)

        def _wrapped(item):
            sem.acquire()
            try:
                return fn(item)
            finally:
                sem.release()

        futures = {}
        for item in items:
            f = self._io_pool.submit(_wrapped, item)
            futures[f] = item

        try:
            for future in as_completed(futures, timeout=timeout):
                item = futures[future]
                try:
                    result = future.result(timeout=1)
                    results.append((item, result))
                    if on_result:
                        on_result(item, result)
                    with self._metrics_lock:
                        self._io_completed += 1
                except Exception:
                    results.append((item, None))
                    with self._metrics_lock:
                        self._io_failed += 1
        except TimeoutError:
            logger.warning(f"[TaskManager] Batch timeout after {timeout}s, got {len(results)}/{len(futures)}")
            for f in futures:
                f.cancel()

        return results

    # ── Async bridges ───────────────────────────────────────────────────

    async def run_in_io_async(self, fn: Callable[..., T], *args, **kwargs) -> T:
        """Run a sync function on IO pool from async context."""
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(self._io_pool, lambda: fn(*args, **kwargs))

    async def run_in_cpu_async(self, fn: Callable[..., T], *args, **kwargs) -> T:
        """Run a sync function on CPU pool from async context."""
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(self._cpu_pool, lambda: fn(*args, **kwargs))

    async def run_batch_io_async(
        self,
        fn: Callable,
        items: Sequence,
        *,
        timeout: float = 30.0,
    ) -> List[Tuple[Any, Any]]:
        """Async version of run_batch_io."""
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            None,
            lambda: self.run_batch_io(fn, items, timeout=timeout),
        )

    # ── Timed execution with timeout ────────────────────────────────────

    def run_with_timeout(
        self,
        fn: Callable[..., T],
        timeout: float,
        *args,
        pool: str = "io",
        **kwargs,
    ) -> Optional[T]:
        """Run fn with a hard timeout. Returns None on timeout/error."""
        executor = self._io_pool if pool == "io" else self._cpu_pool
        future = executor.submit(fn, *args, **kwargs)
        try:
            return future.result(timeout=timeout)
        except Exception:
            future.cancel()
            return None

    # ── Metrics ─────────────────────────────────────────────────────────

    def get_metrics(self) -> Dict[str, Any]:
        hw = self.get_hardware()
        with self._metrics_lock:
            return {
                "io_submitted": self._io_submitted,
                "io_completed": self._io_completed,
                "io_failed": self._io_failed,
                "cpu_submitted": self._cpu_submitted,
                "cpu_completed": self._cpu_completed,
                "io_pool_size": hw.io_pool_size,
                "cpu_pool_size": hw.cpu_pool_size,
                "mode": hw.mode.value,
                "ram_percent": hw.ram_percent,
                "cpu_count": hw.cpu_count,
                "thread_count": threading.active_count(),
            }

    # ── Shutdown ────────────────────────────────────────────────────────

    def shutdown(self, wait: bool = True, timeout: float = 10.0):
        if not self._running:
            return
        self._running = False
        logger.info("[TaskManager] Shutting down pools...")
        try:
            self._io_pool.shutdown(wait=wait, cancel_futures=True)
        except TypeError:
            # Python < 3.9 doesn't have cancel_futures
            self._io_pool.shutdown(wait=wait)
        try:
            self._cpu_pool.shutdown(wait=wait, cancel_futures=True)
        except TypeError:
            self._cpu_pool.shutdown(wait=wait)
        logger.info("[TaskManager] Shutdown complete")
