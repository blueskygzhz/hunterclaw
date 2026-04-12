// ============================================================================
// outbound_session.cc
// 严格对齐 OpenClaw: infra/outbound/outbound-session.ts (182行)
//
// 出站会话路由解析:
//   - resolveOutboundSessionRoute: 解析出站消息的 session key + peer
//   - stripProviderPrefix: 去除 provider 前缀 (e.g. "telegram:xxx" → "xxx")
//   - stripKindPrefix: 去除 kind 前缀 (e.g. "user:xxx" → "xxx")
//   - inferPeerKind: 推断 peer 类型 (direct/group/channel)
//   - buildBaseSessionKey: 构建 base session key
//   - resolveFallbackSession: 回退会话解析
// ============================================================================

#include "hunterclaw/core/types.h"
#include <string>
#include <optional>
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace outbound {

namespace {

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// 对齐 outbound-session.ts:41-49
std::string strip_provider_prefix(const std::string& raw, const std::string& channel) {
    auto trimmed = trim(raw);
    auto lower = to_lower(trimmed);
    auto prefix = to_lower(channel) + ":";
    if (lower.size() > prefix.size() && lower.substr(0, prefix.size()) == prefix) {
        return trim(trimmed.substr(prefix.size()));
    }
    return trimmed;
}

// 对齐 outbound-session.ts:51-53
std::string strip_kind_prefix(const std::string& raw) {
    static const std::regex kind_re("^(user|channel|group|conversation|room|dm):",
                                     std::regex::icase);
    return trim(std::regex_replace(raw, kind_re, ""));
}

} // namespace

// ============================================================================
// Types (对齐 outbound-session.ts:15-23)
// ============================================================================

struct RoutePeer {
    std::string kind;  // "direct" | "group" | "channel"
    std::string id;
};

struct OutboundSessionRoute {
    std::string session_key;
    std::string base_session_key;
    RoutePeer peer;
    std::string chat_type;  // "direct" | "group" | "channel"
    std::string from;
    std::string to;
    std::optional<std::string> thread_id;
};

// ============================================================================
// inferPeerKind (对齐 outbound-session.ts:55-77)
// ============================================================================

static std::string infer_peer_kind(
    const std::string& /*channel*/,
    const std::optional<std::string>& resolved_kind
) {
    if (resolved_kind) {
        if (*resolved_kind == "user") return "direct";
        if (*resolved_kind == "channel") return "channel";
        if (*resolved_kind == "group") return "group";
    }
    return "direct";
}

// ============================================================================
// buildBaseSessionKey (对齐 outbound-session.ts:79-94)
// ============================================================================

static std::string build_base_session_key(
    const std::string& agent_id,
    const std::string& channel,
    const std::optional<std::string>& account_id,
    const RoutePeer& peer
) {
    // 对齐 OC: buildAgentSessionKey
    std::string key = agent_id + ":" + channel;
    if (account_id && !account_id->empty()) {
        key += ":" + *account_id;
    }
    key += ":" + peer.kind + ":" + peer.id;
    return key;
}

// ============================================================================
// resolveFallbackSession (对齐 outbound-session.ts:96-133)
// ============================================================================

static std::optional<OutboundSessionRoute> resolve_fallback_session(
    const std::string& agent_id,
    const std::string& channel,
    const std::string& target,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& resolved_kind
) {
    auto trimmed = strip_provider_prefix(target, channel);
    if (trimmed.empty()) return std::nullopt;

    auto peer_kind = infer_peer_kind(channel, resolved_kind);
    auto peer_id = strip_kind_prefix(trimmed);
    if (peer_id.empty()) return std::nullopt;

    RoutePeer peer{peer_kind, peer_id};
    auto base_key = build_base_session_key(agent_id, channel, account_id, peer);

    auto chat_type = (peer_kind == "direct") ? "direct" :
                     (peer_kind == "channel") ? "channel" : "group";

    auto from = (peer_kind == "direct")
        ? (channel + ":" + peer_id)
        : (channel + ":" + peer_kind + ":" + peer_id);

    auto to_prefix = (peer_kind == "direct") ? "user" : "channel";

    return OutboundSessionRoute{
        base_key,
        base_key,
        peer,
        chat_type,
        from,
        to_prefix + std::string(":") + peer_id,
        std::nullopt
    };
}

// ============================================================================
// resolveOutboundSessionRoute (对齐 outbound-session.ts:135-149)
// ============================================================================

std::optional<OutboundSessionRoute> resolve_outbound_session_route(
    const std::string& agent_id,
    const std::string& channel,
    const std::string& target,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& resolved_kind
) {
    auto trimmed = trim(target);
    if (trimmed.empty()) return std::nullopt;

    // In C++, we don't have dynamic plugin resolver; use fallback
    return resolve_fallback_session(agent_id, channel, trimmed, account_id, resolved_kind);
}

} // namespace outbound
} // namespace hunterclaw
