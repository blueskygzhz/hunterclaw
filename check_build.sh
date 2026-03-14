#!/bin/bash
# Quick build status checker

LOG_FILE="/data/workspace/QQMail/HunterClaw/bazel_build_final.log"

echo "Checking build status..."
echo ""

# Wait for log file to appear
for i in {1..10}; do
    if [ -f "$LOG_FILE" ]; then
        break
    fi
    sleep 1
done

# Check for errors in the last 100 lines
if [ -f "$LOG_FILE" ]; then
    echo "=== Recent errors ==="
    tail -100 "$LOG_FILE" | grep -E "error:|ERROR:" | head -10
    
    echo ""
    echo "=== Build status (last 20 lines) ==="
    tail -20 "$LOG_FILE"
else
    echo "Log file not found yet"
fi
