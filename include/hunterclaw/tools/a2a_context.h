#pragma once
// ============================================================================
// A2A Context 构建器 — 严格对齐 OpenClaw sessions-send-helpers.ts
// ============================================================================
// 提供 ping-pong 和 announce 阶段的消息上下文构建函数。
// buildAgentToAgentMessageContext / ReplyContext / AnnounceContext
// ============================================================================

#include <string>
#include <string_view>

namespace hunterclaw {
namespace tools {

/// 对齐 OpenClaw: buildAgentToAgentMessageContext(params)
/// 示例输出:
///   "[SYSTEM: This is a message from Agent <sourceName>
///   (<sourceSessionKey>).  If you want to reply, include ONLY the reply
///   in your response. ...]"
std::string build_agent_to_agent_message_context(
    std::string_view source_session_key,
    std::string_view agent_name_or_id,
    std::string_view message);

/// 对齐 OpenClaw: buildAgentToAgentReplyContext(params)
/// Ping-pong 阶段:"Agent-to-agent reply step: Turn N of 5\n\n{reply}"
std::string build_agent_to_agent_reply_context(
    int turn,
    int max_turns,
    std::string_view reply_text);

/// 对齐 OpenClaw: buildAgentToAgentAnnounceContext(params)
/// Announce 阶段: "Agent-to-agent announce step: Original request: {msg}"
std::string build_agent_to_agent_announce_context(
    std::string_view original_message);

}  // namespace tools
}  // namespace hunterclaw
