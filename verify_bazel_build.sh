#!/bin/bash

# ============================================================================
# Bazel 构建验证脚本
# ============================================================================

set -e

echo "🔍 检查 Bazel 安装..."
if ! command -v bazel &> /dev/null; then
    echo "❌ Bazel 未安装！"
    echo "请按照 BUILD.bazel.md 中的说明安装 Bazel"
    exit 1
fi

BAZEL_VERSION=$(bazel version | grep "Build label" | awk '{print $3}')
echo "✅ Bazel 版本: $BAZEL_VERSION"

echo ""
echo "🔍 检查系统依赖..."
MISSING_DEPS=()

check_lib() {
    if ! pkg-config --exists "$1" 2>/dev/null && ! ldconfig -p | grep -q "$2" 2>/dev/null; then
        MISSING_DEPS+=("$1")
        return 1
    fi
    return 0
}

check_lib "openssl" "libssl"
check_lib "libcurl" "libcurl"
check_lib "uuid" "libuuid"
check_lib "jsoncpp" "libjsoncpp"

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo "⚠️  缺少以下系统依赖: ${MISSING_DEPS[*]}"
    echo "   请运行: sudo apt-get install -y libssl-dev libcurl4-openssl-dev uuid-dev libjsoncpp-dev"
    echo "   继续构建可能会失败..."
else
    echo "✅ 所有系统依赖已安装"
fi

echo ""
echo "🧹 清理之前的构建..."
bazel clean

echo ""
echo "🔨 开始构建核心库..."
if bazel build //src:hunterclaw_core; then
    echo "✅ 核心库构建成功"
else
    echo "❌ 核心库构建失败"
    exit 1
fi

echo ""
echo "🔨 开始构建网关服务器..."
if bazel build //src:hunterclaw-gateway; then
    echo "✅ 网关服务器构建成功"
else
    echo "❌ 网关服务器构建失败"
    exit 1
fi

echo ""
echo "🔨 开始构建测试..."
if bazel build //tests:test_process_integration; then
    echo "✅ 测试构建成功"
else
    echo "❌ 测试构建失败"
    exit 1
fi

echo ""
echo "🎉 所有目标构建成功！"
echo ""
echo "可用的命令："
echo "  - 运行网关服务器:   bazel run //src:hunterclaw-gateway"
echo "  - 运行测试:         bazel test //tests:test_process_integration"
echo "  - 构建所有目标:     bazel build //..."
echo "  - 运行所有测试:     bazel test //..."
echo ""
echo "📖 更多信息请查看 BUILD.bazel.md"
