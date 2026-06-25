#!/bin/bash
# Test WebSocket status endpoint
echo "=== WS Status ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/ws-status -H 'Host: api.abharcable.com' -H 'Upgrade: websocket' -H 'Connection: Upgrade' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H 'Sec-WebSocket-Version: 13' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

# Test API endpoint
echo "=== API ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

# Test without auth (should be 401)
echo "=== No Auth ==="
curl -sk --noproxy '*' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1
