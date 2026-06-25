#!/bin/bash
echo "=== Manifest WITHOUT auth (should be 200) ==="
curl -sk --noproxy '*' https://127.0.0.1/myaddresses/mon/manifest.json -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Icon WITHOUT auth (should be 200) ==="
curl -sk --noproxy '*' https://127.0.0.1/myaddresses/mon/icon-192.png -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== UI WITHOUT auth (should be 401) ==="
curl -sk --noproxy '*' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== UI WITH auth (should be 200) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== API configs WITH auth (should be 200) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== WS status WITH auth (should be 000 = upgrade) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' -H 'Host: api.abharcable.com' -H 'Upgrade: websocket' -H 'Connection: Upgrade' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' -H 'Sec-WebSocket-Version: 13' -o /dev/null -w 'HTTP %{http_code}\n' https://127.0.0.1/myaddresses/mon/ws-status 2>&1
