"""
Thread Manager - Hardware-aware thread orchestration for Hunter.

Each major operation runs in its own dedicated thread:
  1. ConfigScannerWorker  - discovers & validates configs every 30 min
  2. TelegramPublisher    - publishes configs to Telegram every 30 min
  3. BalancerWorker       - keeps load-balancer alive on 10808 / 10809
  4. HealthMonitor        - watches RAM / CPU and throttles workers
  5. Web dashboard        - (managed externally by Flask, listed for status)

All workers respect a global stop event and adapt to hardware limits.
Hardware detection is delegated to the unified HunterTaskManager.
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
    from hunter.core.task_manager import HunterTaskManager, HardwareSnapshot
except ImportError:
    from core.task_manager import HunterTaskManager, HardwareSnapshot

# New aggressive harvesting components
AggressiveHarvester = None
ConfigDatabase = None
ContinuousValidator = None
DPIPressureEngine = None
try:
    from hunter.network.aggressive_harvester import AggressiveHarvester
    from hunter.network.continuous_validator import ConfigDatabase, ContinuousValidator
    from hunter.network.dpi_pressure import DPIPressureEngine
except ImportError:
    try:
        from network.aggressive_harvester import AggressiveHarvester
        from network.continuous_validator import ConfigDatabase, ContinuousValidator
        from network.dpi_pressure import DPIPressureEngine
    except ImportError:
        pass


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

    def __init__(self, stop_event: threading.Event, hw_profile_fn: Callable[[], HardwareSnapshot]):
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

# Shared lock to prevent harvester and scanner from fetching the same URLs simultaneously
_fetch_lock = threading.Lock()


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
            f"[Scanner] Starting cycle (mode={hw.mode.value}, "
            f"RAM={hw.ram_percent:.0f}%, workers={hw.io_pool_size})"
        )
        self.status.extra["mode"] = hw.mode.value
        self.status.extra["ram_percent"] = hw.ram_percent

        # Adapt orchestrator config to hardware via unified snapshot
        self.orch.config.set("max_total", hw.max_configs)
        self.orch.config.set("max_workers", hw.io_pool_size)

        acquired = _fetch_lock.acquire(timeout=120)
        try:
            ran = await self.orch.run_cycle()
            self.status.extra["last_cycle_ok"] = ran
            self.status.extra["validated"] = self.orch._last_validated_count
        except Exception as exc:
            self.status.extra["last_cycle_ok"] = False
            raise
        finally:
            if acquired:
                _fetch_lock.release()

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

        # Trigger pool resize + GC via unified task manager
        HunterTaskManager.get_instance().maybe_resize()


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
    """Monitors system health via HunterTaskManager."""
    name = "health_monitor"
    interval_seconds = 30  # Check every 30 seconds

    def __init__(self, stop_event, hw_fn, workers: List[BaseWorker] = None):
        super().__init__(stop_event, hw_fn)
        self.workers = workers or []
        self._gc_triggered = 0

    async def execute(self):
        hw = self._hw()
        mgr = HunterTaskManager.get_instance()

        self.status.extra.update({
            "cpu_count": hw.cpu_count,
            "cpu_percent": hw.cpu_percent,
            "ram_percent": hw.ram_percent,
            "ram_total_gb": round(hw.ram_total_gb, 1),
            "ram_used_gb": round(hw.ram_used_gb, 1),
            "mode": hw.mode.value,
            "thread_count": threading.active_count(),
        })

        # Delegate GC + pool resize to task manager
        mgr.maybe_resize()

        if hw.ram_percent >= 85:
            self._gc_triggered += 1
            self.status.extra["gc_triggered"] = self._gc_triggered
            if hw.ram_percent >= 90:
                self.logger.warning(
                    f"[Health] RAM critical: {hw.ram_percent:.0f}% "
                    f"({hw.ram_used_gb:.1f}/{hw.ram_total_gb:.1f} GB)"
                )

        # Include task manager metrics
        self.status.extra["task_manager"] = mgr.get_metrics()

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


class HarvesterWorker(BaseWorker):
    """Aggressive config harvester — collects tens of thousands of configs."""
    name = "harvester"
    interval_seconds = 1800  # 30 minutes
    initial_delay = 900  # 15 min offset so it doesn't overlap with scanner

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._harvester = None
        self._harvest_count = 0
        self._first_run = True

    def _get_harvester(self):
        if self._harvester is None and AggressiveHarvester is not None:
            extra_ports = []
            try:
                extra_ports.append(self.orch.config.get("multiproxy_port", 10808))
                extra_ports.append(self.orch.config.get("gemini_port", 10809))
            except Exception:
                pass
            self._harvester = AggressiveHarvester(extra_proxy_ports=extra_ports)
        return self._harvester

    async def execute(self):
        # Offset first run to avoid overlapping with scanner's initial cycle
        if self._first_run:
            self._first_run = False
            self.logger.info(f"[Harvester] Initial delay {self.initial_delay}s to stagger with scanner")
            remaining = self.initial_delay
            while remaining > 0 and not self._stop.is_set():
                await asyncio.sleep(min(5, remaining))
                remaining -= 5
            if self._stop.is_set():
                return

        harvester = self._get_harvester()
        if harvester is None:
            self.logger.info("[Harvester] AggressiveHarvester not available")
            return

        config_db = getattr(self.orch, 'config_db', None)
        if config_db is None:
            self.logger.info("[Harvester] ConfigDatabase not available on orchestrator")
            return

        # Acquire shared lock to prevent overlap with scanner
        acquired = _fetch_lock.acquire(timeout=120)
        if not acquired:
            self.logger.info("[Harvester] Scanner is running, skipping this cycle")
            return

        loop = asyncio.get_event_loop()
        mgr = HunterTaskManager.get_instance()

        try:
            self.logger.info("[Harvester] Starting aggressive harvest...")
            configs = await loop.run_in_executor(mgr._io_pool, harvester.harvest)
        finally:
            _fetch_lock.release()

        if configs:
            added = config_db.add_configs(configs, tag="harvest")
            self._harvest_count += 1
            self.status.extra["last_count"] = len(configs)
            self.status.extra["new_added"] = added
            self.status.extra["db_size"] = config_db.size
            self.status.extra["harvest_count"] = self._harvest_count
            self.logger.info(
                f"[Harvester] Got {len(configs)} configs, {added} new "
                f"(DB: {config_db.size})"
            )

            # Feed healthy configs from DB into orchestrator's cache
            try:
                healthy = config_db.get_healthy_configs(max_count=200)
                if healthy and hasattr(self.orch, '_last_good_configs'):
                    existing = {u for u, _ in self.orch._last_good_configs}
                    new_healthy = [(u, l) for u, l in healthy if u not in existing]
                    if new_healthy:
                        self.orch._last_good_configs.extend(new_healthy[:100])
                        self.logger.info(
                            f"[Harvester] Fed {len(new_healthy[:100])} healthy configs to orchestrator"
                        )
            except Exception as exc:
                self.logger.debug(f"[Harvester] Feed error: {exc}")
        else:
            self.logger.warning("[Harvester] No configs collected")
            self.status.extra["last_count"] = 0


class GitHubDownloaderWorker(BaseWorker):
    name = "github_downloader"
    interval_seconds = 1800
    initial_delay = 300

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._first_run = True
        self._seen: Optional[set] = None
        self._cache_path: Optional[str] = None
        self._download_count = 0

    def _cache_file(self) -> str:
        if self._cache_path:
            return self._cache_path
        base_dir = ""
        try:
            state_file = str(self.orch.config.get("state_file", "") or "")
            if state_file:
                base_dir = os.path.dirname(state_file)
        except Exception:
            base_dir = ""
        if not base_dir:
            base_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "runtime")
        try:
            os.makedirs(base_dir, exist_ok=True)
        except Exception:
            pass
        self._cache_path = os.path.join(base_dir, "HUNTER_github_configs_cache.txt")
        return self._cache_path

    def _load_seen(self) -> set:
        if self._seen is not None:
            return self._seen
        try:
            try:
                from hunter.core.utils import read_lines
            except ImportError:
                from core.utils import read_lines
            self._seen = set(read_lines(self._cache_file()))
        except Exception:
            self._seen = set()
        return self._seen

    def _append_new(self, configs: List[str]) -> int:
        seen = self._load_seen()
        new_lines = [c for c in configs if c and c not in seen]
        if not new_lines:
            return 0
        try:
            with open(self._cache_file(), "a", encoding="utf-8") as f:
                for c in new_lines:
                    f.write(c + "\n")
        except Exception:
            return 0
        for c in new_lines:
            seen.add(c)
        return len(new_lines)

    async def execute(self):
        self.interval_seconds = 1800
        enabled = os.getenv("HUNTER_GITHUB_BG_ENABLED", "true").lower() == "true"
        if not enabled:
            self.status.extra["reason"] = "disabled"
            return

        if self._first_run:
            self._first_run = False
            remaining = int(self.initial_delay)
            while remaining > 0 and not self._stop.is_set():
                await asyncio.sleep(min(5, remaining))
                remaining -= 5
            if self._stop.is_set():
                return

        fetcher = getattr(self.orch, "config_fetcher", None)
        if fetcher is None:
            self.status.extra["reason"] = "no_fetcher"
            return

        config_db = getattr(self.orch, "config_db", None)
        proxy_ports = []
        try:
            proxy_ports.append(self.orch.config.get("multiproxy_port", 10808))
            proxy_ports.append(self.orch.config.get("gemini_port", 10809))
        except Exception:
            pass

        try:
            cap = int(os.getenv("HUNTER_GITHUB_BG_CAP", "6000"))
        except Exception:
            cap = 6000
        cap = max(200, min(60_000, cap))

        acquired = _fetch_lock.acquire(timeout=120)
        if not acquired:
            self.status.extra["reason"] = "fetch_lock_busy"
            self.interval_seconds = 300
            return

        loop = asyncio.get_event_loop()
        mgr = HunterTaskManager.get_instance()
        try:
            configs = await loop.run_in_executor(
                mgr._io_pool,
                lambda: fetcher.fetch_github_configs(proxy_ports, max_configs=cap),
            )
        finally:
            try:
                _fetch_lock.release()
            except Exception:
                pass

        if not configs:
            self.logger.warning("[GitHubBG] No configs fetched")
            self.status.extra["last_count"] = 0
            self.interval_seconds = 300
            return

        if isinstance(configs, set):
            configs_list = list(configs)
        else:
            configs_list = list(configs) if isinstance(configs, list) else []

        appended = self._append_new(configs_list)
        if self.orch.cache is not None:
            try:
                self.orch.cache.save_configs(configs_list, working=False)
            except Exception:
                pass

        added = 0
        if config_db is not None:
            try:
                added = config_db.add_configs(set(configs_list), tag="github_bg")
            except Exception:
                pass

        self._download_count += 1
        self.status.extra["last_count"] = len(configs_list)
        self.status.extra["new_appended"] = appended
        self.status.extra["new_added_db"] = added
        self.status.extra["db_size"] = getattr(config_db, "size", 0) if config_db else 0
        self.status.extra["downloads"] = self._download_count
        self.logger.info(
            f"[GitHubBG] Got {len(configs_list)} configs, appended {appended}, db+{added}"
        )


class ValidatorWorker(BaseWorker):
    """Continuous background config validation."""
    name = "validator"
    interval_seconds = 30  # Run every 30 seconds (continuous small batches)

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._validator = None

    def _get_validator(self):
        if self._validator is None and ContinuousValidator is not None:
            config_db = getattr(self.orch, 'config_db', None)
            if config_db is not None:
                self._validator = ContinuousValidator(config_db, batch_size=80)
        return self._validator

    async def execute(self):
        validator = self._get_validator()
        if validator is None:
            return

        loop = asyncio.get_event_loop()
        mgr = HunterTaskManager.get_instance()
        tested, passed = await loop.run_in_executor(
            mgr._io_pool, validator.validate_batch
        )

        self.status.extra["last_tested"] = tested
        self.status.extra["last_passed"] = passed
        stats = validator.get_stats()
        self.status.extra["total_tested"] = stats["total_tested"]
        self.status.extra["total_passed"] = stats["total_passed"]
        self.status.extra["db_alive"] = stats.get("db", {}).get("alive_configs", 0)

        # Feed freshly validated healthy configs to balancer
        config_db = getattr(self.orch, 'config_db', None)
        if config_db and passed > 0:
            try:
                healthy = config_db.get_healthy_configs(max_count=50)
                if healthy and self.orch.balancer is not None:
                    self.orch.balancer.update_available_configs(healthy, trusted=True)
            except Exception:
                pass


class DPIPressureWorker(BaseWorker):
    """Periodic DPI stress testing to open bypass holes."""
    name = "dpi_pressure"
    interval_seconds = 300  # Every 5 minutes

    def __init__(self, orchestrator, stop_event, hw_fn):
        super().__init__(stop_event, hw_fn)
        self.orch = orchestrator
        self._engine = None

    def _get_engine(self):
        if self._engine is None and DPIPressureEngine is not None:
            intensity = float(self.orch.config.get("dpi_pressure_intensity", 0.7))
            self._engine = DPIPressureEngine(intensity=intensity)
        return self._engine

    async def execute(self):
        engine = self._get_engine()
        if engine is None:
            self.logger.debug("[DPI-Pressure] Engine not available")
            return

        loop = asyncio.get_event_loop()
        mgr = HunterTaskManager.get_instance()
        stats = await loop.run_in_executor(mgr._io_pool, engine.run_pressure_cycle)

        self.status.extra["tls_ok"] = stats.get("tls_probes_ok", 0)
        self.status.extra["telegram_reachable"] = stats.get("telegram_reachable", 0)
        self.status.extra["cycles"] = stats.get("pressure_cycles", 0)
        self.status.extra["bytes_sent"] = stats.get("total_bytes_sent", 0)


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
        self._task_mgr = HunterTaskManager.get_instance()

        # Create workers - all share hardware snapshot via task manager
        self.scanner = ConfigScannerWorker(orchestrator, self._stop_event, self._get_hw)
        self.telegram = TelegramPublisherWorker(orchestrator, self._stop_event, self._get_hw)
        self.balancer = BalancerWorker(orchestrator, self._stop_event, self._get_hw)
        self.harvester = HarvesterWorker(orchestrator, self._stop_event, self._get_hw)
        self.github_downloader = GitHubDownloaderWorker(orchestrator, self._stop_event, self._get_hw)
        self.validator = ValidatorWorker(orchestrator, self._stop_event, self._get_hw)
        self.dpi_pressure = DPIPressureWorker(orchestrator, self._stop_event, self._get_hw)

        all_workers = [
            self.scanner, self.telegram, self.balancer,
            self.harvester, self.github_downloader, self.validator, self.dpi_pressure,
        ]
        self.health = HealthMonitorWorker(self._stop_event, self._get_hw, workers=all_workers)

        self._workers: List[BaseWorker] = [
            self.health,
            self.balancer,
            self.scanner,
            self.telegram,
            self.harvester,
            self.github_downloader,
            self.validator,
            self.dpi_pressure,
        ]

    def _get_hw(self) -> HardwareSnapshot:
        """Delegate to HunterTaskManager's cached hardware snapshot."""
        return self._task_mgr.get_hardware()

    def start_all(self):
        """Start all worker threads."""
        hw = self._get_hw()
        self.logger.info(
            f"ThreadManager starting - {hw.cpu_count} CPUs, "
            f"{hw.ram_total_gb:.1f} GB RAM ({hw.ram_percent:.0f}% used), "
            f"mode={hw.mode.value}"
        )
        for w in self._workers:
            try:
                w.start()
            except Exception as exc:
                self.logger.error(f"Failed to start {w.name}: {exc}")

        self.logger.info(f"All {len(self._workers)} workers started")

    def stop_all(self, timeout: float = 15.0):
        """Graceful shutdown of all workers and shared pools."""
        self.logger.info("ThreadManager: stopping all workers...")
        self._stop_event.set()

        for w in self._workers:
            try:
                w.join(timeout=timeout / len(self._workers))
            except Exception:
                pass

        # Shutdown the shared task manager pools
        try:
            self._task_mgr.shutdown(wait=True, timeout=5.0)
        except Exception:
            pass

        self.logger.info("ThreadManager: all workers stopped")

    def get_status(self) -> Dict[str, Any]:
        """Get status of all workers + task manager metrics."""
        hw = self._get_hw()
        result = {
            "hardware": {
                "cpu_count": hw.cpu_count,
                "cpu_percent": hw.cpu_percent,
                "ram_total_gb": round(hw.ram_total_gb, 1),
                "ram_used_gb": round(hw.ram_used_gb, 1),
                "ram_percent": round(hw.ram_percent, 1),
                "mode": hw.mode.value,
                "thread_count": threading.active_count(),
            },
            "task_manager": self._task_mgr.get_metrics(),
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
