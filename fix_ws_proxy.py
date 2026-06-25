#!/usr/bin/python3
with open('/etc/nginx/sites-available/beta.abharcable.conf', 'r') as f:
    content = f.read()

# Fix proxy_pass for ws-status to strip the path prefix
content = content.replace(
    "location /myaddresses/mon/ws-status {\n        auth_basic off;\n        proxy_pass http://127.0.0.1:7802;",
    "location /myaddresses/mon/ws-status {\n        auth_basic off;\n        proxy_pass http://127.0.0.1:7802/;"
)

# Fix proxy_pass for ws-command to strip the path prefix
content = content.replace(
    "location /myaddresses/mon/ws-command {\n        auth_basic off;\n        proxy_pass http://127.0.0.1:7801;",
    "location /myaddresses/mon/ws-command {\n        auth_basic off;\n        proxy_pass http://127.0.0.1:7801/;"
)

with open('/etc/nginx/sites-available/beta.abharcable.conf', 'w') as f:
    f.write(content)

print("Fixed proxy_pass trailing slashes for ws-status and ws-command")
