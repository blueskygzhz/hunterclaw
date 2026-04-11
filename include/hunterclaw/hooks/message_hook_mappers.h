#pragma once
// Aligned with openclaw: src/hooks/message-hook-mappers.ts (397 lines)
// Maps auto-reply inbound/sent contexts to canonical hook contexts,
// then to internal-hook and plugin-hook event payloads.

#include "hunterclaw/hooks/hooks.h"
#include "hunterclaw/core/json.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Canonical Inbound Message Hook Context (对齐 message-hook-mappers.ts:22-51)
// ============================================================================

struct CanonicalInboundMessageHookContext {
    std::string from;
    std::optional<std::string> to;
    std::string content;
    std::optional<std::string> body;
    std::optional<std::string> body_for_agent;
    std::optional<std::string> transcript;
    std::optional<int64_t> timestamp;
    std::string channel_id;
    std::optional<std::string> account_id;
    std::optional<std::string> conversation_id;
    std::optional<std::string> message_id;
    std::optional<std::string> sender_id;
    std::optional<std::string> sender_name;
    std::optional<std::string> sender_username;
    std::optional<std::string> sender_e164;
    std::optional<std::string> provider;
    std::optional<std::string> surface;
    std::optional<std::string> thread_id;
    std::optional<std::string> media_path;
    std::optional<std::string> media_type;
    std::vector<std::string> media_paths;
    std::vector<std::string> media_types;
    std::optional<std::string> originating_channel;
    std::optional<std::string> originating_to;
    std::optional<std::string> guild_id;
    std::optional<std::string> channel_name;
    bool is_group = false;
    std::optional<std::string> group_id;
};

// ============================================================================
// Canonical Sent Message Hook Context (对齐 message-hook-mappers.ts:53-64)
// ============================================================================

struct CanonicalSentMessageHookContext {
    std::string to;
    std::string content;
    bool success = true;
    std::optional<std::string> error;
    std::string channel_id;
    std::optional<std::string> account_id;
    std::optional<std::string> conversation_id;
    std::optional<std::string> message_id;
    std::optional<bool> is_group;
    std::optional<std::string> group_id;
};

// ============================================================================
// Context Builders (对齐 message-hook-mappers.ts:137-161)
// ============================================================================

/// Build canonical sent message context (对齐: buildCanonicalSentMessageHookContext)
CanonicalSentMessageHookContext build_canonical_sent_message_context(
    const std::string& to,
    const std::string& content,
    bool success,
    const std::string& channel_id,
    const std::optional<std::string>& error = std::nullopt,
    const std::optional<std::string>& account_id = std::nullopt,
    const std::optional<std::string>& conversation_id = std::nullopt,
    const std::optional<std::string>& message_id = std::nullopt,
    const std::optional<bool>& is_group = std::nullopt,
    const std::optional<std::string>& group_id = std::nullopt);

// ============================================================================
// Internal Hook Context Converters (对齐 message-hook-mappers.ts:309-396)
// ============================================================================

/// Convert canonical inbound to MessageReceivedHookContext
/// 对齐: toInternalMessageReceivedContext
MessageReceivedContext to_internal_message_received_context(
    const CanonicalInboundMessageHookContext& canonical);

/// Convert canonical inbound to MessageTranscribedHookContext
/// 对齐: toInternalMessageTranscribedContext
MessageTranscribedContext to_internal_message_transcribed_context(
    const CanonicalInboundMessageHookContext& canonical);

/// Convert canonical inbound to MessagePreprocessedHookContext
/// 对齐: toInternalMessagePreprocessedContext
MessagePreprocessedContext to_internal_message_preprocessed_context(
    const CanonicalInboundMessageHookContext& canonical);

/// Convert canonical sent to MessageSentHookContext
/// 对齐: toInternalMessageSentContext
MessageSentContext to_internal_message_sent_context(
    const CanonicalSentMessageHookContext& canonical);

}  // namespace hooks
}  // namespace hunterclaw
