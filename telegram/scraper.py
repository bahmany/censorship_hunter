"""
Telegram integration module.

This module handles Telegram client connection, scraping proxy configs
from channels, and reporting validated results back to Telegram.
"""

import asyncio
import logging
import threading
import time
from typing import Any, Dict, List, Optional, Set, Tuple

try:
    from telethon import TelegramClient
    from telethon.errors import FloodWaitError, SessionPasswordNeededError, ChannelPrivateError, UsernameNotOccupiedError, UsernameInvalidError, SecurityError
    from telethon.tl.functions.channels import GetMessagesRequest
    from telethon.tl.functions.messages import GetHistoryRequest
    from telethon.tl.types import Message
    TELETHON_AVAILABLE = True
except ImportError:
    TelegramClient = None
    TELETHON_AVAILABLE = False

try:
    import paramiko
    import socks
    PARAMIKO_AVAILABLE = True
except ImportError:
    paramiko = None
    PARAMIKO_AVAILABLE = False

import os
import sys
import json
import io

try:
    from hunter.core.config import HunterConfig
    from hunter.core.utils import extract_raw_uris_from_text
    from hunter.config.cache import ResilientHeartbeat
    from hunter.telegram.interactive_auth import InteractiveTelegramAuth
    from hunter.ssh_config_manager import SSHConfigManager, ConfigCacheManager
except ImportError:
    # Fallback for direct execution
    try:
        from core.config import HunterConfig
        from core.utils import extract_raw_uris_from_text
        from config.cache import ResilientHeartbeat
        from telegram.interactive_auth import InteractiveTelegramAuth
        from ssh_config_manager import SSHConfigManager, ConfigCacheManager
    except ImportError:
        # Final fallback - set to None if imports fail
        HunterConfig = None
        extract_raw_uris_from_text = None
        ResilientHeartbeat = None
        InteractiveTelegramAuth = None
        SSHConfigManager = None
        ConfigCacheManager = None


