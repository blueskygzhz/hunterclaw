// Aligned with openclaw: src/auto-reply/reply/inbound-context.ts
// Finalizes inbound message context: text normalization, system tag sanitization,
// ChatType resolution, BodyForAgent/BodyForCommands derivation, MediaType alignment.

#include "hunterclaw/auto_reply/inbound_context.h"
#include <regex>
#include <algorithm>

namespace hunterclaw {
namespace auto_reply {

static const std::string DEFAULT_MEDIA_TYPE = "application/octet-stream";

// ============================================================================
// Text normalization helpers
// Aligned with openclaw: normalizeInboundTextNewlines + sanitizeInboundSystemTags
// ============================================================================

// Normalize newlines: \r\n → \n, \r → \n
static std::string normalize_newlines(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            result += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;  // skip \n after \r
            }
        } else {
            result += text[i];
        }
    }
    return result;
}

// Sanitize system tags: neutralize <|...|> injection attempts
// Aligned with openclaw: sanitizeInboundSystemTags
static std::string sanitize_system_tags(const std::string& text) {
    if (text.empty()) return text;

    // Replace <|...|> patterns that could inject system-level markers
    static const std::regex system_tag_re(R"(<\|[^|]*\|>)");
    std::string result = std::regex_replace(text, system_tag_re, "[system-tag-removed]");

    // Also strip \x00 null bytes
    result.erase(
        std::remove(result.begin(), result.end(), '\0'),
        result.end()
    );

    return result;
}

// Full text normalization pipeline
static std::string normalize_text_field(const std::string& text) {
    if (text.empty()) return text;
    return sanitize_system_tags(normalize_newlines(text));
}

// ============================================================================
// normalize_chat_type
// Aligned with openclaw: normalizeChatType (channels/chat-type.ts)
// ============================================================================

static std::string normalize_chat_type(const std::string& raw) {
    if (raw.empty()) return "";
    std::string lower = raw;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // Trim
    size_t s = lower.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    size_t e = lower.find_last_not_of(" \t\r\n");
    lower = lower.substr(s, e - s + 1);

    if (lower == "group" || lower == "channel" || lower == "room" ||
        lower == "supergroup" || lower == "megagroup") {
        return "group";
    }
    if (lower == "direct" || lower == "dm" || lower == "private" ||
        lower == "im" || lower == "personal") {
        return "direct";
    }
    if (lower == "thread") {
        return "thread";
    }
    return lower;
}

// ============================================================================
// resolve_conversation_label
// Aligned with openclaw: resolveConversationLabel (channels/conversation-label.ts)
// ============================================================================

static std::string resolve_conversation_label(const InboundContext& ctx) {
    if (!ctx.group_subject.empty()) return ctx.group_subject;
    if (!ctx.from.empty()) return ctx.from;
    if (!ctx.sender_name.empty()) return ctx.sender_name;
    return "";
}

// ============================================================================
// finalize_inbound_context
// Aligned with openclaw: finalizeInboundContext (reply/inbound-context.ts)
//
// Core inbound message standardization:
//   1. Normalize Body/RawBody/CommandBody newlines + sanitize system tags
//   2. Resolve ChatType → normalized form
//   3. Derive BodyForAgent (RawBody → CommandBody → Body fallback chain)
//   4. Derive BodyForCommands (CommandBody → RawBody → Body fallback chain)
//   5. Default CommandAuthorized to false
//   6. Align MediaType/MediaTypes with media entry count
// ============================================================================

InboundContext finalize_inbound_context(
    const InboundContext& ctx,
    const FinalizeInboundContextOptions& opts
) {
    InboundContext result = ctx;

    // Step 1: Normalize text fields
    result.body = sanitize_system_tags(normalize_newlines(result.body));
    result.raw_body = normalize_text_field(result.raw_body);
    result.command_body = normalize_text_field(result.command_body);
    result.transcript = normalize_text_field(result.transcript);

    // Step 2: Resolve ChatType
    std::string chat_type = normalize_chat_type(result.chat_type);
    if (!chat_type.empty() &&
        (opts.force_chat_type || result.chat_type != chat_type)) {
        result.chat_type = chat_type;
    }

    // Step 3: Derive BodyForAgent
    // Aligned with openclaw priority chain:
    //   forceBodyForAgent ? Body : (BodyForAgent ?? CommandBody ?? RawBody ?? Body)
    if (opts.force_body_for_agent) {
        result.body_for_agent = sanitize_system_tags(
            normalize_newlines(result.body));
    } else {
        const std::string& source =
            !result.body_for_agent.empty() ? result.body_for_agent :
            !result.command_body.empty() ? result.command_body :
            !result.raw_body.empty() ? result.raw_body :
            result.body;
        result.body_for_agent = sanitize_system_tags(normalize_newlines(source));
    }

    // Step 4: Derive BodyForCommands
    // Aligned with openclaw priority chain:
    //   forceBodyForCommands ? (CommandBody ?? RawBody ?? Body) :
    //                          (BodyForCommands ?? CommandBody ?? RawBody ?? Body)
    if (opts.force_body_for_commands) {
        const std::string& source =
            !result.command_body.empty() ? result.command_body :
            !result.raw_body.empty() ? result.raw_body :
            result.body;
        result.body_for_commands = sanitize_system_tags(normalize_newlines(source));
    } else {
        const std::string& source =
            !result.body_for_commands.empty() ? result.body_for_commands :
            !result.command_body.empty() ? result.command_body :
            !result.raw_body.empty() ? result.raw_body :
            result.body;
        result.body_for_commands = sanitize_system_tags(normalize_newlines(source));
    }

    // Step 5: Resolve ConversationLabel
    if (opts.force_conversation_label || result.conversation_label.empty()) {
        std::string resolved = resolve_conversation_label(result);
        if (!resolved.empty()) {
            result.conversation_label = resolved;
        }
    }

    // Step 6: Default CommandAuthorized to false (default-deny)
    // Aligned with openclaw: normalized.CommandAuthorized = normalized.CommandAuthorized === true

    // Step 7: Align MediaType/MediaTypes
    // Count media entries (aligned with openclaw: countMediaEntries)
    size_t media_count = 0;
    if (!result.media_paths.empty()) {
        media_count = std::max(media_count, result.media_paths.size());
    }
    if (!result.media_urls.empty()) {
        media_count = std::max(media_count, result.media_urls.size());
    }
    if (media_count == 0 && (!result.media_path.empty() || !result.media_url.empty())) {
        media_count = 1;
    }

    if (media_count > 0) {
        // Pad MediaTypes to match media_count
        if (result.media_types.empty() && !result.media_type.empty()) {
            result.media_types.push_back(result.media_type);
        }
        while (result.media_types.size() < media_count) {
            result.media_types.push_back(DEFAULT_MEDIA_TYPE);
        }
        // Ensure MediaType is set
        if (result.media_type.empty()) {
            result.media_type = result.media_types.empty()
                ? DEFAULT_MEDIA_TYPE
                : result.media_types[0];
        }
    }

    return result;
}

} // namespace auto_reply
} // namespace hunterclaw
