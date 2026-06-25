#!/bin/bash
# Test WebSocket upgrade through nginx
echo "=== WS Status through nginx ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' \
  -H 'Host: api.abharcable.com' \
  -H 'Upgrade: websocket' \
  -H 'Connection: Upgrade' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  -H 'Sec-WebSocket-Version: 13' \
  -o /dev/null -w 'HTTP %{http_code}\n' \
  https://127.0.0.1/myaddresses/mon/ws-status 2>&1

echo "=== WS Command through nginx ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' \
  -H 'Host: api.abharcable.com' \
  -H 'Upgrade: websocket' \
  -H 'Connection: Upgrade' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  -H 'Sec-WebSocket-Version: 13' \
  -o /dev/null -w 'HTTP %{http_code}\n' \
  https://127.0.0.1/myaddresses/mon/ws-command 2>&1

echo "=== WS Status direct ==="
curl -sk --noproxy '*' \
  -H 'Upgrade: websocket' \
  -H 'Connection: Upgrade' \
  -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  -H 'Sec-WebSocket-Version: 13' \
  -o /dev/null -w 'HTTP %{http_code}\n' \
  http://127.0.0.1:7802 2>&1
