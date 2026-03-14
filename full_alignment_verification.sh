#!/bin/bash
# full_alignment_verification.sh
# HunterClaw vs OpenClaw 完整对齐验证脚本

echo "=========================================="
echo "🎯 HunterClaw vs OpenClaw 对齐验证"
echo "=========================================="
echo ""

# 计数器
total_checks=0
passed_checks=0

# 1. 目录结构验证
echo "📁 1. 目录结构验证..."
required_dirs=(
    "src/agents"
    "src/tools"
    "src/mcp"
    "src/skills"
    "src/sandbox"
    "src/llm"
    "src/memory"
    "src/routing"
    "src/sessions"
    "src/gateway"
    "src/channels"
    "src/plugins"
    "src/cron"
    "src/react"
    "include/hunterclaw"
)

for dir in "${required_dirs[@]}"; do
    ((total_checks++))
    if [ -d "$dir" ]; then
        echo "  ✅ $dir"
        ((passed_checks++))
    else
        echo "  ❌ $dir missing"
    fi
done
echo ""

# 2. MCP API 验证
echo "🔌 2. MCP API 验证..."
mcp_apis=(
    "enable_mcp"
    "refresh_mcp_tools"
    "list_servers"
    "list_tools"
    "call_tool"
)

for api in "${mcp_apis[@]}"; do
    ((total_checks++))
    if grep -rq "$api" src/mcp/ 2>/dev/null; then
        echo "  ✅ $api"
        ((passed_checks++))
    else
        echo "  ❌ $api missing"
    fi
done
echo ""

# 3. 内置工具验证
echo "🛠️  3. 内置工具验证..."
builtin_tools=(
    "read_file"
    "write_to_file"
    "replace_in_file"
    "execute_command"
    "web_fetch"
    "web_search"
    "task"
    "send_message"
)

for tool in "${builtin_tools[@]}"; do
    ((total_checks++))
    if grep -rq "$tool" src/tools/ 2>/dev/null; then
        echo "  ✅ $tool"
        ((passed_checks++))
    else
        echo "  ❌ $tool missing"
    fi
done
echo ""

# 4. 关键代码检查
echo "💻 4. 关键代码检查..."

# 4.1 MCP Daemon 单例
((total_checks++))
if grep -q "struct DaemonState" src/mcp/mcporter_client.cc 2>/dev/null; then
    echo "  ✅ MCP Daemon 单例"
    ((passed_checks++))
else
    echo "  ❌ MCP Daemon 单例 missing"
fi

# 4.2 命名空间隔离
((total_checks++))
if grep -q 'server_name + "." + tool_name' src/mcp/mcporter_client.cc 2>/dev/null; then
    echo "  ✅ 命名空间隔离"
    ((passed_checks++))
else
    echo "  ❌ 命名空间隔离 missing"
fi

# 4.3 工具注册
((total_checks++))
if grep -q "tools_\[tool->name()\]" src/tools/tool_manager.cc 2>/dev/null; then
    echo "  ✅ 工具注册机制"
    ((passed_checks++))
else
    echo "  ❌ 工具注册机制 missing"
fi

# 4.4 循环检测
((total_checks++))
if grep -q "window_size_" src/tools/tool_loop_detector.cc 2>/dev/null; then
    echo "  ✅ 循环检测算法"
    ((passed_checks++))
else
    echo "  ❌ 循环检测算法 missing"
fi

# 4.5 审批机制
((total_checks++))
if grep -q "DANGEROUS_KEYWORDS" src/tools/approval.cc 2>/dev/null; then
    echo "  ✅ 审批机制"
    ((passed_checks++))
else
    echo "  ❌ 审批机制 missing"
fi

# 4.6 钩子系统
((total_checks++))
if grep -q "register_pre_hook" include/hunterclaw/tools/tool_hooks.h 2>/dev/null; then
    echo "  ✅ 钩子系统"
    ((passed_checks++))
else
    echo "  ❌ 钩子系统 missing"
fi

echo ""

# 5. Skill 格式验证
echo "📚 5. Skill 格式验证..."
((total_checks++))
if [ -d "skills" ]; then
    skill_count=0
    for skill in skills/*/; do
        if [ -f "$skill/SKILL.md" ]; then
            ((skill_count++))
        fi
    done
    if [ $skill_count -gt 0 ]; then
        echo "  ✅ 找到 $skill_count 个 Skills"
        ((passed_checks++))
    else
        echo "  ⚠️  skills/ 目录存在但无 SKILL.md"
    fi
else
    echo "  ❌ skills/ 目录不存在"
fi
echo ""

# 6. 头文件验证
echo "📄 6. 头文件验证..."
key_headers=(
    "include/hunterclaw/tools/tool.h"
    "include/hunterclaw/tools/tool_manager.h"
    "include/hunterclaw/mcp/mcporter_client.h"
    "include/hunterclaw/mcp/mcp_tool_adapter.h"
    "include/hunterclaw/skills/skill_loader.h"
)

for header in "${key_headers[@]}"; do
    ((total_checks++))
    if [ -f "$header" ]; then
        echo "  ✅ $(basename $header)"
        ((passed_checks++))
    else
        echo "  ❌ $(basename $header) missing"
    fi
done
echo ""

# 7. 文档验证
echo "📖 7. 文档验证..."
key_docs=(
    "docs/MCP_STRICT_ALIGNMENT_AUDIT.md"
    "docs/DIRECTORY_ALIGNMENT_OPENCLAW.md"
    "docs/FEATURE_ALIGNMENT_MATRIX.md"
    "docs/CODE_ALIGNMENT_COMPARISON.md"
)

for doc in "${key_docs[@]}"; do
    ((total_checks++))
    if [ -f "$doc" ]; then
        echo "  ✅ $(basename $doc)"
        ((passed_checks++))
    else
        echo "  ❌ $(basename $doc) missing"
    fi
done
echo ""

# 计算对齐度
alignment_percentage=$((passed_checks * 100 / total_checks))

echo "=========================================="
echo "📊 验证结果统计"
echo "=========================================="
echo "通过检查: $passed_checks / $total_checks"
echo "对齐度: $alignment_percentage%"
echo ""

if [ $alignment_percentage -ge 95 ]; then
    echo "✅ 对齐度优秀! HunterClaw 与 OpenClaw 完美对齐 🚀"
    exit 0
elif [ $alignment_percentage -ge 80 ]; then
    echo "⚠️  对齐度良好,但需要改进部分项目"
    exit 1
else
    echo "❌ 对齐度不足,需要大幅改进"
    exit 2
fi
