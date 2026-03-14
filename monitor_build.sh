#!/bin/bash
# Monitor Bazel build progress

LOG_FILE="/data/workspace/QQMail/HunterClaw/bazel_build_final.log"

while true; do
    clear
    echo "==================================="
    echo "Bazel Build Monitor"
    echo "==================================="
    echo "Time: $(date)"
    echo ""
    
    # Check if build process is running
    if ps aux | grep -E "build_with_bazel" | grep -v grep > /dev/null; then
        echo "Status: BUILD IN PROGRESS"
    else
        echo "Status: BUILD COMPLETED OR NOT RUNNING"
    fi
    
    echo ""
    echo "Latest build output (last 30 lines):"
    echo "-----------------------------------"
    tail -30 "$LOG_FILE" 2>/dev/null || echo "No log file yet"
    
    echo ""
    echo "Press Ctrl+C to exit"
    
    sleep 3
done
