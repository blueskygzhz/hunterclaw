#pragma once
// ============================================================================
// sessions_send A2A Flow — 严格对齐 OpenClaw sessions-send-tool.a2a.ts
// ============================================================================
// 在 sessions_send (timeout=0) 触发后，以后台线程运行 ping-pong + announce 流程。
// 包含: round-1 reply 获取、ping-pong 多轮交互、announce 判断与投递。
// ============================================================================

#include "hunterclaw/gateway/client.h"
#include <string>
#include <memory>

namespace hunterclaw {
namespace tools {

/// 对齐 OpenClaw: runSessionsSendA2AFlow(params)
///
/// 在后台线程中执行异步 A2A 流程:
///   1. 通过 agent.wait 获取 round-1 reply
///   2. 若 reply 包含 REPLY_SKIP → 跳过 ping-pong
///   3. Ping-pong: 最多 max_ping_pong_turns 轮（默认 5）
///      a. 将目标 reply 作为上下文发给 requester → 获取 follow-up
///      b. 将 follow-up 发给 target → 获取下一个 reply
///      c. 若 reply 包含 REPLY_SKIP → 停止
///   4. Announce: 构建公告上下文发给 target，判断是否投递
///      a. 若 reply 包含 ANNOUNCE_SKIP → 跳过投递
///      b. 否则 → deliver 到 source channel
///
/// 对齐 OpenClaw:
///   - same-session reply: 直接 deliver，不走 ping-pong + announce
///   - waitForAgentRun + readLatestAssistantReplySnapshot
///   - isReplySkip / isAnnounceSkip / isNonDeliverableSessionsReply
///   - deliverAnnounceReply (gateway "send")
struct SessionsSendA2AParams {
    std::string target_session_key;
    std::string display_key;
    std::string message;
    int announce_timeout_ms = 30000;
    int max_ping_pong_turns = 5;
    std::string requester_session_key;
    std::string requester_channel;     // GatewayMessageChannel enum
    std::string wait_run_id;           // agent.runId for round-1 wait
    std::string round_one_reply;       // when timeout>0: reply already fetched
    std::string baseline_reply;        // snapshot from before agent run
    bool notify_requester_on_wait_failure = false;
};

/// 运行 A2A 后台流程（应在独立线程中调用）
void run_sessions_send_a2a_flow(
    std::shared_ptr<gateway::GatewayClient> gw,
    const SessionsSendA2AParams& params);

}  // namespace tools
}  // namespace hunterclaw