class TelegramScraper:
    """Handles Telegram scraping for proxy configurations."""

    # SECURITY: SSH credentials MUST be provided via environment variables.
    # Set SSH_SERVERS env var as JSON array, or individual SSH{idx}_HOST/PORT/USER/PASS vars.
    # Example: SSH_SERVERS='[{"host":"1.2.3.4","port":22,"username":"user","password":"pass"}]'
    DEFAULT_SSH_SERVERS = []
    DEFAULT_SSH_SOCKS_HOST = "127.0.0.1"
    DEFAULT_SSH_SOCKS_PORT = 1088

    @classmethod
    def _load_ssh_servers(cls) -> List[Dict[str, Any]]:
        """Load SSH server definitions from environment or fall back to defaults."""
        raw_value = os.getenv("SSH_SERVERS") or os.getenv("SSH_SERVERS_JSON")
        if raw_value:
            try:
                parsed = json.loads(raw_value)
                if isinstance(parsed, list) and all(isinstance(item, dict) for item in parsed):
                    return parsed
                logging.getLogger(__name__).warning("SSH_SERVERS env var is not a list of dicts; using defaults")
            except json.JSONDecodeError as exc:
                logging.getLogger(__name__).warning(f"Invalid SSH_SERVERS JSON ({exc}); falling back to SSH1_* vars/defaults")

        # Robust alternative to JSON (compatible with run.bat parsing)
        servers: List[Dict[str, Any]] = []
        for idx in range(1, 10):
            host = os.getenv(f"SSH{idx}_HOST")
            if not host:
                continue
            port_raw = os.getenv(f"SSH{idx}_PORT", "22")
            user = os.getenv(f"SSH{idx}_USER")
            password = os.getenv(f"SSH{idx}_PASS")
            try:
                port = int(port_raw)
            except Exception:
                port = 22
            server: Dict[str, Any] = {"host": host, "port": port}
            if user:
                server["username"] = user
            if password:
                server["password"] = password
            servers.append(server)
        if servers:
            return servers

        # fall back to defaults (return copy to avoid accidental mutation)
        return [dict(server) for server in cls.DEFAULT_SSH_SERVERS]

    def __init__(self, config: HunterConfig):
        self.config = config
        self.client: Optional[TelegramClient] = None
        self.heartbeat = ResilientHeartbeat()
        self.logger = logging.getLogger(__name__)
        self.ssh_tunnel = None
        self.local_proxy_port = None
        self._socks_server = None
        self._socks_thread = None
        self._telegram_lock = threading.Lock()
        self._db_locked_count = 0
        
        # Initialize SSH config and cache managers
        self.ssh_config_manager = SSHConfigManager()
        self.cache_manager = ConfigCacheManager()
        self.ssh_servers = self.ssh_config_manager.get_servers()
        self.ssh_socks_host = os.getenv("SSH_SOCKS_HOST", self.DEFAULT_SSH_SOCKS_HOST)
        self.ssh_socks_port = int(os.getenv("SSH_SOCKS_PORT", str(self.DEFAULT_SSH_SOCKS_PORT)))
        
        # Initialize interactive authentication
        if InteractiveTelegramAuth:
            self.telegram_auth = InteractiveTelegramAuth(self.logger)
        else:
            self.telegram_auth = None
            self.logger.warning("Interactive authentication not available")

        if not TELETHON_AVAILABLE:
            self.logger.warning("Telethon not available - Telegram functionality disabled")
        if not PARAMIKO_AVAILABLE:
            self.logger.warning("Paramiko not available - SSH tunnel disabled")

    def establish_ssh_tunnel(self) -> Optional[int]:
        """Establish an SSH-backed local SOCKS5 proxy (ssh -D equivalent).

        This avoids requiring a SOCKS server to be running on the remote host.
        """
        if not PARAMIKO_AVAILABLE:
            return None

        if self._socks_server and self.local_proxy_port:
            return self.local_proxy_port

        if not self.ssh_servers:
            self.logger.info("No SSH servers configured - skipping tunnel")
            return None

        import threading
        import socket

        def _recv_exact(sock_obj, size: int) -> bytes:
            buf = b""
            while len(buf) < size:
                chunk = sock_obj.recv(size - len(buf))
                if not chunk:
                    return b""
                buf += chunk
            return buf

        def _pipe_socket_to_chan(src_sock: socket.socket, dst_chan) -> None:
            try:
                while True:
                    data = src_sock.recv(4096)
                    if not data:
                        break
                    dst_chan.send(data)
            except Exception:
                pass
            finally:
                try:
                    dst_chan.shutdown_write()
                except Exception:
                    pass

        def _pipe_chan_to_socket(src_chan, dst_sock: socket.socket) -> None:
            try:
                while True:
                    data = src_chan.recv(4096)
                    if not data:
                        break
                    dst_sock.sendall(data)
            except Exception:
                pass
            finally:
                try:
                    dst_sock.shutdown(socket.SHUT_WR)
                except Exception:
                    pass

        def _handle_socks_client(client_sock: socket.socket, client_addr, transport) -> None:
            try:
                client_sock.settimeout(20)

                # Greeting
                header = _recv_exact(client_sock, 2)
                if not header or header[0] != 0x05:
                    return
                nmethods = header[1]
                methods = _recv_exact(client_sock, nmethods)
                if not methods:
                    return
                # no-auth
                client_sock.sendall(b"\x05\x00")

                # Request
                req = _recv_exact(client_sock, 4)
                if not req or req[0] != 0x05:
                    return
                cmd = req[1]
                atyp = req[3]
                if cmd != 0x01:
                    # command not supported
                    client_sock.sendall(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")
                    return

                if atyp == 0x01:  # IPv4
                    addr_bytes = _recv_exact(client_sock, 4)
                    if not addr_bytes:
                        return
                    dest_host = socket.inet_ntoa(addr_bytes)
                elif atyp == 0x03:  # Domain
                    ln_b = _recv_exact(client_sock, 1)
                    if not ln_b:
                        return
                    ln = ln_b[0]
                    host_b = _recv_exact(client_sock, ln)
                    if not host_b:
                        return
                    dest_host = host_b.decode("utf-8", errors="ignore")
                elif atyp == 0x04:  # IPv6
                    addr_bytes = _recv_exact(client_sock, 16)
                    if not addr_bytes:
                        return
                    dest_host = socket.inet_ntop(socket.AF_INET6, addr_bytes)
                else:
                    client_sock.sendall(b"\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00")
                    return

                port_bytes = _recv_exact(client_sock, 2)
                if not port_bytes:
                    return
                dest_port = int.from_bytes(port_bytes, "big")

                # Open SSH channel to destination
                try:
                    chan = transport.open_channel(
                        "direct-tcpip",
                        (dest_host, dest_port),
                        (client_addr[0], client_addr[1])
                    )
                except Exception:
                    client_sock.sendall(b"\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00")
                    return

                # success reply
                client_sock.sendall(b"\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00")

                t1 = threading.Thread(target=_pipe_socket_to_chan, args=(client_sock, chan), daemon=True)
                t2 = threading.Thread(target=_pipe_chan_to_socket, args=(chan, client_sock), daemon=True)
                t1.start()
                t2.start()
                t1.join()
                t2.join()
            except Exception:
                return
            finally:
                try:
                    client_sock.close()
                except Exception:
                    pass

        def _socks_accept_loop(server_sock: socket.socket, transport) -> None:
            while True:
                try:
                    client_sock, client_addr = server_sock.accept()
                    threading.Thread(
                        target=_handle_socks_client,
                        args=(client_sock, client_addr, transport),
                        daemon=True
                    ).start()
                except OSError:
                    break
                except Exception:
                    continue

        for server_idx, server in enumerate(self.ssh_servers, 1):
            try:
                host = server['host']
                port = server['port']
                self.logger.info(f"[{server_idx}/{len(self.ssh_servers)}] Attempting SSH tunnel to {host}:{port}")
                
                ssh = paramiko.SSHClient()
                ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                
                username = server.get('username') or server.get('user') or server.get('login') or server.get('uname') or ""
                password = server.get('password') or server.get('pass') or ""
                
                self.logger.debug(f"SSH auth: user={username}, pass={'*' * len(password) if password else 'none'}")
                
                ssh.connect(
                    hostname=host,
                    port=port,
                    username=username,
                    password=password,
                    timeout=30,
                    banner_timeout=30,
                    auth_timeout=30,
                    look_for_keys=False,
                    allow_agent=False
                )

                transport = ssh.get_transport()
                if transport is None:
                    raise RuntimeError("SSH transport is not available")
                
                self.logger.info(f"SSH connected to {host}:{port}, establishing SOCKS tunnel...")

                # Create local SOCKS server socket
                server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                server_sock.bind(("127.0.0.1", 0))
                server_sock.listen(50)
                local_port = server_sock.getsockname()[1]

                t = threading.Thread(target=_socks_accept_loop, args=(server_sock, transport), daemon=True)
                t.start()

                self._socks_server = server_sock
                self._socks_thread = t
                self.ssh_tunnel = ssh
                self.local_proxy_port = local_port
                self.logger.info(f"[SUCCESS] SSH tunnel established: {host}:{port} -> localhost:{local_port}")
                return local_port
                
            except paramiko.AuthenticationException as e:
                self.logger.warning(f"SSH auth failed to {server['host']}:{server['port']}: {e}")
                continue
            except paramiko.SSHException as e:
                self.logger.warning(f"SSH error to {server['host']}:{server['port']}: {e}")
                continue
            except socket.timeout as e:
                self.logger.warning(f"SSH timeout to {server['host']}:{server['port']}: {e}")
                continue
            except Exception as e:
                self.logger.warning(f"SSH tunnel failed to {server['host']}:{server['port']}: {type(e).__name__}: {e}")
                continue
        
        self.logger.error(f"Failed to establish SSH tunnel to any of {len(self.ssh_servers)} servers")
        return None

    def close_ssh_tunnel(self):
        """Close the SSH tunnel."""
        if self._socks_server:
            try:
                self._socks_server.close()
            except Exception:
                pass
            self._socks_server = None
        if self.ssh_tunnel:
            try:
                self.ssh_tunnel.close()
            except Exception:
                pass
            self.ssh_tunnel = None
        self.local_proxy_port = None
        self.logger.info("SSH tunnel closed")
    
    async def _try_v2ray_proxy_fallback(self) -> Optional[int]:
        """Try to establish V2Ray proxy tunnel using validated configs from cache.
        
        Returns:
            Port number if successful, None otherwise
        """
        try:
            from hunter.proxy_fallback import ProxyTunnelManager
            
            # Load cached validated configs
            cached_configs = self._load_cached_validated_configs()
            if not cached_configs:
                self.logger.warning("No cached validated configs for proxy fallback")
                return None
            
            self.logger.info(f"Attempting V2Ray proxy fallback with {len(cached_configs)} cached configs")
            
            # Try to establish tunnel
            tunnel_manager = ProxyTunnelManager()
            tunnel_port = tunnel_manager.establish_tunnel(
                cached_configs,
                max_latency_ms=200,
                socks_port=11088
            )
            
            if tunnel_port:
                self.logger.info(f"[SUCCESS] V2Ray proxy tunnel established on port {tunnel_port}")
                # Store for cleanup later
                self._v2ray_tunnel_manager = tunnel_manager
                return tunnel_port
            
            return None
            
        except Exception as e:
            self.logger.debug(f"V2Ray proxy fallback error: {e}")
            return None
    
    def _load_cached_validated_configs(self) -> List:
        """Load cached validated configs and parse them."""
        try:
            from hunter.config.cache import SmartCache
            from hunter.parsers import UniversalParser
            from hunter.core.models import HunterBenchResult
            
            cache = SmartCache()
            cached_uris = cache.load_cached_configs(max_count=50, working_only=True)
            
            if not cached_uris:
                return []
            
            parser = UniversalParser()
            configs = []
            
            for uri in cached_uris[:20]:  # Use top 20 cached
                try:
                    parsed = parser.parse(uri)
                    if parsed:
                        result = HunterBenchResult(
                            uri=uri,
                            outbound=parsed.outbound,
                            host=parsed.host,
                            port=parsed.port,
                            identity=parsed.identity,
                            ps=parsed.ps,
                            latency_ms=100.0,  # Assume cached configs are reasonably fast
                            ip=None,
                            country_code=None,
                            region="Unknown",
                            tier="silver"
                        )
                        configs.append(result)
                except Exception:
                    continue
            
            return configs
            
        except Exception as e:
            self.logger.debug(f"Failed to load cached configs: {e}")
            return []

    async def _reset_client(self) -> None:
        """Properly disconnect Telethon client to avoid 'Task destroyed but pending' warnings."""
        if not self.client:
            return
        try:
            if self.client.is_connected():
                # Disconnect gracefully first
                await self.client.disconnect()
        except Exception:
            pass
        
        # Give Telethon's internal tasks time to clean up
        try:
            await asyncio.sleep(0.5)
        except Exception:
            pass
        
        # Cancel any remaining internal tasks from Telethon
        try:
            for task in asyncio.all_tasks():
                if task is asyncio.current_task():
                    continue
                task_name = task.get_name() if hasattr(task, 'get_name') else str(task)
                coro = getattr(task, '_coro', None)
                coro_name = ''
                if coro:
                    coro_name = getattr(coro, '__qualname__', '')
                
                # Check if this is a Telethon internal connection task
                if '_send_loop' in coro_name or '_recv_loop' in coro_name or 'telethon' in coro_name.lower():
                    if not task.done():
                        try:
                            task.cancel()
                        except Exception:
                            pass
        except Exception:
            pass
        
        # Wait a bit for cancellations to process
        try:
            await asyncio.sleep(0.3)
        except Exception:
            pass
        
        try:
            sess = getattr(self.client, "session", None)
            if sess is not None:
                try:
                    sess.close()
                except Exception:
                    pass
        except Exception:
            pass
        
        self.client = None

    def _runtime_dir(self) -> str:
        try:
            state_file = str(self.config.get("state_file", "") or "")
            if state_file:
                base_dir = os.path.dirname(state_file)
                if base_dir:
                    return base_dir
        except Exception:
            pass
        return os.path.join(os.path.dirname(os.path.dirname(__file__)), "runtime")

    def _session_base_path(self) -> str:
        try:
            raw = str(self.config.get("session_name", "hunter_session") or "hunter_session")
        except Exception:
            raw = "hunter_session"

        raw = raw.strip().strip('"').strip("'")
        if raw.lower().endswith(".session"):
            raw = raw[:-len(".session")]

        if os.path.isabs(raw):
            return raw
        if any(sep in raw for sep in ("/", "\\")):
            return os.path.abspath(raw)
        return os.path.join(self._runtime_dir(), raw)

    def _delete_session_files(self) -> None:
        """Best-effort delete Telethon session files (requires re-login)."""
        base = self._session_base_path()
        for suffix in (".session", ".session-journal", ".session-wal", ".session-shm"):
            path = base + suffix
            try:
                if os.path.exists(path):
                    os.remove(path)
            except Exception:
                pass

    def _check_other_hunter_processes(self) -> bool:
        """Check if other Hunter processes are running."""
        try:
            import subprocess
            import sys
            
            current_pid = os.getpid()
            
            if sys.platform == "win32":
                # Windows: use wmic to check command lines of python processes
                result = subprocess.run(
                    ["wmic", "process", "where", "name='python.exe'", "get", "CommandLine,ProcessId", "/FORMAT:CSV"],
                    capture_output=True, text=True, timeout=5
                )
                
                for line in result.stdout.splitlines():
                    if not line or line.startswith("Node,CommandLine,ProcessId"):
                        continue
                    
                    parts = line.split(',')
                    if len(parts) >= 3:
                        cmd_line = parts[1].strip('"')
                        pid_str = parts[2].strip()
                        
                        # Check if this is a Hunter process and not our own
                        if ("hunter" in cmd_line.lower() or "pythonProject1" in cmd_line.lower()) and pid_str != str(current_pid):
                            try:
                                # Verify the process is actually running
                                subprocess.run(["tasklist", "/FI", f"PID {pid_str}"], 
                                             capture_output=True, timeout=2)
                                return True
                            except:
                                continue
            else:
                # Linux/Mac: use pgrep
                result = subprocess.run(
                    ["pgrep", "-f", "hunter"],
                    capture_output=True, text=True, timeout=3
                )
                
                for pid_str in result.stdout.strip().split('\n'):
                    if pid_str and pid_str.strip() != str(current_pid):
                        return True
                        
        except Exception as e:
            self.logger.debug(f"Failed to check other Hunter processes: {e}")
        
        return False

    async def connect(self, use_proxy_fallback: bool = True, _retry: bool = False) -> bool:
        """Establish connection to Telegram.
        
        Args:
            use_proxy_fallback: If True, use V2Ray proxy tunnel if SSH fails
            _retry: Internal flag to prevent infinite retry loops
        """
        if not TELETHON_AVAILABLE:
            return False

        # Prevent concurrent access to Telethon session (SQLite is single-writer)
        # Use threading.Lock because web dashboard calls from a different event loop/thread
        acquired = self._telegram_lock.acquire(blocking=False)
        if not acquired:
            self.logger.info("Telegram connection already in progress, waiting...")
            # Wait in a non-blocking way so we don't freeze the event loop
            while not self._telegram_lock.acquire(blocking=False):
                await asyncio.sleep(0.5)
        
        try:
            return await self._connect_inner(use_proxy_fallback, _retry)
        finally:
            self._telegram_lock.release()

    async def _connect_inner(self, use_proxy_fallback: bool = True, _retry: bool = False, _lock_retry: int = 0) -> bool:
        """Inner connection logic — tries multiple proxy methods until one works.

        Order: env proxy → SSH tunnel → Tor → balancer → V2Ray cache → direct.
        Each method gets its own TelegramClient attempt so a single failure
        doesn't block the rest.
        """
        if self.client and await self.heartbeat.check_connection(self.client):
            return True

        # ── Build ordered list of (label, proxy_tuple | None) candidates ──
        candidates: list = []  # [(label, proxy_or_none), ...]

        # 1) Explicit env proxy
        proxy_host = os.getenv("HUNTER_TELEGRAM_PROXY_HOST")
        proxy_port_raw = os.getenv("HUNTER_TELEGRAM_PROXY_PORT")
        if proxy_host and proxy_port_raw:
            try:
                _pp = int(proxy_port_raw)
                _pu = os.getenv("HUNTER_TELEGRAM_PROXY_USER")
                _ppw = os.getenv("HUNTER_TELEGRAM_PROXY_PASS")
                if _pu or _ppw:
                    candidates.append(("env-proxy", (socks.SOCKS5, proxy_host, _pp, True, _pu or "", _ppw or "")))
                else:
                    candidates.append(("env-proxy", (socks.SOCKS5, proxy_host, _pp)))
            except Exception:
                pass

        # 2) SSH tunnel
        if PARAMIKO_AVAILABLE and self.ssh_servers:
            ssh_port = self.establish_ssh_tunnel()
            if ssh_port:
                candidates.append(("ssh-tunnel", (socks.SOCKS5, '127.0.0.1', ssh_port)))

        # 3) Tor SOCKS5 (start tor.exe if available)
        if use_proxy_fallback:
            tor_port = self._start_tor_socks()
            if tor_port:
                candidates.append(("tor", (socks.SOCKS5, '127.0.0.1', tor_port)))

        # 4) Running balancer SOCKS port
        if use_proxy_fallback:
            bal_proxy = self._get_balancer_proxy()
            if bal_proxy:
                candidates.append(("balancer", bal_proxy))

        # 5) Well-known local SOCKS ports (e.g. v2rayN, Clash, etc.)
        if use_proxy_fallback:
            import socket as _sock
            for _lp in (10808, 1080, 2080, 7890):
                try:
                    with _sock.create_connection(("127.0.0.1", _lp), timeout=0.5):
                        candidates.append((f"local:{_lp}", (socks.SOCKS5, '127.0.0.1', _lp)))
                        break  # only use first working local port
                except Exception:
                    continue

        # 6) V2Ray proxy fallback from cached configs
        if use_proxy_fallback:
            v2_port = await self._try_v2ray_proxy_fallback()
            if v2_port:
                candidates.append(("v2ray-cache", (socks.SOCKS5, '127.0.0.1', v2_port)))

        # 7) Direct (no proxy) — last resort, may work outside Iran
        candidates.append(("direct", None))

        self.logger.info(f"Telegram connect: {len(candidates)} methods to try: {[c[0] for c in candidates]}")

        # ── Try each candidate ──
        for label, proxy in candidates:
            try:
                await self._reset_client()
                self.logger.info(f"[TG-Connect] Trying {label}...")

                self.client = TelegramClient(
                    self._session_base_path(),
                    self.config.get("api_id"),
                    self.config.get("api_hash"),
                    proxy=proxy,
                    connection_retries=1,
                    auto_reconnect=False,
                    timeout=15,
                )

                await asyncio.wait_for(self.client.connect(), timeout=20)

                if not await self.client.is_user_authorized():
                    ok = await self._try_authorize()
                    if not ok:
                        return False

                if await self.client.is_user_authorized():
                    self.logger.info(f"[TG-Connect] Connected via {label}")
                    return True

            except SecurityError as e:
                msg = str(e)
                self.logger.warning(f"Telegram SecurityError via {label}: {msg}")
                await self._reset_client()
                self.close_ssh_tunnel()
                reset_ok = os.getenv("HUNTER_RESET_TELEGRAM_SESSION", "false").lower() == "true"
                if reset_ok and ("wrong session" in msg.lower() or "session id" in msg.lower()):
                    self.logger.warning("Resetting session files due to SecurityError")
                    self._delete_session_files()
                return False  # SecurityError = don't try other proxies

            except Exception as e:
                msg = str(e)

                # Database locked — special retry logic
                if "database is locked" in msg.lower():
                    return await self._handle_db_locked(
                        use_proxy_fallback=use_proxy_fallback,
                        _retry=_retry,
                        _lock_retry=_lock_retry,
                    )

                self.logger.info(f"[TG-Connect] {label} failed: {type(e).__name__}: {e}")
                await self._reset_client()
                continue  # try next method

        self.logger.warning("[TG-Connect] All methods exhausted — Telegram unavailable this cycle")
        return False

    async def _try_authorize(self) -> bool:
        """Handle Telegram user authorization (send code / 2FA)."""
        phone = self.config.get("phone")
        if not phone:
            self.logger.info("Phone number not set - Telegram scraping disabled")
            return False

        _web_active = False
        try:
            from web.auth_bridge import AuthBridge
            _web_active = AuthBridge().active
        except ImportError:
            pass
        if not _web_active and (not sys.stdin or not sys.stdin.isatty()):
            self.logger.info("Non-interactive mode - Telegram user auth skipped (use Bot API for reporting)")
            return False

        if self.telegram_auth:
            if not await self.telegram_auth.authenticate(self.client, phone):
                self.logger.info("Telegram authentication not completed")
                return False
        else:
            self.logger.info("Using basic Telegram auth")
            await self.client.send_code_request(phone)
            code = input("Enter the Telegram login code: ").strip()
            if code:
                try:
                    await self.client.sign_in(phone=phone, code=code)
                except SessionPasswordNeededError:
                    pwd = input("Enter Telegram 2FA password: ").strip()
                    if pwd:
                        await self.client.sign_in(password=pwd)
            else:
                self.logger.info("Telegram authentication cancelled")
                return False
        return True

    async def _handle_db_locked(self, use_proxy_fallback: bool, _retry: bool, _lock_retry: int) -> bool:
        """Handle 'database is locked' errors with retry and optional session reset."""
        self._db_locked_count += 1
        try:
            sp = self._session_base_path() + ".session"
        except Exception:
            sp = "(unknown)"
        self.logger.warning(
            f"Telegram session database is locked (attempt #{self._db_locked_count}, "
            f"lock_retry={_lock_retry}, session={sp})"
        )
        await self._reset_client()

        max_lock_retries = int(os.getenv("HUNTER_TELEGRAM_LOCK_RETRIES", "3"))
        if _lock_retry < max_lock_retries:
            await asyncio.sleep(min(5.0, 0.8 * (_lock_retry + 1)))
            return await self._connect_inner(
                use_proxy_fallback=use_proxy_fallback,
                _retry=_retry,
                _lock_retry=_lock_retry + 1,
            )

        if _retry:
            self.logger.info("Already retried after session reset - giving up")
            return False

        auto_reset = os.getenv("HUNTER_AUTO_RESET_LOCKED_SESSION", "false").lower() == "true"
        smart_auto_reset = os.getenv("HUNTER_SMART_AUTO_RESET", "true").lower() == "true"
        other_hunter_running = self._check_other_hunter_processes()

        if auto_reset or (smart_auto_reset and not other_hunter_running):
            if not other_hunter_running:
                self.logger.info("No other Hunter processes detected - safe to reset session")
            self.logger.info("Deleting locked session files and retrying...")
            self._delete_session_files()
            await asyncio.sleep(1)
            return await self._connect_inner(use_proxy_fallback=use_proxy_fallback, _retry=True, _lock_retry=0)

        if other_hunter_running:
            self.logger.warning("Other Hunter processes detected - not auto-resetting")
        self.logger.info("Tip: Set HUNTER_AUTO_RESET_LOCKED_SESSION=true to force auto-reset")
        return False

    def _start_tor_socks(self) -> Optional[int]:
        """Start a local Tor SOCKS5 proxy if tor binary is available.

        Returns the SOCKS port if successful, None otherwise.
        """
        if getattr(self, '_tor_process', None) and getattr(self, '_tor_port', None):
            # Already running
            try:
                if self._tor_process.poll() is None:
                    return self._tor_port
            except Exception:
                pass

        import socket as _sock
        import subprocess as _sp

        tor_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "tor.exe")
        if not os.path.isfile(tor_path):
            # Try non-Windows name
            tor_path2 = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "tor")
            if os.path.isfile(tor_path2):
                tor_path = tor_path2
            else:
                return None

        # Find a free port
        tor_port = 9150
        for candidate_port in (9150, 9250, 9350):
            try:
                s = _sock.socket(_sock.AF_INET, _sock.SOCK_STREAM)
                s.bind(("127.0.0.1", candidate_port))
                s.close()
                tor_port = candidate_port
                break
            except OSError:
                continue

        try:
            runtime = os.path.join(os.path.dirname(os.path.dirname(__file__)), "runtime", "tor_data")
            os.makedirs(runtime, exist_ok=True)

            cmd = [
                tor_path,
                "--SocksPort", str(tor_port),
                "--DataDirectory", runtime,
                "--Log", "err stderr",
            ]
            creation_flags = 0
            if sys.platform == "win32":
                creation_flags = _sp.CREATE_NO_WINDOW

            proc = _sp.Popen(cmd, stdout=_sp.PIPE, stderr=_sp.PIPE, creationflags=creation_flags)

            # Wait for Tor to bootstrap (up to 25 seconds)
            for _ in range(50):
                time.sleep(0.5)
                if proc.poll() is not None:
                    self.logger.info(f"Tor exited early (rc={proc.returncode})")
                    return None
                try:
                    with _sock.create_connection(("127.0.0.1", tor_port), timeout=1):
                        self._tor_process = proc
                        self._tor_port = tor_port
                        self.logger.info(f"[Tor] SOCKS5 ready on port {tor_port}")
                        return tor_port
                except Exception:
                    continue

            # Timeout
            proc.terminate()
            self.logger.info("[Tor] Failed to bootstrap within 25s")
            return None

        except Exception as e:
            self.logger.debug(f"[Tor] Start failed: {e}")
            return None

    def _get_balancer_proxy(self) -> Optional[tuple]:
        """Get proxy tuple from a running Hunter balancer if available."""
        try:
            # The orchestrator stores itself at a well-known module attribute
            import hunter.orchestrator as _omod
            orch = getattr(_omod, '_global_orchestrator', None)
            if orch is None:
                return None
            bal = getattr(orch, 'balancer', None)
            if bal and getattr(bal, '_running', False) and getattr(bal, '_backends', None):
                return (socks.SOCKS5, '127.0.0.1', bal.port)
        except Exception:
            pass
        return None

    async def scrape_configs(self, channels: List[str], limit: int = 100) -> Set[str]:
        """Scrape proxy configurations from Telegram channels."""
        # Acquire lock to prevent concurrent Telegram access (database locked)
        # Use threading.Lock because web dashboard calls from a different event loop/thread
        acquired = self._telegram_lock.acquire(blocking=False)
        if not acquired:
            self.logger.info("Another Telegram operation in progress, waiting...")
            wait_start = time.time()
            while not self._telegram_lock.acquire(blocking=False):
                if time.time() - wait_start > 60:
                    self.logger.warning("Telegram lock wait timeout (60s), skipping scrape")
                    return set()
                await asyncio.sleep(0.5)
        
        try:
            return await self._scrape_configs_inner(channels, limit)
        finally:
            self._telegram_lock.release()

    async def _scrape_configs_inner(self, channels: List[str], limit: int = 100) -> Set[str]:
        """Inner scrape logic (must be called under _telegram_lock)."""
        if not self.client or not await self.heartbeat.check_connection(self.client):
            if not await self._connect_inner():
                return set()

        configs: Set[str] = set()

        dialog_entities: Dict[str, Any] = {}
        try:
            dialogs = await asyncio.wait_for(self.client.get_dialogs(limit=300), timeout=18)
            for d in dialogs or []:
                try:
                    ent = getattr(d, "entity", None)
                    uname = getattr(ent, "username", None)
                    if uname:
                        dialog_entities[str(uname).lower()] = ent
                except Exception:
                    continue
        except Exception:
            pass

        try:
            deadline_s = float(os.getenv("HUNTER_TG_DEADLINE", "90"))
        except Exception:
            deadline_s = 90.0
        deadline_s = max(15.0, min(240.0, deadline_s))
        started = time.time()

        try:
            max_concurrent = int(os.getenv("HUNTER_TG_CONCURRENCY", "3"))
        except Exception:
            max_concurrent = 3
        max_concurrent = max(1, min(5, max_concurrent))

        try:
            target_total = int(os.getenv("HUNTER_TG_MAX_TOTAL", str(int(limit) * min(6, max(1, len(channels))))))
        except Exception:
            target_total = int(limit) * 3
        target_total = max(10, min(800, target_total))

        sem = asyncio.Semaphore(max_concurrent)

        async def _scrape_channel(channel: str) -> Tuple[str, List[str], str, int, str]:
            async with sem:
                if not self.client:
                    return (channel, [], "no_client", 0, "")
                try:
                    if not self.client.is_connected():
                        await self.client.connect()
                except Exception:
                    pass

                raw = (channel or "").strip()
                uname = raw.lstrip("@").strip()

                if uname:
                    ent = dialog_entities.get(uname.lower())
                    if ent is not None:
                        entity = ent
                    else:
                        entity = None
                else:
                    entity = None

                refs: List[str] = []
                for r in (raw, uname, f"@{uname}" if uname else "", f"https://t.me/{uname}" if uname else ""):
                    if r and r not in refs:
                        refs.append(r)

                entity_err = ""

                if entity is None:
                    for channel_ref in refs:
                        try:
                            entity = await asyncio.wait_for(self.client.get_entity(channel_ref), timeout=12)
                            entity_err = ""
                            break
                        except Exception as e:
                            entity_err = type(e).__name__
                            try:
                                self.logger.debug(f"Telegram get_entity failed for {channel_ref}: {entity_err}: {e}")
                            except Exception:
                                pass

                if entity is None:
                    return (channel, [], "entity_fail", 0, entity_err)

                try:
                    fetch_limit = int(os.getenv("HUNTER_TG_HISTORY_LIMIT", "50"))
                except Exception:
                    fetch_limit = 50
                fetch_limit = max(10, min(400, fetch_limit))

                try:
                    messages = await asyncio.wait_for(
                        self.client(GetHistoryRequest(
                            peer=entity,
                            limit=fetch_limit,
                            offset_date=None,
                            offset_id=0,
                            max_id=0,
                            min_id=0,
                            add_offset=0,
                            hash=0
                        )),
                        timeout=15
                    )
                except FloodWaitError as e:
                    try:
                        await asyncio.sleep(min(max(int(e.seconds), 1), 10))
                    except Exception:
                        pass
                    return (channel, [], "flood_wait", 0, "FloodWaitError")
                except Exception as e:
                    try:
                        self.logger.debug(f"Telegram GetHistory failed for {raw}: {type(e).__name__}: {e}")
                    except Exception:
                        pass
                    return (channel, [], "history_fail", 0, type(e).__name__)

                channel_configs: Set[str] = set()
                channel_configs_ordered: List[str] = []
                message_list = getattr(messages, "messages", []) or []
                msg_count = len(message_list)

                for message in message_list:
                    if len(channel_configs) >= limit:
                        break

                    if hasattr(message, 'message') and message.message:
                        found = extract_raw_uris_from_text(message.message)
                        for uri in found:
                            if uri not in channel_configs:
                                channel_configs.add(uri)
                                channel_configs_ordered.append(uri)
                                if len(channel_configs) >= limit:
                                    break

                    if len(channel_configs) >= limit:
                        break

                    try:
                        if getattr(message, "media", None) and getattr(message, "file", None):
                            name = (getattr(message.file, "name", None) or "").lower()
                            mime = (getattr(message.file, "mime_type", None) or "").lower()
                            is_text = (
                                mime.startswith("text/")
                                or name.endswith(".txt")
                                or name.endswith(".conf")
                                or name.endswith(".json")
                                or name.endswith(".yaml")
                                or name.endswith(".yml")
                                or name.endswith(".npvt")
                                or name.endswith(".npv")
                                or name.endswith(".nptv")
                            )
                            if is_text:
                                data = await asyncio.wait_for(
                                    self.client.download_media(message, file=bytes),
                                    timeout=12,
                                )
                                if data:
                                    try:
                                        text = data.decode("utf-8", errors="ignore")
                                    except Exception:
                                        text = ""
                                    if text:
                                        found = extract_raw_uris_from_text(text)
                                        for uri in found:
                                            if uri not in channel_configs:
                                                channel_configs.add(uri)
                                                channel_configs_ordered.append(uri)
                                                if len(channel_configs) >= limit:
                                                    break
                    except Exception:
                        pass

                if msg_count <= 0:
                    status = "empty"
                elif not channel_configs_ordered:
                    status = "no_uri"
                else:
                    status = "ok"

                return (channel, channel_configs_ordered, status, msg_count, "")

        tasks = []
        task_to_channel: Dict[asyncio.Task, str] = {}
        for ch in channels:
            try:
                t = asyncio.create_task(_scrape_channel(ch))
                tasks.append(t)
                task_to_channel[t] = ch
            except Exception:
                continue

        try:
            stats = {
                "ok": 0,
                "no_uri": 0,
                "empty": 0,
                "entity_fail": 0,
                "history_fail": 0,
                "flood_wait": 0,
                "no_client": 0,
                "other": 0,
            }
            err_stats: Dict[str, int] = {}
            remaining = max(5.0, deadline_s - (time.time() - started))
            try:
                for t in asyncio.as_completed(tasks, timeout=remaining):
                    try:
                        channel, found_list, status, msg_count, err_name = await t
                        if status in stats:
                            stats[status] += 1
                        else:
                            stats["other"] += 1
                        if err_name:
                            err_stats[err_name] = err_stats.get(err_name, 0) + 1
                        if found_list:
                            for uri in found_list:
                                if uri not in configs:
                                    configs.add(uri)
                        if status == "entity_fail" and err_name:
                            self.logger.info(f"Scraped {len(found_list)} configs from {channel} ({err_name})")
                        else:
                            self.logger.info(f"Scraped {len(found_list)} configs from {channel}")
                        if len(configs) >= target_total:
                            break
                        if time.time() - started >= deadline_s:
                            break
                    except Exception:
                        continue
            except asyncio.TimeoutError:
                pass

            if not configs:
                try:
                    top_err = ""
                    if err_stats:
                        items = sorted(err_stats.items(), key=lambda kv: kv[1], reverse=True)[:3]
                        top_err = " ".join([f"{k}:{v}" for k, v in items])
                    self.logger.info(
                        "Telegram scrape summary: "
                        f"ok={stats['ok']} no_uri={stats['no_uri']} empty={stats['empty']} "
                        f"entity_fail={stats['entity_fail']} history_fail={stats['history_fail']} "
                        f"flood_wait={stats['flood_wait']}" + (f" errors={top_err}" if top_err else "")
                    )
                except Exception:
                    pass
        finally:
            for t in tasks:
                if not t.done():
                    try:
                        t.cancel()
                    except Exception:
                        pass
            try:
                await asyncio.gather(*tasks, return_exceptions=True)
            except Exception:
                pass
            # Properly disconnect client to prevent pending task warnings
            try:
                await self._reset_client()
            except Exception:
                pass

        return configs

    async def send_report(self, report_text: str, parse_mode: Optional[str] = None, chat_id: Optional[Any] = None) -> bool:
        """Send report to Telegram channel."""
        acquired = self._telegram_lock.acquire(blocking=False)
        if not acquired:
            wait_start = time.time()
            while not self._telegram_lock.acquire(blocking=False):
                if time.time() - wait_start > 30:
                    self.logger.warning("send_report: Telegram lock timeout (30s), skipping")
                    return False
                await asyncio.sleep(0.5)
        try:
            if not self.client or not await self.heartbeat.check_connection(self.client):
                if not await self._connect_inner():
                    return False

            channel = chat_id if chat_id is not None else self.config.get("report_channel")
            if not channel:
                return False

            max_retries = int(os.getenv("HUNTER_TELEGRAM_SEND_RETRIES", "6"))
            base_backoff = float(os.getenv("HUNTER_TELEGRAM_SEND_BACKOFF", "1.5"))

            for attempt in range(1, max_retries + 1):
                try:
                    await self.client.send_message(channel, report_text, parse_mode=parse_mode)
                    self.logger.info("Report sent to Telegram")
                    return True
                except FloodWaitError as e:
                    wait_s = int(min(max(e.seconds, 1), 60))
                    self.logger.warning(f"Telegram FloodWait on send_message: {e.seconds}s (sleep {wait_s}s)")
                    await asyncio.sleep(wait_s)
                except Exception as e:
                    self.logger.warning(f"Failed to send report (attempt {attempt}/{max_retries}): {e}")
                    try:
                        if not self.client or not self.client.is_connected():
                            await self._reset_client()
                            await self._connect_inner()
                        else:
                            await self.heartbeat.try_reconnect(self.client)
                    except Exception:
                        pass
                    await asyncio.sleep(min(10.0, base_backoff * attempt))

            return False
        finally:
            self._telegram_lock.release()

    async def send_file(self, filename: str, content: bytes, caption: str = "", chat_id: Optional[Any] = None) -> bool:
        """Send a file to the Telegram report channel."""
        acquired = self._telegram_lock.acquire(blocking=False)
        if not acquired:
            wait_start = time.time()
            while not self._telegram_lock.acquire(blocking=False):
                if time.time() - wait_start > 30:
                    self.logger.warning("send_file: Telegram lock timeout (30s), skipping")
                    return False
                await asyncio.sleep(0.5)
        try:
            if not self.client or not await self.heartbeat.check_connection(self.client):
                if not await self._connect_inner():
                    return False

            channel = chat_id if chat_id is not None else self.config.get("report_channel")
            if not channel:
                return False

            max_retries = int(os.getenv("HUNTER_TELEGRAM_SEND_RETRIES", "6"))
            base_backoff = float(os.getenv("HUNTER_TELEGRAM_SEND_BACKOFF", "1.5"))

            for attempt in range(1, max_retries + 1):
                try:
                    bio = io.BytesIO(content)
                    bio.name = filename
                    await self.client.send_file(channel, file=bio, caption=caption)
                    self.logger.info(f"File sent to Telegram: {filename}")
                    return True
                except FloodWaitError as e:
                    wait_s = int(min(max(e.seconds, 1), 60))
                    self.logger.warning(f"Telegram FloodWait on send_file: {e.seconds}s (sleep {wait_s}s)")
                    await asyncio.sleep(wait_s)
                except Exception as e:
                    self.logger.warning(f"Failed to send file '{filename}' (attempt {attempt}/{max_retries}): {e}")
                    try:
                        if not self.client or not self.client.is_connected():
                            await self._reset_client()
                            await self._connect_inner()
                        else:
                            await self.heartbeat.try_reconnect(self.client)
                    except Exception:
                        pass
                    await asyncio.sleep(min(12.0, base_backoff * attempt))

            return False
        finally:
            self._telegram_lock.release()

    async def disconnect(self):
        """Disconnect from Telegram and close SSH tunnel."""
        # Acquire lock to prevent disconnect during active scrape/connect
        acquired = self._telegram_lock.acquire(blocking=False)
        if not acquired:
            self.logger.info("Waiting for active Telegram operation to finish before disconnect...")
            while not self._telegram_lock.acquire(blocking=False):
                await asyncio.sleep(0.5)
        try:
            await self._reset_client()
            self.close_ssh_tunnel()
        finally:
            self._telegram_lock.release()


