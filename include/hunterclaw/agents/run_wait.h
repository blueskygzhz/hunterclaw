#pragma once
// ============================================================================
// Agent Run Wait — 严格对齐 OpenClaw agents/run-wait.ts
// ============================================================================
// 把 agent.wait 的结果规整成三态，并按「轮次边界」读取 assistant 回复快照。
//
// 为什么必须单独成模块：agent_step 与 sessions_send_a2a 都需要
// 「等待 run 结束 + 取本轮新回复」这套逻辑。此前两处各写一份简化版
// （都只是"倒着找最后一条 assistant"），结果是：
//   * 丢掉 wait 的三态，无法区分「连接抖动」与「真投递失败」
//   * 跨过轮次边界，把上一轮的旧回复当成本轮结果重复投递
// 收拢到一处后，两个调用方共享同一份对齐实现。
// ============================================================================

#include "hunterclaw/gateway/client.h"

#include <string>

namespace hunterclaw {
namespace agents {

/// 対齐 run-wait.ts:63 AgentWaitResult["status"]
enum class AgentWaitStatus { Ok, Timeout, Error, Pending };

/// 対齐 agent-run-terminal-outcome.ts:528-542 AgentRunTerminalReason
/// 归一化后的终态原因。顺序即优先级（hard_timeout 最高，completed 最低），
/// 与真源三元表达式的短路顺序严格一致。
enum class AgentRunTerminalReason {
    HardTimeout,   ///< провider/preflight/post_turn 阶段超时 —— 粘性，不可被后续覆盖
    Blocked,       ///< livenessState == "blocked"
    Aborted,       ///< stopReason ∈ {aborted, restart}
    Cancelled,     ///< restart，或非 ok 且 stopReason ∈ {rpc, stop}
    Abandoned,     ///< livenessState == "abandoned"
    TimedOut,      ///< status == timeout 且非上述任一
    Failed,        ///< status == error 且非上述任一
    Completed,     ///< 其余
};

/// 対齐 run-timeout-attribution.ts:2-8 AGENT_RUN_TIMEOUT_PHASES
enum class AgentRunTimeoutPhase {
    None,             ///< 対齐 undefined（未知值一律归此）
    Queue,
    Preflight,
    Provider,
    PostTurn,
    GatewayDraining,
};

/// 対齐 agent-run-terminal-reply.ts:7-10 AgentRunTerminalReplySnapshot
enum class TerminalReplyDisposition { None, Visible, Silent, Empty };

/// 対齐 run-wait.ts:62-74 AgentWaitResult
///
/// ⚠ 这个结构体不是「顺手加几个字段」：真源的 status 是由
/// buildAgentRunTerminalOutcomeFromWaitResult + normalizeBlockedLivenessWaitStatus
/// 两级归一化算出来的，原始 gateway status 只是输入之一。
/// stopReason / livenessState / timeoutPhase / providerStarted 都参与判定，
/// 缺任何一个都会让 hard_timeout / blocked / aborted 被降级成普通 timeout。
struct AgentWaitResult {
    AgentWaitStatus status = AgentWaitStatus::Error;
    std::string error;
    bool pending_error = false;

    /// 対齐:65-66 —— 用 has_* 表达 presence（JS 侧是 undefined 与否）
    bool has_started_at = false;
    bool has_ended_at = false;
    double started_at = 0;
    double ended_at = 0;

    /// 対齐:67-68 —— asNonEmptyString 语义：trim 后为空视为不存在
    std::string stop_reason;
    std::string liveness_state;

    /// 対齐:69
    bool yielded = false;

    /// 対齐:71 —— 归一化后的超时归因阶段
    AgentRunTimeoutPhase timeout_phase = AgentRunTimeoutPhase::None;

    /// 対齐:72 —— providerStarted 三态（未提供 / true / false）
    bool has_provider_started = false;
    bool provider_started = false;

    /// 対齐:73 —— terminalReply 快照
    TerminalReplyDisposition terminal_reply = TerminalReplyDisposition::None;
    std::string terminal_reply_text;

    /// 対齐 buildAgentRunTerminalOutcome 算出的 reason（归一化产物）
    AgentRunTerminalReason terminal_reason = AgentRunTerminalReason::Completed;

    /// 対齐 sessions-send-tool.ts:288-290 isTerminalAgentWaitTimeout
    /// endedAt 存在，或 stopReason / livenessState 任一非空 → run 已到终态。
    bool is_terminal_timeout() const {
        return has_ended_at || !stop_reason.empty() || !liveness_state.empty();
    }

    /// 対齐 sessions-send-tool.ts:292-296 isPendingErrorAgentWaitTimeout
    /// pendingError 为真**且**错误文本非空 —— 说明消息可能已发出但确认失败。
    bool is_pending_error_timeout() const;

