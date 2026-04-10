#include "hunterclaw/react/reply_payloads.h"
#include "hunterclaw/react/execution_guard.h"
#include <algorithm>
#include <sstream>
#include <functional>

namespace hunterclaw {
namespace react {

namespace {
std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

bool has_media(const ReplyPayload& p) {
    return p.media_url.has_value() || !p.media_urls.empty();
}

// Normalize text for comparison (lowercase, collapse whitespace)
std::string normalize_for_compare(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    bool last_space = false;
    for (char c : to_lower(text)) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!last_space && !result.empty()) {
                result.push_back(' ');
                last_space = true;
            }
        } else {
            result.push_back(c);
            last_space = false;
        }
    }
    // Trim trailing space
    if (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}
} // anon

// ============================================================================
// 对齐 reply-payloads-base.ts: isRenderablePayload
// ============================================================================
bool is_renderable_payload(const ReplyPayload& payload) {
    if (has_media(payload)) return true;
    if (payload.interactive) return true;
    auto trimmed = trim(payload.text);
    return !trimmed.empty();
}

// ============================================================================
// 对齐 reply-payloads-base.ts: applyReplyThreading
// ============================================================================
std::vector<ReplyPayload> apply_reply_threading(
    const std::vector<ReplyPayload>& payloads,
    ReplyToMode mode,
    const std::optional<std::string>& current_message_id
) {
    if (mode == ReplyToMode::Off || !current_message_id.has_value() ||
        current_message_id->empty()) {
        return payloads;
    }

    std::vector<ReplyPayload> result;
    result.reserve(payloads.size());
    bool first_tagged = false;

    for (const auto& p : payloads) {
        ReplyPayload tagged = p;
        if (mode == ReplyToMode::All ||
            (mode == ReplyToMode::First && !first_tagged)) {
            if (!tagged.reply_to_id.has_value()) {
                tagged.reply_to_id = *current_message_id;
                tagged.reply_to_current = true;
            }
            first_tagged = true;
        }
        result.push_back(std::move(tagged));
    }
    return result;
}

// ============================================================================
// 对齐 block-reply-pipeline.ts: createBlockReplyContentKey
// ============================================================================
std::string create_block_reply_content_key(const ReplyPayload& payload) {
    // Content-only key for dedup (ignores replyToId)
    std::ostringstream oss;
    oss << "{\"text\":\"" << trim(payload.text) << "\",\"mediaList\":[";
    for (size_t i = 0; i < payload.media_urls.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << payload.media_urls[i] << "\"";
    }
    if (payload.media_url.has_value()) {
        if (!payload.media_urls.empty()) oss << ",";
        oss << "\"" << *payload.media_url << "\"";
    }
    oss << "]}";
    return oss.str();
}

// ============================================================================
// 对齐 reply-payloads-dedupe.ts: filterMessagingToolDuplicates
// ============================================================================
std::vector<ReplyPayload> filter_messaging_tool_duplicates(
    const std::vector<ReplyPayload>& payloads,
    const std::vector<std::string>& sent_texts
) {
    if (sent_texts.empty()) return payloads;

    // Normalize all sent texts
    std::set<std::string> normalized_sent;
    for (const auto& t : sent_texts) {
        auto n = normalize_for_compare(t);
        if (!n.empty()) normalized_sent.insert(n);
    }

    std::vector<ReplyPayload> result;
    for (const auto& p : payloads) {
        auto normalized = normalize_for_compare(p.text);
        if (normalized.empty() || normalized_sent.find(normalized) == normalized_sent.end()) {
            result.push_back(p);
        }
    }
    return result;
}

