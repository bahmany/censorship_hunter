"""
Hunter UI - Professional single-line progress bar system.

Provides beautiful, compact progress display for all Hunter operations:
- Config scraping from Telegram/GitHub/anti-censorship sources
- Benchmark validation with XRay/sing-box/mihomo engines
- Download speed testing
- Balancer health checks
- Service lifecycle management

Uses rich library for professional terminal rendering with fallback
to simple ANSI progress bars if rich is not available.
"""

import logging
import os
import shutil
import sys
import threading
import time
from contextlib import contextmanager
from typing import Any, Callable, Dict, List, Optional, Tuple

try:
    from rich.console import Console
    from rich.live import Live
    from rich.table import Table
    from rich.text import Text
    from rich.progress import (
        Progress, SpinnerColumn, TextColumn, BarColumn,
        TaskProgressColumn, TimeElapsedColumn, TimeRemainingColumn,
        MofNCompleteColumn,
    )
    from rich.panel import Panel
    from rich.layout import Layout
    from rich.style import Style
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False

logger = logging.getLogger(__name__)

# ─── Theme (ASCII-safe for Windows cp1252 consoles) ──────────────────────────
ICONS = {
    "telegram": "TG",
    "github":   "GH",
    "shield":   "AC",
    "iran":     "IR",
    "check":    "+",
    "cross":    "x",
    "bolt":     "*",
    "gear":     "#",
    "globe":    "@",
    "lock":     "!",
    "clock":    "~",
    "fire":     "!",
    "wave":     "~",
    "bar":      "#",
    "light":    "-",
    "mid":      "=",
    "arrow":    ">",
    "dot":      ".",
    "gold":     "G",
    "silver":   "S",
}


def _safe_write(stream, text: str):
    """Write text to stream, replacing unencodable chars."""
    try:
        stream.write(text)
        stream.flush()
    except UnicodeEncodeError:
        stream.write(text.encode("ascii", errors="replace").decode("ascii"))
        stream.flush()


# ─── Fallback ANSI progress bar (no dependencies) ────────────────────────────

class _AnsiProgressBar:
    """Minimal single-line progress bar using ANSI escape codes."""

    def __init__(self, total: int, desc: str = "", width: int = 30):
        self.total = max(total, 1)
        self.current = 0
        self.desc = desc
        self.width = width
        self.extra = ""
        self._lock = threading.Lock()
        self._start = time.monotonic()

    def update(self, n: int = 1, extra: str = ""):
        with self._lock:
            self.current = min(self.current + n, self.total)
            if extra:
                self.extra = extra
            self._render()

    def set_description(self, desc: str):
        with self._lock:
            self.desc = desc
            self._render()

    def _render(self):
        pct = self.current / self.total
        filled = int(self.width * pct)
        bar = ICONS["bar"] * filled + ICONS["light"] * (self.width - filled)
        elapsed = time.monotonic() - self._start
        rate = self.current / elapsed if elapsed > 0 else 0
        tail = f" {self.extra}" if self.extra else ""
        line = f"\r  {self.desc} |{bar}| {self.current}/{self.total} [{elapsed:.0f}s, {rate:.1f}/s]{tail}"
        # Truncate to terminal width
        cols = shutil.get_terminal_size((120, 24)).columns
        if len(line) > cols:
            line = line[: cols - 1]
        _safe_write(sys.stderr, line)

    def close(self):
        _safe_write(sys.stderr, "\n")


# ─── Rich-based progress bar ─────────────────────────────────────────────────

def _make_rich_progress(transient: bool = True) -> "Progress":
    """Create a rich Progress bar with Hunter styling."""
    return Progress(
        SpinnerColumn("dots", style="cyan"),
        TextColumn("[bold blue]{task.description}[/]", justify="left"),
        BarColumn(bar_width=28, style="grey37", complete_style="green", finished_style="bold green"),
        TaskProgressColumn(),
        MofNCompleteColumn(),
        TextColumn("[dim]{task.fields[extra]}[/]"),
        TimeElapsedColumn(),
        console=Console(stderr=True),
        transient=transient,
        expand=False,
    )


# ─── LiveStatus: persistent status line at bottom of terminal ─────────────────

