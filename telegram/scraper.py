"""
Telegram integration module.

This module handles Telegram client connection, scraping proxy configs
from channels, and reporting validated results back to Telegram.
"""

import asyncio
import logging
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

    DEFAULT_SSH_SERVERS = [
        {"host": "71.143.156.145", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "71.143.156.146", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "71.143.156.147", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "71.143.156.148", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "71.143.156.149", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "50.114.11.18", "port": 22, "username": "deployer", "password": "009100mohammad_mrb"},
    ]
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
        try:
            if self.client:
                await self.client.disconnect()
        except Exception:
            pass
        self.client = None

    def _delete_session_files(self) -> None:
        """Best-effort delete Telethon session files (requires re-login)."""
        try:
            session_name = str(self.config.get("session_name", "hunter_session") or "hunter_session")
        except Exception:
            session_name = "hunter_session"
        for suffix in (".session", ".session-journal"):
            path = session_name + suffix
            try:
                if os.path.exists(path):
                    os.remove(path)
            except Exception:
                pass

    async def connect(self, use_proxy_fallback: bool = True) -> bool:
        """Establish connection to Telegram.
        
        Args:
            use_proxy_fallback: If True, use V2Ray proxy tunnel if SSH fails
        """
        if not TELETHON_AVAILABLE:
            return False

        if self.client and await self.heartbeat.check_connection(self.client):
            return True

        try:
            proxy = None
            proxy_host = os.getenv("HUNTER_TELEGRAM_PROXY_HOST")
            proxy_port_raw = os.getenv("HUNTER_TELEGRAM_PROXY_PORT")
            proxy_user = os.getenv("HUNTER_TELEGRAM_PROXY_USER")
            proxy_pass = os.getenv("HUNTER_TELEGRAM_PROXY_PASS")

            if proxy_host and proxy_port_raw:
                try:
                    proxy_port = int(proxy_port_raw)
                    if proxy_user or proxy_pass:
                        proxy = (socks.SOCKS5, proxy_host, proxy_port, True, proxy_user or "", proxy_pass or "")
                    else:
                        proxy = (socks.SOCKS5, proxy_host, proxy_port)
                except Exception:
                    proxy = None

            # Fall back to SSH tunnel for proxy (local SOCKS5)
            if proxy is None:
                proxy_port = self.establish_ssh_tunnel()
                if proxy_port:
                    proxy = (socks.SOCKS5, '127.0.0.1', proxy_port)
                elif use_proxy_fallback:
                    # SSH failed, try V2Ray proxy fallback
                    self.logger.info("SSH tunnel failed, attempting V2Ray proxy fallback...")
                    proxy_port = await self._try_v2ray_proxy_fallback()
                    if proxy_port:
                        proxy = (socks.SOCKS5, '127.0.0.1', proxy_port)

            # Disable automatic reconnects - we manage reconnection manually
            # This prevents noisy reconnect loops and gives us control
            self.client = TelegramClient(
                self.config.get("session_name", "hunter_session"),
                self.config.get("api_id"),
                self.config.get("api_hash"),
                proxy=proxy,
                connection_retries=1,
                auto_reconnect=False,
            )

            await self.client.connect()

            if not await self.client.is_user_authorized():
                phone = self.config.get("phone")
                if not phone:
                    self.logger.info("Phone number not set - Telegram scraping disabled")
                    return False

                # Only attempt interactive auth if stdin is a TTY or web dashboard is active
                _web_active = False
                try:
                    from web.auth_bridge import AuthBridge
                    _web_active = AuthBridge().active
                except ImportError:
                    pass
                if not _web_active and (not sys.stdin or not sys.stdin.isatty()):
                    self.logger.info("Non-interactive mode - Telegram user auth skipped (use Bot API for reporting)")
                    return False

                # Use interactive authentication
                if self.telegram_auth:
                    if not await self.telegram_auth.authenticate(self.client, phone):
                        self.logger.info("Telegram authentication not completed")
                        return False
                else:
                    # Fallback to basic authentication
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

            if await self.client.is_user_authorized():
                self.logger.info("Connected to Telegram successfully")
                return True

        except SecurityError as e:
            msg = str(e)
            self.logger.warning(f"Telegram SecurityError: {msg}")
            await self._reset_client()
            self.close_ssh_tunnel()

            reset_ok = os.getenv("HUNTER_RESET_TELEGRAM_SESSION", "false").lower() == "true"
            if reset_ok and ("wrong session" in msg.lower() or "session id" in msg.lower()):
                self.logger.warning("Resetting Telegram session files due to SecurityError; you may need to re-login")
                self._delete_session_files()
            return False

        except Exception as e:
            self.logger.info(f"Telegram connection skipped: {e}")
            await self._reset_client()

        return False

    async def scrape_configs(self, channels: List[str], limit: int = 100) -> Set[str]:
        """Scrape proxy configurations from Telegram channels."""
        if not self.client or not await self.heartbeat.check_connection(self.client):
            if not await self.connect():
                return set()

        configs: Set[str] = set()
        consecutive_errors = 0
        max_consecutive_errors = 3
        last_flood_wait_seconds = None
        consecutive_flood_waits = 0

        for channel in channels:
            # Check connection before each channel (standard pattern from old_hunter)
            if consecutive_errors >= max_consecutive_errors:
                self.logger.warning(f"Too many consecutive errors ({consecutive_errors}), stopping scrape")
                break
            
            try:
                if not self.client.is_connected():
                    self.logger.warning(f"Connection lost before scraping {channel}")
                    reconnected = await self.heartbeat.try_reconnect(self.client)
                    if not reconnected:
                        break
            except Exception:
                pass
            
            try:
                entity = await self.client.get_entity(channel)

                fetch_limit = max(1, min(200, int(limit) * 4))

                messages = await self.client(GetHistoryRequest(
                    peer=entity,
                    limit=fetch_limit,
                    offset_date=None,
                    offset_id=0,
                    max_id=0,
                    min_id=0,
                    add_offset=0,
                    hash=0
                ))

                channel_configs: Set[str] = set()
                channel_configs_ordered: List[str] = []
                for message in messages.messages:
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
                            )
                            if is_text:
                                data = await self.client.download_media(message, file=bytes)
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

                    if len(channel_configs) >= limit:
                        break

                configs.update(channel_configs_ordered)

                self.logger.info(f"Scraped {len(channel_configs)} configs from {channel}")
                consecutive_errors = 0  # Reset on success

            except FloodWaitError as e:
                self.logger.warning(f"Flood wait on {channel}: {e.seconds}s - skipping channel")
                # Detect global account-level throttle: same wait time on multiple channels
                if last_flood_wait_seconds == e.seconds:
                    consecutive_flood_waits += 1
                else:
                    consecutive_flood_waits = 1
                    last_flood_wait_seconds = e.seconds
                if consecutive_flood_waits >= 3:
                    self.logger.warning(
                        f"Global account flood wait detected ({e.seconds}s on {consecutive_flood_waits} channels) - aborting scrape"
                    )
                    break
                if e.seconds > 300:  # > 5 minutes: skip entirely, not worth waiting
                    continue
                await asyncio.sleep(min(e.seconds, 60))  # Cap wait at 60s
            except (ChannelPrivateError, UsernameNotOccupiedError, UsernameInvalidError) as e:
                self.logger.warning(f"Skipping {channel}: {e}")
            except Exception as e:
                error_str = str(e).lower()
                is_connection_error = (
                    "disconnected" in error_str or
                    "connection" in error_str or
                    "security error" in error_str or
                    "cancelled" in error_str
                )
                
                if is_connection_error:
                    consecutive_errors += 1
                    self.logger.warning(f"Connection error for {channel}: {e}")
                    # Don't attempt reconnect if SSH tunnel is down - just stop scraping
                    if consecutive_errors >= 2:
                        self.logger.warning("Too many connection errors - stopping scrape session")
                        break
                    reconnected = await self.heartbeat.try_reconnect(self.client)
                    if reconnected:
                        consecutive_errors = 0
                else:
                    self.logger.warning(f"Failed to scrape {channel}: {e}")

        return configs

    async def send_report(self, report_text: str) -> bool:
        """Send report to Telegram channel."""
        if not self.client or not await self.heartbeat.check_connection(self.client):
            if not await self.connect():
                return False

        try:
            channel = self.config.get("report_channel")
            if not channel:
                return False

            await self.client.send_message(channel, report_text)
            self.logger.info("Report sent to Telegram")
            return True

        except Exception as e:
            self.logger.error(f"Failed to send report: {e}")
            return False

    async def send_file(self, filename: str, content: bytes, caption: str = "") -> bool:
        """Send a file to the Telegram report channel."""
        if not self.client or not await self.heartbeat.check_connection(self.client):
            if not await self.connect():
                return False

        try:
            channel = self.config.get("report_channel")
            if not channel:
                return False

            bio = io.BytesIO(content)
            bio.name = filename
            await self.client.send_file(channel, file=bio, caption=caption)
            self.logger.info(f"File sent to Telegram: {filename}")
            return True
        except Exception as e:
            self.logger.error(f"Failed to send file: {e}")
            return False

    async def disconnect(self):
        """Disconnect from Telegram and close SSH tunnel."""
        if self.client:
            await self.client.disconnect()
            self.client = None
        self.close_ssh_tunnel()


