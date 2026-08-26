// ============================================================================
// acp_client.cc
// 🎯 严格对齐 OpenClaw:
//   - src/acp/client.ts (ClientSideConnection, spawn, permission resolver)
//   - src/acp/approval-classifier.ts (ACP 审批分类器)
//   - src/acp/policy.ts (ACP 策略控制)
//   - src/acp/session-interaction-mode.ts (会话交互模式)
// ============================================================================

#include "hunterclaw/acp/acp_client.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/infra/utilities.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace acp {

// ============================================================================
// 🎯 严格对齐 approval-classifier.ts
// ACP Approval Classifier — 工具审批分类
// ============================================================================

// 🎯 対齐 approval-classifier.ts:14-31 —— 三个集合逐字对齐，多一个少一个都是安全洞
static const std::set<std::string> SAFE_SEARCH_TOOL_IDS = {
    "search", "web_search", "memory_search"
};
static const std::set<std::string> TRUSTED_SAFE_TOOL_ALIASES = {"search"};
// 対齐:16-24 —— 注意含 "nodes"（此前漏），不含 "run"/"terminal"（此前多加）
static const std::set<std::string> EXEC_CAPABLE_TOOL_IDS = {
    "exec", "spawn", "shell", "bash", "process", "code_execution", "nodes"
};
// 対齐:25-31 —— 注意含 "cron" 与 "gateway"（此前两个都漏）
static const std::set<std::string> CONTROL_PLANE_TOOL_IDS = {
    "cron", "gateway", "sessions_spawn", "sessions_send", "session_status"
};

