#pragma once

// 严格对齐 OpenClaw auto-reply/tool-meta.ts (144行)
// 工具元数据格式化: emoji前缀 + 路径分组 + brace折叠

#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace auto_reply {

/// Shorten a home directory path (~/ prefix).
std::string shorten_path(const std::string& path);

/// Shorten home paths within a meta string.
std::string shorten_meta(const std::string& meta);

/// Format a tool aggregate display line.
/// 对齐 formatToolAggregate
std::string format_tool_aggregate(
    const std::optional<std::string>& tool_name = std::nullopt,
    const std::vector<std::string>& metas = {},
    bool markdown = false
);

/// Format a tool prefix (emoji + label + optional meta).
/// 对齐 formatToolPrefix
std::string format_tool_prefix(
    const std::optional<std::string>& tool_name = std::nullopt,
    const std::optional<std::string>& meta = std::nullopt
);

}  // namespace auto_reply
}  // namespace hunterclaw
