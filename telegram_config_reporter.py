"""
Telegram Config Reporter - Sends validated configs to Telegram group.

Features:
- Sends 10 best configs as copyable text messages
- Sends 50 best configs as a single file for import
- Sorts by speed (latency)
- Formats for easy copying and importing
"""

import asyncio
import json
import logging
import os
from pathlib import Path
from typing import List, Optional, Dict, Any
from datetime import datetime

try:
    from telethon import TelegramClient
    from telethon.errors import SessionPasswordNeededError
    TELETHON_AVAILABLE = True
except ImportError:
    TELETHON_AVAILABLE = False


class TelegramConfigReporter:
    """Reports validated configs to Telegram group."""
    
    def __init__(self, api_id: Optional[int] = None, api_hash: Optional[str] = None, 
                 phone: Optional[str] = None, group_id: Optional[int] = None):
        self.logger = logging.getLogger(__name__)
        self.api_id = api_id or self._get_from_env("TELEGRAM_API_ID")
        self.api_hash = api_hash or self._get_from_env("TELEGRAM_API_HASH")
        self.phone = phone or self._get_from_env("TELEGRAM_PHONE")
        self.group_id = group_id or self._get_from_env("TELEGRAM_GROUP_ID")
        self.client = None
        
        if not all([self.api_id, self.api_hash, self.phone, self.group_id]):
            self.logger.info("Telegram credentials not set - reporting disabled")
    
    # Mapping from TELEGRAM_* keys to HUNTER_* aliases
    _ENV_ALIASES = {
        "TELEGRAM_API_ID": ["HUNTER_API_ID"],
        "TELEGRAM_API_HASH": ["HUNTER_API_HASH"],
        "TELEGRAM_PHONE": ["HUNTER_PHONE"],
        "TELEGRAM_GROUP_ID": ["CHAT_ID"],
    }
    
    def _get_from_env(self, key: str) -> Optional[str]:
        """Get value from environment or secrets file."""
        # Try environment variable first (exact key)
        value = os.getenv(key)
        if value:
            return value
        
        # Try aliases (e.g. TELEGRAM_API_ID -> HUNTER_API_ID)
        for alias in self._ENV_ALIASES.get(key, []):
            value = os.getenv(alias)
            if value:
                return value
        
        # Try secrets files
        for filename in ("hunter_secrets.env", ".env"):
            secrets_path = Path(__file__).parent / filename
            if secrets_path.exists():
                try:
                    search_keys = [key] + self._ENV_ALIASES.get(key, [])
                    with open(secrets_path, 'r', encoding='utf-8') as f:
                        for line in f:
                            line = line.strip()
                            for sk in search_keys:
                                if line.startswith(sk + "="):
                                    return line.split("=", 1)[1].strip().strip('"\'')
                except Exception as e:
                    self.logger.debug(f"Failed to read {filename}: {e}")
        
        return None
    
    async def connect(self) -> bool:
        """Connect to Telegram."""
        if not TELETHON_AVAILABLE:
            self.logger.error("Telethon not installed")
            return False
        
        if not all([self.api_id, self.api_hash, self.phone]):
            self.logger.info("Telegram credentials not set - skipping")
            return False
        
        try:
            self.client = TelegramClient(
                "hunter_config_reporter",
                self.api_id,
                self.api_hash
            )
            
            await self.client.connect()
            
            if not await self.client.is_user_authorized():
                await self.client.send_code_request(self.phone)
                code = input("Enter Telegram code: ").strip()
                try:
                    await self.client.sign_in(phone=self.phone, code=code)
                except SessionPasswordNeededError:
                    pwd = input("Enter 2FA password: ").strip()
                    await self.client.sign_in(password=pwd)
            
            self.logger.info("Connected to Telegram")
            return True
        
        except Exception as e:
            self.logger.error(f"Telegram connection failed: {e}")
            return False
    
    async def disconnect(self):
        """Disconnect from Telegram."""
        if self.client:
            await self.client.disconnect()
            self.client = None
    
    async def send_configs(self, validated_configs: List[Dict[str, Any]], 
                          max_text_configs: int = 10, max_file_configs: int = 50):
        """Send validated configs to Telegram group.
        
        Args:
            validated_configs: List of validated config dicts with 'uri' and 'latency_ms'
            max_text_configs: Number of configs to send as text (default 10)
            max_file_configs: Number of configs to send as file (default 50)
        """
        if not self.client:
            self.logger.error("Not connected to Telegram")
            return False
        
        if not self.group_id:
            self.logger.error("No group ID configured")
            return False
        
        try:
            # Sort by latency (speed)
            sorted_configs = sorted(
                validated_configs,
                key=lambda x: x.get('latency_ms', float('inf'))
            )
            
            # Send text configs (top 10)
            text_configs = sorted_configs[:max_text_configs]
            if text_configs:
                await self._send_text_configs(text_configs)
            
            # Send file configs (top 50)
            file_configs = sorted_configs[:max_file_configs]
            if file_configs:
                await self._send_file_configs(file_configs)
            
            return True
        
        except Exception as e:
            self.logger.error(f"Failed to send configs: {e}")
            return False
    
    async def _send_text_configs(self, configs: List[Dict[str, Any]]):
        """Send top configs as copyable text messages."""
        self.logger.info(f"Sending {len(configs)} configs as text messages...")
        
        # Create message for each config
        for i, config in enumerate(configs, 1):
            uri = config.get('uri', '')
            latency = config.get('latency_ms', 0)
            ps = config.get('ps', 'Unknown')
            
            # Format message
            message = (
                f"🚀 **Config #{i}**\n"
                f"Name: {ps}\n"
                f"Speed: {latency:.0f}ms\n"
                f"\n"
                f"`{uri}`\n"
                f"\n"
                f"_Tap to copy the config above_"
            )
            
            try:
                await self.client.send_message(self.group_id, message)
                self.logger.info(f"Sent config #{i}: {ps} ({latency:.0f}ms)")
                await asyncio.sleep(0.5)  # Rate limiting
            except Exception as e:
                self.logger.warning(f"Failed to send config #{i}: {e}")
    
    async def _send_file_configs(self, configs: List[Dict[str, Any]]):
        """Send configs as a file for import."""
        self.logger.info(f"Creating file with {len(configs)} configs...")
        
        try:
            # Create file content
            file_content = self._create_config_file(configs)
            
            # Save to temporary file
            temp_file = Path("/tmp/hunter_configs.txt")
            if not temp_file.parent.exists():
                temp_file = Path.home() / "hunter_configs.txt"
            
            with open(temp_file, 'w', encoding='utf-8') as f:
                f.write(file_content)
            
            self.logger.info(f"Config file created: {temp_file}")
            
            # Send file
            caption = (
                f"📦 **Hunter Validated Configs**\n"
                f"Total: {len(configs)} configs\n"
                f"Speed: {configs[0].get('latency_ms', 0):.0f}ms - {configs[-1].get('latency_ms', 0):.0f}ms\n"
                f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n"
                f"_Import these configs into your V2Ray client_"
            )
            
            await self.client.send_file(
                self.group_id,
                temp_file,
                caption=caption
            )
            
            self.logger.info("Config file sent successfully")
            
            # Cleanup
            try:
                temp_file.unlink()
            except Exception:
                pass
            
            return True
        
        except Exception as e:
            self.logger.error(f"Failed to send config file: {e}")
            return False
    
    def _create_config_file(self, configs: List[Dict[str, Any]]) -> str:
        """Create config file content."""
        lines = []
        
        # Header
        lines.append("# Hunter Validated Configs")
        lines.append(f"# Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"# Total: {len(configs)} configs")
        lines.append("# Sort: By speed (latency)")
        lines.append("")
        
        # Configs
        for i, config in enumerate(configs, 1):
            uri = config.get('uri', '')
            latency = config.get('latency_ms', 0)
            ps = config.get('ps', 'Unknown')
            
            lines.append(f"# [{i}] {ps} - {latency:.0f}ms")
            lines.append(uri)
            lines.append("")
        
        return "\n".join(lines)


class ConfigReportingService:
    """Service for reporting configs to Telegram."""
    
    def __init__(self, orchestrator):
        self.logger = logging.getLogger(__name__)
        self.orchestrator = orchestrator
        self.reporter = TelegramConfigReporter()
    
    async def report_validated_configs(self, validated_results: List[Any]):
        """Report validated configs to Telegram group.
        
        Args:
            validated_results: List of HunterBenchResult objects
        """
        if not validated_results:
            self.logger.warning("No validated configs to report")
            return False
        
        try:
            # Convert results to dict format
            configs = []
            for result in validated_results:
                config_dict = {
                    'uri': result.uri,
                    'latency_ms': result.latency_ms,
                    'ps': result.ps or result.host,
                    'host': result.host,
                    'port': result.port,
                    'country': result.country_code or 'Unknown',
                    'tier': result.tier
                }
                configs.append(config_dict)
            
            # Connect to Telegram
            connected = await self.reporter.connect()
            if not connected:
                self.logger.error("Failed to connect to Telegram")
                return False
            
            # Send configs
            success = await self.reporter.send_configs(
                configs,
                max_text_configs=10,
                max_file_configs=50
            )
            
            # Disconnect
            await self.reporter.disconnect()
            
            if success:
                self.logger.info(f"Successfully reported {len(configs)} configs to Telegram")
            
            return success
        
        except Exception as e:
            self.logger.error(f"Config reporting error: {e}")
            return False


async def main():
    """Example usage."""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s | %(levelname)-8s | %(message)s'
    )
    
    # Create sample configs
    sample_configs = [
        {
            'uri': 'vmess://base64encodedconfig1',
            'latency_ms': 45.5,
            'ps': 'Fast Server 1',
            'host': '1.2.3.4',
            'port': 8080,
            'country': 'US',
            'tier': 'gold'
        },
        {
            'uri': 'vless://base64encodedconfig2',
            'latency_ms': 52.3,
            'ps': 'Fast Server 2',
            'host': '2.3.4.5',
            'port': 443,
            'country': 'DE',
            'tier': 'gold'
        },
        {
            'uri': 'trojan://base64encodedconfig3',
            'latency_ms': 78.9,
            'ps': 'Medium Server',
            'host': '3.4.5.6',
            'port': 443,
            'country': 'FR',
            'tier': 'silver'
        },
    ]
    
    reporter = TelegramConfigReporter()
    
    # Connect
    connected = await reporter.connect()
    if not connected:
        print("Failed to connect to Telegram")
        return
    
    # Send configs
    success = await reporter.send_configs(sample_configs)
    
    # Disconnect
    await reporter.disconnect()
    
    print(f"Reporting result: {success}")


if __name__ == "__main__":
    asyncio.run(main())
