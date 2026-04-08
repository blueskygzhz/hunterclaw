// Aligned with openclaw: src/auto-reply/reply/inbound-context.ts
#pragma once

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// InboundContext
// Aligned with openclaw: MsgContext / FinalizedMsgContext (templating.ts)
// Represents a normalized inbound message with all derived fields.
// ============================================================================

struct InboundContext {
    // Core message fields
    std::string body;
    std::string raw_body;
    std::string command_body;
    std::string transcript;
    std::string body_for_agent;
    std::string body_for_commands;

    // Sender/recipient
    std::string from;
    std::string to;
    std::string session_key;
    std::string account_id;
    std::string sender_name;
    std::string sender_id;
    std::string sender_e164;
    std::string message_sid;

    // Chat type / group
    std::string chat_type;          // "direct" | "group" | "thread"
    std::string conversation_label;
    std::string group_subject;
    std::string group_members;

    // Reply threading
    std::string reply_to_id;
    std::string reply_to_body;
    std::string reply_to_sender;

    // Media
    std::string media_path;
    std::string media_url;
    std::string media_type;
    std::vector<std::string> media_paths;
    std::vector<std::string> media_urls;
    std::vector<std::string> media_types;

    // Provider / channel
    std::string provider;
    std::string surface;
    std::string originating_channel;
    std::string originating_to;

    // Authorization
    bool command_authorized = false;
    bool was_mentioned = false;

    // Command source (native slash command vs text)
    std::string command_source;
    std::string command_target_session_key;
};

// ============================================================================
// FinalizeInboundContextOptions
// Aligned with openclaw: FinalizeInboundContextOptions
// ============================================================================

struct FinalizeInboundContextOptions {
    bool force_body_for_agent = false;
    bool force_body_for_commands = false;
    bool force_chat_type = false;
    bool force_conversation_label = false;
};

// ============================================================================
// finalize_inbound_context
// Core pipeline function: normalizes text, resolves chat type, derives
// BodyForAgent/BodyForCommands, aligns media types.
// ============================================================================

InboundContext finalize_inbound_context(
    const InboundContext& ctx,
    const FinalizeInboundContextOptions& opts = {}
);

} // namespace auto_reply
} // namespace hunterclaw
