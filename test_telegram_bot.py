#!/usr/bin/env python3
"""
Test Telegram Bot API with provided credentials.
"""

import os
import sys
import json
import asyncio
import logging

# Add project root to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from hunter.telegram.scraper import BotReporter

async def test_bot_api():
    """Test the Bot API reporter."""
    logging.basicConfig(level=logging.INFO, format='%(asctime)s | %(levelname)s | %(message)s')
    logger = logging.getLogger(__name__)
    
    # Check environment variables
    token = os.getenv("TOKEN") or os.getenv("TELEGRAM_BOT_TOKEN")
    chat_id = os.getenv("CHAT_ID") or os.getenv("TELEGRAM_GROUP_ID")
    
    logger.info(f"Token: {token[:10] + '...' if token and len(token) > 10 else 'None'}")
    logger.info(f"Chat ID: {chat_id}")
    
    if not token or not chat_id:
        logger.error("TOKEN or CHAT_ID not set in environment variables")
        return False
    
    # Create BotReporter instance
    reporter = BotReporter()
    
    if not reporter.enabled:
        logger.error("Bot reporter not enabled")
        return False
    
    logger.info("Bot reporter initialized successfully")
    
    # Test sending a message
    test_message = f"🧪 Test message from Hunter Bot API Test\n⏰ Time: {asyncio.get_event_loop().time()}"
    logger.info(f"Sending test message: {test_message}")
    
    try:
        success = await reporter.send_message(test_message)
        if success:
            logger.info("✅ Test message sent successfully via Bot API")
            
            # Test sending a file
            test_content = b"Test file content from Hunter\nLine 2\nLine 3"
            file_success = await reporter.send_file(
                filename="test.txt",
                content=test_content,
                caption="📄 Test file from Hunter Bot API"
            )
            
            if file_success:
                logger.info("✅ Test file sent successfully via Bot API")
                return True
            else:
                logger.error("❌ Failed to send test file")
                return False
        else:
            logger.error("❌ Failed to send test message")
            return False
    except Exception as e:
        logger.error(f"❌ Error testing Bot API: {e}")
        return False

if __name__ == "__main__":
    result = asyncio.run(test_bot_api())
    sys.exit(0 if result else 1)