class BotReporter:
    """Send messages and files to Telegram via Bot API (HTTP).
    
    Uses TOKEN and CHAT_ID from environment - no Telethon, no SSH tunnel,
    no user session needed. Works directly over the internet.
    """

    BOT_API_URL = "https://api.telegram.org/bot{token}/{method}"

    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.token = os.getenv("TOKEN") or os.getenv("TELEGRAM_BOT_TOKEN")
        self.chat_id = os.getenv("CHAT_ID") or os.getenv("TELEGRAM_GROUP_ID")
        self.enabled = bool(self.token and self.chat_id)
        if not self.enabled:
            self.logger.info("Bot reporter disabled: TOKEN or CHAT_ID not set")

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
        loop = asyncio.get_event_loop()
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
        loop = asyncio.get_event_loop()
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

    async def send_message(self, text: str, parse_mode: str = "Markdown") -> bool:
        if not self.enabled:
            return False
        result = await self._post_json("sendMessage", {
            "chat_id": self.chat_id,
            "text": text,
            "parse_mode": parse_mode,
        })
        ok = result.get("ok", False)
        if ok:
            self.logger.info("Bot: message sent to Telegram")
        return ok

    async def send_file(self, filename: str, content: bytes,
                        caption: str = "") -> bool:
        if not self.enabled:
            return False
        fields = {"chat_id": self.chat_id}
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

    async def report_gold_configs(self, configs: List[Dict[str, Any]]):
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
        await self.send_message(summary)

    async def report_config_files(
        self,
        gold_uris: List[str],
        gemini_uris: Optional[List[str]] = None,
        max_lines: int = 200,
    ) -> None:
        """Send configs as copyable code-block posts + npvt file.

        Each post contains up to CONFIGS_PER_POST URIs in a <pre> block.
        User taps the block → all URIs in that post are copied at once.
        """
        if not gold_uris:
            return

        uris = gold_uris[:max_lines]
        batch = self.CONFIGS_PER_POST
        total_posts = (len(uris) + batch - 1) // batch

        for idx in range(0, len(uris), batch):
            chunk = uris[idx : idx + batch]
            post_num = idx // batch + 1
            block = "\n".join(chunk)
            # HTML <pre> so user can tap-to-copy the whole block
            msg = (
                f"📌 <b>{post_num}/{total_posts}</b>\n"
                f"<pre>{block}</pre>"
            )
            await self.send_message(msg, parse_mode="HTML")
            # small delay to keep order
            await asyncio.sleep(0.3)

        # --- npvt file with ALL configs ---
        content = ("\n".join(uris) + "\n").encode("utf-8", errors="ignore")
        await self.send_file(
            filename="npvt.txt",
            content=content,
            caption=f"📂 HUNTER npvt — {len(uris)} validated configs",
        )

        # --- gemini file if present ---
        if gemini_uris:
            g_content = ("\n".join(gemini_uris[:max_lines]) + "\n").encode("utf-8", errors="ignore")
            await self.send_file(
                filename="npvt_gemini.txt",
                content=g_content,
                caption=f"♊ Gemini — {len(gemini_uris)} configs",
            )

    async def report_status(self, status: Dict[str, Any]):
        report = "📊 *Hunter Status*\n\n"
        report += f"Balancer: {'Running' if status.get('running') else 'Stopped'}\n"
        report += f"Backends: {status.get('backends', 0)}\n"
        report += f"Restarts: {status.get('stats', {}).get('restarts', 0)}\n"
        await self.send_message(report)


