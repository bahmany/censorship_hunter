import asyncio
import os
import json
import urllib.request
import urllib.error

async def test_bot():
    token = "7736297215:AAGxmcqBlTZ2GYIX2FzVcyTixzYDUTzWHiA"
    chat_id = "-1002567385742"
    
    url = f'https://api.telegram.org/bot{token}/sendMessage'
    data = {
        'chat_id': chat_id,
        'text': f'Test message from Hunter Bot API - Time: {asyncio.get_event_loop().time()}',
        'parse_mode': 'Markdown'
    }
    
    try:
        body = json.dumps(data).encode('utf-8')
        req = urllib.request.Request(url, data=body, headers={'Content-Type': 'application/json'}, method='POST')
        
        with urllib.request.urlopen(req, timeout=30) as resp:
            result = json.loads(resp.read().decode())
            
        if result.get('ok'):
            print('SUCCESS: Test message sent via Bot API')
            msg_id = result.get('result', {}).get('message_id', 'N/A')
            print(f'Message ID: {msg_id}')
            return True
        else:
            desc = result.get('description', 'Unknown error')
            print(f'FAILED: {desc}')
            return False
            
    except Exception as e:
        print(f'ERROR: {e}')
        return False

if __name__ == "__main__":
    result = asyncio.run(test_bot())
    print(f'Test result: {"PASS" if result else "FAIL"}')
