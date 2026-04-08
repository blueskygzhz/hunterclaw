// 🎯 严格对齐 OpenClaw: src/agents/sandbox/tool-policy.ts (110 lines)
// 沙箱工具策略解析与匹配：支持 glob 模式的 allow/deny 列表

#include <string>
#include <vector>
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace sandbox {

// Forward declarations from constants.cc
extern const std::vector<std::string> DEFAULT_TOOL_ALLOW;
extern const std::vector<std::string> DEFAULT_TOOL_DENY;

// ============================================================================
// Glob pattern matching - 对齐 OpenClaw compileGlobPatterns + matchesAnyGlobPattern
// ============================================================================

static std::string normalize_glob(const std::string& value) {
    std::string result = value;
    // Trim + lowercase
    size_t s = result.find_first_not_of(" \t");
    size_t e = result.find_last_not_of(" \t");
    if (s == std::string::npos) return "";
    result = result.substr(s, e - s + 1);
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static bool matches_glob(const std::string& value, const std::string& pattern) {
    // Simple glob: * matches anything
    if (pattern == "*") return true;
    if (pattern.find('*') == std::string::npos) {
        return value == pattern;
    }
    // Convert glob to regex
    std::string regex_str;
    for (char c : pattern) {
        if (c == '*') regex_str += ".*";
        else if (c == '?') regex_str += ".";
        else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']')
            regex_str += "\\" + std::string(1, c);
        else regex_str += c;
    }
    try {
        std::regex re(regex_str, std::regex::icase);
        return std::regex_match(value, re);
    } catch (...) {
        return value == pattern;
    }
}

static bool matches_any_glob(const std::string& value, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        if (matches_glob(value, normalize_glob(p))) return true;
    }
    return false;
}

// 🎯 对齐 OpenClaw: isToolAllowed
bool is_tool_allowed(
    const std::vector<std::string>& allow,
    const std::vector<std::string>& deny,
    const std::string& name
) {
    std::string normalized = normalize_glob(name);
    // Deny takes precedence
    if (matches_any_glob(normalized, deny)) return false;
    // Empty allow means allow all
    if (allow.empty()) return true;
    return matches_any_glob(normalized, allow);
}

// ============================================================================
// resolveSandboxToolPolicyForAgent
// 对齐 OpenClaw: resolveSandboxToolPolicyForAgent
// ============================================================================

struct SandboxToolPolicyResolved {
    std::vector<std::string> allow;
    std::vector<std::string> deny;
};

SandboxToolPolicyResolved resolve_sandbox_tool_policy_for_agent(
    const std::vector<std::string>& agent_allow,
    const std::vector<std::string>& agent_deny,
    const std::vector<std::string>& global_allow,
    const std::vector<std::string>& global_deny
) {
    SandboxToolPolicyResolved result;

    // 🎯 对齐 OpenClaw: agent > global > default 优先级
    result.deny = !agent_deny.empty() ? agent_deny :
                  (!global_deny.empty() ? global_deny : DEFAULT_TOOL_DENY);
    result.allow = !agent_allow.empty() ? agent_allow :
                   (!global_allow.empty() ? global_allow :
                    std::vector<std::string>(DEFAULT_TOOL_ALLOW.begin(), DEFAULT_TOOL_ALLOW.end()));

    // 🎯 对齐 OpenClaw: auto-inject "image" tool
    if (!result.allow.empty()) {
        bool has_image = false;
        bool deny_image = false;
        for (const auto& a : result.allow)
            if (normalize_glob(a) == "image") { has_image = true; break; }
        for (const auto& d : result.deny)
            if (normalize_glob(d) == "image") { deny_image = true; break; }
        if (!has_image && !deny_image) {
            result.allow.push_back("image");
        }
    }

    return result;
}

}  // namespace sandbox
}  // namespace hunterclaw
