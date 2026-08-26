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

// ============================================================================
// Thread suffix / delivery route 解析
// Aligned with OpenClaw: session-key-utils.ts:360-429
// ============================================================================

/// 対齐 OpenClaw: ParsedThreadSessionSuffix
struct ParsedThreadSessionSuffix {
    std::optional<std::string> base_session_key;
    std::optional<std::string> thread_id;
};

/**
 * 対齐 OpenClaw: parseThreadSessionSuffix() (session-key-utils.ts:360-379)
 *
 * 按**最后一个** ":thread:" 切分，前半为 base session key，后半为 thread id。
 * 注意标记查找在 lowercase 串上做，但切分取自**原串**（保留大小写）——
 * thread id 可能是大小写敏感的外部 ID。
 */
inline ParsedThreadSessionSuffix parse_thread_session_suffix(
    const std::string& session_key
) {
    ParsedThreadSessionSuffix out;

    size_t s = session_key.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return out;   // 対齐:364-366 空串返回双 undefined
    size_t e = session_key.find_last_not_of(" \t\r\n");
    const std::string raw = session_key.substr(s, e - s + 1);
    if (raw.empty()) return out;

    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    static const std::string kThreadMarker = ":thread:";
    const size_t marker_index = lower.rfind(kThreadMarker);

    if (marker_index == std::string::npos) {
        // 対齐:374 —— 无标记时 baseSessionKey 就是原串
        out.base_session_key = raw;
        return out;
    }
    out.base_session_key = raw.substr(0, marker_index);
    const std::string thread_raw = raw.substr(marker_index + kThreadMarker.size());
    // 対齐:376 normalizeOptionalString —— trim 后为空则视为 undefined
    size_t ts = thread_raw.find_first_not_of(" \t\r\n");
    if (ts != std::string::npos) {
        size_t te = thread_raw.find_last_not_of(" \t\r\n");
        out.thread_id = thread_raw.substr(ts, te - ts + 1);
    }
    return out;
}

/// 対齐 OpenClaw: ParsedSessionDeliveryRoute
struct ParsedSessionDeliveryRoute {
    std::string channel;                       ///< 已 lowercase
    std::string peer_kind;                     ///< "channel" | "direct" | "dm" | "group"
    std::string peer_id;
    std::optional<std::string> account_id;     ///< 仅 direct/dm 形态有
    std::optional<std::string> thread_id;
};

/**
 * 対齐 OpenClaw: parseSessionDeliveryRoute() (session-key-utils.ts:388-429)
 *
 * 只解析**完整的外部投递形态**，嵌套的 ownership 结构保持不透明（返回空）。
 * 支持两种形态：
 *   agent:{agentId}:{channel}:{accountId}:direct|dm:{peerId...}
 *   agent:{agentId}:{channel}:{group|channel}:{peerId...}
 * peerId 可含 ':'，故用 slice(n).join(":") 语义把剩余段全部拼回。
 */
