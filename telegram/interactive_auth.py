"""
Interactive Telegram Authentication Module

Provides enhanced interactive authentication for Telegram with better user experience.
Supports both console input and environment variable fallback.
"""

import os
import sys
import logging
from typing import Optional
from telethon.errors import SessionPasswordNeededError


class InteractiveTelegramAuth:
    """Interactive Telegram authentication with enhanced user experience."""
    
    def __init__(self, logger=None):
        self.logger = logger or logging.getLogger(__name__)

    def _get_web_bridge(self):
        """Get web auth bridge if active (checked dynamically each call)."""
        try:
            from web.auth_bridge import AuthBridge
            bridge = AuthBridge()
            if bridge.active:
                return bridge
        except ImportError:
            pass
        return None
    
    def get_user_input(self, prompt: str, mask: bool = False) -> str:
        """Get user input with optional masking for passwords."""
        try:
            # Try to get input from environment variable first
            env_var_name = prompt.upper().replace(" ", "_").replace(":", "").replace("(", "").replace(")", "")
            env_value = os.getenv(env_var_name)
            
            if env_value:
                self.logger.info(f"Using {env_var_name} from environment")
                return env_value
            
            # Console input with masking for passwords
            if mask and sys.stdin.isatty():
                import getpass
                return getpass.getpass(prompt)
            else:
                return input(prompt)
        
        except (EOFError, KeyboardInterrupt):
            self.logger.error("Input cancelled by user")
            return ""
    
    def get_telegram_code(self, phone: str) -> Optional[str]:
        """Get Telegram verification code from user (web or console)."""
        # Route through web dashboard if active
        web_bridge = self._get_web_bridge()
        if web_bridge:
            self.logger.info("Waiting for verification code via web dashboard...")
            code = web_bridge.request_code(phone, timeout=300)
            if code and code.isdigit() and len(code) == 5:
                return code
            self.logger.warning("No valid code received from web dashboard")
            return None

        print("\n" + "=" * 60)
        print("TELEGRAM AUTHENTICATION")
        print("=" * 60)
        print(f"Phone: {phone}")
        print("\nA verification code has been sent to your Telegram account.")
        print("Please check your Telegram app and enter the code below.")
        print("\nOptions:")
        print("  - Enter the 5-digit code from Telegram")
        print("  - Press Ctrl+C to cancel")
        print("=" * 60)
        
        max_attempts = 3
        for attempt in range(max_attempts):
            try:
                prompt = f"Enter Telegram code (attempt {attempt + 1}/{max_attempts}): "
                code = self.get_user_input(prompt)
                
                if not code:
                    if attempt < max_attempts - 1:
                        print("No code entered. Please try again.")
                        continue
                    else:
                        print("No code entered. Authentication cancelled.")
                        return None
                
                # Validate code format (5 digits)
                if code.isdigit() and len(code) == 5:
                    return code
                else:
                    print("Invalid code format. Please enter the 5-digit code from Telegram.")
                    if attempt < max_attempts - 1:
                        continue
                    else:
                        print("Too many invalid attempts. Authentication cancelled.")
                        return None
            
            except (EOFError, KeyboardInterrupt):
                print("\nAuthentication cancelled by user.")
                return None
        
        return None
    
    def get_2fa_password(self) -> Optional[str]:
        """Get 2FA password from user (web or console)."""
        # Route through web dashboard if active
        web_bridge = self._get_web_bridge()
        if web_bridge:
            self.logger.info("Waiting for 2FA password via web dashboard...")
            password = web_bridge.request_2fa(timeout=300)
            if password:
                return password
            self.logger.warning("No 2FA password received from web dashboard")
            return None

        print("\n" + "=" * 60)
        print("TWO-FACTOR AUTHENTICATION")
        print("=" * 60)
        print("Your account has 2-factor authentication enabled.")
        print("Please enter your 2FA password.")
        print("\nOptions:")
        print("  - Enter your 2FA password")
        print("  - Press Ctrl+C to cancel")
        print("=" * 60)
        
        try:
            password = self.get_user_input("Enter 2FA password: ", mask=True)
            
            if not password:
                print("No password entered. Authentication cancelled.")
                return None
            
            return password
        
        except (EOFError, KeyboardInterrupt):
            print("\nAuthentication cancelled by user.")
            return None
    
    async def authenticate(self, client, phone: str) -> bool:
        """Perform interactive Telegram authentication."""
        try:
            # Send code request
            print(f"\nSending verification code to {phone}...")
            await client.send_code_request(phone)
            
            # Get verification code
            code = self.get_telegram_code(phone)
            if not code:
                return False
            
            # Try to sign in with code
            try:
                await client.sign_in(phone=phone, code=code)
                self.logger.info("Successfully authenticated with verification code")
                return True
            
            except SessionPasswordNeededError:
                # Need 2FA password
                password = self.get_2fa_password()
                if not password:
                    return False
                
                await client.sign_in(password=password)
                self.logger.info("Successfully authenticated with 2FA password")
                return True
            
        except Exception as e:
            self.logger.error(f"Authentication failed: {e}")
            return False
    
    def show_connection_info(self, phone: str, proxy_info: Optional[str] = None):
        """Show connection information to user."""
        print("\n" + "=" * 60)
        print("TELEGRAM CONNECTION INFO")
        print("=" * 60)
        print(f"Phone Number: {phone}")
        
        if proxy_info:
            print(f"Proxy: {proxy_info}")
        
        print("\nAuthentication Methods:")
        print("  1. Verification code (5-digit)")
        print("  2. 2FA password (if enabled)")
        print("\nNote: Make sure you have access to your Telegram app")
        print("      to receive the verification code.")
        print("=" * 60)


