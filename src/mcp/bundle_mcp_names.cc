// 🎯 对齐 OpenClaw: agents/pi-bundle-mcp-names.ts (完整实现)
// 工具名安全化：sanitizeServerName, buildSafeToolName

#include "hunterclaw/mcp/bundle_mcp_types.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace mcp {

// ============================================================================
// Constants (对齐 OpenClaw line 6-9)
// ============================================================================

static const int TOOL_NAME_MAX_PREFIX = 30;
static const int TOOL_NAME_MAX_TOTAL = 64;
static const std::regex TOOL_NAME_SAFE_RE("[^A-Za-z0-9_-]");

// ============================================================================
// sanitizeToolFragment (对齐 OpenClaw line 11-18)
// ============================================================================

static std::string sanitize_tool_fragment(const std::string& raw, const std::string& fallback,
                                          int max_chars = 0) {
    // trim
    std::string s = raw;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();

    // replace non-safe chars with -
    std::string cleaned = std::regex_replace(s, TOOL_NAME_SAFE_RE, "-");
    std::string normalized = cleaned.empty() ? fallback : cleaned;

    if (max_chars > 0 && (int)normalized.size() > max_chars) {
        normalized = normalized.substr(0, max_chars);
    }
    return normalized;
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ============================================================================
// sanitize_server_name (对齐 OpenClaw line 20-31)
// ============================================================================

std::string sanitize_server_name(const std::string& raw, std::set<std::string>& used_names) {
    std::string base = sanitize_tool_fragment(raw, "mcp", TOOL_NAME_MAX_PREFIX);
    std::string candidate = base;
    int n = 2;
    while (used_names.count(to_lower(candidate))) {
        std::string suffix = "-" + std::to_string(n);
        int max_len = std::max(1, TOOL_NAME_MAX_PREFIX - (int)suffix.size());
        candidate = base.substr(0, max_len) + suffix;
        n++;
    }
    used_names.insert(to_lower(candidate));
    return candidate;
}

// ============================================================================
// sanitize_tool_name (对齐 OpenClaw line 33-35)
// ============================================================================

std::string sanitize_tool_name(const std::string& raw) {
    return sanitize_tool_fragment(raw, "tool");
}

// ============================================================================
// normalize_reserved_tool_names (对齐 OpenClaw line 37-43)
// ============================================================================

std::set<std::string> normalize_reserved_tool_names(const std::vector<std::string>& names) {
    std::set<std::string> result;
    for (const auto& name : names) {
        std::string lower = to_lower(name);
        while (!lower.empty() && lower.front() == ' ') lower.erase(lower.begin());
        while (!lower.empty() && lower.back() == ' ') lower.pop_back();
        if (!lower.empty()) result.insert(lower);
    }
    return result;
}

// ============================================================================
// build_safe_tool_name (对齐 OpenClaw line 45-66)
// ============================================================================

std::string build_safe_tool_name(
    const std::string& server_name,
    const std::string& tool_name,
    std::set<std::string>& reserved_names) {

    std::string cleaned_tool = sanitize_tool_name(tool_name);
    int max_tool_chars = std::max(1,
        TOOL_NAME_MAX_TOTAL - (int)server_name.size() - (int)TOOL_NAME_SEPARATOR.size());
    std::string truncated = cleaned_tool.substr(0, max_tool_chars);
    if (truncated.empty()) truncated = "tool";

    std::string candidate_tool = truncated;
    std::string candidate = server_name + TOOL_NAME_SEPARATOR + candidate_tool;
    int n = 2;

    while (reserved_names.count(to_lower(candidate))) {
        std::string suffix = "-" + std::to_string(n);
        int max_len = std::max(1, max_tool_chars - (int)suffix.size());
        candidate_tool = (truncated.empty() ? "tool" : truncated).substr(0, max_len) + suffix;
        candidate = server_name + TOOL_NAME_SEPARATOR + candidate_tool;
        n++;
    }
    return candidate;
}

} // namespace mcp
} // namespace hunterclaw