namespace {

std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim_copy(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

/// 対齐 utils/string-readers.ts readTrimmedStringAlias —— 按 keys 顺序取第一个
/// 非空字符串值。注意是**顺序敏感**的：真源用它表达优先级。
std::string read_first_string(const nlohmann::json& obj,
                              const std::vector<std::string>& keys) {
    if (!obj.is_object()) return std::string();
    for (const auto& k : keys) {
        if (obj.contains(k) && obj[k].is_string()) {
            const std::string v = trim_copy(obj[k].get<std::string>());
            if (!v.empty()) return v;
        }
    }
    return std::string();
}

/// 対齐 approval-classifier.ts:74-80 parseToolNameFromTitle
/// title 形如 "read: /path/to/file"，取冒号前的头部作为工具名。
std::string parse_tool_name_from_title(const nlohmann::json& tool_call) {
    if (!tool_call.is_object() || !tool_call.contains("title") ||
        !tool_call["title"].is_string()) {
        return std::string();
    }
    const std::string title = tool_call["title"].get<std::string>();
    // 対齐:78 —— title.split(":", 1)[0]，即第一个冒号之前
    const size_t colon = title.find(':');
    const std::string head = trim_copy(colon == std::string::npos
                                           ? title
                                           : title.substr(0, colon));
    return head.empty() ? std::string() : normalize_tool_name(head);
}

/// 対齐 approval-classifier.ts:114-137 extractPathFromToolTitle
std::string extract_path_from_tool_title(const std::string& tool_title,
                                        const std::string& tool_name) {
    if (tool_title.empty()) return std::string();
    const size_t sep = tool_title.find(':');
    if (sep == std::string::npos) return std::string();      // 対齐:121-123
    const std::string tail = trim_copy(tool_title.substr(sep + 1));
    if (tail.empty()) return std::string();                   // 対齐:125-128

    // 対齐:129-135 —— read 允许 "path:" 出现在任意逗号段之后，
    // 其它工具只认开头的 "path:"。这个区别不能抹平：read 的 title 常带
    // 多个键值段，而其它工具若允许中段匹配会被构造出的 title 骗过。
    auto find_keyed = [&](bool anywhere) -> std::string {
        static const char* kKeys[] = {"path", "file_path", "filePath"};
        for (const char* key : kKeys) {
            const std::string pat = std::string(key);
            size_t pos = std::string::npos;
            if (anywhere) {
                // 対齐 /(?:^|,\s*)(?:path|file_path|filePath)\s*:\s*([^,]+)/
                size_t search = 0;
                while (search <= tail.size()) {
                    size_t at = tail.find(pat, search);
                    if (at == std::string::npos) break;
                    bool boundary_ok = (at == 0);
                    if (!boundary_ok) {
                        // 前面须是 ',' 后跟可选空白
                        size_t back = at;
                        while (back > 0 && (tail[back - 1] == ' ' || tail[back - 1] == '\t')) --back;
                        boundary_ok = (back > 0 && tail[back - 1] == ',');
                    }
                    if (boundary_ok) { pos = at; break; }
                    search = at + 1;
                }
            } else {
                // 対齐 /^(?:path|file_path|filePath)\s*:\s*([^,]+)/
                if (tail.compare(0, pat.size(), pat) == 0) pos = 0;
            }
            if (pos == std::string::npos) continue;
            size_t after = pos + pat.size();
            while (after < tail.size() && (tail[after] == ' ' || tail[after] == '\t')) ++after;
            if (after >= tail.size() || tail[after] != ':') continue;
            ++after;
            while (after < tail.size() && (tail[after] == ' ' || tail[after] == '\t')) ++after;
            const size_t comma = tail.find(',', after);
            const std::string val = trim_copy(comma == std::string::npos
                                                  ? tail.substr(after)
                                                  : tail.substr(after, comma - after));
            if (!val.empty()) return val;
        }
        return std::string();
    };

    const std::string keyed = find_keyed(/*anywhere=*/tool_name == "read");
    if (!keyed.empty()) return keyed;
    // 対齐:136 —— 只有 read 允许把整个 tail 当路径
    return tool_name == "read" ? tail : std::string();
}

/// 対齐 approval-classifier.ts:139-151 readLocationPaths
std::vector<std::string> read_location_paths(const nlohmann::json& tool_call) {
    std::vector<std::string> out;
    if (!tool_call.is_object() || !tool_call.contains("locations") ||
        !tool_call["locations"].is_array()) {
        return out;
    }
    for (const auto& loc : tool_call["locations"]) {
        const std::string p = read_first_string(loc, {"path", "file_path", "filePath"});
        if (!p.empty()) out.push_back(p);
    }
    return out;
}

/// 対齐 approval-classifier.ts:153-165 resolveToolPathCandidates
std::vector<std::string> resolve_tool_path_candidates(const nlohmann::json& tool_call,
                                                     const std::string& tool_name,
                                                     bool include_locations) {
    std::vector<std::string> out;
    const nlohmann::json raw_input =
        (tool_call.is_object() && tool_call.contains("rawInput") &&
         tool_call["rawInput"].is_object())
            ? tool_call["rawInput"] : nlohmann::json::object();

    const std::string from_raw = read_first_string(raw_input, {"path", "file_path", "filePath"});
    if (!from_raw.empty()) out.push_back(from_raw);

    std::string title;
    if (tool_call.is_object() && tool_call.contains("title") && tool_call["title"].is_string()) {
        title = tool_call["title"].get<std::string>();
    }
    const std::string from_title = extract_path_from_tool_title(title, tool_name);
    if (!from_title.empty()) out.push_back(from_title);

    if (include_locations) {
        for (auto& p : read_location_paths(tool_call)) out.push_back(std::move(p));
    }
    return out;
}

/// 対齐 approval-classifier.ts:167-186 resolveAbsoluteScopedPath
std::string resolve_absolute_scoped_path(const std::string& value, const std::string& cwd) {
    std::string candidate = trim_copy(value);
    if (candidate.empty()) return std::string();

    // 対齐:172-179 —— file:// URL 需解码 pathname
    if (candidate.rfind("file://", 0) == 0) {
        candidate = candidate.substr(7);
        // 去掉可能的 host 段（file://host/path → /path）
        if (!candidate.empty() && candidate[0] != '/') {
            const size_t slash = candidate.find('/');
            if (slash == std::string::npos) return std::string();
            candidate = candidate.substr(slash);
        }
        // percent-decode
        std::string decoded;
        for (size_t i = 0; i < candidate.size(); ++i) {
            if (candidate[i] == '%' && i + 2 < candidate.size()) {
                const std::string hex = candidate.substr(i + 1, 2);
                char* end = nullptr;
                const long v = std::strtol(hex.c_str(), &end, 16);
                if (end && *end == '\0') {
                    decoded += static_cast<char>(v);
                    i += 2;
                    continue;
                }
            }
            decoded += candidate[i];
        }
        candidate = decoded;
    }

    // 対齐:180-184 —— ~ 与 ~/ 展开
    const char* home = std::getenv("HOME");
    const std::string home_dir = home ? home : "";
    if (candidate == "~") {
        candidate = home_dir;
    } else if (candidate.rfind("~/", 0) == 0 && !home_dir.empty()) {
        candidate = (std::filesystem::path(home_dir) / candidate.substr(2)).string();
    }
    if (candidate.empty()) return std::string();

    // 対齐:185 —— 绝对路径归一化，相对路径按 cwd 解析
    try {
        std::filesystem::path p(candidate);
        if (p.is_absolute()) return p.lexically_normal().string();
        return (std::filesystem::path(cwd) / p).lexically_normal().string();
    } catch (...) {
        return std::string();
    }
}

}  // namespace

std::string normalize_tool_name(const std::string& value) {
    // 対齐 approval-classifier.ts:66-72
    const std::string normalized = to_lower_copy(trim_copy(value));
    if (normalized.empty() || normalized.size() > 128) return std::string();
    // 対齐:71 —— /^[a-z0-9._-]+$/
    for (unsigned char c : normalized) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) return std::string();
    }
    return normalized;
}

