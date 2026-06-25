#!/bin/bash
# Test API directly to backend
echo "=== Direct to backend: /api/configs/all ==="
curl -sk --noproxy '*' http://127.0.0.1:7801/api/configs/all -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

# Test through nginx with rewrite
echo "=== Through nginx: /myaddresses/mon/api/configs/all ==="
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/api/configs/all -H 'Host: api.abharcable.com' -o /dev/null -w 'HTTP %{http_code}\n' 2>&1

# Check nginx error log
echo "=== Nginx error log ==="
sudo tail -5 /var/log/nginx/api_error.log 2>&1
