#!/bin/bash
# HunterClaw Gateway 编译脚本
# 使用 devtoolset-11 编译

set -e

echo "========================================"
echo "HunterClaw Gateway 编译"
echo "========================================"

# 启用 devtoolset-11
echo "启用 devtoolset-11..."
source /opt/rh/devtoolset-11/enable

# 显示编译器版本
echo "编译器版本:"
gcc --version | head -1
g++ --version | head -1

echo ""
echo "开始编译..."
echo ""

# 编译
bazel build //src:hunterclaw-gateway \
    --action_env=PATH \
    --action_env=LD_LIBRARY_PATH \
    --verbose_failures

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "编译成功！"
    echo "========================================"
    echo "可执行文件位置:"
    echo "  bazel-bin/src/hunterclaw-gateway"
    echo ""
    ls -lh bazel-bin/src/hunterclaw-gateway
else
    echo ""
    echo "========================================"
    echo "编译失败"
    echo "========================================"
    exit 1
fi
