// Aligned with openclaw: src/auto-reply/reply/conversation-label-generator.ts (87行)
// 会话标签自动生成 — 使用 LLM 从用户首条消息生成简短标签
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// ConversationLabelParams (对齐 OC: ConversationLabelParams)
// ============================================================================

struct ConversationLabelParams {
    std::string user_message;
    std::string prompt;
    std::string config_json;
    std::string agent_id;
    std::string agent_dir;
    int max_length = 128;          // DEFAULT_MAX_LABEL_LENGTH
    int timeout_ms = 15000;        // TIMEOUT_MS = 15_000
};

// ============================================================================
// generateConversationLabel (对齐 OC: generateConversationLabel)
// ============================================================================

/// Generate a short conversation label from the user's first message.
/// Uses a simple LLM completion with low temperature (0.3).
/// Returns nullopt on failure or timeout.
/// 对齐 OC: generateConversationLabel (conversation-label-generator.ts:25-86)
std::optional<std::string> generate_conversation_label(
    const ConversationLabelParams& params);

/// Async version with callback (for non-blocking usage in the dispatcher).
using ConversationLabelCallback =
    std::function<void(const std::optional<std::string>& label)>;

void generate_conversation_label_async(
    const ConversationLabelParams& params,
    ConversationLabelCallback callback);

}  // namespace auto_reply
}  // namespace hunterclaw
