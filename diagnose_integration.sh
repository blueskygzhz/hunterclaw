#!/bin/bash

# HunterClaw Gateway & Channel 集成诊断脚本
# 快速检查代码状态，不需要编译

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo -e "${BLUE}"
cat << "EOF"
╔══════════════════════════════════════════════════════════╗
║  HunterClaw 集成诊断工具                                  ║
║  检查 Gateway 和 Channel 的代码集成状态                   ║
╚══════════════════════════════════════════════════════════╝
EOF
echo -e "${NC}"

# 统计变量
TOTAL_CHECKS=0
PASSED_CHECKS=0
FAILED_CHECKS=0
WARNING_CHECKS=0

check_pass() {
    echo -e "${GREEN}✓${NC} $1"
    ((PASSED_CHECKS++))
    ((TOTAL_CHECKS++))
}

check_fail() {
    echo -e "${RED}✗${NC} $1"
    ((FAILED_CHECKS++))
    ((TOTAL_CHECKS++))
}

check_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
    ((WARNING_CHECKS++))
    ((TOTAL_CHECKS++))
}

section() {
    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${CYAN}$1${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

# ============================================================================
# 1. 文件结构检查
# ============================================================================
section "1. 文件结构检查"

if [ -f "include/hunterclaw/gateway/gateway.h" ]; then
    check_pass "Gateway 头文件存在"
else
    check_fail "Gateway 头文件缺失"
fi

if [ -f "include/hunterclaw/gateway/server.h" ]; then
    check_pass "Gateway Server 头文件存在"
else
    check_fail "Gateway Server 头文件缺失"
fi

if [ -f "src/gateway/gateway.cc" ]; then
    check_pass "Gateway 实现文件存在"
else
    check_fail "Gateway 实现文件缺失"
fi

if [ -f "include/hunterclaw/channels/channel.h" ]; then
    check_pass "Channel 头文件存在"
else
    check_fail "Channel 头文件缺失"
fi

if [ -f "include/hunterclaw/channels/plugin.h" ]; then
    check_pass "Channel Plugin 头文件存在"
else
    check_fail "Channel Plugin 头文件缺失"
fi

if [ -f "src/channels/manager.cc" ]; then
    check_pass "Channel Manager 实现存在"
else
    check_fail "Channel Manager 实现缺失"
fi

if [ -f "src/channels/feishu_channel.cc" ]; then
    check_pass "Feishu Channel 实现存在"
else
    check_fail "Feishu Channel 实现缺失"
fi

# ============================================================================
# 2. 接口一致性检查
# ============================================================================
section "2. 接口一致性检查"

# 检查 ChannelAdapter 方法
if grep -q "virtual.*start_account" include/hunterclaw/channels/channel.h 2>/dev/null; then
    check_pass "ChannelAdapter::start_account 接口定义"
    
    if grep -q "adapter->start(" src/channels/manager.cc 2>/dev/null; then
        check_fail "ChannelManager 调用 start() 而非 start_account() - 接口不匹配"
    else
        check_pass "ChannelManager 调用接口匹配"
    fi
else
    check_warn "ChannelAdapter::start_account 未找到"
fi

# 检查 GatewayServer 构造函数
if grep -q "GatewayServerOptions" include/hunterclaw/gateway/server.h 2>/dev/null; then
    check_pass "GatewayServerOptions 结构定义"
    
    if grep -q "GatewayServerOptions" src/gateway/gateway.cc 2>/dev/null; then
        check_pass "Gateway 实现使用 GatewayServerOptions"
    else
        check_warn "Gateway 实现可能使用旧的 GatewayConfig"
    fi
else
    check_fail "GatewayServerOptions 结构缺失"
fi

# ============================================================================
# 3. TODO 和未实现功能检查
# ============================================================================
section "3. TODO 和未实现功能"

TODO_COUNT=$(grep -r "TODO" src/gateway/ src/channels/ 2>/dev/null | wc -l)
echo "  发现 $TODO_COUNT 个 TODO 标记"

if [ $TODO_COUNT -gt 0 ]; then
    echo ""
    echo "  关键 TODO:"
    grep -n "TODO" src/gateway/gateway.cc 2>/dev/null | head -5 | while read line; do
        echo "    - $line"
    done
    grep -n "TODO" src/channels/feishu_channel.cc 2>/dev/null | head -5 | while read line; do
        echo "    - $line"
    done
fi

# ============================================================================
# 4. 代码实现检查
# ============================================================================
section "4. 核心功能实现检查"

# Gateway RPC 方法
if grep -q "handle_request" src/gateway/gateway.cc 2>/dev/null; then
    check_pass "Gateway RPC 处理框架存在"
    
    RPC_METHODS=$(grep -c "req.method ==" src/gateway/gateway.cc 2>/dev/null || echo "0")
    echo "  已实现 RPC 方法数: $RPC_METHODS"
    
    if [ $RPC_METHODS -lt 3 ]; then
        check_warn "RPC 方法实现不足（推荐至少 5 个）"
    fi
else
    check_fail "Gateway RPC 处理未实现"
fi

# Channel 消息回调
if grep -q "on_message_received" src/channels/manager.cc 2>/dev/null; then
    check_pass "Channel 消息回调机制存在"
else
    check_fail "Channel 消息回调机制缺失"
fi

# Feishu WebSocket
if grep -q "run_websocket_mode" src/channels/feishu_channel.cc 2>/dev/null; then
    check_pass "Feishu WebSocket 模式框架存在"
    
    if grep -q "sleep_for" src/channels/feishu_channel.cc | grep -q "run_websocket_mode" 2>/dev/null; then
        check_warn "Feishu WebSocket 只有框架，未实际实现"
    fi
else
    check_fail "Feishu WebSocket 模式缺失"
fi

# 认证系统
if grep -q "authenticate" src/gateway/gateway.cc 2>/dev/null; then
    check_pass "Gateway 认证框架存在"
else
    check_warn "Gateway 认证功能未实现"
fi

# Session 管理
if grep -q "SessionManager" src/gateway/gateway.cc 2>/dev/null; then
    check_pass "Session 管理器存在"
else
    check_fail "Session 管理器缺失"
fi

# ============================================================================
# 5. 依赖库检查
# ============================================================================
section "5. 依赖库检查"

if grep -q "#include <websocketpp" src/gateway/gateway.cc 2>/dev/null; then
    check_pass "Gateway 使用 websocketpp"
else
    check_warn "Gateway 可能使用其他 WebSocket 库"
fi

if grep -q "#include <curl/curl.h>" src/channels/feishu_channel.cc 2>/dev/null; then
    check_pass "Feishu Channel 使用 libcurl"
else
    check_warn "Feishu Channel 未使用 HTTP 库"
fi

if grep -q "nlohmann/json" include/hunterclaw/ -r 2>/dev/null; then
    check_pass "项目使用 nlohmann/json"
else
    check_warn "可能使用其他 JSON 库"
fi

# ============================================================================
# 6. 测试文件检查
# ============================================================================
section "6. 测试文件检查"

if [ -f "tests/test_gateway_channel_unit.cc" ]; then
    check_pass "单元测试文件存在"
    
    TEST_COUNT=$(grep -c "TEST_F" tests/test_gateway_channel_unit.cc 2>/dev/null || echo "0")
    echo "  测试用例数: $TEST_COUNT"
else
    check_fail "单元测试文件缺失"
fi

if [ -f "examples/test_gateway_channel_integration.cc" ]; then
    check_pass "集成测试文件存在"
else
    check_warn "集成测试文件缺失"
fi

# ============================================================================
# 7. 编译配置检查
# ============================================================================
section "7. 编译配置检查"

if [ -f "CMakeLists.txt" ]; then
    check_pass "CMake 配置存在"
    
    if grep -q "gateway" CMakeLists.txt 2>/dev/null; then
        check_pass "CMake 包含 Gateway 模块"
    else
        check_warn "CMake 可能未包含 Gateway 模块"
    fi
    
    if grep -q "channels" CMakeLists.txt 2>/dev/null; then
        check_pass "CMake 包含 Channels 模块"
    else
        check_warn "CMake 可能未包含 Channels 模块"
    fi
else
    check_warn "根目录缺少 CMakeLists.txt"
fi

# ============================================================================
# 8. 代码质量检查
# ============================================================================
section "8. 代码质量检查"

# 计算代码行数
GW_LINES=$(wc -l src/gateway/*.cc 2>/dev/null | tail -1 | awk '{print $1}' || echo "0")
CH_LINES=$(wc -l src/channels/*.cc 2>/dev/null | tail -1 | awk '{print $1}' || echo "0")

echo "  Gateway 代码行数: $GW_LINES"
echo "  Channel 代码行数: $CH_LINES"

if [ $GW_LINES -gt 100 ]; then
    check_pass "Gateway 代码有实质内容"
else
    check_warn "Gateway 代码行数较少"
fi

if [ $CH_LINES -gt 100 ]; then
    check_pass "Channel 代码有实质内容"
else
    check_warn "Channel 代码行数较少"
fi

# 检查是否有明显的空实现
EMPTY_IMPL=$(grep -r "return.*unimplemented" src/ 2>/dev/null | wc -l)
if [ $EMPTY_IMPL -gt 0 ]; then
    check_warn "发现 $EMPTY_IMPL 个明确的未实现标记"
fi

# ============================================================================
# 总结
# ============================================================================
section "诊断总结"

echo ""
echo "  总检查项: $TOTAL_CHECKS"
echo -e "  ${GREEN}通过: $PASSED_CHECKS${NC}"
echo -e "  ${YELLOW}警告: $WARNING_CHECKS${NC}"
echo -e "  ${RED}失败: $FAILED_CHECKS${NC}"
echo ""

# 计算健康度
HEALTH_SCORE=$(( (PASSED_CHECKS * 100) / TOTAL_CHECKS ))

echo -e "  集成健康度: ${BLUE}$HEALTH_SCORE%${NC}"
echo ""

if [ $HEALTH_SCORE -ge 80 ]; then
    echo -e "${GREEN}✅ 集成状态良好，可以进行功能测试${NC}"
elif [ $HEALTH_SCORE -ge 60 ]; then
    echo -e "${YELLOW}⚠️  集成部分完成，需要修复关键问题${NC}"
else
    echo -e "${RED}❌ 集成状态不佳，需要大量工作${NC}"
fi

echo ""
echo -e "${BLUE}📚 详细报告: cat GATEWAY_CHANNEL_STATUS.md${NC}"
echo -e "${BLUE}🧪 运行测试: ./run_unit_tests.sh${NC}"
echo ""
