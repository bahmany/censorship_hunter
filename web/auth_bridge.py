"""Thread-safe bridge for Telegram authentication via web dashboard.

When the web dashboard is active, Telegram auth requests (verification code, 2FA)
are routed through this bridge instead of console input(). The web dashboard
shows a form, the user submits the code, and it's passed back to the auth module.
"""

import threading
import time
from typing import Optional


class AuthBridge:
    """Singleton bridge between Telegram auth and web dashboard."""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._event = threading.Event()
        self._response: Optional[str] = None
        self._pending: Optional[dict] = None
        self._result_lock = threading.Lock()
        self.active = False  # Set to True when web dashboard is running

    def request_code(self, phone: str, timeout: float = 300) -> Optional[str]:
        """Request verification code from web user. Blocks until received or timeout."""
        self._event.clear()
        self._response = None
        self._pending = {
            'type': 'code',
            'phone': phone,
            'message': 'Enter the 5-digit Telegram verification code',
            'timestamp': time.time()
        }

        self._event.wait(timeout=timeout)

        with self._result_lock:
            response = self._response
            self._response = None
            self._pending = None

        return response

    def request_2fa(self, timeout: float = 300) -> Optional[str]:
        """Request 2FA password from web user. Blocks until received or timeout."""
        self._event.clear()
        self._response = None
        self._pending = {
            'type': '2fa',
            'message': 'Enter your Telegram 2FA password',
            'timestamp': time.time()
        }

        self._event.wait(timeout=timeout)

        with self._result_lock:
            response = self._response
            self._response = None
            self._pending = None

        return response

    def submit_response(self, value: str) -> bool:
        """Submit auth response from web dashboard."""
        if not self._pending:
            return False
        with self._result_lock:
            self._response = value
        self._event.set()
        return True

    def get_pending(self) -> Optional[dict]:
        """Get current pending auth request (for web dashboard to poll)."""
        return self._pending

    def cancel(self):
        """Cancel pending auth request."""
        self._pending = None
        self._response = None
        self._event.set()
