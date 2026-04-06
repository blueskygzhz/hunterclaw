#pragma once
// Aligned with openclaw: src/routing/session-key.ts (254 lines) + account-id.ts (71 lines)
// Session key normalization, agent ID, account ID, session key construction

#include <string>
#include <optional>
#include <regex>
#include <unordered_map>
#include <mutex>

namespace hunterclaw {
namespace routing {

// ============================================================================
// Constants (对齐 session-key.ts) - use sk_ prefix to avoid conflicts
// ============================================================================
inline constexpr const char* SK_DEFAULT_AGENT_ID = "main";
inline constexpr const char* SK_DEFAULT_MAIN_KEY = "main";
inline constexpr const char* SK_DEFAULT_ACCOUNT_ID = "default";

// ============================================================================
// SessionKeyShape (对齐 session-key.ts)
// ============================================================================
enum class SessionKeyShape { Missing, Agent, LegacyOrAlias, MalformedAgent };

// ============================================================================
// ParsedAgentSessionKey (对齐 session-key-utils.ts)
// ============================================================================
struct ParsedAgentSessionKey {
    std::string agent_id;
    std::string rest;
};

// Aligned with openclaw: parseAgentSessionKey
inline std::optional<ParsedAgentSessionKey> parse_agent_session_key(const std::string& key) {
    if (key.empty()) return std::nullopt;
    // Pattern: agent:{agentId}:{rest}
    if (key.size() < 7 || key.substr(0, 6) != "agent:") return std::nullopt;
    auto second_colon = key.find(':', 6);
    if (second_colon == std::string::npos) return std::nullopt;
    return ParsedAgentSessionKey{key.substr(6, second_colon - 6), key.substr(second_colon + 1)};
}

// ============================================================================
// Agent ID normalization (对齐 session-key.ts:89-116)
// ============================================================================
inline std::string normalize_agent_id(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    auto e = trimmed.find_last_not_of(" \t\n\r");
    if (s == std::string::npos) return SK_DEFAULT_AGENT_ID;
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return SK_DEFAULT_AGENT_ID;
    // Check valid ID
    static const std::regex valid_re("^[a-z0-9][a-z0-9_-]{0,63}$", std::regex::icase);
    if (std::regex_match(trimmed, valid_re)) {
        std::string lower = trimmed;
        for (auto& c : lower) c = std::tolower(c);
        return lower;
    }
    // Best-effort fallback
    std::string result;
    for (auto& c : trimmed) result += std::tolower(c);
    static const std::regex invalid_re("[^a-z0-9_-]+");
    result = std::regex_replace(result, invalid_re, "-");
    // Strip leading/trailing dashes
    while (!result.empty() && result.front() == '-') result.erase(0, 1);
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.size() > 64) result = result.substr(0, 64);
    return result.empty() ? SK_DEFAULT_AGENT_ID : result;
}

inline bool is_valid_agent_id(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return false;
    static const std::regex valid_re("^[a-z0-9][a-z0-9_-]{0,63}$", std::regex::icase);
    return std::regex_match(trimmed, valid_re);
}

inline std::string sanitize_agent_id(const std::string& value) {
    return normalize_agent_id(value);
}

// ============================================================================
// Account ID normalization (对齐 account-id.ts)
// ============================================================================
inline std::string normalize_account_id(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    auto e = trimmed.find_last_not_of(" \t\n\r");
    if (s == std::string::npos) return SK_DEFAULT_ACCOUNT_ID;
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return SK_DEFAULT_ACCOUNT_ID;
    // Prototype pollution check
    if (trimmed == "__proto__" || trimmed == "constructor" || trimmed == "prototype") {
        return SK_DEFAULT_ACCOUNT_ID;
    }
    static const std::regex valid_re("^[a-z0-9][a-z0-9_-]{0,63}$", std::regex::icase);
    if (std::regex_match(trimmed, valid_re)) {
        std::string lower = trimmed;
        for (auto& c : lower) c = std::tolower(c);
        return lower;
    }
    std::string result;
    for (auto& c : trimmed) result += std::tolower(c);
    static const std::regex invalid_re("[^a-z0-9_-]+");
    result = std::regex_replace(result, invalid_re, "-");
    while (!result.empty() && result.front() == '-') result.erase(0, 1);
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.size() > 64) result = result.substr(0, 64);
    return result.empty() ? SK_DEFAULT_ACCOUNT_ID : result;
}

