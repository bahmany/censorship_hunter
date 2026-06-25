#!/usr/bin/python3
import re

with open('/etc/nginx/sites-available/beta.abharcable.conf', 'r') as f:
    content = f.read()

# Fix the rewrite line
old = 'rewrite ^/myconfigs/(.*)$ /myaddresses/mon/api/myconfigs/ break;'
new = 'rewrite ^/myconfigs/(.*)$ /myaddresses/mon/api/myconfigs/$1 break;'
content = content.replace(old, new)

with open('/etc/nginx/sites-available/beta.abharcable.conf', 'w') as f:
    f.write(content)

print("Fixed rewrite line")
