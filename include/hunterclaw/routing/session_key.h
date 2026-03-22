#ifndef HUNTERCLAW_ROUTING_SESSION_KEY_H
#define HUNTERCLAW_ROUTING_SESSION_KEY_H

#include <string>
#include <optional>
#include <vector>
#include <map>
#include <algorithm>
#include <regex>
#include "hunterclaw/sessions/session_key_utils.h"
#include "hunterclaw/routing/router.h"

namespace hunterclaw {
namespace routing {

// Session key shape classification (aligned with OpenClaw: SessionKeyShape)
enum class SessionKeyShape {
    Missing,          // "missing"
    Agent,            // "agent"
    LegacyOrAlias,    // "legacy_or_alias"
    MalformedAgent    // "malformed_agent"
};

// Default constants (aligned with OpenClaw)
inline const std::string DEFAULT_AGENT_ID = "main";
inline const std::string DEFAULT_MAIN_KEY_STR = "main";

/**
 * Normalize a main key value.
 * Aligned with OpenClaw: normalizeMainKey()
 */
inline std::string normalize_main_key(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return DEFAULT_MAIN_KEY_STR;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return DEFAULT_MAIN_KEY_STR;
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
    return trimmed;
}

/**
 * Normalize an agent ID.
 * Aligned with OpenClaw: normalizeAgentId()
 */
inline std::string normalize_agent_id(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return DEFAULT_AGENT_ID;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return DEFAULT_AGENT_ID;

    static const std::regex valid_id_re{"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$"};
    if (std::regex_match(trimmed, valid_id_re)) {
        std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
        return trimmed;
    }

    // Best-effort fallback: collapse invalid characters to "-"
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string result;
    for (char c : lower) {
        if (std::isalnum(c) || c == '_' || c == '-') result += c;
        else result += '-';
    }
    // Remove leading/trailing dashes
    size_t rs = result.find_first_not_of('-');
    if (rs == std::string::npos) return DEFAULT_AGENT_ID;
    size_t re = result.find_last_not_of('-');
    result = result.substr(rs, re - rs + 1);
    if (result.size() > 64) result = result.substr(0, 64);
    return result.empty() ? DEFAULT_AGENT_ID : result;
}

/**
 * Check if an agent ID is valid.
 * Aligned with OpenClaw: isValidAgentId()
 */
inline bool is_valid_agent_id(const std::string& value) {
    std::string trimmed = value;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return false;
    static const std::regex valid_id_re{"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$"};
    return std::regex_match(trimmed, valid_id_re);
}

/**
 * Convert a store session key to a request session key (strip agent prefix).
 * Aligned with OpenClaw: toAgentRequestSessionKey()
 */
inline std::optional<std::string> to_agent_request_session_key(const std::string& store_key) {
    std::string raw = store_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return std::nullopt;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return std::nullopt;

    auto parsed = sessions::parse_agent_session_key(raw);
    if (parsed) return parsed->rest;
    return raw;
}

/**
 * Convert a request session key to a store session key (add agent prefix).
 * Aligned with OpenClaw: toAgentStoreSessionKey()
 */
inline std::string to_agent_store_session_key(
    const std::string& agent_id,
    const std::string& request_key,
    const std::string& main_key = ""
) {
    std::string raw = request_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) raw = "";
    else {
        size_t e = raw.find_last_not_of(" \t\r\n");
        raw = raw.substr(s, e - s + 1);
    }

    if (raw.empty() || raw == DEFAULT_MAIN_KEY_STR) {
        std::string mk = main_key.empty() ? DEFAULT_MAIN_KEY_STR : normalize_main_key(main_key);
        return "agent:" + normalize_agent_id(agent_id) + ":" + mk;
    }

    auto parsed = sessions::parse_agent_session_key(raw);
    if (parsed) {
        return "agent:" + parsed->agent_id + ":" + parsed->rest;
    }

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (lower.substr(0, 6) == "agent:") return lower;

