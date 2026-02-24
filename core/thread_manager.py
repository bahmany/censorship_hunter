"""
Thread Manager - Hardware-aware thread orchestration for Hunter.

Each major operation runs in its own dedicated thread:
  1. ConfigScannerWorker  – discovers & validates configs every 30 min
  2. TelegramPublisher    – publishes configs to Telegram every 30 min
  3. BalancerWorker        – keeps load-balancer alive on 10808 / 10809
  4. HealthMonitor         – watches RAM / CPU and throttles workers
  5. Web dashboard         – (managed externally by Flask, listed for status)

All workers respect a global stop event and adapt to hardware limits.
"""

import asyncio
import gc
import logging
import os
import sys
import threading
import time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Dict, List, Optional, Tuple

try:
    import psutil
except ImportError:
    psutil = None


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

class WorkerState(Enum):
    IDLE = "idle"
    RUNNING = "running"
    SLEEPING = "sleeping"
    ERROR = "error"
    STOPPED = "stopped"


@dataclass
class HardwareProfile:
    """Snapshot of current hardware resources."""
    cpu_count: int = 4
    cpu_percent: float = 0.0
    ram_total_gb: float = 8.0
    ram_used_gb: float = 4.0
    ram_percent: float = 50.0
    # Derived limits
    max_scan_workers: int = 8
    max_scan_configs: int = 600
    scan_chunk_size: int = 50
    mode: str = "NORMAL"

    @staticmethod
    def detect() -> "HardwareProfile":
        """Detect current hardware and compute adaptive limits."""
        p = HardwareProfile()
        p.cpu_count = os.cpu_count() or 4

        if psutil:
            try:
                mem = psutil.virtual_memory()
                p.ram_total_gb = mem.total / (1024 ** 3)
                p.ram_used_gb = mem.used / (1024 ** 3)
                p.ram_percent = mem.percent
            except Exception:
                pass
            try:
                p.cpu_percent = psutil.cpu_percent(interval=0.3)
            except Exception:
                pass

        # Adaptive limits based on RAM
        base_workers = max(4, p.cpu_count)
        if p.ram_percent >= 95:
            p.max_scan_workers = max(2, base_workers // 4)
            p.max_scan_configs = 80
            p.scan_chunk_size = 20
            p.mode = "ULTRA-MINIMAL"
        elif p.ram_percent >= 90:
            p.max_scan_workers = max(4, base_workers // 2)
            p.max_scan_configs = 150
            p.scan_chunk_size = 30
            p.mode = "MINIMAL"
        elif p.ram_percent >= 85:
            p.max_scan_workers = max(6, base_workers)
            p.max_scan_configs = 250
            p.scan_chunk_size = 40
            p.mode = "REDUCED"
        elif p.ram_percent >= 75:
            p.max_scan_workers = max(8, base_workers + 2)
            p.max_scan_configs = 400
            p.scan_chunk_size = 50
            p.mode = "CONSERVATIVE"
        elif p.ram_percent >= 60:
            p.max_scan_workers = max(10, base_workers * 2)
            p.max_scan_configs = 700
            p.scan_chunk_size = 50
            p.mode = "MODERATE"
        else:
            p.max_scan_workers = max(12, base_workers * 2)
            p.max_scan_configs = 1000
            p.scan_chunk_size = 50
            p.mode = "NORMAL"

        return p


@dataclass
class WorkerStatus:
    """Status of a single worker thread."""
    name: str
    state: WorkerState = WorkerState.IDLE
    last_run: float = 0.0
    last_error: str = ""
    runs: int = 0
    errors: int = 0
    next_run_in: float = 0.0
    extra: Dict[str, Any] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Base worker
# ---------------------------------------------------------------------------

class BaseWorker:
    """Base class for all managed worker threads."""

    name: str = "base"
    interval_seconds: int = 1800  # 30 minutes

    def __init__(self, stop_event: threading.Event, hw_profile_fn: Callable[[], HardwareProfile]):
        self.logger = logging.getLogger(f"hunter.worker.{self.name}")
        self._stop = stop_event
        self._hw = hw_profile_fn
        self._thread: Optional[threading.Thread] = None
        self.status = WorkerStatus(name=self.name)
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    # -- public API --

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(
            target=self._run_loop, name=f"hunter-{self.name}", daemon=True
        )
        self._thread.start()
        self.logger.info(f"[{self.name}] Thread started")

    def join(self, timeout: float = 10.0):
        if self._thread:
            self._thread.join(timeout=timeout)

    # -- internal --

    def _run_loop(self):
        """Thread entry point – creates its own asyncio loop and runs periodic work."""
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._async_loop())
        except Exception as exc:
            self.logger.error(f"[{self.name}] Fatal: {exc}")
            self.status.state = WorkerState.ERROR
            self.status.last_error = str(exc)
        finally:
            try:
                self._loop.close()
            except Exception:
                pass

    async def _async_loop(self):
        """Periodic execution with sleep between runs."""
        while not self._stop.is_set():
            self.status.state = WorkerState.RUNNING
            try:
                await self.execute()
                self.status.runs += 1
                self.status.last_run = time.time()
                self.status.last_error = ""
            except Exception as exc:
                self.status.errors += 1
                self.status.last_error = str(exc)
                self.status.state = WorkerState.ERROR
                self.logger.error(f"[{self.name}] Error: {exc}")

            if self._stop.is_set():
                break

            # Sleep with countdown
            self.status.state = WorkerState.SLEEPING
            remaining = self.interval_seconds
            while remaining > 0 and not self._stop.is_set():
                self.status.next_run_in = remaining
                await asyncio.sleep(min(5, remaining))
                remaining -= 5

        self.status.state = WorkerState.STOPPED

    async def execute(self):
        """Override in subclass – the actual work to do each cycle."""
        raise NotImplementedError


# ---------------------------------------------------------------------------
# Concrete workers
# ---------------------------------------------------------------------------

class ConfigScannerWorker(BaseWorker):
    """Discovers and validates new proxy configs."""
    name = "config_scanner"
    interval_seconds = 1800  # 30 minutes

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator

    async def execute(self):
        hw = self._hw()
        self.logger.info(
            f"[Scanner] Starting cycle (mode={hw.mode}, "
            f"RAM={hw.ram_percent:.0f}%, workers={hw.max_scan_workers})"
        )
        self.status.extra["mode"] = hw.mode
        self.status.extra["ram_percent"] = hw.ram_percent

        # Adapt orchestrator config to hardware
        self.orch.config.set("max_total", hw.max_scan_configs)
        self.orch.config.set("max_workers", hw.max_scan_workers)

        try:
            ran = await self.orch.run_cycle()
            self.status.extra["last_cycle_ok"] = ran
            self.status.extra["validated"] = self.orch._last_validated_count
        except Exception as exc:
            self.status.extra["last_cycle_ok"] = False
            raise

        # Adaptive interval based on results
        if self.orch._last_validated_count == 0:
            self.interval_seconds = max(300, 1800 // 3)  # Retry sooner
            self.logger.info("[Scanner] No configs found, retrying in 10 min")
        elif self.orch._last_validated_count < 5:
            self.interval_seconds = max(600, 1800 // 2)
            self.logger.info("[Scanner] Few configs, retrying in 15 min")
        else:
            self.interval_seconds = 1800
            self.logger.info(
                f"[Scanner] Good cycle ({self.orch._last_validated_count} configs), "
                f"next in 30 min"
            )

        # Force GC after scan
        gc.collect()


class TelegramPublisherWorker(BaseWorker):
    """Publishes validated configs to Telegram group."""
    name = "telegram_publisher"
    interval_seconds = 1800  # 30 minutes

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._publish_count = 0

    async def execute(self):
        reporters = [
            r for r in (
                getattr(self.orch, 'bot_reporter', None),
                getattr(self.orch, 'telegram_reporter', None),
            ) if r is not None
        ]
        if not reporters:
            self.logger.info("[TelegramPub] No reporters configured, skipping")
            self.status.extra["reason"] = "no_reporters"
            return

        # Collect URIs from gold/silver files
        gold_file = str(self.orch.config.get("gold_file", "") or "")
        silver_file = str(self.orch.config.get("silver_file", "") or "")
        all_uris: List[str] = []
        for fpath in [gold_file, silver_file]:
            if fpath:
                try:
                    from hunter.core.utils import read_lines
                    all_uris.extend(read_lines(fpath))
                except Exception:
                    pass

        if not all_uris:
            # Try balancer cache
            try:
                if self.orch._last_good_configs:
                    all_uris = [uri for uri, _ in self.orch._last_good_configs]
            except Exception:
                pass

        if not all_uris:
            self.logger.info("[TelegramPub] No configs to publish")
            self.status.extra["reason"] = "no_configs"
            return

        self.logger.info(f"[TelegramPub] Publishing {len(all_uris)} configs...")
        self.status.extra["config_count"] = len(all_uris)

        try:
            ok = await self.orch._publish_uris_to_targets(all_uris, force=True)
            if ok:
                self._publish_count += 1
                self.status.extra["publish_count"] = self._publish_count
                self.logger.info(f"[TelegramPub] Published successfully (#{self._publish_count})")
            else:
                self.logger.warning("[TelegramPub] Publish returned False")
                self.status.extra["reason"] = "publish_failed"
        except Exception as exc:
            self.logger.error(f"[TelegramPub] Publish error: {exc}")
            raise


class BalancerWorker(BaseWorker):
    """Keeps load-balancer alive on configured ports."""
    name = "balancer"
    interval_seconds = 60  # Health-check every 60s

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._started = False

    async def execute(self):
        # Initial start
        if not self._started:
            await self._start_balancers()
            self._started = True
            return

        # Health check
        await self._health_check()

    async def _start_balancers(self):
        """Start main and gemini balancers."""
        bal = self.orch.balancer
        if bal is not None:
            try:
                seed = self.orch._load_balancer_cache(name="HUNTER_balancer_cache.json")
                bal.start(initial_configs=seed if seed else None)
                port = bal.port
                self.logger.info(f"[Balancer] Main balancer started on port {port}")
                self.status.extra["main_port"] = port
                self.status.extra["main_backends"] = len(seed) if seed else 0
            except Exception as exc:
                self.logger.error(f"[Balancer] Main start failed: {exc}")

        gbal = self.orch.gemini_balancer
        if gbal is not None:
            try:
                gseed = self.orch._load_balancer_cache(name="HUNTER_gemini_balancer_cache.json")
                gbal.start(initial_configs=gseed if gseed else None)
                port = gbal.port
                self.logger.info(f"[Balancer] Gemini balancer started on port {port}")
                self.status.extra["gemini_port"] = port
            except Exception as exc:
                self.logger.error(f"[Balancer] Gemini start failed: {exc}")

    async def _health_check(self):
        """Check balancer health and restart if needed."""
        bal = self.orch.balancer
        if bal is not None:
            try:
                import socket
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(3)
                result = s.connect_ex(("127.0.0.1", bal.port))
                s.close()
                alive = result == 0
                self.status.extra["main_alive"] = alive
                if not alive:
                    self.logger.warning(f"[Balancer] Port {bal.port} not responding, restarting...")
                    bal.stop()
                    await asyncio.sleep(1)
                    seed = self.orch._load_balancer_cache(name="HUNTER_balancer_cache.json")
                    bal.start(initial_configs=seed if seed else None)
            except Exception as exc:
                self.logger.error(f"[Balancer] Health check error: {exc}")


class HealthMonitorWorker(BaseWorker):
    """Monitors system health and triggers GC / throttling."""
    name = "health_monitor"
    interval_seconds = 30  # Check every 30 seconds

    def __init__(self, stop_event, hw_fn, workers: List[BaseWorker] = None):
        super().__init__(stop_event, hw_fn)
        self.workers = workers or []
        self._gc_triggered = 0

    async def execute(self):
        hw = self._hw()
        self.status.extra.update({
            "cpu_count": hw.cpu_count,
            "cpu_percent": hw.cpu_percent,
            "ram_percent": hw.ram_percent,
            "ram_total_gb": round(hw.ram_total_gb, 1),
            "ram_used_gb": round(hw.ram_used_gb, 1),
            "mode": hw.mode,
            "thread_count": threading.active_count(),
        })

        # Aggressive GC if memory is high
        if hw.ram_percent >= 85:
            gc.collect()
            self._gc_triggered += 1
            self.status.extra["gc_triggered"] = self._gc_triggered
            if hw.ram_percent >= 90:
                gc.collect(0)
                gc.collect(1)
                gc.collect(2)
                self.logger.warning(
                    f"[Health] RAM critical: {hw.ram_percent:.0f}% "
                    f"({hw.ram_used_gb:.1f}/{hw.ram_total_gb:.1f} GB) — forced GC"
                )

        # Collect worker statuses
        worker_states = {}
        for w in self.workers:
            worker_states[w.name] = {
                "state": w.status.state.value,
                "runs": w.status.runs,
                "errors": w.status.errors,
                "next_in": int(w.status.next_run_in),
                "last_error": w.status.last_error[:100] if w.status.last_error else "",
            }
        self.status.extra["workers"] = worker_states


# ---------------------------------------------------------------------------
# Thread Manager
# ---------------------------------------------------------------------------

class ThreadManager:
    """Central thread orchestrator — manages all worker threads.
    
    Usage:
        mgr = ThreadManager(orchestrator)
        mgr.start_all()       # non-blocking, starts all workers
        ...
        mgr.stop_all()        # graceful shutdown
    """

    def __init__(self, orchestrator):
        self.logger = logging.getLogger("hunter.thread_manager")
        self.orch = orchestrator
        self._stop_event = threading.Event()
        self._hw_cache: Optional[HardwareProfile] = None
        self._hw_cache_ts: float = 0.0

        # Create workers
        self.scanner = ConfigScannerWorker(orchestrator, self._stop_event, self._get_hw)
        self.telegram = TelegramPublisherWorker(orchestrator, self._stop_event, self._get_hw)
        self.balancer = BalancerWorker(orchestrator, self._stop_event, self._get_hw)

        all_workers = [self.scanner, self.telegram, self.balancer]
        self.health = HealthMonitorWorker(self._stop_event, self._get_hw, workers=all_workers)

        self._workers: List[BaseWorker] = [
            self.health,
            self.balancer,
            self.scanner,
            self.telegram,
        ]

    def _get_hw(self) -> HardwareProfile:
        """Cached hardware profile (refreshed every 10s)."""
        now = time.time()
        if self._hw_cache is None or (now - self._hw_cache_ts) > 10:
            self._hw_cache = HardwareProfile.detect()
            self._hw_cache_ts = now
        return self._hw_cache

    def start_all(self):
        """Start all worker threads."""
        hw = self._get_hw()
        self.logger.info(
            f"ThreadManager starting — {hw.cpu_count} CPUs, "
            f"{hw.ram_total_gb:.1f} GB RAM ({hw.ram_percent:.0f}% used), "
            f"mode={hw.mode}"
        )
        for w in self._workers:
            try:
                w.start()
            except Exception as exc:
                self.logger.error(f"Failed to start {w.name}: {exc}")

        self.logger.info(f"All {len(self._workers)} workers started")

    def stop_all(self, timeout: float = 15.0):
        """Graceful shutdown of all workers."""
        self.logger.info("ThreadManager: stopping all workers...")
        self._stop_event.set()

        # Join all threads
        for w in self._workers:
            try:
                w.join(timeout=timeout / len(self._workers))
            except Exception:
                pass

        self.logger.info("ThreadManager: all workers stopped")

    def get_status(self) -> Dict[str, Any]:
        """Get status of all workers."""
        hw = self._get_hw()
        result = {
            "hardware": {
                "cpu_count": hw.cpu_count,
                "cpu_percent": hw.cpu_percent,
                "ram_total_gb": round(hw.ram_total_gb, 1),
                "ram_used_gb": round(hw.ram_used_gb, 1),
                "ram_percent": round(hw.ram_percent, 1),
                "mode": hw.mode,
                "thread_count": threading.active_count(),
            },
            "workers": {},
        }
        for w in self._workers:
            result["workers"][w.name] = {
                "state": w.status.state.value,
                "runs": w.status.runs,
                "errors": w.status.errors,
                "last_run": w.status.last_run,
                "next_run_in": int(w.status.next_run_in),
                "last_error": w.status.last_error[:200] if w.status.last_error else "",
                "interval": w.interval_seconds,
                **w.status.extra,
            }
        return result

    def is_running(self) -> bool:
        return not self._stop_event.is_set()
