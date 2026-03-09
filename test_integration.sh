#!/bin/bash

set -e  # 遇到错误立即退出

echo "=========================================="
echo "  HunterClaw Gateway & Channel"
echo "  集成测试编译和运行脚本"
echo "=========================================="
echo ""

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# 检查必要的依赖
echo "📦 检查依赖..."

if ! command -v g++ &> /dev/null; then
    echo -e "${RED}✗ g++ 未安装${NC}"
    exit 1
fi

if ! command -v pkg-config &> /dev/null; then
    echo -e "${YELLOW}⚠ pkg-config 未安装，将尝试手动配置${NC}"
fi

echo -e "${GREEN}✓ 编译器检查通过${NC}"
echo ""

# 创建构建目录
echo "🏗️  准备构建..."
mkdir -p build
mkdir -p bin

# 编译集成测试
echo "🔨 编译 Gateway & Channel 集成测试..."

g++ -std=c++17 \
    examples/test_gateway_channel_integration.cc \
    src/gateway/gateway.cc \
    src/gateway/protocol.cc \
    src/channels/channel.cc \
    src/channels/feishu_channel.cc \
    src/core/config.cc \
    src/core/utils.cc \
    -Iinclude \
    -pthread \
    -lcurl \
    -lwebsocketpp \
    -lboost_system \
    -o bin/test_gateway_channel_integration \
    2>&1 | tee build/integration_test.log || {
        echo -e "${RED}✗ 编译失败，查看日志: build/integration_test.log${NC}"
        exit 1
    }

echo -e "${GREEN}✓ 编译成功${NC}"
echo ""

# 检查生成的可执行文件
if [ -f bin/test_gateway_channel_integration ]; then
    echo "📊 可执行文件信息:"
    ls -lh bin/test_gateway_channel_integration
    echo ""
else
    echo -e "${RED}✗ 可执行文件未生成${NC}"
    exit 1
fi

# 运行测试
echo "=========================================="
echo "  运行集成测试"
echo "=========================================="
echo ""

./bin/test_gateway_channel_integration

echo ""
echo "=========================================="
echo "  测试完成"
echo "=========================================="
