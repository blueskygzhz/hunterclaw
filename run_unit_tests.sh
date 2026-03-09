#!/bin/bash

# HunterClaw Gateway & Channel 单元测试运行脚本

set -e  # 遇到错误立即退出

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}"
echo "========================================"
echo "  HunterClaw 单元测试套件"
echo "  Gateway & Channel 集成测试"
echo "========================================"
echo -e "${NC}"

# 检查依赖
echo -e "${YELLOW}[1/5] 检查依赖...${NC}"

if ! command -v g++ &> /dev/null; then
    echo -e "${RED}✗ g++ 未安装${NC}"
    exit 1
fi

if ! command -v cmake &> /dev/null; then
    echo -e "${RED}✗ cmake 未安装${NC}"
    exit 1
fi

echo -e "${GREEN}✓ 编译器和构建工具就绪${NC}"

# 检查 Google Test
echo -e "${YELLOW}[2/5] 检查 Google Test...${NC}"

if ! pkg-config --exists gtest 2>/dev/null; then
    echo -e "${YELLOW}⚠ Google Test 未安装，尝试安装...${NC}"
    echo "  Ubuntu/Debian: sudo apt-get install libgtest-dev"
    echo "  Arch: sudo pacman -S gtest"
    echo "  macOS: brew install googletest"
    echo ""
    echo -e "${YELLOW}继续使用源码编译...${NC}"
fi

# 创建构建目录
echo -e "${YELLOW}[3/5] 准备构建环境...${NC}"

BUILD_DIR="build_tests"
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# 配置 CMake（简化版，不依赖完整项目）
echo -e "${YELLOW}[4/5] 编译测试...${NC}"

# 直接编译测试（不依赖 CMake）
echo "  使用直接编译模式..."

cd ..

g++ -std=c++17 -g -O0 \
    tests/test_gateway_channel_unit.cc \
    -Iinclude \
    -I/usr/include \
    -pthread \
    -lgtest -lgtest_main \
    -o $BUILD_DIR/test_gateway_channel_unit \
    2>&1 | tee $BUILD_DIR/compile.log

if [ ! -f $BUILD_DIR/test_gateway_channel_unit ]; then
    echo -e "${RED}✗ 编译失败，查看日志: $BUILD_DIR/compile.log${NC}"
    echo ""
    echo "常见问题:"
    echo "  1. Google Test 未安装: sudo apt-get install libgtest-dev googletest"
    echo "  2. 缺少依赖头文件"
    echo "  3. C++17 不支持: 升级 g++"
    exit 1
fi

echo -e "${GREEN}✓ 编译成功${NC}"
echo ""

# 运行测试
echo -e "${YELLOW}[5/5] 运行测试...${NC}"
echo ""
echo "========================================"
echo ""

./$BUILD_DIR/test_gateway_channel_unit --gtest_output=xml:$BUILD_DIR/test_results.xml || {
    TEST_EXIT_CODE=$?
    echo ""
    echo -e "${RED}========================================"
    echo "  测试失败 (退出码: $TEST_EXIT_CODE)"
    echo "========================================${NC}"
    exit $TEST_EXIT_CODE
}

echo ""
echo -e "${GREEN}========================================"
echo "  ✅ 所有测试通过！"
echo "========================================${NC}"
echo ""

# 生成报告
echo -e "${BLUE}📊 测试报告:${NC}"
echo "  详细报告: $BUILD_DIR/test_results.xml"
echo "  编译日志: $BUILD_DIR/compile.log"
echo ""

# 统计信息
if [ -f $BUILD_DIR/test_results.xml ]; then
    TOTAL_TESTS=$(grep -o 'tests="[0-9]*"' $BUILD_DIR/test_results.xml | head -1 | grep -o '[0-9]*')
    FAILED_TESTS=$(grep -o 'failures="[0-9]*"' $BUILD_DIR/test_results.xml | head -1 | grep -o '[0-9]*')
    
    if [ -n "$TOTAL_TESTS" ]; then
        echo -e "${BLUE}测试统计:${NC}"
        echo "  总测试数: $TOTAL_TESTS"
        echo "  失败: $FAILED_TESTS"
        echo "  通过: $((TOTAL_TESTS - FAILED_TESTS))"
    fi
fi

echo ""
echo -e "${BLUE}💡 提示:${NC}"
echo "  查看详细状态: cat GATEWAY_CHANNEL_STATUS.md"
echo "  运行集成测试: ./test_integration.sh"
echo ""
