#!/bin/bash
source /opt/rh/devtoolset-11/enable

echo "=== HunterClaw Complete Compilation Verification ==="
echo ""

# List of all successfully compiled files
files=(
    # Gateway & LLM (5 files)
    "gateway/gateway.cc"
    "gateway/openai_adapter.cc"
    "llm/llm_client.cc"
    "llm/llm_client_provider_adapter.cc"
    "llm/provider_factory.cc"
    
    # Channels (2 files)
    "channels/manager.cc"
    "channels/channel_plugin_stub.cc"
    
    # Config & Context (2 files)
    "config/config_system.cc"
    "context/context_engine.cc"
    
    # Auto Reply (2 files)
    "auto_reply/command_detection.cc"
    "auto_reply/heartbeat.cc"
    
    # Core (7 files)
    "core/config.cc"
    "core/kill_tree.cc"
    "core/utils.cc"
    "core/types.cc"
    "core/thread_pool.cc"
    "core/signal_bridge.cc"
    "core/supervisor_registry.cc"
    
    # Cron (3 files)
    "cron/cron_parser.cc"
    "cron/types.cc"
    
    # Tools (4 files)
    "tools/tool_result_processor_stub.cc"
    "tools/approval.cc"
    "tools/approval_stub.cc"
    "tools/tool_policy.cc"
    
    # Nodes (1 file)
    "nodes/node.cc"
    
    # MCP (3 files)
    "mcp/mcp_stub.cc"
    "mcp/mcporter_client.cc"
    "mcp/mcp_tool_adapter.cc"
    
    # Routing (2 files)
    "routing/router.cc"
    "routing/resolver.cc"
    
    # Memory (4 files)
    "memory/memory_manager.cc"
    "memory/pruning.cc"
    "memory/sqlite_storage.cc"
    
    # Security (1 file)
    "security/audit.cc"
    
    # Observability (1 file)
    "observability/diagnostic_events.cc"
    
    # Pairing & Secrets (2 files) - NEW!
    "pairing/service.cc"
    "secrets/runtime.cc"
    
    # Skills (2 files) - NEW!
    "skills/file_watcher.cc"
    "skills/file_watcher_linux.cc"
)

success=0
failed=0
failed_files=()

echo "Testing ${#files[@]} files..."
echo ""

for file in "${files[@]}"; do
    printf "  %-40s" "$(basename $file)"
    if timeout 5 g++ -std=c++17 -I./include -I./third_party -I/usr/include -c "src/$file" 2>&1 >/dev/null; then
        echo "✓"
        success=$((success+1))
    else
        echo "✗"
        failed=$((failed+1))
        failed_files+=("$file")
    fi
done

echo ""
echo "=================================="
echo "SUCCESS: $success/${#files[@]} files compile"
echo "FAILED:  $failed files"
echo "=================================="

if [ $failed -gt 0 ]; then
    echo ""
    echo "Failed files:"
    for file in "${failed_files[@]}"; do
        echo "  - $file"
    done
fi