    return "agent:" + normalize_agent_id(agent_id) + ":" + lower;
}

/**
 * Resolve agent ID from session key.
 * Aligned with OpenClaw: resolveAgentIdFromSessionKey()
 */
inline std::string resolve_agent_id_from_session_key(const std::string& session_key) {
    auto parsed = sessions::parse_agent_session_key(session_key);
    return normalize_agent_id(parsed ? parsed->agent_id : DEFAULT_AGENT_ID);
}

/**
 * Classify the shape of a session key.
 * Aligned with OpenClaw: classifySessionKeyShape()
 */
inline SessionKeyShape classify_session_key_shape(const std::string& session_key) {
    std::string raw = session_key;
    size_t s = raw.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return SessionKeyShape::Missing;
    size_t e = raw.find_last_not_of(" \t\r\n");
    raw = raw.substr(s, e - s + 1);
    if (raw.empty()) return SessionKeyShape::Missing;

    if (sessions::parse_agent_session_key(raw)) return SessionKeyShape::Agent;

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.substr(0, 6) == "agent:" ? SessionKeyShape::MalformedAgent : SessionKeyShape::LegacyOrAlias;
}

/**
 * Build the main session key for an agent.
 * Aligned with OpenClaw: buildAgentMainSessionKey()
 */
inline std::string build_agent_main_session_key(
    const std::string& agent_id,
    const std::string& main_key = ""
) {
    return "agent:" + normalize_agent_id(agent_id) + ":" +
           (main_key.empty() ? DEFAULT_MAIN_KEY_STR : normalize_main_key(main_key));
}

/**
 * Build a peer session key for an agent.
 * Aligned with OpenClaw: buildAgentPeerSessionKey()
 */
inline std::string build_agent_peer_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::string& account_id,
    const std::string& peer_kind,  // "direct", "group", "channel"
    const std::string& peer_id,
    DmScope dm_scope = DmScope::Main,
    const std::string& main_key = ""
) {
    std::string normalized_agent = normalize_agent_id(agent_id);

    if (peer_kind == "direct") {
        std::string pid = peer_id;
        std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);

        if (dm_scope == DmScope::PerAccountChannelPeer && !pid.empty()) {
            std::string ch = channel;
            std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);
            if (ch.empty()) ch = "unknown";
            std::string aid = account_id.empty() ? DEFAULT_ACCOUNT_ID : account_id;
            return "agent:" + normalized_agent + ":" + ch + ":" + aid + ":direct:" + pid;
        }
        if (dm_scope == DmScope::PerChannelPeer && !pid.empty()) {
            std::string ch = channel;
            std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);
            if (ch.empty()) ch = "unknown";
            return "agent:" + normalized_agent + ":" + ch + ":direct:" + pid;
        }
        if (dm_scope == DmScope::PerPeer && !pid.empty()) {
            return "agent:" + normalized_agent + ":direct:" + pid;
        }
        return build_agent_main_session_key(agent_id, main_key);
    }

    std::string ch = channel;
    std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);
    if (ch.empty()) ch = "unknown";
    std::string pid = peer_id.empty() ? "unknown" : peer_id;
    std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);

    return "agent:" + normalized_agent + ":" + ch + ":" + peer_kind + ":" + pid;
}

/**
 * Resolve thread session keys.
 * Aligned with OpenClaw: resolveThreadSessionKeys()
 */
struct ThreadSessionKeys {
    std::string session_key;
    std::optional<std::string> parent_session_key;
};

inline ThreadSessionKeys resolve_thread_session_keys(
    const std::string& base_session_key,
    const std::string& thread_id = "",
    const std::string& parent_session_key = "",
    bool use_suffix = true
) {
    std::string tid = thread_id;
    size_t s = tid.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) tid = "";
    else {
        size_t e = tid.find_last_not_of(" \t\r\n");
        tid = tid.substr(s, e - s + 1);
    }

    if (tid.empty()) {
        return {base_session_key, std::nullopt};
    }

    std::string normalized_tid = tid;
    std::transform(normalized_tid.begin(), normalized_tid.end(), normalized_tid.begin(), ::tolower);

    std::string session_key = use_suffix
        ? base_session_key + ":thread:" + normalized_tid
        : base_session_key;

    std::optional<std::string> parent;
    if (!parent_session_key.empty()) parent = parent_session_key;

    return {session_key, parent};
}

/**
 * Build a group history key.
 * Aligned with OpenClaw: buildGroupHistoryKey()
 */
inline std::string build_group_history_key(
    const std::string& channel,
    const std::string& account_id,
    const std::string& peer_kind,  // "group" | "channel"
    const std::string& peer_id
) {
    std::string ch = channel;
    std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);
    if (ch.empty()) ch = "unknown";
    std::string aid = account_id.empty() ? DEFAULT_ACCOUNT_ID : account_id;
    std::string pid = peer_id.empty() ? "unknown" : peer_id;
    std::transform(pid.begin(), pid.end(), pid.begin(), ::tolower);
    return ch + ":" + aid + ":" + peer_kind + ":" + pid;
}

} // namespace routing
} // namespace hunterclaw

#endif // HUNTERCLAW_ROUTING_SESSION_KEY_H
