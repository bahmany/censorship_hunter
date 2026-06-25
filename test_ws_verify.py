import asyncio
import websockets

async def test():
    try:
        async with websockets.connect('ws://127.0.0.1:7802') as ws:
            print('CONNECTED to ws://127.0.0.1:7802')
            msg = await asyncio.wait_for(ws.recv(), timeout=5)
            print(f'MSG (first 200 chars): {msg[:200]}')
            msg2 = await asyncio.wait_for(ws.recv(), timeout=5)
            print(f'MSG2 (first 200 chars): {msg2[:200]}')
    except Exception as e:
        print(f'ERROR: {e}')

asyncio.run(test())
