#!/bin/bash
# Iterative build script that continues until success or max iterations

set -e

source /opt/rh/devtoolset-11/enable
export PATH="$HOME/.local/bin:$PATH"

MAX_ITERATIONS=20
ITERATION=0

echo "Starting iterative build process..."
echo "=================================="

while [ $ITERATION -lt $MAX_ITERATIONS ]; do
    ITERATION=$((ITERATION + 1))
    echo ""
    echo "Iteration $ITERATION / $MAX_ITERATIONS"
    echo "Time: $(date)"
    echo ""
    
    # Run build and capture output
    if bazel build //src:hunterclaw_core //src:hunterclaw-gateway 2>&1 | tee build_iter_${ITERATION}.log; then
        echo ""
        echo "=================================="
        echo "BUILD SUCCESSFUL!"
        echo "=================================="
        echo ""
        echo "Build artifacts:"
        ls -lh bazel-bin/src/libhunterclaw_core.so 2>&1
        ls -lh bazel-bin/src/hunterclaw-gateway 2>&1
        exit 0
    else
        echo ""
        echo "Build failed in iteration $ITERATION"
        echo "Check build_iter_${ITERATION}.log for details"
        
        # Extract first error
        ERROR=$(grep -E "fatal error:|error:" build_iter_${ITERATION}.log | head -1)
        echo "First error: $ERROR"
    fi
done

echo ""
echo "=================================="
echo "BUILD FAILED after $MAX_ITERATIONS iterations"
echo "=================================="
exit 1
