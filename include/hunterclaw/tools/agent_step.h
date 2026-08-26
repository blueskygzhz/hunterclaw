#pragma once
// ============================================================================
// Agent Step — 严格对齐 OpenClaw agents/tools/agent-step.ts
// ============================================================================
// run_agent_step(): 向指定 session 发送带 provenance 标注的跨会话消息，
// 等待 assistant 回复并返回文本。用于 A2A ping-pong / announce / 失败通知。
// ============================================================================

#include "hunterclaw/agents/agent_lanes.h"
#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/types.h"

#include <optional>
#include <string>

namespace hunterclaw {
namespace tools {

// lane / channel 常量与解析统一由 agents/agent_lanes.h 提供
// （対齐真源：它们本就在 lanes.ts / message-channel-constants.ts，
//   与 agent-step.ts 分属不同模块）。此处仅做转发别名，避免重复定义。
using agents::AGENT_LANE_NESTED;
using agents::INTERNAL_MESSAGE_CHANNEL;
using agents::NESTED_LANE_PREFIX;
using agents::resolve_nested_agent_lane_for_session;

/// 対齐 agent-step.ts:59-70 runAgentStep(params)
struct AgentStepParams {
    std::string session_key;          ///< 目标 sessionKey（必填）
    std::string message;              ///< 用户消息
    std::string extra_system_prompt;  ///< extraSystemPrompt（A2A context）
    int timeout_ms = 120000;

    /// 対齐:81 channel —— 缺省 INTERNAL_MESSAGE_CHANNEL ("webchat")
    std::string channel;
    /// 対齐:80 lane —— 缺省 resolve_nested_agent_lane_for_session(session_key)
    std::string lane;

    /// 対齐:66,82 transcriptMessage —— 有值（含空串）时走保留 transcript 的路径；
    /// nullopt 表示走普通 Gateway 路径。
    std::optional<std::string> transcript_message;

    /// 対齐:72-77 inputProvenance 来源信息
    std::string source_session_key;
    std::string source_channel;
    std::string source_tool;  ///< 缺省 "sessions_send"

    /// 対齐:122-126 —— 与 baseline 比对用。为空表示无 baseline，
    /// 此时任何回复都视为本轮新回复（対齐 run-wait.ts:320-322）。
    /// 注意真源在 agent-step 里不传 baseline，本字段供调用方按需收紧。
    std::string baseline_reply;
};

/// 対齐 agent-step.ts:59 runAgentStep(params) → Promise<string | undefined>
/// @return Ok(reply_text) 或 Err(reason)
core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const AgentStepParams& params);

/// 兼容旧调用点的薄封装（等价于仅填 session_key/message/extra_system_prompt/timeout）。
core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    const std::string& message,
    const std::string& extra_context,
    int timeout_ms = 120000);

}  // namespace tools
}  // namespace hunterclaw
