"""
Hunter Dashboard - Real-time terminal dashboard replacing verbose logs.

Shows all system sections in a single screen with live updates:
- System status (cycle, memory, mode, uptime)
- Scraping progress per source
- Benchmark results (gold/silver/dead)
- Balancer health (port 10808/10809)
- Recent events log (compact)
"""

import logging
import os
import shutil
import sys
import threading
import time
from collections import deque
from typing import Any, Dict, List, Optional, Tuple

try:
    from rich.console import Console
    from rich.live import Live
    from rich.table import Table
    from rich.text import Text
    from rich.panel import Panel
    from rich.layout import Layout
    from rich.columns import Columns
    from rich import box
    RICH_AVAILABLE = True
except ImportError:
    RICH_AVAILABLE = False
    # Fallback stubs so type hints don't fail at import time
    Console = None
    Live = None
    Table = None
    Text = None
    Panel = None
    Layout = None
    Columns = None
    box = None


class DashboardLogHandler(logging.Handler):
    """Captures log records for the dashboard events panel."""

    def __init__(self, max_lines: int = 18):
        super().__init__()
        self.lines: deque = deque(maxlen=max_lines)
        self._lock = threading.Lock()
        # Patterns to suppress from dashboard (still go to file)
        self._suppress = (
            "Connection pool is full",
            "Retrying (Retry",
            "NewConnectionError",
            "connection broken by",
            "WinError 10061",
            "Failed to establish",
        )

    def emit(self, record: logging.LogRecord):
        try:
            msg = self.format(record)
            for s in self._suppress:
                if s in msg:
                    return
            with self._lock:
                self.lines.append((record.levelno, msg))
        except Exception:
            pass

    def get_lines(self) -> List[Tuple[int, str]]:
        with self._lock:
            return list(self.lines)


