#!/bin/bash
set -e

# Enable devtoolset
source /opt/rh/devtoolset-11/enable

# Add bazelisk to PATH
export PATH="$HOME/.local/bin:$PATH"

# Clean previous build logs
rm -f bazel_build_final.log

# Start build
echo "Starting Bazel build..."
echo "Build started at: $(date)" | tee bazel_build_final.log

bazel build //src:hunterclaw_core //src:hunterclaw-gateway 2>&1 | tee -a bazel_build_final.log

# Check result
if [ $? -eq 0 ]; then
    echo "" | tee -a bazel_build_final.log
    echo "==================================" | tee -a bazel_build_final.log
    echo "BUILD SUCCESSFUL!" | tee -a bazel_build_final.log
    echo "==================================" | tee -a bazel_build_final.log
    echo "" | tee -a bazel_build_final.log
    echo "Build artifacts:" | tee -a bazel_build_final.log
    ls -lh bazel-bin/src/libhunterclaw_core.so 2>&1 | tee -a bazel_build_final.log
    ls -lh bazel-bin/src/hunterclaw-gateway 2>&1 | tee -a bazel_build_final.log
else
    echo "" | tee -a bazel_build_final.log
    echo "==================================" | tee -a bazel_build_final.log
    echo "BUILD FAILED!" | tee -a bazel_build_final.log
    echo "==================================" | tee -a bazel_build_final.log
    exit 1
fi
