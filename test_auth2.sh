#!/bin/bash
# Create a simple test password
htpasswd -bc /etc/nginx/.htpasswd_test admin 'test123'
# Test it
curl -sk --noproxy '*' -u 'admin:test123' https://127.0.0.1/myaddresses/mon/ -H 'Host: api.abharcable.com' -o /dev/null -w '%{http_code}\n' 2>&1
# Now test the real password
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/ -H 'Host: api.abharcable.com' -o /dev/null -w '%{http_code}\n' 2>&1
# Show what htpasswd_test looks like
cat /etc/nginx/.htpasswd_test
echo "---"
cat /etc/nginx/.htpasswd_hunter