class HunterDashboard:
    """Real-time terminal dashboard for Hunter."""

    def __init__(self):
        self._lock = threading.Lock()
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._start_time = time.monotonic()
        self._console = Console() if RICH_AVAILABLE else None
        self._live: Optional[Live] = None

        # Log handler
        self.log_handler = DashboardLogHandler(max_lines=18)
        self.log_handler.setLevel(logging.INFO)
        fmt = logging.Formatter('%(asctime)s | %(levelname)-7s | %(message)s', datefmt='%H:%M:%S')
        self.log_handler.setFormatter(fmt)

        # State
        self._state: Dict[str, Any] = {
            "cycle": 0,
            "phase": "starting",
            "mode": "NORMAL",
            "memory_pct": 0.0,
            "memory_used_gb": 0.0,
            "memory_total_gb": 0.0,
            "gold": 0,
            "silver": 0,
            "dead": 0,
            "failed": 0,
            "total_tested": 0,
            "total_scraped": 0,
            "next_in": 0,
            # Scraping sources
            "sources": {},  # name -> {"status": "...", "count": N}
            # Benchmark
            "bench_total": 0,
            "bench_done": 0,
            "bench_chunk": "",
            "bench_current": "",
            # Balancer
            "bal_main_port": 10808,
            "bal_main_backends": 0,
            "bal_main_latency": "",
            "bal_main_status": "starting",
            "bal_gemini_port": 10809,
            "bal_gemini_backends": 0,
            "bal_gemini_status": "starting",
            # Telegram
            "tg_status": "",
            # DPI
            "dpi_strategy": "",
            "dpi_network": "",
        }

    def update(self, **kwargs):
        with self._lock:
            self._state.update(kwargs)

    def source_update(self, name: str, status: str = "", count: int = 0):
        with self._lock:
            self._state["sources"][name] = {"status": status, "count": count}

    def bench_result(self, success: bool, name: str = "", latency: float = 0, tier: str = ""):
        with self._lock:
            self._state["bench_done"] += 1
            self._state["total_tested"] += 1
            if success:
                if tier == "gold":
                    self._state["gold"] += 1
                elif tier == "silver":
                    self._state["silver"] += 1
                elif tier == "dead":
                    self._state["dead"] += 1
                short = name[:25] if name else "?"
                self._state["bench_current"] = f"{short} {latency:.0f}ms [{tier}]"
            else:
                self._state["failed"] += 1
                self._state["bench_current"] = "x failed"

    def reset_bench(self, total: int):
        with self._lock:
            self._state["bench_total"] = total
            self._state["bench_done"] = 0
            self._state["bench_current"] = ""

    def reset_sources(self):
        with self._lock:
            self._state["sources"] = {}
            self._state["total_scraped"] = 0

    def start(self):
        if not RICH_AVAILABLE or self._running:
            return
        self._running = True
        self._start_time = time.monotonic()
        self._thread = threading.Thread(target=self._loop, daemon=True, name="dashboard")
        self._thread.start()

    def stop(self):
        self._running = False
        if self._live:
            try:
                self._live.stop()
            except Exception:
                pass

    def _loop(self):
        try:
            self._live = Live(
                self._render(),
                console=self._console,
                refresh_per_second=2,
                screen=False,
                transient=True,
                vertical_overflow="crop",
            )
            with self._live:
                while self._running:
                    try:
                        self._live.update(self._render())
                    except Exception as e:
                        logging.getLogger(__name__).debug(f"Dashboard render error: {e}")
                    time.sleep(0.5)
        except Exception as e:
            logging.getLogger(__name__).warning(f"Dashboard loop crashed: {e}")

    def _uptime(self) -> str:
        s = int(time.monotonic() - self._start_time)
        h, rem = divmod(s, 3600)
        m, sec = divmod(rem, 60)
        return f"{h}h{m:02d}m{sec:02d}s" if h else f"{m}m{sec:02d}s"

    def _render(self) -> Table:
        with self._lock:
            d = dict(self._state)
            sources = dict(d.get("sources", {}))
            log_lines = self.log_handler.get_lines()

        # Colors
        mem = d["memory_pct"]
        mem_style = "red bold" if mem > 90 else ("yellow" if mem > 80 else "green")
        phase = d["phase"]
        phase_style = {"scraping": "cyan", "validating": "yellow", "balancing": "green", "reporting": "magenta", "sleeping": "dim"}.get(phase, "white")

        # Main outer table (no border, full width)
        main = Table(box=None, show_header=False, show_edge=False, pad_edge=False, expand=True)
        main.add_column(ratio=1)

        # ── Header row
        header = Text()
        header.append(" HUNTER ", style="bold white on blue")
        header.append(f"  C#{d['cycle']} ", style="bold cyan")
        header.append(f" {phase.upper()} ", style=f"bold {phase_style}")
        header.append(f"  Mode: {d.get('mode', '?')} ", style="dim")
        header.append(f"  Mem: {mem:.0f}% ", style=mem_style)
        if d["memory_total_gb"] > 0:
            header.append(f"({d['memory_used_gb']:.1f}/{d['memory_total_gb']:.1f}GB) ", style="dim")
        header.append(f"  Up: {self._uptime()} ", style="dim")
        next_in = d.get("next_in", 0)
        if next_in > 0 and phase == "sleeping":
            header.append(f"  Next: {int(next_in)}s ", style="cyan")
        main.add_row(header)
        main.add_row(Text(""))

        # ── Two-column layout: left (sources + bench), right (balancer + info)
        cols_table = Table(box=None, show_header=False, show_edge=False, pad_edge=False, expand=True)
        cols_table.add_column("left", ratio=3)
        cols_table.add_column("right", ratio=2)

        # LEFT: Sources table
        src_table = Table(title="Sources", box=box.SIMPLE, expand=True, title_style="bold cyan")
        src_table.add_column("Source", style="bold", min_width=14)
        src_table.add_column("Status", min_width=10)
        src_table.add_column("Configs", justify="right", min_width=7)

        total_scraped = 0
        for sname, sdata in sources.items():
            st = sdata.get("status", "")
            cnt = sdata.get("count", 0)
            total_scraped += cnt
            st_style = "green" if st == "done" else ("yellow" if st == "fetching" else ("red" if st == "failed" else "dim"))
            src_table.add_row(sname, Text(st, style=st_style), str(cnt))

        if not sources:
            src_table.add_row("--", Text("waiting", style="dim"), "--")

        # Bench progress
        bench_table = Table(title="Benchmark", box=box.SIMPLE, expand=True, title_style="bold yellow")
        bench_table.add_column("Metric", style="bold", min_width=10)
        bench_table.add_column("Value", min_width=20)

        bt = d["bench_total"]
        bd = d["bench_done"]
        if bt > 0:
            pct = bd * 100 // bt
            bar_w = 20
            filled = bar_w * bd // bt
            bar = "[green]" + "#" * filled + "[/green]" + "[dim]" + "-" * (bar_w - filled) + "[/dim]"
            bench_table.add_row("Progress", f"{bar} {bd}/{bt} ({pct}%)")
        else:
            bench_table.add_row("Progress", Text("waiting", style="dim"))

        bench_table.add_row("Gold", Text(str(d["gold"]), style="bold yellow"))
        bench_table.add_row("Silver", Text(str(d["silver"]), style="bold white"))
        bench_table.add_row("Dead", Text(str(d["dead"]), style="dim"))
        bench_table.add_row("Failed", Text(str(d["failed"]), style="red" if d["failed"] else "dim"))
        if d["bench_current"]:
            bench_table.add_row("Last", Text(d["bench_current"][:40], style="dim"))

        left = Table(box=None, show_header=False, show_edge=False, pad_edge=False, expand=True)
        left.add_column(ratio=1)
        left.add_row(src_table)
        left.add_row(bench_table)

        # RIGHT: Balancer + DPI
        bal_table = Table(title="Balancers", box=box.SIMPLE, expand=True, title_style="bold green")
        bal_table.add_column("Port", style="bold", min_width=8)
        bal_table.add_column("Backends", justify="center", min_width=8)
        bal_table.add_column("Status", min_width=12)

        ms = d["bal_main_status"]
        ms_style = "green" if ms == "OK" else ("yellow" if ms == "starting" else "red")
        bal_table.add_row(
            str(d["bal_main_port"]),
            str(d["bal_main_backends"]),
            Text(f"{ms} {d['bal_main_latency']}", style=ms_style),
        )
        gs = d["bal_gemini_status"]
        gs_style = "green" if gs == "OK" else ("yellow" if gs == "starting" else "red")
        bal_table.add_row(
            str(d["bal_gemini_port"]),
            str(d["bal_gemini_backends"]),
            Text(gs, style=gs_style),
        )

        info_table = Table(title="Info", box=box.SIMPLE, expand=True, title_style="bold magenta")
        info_table.add_column("", style="bold", min_width=10)
        info_table.add_column("", min_width=18)
        info_table.add_row("Scraped", str(total_scraped))
        info_table.add_row("Tested", str(d["total_tested"]))
        info_table.add_row("Working", str(d["gold"] + d["silver"]))
        if d.get("dpi_strategy"):
            info_table.add_row("DPI", d["dpi_strategy"])
        if d.get("tg_status"):
            info_table.add_row("Telegram", d["tg_status"])

        right = Table(box=None, show_header=False, show_edge=False, pad_edge=False, expand=True)
        right.add_column(ratio=1)
        right.add_row(bal_table)
        right.add_row(info_table)

        cols_table.add_row(left, right)
        main.add_row(cols_table)
        main.add_row(Text(""))

        # ── Events log
        events_table = Table(title="Events", box=box.SIMPLE, expand=True, title_style="bold blue")
        events_table.add_column("Log", no_wrap=True)

        cols_w = shutil.get_terminal_size((120, 30)).columns
        max_show = min(12, len(log_lines))
        for lvl, msg in log_lines[-max_show:]:
            style = "red" if lvl >= logging.ERROR else ("yellow" if lvl >= logging.WARNING else "dim")
            truncated = msg[:cols_w - 4] if len(msg) > cols_w - 4 else msg
            events_table.add_row(Text(truncated, style=style))

        if not log_lines:
            events_table.add_row(Text("waiting for events...", style="dim"))

        main.add_row(events_table)
        return main
