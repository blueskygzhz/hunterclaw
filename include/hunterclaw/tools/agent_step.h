#pragma once
// ============================================================================
// Agent Step — 严格对齐 OpenClaw agent-step.ts
// ============================================================================
// 提供 run_agent_step(): 向指定 session 发送消息并等待 LLM 完成推理，
// 返回最终回复文本。用于 A2A ping-pong 和 announce 流程中触发单轮 agent 推理。
// ============================================================================

#include "hunterclaw/gateway/client.h"
#include "hunterclaw/core/types.h"

#include <functional>
#include <string>

namespace hunterclaw {
namespace tools {

/// 对齐 OpenClaw: runAgentStep(params) → reply text
///
/// 向 sessionKey 发送 extra_context 作为附加系统指令 + message 作为用户消息，
/// 等待 agent 完成推理后返回回复文本。
///
/// 与 sessions_send 的区别:
///   - run_agent_step 是 A2A 内部步骤，不走 tool 注册
///   - 不触发 visibility guard / inputProvenance 因为 source 固定为 A2A 内部
///   - 消息注入 A2A 上下文（源 agent session/name 等）
///
/// @param gw              Gateway 客户端（连接已建立）
/// @param session_key     目标 sessionKey
/// @param message         用户消息（ping-pong reply / announce 上下文）
/// @param extra_context   额外系统级上下文（A2A 上下文，注入在消息前）
/// @param timeout_ms      等待 agent 完成的超时（毫秒），默认 120s
/// @return                结果：(ok, reply_text) 或 (error, message)
core::Result<std::string> run_agent_step(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    const std::string& message,
    const std::string& extra_context,
    int timeout_ms = 120000);

}  // namespace tools
}  // namespace hunterclaw
