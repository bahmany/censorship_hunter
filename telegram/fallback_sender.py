"""
Telegram sender with multiple fallback mechanisms:
1. Through found v2ray configs
2. Through SSH servers defined in project
3. Through local ports 1080/11808
4. Direct connection
5. Report failure if all fail
"""

import os
import sys
import asyncio
import logging
import socket
import time
from typing import List, Dict, Any, Optional, Tuple
from urllib.request import urlopen, Request
from urllib.error import URLError, HTTPError
import json
import subprocess
import tempfile

try:
    from hunter.proxy.parser import ConfigParser
except ImportError:
    try:
        from proxy.parser import ConfigParser
    except ImportError:
        ConfigParser = None

class TelegramFallbackSender:
    """Telegram sender with multiple fallback mechanisms."""
    
    def __init__(self):
        self.logger = logging.getLogger(__name__)
        self.token = os.getenv("TOKEN") or os.getenv("TELEGRAM_BOT_TOKEN")
        self.chat_id = os.getenv("CHAT_ID") or os.getenv("TELEGRAM_GROUP_ID")
        self.parser = ConfigParser() if ConfigParser else None
        
        # SSH servers from project
        self.ssh_servers = []
        try:
            from .ssh_servers import SSH_SERVERS
            self.ssh_servers = SSH_SERVERS
        except ImportError:
            pass
        
        # Also try to load from environment
        import json
        env_ssh = os.getenv("HUNTER_SSH_SERVERS")
        if env_ssh:
            try:
                self.ssh_servers.extend(json.loads(env_ssh))
            except Exception as e:
                self.logger.warning(f"Failed to parse HUNTER_SSH_SERVERS: {e}")
        
    async def send_with_fallbacks(self, message: str, configs: Optional[List[str]] = None) -> Dict[str, Any]:
        """Send message using fallback mechanisms in priority order."""
        
        attempts = []
        
        # 1. Try through found v2ray configs
        if configs:
            result = await self._send_via_v2ray_configs(message, configs)
            attempts.append(("V2Ray Configs", result))
            if result.get("success"):
                return {"ok": True, "method": "V2Ray Configs", "attempts": attempts}
        
        # 2. Try through SSH servers
        if self.ssh_servers:
            result = await self._send_via_ssh_servers(message)
            attempts.append(("SSH Servers", result))
            if result.get("success"):
                return {"ok": True, "method": "SSH Servers", "attempts": attempts}
        
        # 3. Try through local ports
        for port in [1080, 11808]:
            result = await self._send_via_local_port(message, port)
            attempts.append((f"Local Port {port}", result))
            if result.get("success"):
                return {"ok": True, "method": f"Local Port {port}", "attempts": attempts}
        
        # 4. Try direct connection
        result = await self._send_direct(message)
        attempts.append(("Direct", result))
        if result.get("success"):
            return {"ok": True, "method": "Direct", "attempts": attempts}
        
        # 5. All failed
        return {
            "ok": False, 
            "error": "All fallback methods failed",
            "attempts": attempts
        }
    
    async def _send_via_v2ray_configs(self, message: str, configs: List[str]) -> Dict[str, Any]:
        """Send message using v2ray configs as proxy."""
        
        if not self.parser:
            return {"success": False, "error": "ConfigParser not available"}
        
        for config_uri in configs[:5]:  # Try first 5 configs
            try:
                # Parse config
                parsed = self.parser.parse(config_uri)
                if not parsed:
                    continue
                
                # Create temporary XRay config
                port = 10808 + (len(config_uri) % 100)  # Use different port for each attempt
                xray_config = {
                    "log": {"loglevel": "none"},
                    "inbounds": [{
                        "port": port,
                        "listen": "127.0.0.1",
                        "protocol": "socks",
                        "settings": {"auth": "noauth", "udp": False}
                    }],
                    "outbounds": [parsed.outbound]
                }
                
                # Start XRay
                success = await self._send_via_xray_proxy(message, xray_config, port)
                if success:
                    return {"success": True, "config": config_uri[:50] + "..."}
                    
            except Exception as e:
                self.logger.debug(f"Failed to use config {config_uri[:30]}...: {e}")
                continue
        
        return {"success": False, "error": "No v2ray config worked"}
    
    async def _send_via_ssh_servers(self, message: str) -> Dict[str, Any]:
        """Send message using SSH servers as proxy."""
        
        for ssh_server in self.ssh_servers:
            try:
                # Create SSH tunnel to Telegram API
                proxy_port = 9080 + (hash(ssh_server["host"]) % 100)
                
                # Build SSH command
                ssh_cmd = [
                    "ssh", "-N", "-L", f"{proxy_port}:api.telegram.org:443",
                    f"{ssh_server['user']}@{ssh_server['host']}",
                    "-p", str(ssh_server.get("port", 22)),
                    "-o", "ConnectTimeout=10",
                    "-o", "BatchMode=yes"
                ]
                
                # Start SSH tunnel
                process = subprocess.Popen(
                    ssh_cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
                )
                
                # Wait for tunnel to establish
                await asyncio.sleep(2)
                
                if process.poll() is not None:
                    continue  # SSH failed to start
                
                try:
                    # Send through SSH tunnel
                    result = await self._send_http_request(message, proxy_port)
                    if result:
                        return {"success": True, "server": ssh_server["host"]}
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        
            except Exception as e:
                self.logger.debug(f"SSH server {ssh_server['host']} failed: {e}")
                continue
        
        return {"success": False, "error": "No SSH server worked"}
    
    async def _send_via_local_port(self, message: str, port: int) -> Dict[str, Any]:
        """Send message using local SOCKS proxy on specified port."""
        
        try:
            # Check if port is open
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2)
            result = sock.connect_ex(("127.0.0.1", port))
            sock.close()
            
            if result != 0:
                return {"success": False, "error": f"Port {port} not open"}
            
            # Send through SOCKS proxy
            import socks
            socks.set_default_proxy(socks.SOCKS5, "127.0.0.1", port)
            socket.socket = socks.socksocket
            
            try:
                result = await self._send_http_request(message)
                return {"success": True, "port": port}
            finally:
                # Reset socket
                socket.socket = socket._socketobject
                
        except Exception as e:
            return {"success": False, "error": str(e)}
    
    async def _send_direct(self, message: str) -> Dict[str, Any]:
        """Send message directly without proxy."""
        
        try:
            result = await self._send_http_request(message)
            return {"success": True, "method": "Direct"}
        except Exception as e:
            return {"success": False, "error": str(e)}
    
    async def _send_via_xray_proxy(self, message: str, xray_config: Dict, port: int) -> bool:
        """Send message using XRay as proxy."""
        
        xray_path = os.path.join(os.path.dirname(__file__), "..", "bin", "xray.exe")
        if not os.path.exists(xray_path):
            xray_path = "xray"  # Assume in PATH
        
        # Write config to temp file
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', delete=False) as f:
            json.dump(xray_config, f)
            config_file = f.name
        
        process = None
        try:
            # Start XRay
            process = subprocess.Popen(
                [xray_path, "run", "-c", config_file],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            )
            
            # Wait for XRay to start
            await asyncio.sleep(2)
            
            if process.poll() is not None:
                return False
            
            # Send through XRay proxy
            import socks
            socks.set_default_proxy(socks.SOCKS5, "127.0.0.1", port)
            socket.socket = socks.socksocket
            
            try:
                result = await self._send_http_request(message)
                return result
            finally:
                # Reset socket
                socket.socket = socket._socketobject
                
        except Exception as e:
            self.logger.debug(f"XRay proxy failed: {e}")
            return False
        finally:
            if process:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
            os.unlink(config_file)
    
    async def _send_http_request(self, message: str, proxy_port: Optional[int] = None) -> bool:
        """Send HTTP request to Telegram API."""
        
        url = f"https://api.telegram.org/bot{self.token}/sendMessage"
        data = {
            "chat_id": self.chat_id,
            "text": message,
            "parse_mode": "Markdown"
        }
        
        try:
            body = json.dumps(data).encode('utf-8')
            req = Request(url, data=body, headers={'Content-Type': 'application/json'}, method='POST')
            
            if proxy_port:
                # Use proxy
                import urllib.request
                proxy_handler = urllib.request.ProxyHandler({
                    'https': f'http://127.0.0.1:{proxy_port}',
                    'http': f'http://127.0.0.1:{proxy_port}'
                })
                opener = urllib.request.build_opener(proxy_handler)
                resp = opener.open(req, timeout=30)
            else:
                # Direct request
                resp = urlopen(req, timeout=30)
            
            result = json.loads(resp.read().decode())
            return result.get('ok', False)
            
        except Exception as e:
            self.logger.debug(f"HTTP request failed: {e}")
            return False


# Test the fallback sender
async def test_fallback_sender():
    """Test the fallback sender."""
    logging.basicConfig(level=logging.INFO)
    
    sender = TelegramFallbackSender()
    
    # Test message
    message = f"🧪 Test from Fallback Sender\\n⏰ Time: {time.time()}"
    
    # Get some configs (you can pass actual configs here)
    configs = []  # Add your v2ray configs here
    
    result = await sender.send_with_fallbacks(message, configs)
    
    print(f"Result: {result}")
    return result.get("ok", False)


if __name__ == "__main__":
    success = asyncio.run(test_fallback_sender())
    sys.exit(0 if success else 1)
