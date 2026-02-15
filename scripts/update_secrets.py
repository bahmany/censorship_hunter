#!/usr/bin/env python3
"""
Script to update hunter_secrets.env with SSH server credentials.

This script will:
1. Read the current hunter_secrets.env file
2. Add SSH server configuration if not present
3. Preserve existing credentials
"""

import os
from pathlib import Path

def update_secrets_file():
    """Update hunter_secrets.env with SSH server configuration."""
    
    secrets_path = Path(__file__).parent / "hunter_secrets.env"
    
    # SSH server configuration
    ssh_config = """
# SSH Servers Configuration for Telegram Tunnel
SSH_SERVERS_JSON=[
  {"host": "71.143.156.145", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
  {"host": "71.143.156.146", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
  {"host": "71.143.156.147", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
  {"host": "71.143.156.148", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
  {"host": "71.143.156.149", "port": 2, "username": "deployer", "password": "009100mohammad_mrb"},
  {"host": "50.114.11.18", "port": 22, "username": "deployer", "password": "009100mohammad_mrb"}
]

# SSH Tunnel Configuration
SSH_SOCKS_HOST=127.0.0.1
SSH_SOCKS_PORT=1088
"""
    
    # Check if file exists
    if not secrets_path.exists():
        print(f"Creating new hunter_secrets.env file...")
        with open(secrets_path, 'w', encoding='utf-8') as f:
            f.write(ssh_config)
        print(f"[OK] Created {secrets_path}")
        return True
    
    # Read existing content
    with open(secrets_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Check if SSH_SERVERS_JSON already exists
    if "SSH_SERVERS_JSON=" in content:
        print("SSH_SERVERS_JSON already exists in hunter_secrets.env")
        print("[OK] No changes needed")
        return True
    
    # Append SSH configuration
    with open(secrets_path, 'a', encoding='utf-8') as f:
        f.write(ssh_config)
    
    print(f"[OK] Updated {secrets_path} with SSH server configuration")
    return True

def show_current_secrets():
    """Show current content of hunter_secrets.env (masked)."""
    
    secrets_path = Path(__file__).parent / "hunter_secrets.env"
    
    if not secrets_path.exists():
        print("hunter_secrets.env does not exist")
        return
    
    with open(secrets_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    print("\nCurrent hunter_secrets.env content:")
    print("=" * 50)
    
    # Mask sensitive information
    lines = content.split('\n')
    for line in lines:
        if 'PASSWORD=' in line or 'PASS=' in line:
            # Mask passwords
            parts = line.split('=', 1)
            if len(parts) == 2:
                key = parts[0]
                password = parts[1]
                masked_password = password[:4] + '*' * (len(password) - 4)
                print(f"{key}={masked_password}")
            else:
                print(line)
        elif 'API_HASH=' in line:
            # Mask API hash
            parts = line.split('=', 1)
            if len(parts) == 2:
                key = parts[0]
                hash_val = parts[1]
                masked_hash = hash_val[:8] + '*' * (len(hash_val) - 8)
                print(f"{key}={masked_hash}")
            else:
                print(line)
        else:
            print(line)
    
    print("=" * 50)

if __name__ == "__main__":
    print("Hunter Secrets File Updater")
    print("=" * 50)
    
    # Update the file
    if update_secrets_file():
        print("\n[OK] SSH server configuration added successfully!")
        
        # Show current content
        show_current_secrets()
        
        print("\nNext steps:")
        print("1. Restart Hunter to load new SSH configuration")
        print("2. Run 'python test_ssh_caching.py' to verify SSH servers")
        print("3. Hunter will now use all 6 SSH servers with fallback")
    else:
        print("[ERROR] Failed to update hunter_secrets.env")