bool is_tool_path_scoped_to_cwd(const std::string& raw_path, const std::string& cwd) {
    // 対齐 approval-classifier.ts:188-194
    const std::string abs = resolve_absolute_scoped_path(raw_path, cwd);
    if (abs.empty()) return false;   // 対齐:190-192 —— 解析失败视为不在范围内
    std::string root;
    try {
        root = std::filesystem::path(cwd).lexically_normal().string();
    } catch (...) {
        return false;
    }
    return infra::is_path_inside(root, abs);
}

std::string resolve_tool_name_for_permission(const nlohmann::json& tool_call) {
    // 対齐 approval-classifier.ts:82-112 —— 三源解析 + 交叉校验。
    //
    // ⚠ 这是**防伪造**的核心闸门。此前的实现直接接受调用方传入的 tool_name，
    // 等于让对端自报身份：把 title 写成 "read: /etc/shadow" 而 _meta.toolName
    // 写成 "exec"，就能以 read 的宽松策略执行 exec。真源的做法是任意两源
    // 冲突就返回 undefined（不自动批准），fail closed。
    const nlohmann::json meta =
        (tool_call.is_object() && tool_call.contains("_meta") &&
         tool_call["_meta"].is_object()) ? tool_call["_meta"] : nlohmann::json::object();
    const nlohmann::json raw_input =
        (tool_call.is_object() && tool_call.contains("rawInput") &&
         tool_call["rawInput"].is_object()) ? tool_call["rawInput"] : nlohmann::json::object();

    // 対齐:93-95 —— 三源各自的键别名顺序不同，须逐字照抄
    const std::string from_meta = read_first_string(meta, {"toolName", "tool_name", "name"});
    const std::string from_raw_input =
        read_first_string(raw_input, {"tool", "toolName", "tool_name", "name"});
    const std::string title_name = parse_tool_name_from_title(tool_call);

    const std::string meta_name = from_meta.empty() ? std::string()
                                                    : normalize_tool_name(from_meta);
    const std::string raw_input_name = from_raw_input.empty()
                                           ? std::string()
                                           : normalize_tool_name(from_raw_input);

    // 対齐:99-101 —— 源里有值但格式非法 → 直接拒绝（不能忽略后继续）
    if ((!from_meta.empty() && meta_name.empty()) ||
        (!from_raw_input.empty() && raw_input_name.empty())) {
        return std::string();
    }
    // 対齐:102-110 —— 三组两两冲突检查
    if (!meta_name.empty() && !title_name.empty() && meta_name != title_name) {
        return std::string();
    }
    if (!raw_input_name.empty() && !meta_name.empty() && raw_input_name != meta_name) {
        return std::string();
    }
    if (!raw_input_name.empty() && !title_name.empty() && raw_input_name != title_name) {
        return std::string();
    }
    // 対齐:111 —— 优先级 meta > title > rawInput
    if (!meta_name.empty()) return meta_name;
    if (!title_name.empty()) return title_name;
    return raw_input_name;
}