class LiveStatus:
    """Persistent single-line status bar showing service state.
    
    Displays: cycle number, uptime, configs in balancer, memory, next cycle countdown.
    """

    def __init__(self):
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._data: Dict[str, Any] = {
            "cycle": 0,
            "gold": 0,
            "silver": 0,
            "balancer": 0,
            "memory_pct": 0.0,
            "phase": "idle",
            "next_in": 0,
            "uptime": 0,
        }
        self._start_time = time.monotonic()

    def update(self, **kwargs):
        with self._lock:
            self._data.update(kwargs)

    def start(self):
        if self._running:
            return
        self._running = True
        self._start_time = time.monotonic()
        self._thread = threading.Thread(target=self._loop, daemon=True, name="hunter-status")
        self._thread.start()

    def stop(self):
        self._running = False

    def _loop(self):
        while self._running:
            try:
                self._render()
            except Exception:
                pass
            time.sleep(1.0)
        # Clear line on exit
        _safe_write(sys.stderr, "\r" + " " * shutil.get_terminal_size((120, 24)).columns + "\r")

    def _render(self):
        with self._lock:
            d = dict(self._data)
        uptime = time.monotonic() - self._start_time
        h, rem = divmod(int(uptime), 3600)
        m, s = divmod(rem, 60)
        uptime_str = f"{h}h{m:02d}m" if h else f"{m}m{s:02d}s"

        phase = d.get("phase", "idle")
        phase_icon = {
            "idle": ICONS["clock"],
            "scraping": ICONS["telegram"],
            "validating": ICONS["bolt"],
            "balancing": ICONS["gear"],
            "sleeping": ICONS["wave"],
        }.get(phase, ICONS["dot"])

        next_in = d.get("next_in", 0)
        next_str = f"{int(next_in)}s" if next_in > 0 else "--"

        mem_pct = d.get("memory_pct", 0)
        mem_color = "31" if mem_pct > 85 else ("33" if mem_pct > 70 else "32")

        line = (
            f"\r\033[36m{ICONS['gear']} HUNTER\033[0m "
            f"C#{d.get('cycle', 0)} "
            f"\033[33m{ICONS['gold']}{d.get('gold', 0)}\033[0m/"
            f"\033[37m{ICONS['silver']}{d.get('silver', 0)}\033[0m "
            f"bal:{d.get('balancer', 0)} "
            f"\033[{mem_color}mmem:{mem_pct:.0f}%\033[0m "
            f"{phase_icon} {phase} "
            f"next:{next_str} "
            f"up:{uptime_str}"
        )
        cols = shutil.get_terminal_size((120, 24)).columns
        # Pad to clear previous content
        visible_len = len(line) - line.count("\033[") * 5  # rough estimate
        pad = max(0, cols - visible_len - 5)
        _safe_write(sys.stderr, line + " " * pad)


# ─── HunterUI: main interface ────────────────────────────────────────────────

class HunterUI:
    """Professional progress bar manager for all Hunter operations.
    
    Usage:
        ui = HunterUI()
        
        # For scraping phase:
        with ui.scrape_progress(sources=["Telegram", "GitHub", ...]) as tracker:
            tracker.source_done("Telegram", count=42)
            tracker.source_done("GitHub", count=130)
        
        # For benchmark phase:
        with ui.benchmark_progress(total=500) as tracker:
            tracker.tested(success=True, name="US-vless-reality", latency=120)
            tracker.tested(success=False)
        
        # Persistent service status:
        ui.status.update(cycle=3, gold=15, silver=40, phase="sleeping")
    """

    def __init__(self):
        self.status = LiveStatus()
        self._use_rich = RICH_AVAILABLE and not os.getenv("HUNTER_NO_RICH")
        self.logger = logging.getLogger(__name__)
        # When dashboard is active, suppress progress bars to avoid display conflicts
        self.dashboard_active = False

    # ── Scrape progress ──────────────────────────────────────────────────

    @contextmanager
    def scrape_progress(self, sources: List[str]):
        """Context manager for config scraping progress."""
        tracker = _ScrapeTracker(sources, use_rich=self._use_rich and not self.dashboard_active)
        if not self.dashboard_active:
            tracker.start()
        try:
            yield tracker
        finally:
            if not self.dashboard_active:
                tracker.finish()

    # ── Benchmark progress ───────────────────────────────────────────────

    @contextmanager
    def benchmark_progress(self, total: int):
        """Context manager for benchmark/validation progress."""
        tracker = _BenchmarkTracker(total, use_rich=self._use_rich and not self.dashboard_active)
        if not self.dashboard_active:
            tracker.start()
        try:
            yield tracker
        finally:
            if not self.dashboard_active:
                tracker.finish()

    # ── Generic task progress ────────────────────────────────────────────

    @contextmanager
    def task_progress(self, total: int, description: str = "Processing"):
        """Generic progress bar for any task."""
        tracker = _GenericTracker(total, description, use_rich=self._use_rich and not self.dashboard_active)
        if not self.dashboard_active:
            tracker.start()
        try:
            yield tracker
        finally:
            if not self.dashboard_active:
                tracker.finish()

    # ── Service banner ───────────────────────────────────────────────────

    def print_banner(self):
        """Print professional startup banner."""
        cols = shutil.get_terminal_size((80, 24)).columns
        w = min(cols, 64)
        border = "\033[36m" + "=" * w + "\033[0m"
        title = "HUNTER - Autonomous V2Ray Proxy Hunting Service"
        sub = "Anti-censorship | DPI evasion | Iran optimized"
        print()
        print(border)
        print(f"\033[1;36m  {title}\033[0m")
        print(f"\033[90m  {sub}\033[0m")
        print(border)
        print()

    def print_tools_check(self, bin_dir: str):
        """Check and display status of required tools in bin/."""
        tools = {
            "xray.exe":        ("XRay Core",       "Proxy engine"),
            "sing-box.exe":    ("sing-box",         "Fallback engine"),
            "mihomo-windows-amd64-compatible.exe": ("Mihomo", "Clash Meta engine"),
            "tor.exe":         ("Tor",              "Onion routing"),
            "chromedriver.exe":("ChromeDriver",     "Browser automation"),
            "AmazTool.exe":    ("AmazTool",         "Utility"),
        }
        print(f"  \033[1m{ICONS['gear']} Tool Check\033[0m  ({bin_dir})")
        all_ok = True
        for filename, (name, desc) in tools.items():
            path = os.path.join(bin_dir, filename)
            exists = os.path.isfile(path)
            if exists:
                size_mb = os.path.getsize(path) / (1024 * 1024)
                print(f"    \033[32m{ICONS['check']}\033[0m {name:<16} {desc:<22} \033[90m({size_mb:.1f}MB)\033[0m")
            else:
                print(f"    \033[31m{ICONS['cross']}\033[0m {name:<16} {desc:<22} \033[31mMISSING\033[0m")
                all_ok = False
        print()
        return all_ok