// ============================================================================
// 对齐 reply-payloads-dedupe.ts: filterMessagingToolMediaDuplicates
// ============================================================================
std::vector<ReplyPayload> filter_messaging_tool_media_duplicates(
    const std::vector<ReplyPayload>& payloads,
    const std::vector<std::string>& sent_media_urls
) {
    if (sent_media_urls.empty()) return payloads;

    std::set<std::string> sent_set(sent_media_urls.begin(), sent_media_urls.end());

    std::vector<ReplyPayload> result;
    for (const auto& p : payloads) {
        bool is_media_dup = false;
        if (has_media(p) && trim(p.text).empty()) {
            // Media-only payload — check if all media URLs already sent
            bool all_sent = true;
            if (p.media_url.has_value() &&
                sent_set.find(*p.media_url) == sent_set.end()) {
                all_sent = false;
            }
            for (const auto& url : p.media_urls) {
                if (sent_set.find(url) == sent_set.end()) {
                    all_sent = false;
                    break;
                }
            }
            is_media_dup = all_sent && (p.media_url.has_value() || !p.media_urls.empty());
        }
        if (!is_media_dup) {
            result.push_back(p);
        }
    }
    return result;
}

// ============================================================================
// 对齐 agent-runner-payloads.ts:90-234: buildReplyPayloads
// ============================================================================
BuildReplyPayloadsResult build_reply_payloads(const BuildReplyPayloadsParams& params) {
    BuildReplyPayloadsResult output;
    output.did_log_heartbeat_strip = params.did_log_heartbeat_strip;

    // Step 1: Strip heartbeat tokens (对齐 L113-135)
    std::vector<ReplyPayload> sanitized;
    if (params.is_heartbeat) {
        sanitized = params.payloads;
    } else {
        for (const auto& payload : params.payloads) {
            std::string text = payload.text;
            if (text.find("HEARTBEAT_OK") == std::string::npos) {
                sanitized.push_back(payload);
                continue;
            }
            auto stripped = strip_heartbeat_token(text);
            if (stripped.did_strip && !output.did_log_heartbeat_strip) {
                output.did_log_heartbeat_strip = true;
            }
            if (stripped.should_skip && !has_media(payload)) continue;
            ReplyPayload p = payload;
            p.text = stripped.text;
            sanitized.push_back(p);
        }
    }

    // Step 2: Apply reply threading (对齐 L137-158)
    auto threaded = apply_reply_threading(
        sanitized, params.reply_to_mode, params.current_message_id);

    // Step 3: Normalize media paths (对齐 L146-155)
    std::vector<ReplyPayload> normalized;
    for (const auto& p : threaded) {
        if (params.normalize_media_paths && has_media(p)) {
            try {
                normalized.push_back(params.normalize_media_paths(p));
            } catch (...) {
                ReplyPayload stripped = p;
                stripped.media_url = std::nullopt;
                stripped.media_urls.clear();
                normalized.push_back(stripped);
            }
        } else {
            normalized.push_back(p);
        }
    }

    // Step 4: Filter non-renderable (对齐 L158)
    std::vector<ReplyPayload> renderable;
    for (const auto& p : normalized) {
        if (is_renderable_payload(p)) renderable.push_back(p);
    }

    // Step 5: Silent filter (对齐 L159)
    auto filtered = params.silent_expected ?
        std::vector<ReplyPayload>{} : renderable;

    // Step 6: Messaging tool dedup (对齐 L167-227)
    auto deduped = filter_messaging_tool_duplicates(
        filtered, params.messaging_tool_sent_texts);
    auto media_deduped = filter_messaging_tool_media_duplicates(
        deduped, params.messaging_tool_sent_media_urls);

    // Step 7: Block streaming dedup (对齐 L217-227)
    bool should_drop_final = params.block_streaming_enabled &&
        params.block_pipeline_did_stream &&
        !params.block_pipeline_is_aborted;

    if (should_drop_final) {
        output.reply_payloads = {};
    } else if (params.block_streaming_enabled && params.has_sent_payload) {
        for (const auto& p : media_deduped) {
            if (!params.has_sent_payload(p)) {
                output.reply_payloads.push_back(p);
            }
        }
    } else if (!params.directly_sent_block_keys.empty()) {
        for (const auto& p : media_deduped) {
            auto key = create_block_reply_content_key(p);
            if (params.directly_sent_block_keys.find(key) ==
                params.directly_sent_block_keys.end()) {
                output.reply_payloads.push_back(p);
            }
        }
    } else {
        output.reply_payloads = media_deduped;
    }

    return output;
}

} // namespace react
} // namespace hunterclaw
