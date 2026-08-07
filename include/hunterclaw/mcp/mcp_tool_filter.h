#pragma once

// 🎯 严格对齐 OpenClaw src/agents/agent-bundle-mcp-filter.ts:
//   matchesMcpToolFilterPattern —— MCP tool-filter 的文档化 glob 语法：
//   精确文本 + "*" 通配符（非标准 fnmatch；无 "?"、无字符组、无转义）。
//
//   规则：
//     - pattern trim 后为空 → false
//     - 不含 "*" → 精确相等
//     - 含 "*" → 按 "*" 切分，首段须为前缀、尾段须为后缀（且不与前缀重叠），
//       中间各非空段按顺序在 [cursor, endBound) 内依次出现。

#include <string>
#include <vector>

namespace hunterclaw {
namespace mcp {

// 对齐 matchesMcpToolFilterPattern(pattern, value)。
bool matches_mcp_tool_filter_pattern(const std::string& pattern, const std::string& value);

// ============================================================================
// McpServerToolFilterConfig (对齐 OpenClaw config/types.mcp.ts:11-20)
// ============================================================================
struct McpServerToolFilterConfig {
    std::vector<std::string> include;  // 精确名或 "*" glob；空表示全部候选
    std::vector<std::string> exclude;  // 精确名或 "*" glob；命中则隐藏
    bool has_include = false;          // 区分"未配置"与"空数组"
    bool has_exclude = false;
};

// 便捷判定：给定 filter，tool 名是否应暴露（对齐 serverAllowsUtilityTool 的
//   include/exclude 逻辑：include 非空时须命中其一，且不得命中任一 exclude）。
bool mcp_tool_filter_allows(
    const McpServerToolFilterConfig& filter,
    const std::string& tool_name);

} // namespace mcp
} // namespace hunterclaw
