#pragma once

// 严格对齐 OpenClaw: channels/conversation-binding-context.ts (243行)
// Conversation binding context — resolves channel/accountId/conversationId/threadId for message routing

#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace channels {

// ============================================================================
// ConversationBindingContext (对齐 OC: ConversationBindingContext)
// ============================================================================

struct ConversationBindingContext {
    std::string channel;
    std::string account_id;
    std::string conversation_id;
    std::optional<std::string> parent_conversation_id;
    std::optional<std::string> thread_id;
};

// ============================================================================
// ResolveConversationBindingContextInput (对齐 OC)
// ============================================================================

struct ResolveConversationBindingContextInput {
    core::Config cfg;
    std::optional<std::string> channel;
    std::optional<std::string> account_id;
    std::optional<std::string> chat_type;
    std::optional<std::string> thread_id;
    std::optional<std::string> thread_parent_id;
    std::optional<std::string> sender_id;
    std::optional<std::string> session_key;
    std::optional<std::string> parent_session_key;
    std::optional<std::string> originating_to;
    std::optional<std::string> command_to;
    std::optional<std::string> fallback_to;
    std::optional<std::string> from;
    std::optional<std::string> native_channel_id;
};

// ============================================================================
// resolve_conversation_binding_context (对齐 OC: resolveConversationBindingContext)
//
// Resolution order:
// 1. Plugin bindings.resolveCommandConversation() hook
// 2. Plugin threading.resolveFocusedBinding() hook
// 3. Generic: channel target id from originating_to / command_to / fallback_to
// 4. Thread parent fallback
// ============================================================================

std::optional<ConversationBindingContext> resolve_conversation_binding_context(
    const ResolveConversationBindingContextInput& params);

// ============================================================================
// Helpers (internal, exposed for testing)
// ============================================================================

/// Resolve channel target id stripping canonical prefixes (user:/channel:/group:/etc.)
/// (对齐 OC: resolveChannelTargetId)
std::optional<std::string> resolve_channel_target_id(
    const std::string& channel,
    const std::optional<std::string>& target);

} // namespace channels
} // namespace hunterclaw