class BotReporter:
    """Send messages and files to Telegram via Bot API (HTTP).
    
    Uses TOKEN and CHAT_ID from environment - no Telethon, no SSH tunnel,
    no user session needed. Works directly over the internet.
    
    Enhanced with fallback mechanisms for Iranian censorship.
    """

    BOT_API_URL = "https://api.telegram.org/bot{token}/{method}"

    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.token = os.getenv("TOKEN") or os.getenv("TELEGRAM_BOT_TOKEN")
        self.chat_id = os.getenv("CHAT_ID") or os.getenv("TELEGRAM_GROUP_ID")
        self.enabled = bool(self.token and self.chat_id)
        if not self.enabled:
            self.logger.info("Bot reporter disabled: TOKEN or CHAT_ID not set")
        
        # Initialize fallback sender
        self.fallback_sender = None
        if self.enabled:
            try:
                from .fallback_sender import TelegramFallbackSender
                self.fallback_sender = TelegramFallbackSender()
                self.logger.info("Fallback sender initialized")
            except Exception as e:
                self.logger.warning(f"Failed to initialize fallback sender: {e}")

    # ---- low-level helpers ------------------------------------------------

    def _url(self, method: str) -> str:
        return self.BOT_API_URL.format(token=self.token, method=method)

    async def _post_json(self, method: str, data: dict) -> dict:
        """POST JSON to Bot API and return response dict."""
        import urllib.request
        import urllib.error
        url = self._url(method)
        body = json.dumps(data).encode("utf-8")
        req = urllib.request.Request(
            url, data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        loop = asyncio.get_running_loop()
        try:
            resp = await loop.run_in_executor(
                None,
                lambda: urllib.request.urlopen(req, timeout=30).read(),
            )
            return json.loads(resp)
        except urllib.error.HTTPError as e:
            err_body = e.read().decode(errors="ignore")
            self.logger.info(f"Bot API {method} error {e.code}: {err_body[:200]}")
            return {"ok": False, "description": err_body[:200]}
        except Exception as e:
            self.logger.info(f"Bot API {method} failed: {e}")
            return {"ok": False, "description": str(e)}

    async def _post_multipart(self, method: str, fields: dict,
                               file_field: str, filename: str,
                               file_bytes: bytes) -> dict:
        """POST multipart/form-data (for file uploads)."""
        import urllib.request
        import urllib.error
        boundary = "----HunterBotBoundary"
        body_parts = []
        for key, val in fields.items():
            body_parts.append(
                f"--{boundary}\r\n"
                f"Content-Disposition: form-data; name=\"{key}\"\r\n\r\n"
                f"{val}\r\n"
            )
        body_parts.append(
            f"--{boundary}\r\n"
            f"Content-Disposition: form-data; name=\"{file_field}\"; filename=\"{filename}\"\r\n"
            f"Content-Type: application/octet-stream\r\n\r\n"
        )
        payload = "".join(body_parts).encode("utf-8") + file_bytes + f"\r\n--{boundary}--\r\n".encode("utf-8")

        url = self._url(method)
        req = urllib.request.Request(
            url, data=payload,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            method="POST",
        )
        loop = asyncio.get_running_loop()
        try:
            resp = await loop.run_in_executor(
                None,
                lambda: urllib.request.urlopen(req, timeout=60).read(),
            )
            return json.loads(resp)
        except Exception as e:
            self.logger.info(f"Bot API file upload failed: {e}")
            return {"ok": False, "description": str(e)}

    # ---- public interface (same as TelegramReporter) ----------------------

    async def send_message(self, text: str, parse_mode: str = "Markdown", chat_id: Optional[str] = None) -> bool:
        if not self.enabled:
            return False
        target = chat_id or self.chat_id
        if not target:
            return False
        
        # Try fallback sender first if available
        if self.fallback_sender:
            try:
                # Get current configs from orchestrator if available
                configs = []
                import sys
                if 'orchestrator' in sys.modules:
                    from hunter.orchestrator import HunterOrchestrator
                    # Try to get current working configs
                    # This is a placeholder - you'll need to implement getting actual configs
                
                result = await self.fallback_sender.send_with_fallbacks(text, configs)
                if result.get("ok"):
                    self.logger.info(f"Message sent via {result.get('method')}")
                    return True
                else:
                    self.logger.warning(f"All fallback methods failed: {result.get('error')}")
            except Exception as e:
                self.logger.warning(f"Fallback sender failed: {e}")
        
        # Fallback to direct HTTP API
        result = await self._post_json("sendMessage", {
            "chat_id": target,
            "text": text,
            "parse_mode": parse_mode,
        })
        ok = result.get("ok", False)
        if ok:
            self.logger.info("Bot: message sent to Telegram")
        return ok

    async def send_file(self, filename: str, content: bytes,
                        caption: str = "", chat_id: Optional[str] = None) -> bool:
        if not self.enabled:
            return False
        target = chat_id or self.chat_id
        if not target:
            return False
        fields = {"chat_id": target}
        if caption:
            fields["caption"] = caption
        result = await self._post_multipart(
            "sendDocument", fields, "document", filename, content,
        )
        ok = result.get("ok", False)
        if ok:
            self.logger.info(f"Bot: file '{filename}' sent to Telegram")
        return ok

    # ---- high-level reporting (called by orchestrator) --------------------

    CONFIGS_PER_POST = 5  # each Telegram post = 5 URIs (tap to copy)

    async def report_gold_configs(self, configs: List[Dict[str, Any]], chat_id: Optional[str] = None):
        """Send summary + copyable config batches to Telegram.

        Format:
          1) Summary post with tier/latency info
          2) Multiple posts, each containing CONFIGS_PER_POST URIs inside
             a single code block so the user can tap-to-copy the whole block
        """
        if not configs:
            return

        gold = [c for c in configs if c.get("tier") == "gold"]
        silver = [c for c in configs if c.get("tier") != "gold"]

        # --- summary message ---
        import datetime
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        summary = f"🏆 *Hunter — {now}*\n\n"
        for i, cfg in enumerate(configs[:20], 1):
            tier = "🥇" if cfg.get("tier") == "gold" else "🥈"
            ps = cfg.get("ps", "—")
            lat = cfg.get("latency_ms", 0)
            summary += f"{tier} {ps}  `{lat:.0f}ms`\n"
        parts = []
        if gold:
            parts.append(f"🥇 {len(gold)} gold")
        if silver:
            parts.append(f"🥈 {len(silver)} silver")
        summary += f"\n{'  •  '.join(parts)}"
        await self.send_message(summary, chat_id=chat_id)

    async def report_config_files(
        self,
        gold_uris: List[str],
        gemini_uris: Optional[List[str]] = None,
        max_lines: int = 200,
        chat_id: Optional[str] = None,
    ) -> bool:
        """Send configs as files only (no text messages).

        Sends npvt.txt with all configs and optionally npvt_gemini.txt.
        """
        if not gold_uris:
            return False

        uris = gold_uris[:max_lines]

        # Try fallback sender first if available (just for connectivity, skip summary msg)
        if self.fallback_sender:
            try:
                configs = getattr(self.fallback_sender, 'current_configs', [])
                result = await self.fallback_sender.send_with_fallbacks(
                    f"📊 {len(uris)} configs ready", configs
                )
                if result.get("ok"):
                    self.logger.info(f"Fallback connectivity via {result.get('method')}")
            except Exception as e:
                self.logger.warning(f"Fallback sender error: {e}")

        ok = True

        # --- npvt file with ALL configs ---
        content = ("\n".join(uris) + "\n").encode("utf-8", errors="ignore")
        sent_main = await self.send_file(
            filename="npvt.txt",
            content=content,
            caption=f"📂 HUNTER npvt — {len(uris)} validated configs",
            chat_id=chat_id,
        )
        ok = ok and bool(sent_main)

        # --- gemini file if present ---
        if gemini_uris:
            g_content = ("\n".join(gemini_uris[:max_lines]) + "\n").encode("utf-8", errors="ignore")
            sent_g = await self.send_file(
                filename="npvt_gemini.txt",
                content=g_content,
                caption=f"♊ Gemini — {len(gemini_uris)} configs",
                chat_id=chat_id,
            )
            ok = ok and bool(sent_g)

        return ok

    async def report_status(self, status: Dict[str, Any], chat_id: Optional[str] = None):
        report = "📊 *Hunter Status*\n\n"
        report += f"Balancer: {'Running' if status.get('running') else 'Stopped'}\n"
        report += f"Backends: {status.get('backends', 0)}\n"
        report += f"Restarts: {status.get('stats', {}).get('restarts', 0)}\n"
        await self.send_message(report, chat_id=chat_id)


class TelegramReporter:
    """Handles reporting validated proxy configs to Telegram (via Telethon user client)."""

    def __init__(self, scraper: TelegramScraper):
        self.scraper = scraper
        self.logger = logging.getLogger(__name__)

    CONFIGS_PER_POST = 5

    async def report_gold_configs(self, configs: List[Dict[str, Any]], chat_id: Optional[Any] = None):
        """Report gold-tier configs to Telegram."""
        if not configs:
            return False

        try:
            import datetime
            now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
            gold = [c for c in configs if c.get("tier") == "gold"]
            silver = [c for c in configs if c.get("tier") != "gold"]
            summary = f"🏆 <b>Hunter — {now}</b>\n\n"
            for i, cfg in enumerate(configs[:20], 1):
                tier = "🥇" if cfg.get("tier") == "gold" else "🥈"
                ps = cfg.get("ps", "—")
                lat = cfg.get("latency_ms", 0)
                summary += f"{tier} {ps}  <code>{float(lat):.0f}ms</code>\n"
            parts = []
            if gold:
                parts.append(f"🥇 {len(gold)} gold")
            if silver:
                parts.append(f"🥈 {len(silver)} silver")
            if parts:
                summary += f"\n{'  •  '.join(parts)}"
            ok = await self.scraper.send_report(summary, parse_mode="html", chat_id=chat_id)
            return bool(ok)
        except Exception as e:
            self.logger.info(f"Failed to report gold configs to Telegram: {e}")
            return False

    async def report_config_files(
        self,
        gold_uris: List[str],
        gemini_uris: Optional[List[str]] = None,
        max_lines: int = 200,
        chat_id: Optional[Any] = None,
    ) -> bool:
        """Send configs as files only (no text messages)."""
        if not gold_uris and not gemini_uris:
            return False

        ok = True

        uris = (gold_uris or [])[:max_lines]
        if uris:
            content = ("\n".join(uris) + "\n").encode("utf-8", errors="ignore")
            sent_file = await self.scraper.send_file(
                filename="npvt.txt",
                content=content,
                caption=f"HUNTER npvt — {len(uris)} validated configs",
                chat_id=chat_id,
            )
            ok = ok and bool(sent_file)

        if gemini_uris:
            g_uris = gemini_uris[:max_lines]
            g_content = ("\n".join(g_uris) + "\n").encode("utf-8", errors="ignore")
            sent_g = await self.scraper.send_file(
                filename="npvt_gemini.txt",
                content=g_content,
                caption=f"Gemini — {len(g_uris)} configs",
                chat_id=chat_id,
            )
            ok = ok and bool(sent_g)

        return ok

    async def report_status(self, status: Dict[str, Any], chat_id: Optional[Any] = None):
        """Report system status to Telegram."""
        report = "📊 **Hunter Status Report**\n\n"
        report += f"Balancer: {'Running' if status.get('running') else 'Stopped'}\n"
        report += f"Backends: {status.get('backends', 0)}\n"
        report += f"Restarts: {status.get('stats', {}).get('restarts', 0)}\n"

        try:
            await self.scraper.send_report(report, chat_id=chat_id)
        except Exception as e:
            self.logger.debug(f"Failed to report status to Telegram: {e}")