AcpApprovalClassification classify_acp_tool_approval(
    const nlohmann::json& tool_call,
    const std::string& cwd) {

    AcpApprovalClassification result;
    result.approval_class = AcpApprovalClass::Unknown;
    result.auto_approve = false;

    // 対齐:201-204 —— 名字解析不出来就是 unknown，绝不自动批准
    const std::string tool_name = resolve_tool_name_for_permission(tool_call);
    if (tool_name.empty()) return result;
    result.tool_name = tool_name;

    // 対齐:206 —— isKnownCoreToolId(toolName) || TRUSTED_SAFE_TOOL_ALIASES.has(...)
    // HunterClaw 无 tool-catalog 的核心工具 id 判定接口，此处用
    // "已知核心工具名集合" 近似。宁可判为 not trusted（走人工确认），
    // 不可反向放宽。
    static const std::set<std::string> kKnownCoreToolIds = {
        "read", "write", "edit", "search", "exec", "web_search", "web_fetch",
        "memory_search", "sessions_spawn", "sessions_send", "session_status",
        "cron", "gateway", "nodes", "spawn", "shell", "bash", "process",
        "code_execution",
    };
    const bool is_trusted_tool_id =
        kKnownCoreToolIds.count(tool_name) > 0 || TRUSTED_SAFE_TOOL_ALIASES.count(tool_name) > 0;

    // 対齐:207-222 —— read 且可信：**必须**路径全部落在 cwd 内才自动批准。
    // 此前实现无条件把 read/read_file/list_dir/find/grep/list_files 全部
    // auto_approve=true，等于允许对端读取任意路径（/etc/shadow、~/.ssh/id_rsa）。
    if (tool_name == "read" && is_trusted_tool_id) {
        const std::vector<std::string> raw_paths =
            resolve_tool_path_candidates(tool_call, tool_name, /*include_locations=*/false);
        bool all_scoped = !raw_paths.empty();
        for (const auto& p : raw_paths) {
            if (!is_tool_path_scoped_to_cwd(p, cwd)) { all_scoped = false; break; }
        }
        result.approval_class = all_scoped ? AcpApprovalClass::ReadonlyScoped
                                           : AcpApprovalClass::Other;
        result.auto_approve = all_scoped;
        return result;
    }

    // 対齐:223-234 —— 安全搜索类：**任一**路径越界即降级为 other。
    // 注意与 read 的差别：这里 include_locations=true，且判定是
    // "some 越界 → 拒绝"，而非 "every 合规 → 批准" ——
    // 无路径时搜索仍可自动批准（対齐:233）。
    if (SAFE_SEARCH_TOOL_IDS.count(tool_name) && is_trusted_tool_id) {
        const std::vector<std::string> raw_paths =
            resolve_tool_path_candidates(tool_call, tool_name, /*include_locations=*/true);
        for (const auto& p : raw_paths) {
            if (!is_tool_path_scoped_to_cwd(p, cwd)) {
                result.approval_class = AcpApprovalClass::Other;
                result.auto_approve = false;
                return result;
            }
        }
        result.approval_class = AcpApprovalClass::ReadonlySearch;
        result.auto_approve = true;
        return result;
    }

    // 対齐:235-237
    if (EXEC_CAPABLE_TOOL_IDS.count(tool_name)) {
        result.approval_class = AcpApprovalClass::ExecCapable;
        result.auto_approve = false;
        return result;
    }
    // 対齐:238-240 —— ⚠ auto_approve **必须为 false**。
    // 此前写成 true，意味着 sessions_spawn / sessions_send / cron / gateway
    // 这些控制面工具被静默放行 —— 对端可无声地拉起新会话、改配置、投递消息。
    if (CONTROL_PLANE_TOOL_IDS.count(tool_name)) {
        result.approval_class = AcpApprovalClass::ControlPlane;
        result.auto_approve = false;
        return result;
    }
    // 対齐:241-243 —— isMutatingToolCall(toolName, rawInput)
    // HunterClaw 无该函数，用名字前缀/白名单近似（覆盖真源同类工具）。
    static const std::set<std::string> kMutatingToolIds = {
        "write", "write_file", "edit", "replace", "insert", "delete", "remove",
        "create", "move", "rename", "patch", "append",
        "sessions_patch", "sessions_reset", "sessions_delete", "sessions_yield",
        "set_session_mode", "set_session_config",
    };
    if (kMutatingToolIds.count(tool_name)) {
        result.approval_class = AcpApprovalClass::Mutating;
        result.auto_approve = false;
        return result;
    }
    // 対齐:244 —— 兜底 other，不自动批准
    result.approval_class = AcpApprovalClass::Other;
    result.auto_approve = false;
    return result;
}

