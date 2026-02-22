#!/usr/bin/env python3
"""
Hunter Launcher - Python-based launcher that properly loads environment variables
"""

import os
import sys
import json
from pathlib import Path

# Add the project directory to Python path
sys.path.insert(0, str(Path(__file__).parent))

def load_secrets():
    """Load environment variables from hunter_secrets.env."""
    secrets_path = Path(__file__).parent / "hunter_secrets.env"
    
    if not secrets_path.exists():
        print("Warning: hunter_secrets.env not found.")
        return
    
    print("Loading secrets from hunter_secrets.env...")
    
    try:
        with open(secrets_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # Handle SSH_SERVERS_JSON (multi-line)
        if 'SSH_SERVERS_JSON=' in content:
            start = content.find('SSH_SERVERS_JSON=')
            if start != -1:
                start += len('SSH_SERVERS_JSON=')
                remaining = content[start:].strip()
                
                # Remove quotes if present
                if remaining.startswith('"') and remaining.endswith('"'):
                    remaining = remaining[1:-1]
                elif remaining.startswith("'") and remaining.endswith("'"):
                    remaining = remaining[1:-1]
                
                # Find the end of the JSON array
                if remaining.startswith('[') and ']' in remaining:
                    end = remaining.find(']') + 1
                    json_str = remaining[:end]
                    os.environ['SSH_SERVERS_JSON'] = json_str
                    print(f"  Loaded SSH_SERVERS_JSON with {len(json.loads(json_str))} servers")
        
        # Handle other variables line by line
        for line in content.split('\n'):
            line = line.strip()
            
            # Skip comments and empty lines
            if not line or line.startswith('#'):
                continue
            
            # Skip SSH_SERVERS_JSON (already handled)
            if line.startswith('SSH_SERVERS_JSON'):
                continue
            
            # Parse key=value pairs
            if '=' in line:
                # Remove inline comments
                if '#' in line:
                    line = line[:line.index('#')]
                
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                
                # Remove quotes
                if value.startswith('"') and value.endswith('"'):
                    value = value[1:-1]
                elif value.startswith("'") and value.endswith("'"):
                    value = value[1:-1]
                
                os.environ[key] = value
                
                # Mask sensitive values in output
                if any(sensitive in key for sensitive in ['API_HASH', 'TOKEN', 'PASS', 'PASSWORD']):
                    masked = value[:8] + '*' * (len(value) - 8) if len(value) > 8 else '*' * len(value)
                    print(f"  Loaded {key}: {masked}")
                else:
                    print(f"  Loaded {key}: {value}")
        
        print("Secrets loaded successfully.")
        
    except Exception as e:
        print(f"Error loading secrets: {e}")

def check_requirements():
    """Check if required files and directories exist."""
    print("Checking requirements...")
    
    # Check virtual environment
    venv_path = Path(__file__).parent / ".venv"
    if not venv_path.exists():
        print("Error: Virtual environment not found at .venv")
        print("Please run setup first.")
        return False
    
    # Check main.py
    main_path = Path(__file__).parent / "main.py"
    if not main_path.exists():
        print("Error: main.py not found")
        return False
    
    # Create runtime directory
    runtime_path = Path(__file__).parent / "runtime"
    runtime_path.mkdir(exist_ok=True)
    print("Runtime directory ready.")
    
    return True

def main():
    """Main launcher function."""
    print("=" * 60)
    print("  HUNTER - Advanced V2Ray Proxy Hunting System")
    print("  Autonomous censorship circumvention tool")
    print("=" * 60)
    
    # Load secrets
    load_secrets()
    print()
    
    # Check requirements
    if not check_requirements():
        input("Press Enter to exit...")
        return 1
    
    print()
    print("Starting Hunter...")
    
    # Run main directly (no subprocess - keeps everything in one process)
    try:
        import asyncio
        from main import main as hunter_main
        return asyncio.run(hunter_main())
        
    except KeyboardInterrupt:
        print("\nHunter stopped by user.")
        return 0
    except Exception as e:
        print(f"Error running Hunter: {e}")
        import traceback
        traceback.print_exc()
        return 1

if __name__ == "__main__":
    sys.exit(main() or 0)
