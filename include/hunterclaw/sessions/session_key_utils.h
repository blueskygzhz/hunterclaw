#ifndef HUNTERCLAW_SESSIONS_SESSION_KEY_UTILS_H
#define HUNTERCLAW_SESSIONS_SESSION_KEY_UTILS_H

#include <string>
#include <optional>
#include <vector>
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace sessions {

// Chat type enum (aligned with OpenClaw: SessionKeyChatType)
enum class SessionKeyChatType {
    Direct,   // "direct"
    Group,    // "group"
    Channel,  // "channel"
    Unknown   // "unknown"
};

// Parsed agent session key (aligned with OpenClaw: ParsedAgentSessionKey)
struct ParsedAgentSessionKey {
    std::string agent_id;
    std::string rest;
};

/**
 * Parse agent-scoped session keys in a canonical, case-insensitive way.
 * Returned values are normalized to lowercase for stable comparisons/routing.
 * Aligned with OpenClaw: parseAgentSessionKey()
 */
inline std::optional<ParsedAgentSessionKey> parse_agent_session_key(
    const std::string& session_key
) {
    if (session_key.empty()) return std::nullopt;

    // Normalize to lowercase and trim
    std::string raw = session_key;
    // Trim
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    // Lowercase
    std::transform(raw.begin(), raw.end(), raw.begin(), ::tolower);

    if (raw.empty()) return std::nullopt;

    // Split by ':'
    std::vector<std::string> parts;
    std::string token;
    for (char c : raw) {
        if (c == ':') {
            if (!token.empty()) parts.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    if (!token.empty()) parts.push_back(token);

    if (parts.size() < 3) return std::nullopt;
    if (parts[0] != "agent") return std::nullopt;

    std::string agent_id = parts[1];
    if (agent_id.empty()) return std::nullopt;

    // rest = parts[2:]
    std::string rest;
    for (size_t i = 2; i < parts.size(); ++i) {
        if (i > 2) rest += ":";
        rest += parts[i];
    }
    if (rest.empty()) return std::nullopt;

    return ParsedAgentSessionKey{agent_id, rest};
}

/**
 * Best-effort chat-type extraction from session keys.
 * Aligned with OpenClaw: deriveSessionChatType()
 */
inline SessionKeyChatType derive_session_chat_type(const std::string& session_key) {
    if (session_key.empty()) return SessionKeyChatType::Unknown;

    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return SessionKeyChatType::Unknown;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    std::transform(raw.begin(), raw.end(), raw.begin(), ::tolower);

    auto parsed = parse_agent_session_key(raw);
    std::string scoped = parsed ? parsed->rest : raw;

    // Split scoped into tokens
    std::vector<std::string> tokens;
    std::string tok;
    for (char c : scoped) {
        if (c == ':') {
            if (!tok.empty()) tokens.push_back(tok);
            tok.clear();
        } else {
            tok += c;
        }
    }
    if (!tok.empty()) tokens.push_back(tok);

    for (const auto& t : tokens) {
        if (t == "group") return SessionKeyChatType::Group;
    }
    for (const auto& t : tokens) {
        if (t == "channel") return SessionKeyChatType::Channel;
    }
    for (const auto& t : tokens) {
        if (t == "direct" || t == "dm") return SessionKeyChatType::Direct;
    }

    // Legacy Discord keys: discord:<accountId>:guild-<guildId>:channel-<channelId>
    static const std::regex discord_re{
        "^discord:(?:[^:]+:)?guild-[^:]+:channel-[^:]+$"
    };
    if (std::regex_match(scoped, discord_re)) {
        return SessionKeyChatType::Channel;
    }

    return SessionKeyChatType::Unknown;
}

/**
 * Check if session key is a cron run session key.
 * Aligned with OpenClaw: isCronRunSessionKey()
 */
inline bool is_cron_run_session_key(const std::string& session_key) {
    auto parsed = parse_agent_session_key(session_key);
    if (!parsed) return false;
    static const std::regex cron_run_re{"^cron:[^:]+:run:[^:]+$"};
    return std::regex_match(parsed->rest, cron_run_re);
}

/**
 * Check if session key is a cron session key.
 * Aligned with OpenClaw: isCronSessionKey()
 */
inline bool is_cron_session_key(const std::string& session_key) {
    auto parsed = parse_agent_session_key(session_key);
    if (!parsed) return false;
    return parsed->rest.substr(0, 5) == "cron:";
}

/**
 * Check if session key is a subagent session key.
 * Aligned with OpenClaw: isSubagentSessionKey()
 */
inline bool is_subagent_session_key(const std::string& session_key) {
    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return false;

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.substr(0, 9) == "subagent:") return true;

    auto parsed = parse_agent_session_key(raw);
    if (!parsed) return false;
    return parsed->rest.substr(0, 9) == "subagent:";
}

/**
 * Get subagent nesting depth from session key.
 * Aligned with OpenClaw: getSubagentDepth()
 */
inline int get_subagent_depth(const std::string& session_key) {
    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return 0;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    std::transform(raw.begin(), raw.end(), raw.begin(), ::tolower);
    if (raw.empty()) return 0;

    // Count occurrences of ":subagent:"
    int count = 0;
    size_t pos = 0;
    const std::string marker = ":subagent:";
    while ((pos = raw.find(marker, pos)) != std::string::npos) {
        ++count;
        pos += marker.size();
    }
    return count;
}

/**
 * Check if session key is an ACP session key.
 * Aligned with OpenClaw: isAcpSessionKey()
 */
inline bool is_acp_session_key(const std::string& session_key) {
    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return false;

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.substr(0, 4) == "acp:") return true;

    auto parsed = parse_agent_session_key(raw);
    if (!parsed) return false;
    return parsed->rest.substr(0, 4) == "acp:";
}

/**
 * Resolve the parent session key for thread/topic sessions.
 * Aligned with OpenClaw: resolveThreadParentSessionKey()
 */
inline std::optional<std::string> resolve_thread_parent_session_key(
    const std::string& session_key
) {
    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return std::nullopt;

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Find last occurrence of ":thread:" or ":topic:"
    const std::vector<std::string> markers = {":thread:", ":topic:"};
    size_t idx = std::string::npos;
    for (const auto& marker : markers) {
        size_t pos = lower.rfind(marker);
        if (pos != std::string::npos) {
            if (idx == std::string::npos || pos > idx) {
                idx = pos;
            }
        }
    }

    if (idx == std::string::npos || idx == 0) return std::nullopt;

    std::string parent = raw.substr(0, idx);
    // Trim
    size_t ps = parent.find_first_not_of(" \t\r\n");
    if (ps == std::string::npos) return std::nullopt;
    size_t pe = parent.find_last_not_of(" \t\r\n");
    parent = parent.substr(ps, pe - ps + 1);

    return parent.empty() ? std::nullopt : std::optional<std::string>{parent};
}

} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_KEY_UTILS_H
