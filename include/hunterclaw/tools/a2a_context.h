#pragma once
// ============================================================================
// A2A Context 构建器 — 严格对齐 OpenClaw sessions-send-helpers.ts
// ============================================================================
// 提供 message / ping-pong reply / announce 三阶段的 system-prompt 上下文构建。
//   buildAgentToAgentMessageContext  (helpers.ts:99-108)
//   buildAgentToAgentReplyContext    (helpers.ts:111-130)
//   buildAgentToAgentAnnounceContext (helpers.ts:133-155)
//   resolvePingPongTurns             (helpers.ts:158-160)
//
// 重要设计（対齐 helpers.ts:86-88）：
//   session key 属高基数值（含 thread/run id），直接写入 system prompt 会导致
//   provider prompt-cache 在 A2A 多轮之间无法复用，故统一替换为占位符
//   <REQUESTER_SESSION> / <TARGET_SESSION>；channel 属低基数且影响回复格式，
//   保留具体值。
// ============================================================================

#include <string>
#include <string_view>

namespace hunterclaw {
namespace tools {

/// 対齐 helpers.ts:20 DEFAULT_AGENTNG_PONG_TURNS
constexpr int DEFAULT_AGENT_PING_PONG_TURNS = 5;
/// 対齐 helpers.ts:21 MAX_PING_PONG_TURNS（硬上限）
constexpr int MAX_PING_PONG_TURNS = 20;

/// 対齐 helpers.ts:158-160 resolvePingPongTurns()
int resolve_ping_pong_turns();

/// A2A 当前参与方角色（対齐 helpers.ts:116 currentRole）
enum class A2ARole {
    Requester,  ///< "Agent 1 (requester)"
    Target,     ///< "Agent 2 (target)"
};

/// 対齐 helpers.ts:99-108 buildAgentToAgentMessageContext(params)
/// 输出示例：
///   Agent-to-agent message context:
///   Agent 1 (requester) session: <REQUESTER_SESSION>.
///   Agent 1 (requester) channel: whatsapp.
///   Agent 2 (target) session: <TARGET_SESSION>.
std::string build_agent_to_agent_message_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key);

/// 対齐 helpers.ts:111-130 buildAgentToAgentReplyContext(params)
/// 输出示例：
///   Agent-to-agent reply step:
///   Current agent: Agent 1 (requester).
///   Turn 1 of 5.
///   Agent 1 (requester) session: <REQUESTER_SESSION>.
///   ...
///   If you want to stop the ping-pong, reply exactly "REPLY_SKIP".
std::string build_agent_to_agent_reply_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key,
    std::string_view target_channel,
    A2ARole current_role,
    int turn,
    int max_turns);

/// 対齐 helpers.ts:133-155 buildAgentToAgentAnnounceContext(params)
/// 输出示例：
///   Agent-to-agent announce step:
///   ...session lines...
///   Original request: {msg}
///   Round 1 reply: {r1}            // 缺失时 "(not available)."
///   Latest reply: {latest}         // 缺失时 "(not available)."
///   If you want to remain silent, reply exactly "ANNOUNCE_SKIP".
///   Any other reply will be posted to the target channel.
///   After this reply, the agent-to-agent conversation is over.
std::string build_agent_to_agent_announce_context(
    std::string_view requester_session_key,
    std::string_view requester_channel,
    std::string_view target_session_key,
    std::string_view target_channel,
    std::string_view original_message,
    std::string_view round_one_reply,
    std::string_view latest_reply);

}  // namespace tools
}  // namespace hunterclaw
