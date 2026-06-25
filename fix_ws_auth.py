#!/usr/bin/python3
import re

with open('/etc/nginx/sites-available/beta.abharcable.conf', 'r') as f:
    content = f.read()

# Remove auth_basic lines from ws-status and ws-command location blocks
# These need to be accessible by the browser's WebSocket API which can't send auth headers

# Remove auth_basic from ws-status
content = content.replace(
    """location /myaddresses/mon/ws-status {
        auth_basic 'Hunter Monitor';
        auth_basic_user_file /etc/nginx/.htpasswd_hunter;""",
    """location /myaddresses/mon/ws-status {
        auth_basic off;"""
)

# Remove auth_basic from ws-command
content = content.replace(
    """location /myaddresses/mon/ws-command {
        auth_basic 'Hunter Monitor';
        auth_basic_user_file /etc/nginx/.htpasswd_hunter;""",
    """location /myaddresses/mon/ws-command {
        auth_basic off;"""
)

with open('/etc/nginx/sites-available/beta.abharcable.conf', 'w') as f:
    f.write(content)

print("Removed auth_basic from ws-status and ws-command")
