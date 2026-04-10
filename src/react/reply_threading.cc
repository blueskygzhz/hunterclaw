#include "hunterclaw/react/reply_threading.h"
#include <algorithm>
#include <memory>

namespace hunterclaw {
namespace react {

namespace {
std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

std::string trim_opt(const std::optional<std::string>& s) {
    if (!s.has_value() || s->empty()) return "";
    auto start = s->find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s->find_last_not_of(" \t\n\r");
    return s->substr(start, end - start + 1);
}

// 对齐 reply-reference.ts: isSingleUseReplyToMode
bool is_single_use_reply_to_mode(ReplyToModeEx mode) {
    return mode == ReplyToModeEx::First || mode == ReplyToModeEx::Batched;
}
} // anon

// 对齐 reply-threading.ts:26-52
ReplyToModeEx resolve_configured_reply_to_mode(
    const std::string& /*channel*/,
    const std::string& /*chat_type*/
) {
    // Default: "all" — all payloads get reply threading
    // In production, would read from config per channel/chatType
    return ReplyToModeEx::All;
}

// 对齐 reply-threading.ts:71-79
ReplyToModeEx resolve_reply_to_mode(
    const std::string& channel,
    const std::string& /*account_id*/,
    const std::string& chat_type
) {
    return resolve_configured_reply_to_mode(channel, chat_type);
}

// 对齐 reply-threading.ts:81-123
std::function<ReplyPayload(const ReplyPayload&)> create_reply_to_mode_filter(
    ReplyToModeEx mode,
    bool allow_explicit_reply_tags_when_off
) {
    auto has_threaded = std::make_shared<bool>(false);

    return [mode, allow_explicit_reply_tags_when_off, has_threaded](
        const ReplyPayload& payload
    ) -> ReplyPayload {
        if (!payload.reply_to_id.has_value()) {
            return payload;
        }

        if (mode == ReplyToModeEx::Off) {
            // 对齐 L91-100: explicit tags bypass off-mode (except compaction notices)
            bool is_explicit = payload.reply_to_current;
            if (allow_explicit_reply_tags_when_off && is_explicit &&
                !payload.is_compaction_notice) {
                return payload;
            }
            ReplyPayload stripped = payload;
            stripped.reply_to_id = std::nullopt;
            return stripped;
        }

        if (mode == ReplyToModeEx::All) {
            return payload;
        }

        // 对齐 L105-122: first/batched modes — compaction notices always threaded
        if (is_single_use_reply_to_mode(mode) && *has_threaded) {
            if (payload.is_compaction_notice) {
                return payload;
            }
            ReplyPayload stripped = payload;
            stripped.reply_to_id = std::nullopt;
            return stripped;
        }

        // Compaction notices don't consume the "first" slot
        if (is_single_use_reply_to_mode(mode) && !payload.is_compaction_notice) {
            *has_threaded = true;
        }
        return payload;
    };
}

// 对齐 reply-threading.ts:125-137
bool resolve_implicit_current_message_reply_allowance(
    ReplyToModeEx mode,
    const std::optional<ReplyThreadingPolicy>& policy
) {
    std::string implicit_current = policy.has_value() ?
        policy->implicit_current_message : "default";

    if (implicit_current == "allow") return true;
    if (implicit_current == "deny") return false;
    return mode != ReplyToModeEx::Batched;
}

// 对齐 reply-threading.ts:139-149
std::optional<ReplyThreadingPolicy> resolve_batched_reply_threading_policy(
    ReplyToModeEx mode,
    bool is_batched
) {
    if (mode != ReplyToModeEx::Batched) return std::nullopt;
    ReplyThreadingPolicy policy;
    policy.implicit_current_message = is_batched ? "allow" : "deny";
    return policy;
}

// 对齐 reply-threading.ts:151-163
std::function<ReplyPayload(const ReplyPayload&)> create_reply_to_mode_filter_for_channel(
    ReplyToModeEx mode,
    const std::string& channel
) {
    auto normalized = to_lower(channel);
    bool is_webchat = (normalized == "webchat");
    // Default: allow explicit reply tags even when off
    bool allow_explicit = !normalized.empty() || is_webchat;
    return create_reply_to_mode_filter(mode, allow_explicit);
}

// ============================================================================
// 对齐 origin-routing.ts (27行)
// ============================================================================
std::optional<std::string> resolve_origin_message_provider(
    const std::optional<std::string>& originating_channel,
    const std::optional<std::string>& provider
) {
    auto ch = trim_opt(originating_channel);
    if (!ch.empty()) return to_lower(ch);
    auto prov = trim_opt(provider);
    if (!prov.empty()) return to_lower(prov);
    return std::nullopt;
}

std::optional<std::string> resolve_origin_message_to(
    const std::optional<std::string>& originating_to,
    const std::optional<std::string>& to
) {
    if (originating_to.has_value() && !originating_to->empty()) return originating_to;
    if (to.has_value() && !to->empty()) return to;
    return std::nullopt;
}

std::optional<std::string> resolve_origin_account_id(
    const std::optional<std::string>& originating_account_id,
    const std::optional<std::string>& account_id
) {
    if (originating_account_id.has_value() && !originating_account_id->empty())
        return originating_account_id;
    if (account_id.has_value() && !account_id->empty()) return account_id;
    return std::nullopt;
}

} // namespace react
} // namespace hunterclaw
