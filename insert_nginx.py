import re

config_path = '/etc/nginx/sites-available/beta.abharcable.conf'

with open(config_path, 'r') as f:
    content = f.read()

# The 6 VLESS paths and their corresponding Xray ports
vless_paths = [
    ('/ws/proxy/tunnel_v2/', 17463),
    ('/ws/proxy/tunnel_v2_1/', 17464),
    ('/ws/proxy/tunnel_v2_2/', 17465),
    ('/ws/proxy/tunnel_v2_3/', 17466),
    ('/ws/proxy/tunnel_v2_4/', 17467),
    ('/ws/proxy/tunnel_v2_p/', 17468),
]

new_blocks = "    # VLESS WebSocket Endpoints (Hunter legacy configs)\n"
for path, port in vless_paths:
    new_blocks += "    location " + path + " {\n"
    new_blocks += "        proxy_pass http://127.0.0.1:" + str(port) + ";\n"
    new_blocks += "        proxy_http_version 1.1;\n"
    new_blocks += "        proxy_set_header Upgrade $http_upgrade;\n"
    new_blocks += '        proxy_set_header Connection "upgrade";\n'
    new_blocks += "        proxy_set_header Host $host;\n"
    new_blocks += "        proxy_set_header X-Real-IP $remote_addr;\n"
    new_blocks += "        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\n"
    new_blocks += "        proxy_set_header X-Forwarded-Proto $scheme;\n"
    new_blocks += "        proxy_buffering off;\n"
    new_blocks += "        proxy_request_buffering off;\n"
    new_blocks += "        proxy_cache off;\n"
    new_blocks += "        proxy_read_timeout 7d;\n"
    new_blocks += "        proxy_send_timeout 7d;\n"
    new_blocks += "        proxy_connect_timeout 10s;\n"
    new_blocks += "        client_max_body_size 0;\n"
    new_blocks += "        tcp_nodelay on;\n"
    new_blocks += "    }\n"

# Check if old single /ws/proxy/ block exists
old_marker = "# VLESS WebSocket Endpoints (Hunter legacy configs)"
if old_marker in content:
    # Find and remove the old block
    idx = content.index(old_marker)
    # Find the next "location /ws/ {" after the old block
    rest = content[idx:]
    # The old block ends before "location /ws/ {"
    ws_idx = rest.index("location /ws/ {")
    old_block = rest[:ws_idx]
    content = content[:idx] + new_blocks + content[idx + len(old_block):]
    print("Replaced existing /ws/proxy/ block with 6 specific blocks")
else:
    # Insert before "location /ws/ {"
    lines = content.split('\n')
    inserted = False
    new_lines = []
    for i, line in enumerate(lines):
        if not inserted and line.strip() == 'location /ws/ {':
            new_lines.append(new_blocks.rstrip('\n'))
            new_lines.append(line)
            inserted = True
        else:
            new_lines.append(line)
    content = '\n'.join(new_lines)
    if not inserted:
        print("ERROR: Could not find insertion point")
        exit(1)
    print("Inserted 6 VLESS location blocks before /ws/ block")

with open(config_path, 'w') as f:
    f.write(content)

print("Done")