// ============================================================================
// 🎯 严格对齐 policy.ts
// ACP Policy — 启用/禁用策略控制
// ============================================================================
//
// ⚠ 此前这四个函数全是硬编码 stub（`return true;` / 忽略 cfg），
// 等于**策略配置完全失效**：运维把 acp.enabled 设成 false 也拦不住，
// allowedAgents 白名单形同虚设。现改为真正读取 config.data。

namespace {

/// 取 cfg.data["acp"]，不存在则返回空对象
nlohmann::json acp_config_section(const core::Config& cfg) {
    const auto& data = cfg.data;
    if (data.contains("acp") && data["acp"].is_object()) return data["acp"];
    return nlohmann::json::object();
}

/// 対齐 routing/session-key.ts normalizeAgentId —— trim + lowercase
std::string normalize_agent_id(const std::string& v) {
    return to_lower_copy(trim_copy(v));
}

}  // namespace

bool is_acp_enabled_by_policy(const core::Config& cfg) {
    // 対齐 policy.ts:13-15 —— `cfg.acp?.enabled !== false`
    // 注意语义是「只有显式 false 才算禁用」，缺省/非布尔都视为启用。
    const nlohmann::json acp = acp_config_section(cfg);
    if (acp.contains("enabled") && acp["enabled"].is_boolean()) {
        return acp["enabled"].get<bool>();
    }
    return true;
}

AcpDispatchPolicyState resolve_acp_dispatch_policy_state(const core::Config& cfg) {
    // 対齐 policy.ts:18-27
    if (!is_acp_enabled_by_policy(cfg)) {
        return AcpDispatchPolicyState::AcpDisabled;
    }
    // 対齐:23-25 —— `cfg.acp?.dispatch?.enabled === false`，同样只认显式 false。
    // 此前完全没读这一层，DispatchDisabled 状态永远无法产生。
    const nlohmann::json acp = acp_config_section(cfg);
    if (acp.contains("dispatch") && acp["dispatch"].is_object()) {
        const auto& d = acp["dispatch"];
        if (d.contains("enabled") && d["enabled"].is_boolean() &&
            d["enabled"].get<bool>() == false) {
            return AcpDispatchPolicyState::DispatchDisabled;
        }
    }
    return AcpDispatchPolicyState::Enabled;
}

