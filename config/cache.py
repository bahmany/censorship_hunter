"""
Configuration cache helpers used to persist harvested proxies.
"""
from typing import Dict, List, Optional, Set

from hunter.core.utils import append_unique_lines, load_json, read_lines, save_json, write_lines
from hunter.core.utils import now_ts


class SmartCache:
    """Smart caching with failure tracking."""

    def __init__(self, cache_file: str = "subscriptions_cache.txt", working_cache_file: str = "working_configs_cache.txt") -> None:
        self.cache_file = cache_file
        self.working_cache_file = working_cache_file
        self._last_successful_fetch = 0
        self._consecutive_failures = 0

    def save_configs(self, configs: List[str], working: bool = False) -> int:
        target = self.working_cache_file if working else self.cache_file
        appended = append_unique_lines(target, configs)
        if appended > 0:
            self._last_successful_fetch = now_ts()
            self._consecutive_failures = 0
        return appended

    def load_cached_configs(self, max_count: int = 1000, working_only: bool = False) -> Set[str]:
        configs = set()
        sources = [self.cache_file]
        if working_only:
            sources = [self.working_cache_file]
        else:
            sources.append(self.working_cache_file)
        for path in sources:
            lines = read_lines(path)
            for line in lines[-max_count:]:
                if line and "://" in line:
                    configs.add(line)
        return configs

    def record_failure(self) -> None:
        self._consecutive_failures += 1

    def should_use_cache(self) -> bool:
        return self._consecutive_failures >= 2

    def get_failure_count(self) -> int:
        return self._consecutive_failures


class ResilientHeartbeat:
    """Heartbeat monitor for Telegram clients."""

    def __init__(self, check_interval: int = 30) -> None:
        self.check_interval = check_interval
        self._last_heartbeat = now_ts()
        self._is_connected = False
        self._reconnect_attempts = 0
        self._max_reconnect_attempts = 5

    async def check_connection(self, client: Optional["TelegramClient"]) -> bool:
        if client is None:
            self._is_connected = False
            return False
        try:
            if not client.is_connected():
                self._is_connected = False
                return False
            await client.get_me()
            self._is_connected = True
            self._last_heartbeat = now_ts()
            self._reconnect_attempts = 0
            return True
        except Exception:
            self._is_connected = False
            return False

    async def try_reconnect(self, client: Optional["TelegramClient"]) -> bool:
        if self._reconnect_attempts >= self._max_reconnect_attempts:
            return False
        self._reconnect_attempts += 1
        try:
            if client:
                await client.disconnect()
                await client.connect()
                if await client.is_user_authorized():
                    self._is_connected = True
                    return True
        except Exception:
            return False
        return False

    def is_connected(self) -> bool:
        return self._is_connected

    def time_since_heartbeat(self) -> int:
        return now_ts() - self._last_heartbeat