# ─── Scrape Tracker ──────────────────────────────────────────────────────────

class _ScrapeTracker:
    """Tracks config scraping progress across multiple sources."""

    def __init__(self, sources: List[str], use_rich: bool = True):
        self.sources = sources
        self.total_sources = len(sources)
        self._use_rich = use_rich and RICH_AVAILABLE
        self._done = 0
        self._total_configs = 0
        self._progress = None
        self._task_id = None
        self._lock = threading.Lock()

    def start(self):
        if self._use_rich:
            self._progress = _make_rich_progress(transient=True)
            self._progress.start()
            self._task_id = self._progress.add_task(
                f"{ICONS['telegram']} Scraping configs",
                total=self.total_sources,
                extra="starting..."
            )
        else:
            self._progress = _AnsiProgressBar(
                self.total_sources,
                desc=f"{ICONS['telegram']} Scraping"
            )

    def source_done(self, name: str, count: int):
        with self._lock:
            self._done += 1
            self._total_configs += count
            if self._progress is None:
                return
            extra = f"{name}: +{count} ({self._total_configs} total)"
            if self._use_rich:
                self._progress.update(self._task_id, advance=1, extra=extra)
            else:
                self._progress.update(1, extra=extra)

    def source_failed(self, name: str, reason: str = ""):
        with self._lock:
            self._done += 1
            if self._progress is None:
                return
            extra = f"{name}: {ICONS['cross']} {reason}" if reason else f"{name}: {ICONS['cross']}"
            if self._use_rich:
                self._progress.update(self._task_id, advance=1, extra=extra)
            else:
                self._progress.update(1, extra=extra)

    @property
    def total_configs(self) -> int:
        return self._total_configs

    def finish(self):
        if self._use_rich and self._progress:
            self._progress.update(
                self._task_id,
                extra=f"{ICONS['check']} {self._total_configs} configs from {self._done} sources"
            )
            self._progress.stop()
        elif self._progress:
            self._progress.extra = f"{ICONS['check']} {self._total_configs} configs"
            self._progress.close()


# ─── Benchmark Tracker ───────────────────────────────────────────────────────

