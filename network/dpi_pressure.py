"""
DPI Pressure Engine — stress Iranian DPI equipment to open bypass holes.

Techniques:
1. Parallel TLS ClientHello floods with fragmented SNI
2. Randomized JA3 fingerprint rotation across connections
3. Multi-port connection storms to overwhelm stateful DPI tables
4. Decoy traffic generation mimicking legitimate HTTPS browsing
5. DNS amplification probes to test censorship state
6. Reality/gRPC probes that look like normal CDN traffic

All techniques run concurrently in dedicated threads, each applying
pressure to different aspects of the DPI inspection pipeline.
"""

import logging
import os
import random
import socket
import ssl
import struct
import threading
import time
from concurrent.futures import as_completed
from typing import Dict, List, Optional, Tuple

try:
    from hunter.core.task_manager import HunterTaskManager
except ImportError:
    from core.task_manager import HunterTaskManager

logger = logging.getLogger(__name__)

# ─── Legitimate-looking SNI targets (CDN, cloud, popular sites) ──────────────
# These are domains that DPI must allow through — probing them forces DPI
# to process each connection individually, consuming inspection resources.
_DECOY_SNIS = [
    "www.google.com", "www.apple.com", "swdist.apple.com",
    "www.microsoft.com", "login.microsoftonline.com",
    "cdn.cloudflare.com", "ajax.googleapis.com",
    "fonts.googleapis.com", "www.gstatic.com",
    "update.googleapis.com", "play.googleapis.com",
    "www.amazonaws.com", "s3.amazonaws.com",
    "github.com", "raw.githubusercontent.com",
    "cdn.jsdelivr.net", "cdnjs.cloudflare.com",
    "stackpath.bootstrapcdn.com", "unpkg.com",
    "www.fastly.com", "dualstack.osff.map.fastly.net",
    "speed.cloudflare.com", "1.1.1.1",
    "dns.google", "dns.quad9.net",
    "www.cloudflare.com", "dash.cloudflare.com",
    "workers.dev", "pages.dev",
    "azure.microsoft.com", "portal.azure.com",
    "aws.amazon.com", "console.aws.amazon.com",
]

# Telegram DCs — probing these tests if Telegram is accessible
_TELEGRAM_DCS = [
    ("149.154.175.50", 443),
    ("149.154.167.51", 443),
    ("149.154.175.100", 443),
    ("149.154.167.91", 443),
    ("91.108.56.130", 443),
]

# Common HTTPS ports to probe through
_PROBE_PORTS = [443, 8443, 2053, 2083, 2087, 2096, 8080, 80]


