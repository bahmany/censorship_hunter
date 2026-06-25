#!/bin/bash
# Test without trailing slash (the one Next.js redirects to)
echo "=== /myaddresses/mon (no slash) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== /myaddresses/mon/ (with slash) ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/ -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== /myaddresses/mon (no slash, follow redirects) ==="
curl -sk --noproxy '*' -L -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Direct to Next.js: /myaddresses/mon ==="
curl -sk --noproxy '*' http://127.0.0.1:7800/myaddresses/mon -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Direct to Next.js: /myaddresses/mon/ ==="
curl -sk --noproxy '*' http://127.0.0.1:7800/myaddresses/mon/ -o /dev/null -w 'HTTP %{http_code}\n' 2>&1