class EnvironmentAuth:
    """Environment-based authentication for automated deployment."""
    
    def __init__(self, logger=None):
        self.logger = logger or logging.getLogger(__name__)
    
    def get_auth_credentials(self) -> tuple:
        """Get authentication credentials from environment variables."""
        phone = os.getenv("TELEGRAM_PHONE")
        code = os.getenv("TELEGRAM_CODE")
        password = os.getenv("TELEGRAM_2FA_PASSWORD")
        
        return phone, code, password
    
    async def authenticate(self, client, phone: str) -> bool:
        """Authenticate using environment variables."""
        env_phone, code, password = self.get_auth_credentials()
        
        if not env_phone:
            self.logger.error("TELEGRAM_PHONE not set in environment")
            return False
        
        # Send code request
        await client.send_code_request(env_phone)
        
        if code:
            try:
                await client.sign_in(phone=env_phone, code=code)
                self.logger.info("Authenticated using environment code")
                return True
            except SessionPasswordNeededError:
                if password:
                    await client.sign_in(password=password)
                    self.logger.info("Authenticated using environment 2FA password")
                    return True
                else:
                    self.logger.error("2FA required but TELEGRAM_2FA_PASSWORD not set")
                    return False
        else:
            self.logger.error("TELEGRAM_CODE not set in environment")
            return False


class SmartTelegramAuth:
    """Smart authentication that tries environment variables first, then interactive."""
    
    def __init__(self, logger=None, prefer_interactive: bool = False):
        self.logger = logger or logging.getLogger(__name__)
        self.prefer_interactive = prefer_interactive
        self.env_auth = EnvironmentAuth(logger)
        self.interactive_auth = InteractiveTelegramAuth(logger)
    
    async def authenticate(self, client, phone: str) -> bool:
        """Smart authentication with fallback."""
        # Check if we should use interactive mode
        if self.prefer_interactive:
            self.interactive_auth.show_connection_info(phone)
            return await self.interactive_auth.authenticate(client, phone)
        
        # Try environment variables first
        env_phone, code, password = self.env_auth.get_auth_credentials()
        
        if env_phone and code:
            self.logger.info("Using environment variables for authentication")
            return await self.env_auth.authenticate(client, env_phone)
        
        # Fall back to interactive
        self.logger.info("Environment variables not set, using interactive authentication")
        self.interactive_auth.show_connection_info(phone)
        return await self.interactive_auth.authenticate(client, phone)


# Convenience function for easy usage
async def authenticate_telegram(client, phone: str, logger=None, prefer_interactive: bool = False) -> bool:
    """Convenience function to authenticate with Telegram."""
    auth = SmartTelegramAuth(logger, prefer_interactive)
    return await auth.authenticate(client, phone)


# Example usage
if __name__ == "__main__":
    import asyncio
    from telethon import TelegramClient
    
    async def test_auth():
        # This would normally be called from within TelegramScraper
        print("Testing interactive authentication...")
        
        # Example of how to use it
        auth = InteractiveTelegramAuth()
        phone = "+1234567890"  # Would come from config
        
        # Show connection info
        auth.show_connection_info(phone)
        
        # Get code (simulated)
        code = auth.get_telegram_code(phone)
        if code:
            print(f"Code entered: {code}")
        
        # Get 2FA password (simulated)
        password = auth.get_2fa_password()
        if password:
            print(f"Password entered: {'*' * len(password)}")
    
    asyncio.run(test_auth())
