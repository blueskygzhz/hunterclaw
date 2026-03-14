#!/bin/bash
source /opt/rh/devtoolset-11/enable

files=(
    "routing/resolver.cc"
    "routing/router.cc"
    "security/audit.cc"
    "memory/sqlite_storage.cc"
    "observability/diagnostic_events.cc"
    "gateway/gateway.cc"
    "gateway/openai_adapter.cc"
    "llm/llm_client.cc"
    "llm/llm_client_provider_adapter.cc"
    "llm/provider_factory.cc"
)

success=0
echo "Testing core fixed files..."
for file in "${files[@]}"; do
    if timeout 5 g++ -std=c++17 -I./include -I./third_party -I/usr/include -c "src/$file" 2>&1 >/dev/null; then
        echo "✓ $file"
        success=$((success+1))
    else
        echo "✗ $file"
    fi
done
echo ""
echo "Result: $success/${#files[@]} files compile successfully"
