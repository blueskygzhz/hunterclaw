// inherited_tool_deny.cc
// Aligned with openclaw: src/agents/inherited-tool-deny.ts
//                        src/agents/tool-policy-match.ts
//                        src/agents/glob-pattern.ts
//                        src/agents/tool-policy-shared.ts

#include "hunterclaw/agents/inherited_tool_deny.h"

#include <algorithm>
#include <regex>
#include <set>

namespace hunterclaw {
namespace agents {

namespace {

// ── ACP_UNSUPPORTED_INHERITED_TOOL_DENY ──────────────────────────────────────
// Aligned with openclaw: inherited-tool-deny.ts:8-20
// ACP 运行时依赖的宿主工具全集；其中任何一个被 deny 都会让 ACP 子会话跑不起来。
const std::vector<std::string>& acp_unsupported_inherited_tool_deny() {
    static const std::vector<std::string> kTools = {
        "apply_patch", "edit", "exec", "fs_delete", "fs_move",
        "fs_write",    "process", "read", "shell",  "spawn", "write",
    };
    return kTools;
}

// ── ACP_REQUIRED_INHERITED_TOOL_ALLOW ────────────────────────────────────────
// Aligned with openclaw: inherited-tool-deny.ts:25-32
//
// 真源注释：继承 allowlist 是按「有效工具面」重建的，ACP 专属别名（shell / spawn /
// fs_* 等）通常不会出现在其中，因此 allow 侧只校验这 6 个核心工具，
// 比 deny 侧的 11 个更窄。
const std::vector<std::string>& acp_required_inherited_tool_allow() {
    static const std::vector<std::string> kTools = {
        "apply_patch", "edit", "exec", "process", "read", "write",
    };
    return kTools;
}

// ── TOOL_NAME_ALIASES ────────────────────────────────────────────────────────
// Aligned with openclaw: tool-policy-shared.ts:19-24
const std::map<std::string, std::string>& tool_name_aliases() {
    static const std::map<std::string, std::string> kAliases = {
        {"bash",        "exec"},
        {"apply-patch", "apply_patch"},
        // 真源注释：永久性调度工具别名（owner decision, RFC 0026），同 bash -> exec
        {"cron",        "automations"},
    };
    return kAliases;
}

// ── TOOL_GROUPS ──────────────────────────────────────────────────────────────
// Aligned with openclaw: TOOL_GROUPS = { ...CORE_TOOL_GROUPS }
//   (tool-policy-shared.ts:32, tool-catalog.ts:508-526)
//
// ⚠ 真源的组表由 CORE_TOOL_DEFINITIONS 动态构建，key 形如
//   "group:openclaw" / "group:<sectionId>"。HunterClaw 尚无等价工具目录，
//   故此表为空 —— 效果是 "group:*" 条目不展开、原样参与匹配。
//   对不含组名的输入（本模块两道闸的实际场景）行为与真源一致。
const std::map<std::string, std::vector<std::string>>& tool_groups() {
    static const std::map<std::string, std::vector<std::string>> kGroups = {};
    return kGroups;
}

// Aligned with openclaw: normalizeLowercaseStringOrEmpty
std::string normalize_lowercase_or_empty(const std::string& value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    size_t end = value.find_last_not_of(" \t\r\n");
    std::string out = value.substr(start, end - start + 1);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Aligned with openclaw: uniqueStrings — 保序去重
std::vector<std::string> unique_strings(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& v : values) {
        if (seen.insert(v).second) out.push_back(v);
    }
    return out;
}

// ── CompiledGlobPattern ──────────────────────────────────────────────────────
// Aligned with openclaw: glob-pattern.ts:4-7
struct CompiledGlobPattern {
    enum class Kind { All, Exact, Regex };
    Kind kind = Kind::Exact;
    std::string value;   // Exact 用
    std::regex regex;    // Regex 用
};

// Aligned with openclaw: escapeRegex (glob-pattern.ts:9-12)
//   value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
std::string escape_regex(const std::string& value) {
    static const std::string kSpecial = ".*+?^${}()|[]\\";
    std::string out;
    out.reserve(value.size() * 2);
    for (char c : value) {
        if (kSpecial.find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

// Aligned with openclaw: compileGlobPattern (glob-pattern.ts:14-32)
CompiledGlobPattern compile_glob_pattern(const std::string& raw) {
    CompiledGlobPattern pattern;
    const std::string normalized = normalize_policy_tool_name(raw);
    if (normalized.empty()) {
        pattern.kind = CompiledGlobPattern::Kind::Exact;
        pattern.value = "";
        return pattern;
    }
    if (normalized == "*") {
        pattern.kind = CompiledGlobPattern::Kind::All;
        return pattern;
    }
    if (normalized.find('*') == std::string::npos) {
        pattern.kind = CompiledGlobPattern::Kind::Exact;
        pattern.value = normalized;
        return pattern;
    }
    // 真源：`^${escapeRegex(normalized).replaceAll("\\*", ".*")}$`
    // 即先整体转义（* → \*），再把 \* 换回 .*
    std::string escaped = escape_regex(normalized);
    std::string expanded;
    expanded.reserve(escaped.size() + 8);
    for (size_t i = 0; i < escaped.size(); ++i) {
        if (escaped[i] == '\\' && i + 1 < escaped.size() && escaped[i + 1] == '*') {
            expanded += ".*";
            ++i;
            continue;
        }
        expanded += escaped[i];
    }
    pattern.kind = CompiledGlobPattern::Kind::Regex;
    try {
        pattern.regex = std::regex("^" + expanded + "$");
    } catch (const std::regex_error&) {
        // 模式非法时退化为精确匹配，避免抛到调用方
        pattern.kind = CompiledGlobPattern::Kind::Exact;
        pattern.value = normalized;
    }
    return pattern;
}

// Aligned with openclaw: compileGlobPatterns (glob-pattern.ts:34-44)
//   过滤掉 kind === "exact" 且 value 为空的项
std::vector<CompiledGlobPattern> compile_glob_patterns(const std::vector<std::string>& raw) {
    std::vector<CompiledGlobPattern> out;
    out.reserve(raw.size());
    for (const auto& item : raw) {
        CompiledGlobPattern p = compile_glob_pattern(item);
        if (p.kind == CompiledGlobPattern::Kind::Exact && p.value.empty()) continue;
        out.push_back(std::move(p));
    }
    return out;
}

// Aligned with openclaw: matchesAnyGlobPattern (glob-pattern.ts:46-58)
bool matches_any_glob_pattern(const std::string& value,
                             const std::vector<CompiledGlobPattern>& patterns) {
    for (const auto& p : patterns) {
        switch (p.kind) {
            case CompiledGlobPattern::Kind::All:
                return true;
            case CompiledGlobPattern::Kind::Exact:
                if (value == p.value) return true;
                break;
            case CompiledGlobPattern::Kind::Regex:
                if (std::regex_match(value, p.regex)) return true;
                break;
        }
    }
    return false;
}

} // namespace

std::string normalize_policy_tool_name(const std::string& name) {
    const std::string normalized = normalize_lowercase_or_empty(name);
    const auto& aliases = tool_name_aliases();
    auto it = aliases.find(normalized);
    return it != aliases.end() ? it->second : normalized;
}

std::vector<std::string> expand_tool_groups(const std::vector<std::string>& list) {
    // Aligned with openclaw: normalizeToolList — 逐项归一并丢弃空串
    std::vector<std::string> normalized;
    normalized.reserve(list.size());
    for (const auto& item : list) {
        const std::string n = normalize_policy_tool_name(item);
        if (!n.empty()) normalized.push_back(n);
    }
    normalized = unique_strings(normalized);

    const auto& groups = tool_groups();
    std::vector<std::string> expanded;
    for (const auto& value : normalized) {
        auto it = groups.find(value);
        if (it != groups.end()) {
            expanded.insert(expanded.end(), it->second.begin(), it->second.end());
            continue;
        }
        expanded.push_back(value);
    }
    return unique_strings(expanded);
}

bool is_tool_allowed_by_policy_name(const std::string& name, const ToolPolicy& policy) {
    // Aligned with openclaw: makeToolPolicyMatcher (tool-policy-match.ts:10-38)
    const auto deny = compile_glob_patterns(
        expand_tool_groups(policy.deny.value_or(std::vector<std::string>{})));
    const auto allow = compile_glob_patterns(
        expand_tool_groups(policy.allow.value_or(std::vector<std::string>{})));

    const std::string normalized = normalize_policy_tool_name(name);

    // 1. deny 优先
    if (matches_any_glob_pattern(normalized, deny)) return false;
    // 2. 空 allow ⇒ 未被 deny 即放行
    if (allow.empty()) return true;
    // 3. 命中 allow
    if (matches_any_glob_pattern(normalized, allow)) return true;
    // 4. 真源特例：apply_patch 是具体的写工具，宽泛的 write allow 应当覆盖它
    if (normalized == "apply_patch" && matches_any_glob_pattern("write", allow)) return true;
    return false;
}

std::vector<std::string> normalize_inherited_tool_denylist(
    const std::vector<std::string>& value) {
    std::vector<std::string> mapped;
    mapped.reserve(value.size());
    for (const auto& entry : value) {
        const std::string n = normalize_policy_tool_name(entry);
        if (!n.empty()) mapped.push_back(n);
    }
    return unique_strings(mapped);
}

std::vector<std::string> normalize_inherited_tool_allowlist(
    const std::vector<std::string>& value) {
    // Aligned with openclaw: normalizeInheritedToolAllowlist 直接复用 deny 版实现
    return normalize_inherited_tool_denylist(value);
}

std::optional<std::string> find_acp_unsupported_inherited_tool_deny(
    const std::vector<std::string>& inherited_deny) {
    const auto normalized = normalize_inherited_tool_denylist(inherited_deny);
    if (normalized.empty()) return std::nullopt;

    ToolPolicy policy;
    policy.deny = normalized;
    for (const auto& tool_name : acp_unsupported_inherited_tool_deny()) {
        if (!is_tool_allowed_by_policy_name(tool_name, policy)) return tool_name;
    }
    return std::nullopt;
}

std::optional<std::string> find_acp_unsupported_inherited_tool_allow(
    const std::vector<std::string>& inherited_allow) {
    const auto normalized = normalize_inherited_tool_allowlist(inherited_allow);
    if (normalized.empty()) return std::nullopt;

    ToolPolicy policy;
    policy.allow = normalized;
    for (const auto& tool_name : acp_required_inherited_tool_allow()) {
        if (!is_tool_allowed_by_policy_name(tool_name, policy)) return tool_name;
    }
    return std::nullopt;
}

std::string format_acp_inherited_tool_deny_error(const std::string& tool_name) {
    return "runtime=\"acp\" is unavailable because the requester denies " + tool_name +
           ". Use runtime=\"subagent\".";
}

std::string format_acp_inherited_tool_allow_error(const std::string& tool_name) {
    return "runtime=\"acp\" is unavailable because the requester does not allow " + tool_name +
           ". Use runtime=\"subagent\".";
}

} // namespace agents
} // namespace hunterclaw
