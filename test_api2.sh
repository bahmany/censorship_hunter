#!/bin/bash
echo "=== Test API route through nginx ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Test API route directly to Next.js ==="
curl -sk --noproxy '*' http://127.0.0.1:7800/myaddresses/mon/api/configs/all -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Test API route content (first 5 lines) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' 2>&1 | head -5

echo "=== Test UI still works ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1
