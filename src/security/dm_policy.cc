#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <regex>
#include <optional>

namespace hunterclaw {
namespace security {

// ============================================================================
// DM/Group Access Policy - DM/群组访问控制策略
// 严格对齐 OpenClaw: src/security/dm-policy-shared.ts
// ============================================================================

enum class DmGroupAccessDecision { Allow, Block, Pairing };

// 对齐 OpenClaw: DM_GROUP_ACCESS_REASON
struct DmGroupAccessReason {
    static constexpr const char* GROUP_POLICY_ALLOWED        = "group_policy_allowed";
    static constexpr const char* GROUP_POLICY_DISABLED       = "group_policy_disabled";
    static constexpr const char* GROUP_POLICY_EMPTY_ALLOWLIST = "group_policy_empty_allowlist";
    static constexpr const char* GROUP_POLICY_NOT_ALLOWLISTED = "group_policy_not_allowlisted";
    static constexpr const char* DM_POLICY_OPEN              = "dm_policy_open";
    static constexpr const char* DM_POLICY_DISABLED          = "dm_policy_disabled";
    static constexpr const char* DM_POLICY_ALLOWLISTED       = "dm_policy_allowlisted";
    static constexpr const char* DM_POLICY_PAIRING_REQUIRED  = "dm_policy_pairing_required";
    static constexpr const char* DM_POLICY_NOT_ALLOWLISTED   = "dm_policy_not_allowlisted";
};

struct DmGroupAccessResult {
    DmGroupAccessDecision decision;
    std::string reason_code;
    std::string reason;
    std::vector<std::string> effective_allow_from;
    std::vector<std::string> effective_group_allow_from;
};

namespace {

std::vector<std::string> normalize_string_entries(const std::vector<std::string>& entries) {
    std::vector<std::string> result;
    for (const auto& e : entries) {
        std::string s = e;
        // trim
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end = s.find_last_not_of(" \t\r\n");
        if (start != std::string::npos) {
            s = s.substr(start, end - start + 1);
        } else {
            s.clear();
        }
        if (!s.empty()) result.push_back(s);
    }
    return result;
}

} // anonymous namespace

/**
 * resolve_dm_group_access_decision - 解析 DM/Group 访问决策
 * 对齐 OpenClaw: resolveDmGroupAccessDecision(params)
 */
DmGroupAccessResult resolve_dm_group_access_decision(
    bool is_group,
    const std::string& dm_policy,     // "open" | "disabled" | "pairing" | "allowlist"
    const std::string& group_policy,  // "open" | "disabled" | "allowlist"
    const std::vector<std::string>& effective_allow_from,
    const std::vector<std::string>& effective_group_allow_from,
    std::function<bool(const std::vector<std::string>&)> is_sender_allowed
) {
    DmGroupAccessResult result;
    result.effective_allow_from = effective_allow_from;
    result.effective_group_allow_from = effective_group_allow_from;
    
    std::string resolved_dm = dm_policy.empty() ? "pairing" : dm_policy;
    std::string resolved_group = (group_policy == "open" || group_policy == "disabled")
        ? group_policy : "allowlist";
    
    if (is_group) {
        if (resolved_group == "disabled") {
            result.decision = DmGroupAccessDecision::Block;
            result.reason_code = DmGroupAccessReason::GROUP_POLICY_DISABLED;
            result.reason = "groupPolicy=disabled";
            return result;
        }
        if (resolved_group == "allowlist") {
            if (effective_group_allow_from.empty()) {
                result.decision = DmGroupAccessDecision::Block;
                result.reason_code = DmGroupAccessReason::GROUP_POLICY_EMPTY_ALLOWLIST;
                result.reason = "groupPolicy=allowlist (empty allowlist)";
                return result;
            }
            if (!is_sender_allowed(effective_group_allow_from)) {
                result.decision = DmGroupAccessDecision::Block;
                result.reason_code = DmGroupAccessReason::GROUP_POLICY_NOT_ALLOWLISTED;
                result.reason = "groupPolicy=allowlist (not allowlisted)";
                return result;
            }
        }
        result.decision = DmGroupAccessDecision::Allow;
        result.reason_code = DmGroupAccessReason::GROUP_POLICY_ALLOWED;
        result.reason = "groupPolicy=" + resolved_group;
        return result;
    }
    
    // DM policy
    if (resolved_dm == "disabled") {
        result.decision = DmGroupAccessDecision::Block;
        result.reason_code = DmGroupAccessReason::DM_POLICY_DISABLED;
        result.reason = "dmPolicy=disabled";
        return result;
    }
    if (resolved_dm == "open") {
        result.decision = DmGroupAccessDecision::Allow;
        result.reason_code = DmGroupAccessReason::DM_POLICY_OPEN;
        result.reason = "dmPolicy=open";
        return result;
    }
    if (is_sender_allowed(effective_allow_from)) {
        result.decision = DmGroupAccessDecision::Allow;
        result.reason_code = DmGroupAccessReason::DM_POLICY_ALLOWLISTED;
        result.reason = "dmPolicy=" + resolved_dm + " (allowlisted)";
        return result;
    }
    if (resolved_dm == "pairing") {
        result.decision = DmGroupAccessDecision::Pairing;
        result.reason_code = DmGroupAccessReason::DM_POLICY_PAIRING_REQUIRED;
        result.reason = "dmPolicy=pairing (not allowlisted)";
        return result;
    }
    result.decision = DmGroupAccessDecision::Block;
    result.reason_code = DmGroupAccessReason::DM_POLICY_NOT_ALLOWLISTED;
    result.reason = "dmPolicy=" + resolved_dm + " (not allowlisted)";
    return result;
}

// ============================================================================
// Mutable Allowlist Detectors - 可变身份允许列表检测器
// 严格对齐 OpenClaw: src/security/mutable-allowlist-detectors.ts
// ============================================================================

/**
 * is_discord_mutable_allow_entry - Discord 可变允许列表项检测
 * 对齐 OpenClaw: isDiscordMutableAllowEntry(raw)
 */
bool is_discord_mutable_allow_entry(const std::string& raw) {
    std::string text = raw;
    // trim
    size_t s = text.find_first_not_of(" \t"); 
    size_t e = text.find_last_not_of(" \t");
    text = (s == std::string::npos) ? "" : text.substr(s, e - s + 1);
    if (text.empty() || text == "*") return false;
    
    // Strip <@!?...> mention format
    std::string maybe_id = std::regex_replace(text, std::regex(R"(^<@!?)"), "");
    maybe_id = std::regex_replace(maybe_id, std::regex(R"(>$)"), "");
    if (std::regex_match(maybe_id, std::regex(R"(^\d+$)"))) return false;
    
    // Check prefixes
    for (const auto& prefix : {"discord:", "user:", "pk:"}) {
        std::string p(prefix);
        if (text.substr(0, p.size()) == p) {
            std::string rest = text.substr(p.size());
            // trim rest
            size_t rs = rest.find_first_not_of(" \t");
            return rs == std::string::npos; // mutable if rest is empty
        }
    }
    return true; // name/tag based = mutable
}

/**
 * is_zalouser_mutable_group_entry - Zalouser 可变群组项检测
 * 对齐 OpenClaw: isZalouserMutableGroupEntry(raw)
 */
bool is_zalouser_mutable_group_entry(const std::string& raw) {
    std::string text = raw;
    size_t s = text.find_first_not_of(" \t");
    size_t e = text.find_last_not_of(" \t");
    text = (s == std::string::npos) ? "" : text.substr(s, e - s + 1);
    if (text.empty() || text == "*") return false;
    
    // Strip prefixes
    std::string normalized = text;
    for (const auto& prefix : {"zalouser:", "zlu:", "group:"}) {
        std::string p(prefix);
        std::string lower = normalized;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.substr(0, p.size()) == p) {
            normalized = normalized.substr(p.size());
        }
    }
    // trim
    s = normalized.find_first_not_of(" \t");
    e = normalized.find_last_not_of(" \t");
    normalized = (s == std::string::npos) ? "" : normalized.substr(s, e - s + 1);
    
    if (normalized.empty()) return false;
    if (std::regex_match(normalized, std::regex(R"(^\d+$)"))) return false;
    // g- prefixed IDs are stable
    if (std::regex_match(normalized, std::regex(R"(^g-\S+$)", std::regex::icase))) return false;
    return true;
}

}  // namespace security
}  // namespace hunterclaw