class TelegramReporter:
    """Handles reporting validated proxy configs to Telegram (via Telethon user client)."""

    def __init__(self, scraper: TelegramScraper):
        self.scraper = scraper
        self.logger = logging.getLogger(__name__)

    async def report_gold_configs(self, configs: List[Dict[str, Any]]):
        """Report gold-tier configs to Telegram."""
        if not configs:
            return

        report = "🏆 **Hunter Gold Configs Report**\n\n"
        for i, config in enumerate(configs[:10], 1):  # Limit to top 10
            report += f"{i}. {config.get('ps', 'Unknown')} - {config.get('latency_ms', 0)}ms\n"

        report += f"\nTotal: {len(configs)} gold configs available"

        try:
            await self.scraper.send_report(report)
        except Exception as e:
            self.logger.debug(f"Failed to report gold configs to Telegram: {e}")

    async def report_config_files(
        self,
        gold_uris: List[str],
        gemini_uris: Optional[List[str]] = None,
        max_lines: int = 200,
    ) -> None:
        if not gold_uris and not gemini_uris:
            return

        try:
            if gold_uris:
                content = ("\n".join(gold_uris[:max_lines]) + "\n").encode("utf-8", errors="ignore")
                await self.scraper.send_file(
                    filename="HUNTER_gold.txt",
                    content=content,
                    caption=f"HUNTER Gold (top {min(len(gold_uris), max_lines)}/{len(gold_uris)})",
                )
        except Exception:
            pass

        try:
            if gemini_uris:
                content = ("\n".join(gemini_uris[:max_lines]) + "\n").encode("utf-8", errors="ignore")
                await self.scraper.send_file(
                    filename="HUNTER_gemini.txt",
                    content=content,
                    caption=f"HUNTER Gemini (top {min(len(gemini_uris), max_lines)}/{len(gemini_uris)})",
                )
        except Exception:
            pass

    async def report_status(self, status: Dict[str, Any]):
        """Report system status to Telegram."""
        report = "📊 **Hunter Status Report**\n\n"
        report += f"Balancer: {'Running' if status.get('running') else 'Stopped'}\n"
        report += f"Backends: {status.get('backends', 0)}\n"
        report += f"Restarts: {status.get('stats', {}).get('restarts', 0)}\n"

        try:
            await self.scraper.send_report(report)
        except Exception as e:
            self.logger.debug(f"Failed to report status to Telegram: {e}")
