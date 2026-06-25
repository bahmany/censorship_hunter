#!/bin/bash
# Test with redirect following
curl -sk --noproxy '*' -L -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/ -H 'Host: api.abharcable.com' 2>&1 | head -30
echo "---"
# Check what the redirect location is
curl -sk --noproxy '*' -u 'admin:mohammaD123$%' https://127.0.0.1/myaddresses/mon/ -H 'Host: api.abharcable.com' -v 2>&1 | grep -i 'location\|< HTTP'
