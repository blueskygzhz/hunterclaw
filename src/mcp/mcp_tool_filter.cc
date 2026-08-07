#include "hunterclaw/mcp/mcp_tool_filter.h"

// 🎯 严格对齐 OpenClaw src/agents/agent-bundle-mcp-filter.ts。

namespace hunterclaw {
namespace mcp {

namespace {

// trim 首尾空白（对齐 JS String.prototype.trim 的常见空白集）。
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

// 按 "*" 切分（对齐 JS "a*b*c".split("*") → ["a","b","c"]，
//   保留空段，含首尾空段）。
std::vector<std::string> split_star(const std::string& s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find('*', start);
        if (pos == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

} // namespace

bool matches_mcp_tool_filter_pattern(const std::string& pattern, const std::string& value) {
    const std::string trimmed = trim(pattern);
    if (trimmed.empty()) {
        return false;
    }
    // 不含 "*" → 精确相等。
    if (trimmed.find('*') == std::string::npos) {
        return trimmed == value;
    }

    const std::vector<std::string> parts = split_star(trimmed);
    // first = parts[0] ?? "", last = parts.at(-1) ?? ""
    const std::string first = parts.empty() ? "" : parts.front();
    const std::string last = parts.empty() ? "" : parts.back();

    size_t cursor = 0;
    if (!first.empty()) {
        // !value.startsWith(first) → false
        if (value.size() < first.size() ||
            value.compare(0, first.size(), first) != 0) {
            return false;
        }
        cursor = first.size();
    }

    // endBound = last ? value.length - last.length : value.length
    // 注意 last 非空且比 value 长时，value.length - last.length 会下溢；
    // 用有符号运算对齐 JS（endBound 可为负，随后 endBound < cursor 判定）。
    long long end_bound;
    if (!last.empty()) {
        end_bound = static_cast<long long>(value.size()) - static_cast<long long>(last.size());
    } else {
        end_bound = static_cast<long long>(value.size());
    }

    // if (last && (!value.endsWith(last) || endBound < cursor)) return false
    if (!last.empty()) {
        bool ends_with =
            value.size() >= last.size() &&
            value.compare(value.size() - last.size(), last.size(), last) == 0;
        if (!ends_with || end_bound < static_cast<long long>(cursor)) {
            return false;
        }
    }

    // for (const part of parts.slice(1, -1)) —— 中间段（跳过首尾）。
    if (parts.size() > 2) {
        for (size_t i = 1; i + 1 < parts.size(); ++i) {
            const std::string& part = parts[i];
            if (part.empty()) {
                continue;
            }
            // index = value.indexOf(part, cursor)
            size_t index = value.find(part, cursor);
            // if (index === -1 || index + part.length > endBound) return false
            if (index == std::string::npos ||
                static_cast<long long>(index + part.size()) > end_bound) {
                return false;
            }
            cursor = index + part.size();
        }
    }
    return true;
}

bool mcp_tool_filter_allows(
    const McpServerToolFilterConfig& filter,
    const std::string& tool_name) {
    // 对齐 serverAllowsUtilityTool 的 include/exclude 逻辑：
    //   include 非空 → 须命中其一；随后不得命中任一 exclude。
    if (!filter.include.empty()) {
        bool any = false;
        for (const auto& p : filter.include) {
            if (matches_mcp_tool_filter_pattern(p, tool_name)) { any = true; break; }
        }
        if (!any) return false;
    }
    for (const auto& p : filter.exclude) {
        if (matches_mcp_tool_filter_pattern(p, tool_name)) return false;
    }
    return true;
}

} // namespace mcp
} // namespace hunterclaw
