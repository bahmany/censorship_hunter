#!/bin/sh
# Generate SSH key pairs for upstream tunnel connections
# These keys are used by the UpstreamTunnelManager to establish SOCKS tunnels

SSH_DIR="/app/runtime/ssh"
mkdir -p "$SSH_DIR"

# Generate key for PRIMARY_UPSTREAM_1 (18.194.88.88, ec2-user)
if [ ! -f "$SSH_DIR/upstream1_key" ]; then
    echo "[SSH Setup] Generating key for PRIMARY_UPSTREAM_1..."
    ssh-keygen -t ed25519 -f "$SSH_DIR/upstream1_key" -N "" -C "hunter-upstream1"
    chmod 600 "$SSH_DIR/upstream1_key"
    echo "[SSH Setup] Public key for PRIMARY_UPSTREAM_1:"
    cat "$SSH_DIR/upstream1_key.pub"
    echo "[SSH Setup] Add this key to ec2-user@18.194.88.88:~/.ssh/authorized_keys"
fi

# Generate key for PRIMARY_UPSTREAM_2 (50.114.11.18, deployer)
if [ ! -f "$SSH_DIR/upstream2_key" ]; then
    echo "[SSH Setup] Generating key for PRIMARY_UPSTREAM_2..."
    ssh-keygen -t ed25519 -f "$SSH_DIR/upstream2_key" -N "" -C "hunter-upstream2"
    chmod 600 "$SSH_DIR/upstream2_key"
    echo "[SSH Setup] Public key for PRIMARY_UPSTREAM_2:"
    cat "$SSH_DIR/upstream2_key.pub"
    echo "[SSH Setup] Add this key to deployer@50.114.11.18:~/.ssh/authorized_keys"
fi

# Create SSH config to disable known_hosts checking
cat > "$SSH_DIR/config" << 'EOF'
Host *
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
    ServerAliveInterval 30
    ServerAliveCountMax 3
    ExitOnForwardFailure yes
    ConnectTimeout 10
EOF
chmod 644 "$SSH_DIR/config"

echo "[SSH Setup] SSH key setup complete."
