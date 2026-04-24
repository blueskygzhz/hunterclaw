#include "hunterclaw/react/normalize_reply.h"
#include "hunterclaw/react/execution_guard.h"
#include <algorithm>

namespace hunterclaw {
namespace react {

namespace {
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

bool has_reply_payload_content(const ReplyPayload& p) {
    if (!p.media_urls.empty() || p.media_url.has_value()) return true;
    if (p.interactive) return true;
    if (p.audio_as_voice) return true;
    return !trim(p.text).empty();
}

// 对齐 tokens.ts: isSilentReplyPayloadText — checks if text is purely silent
bool is_silent_reply_payload_text(const std::string& text, const std::string& token) {
    auto trimmed = trim(text);
    if (trimmed == token) return true;
    // Also match "[SILENT]" legacy
    if (trimmed == "[SILENT]") return true;
    return false;
}

// 对齐 tokens.ts: stripSilentToken — remove ALL occurrences
std::string strip_silent_token(const std::string& text, const std::string& token) {
    std::string result = text;
    size_t pos;
    while ((pos = result.find(token)) != std::string::npos) {
        result.erase(pos, token.size());
    }
    return result;
}
} // anon

// 对齐 normalize-reply.ts:34-128
std::optional<ReplyPayload> normalize_reply_payload(
    const ReplyPayload& payload,
    const NormalizeReplyOptions& opts
) {
    const std::string& silent_token = opts.silent_token.empty() ? "NO_REPLY" : opts.silent_token;

    // 对齐 L49-53: empty check
    auto trimmed = trim(payload.text);
    ReplyPayload check_payload = payload;
    check_payload.text = trimmed;
    if (!has_reply_payload_content(check_payload)) {
        if (opts.on_skip) opts.on_skip(NormalizeReplySkipReason::Empty);
        return std::nullopt;
    }

    // 对齐 L56-78: silent token handling
    std::string text = payload.text;

    // 对齐 L57-63: exact silent match
    if (!text.empty() && is_silent_reply_payload_text(text, silent_token)) {
        ReplyPayload empty_check = payload;
        empty_check.text = "";
        if (!has_reply_payload_content(empty_check)) {
            if (opts.on_skip) opts.on_skip(NormalizeReplySkipReason::Silent);
            return std::nullopt;
        }
        text = "";
    }

    // 对齐 L67-78: strip NO_REPLY from mixed content (e.g. "😄 NO_REPLY")
    if (!text.empty() && !is_silent_reply_text(text)) {
        bool has_leading = (text.find(silent_token) == 0);
        if (has_leading) {
            text = strip_leading_silent_token(text);
        }
        if (has_leading || text.find(silent_token) != std::string::npos) {
            text = strip_silent_token(text, silent_token);
            ReplyPayload stripped_check = payload;
            stripped_check.text = text;
            if (!has_reply_payload_content(stripped_check)) {
                if (opts.on_skip) opts.on_skip(NormalizeReplySkipReason::Silent);
                return std::nullopt;
            }
        }
    }

    // 对齐 L80-83: empty text with media → keep empty
    if (!text.empty() && trimmed.empty()) {
        text = "";
    }

    // 对齐 L85-96: heartbeat strip (对齐 OC: shouldStripHeartbeat = opts.stripHeartbeat ?? true)
    // HC: strip_heartbeat 默认值 true 已在 NormalizeReplyOptions 中设置，对齐 OC 默认行为
    if (opts.strip_heartbeat && !text.empty() && text.find("HEARTBEAT_OK") != std::string::npos) {
        auto stripped = strip_heartbeat_token(text);
        if (stripped.did_strip && opts.on_heartbeat_strip) {
            opts.on_heartbeat_strip();
        }
        ReplyPayload hb_check = payload;
        hb_check.text = stripped.text;
        if (stripped.should_skip && !has_reply_payload_content(hb_check)) {
            if (opts.on_skip) opts.on_skip(NormalizeReplySkipReason::Heartbeat);
            return std::nullopt;
        }
        text = stripped.text;
    }

    // 对齐 L98-100: sanitize
    if (!text.empty()) {
        text = sanitize_user_facing_text(text, payload.is_error);
    }

    // 对齐 L101-104: final empty check
    {
        ReplyPayload final_check = payload;
        final_check.text = text;
        if (!has_reply_payload_content(final_check)) {
            if (opts.on_skip) opts.on_skip(NormalizeReplySkipReason::Empty);
            return std::nullopt;
        }
    }

    // 对齐 L106-110: channel transforms
    ReplyPayload enriched = payload;
    enriched.text = text;
    if (opts.apply_channel_transforms && opts.transform_reply_payload) {
        enriched = opts.transform_reply_payload(enriched);
        text = enriched.text;
    }

    // 对齐 L112-124: response prefix
    std::optional<std::string> effective_prefix;
    if (opts.response_prefix_context.has_value()) {
        effective_prefix = resolve_response_prefix_template(
            opts.response_prefix, *opts.response_prefix_context);
    } else {
        effective_prefix = opts.response_prefix;
    }

    if (effective_prefix.has_value() && !effective_prefix->empty() &&
        !text.empty() && trim(text) != "HEARTBEAT_OK" &&
        text.find(*effective_prefix) != 0) {
        text = *effective_prefix + " " + text;
    }

    enriched.text = text;
    return enriched;
}

} // namespace react
} // namespace hunterclaw