bool is_acp_dispatch_enabled_by_policy(const core::Config& cfg) {
    return resolve_acp_dispatch_policy_state(cfg) == AcpDispatchPolicyState::Enabled;
}

std::string resolve_acp_dispatch_policy_message(const core::Config& cfg) {
    // 対齐 policy.ts:30-39 —— 文案逐字一致（运维据此排查）
    auto state = resolve_acp_dispatch_policy_state(cfg);
    if (state == AcpDispatchPolicyState::AcpDisabled) {
        return "ACP is disabled by policy (`acp.enabled=false`).";
    }
    if (state == AcpDispatchPolicyState::DispatchDisabled) {
        return "ACP dispatch is disabled by policy (`acp.dispatch.enabled=false`).";
    }
    return "";
}

bool is_acp_agent_allowed_by_policy(const core::Config& cfg, const std::string& agent_id) {
    // 対齐 policy.ts:59-67 —— allowedAgents 为空数组时放行全部；
    // 非空则必须命中（比较前双方都过 normalizeAgentId）。
    const nlohmann::json acp = acp_config_section(cfg);
    if (!acp.contains("allowedAgents") || !acp["allowedAgents"].is_array()) {
        return true;
    }
    std::vector<std::string> allowed;
    for (const auto& e : acp["allowedAgents"]) {
        if (!e.is_string()) continue;
        const std::string n = normalize_agent_id(e.get<std::string>());
        if (!n.empty()) allowed.push_back(n);   // 対齐:62 .filter(Boolean)
    }
    if (allowed.empty()) return true;           // 対齐:63-65
    const std::string target = normalize_agent_id(agent_id);
    return std::find(allowed.begin(), allowed.end(), target) != allowed.end();
}

/// 対齐 policy.ts:70-81 resolveAcpAgentPolicyError —— 错误文案含归一化后的 id
std::string resolve_acp_agent_policy_message(const core::Config& cfg,
                                            const std::string& agent_id) {
    if (is_acp_agent_allowed_by_policy(cfg, agent_id)) return std::string();
    return "ACP agent \"" + normalize_agent_id(agent_id) + "\" is not allowed by policy.";
}

// ============================================================================
// 🎯 对齐 session-interaction-mode.ts
// ACP Session Interaction Mode
// ============================================================================

// 🎯 对齐: resolveAcpSessionInteractionMode(entry)
AcpSessionInteractionMode resolve_acp_session_interaction_mode(
    const SessionInteractionEntry& entry) {
    // Parent-owned oneshot ACP sessions are background work
    if (entry.acp_mode != "oneshot") {
        return AcpSessionInteractionMode::Interactive;
    }
    if (!entry.spawned_by.empty() || !entry.parent_session_key.empty()) {
        return AcpSessionInteractionMode::ParentOwnedBackground;
    }
    return AcpSessionInteractionMode::Interactive;
}

// 🎯 对齐: isParentOwnedBackgroundAcpSession(entry)
bool is_parent_owned_background_acp_session(const SessionInteractionEntry& entry) {
    return resolve_acp_session_interaction_mode(entry) ==
        AcpSessionInteractionMode::ParentOwnedBackground;
}

// ============================================================================
// 🎯 严格对齐 client-helpers.ts
// ACP Client — 权限解析
// ============================================================================

// 対齐 client-helpers.ts:28-42 resolveToolKindForPermission
std::string resolve_tool_kind_for_permission(
    const std::string& tool_name,
    AcpApprovalClass approval_class) {
    // 対齐:32-34 —— 名字为空**且** class 为 unknown 才返回空
    if (tool_name.empty() && approval_class == AcpApprovalClass::Unknown) {
        return "";
    }
    // 対齐:35-41 —— 其余情况直接返回 approvalClass 的字面量
    switch (approval_class) {
        case AcpApprovalClass::ReadonlyScoped: return "readonly_scoped";
        case AcpApprovalClass::ReadonlySearch: return "readonly_search";
        case AcpApprovalClass::Mutating:       return "mutating";
        case AcpApprovalClass::ExecCapable:    return "exec_capable";
        case AcpApprovalClass::ControlPlane:   return "control_plane";
        case AcpApprovalClass::Interactive:    return "interactive";
        case AcpApprovalClass::Other:          return "other";
        case AcpApprovalClass::Unknown:        return "unknown";
    }
    return "unknown";
}