inline std::optional<std::string> normalize_optional_account_id(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::nullopt;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return std::nullopt;
    auto result = normalize_account_id(trimmed);
    return result == SK_DEFAULT_ACCOUNT_ID ? std::nullopt : std::optional<std::string>(result);
}

// ============================================================================
// Main Key normalization (对齐 session-key.ts:40-43)
// ============================================================================
inline std::string normalize_main_key(const std::string& value) {
    std::string trimmed = value;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return SK_DEFAULT_MAIN_KEY;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return SK_DEFAULT_MAIN_KEY;
    for (auto& c : trimmed) c = std::tolower(c);
    return trimmed;
}

// ============================================================================
// Session Key Classification (对齐 session-key.ts:78-87)
// ============================================================================
inline SessionKeyShape classify_session_key_shape(const std::string& session_key) {
    std::string raw = session_key;
    auto s = raw.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return SessionKeyShape::Missing;
    auto e = raw.find_last_not_of(" \t\n\r");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return SessionKeyShape::Missing;
    if (parse_agent_session_key(raw)) return SessionKeyShape::Agent;
    std::string lower = raw;
    for (auto& c : lower) c = std::tolower(c);
    return lower.substr(0, 6) == "agent:" ? SessionKeyShape::MalformedAgent : SessionKeyShape::LegacyOrAlias;
}

// ============================================================================
// Session Key Builders (对齐 session-key.ts:118-174)
// ============================================================================
inline std::string build_agent_main_session_key(const std::string& agent_id,
                                                  const std::string& main_key = "") {
    return "agent:" + normalize_agent_id(agent_id) + ":" + normalize_main_key(main_key);
}

inline std::string resolve_agent_id_from_session_key(const std::string& session_key) {
    auto parsed = parse_agent_session_key(session_key);
    return normalize_agent_id(parsed ? parsed->agent_id : SK_DEFAULT_AGENT_ID);
}

inline std::optional<std::string> to_agent_request_session_key(const std::string& store_key) {
    std::string raw = store_key;
    auto s = raw.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::nullopt;
    auto e = raw.find_last_not_of(" \t\n\r");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return std::nullopt;
    auto parsed = parse_agent_session_key(raw);
    return parsed ? std::optional<std::string>(parsed->rest) : std::optional<std::string>(raw);
}

// Aligned with openclaw: isAcpSessionKey / isCronSessionKey / isSubagentSessionKey
// NOTE: prefixed with sk_ to avoid conflict with session_key_utils.h equivalents
inline bool sk_is_acp_session_key(const std::string& key) {
    auto parsed = parse_agent_session_key(key);
    return parsed && parsed->rest.find("acp:") == 0;
}

inline bool sk_is_cron_session_key(const std::string& key) {
    auto parsed = parse_agent_session_key(key);
    return parsed && parsed->rest.find("cron:") == 0;
}

inline bool sk_is_subagent_session_key(const std::string& key) {
    auto parsed = parse_agent_session_key(key);
    return parsed && parsed->rest.find("sub:") == 0;
}

// ============================================================================
// Account Lookup (对齐 account-lookup.ts)
// ============================================================================
template<typename T>
inline std::optional<T> resolve_account_entry(
    const std::unordered_map<std::string, T>& accounts,
    const std::string& account_id
) {
    if (accounts.empty()) return std::nullopt;
    auto it = accounts.find(account_id);
    if (it != accounts.end()) return it->second;
    std::string lower = account_id;
    for (auto& c : lower) c = std::tolower(c);
    for (const auto& [key, val] : accounts) {
        std::string key_lower = key;
        for (auto& c : key_lower) c = std::tolower(c);
        if (key_lower == lower) return val;
    }
    return std::nullopt;
}

} // namespace routing
} // namespace hunterclaw