class _BenchmarkTracker:
    """Tracks benchmark/validation progress."""

    def __init__(self, total: int, use_rich: bool = True):
        self.total = total
        self._use_rich = use_rich and RICH_AVAILABLE
        self._tested = 0
        self._ok = 0
        self._fail = 0
        self._gold = 0
        self._silver = 0
        self._progress = None
        self._task_id = None
        self._lock = threading.Lock()

    def start(self):
        if self._use_rich:
            self._progress = _make_rich_progress(transient=True)
            self._progress.start()
            self._task_id = self._progress.add_task(
                f"{ICONS['bolt']} Benchmarking",
                total=self.total,
                extra="starting..."
            )
        else:
            self._progress = _AnsiProgressBar(self.total, desc=f"{ICONS['bolt']} Benchmark")

    def tested(self, success: bool, name: str = "", latency: float = 0, tier: str = ""):
        with self._lock:
            self._tested += 1
            if success:
                self._ok += 1
                if tier == "gold":
                    self._gold += 1
                elif tier == "silver":
                    self._silver += 1
            else:
                self._fail += 1

            if self._progress is None:
                return

            extra = (
                f"{ICONS['gold']}{self._gold} {ICONS['silver']}{self._silver} "
                f"{ICONS['check']}{self._ok}/{self._tested}"
            )
            if success and name:
                short_name = name[:20]
                extra += f" | {short_name} {latency:.0f}ms"

            if self._use_rich:
                self._progress.update(self._task_id, advance=1, extra=extra)
            else:
                self._progress.update(1, extra=extra)

    @property
    def ok_count(self) -> int:
        return self._ok

    @property
    def gold_count(self) -> int:
        return self._gold

    def finish(self):
        summary = (
            f"{ICONS['check']} Done: {ICONS['gold']}{self._gold} gold, "
            f"{ICONS['silver']}{self._silver} silver, "
            f"{self._fail} failed / {self._tested} tested"
        )
        if self._use_rich and self._progress:
            self._progress.update(self._task_id, extra=summary)
            self._progress.stop()
        elif self._progress:
            self._progress.extra = summary
            self._progress.close()


# ─── Generic Tracker ─────────────────────────────────────────────────────────

class _GenericTracker:
    """Generic progress tracker for any operation."""

    def __init__(self, total: int, description: str, use_rich: bool = True):
        self.total = total
        self.description = description
        self._use_rich = use_rich and RICH_AVAILABLE
        self._progress = None
        self._task_id = None

    def start(self):
        if self._use_rich:
            self._progress = _make_rich_progress(transient=True)
            self._progress.start()
            self._task_id = self._progress.add_task(
                self.description, total=self.total, extra=""
            )
        else:
            self._progress = _AnsiProgressBar(self.total, desc=self.description)

    def advance(self, n: int = 1, extra: str = ""):
        if self._progress is None:
            return
        if self._use_rich:
            self._progress.update(self._task_id, advance=n, extra=extra)
        else:
            self._progress.update(n, extra=extra)

    def finish(self):
        if self._use_rich and self._progress:
            self._progress.stop()
        elif self._progress:
            self._progress.close()


# ─── Windows Service Manager ─────────────────────────────────────────────────

class ServiceManager:
    """Manages Hunter as a persistent background service on Windows.
    
    Uses a PID file + watchdog thread to ensure the service stays alive.
    Handles graceful shutdown on SIGINT/SIGTERM and CTRL+C.
    """

    PID_FILE = "hunter_service.pid"

    def __init__(self, runtime_dir: str):
        self.runtime_dir = runtime_dir
        self.pid_file = os.path.join(runtime_dir, self.PID_FILE)
        self.logger = logging.getLogger(__name__)
        self._shutdown_event = threading.Event()

    def is_running(self) -> bool:
        """Check if another Hunter instance is already running."""
        if not os.path.exists(self.pid_file):
            return False
        try:
            with open(self.pid_file, "r") as f:
                pid = int(f.read().strip())
            # Check if process is alive
            import psutil
            return psutil.pid_exists(pid) and pid != os.getpid()
        except Exception:
            return False

    def register(self):
        """Register this process as the active Hunter service."""
        os.makedirs(self.runtime_dir, exist_ok=True)
        with open(self.pid_file, "w") as f:
            f.write(str(os.getpid()))
        self.logger.info(f"Service registered (PID: {os.getpid()})")

    def unregister(self):
        """Remove PID file on shutdown."""
        try:
            if os.path.exists(self.pid_file):
                with open(self.pid_file, "r") as f:
                    pid = int(f.read().strip())
                if pid == os.getpid():
                    os.remove(self.pid_file)
        except Exception:
            pass

    def setup_signal_handlers(self):
        """Setup graceful shutdown handlers."""
        import signal

        def _handler(signum, frame):
            self.logger.info(f"Received signal {signum}, initiating graceful shutdown...")
            self._shutdown_event.set()

        signal.signal(signal.SIGINT, _handler)
        signal.signal(signal.SIGTERM, _handler)
        # Windows: also handle CTRL+C and CTRL+BREAK
        if sys.platform == "win32":
            try:
                signal.signal(signal.SIGBREAK, _handler)
            except (AttributeError, ValueError):
                pass

    @property
    def should_stop(self) -> bool:
        return self._shutdown_event.is_set()

    def wait_for_shutdown(self, timeout: float = None) -> bool:
        """Block until shutdown signal received. Returns True if signaled."""
        return self._shutdown_event.wait(timeout=timeout)