namespace {

/// 対齐 client-helpers.ts:44-55 pickOption
/// **按 kinds 的给定顺序**依次找，不是找到任意一个就算 ——
/// allow_once 必须优先于 allow_always，否则会把一次性授权升级为永久授权。
const PermissionOption* pick_option(const std::vector<PermissionOption>& options,
                                    const std::vector<std::string>& kinds) {
    for (const auto& kind : kinds) {
        for (const auto& opt : options) {
            if (opt.kind == kind) return &opt;
        }
    }
    return nullptr;
}

}  // namespace

// 🎯 对齐: AcpClientConnection — 管理与 ACP agent 进程的连接
AcpClientConnection::AcpClientConnection() = default;
AcpClientConnection::~AcpClientConnection() = default;

bool AcpClientConnection::connect(const AcpClientOptions& opts) {
    opts_ = opts;

    // 🎯 对齐: spawn child process or connect to existing
    // In HunterClaw, this is handled via the subprocess/process module
    connected_ = true;
    return true;
}

void AcpClientConnection::disconnect() {
    connected_ = false;
}

bool AcpClientConnection::is_connected() const {
    return connected_;
}

// 対齐 client-helpers.ts:106-153 resolvePermissionRequest
PermissionResponse AcpClientConnection::resolve_permission(
    const PermissionRequest& req) {

    // 対齐:115 —— 分类必须由 toolCall 自行解析工具名（防伪造），
    // cwd 用于路径范围判定。此前传的是调用方给的 tool_name，绕过了防伪造闸。
    const std::string cwd = opts_.cwd.empty() ? std::string(".") : opts_.cwd;
    auto classification = classify_acp_tool_approval(req.tool_call, cwd);

    // 対齐:119-122 —— 无选项可选 → cancelled（不是 deny）
    if (req.options.empty()) {
        return PermissionResponse{PermissionOutcome::Cancelled, ""};
    }

    // 対齐:124-125 —— option kind 是 ACP 规范名：
    //   allow_once / allow_always / reject_once / reject_always
    // 此前用的 "allow" / "always_allow" 两个名字**在 ACP 规范里不存在**，
    // 导致永远匹配不到任何选项，所有请求最终都走到 cancelled。
    const PermissionOption* allow_option =
        pick_option(req.options, {"allow_once", "allow_always"});
    const PermissionOption* reject_option =
        pick_option(req.options, {"reject_once", "reject_always"});

    // 対齐:126 —— promptRequired = !autoApprove
    const bool prompt_required = !classification.auto_approve;

    // 対齐:128-135 —— 自动批准路径
    if (!prompt_required) {
        if (!allow_option) {
            return PermissionResponse{PermissionOutcome::Cancelled, ""};
        }
        return PermissionResponse{PermissionOutcome::Selected, allow_option->id};
    }

    // 対齐:140 —— 需要人工确认。非交互环境等价于 prompt 返回 false
    // （対齐:66-69 promptUserPermission 的 non-TTY 分支）。
    bool approved = false;
    if (opts_.interactive && prompt_callback_) {
        approved = prompt_callback_(classification.tool_name, req.tool_title);
    }

    // 対齐:142-147 —— 批准/拒绝各自需要对应选项存在
    if (approved && allow_option) {
        return PermissionResponse{PermissionOutcome::Selected, allow_option->id};
    }
    if (!approved && reject_option) {
        return PermissionResponse{PermissionOutcome::Selected, reject_option->id};
    }

    // 対齐:149-152 —— 缺对应选项 → cancelled
    return PermissionResponse{PermissionOutcome::Cancelled, ""};
}

} // namespace acp
} // namespace hunterclaw