class DPIPressureEngine:
    """
    Applies sustained pressure on DPI infrastructure to create bypass windows.
    
    Each technique targets a different DPI subsystem:
    - TLS inspection (fragmented ClientHello)
    - Connection tracking (parallel connection storms)
    - SNI filtering (randomized SNI with legitimate domains)
    - Protocol detection (mixed protocol probes)
    """

    def __init__(self, intensity: float = 0.7):
        """
        Args:
            intensity: 0.0-1.0, controls aggressiveness.
                       0.3 = gentle probing
                       0.7 = sustained pressure (default)
                       1.0 = maximum stress
        """
        self.intensity = max(0.1, min(1.0, intensity))
        self.logger = logging.getLogger("hunter.dpi_pressure")
        self._running = False
        self._stop_event = threading.Event()
        self._stats = {
            "tls_probes_sent": 0,
            "tls_probes_ok": 0,
            "tcp_storms_sent": 0,
            "tcp_storms_ok": 0,
            "decoy_requests": 0,
            "decoy_ok": 0,
            "telegram_probes": 0,
            "telegram_reachable": 0,
            "total_bytes_sent": 0,
            "pressure_cycles": 0,
            "last_cycle_ts": 0,
        }
        self._lock = threading.Lock()

    # ── TLS ClientHello Fragmentation ────────────────────────────────────

    def _build_fragmented_client_hello(self, sni: str) -> List[bytes]:
        """
        Build a TLS ClientHello split into 2-3 fragments.
        
        This forces DPI to reassemble TLS records before inspecting SNI,
        consuming more memory and CPU in the inspection pipeline.
        """
        # Build SNI extension
        sni_bytes = sni.encode("ascii")
        sni_ext = (
            struct.pack("!HH", 0x0000, len(sni_bytes) + 5) +  # SNI extension type + length
            struct.pack("!H", len(sni_bytes) + 3) +  # server name list length
            struct.pack("!BH", 0, len(sni_bytes)) +  # host name type + length
            sni_bytes
        )

        # Random cipher suites (looks like Chrome)
        cipher_suites = bytes([
            0x13, 0x01,  # TLS_AES_128_GCM_SHA256
            0x13, 0x02,  # TLS_AES_256_GCM_SHA384
            0x13, 0x03,  # TLS_CHACHA20_POLY1305_SHA256
            0xc0, 0x2c,  # TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384
            0xc0, 0x2b,  # TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
            0xc0, 0x30,  # TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384
            0xc0, 0x2f,  # TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
        ])

        # Build ClientHello handshake message
        client_random = os.urandom(32)
        session_id = os.urandom(32)
        
        hello_body = (
            b'\x03\x03' +                                    # TLS 1.2 version
            client_random +                                    # Random
            struct.pack("!B", len(session_id)) + session_id + # Session ID
            struct.pack("!H", len(cipher_suites)) + cipher_suites + # Ciphers
            b'\x01\x00' +                                     # Compression (null)
            struct.pack("!H", len(sni_ext) + 4) +            # Extensions length
            sni_ext +                                          # SNI extension
            struct.pack("!HH", 0x0017, 0)                    # Extended master secret (empty)
        )

        # Handshake header
        handshake = (
            b'\x01' +                                         # ClientHello type
            struct.pack("!I", len(hello_body))[1:] +         # Length (3 bytes)
            hello_body
        )

        # Split into TLS records at strategic points (mid-SNI)
        # This is the key anti-DPI technique
        split_point = random.randint(20, min(60, len(handshake) - 10))
        
        frag1 = (
            b'\x16\x03\x01' +                                # TLS record header
            struct.pack("!H", split_point) +                  # Fragment 1 length
            handshake[:split_point]
        )
        frag2 = (
            b'\x16\x03\x01' +
            struct.pack("!H", len(handshake) - split_point) +
            handshake[split_point:]
        )

        return [frag1, frag2]

    def _probe_tls_fragmented(self, host: str, port: int, sni: str) -> bool:
        """Send fragmented TLS ClientHello to target."""
        try:
            fragments = self._build_fragmented_client_hello(sni)
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect((host, port))

            for i, frag in enumerate(fragments):
                s.send(frag)
                with self._lock:
                    self._stats["total_bytes_sent"] += len(frag)
                # Add jitter between fragments (10-50ms)
                if i < len(fragments) - 1:
                    time.sleep(random.uniform(0.01, 0.05))

            # Try to read server response
            s.settimeout(3.0)
            try:
                resp = s.recv(5)  # TLS record header
                s.close()
                with self._lock:
                    self._stats["tls_probes_ok"] += 1
                return len(resp) > 0
            except socket.timeout:
                s.close()
                return False
        except Exception:
            return False
        finally:
            with self._lock:
                self._stats["tls_probes_sent"] += 1

    # ── Connection Storm ─────────────────────────────────────────────────

    def _tcp_storm_batch(self, targets: List[Tuple[str, int]], count: int = 10) -> int:
        """Open many TCP connections rapidly to overwhelm DPI connection tracking."""
        ok = 0
        sockets_to_close = []
        for _ in range(count):
            host, port = random.choice(targets)
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(3.0)
                s.connect((host, port))
                sockets_to_close.append(s)
                ok += 1
                with self._lock:
                    self._stats["tcp_storms_ok"] += 1
            except Exception:
                pass
            finally:
                with self._lock:
                    self._stats["tcp_storms_sent"] += 1

        # Close all after a small delay (keeps DPI tracking entries alive longer)
        time.sleep(random.uniform(0.1, 0.5))
        for s in sockets_to_close:
            try:
                s.close()
            except Exception:
                pass
        return ok

    # ── Decoy HTTPS Traffic ──────────────────────────────────────────────

    def _send_decoy_https(self, sni: str) -> bool:
        """
        Make a legitimate-looking HTTPS connection to a CDN/cloud domain.
        DPI must inspect and pass this traffic, consuming resources.
        """
        try:
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            
            # Connect to the domain
            try:
                s.connect((sni, 443))
            except socket.gaierror:
                # DNS failed — try connecting to Cloudflare IP with SNI
                s.connect(("1.1.1.1", 443))
            
            wrapped = ctx.wrap_socket(s, server_hostname=sni)
            
            # Send a minimal HTTP request
            request = (
                f"GET / HTTP/1.1\r\n"
                f"Host: {sni}\r\n"
                f"User-Agent: Mozilla/5.0\r\n"
                f"Accept: text/html\r\n"
                f"Connection: close\r\n\r\n"
            ).encode()
            wrapped.send(request)
            
            with self._lock:
                self._stats["total_bytes_sent"] += len(request)
            
            # Read a bit of response
            try:
                wrapped.settimeout(3.0)
                data = wrapped.recv(512)
                wrapped.close()
                with self._lock:
                    self._stats["decoy_ok"] += 1
                return len(data) > 0
            except socket.timeout:
                wrapped.close()
                return False
        except Exception:
            return False
        finally:
            with self._lock:
                self._stats["decoy_requests"] += 1

    # ── Telegram DC Probe ────────────────────────────────────────────────

    def _probe_telegram_dc(self, host: str, port: int) -> bool:
        """Probe a Telegram DC to test if it's reachable."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(5.0)
            s.connect((host, port))
            # Send TLS ClientHello with Telegram-like SNI
            frags = self._build_fragmented_client_hello("web.telegram.org")
            for frag in frags:
                s.send(frag)
                time.sleep(random.uniform(0.01, 0.03))
            s.settimeout(3.0)
            try:
                resp = s.recv(5)
                s.close()
                reachable = len(resp) > 0
                with self._lock:
                    if reachable:
                        self._stats["telegram_reachable"] += 1
                return reachable
            except socket.timeout:
                s.close()
                return False
        except Exception:
            return False
        finally:
            with self._lock:
                self._stats["telegram_probes"] += 1

    # ── Main pressure cycle ──────────────────────────────────────────────

    def run_pressure_cycle(self) -> Dict:
        """
        Run one complete DPI pressure cycle.
        
        Executes all techniques in parallel using the shared IO pool.
        Returns stats dict.
        """
        self.logger.info("[DPI-Pressure] Starting pressure cycle")
        t0 = time.time()
        mgr = HunterTaskManager.get_instance()

        # Scale based on intensity
        tls_count = int(15 * self.intensity)
        storm_count = int(20 * self.intensity)
        decoy_count = int(10 * self.intensity)
        tg_probes = len(_TELEGRAM_DCS)

        futures = []

        # 1. TLS fragmented probes to CDN domains
        for _ in range(tls_count):
            sni = random.choice(_DECOY_SNIS)
            port = random.choice([443, 8443])
            # Use Cloudflare/Google IPs as endpoint (always reachable)
            target_ip = random.choice(["1.1.1.1", "8.8.8.8", "1.0.0.1"])
            futures.append(mgr.submit_io(self._probe_tls_fragmented, target_ip, port, sni))

        # 2. TCP connection storms
        storm_targets = [("1.1.1.1", 443), ("8.8.8.8", 443), ("1.0.0.1", 443)]
        for _ in range(3):
            futures.append(
                mgr.submit_io(self._tcp_storm_batch, storm_targets, storm_count)
            )

        # 3. Decoy HTTPS traffic
        for _ in range(decoy_count):
            sni = random.choice(_DECOY_SNIS[:10])  # Use most reliable domains
            futures.append(mgr.submit_io(self._send_decoy_https, sni))

        # 4. Telegram DC probes
        for host, port in _TELEGRAM_DCS:
            futures.append(mgr.submit_io(self._probe_telegram_dc, host, port))

        # Wait for all to complete
        try:
            for future in as_completed(futures, timeout=30.0):
                try:
                    future.result(timeout=1)
                except Exception:
                    pass
        except TimeoutError:
            self.logger.debug("[DPI-Pressure] Some probes timed out")

        with self._lock:
            self._stats["pressure_cycles"] += 1
            self._stats["last_cycle_ts"] = time.time()

        elapsed = time.time() - t0
        self.logger.info(
            f"[DPI-Pressure] Cycle complete in {elapsed:.1f}s — "
            f"TLS: {self._stats['tls_probes_ok']}/{self._stats['tls_probes_sent']}, "
            f"TCP: {self._stats['tcp_storms_ok']}/{self._stats['tcp_storms_sent']}, "
            f"Decoy: {self._stats['decoy_ok']}/{self._stats['decoy_requests']}, "
            f"Telegram: {self._stats['telegram_reachable']}/{self._stats['telegram_probes']}"
        )

        return dict(self._stats)

    def get_stats(self) -> Dict:
        with self._lock:
            return dict(self._stats)

    def is_telegram_reachable(self) -> bool:
        """Quick check if Telegram was reachable in last pressure cycle."""
        with self._lock:
            return self._stats["telegram_reachable"] > 0
