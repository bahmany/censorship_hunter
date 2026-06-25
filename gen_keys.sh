#!/bin/sh
mkdir -p /app/runtime/ssh
ssh-keygen -t ed25519 -f /app/runtime/ssh/upstream1_key -N "" -C hunter-upstream1 2>&1 || true
ssh-keygen -t ed25519 -f /app/runtime/ssh/upstream2_key -N "" -C hunter-upstream2 2>&1 || true
chmod 600 /app/runtime/ssh/upstream1_key /app/runtime/ssh/upstream2_key
echo "--- PUBLIC KEY 1 ---"
cat /app/runtime/ssh/upstream1_key.pub
echo "--- PUBLIC KEY 2 ---"
cat /app/runtime/ssh/upstream2_key.pub
