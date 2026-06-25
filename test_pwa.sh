#!/bin/bash
echo "=== Check HTML for manifest/icon references ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' 2>&1 | grep -o 'manifest\|icon-192\|sw\.js' | sort | uniq -c

echo "=== Manifest request (should be 404, no longer referenced) ==="
curl -sk --noproxy '*' https://127.0.0.1/myaddresses/mon/manifest.json -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== Icon request without basePath (should be 404) ==="
curl -sk --noproxy '*' https://127.0.0.1/icon-192.png -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== UI with auth ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

echo "=== API configs with auth ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1
