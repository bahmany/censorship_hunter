"""
SSH servers configuration for Telegram fallback
Add your SSH servers here in the following format:
"""

SSH_SERVERS = [
    # Example SSH servers
    # {
    #     "host": "server1.example.com",
    #     "port": 22,
    #     "user": "username",
    #     "password": "password"  # or use "key": "path/to/private/key"
    # },
    # {
    #     "host": "server2.example.com", 
    #     "port": 2222,
    #     "user": "username",
    #     "key": "/path/to/private/key"
    # },
    
    # Add your actual SSH servers here
    # These will be used as fallback proxies for Telegram API access
]

# You can also specify these via environment variables:
# HUNTER_SSH_SERVERS = JSON array of server configurations
