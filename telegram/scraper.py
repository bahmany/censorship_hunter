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
    from telethon.errors import FloodWaitError, SessionPasswordNeededError, ChannelPrivateError, UsernameNotOccupiedError, UsernameInvalidError
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
import json

from hunter.core.config import HunterConfig
from hunter.core.utils import extract_raw_uris_from_text
from hunter.config.cache import ResilientHeartbeat


class TelegramScraper:
    """Handles Telegram scraping for proxy configurations."""

    DEFAULT_SSH_SERVERS = [
        {"host": "71.143.156.145", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
        {"host": "50.114.11.18", "port": 22, "username": "deployer", "password": "009100mohammad_mrb"},
    ]
    DEFAULT_SSH_SOCKS_HOST = "127.0.0.1"
    DEFAULT_SSH_SOCKS_PORT = 1088

    @classmethod
    def _load_ssh_servers(cls) -> List[Dict[str, Any]]:
        """Load SSH server definitions from environment or fall back to defaults."""
        raw_value = os.getenv("SSH_SERVERS")
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
        self.ssh_servers = self._load_ssh_servers()
        self.ssh_socks_host = os.getenv("SSH_SOCKS_HOST", self.DEFAULT_SSH_SOCKS_HOST)
        self.ssh_socks_port = int(os.getenv("SSH_SOCKS_PORT", str(self.DEFAULT_SSH_SOCKS_PORT)))

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
            self.logger.error("No SSH servers configured for tunneling")
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

        for server in self.ssh_servers:
            try:
                self.logger.info(f"Attempting SSH tunnel to {server['host']}:{server['port']}")
                ssh = paramiko.SSHClient()
                ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                ssh.connect(
                    hostname=server['host'],
                    port=server['port'],
                    username=server.get('username') or server.get('user') or server.get('login') or server.get('uname') or "",
                    password=server.get('password') or server.get('pass') or "",
                    timeout=10
                )

                transport = ssh.get_transport()
                if transport is None:
                    raise RuntimeError("SSH transport is not available")

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
                self.logger.info(f"SSH tunnel established on local port {local_port}")
                return local_port
                
            except Exception as e:
                self.logger.warning(f"Failed to establish SSH tunnel to {server['host']}: {e}")
                continue
        
        self.logger.error("Failed to establish SSH tunnel to any server")
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

    async def connect(self) -> bool:
        """Establish connection to Telegram."""
        if not TELETHON_AVAILABLE:
            return False

        if self.client and await self.heartbeat.check_connection(self.client):
            return True

        try:
            # Establish SSH tunnel for proxy
            proxy_port = self.establish_ssh_tunnel()
            proxy = None
            if proxy_port:
                proxy = (socks.SOCKS5, '127.0.0.1', proxy_port)

            self.client = TelegramClient(
                self.config.get("session_name", "hunter_session"),
                self.config.get("api_id"),
                self.config.get("api_hash"),
                proxy=proxy
            )

            await self.client.connect()

            if not await self.client.is_user_authorized():
                phone = self.config.get("phone")
                if not phone:
                    self.logger.error("Phone number required for Telegram auth")
                    return False

                await self.client.send_code_request(phone)
                # In real usage, this would prompt for code
                # For now, assume it's handled externally

            if await self.client.is_user_authorized():
                self.logger.info("Connected to Telegram successfully")
                return True

        except Exception as e:
            self.logger.error(f"Failed to connect to Telegram: {e}")

        return False

    async def scrape_configs(self, channels: List[str], limit: int = 100) -> Set[str]:
        """Scrape proxy configurations from Telegram channels."""
        if not self.client or not await self.heartbeat.check_connection(self.client):
            if not await self.connect():
                return set()

        configs = set()

        for channel in channels:
            try:
                entity = await self.client.get_entity(channel)

                messages = await self.client(GetHistoryRequest(
                    peer=entity,
                    limit=limit,
                    offset_date=None,
                    offset_id=0,
                    max_id=0,
                    min_id=0,
                    add_offset=0,
                    hash=0
                ))

                for message in messages.messages:
                    if hasattr(message, 'message') and message.message:
                        found = extract_raw_uris_from_text(message.message)
                        configs.update(found)

                self.logger.info(f"Scraped {len(configs)} configs from {channel}")

            except FloodWaitError as e:
                self.logger.warning(f"Flood wait on {channel}: {e.seconds}s")
                await asyncio.sleep(e.seconds)
            except (ChannelPrivateError, UsernameNotOccupiedError, UsernameInvalidError) as e:
                self.logger.warning(f"Skipping {channel}: {e}")
            except Exception as e:
                self.logger.error(f"Failed to scrape {channel}: {e}")

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

    async def disconnect(self):
        """Disconnect from Telegram and close SSH tunnel."""
        if self.client:
            await self.client.disconnect()
            self.client = None
        self.close_ssh_tunnel()


class TelegramReporter:
    """Handles reporting validated proxy configs to Telegram."""

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

        await self.scraper.send_report(report)

    async def report_status(self, status: Dict[str, Any]):
        """Report system status to Telegram."""
        report = "📊 **Hunter Status Report**\n\n"
        report += f"Balancer: {'Running' if status.get('running') else 'Stopped'}\n"
        report += f"Backends: {status.get('backends', 0)}\n"
        report += f"Restarts: {status.get('stats', {}).get('restarts', 0)}\n"

        await self.scraper.send_report(report)