    /// 対齐 agent-run-terminal-outcome.ts:479-483 isStickyAgentRunTerminalOutcome
    /// hard_timeout / cancelled 是粘性终态，不应被后到的普通状态覆盖。
    bool is_sticky_terminal() const {
        return terminal_reason == AgentRunTerminalReason::HardTimeout ||
               terminal_reason == AgentRunTerminalReason::Cancelled;
    }
};

// ── 対齐 agents/run-termination.ts 与 shared/agent-liveness.ts 的常量/谓词 ──

/// 対齐 run-termination.ts:11 AGENT_RUN_ABORTED_STOP_REASON
extern const std::string AGENT_RUN_ABORTED_STOP_REASON;
/// 対齐 run-termination.ts:13 AGENT_RUN_ABORTED_ERROR
extern const std::string AGENT_RUN_ABORTED_ERROR;
/// 対齐 run-termination.ts:14 AGENT_RUN_RESTART_ABORT_STOP_REASON
extern const std::string AGENT_RUN_RESTART_ABORT_STOP_REASON;

/// 対齐 agent-liveness.ts:2-4 isBlockedLivenessState
/// ⚠ 真源**只认 "blocked" 这一个值**（trim + lowercase 后严格相等）。
/// 不要凭直觉扩充 "stalled" / "unresponsive" 之类 —— 那会把正常完成的 run
/// 误判为阻塞并丢弃其回复。
bool is_blocked_liveness_state(const std::string& liveness_state);

/// 対齐 agent-liveness.ts:7-9 isAbandonedLivenessState —— 同样只认 "abandoned"
bool is_abandoned_liveness_state(const std::string& liveness_state);

/// 対齐 agent-liveness.ts:12-15 formatBlockedLivenessError
std::string format_blocked_liveness_error(const std::string& error);

/// 対齐 agent-liveness.ts:18-21 formatAbandonedLivenessError
std::string format_abandoned_liveness_error(const std::string& error);

/// 対齐 run-timeout-attribution.ts:16-24 normalizeAgentRunTimeoutPhase
/// 未知字符串一律归 None（対齐返回 undefined）。
AgentRunTimeoutPhase normalize_agent_run_timeout_phase(const std::string& value);

/// 対齐 agent-run-terminal-outcome.ts:462,473-476 isHardAgentRunTimeoutPhase
/// 仅 preflight / provider / post_turn 属"硬超时"；queue 与 gateway_draining
/// 是 wait 层的不确定性，不算 run 本身超时。
bool is_hard_agent_run_timeout_phase(AgentRunTimeoutPhase phase);

/// 対齐 agent-run-terminal-outcome.ts:485-487 isCancellationStopReason
bool is_cancellation_stop_reason(const std::string& stop_reason);

/// 対齐 run-termination.ts:135-139 isAbortedAgentStopReason
bool is_aborted_agent_stop_reason(const std::string& stop_reason);

/// 対齐 agent-run-terminal-outcome.ts:496-563 buildAgentRunTerminalOutcome
/// + agent-liveness.ts:24-39 normalizeBlockedLivenessWaitStatus
///
/// 就地把 result 的 status / error / terminal_reason 归一化。输入是 gateway
/// 原始字段（已填入 result），输出覆盖 status 与 error。
/// 対齐 run-wait.ts:97-117 normalizeAgentWaitResult 的整体效果。
void normalize_agent_wait_result(AgentWaitResult* result);

/// 対齐 run-wait.ts:56-59 AssistantReplySnapshot
/// fingerprint 用于 has_updated_assistant_reply_snapshot 的 baseline 比对：
/// 「同样的文本但属于新一条消息」也应判定为已更新，故不能只比文本。
struct AssistantReplySnapshot {
    std::string text;
    std::string fingerprint;
    bool empty() const { return text.empty(); }
};

/// 対齐 run-wait.ts:134-157 isRecoverableAgentWaitError
/// 传输层抖动属可恢复 —— 调用方**不应**据此判定投递失败并打断用户。
bool is_recoverable_agent_wait_error(const std::string& error);

/// 対齐 run-wait.ts:369-401 waitForAgentRun
AgentWaitResult wait_for_agent_run(std::shared_ptr<gateway::GatewayClient> gw,
                                   const std::string& run_id,
                                   int timeout_ms);

/// 対齐 run-wait.ts:333-351 readLatestAssistantReplySnapshot
/// （stopAtTranscriptArtifact = true 的分支）
///
/// **倒序**扫描 chat.history，遇轮次边界（user 消息 / inter_session 注入）即停止，
/// 只取边界内最新的一条 assistant 文本。
AssistantReplySnapshot read_latest_assistant_reply_snapshot(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& session_key,
    int limit = 50);

/// 対齐 run-wait.ts:313-330 hasUpdatedAssistantReplySnapshot
/// baseline 非空且与最新快照相同 → 本轮没产生新回复，必须返回 false，
/// 否则会把上一轮的回复再投递一次。
bool has_updated_assistant_reply_snapshot(const AssistantReplySnapshot& latest,
                                          const AssistantReplySnapshot& baseline);

/// 対齐 run-wait.ts:404-434 waitForAgentRunAndReadUpdatedAssistantReply
/// 返回值的 reply_text 仅在 status==Ok **且**回复相对 baseline 已更新时非空。
struct WaitAndReadResult {
    AgentWaitResult wait;
    std::string reply_text;
};

WaitAndReadResult wait_for_agent_run_and_read_updated_assistant_reply(
    std::shared_ptr<gateway::GatewayClient> gw,
    const std::string& run_id,
    const std::string& session_key,
    int timeout_ms,
    const AssistantReplySnapshot& baseline = AssistantReplySnapshot());

}  // namespace agents
}  // namespace hunterclaw
