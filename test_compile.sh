#!/bin/bash

source /opt/rh/devtoolset-11/enable

echo "HunterClaw Compilation Test Report"
echo "===================================="
echo ""

success=0
failed=0
failed_files=""

for f in $(find src -name "*.cc" | head -50); do
    if g++ -std=c++17 -I./include -I./third_party -c "$f" -o /tmp/test.o 2>/dev/null; then
        ((success++))
        echo "✓ $f"
    else
        ((failed++))
        failed_files="$failed_files\n$f"
        echo "✗ $f"
    fi
done

echo ""
echo "===================================="
echo "Summary: $success passed, $failed failed"
echo ""

if [ $failed -gt 0 ]; then
    echo "Failed files:"
    echo -e "$failed_files"
fi
