#include "hunterclaw/hooks/message_hook_mappers.h"

namespace hunterclaw {
namespace hooks {

// ============================================================================
// Build Canonical Sent Message Context (对齐 message-hook-mappers.ts:137-161)
// ============================================================================

CanonicalSentMessageHookContext build_canonical_sent_message_context(
    const std::string& to,
    const std::string& content,
    bool success,
    const std::string& channel_id,
    const std::optional<std::string>& error,
    const std::optional<std::string>& account_id,
    const std::optional<std::string>& conversation_id,
    const std::optional<std::string>& message_id,
    const std::optional<bool>& is_group,
    const std::optional<std::string>& group_id
) {
    CanonicalSentMessageHookContext ctx;
    ctx.to = to;
    ctx.content = content;
    ctx.success = success;
    ctx.error = error;
    ctx.channel_id = channel_id;
    ctx.account_id = account_id;
    ctx.conversation_id = conversation_id.value_or(to);
    ctx.message_id = message_id;
    ctx.is_group = is_group;
    ctx.group_id = group_id;
    return ctx;
}

// ============================================================================
// Internal Hook Context Converters
// ============================================================================

// 对齐: toInternalMessageReceivedContext (message-hook-mappers.ts:309-333)
MessageReceivedContext to_internal_message_received_context(
    const CanonicalInboundMessageHookContext& c
) {
    MessageReceivedContext ctx;
    ctx.from = c.from;
    ctx.content = c.content;
    ctx.timestamp = c.timestamp;
    ctx.channel_id = c.channel_id;
    ctx.account_id = c.account_id;
    ctx.conversation_id = c.conversation_id;
    ctx.message_id = c.message_id;
    
    // Build metadata map
    auto set_meta = [&](const std::string& key, const std::optional<std::string>& val) {
        if (val && !val->empty()) {
            ctx.metadata[key] = core::JsonValue(*val);
        }
    };
    set_meta("to", c.to);
    set_meta("provider", c.provider);
    set_meta("surface", c.surface);
    set_meta("threadId", c.thread_id);
    set_meta("senderId", c.sender_id);
    set_meta("senderName", c.sender_name);
    set_meta("senderUsername", c.sender_username);
    set_meta("senderE164", c.sender_e164);
    set_meta("guildId", c.guild_id);
    set_meta("channelName", c.channel_name);
    
    return ctx;
}

// 对齐: toInternalMessageTranscribedContext (message-hook-mappers.ts:335-345)
MessageTranscribedContext to_internal_message_transcribed_context(
    const CanonicalInboundMessageHookContext& c
) {
    MessageTranscribedContext ctx;
    ctx.from = c.from;
    ctx.to = c.to;
    ctx.body = c.body;
    ctx.body_for_agent = c.body_for_agent;
    ctx.timestamp = c.timestamp;
    ctx.channel_id = c.channel_id;
    ctx.conversation_id = c.conversation_id;
    ctx.message_id = c.message_id;
    ctx.sender_id = c.sender_id;
    ctx.sender_name = c.sender_name;
    ctx.sender_username = c.sender_username;
    ctx.provider = c.provider;
    ctx.surface = c.surface;
    ctx.media_path = c.media_path;
    ctx.media_type = c.media_type;
    ctx.transcript = c.transcript.value_or("");
    return ctx;
}

// 对齐: toInternalMessagePreprocessedContext (message-hook-mappers.ts:347-359)
MessagePreprocessedContext to_internal_message_preprocessed_context(
    const CanonicalInboundMessageHookContext& c
) {
    MessagePreprocessedContext ctx;
    ctx.from = c.from;
    ctx.to = c.to;
    ctx.body = c.body;
    ctx.body_for_agent = c.body_for_agent;
    ctx.timestamp = c.timestamp;
    ctx.channel_id = c.channel_id;
    ctx.conversation_id = c.conversation_id;
    ctx.message_id = c.message_id;
    ctx.transcript = c.transcript;
    ctx.is_group = c.is_group;
    ctx.group_id = c.group_id;
    return ctx;
}

// 对齐: toInternalMessageSentContext (message-hook-mappers.ts:381-396)
MessageSentContext to_internal_message_sent_context(
    const CanonicalSentMessageHookContext& c
) {
    MessageSentContext ctx;
    ctx.to = c.to;
    ctx.content = c.content;
    ctx.success = c.success;
    ctx.error = c.error;
    ctx.channel_id = c.channel_id;
    ctx.account_id = c.account_id;
    ctx.conversation_id = c.conversation_id;
    ctx.message_id = c.message_id;
    ctx.is_group = c.is_group;
    ctx.group_id = c.group_id;
    return ctx;
}

}  // namespace hooks
}  // namespace hunterclaw
