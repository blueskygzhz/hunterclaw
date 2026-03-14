#!/bin/bash
source /opt/rh/devtoolset-11/enable

echo "=== Quick Compilation Test Round 4 ==="
echo ""

# 候选文件列表
files=(
    "src/queue/command_queue.cc"
    "src/queue/delivery_queue.cc"
    "src/device/device_manager.cc"
    "src/observability/tracing.cc"
    "src/channels/manager.cc"
    "src/sessions/session_manager.cc"
    "src/agents/agent_scope.cc"
    "src/core/supervisor_registry.cc"
    "src/memory/search_manager.cc"
    "src/memory/sqlite_plugin.cc"
    "src/tools/cron_tool.cc"
    "src/tools/nodes_tool.cc"
    "src/tools/pdf_tool.cc"
    "src/tools/memory_tools.cc"
    "src/gateway/websocket_server.cc"
    "src/gateway/gateway.cc"
    "src/main.cc"
)

success_list=""
fail_list=""
success=0
fail=0

for f in "${files[@]}"; do
    echo -n "Testing $(basename $f)... "
    if timeout 5 g++ -std=c++17 -I./include -I./third_party -I/usr/include -c "$f" 2>&1 >/dev/null; then
        echo "✓"
        success=$((success+1))
        success_list="$success_list\n  ✓ $(basename $f)"
    else
        echo "✗"
        fail=$((fail+1))
        fail_list="$fail_list\n  ✗ $(basename $f)"
    fi
done

echo ""
echo "========================================="
echo "SUCCESS: $success | FAILED: $fail"
echo "========================================="

if [ -n "$success_list" ]; then
    echo -e "\n✓ Successful files:$success_list"
fi

if [ -n "$fail_list" ]; then
    echo -e "\n✗ Failed files:$fail_list"
fi
