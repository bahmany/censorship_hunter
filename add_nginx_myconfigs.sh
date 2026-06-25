#!/bin/sh
# Add /myconfigs/ location to beta.abharcable.conf before the Hunter section
sudo sed -i '/# Hunter Censorship Monitor UI/i\
    # ============================================================\
    # Public Config Distribution Endpoints (no auth required)\
    # ============================================================\
    location /myconfigs/ {\
        auth_basic off;\
        proxy_pass http://127.0.0.1:7800;\
        proxy_http_version 1.1;\
        proxy_set_header Host $host;\
        proxy_set_header X-Real-IP $remote_addr;\
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;\
        proxy_set_header X-Forwarded-Proto $scheme;\
        proxy_read_timeout 30;\
        proxy_send_timeout 30;\
    }\
\
' /etc/nginx/sites-available/beta.abharcable.conf

# Check if already added
if grep -q 'myconfigs' /etc/nginx/sites-available/beta.abharcable.conf; then
    echo "myconfigs location already present or added"
else
    echo "ERROR: Failed to add myconfigs location"
fi

sudo nginx -t && sudo nginx -s reload
echo "Nginx reloaded"