inline std::optional<ParsedSessionDeliveryRoute> parse_session_delivery_route(
    const std::string& session_key
) {
    // 対齐:392 —— 先剥 thread 后缀
    const ParsedThreadSessionSuffix thread = parse_thread_session_suffix(session_key);
    const std::string base = thread.base_session_key.has_value()
        ? *thread.base_session_key : session_key;

    // 対齐:393 + :262-278 parseAgentSessionKey
    //
    // ⚠ 这里刻意**不复用**本文件的 parse_agent_session_key()：那个实现用
    //   `if (!token.empty()) parts.push_back(token)` 过滤了空段，而真源用的是
    //   `raw.split(":")` —— 空段会被保留，并在 :267 / :410 处成为拒绝畸形 key
    //   的依据。举例：`agent:main:telegram::direct:123`（accountId 段为空）
    //   真源判定为 null（不可投递），而过滤空段后会被误解析成
    //   peerKind=direct/peerId=123 的合法路由，accountId 静默丢失，
    //   最终投递到错误账号。
    //   修 parse_agent_session_key 会波及 derive_session_chat_type 等既有调用者，
    //   故此处按真源语义就地 split，保证投递路径正确且不影响其它调用点。
    std::string raw = base;
    {
        size_t s = raw.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) return std::nullopt;
        size_t e = raw.find_last_not_of(" \t\r\n");
        raw = raw.substr(s, e - s + 1);
    }
    if (raw.empty()) return std::nullopt;
    std::transform(raw.begin(), raw.end(), raw.begin(), ::tolower);

    // 保留空段的 split（対齐 JS String.prototype.split(":")）
    auto split_keep_empty = [](const std::string& s) {
        std::vector<std::string> out;
        std::string tok;
        for (char c : s) {
            if (c == ':') { out.push_back(tok); tok.clear(); }
            else          { tok += c; }
        }
        out.push_back(tok);
        return out;
    };

    const std::vector<std::string> outer = split_keep_empty(raw);
    // 対齐:267,270 —— 必须是 agent:{agentId}:{rest...} 且 agentId/首段 rest 非空
    if (outer.size() < 3 || outer[0] != "agent" || outer[1].empty() || outer[2].empty()) {
        return std::nullopt;
    }
    // 対齐:274 —— rest = parts.slice(2).join(":")
    std::string rest;
    for (size_t i = 2; i < outer.size(); ++i) {
        if (i > 2) rest += ":";
        rest += outer[i];
    }
    if (rest.empty()) return std::nullopt;   // 対齐:275-277

    // 対齐:397 —— rest 再 split（同样保留空段）
    const std::vector<std::string> parts = split_keep_empty(rest);

    // 対齐:398 —— 内层再套 agent 视为不透明；段数不足 3 无法构成投递形态
    if (parts[0] == "agent" || parts.size() < 3) return std::nullopt;

    // 対齐:401-404
    const std::string channel = parts[0];
    if (channel.empty()) return std::nullopt;

    auto join_from = [&parts](size_t from) {
        std::string out;
        for (size_t i = from; i < parts.size(); ++i) {
            if (i > from) out += ":";
            out += parts[i];
        }
        return out;
    };

    // 対齐:406-420 —— direct/dm：{channel}:{accountId}:direct|dm:{peerId...}
    if (parts.size() >= 4 && (parts[2] == "direct" || parts[2] == "dm")) {
        const std::string account_id = parts[1];
        const std::string first_peer_segment = parts[3];
        const std::string peer_id = join_from(3);
        // 対齐:410-412 —— 三者任一为空即不可投递（空 accountId 正靠这里拦下）
        if (account_id.empty() || first_peer_segment.empty() || peer_id.empty()) {
            return std::nullopt;
        }
        ParsedSessionDeliveryRoute route;
        route.channel = channel;
        route.peer_kind = parts[2];
        route.peer_id = peer_id;
        route.account_id = account_id;
        route.thread_id = thread.thread_id;
        return route;
    }

    // 対齐:422-428 —— group/channel：{channel}:{group|channel}:{peerId...}
    const std::string peer_kind = parts[1];
    const std::string first_peer_segment = parts[2];
    const std::string peer_id = join_from(2);
    // 対齐:381-386 SESSION_DELIVERY_PEER_KINDS
    const bool kind_ok = (peer_kind == "channel" || peer_kind == "direct" ||
                          peer_kind == "dm" || peer_kind == "group");
    if (!kind_ok || first_peer_segment.empty() || peer_id.empty()) {
        return std::nullopt;
    }
    ParsedSessionDeliveryRoute route;
    route.channel = channel;
    route.peer_kind = peer_kind;
    route.peer_id = peer_id;
    route.thread_id = thread.thread_id;
    return route;
}


} // namespace sessions
} // namespace hunterclaw

#endif // HUNTERCLAW_SESSIONS_SESSION_KEY_UTILS_H
