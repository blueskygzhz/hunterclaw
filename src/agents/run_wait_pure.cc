// ============================================================================
// Agent Run Wait —— 纯判定函数 — 严格对齐 OpenClaw agents/run-wait.ts
// ============================================================================
// 这两个函数不触碰 GatewayClient，单独成文件以便被轻量单测直接链接
// （run_wait.cc 依赖 gateway/websocket，拖入测试会引入无关的重依赖）。
// ============================================================================

#include "hunterclaw/agents/run_wait.h"

#include <algorithm>
#include <cctype>

namespace hunterclaw {
namespace agents {

namespace {

bool is_space_ch(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space_ch(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_space_ch(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

}  // namespace

// ── 常量（対齐 agents/run-termination.ts:11,13,14）────────────────────────

const std::string AGENT_RUN_ABORTED_STOP_REASON = "aborted";
const std::string AGENT_RUN_ABORTED_ERROR = "agent run aborted";
const std::string AGENT_RUN_RESTART_ABORT_STOP_REASON = "restart";

// ── liveness 谓词（対齐 shared/agent-liveness.ts）─────────────────────────

bool is_blocked_liveness_state(const std::string& liveness_state) {
    // 対齐:2-4 —— trim + lowercase 后**严格等于** "blocked"。
    // 只此一个值，不可扩充。
    return to_lower(trim(liveness_state)) == "blocked";
}

bool is_abandoned_liveness_state(const std::string& liveness_state) {
    // 対齐:7-9 —— 同上，只认 "abandoned"
    return to_lower(trim(liveness_state)) == "abandoned";
}

std::string format_blocked_liveness_error(const std::string& error) {
    // 対齐:12-15 —— 有原文用原文（已 trim），否则用固定兜底文案
    const std::string message = trim(error);
    return message.empty() ? "Agent run blocked before producing a usable result."
                           : message;
}

std::string format_abandoned_liveness_error(const std::string& error) {
    // 対齐:18-21
    const std::string message = trim(error);
    return message.empty() ? "Agent run ended before producing a complete result."
                           : message;
}

// ── timeout phase（対齐 agents/run-timeout-attribution.ts）────────────────

AgentRunTimeoutPhase normalize_agent_run_timeout_phase(const std::string& value) {
    // 対齐:16-24 —— trim 后在白名单内才认，否则 undefined(None)。
    // 注意真源**不做** lowercase，故这里也保持大小写敏感。
    const std::string v = trim(value);
    if (v == "queue")             return AgentRunTimeoutPhase::Queue;
    if (v == "preflight")         return AgentRunTimeoutPhase::Preflight;
    if (v == "provider")          return AgentRunTimeoutPhase::Provider;
    if (v == "post_turn")         return AgentRunTimeoutPhase::PostTurn;
    if (v == "gateway_draining")  return AgentRunTimeoutPhase::GatewayDraining;
    return AgentRunTimeoutPhase::None;
}

bool is_hard_agent_run_timeout_phase(AgentRunTimeoutPhase phase) {
    // 対齐:462 HARD_TIMEOUT_PHASES = {"preflight","provider","post_turn"}
    // queue 与 gateway_draining 刻意排除：它们是 wait 层的不确定性，
    // 不代表 run 自身超时，误判会让本可继续等待的 run 被当成终态放弃。
    return phase == AgentRunTimeoutPhase::Preflight ||
           phase == AgentRunTimeoutPhase::Provider ||
           phase == AgentRunTimeoutPhase::PostTurn;
}

// ── stop reason 谓词 ─────────────────────────────────────────────────────

bool is_cancellation_stop_reason(const std::string& stop_reason) {
    // 対齐:485-487 —— 仅 "rpc" / "stop"
    return stop_reason == "rpc" || stop_reason == "stop";
}

bool is_aborted_agent_stop_reason(const std::string& stop_reason) {
    // 対齐 run-termination.ts:135-139
    return stop_reason == AGENT_RUN_ABORTED_STOP_REASON ||
           stop_reason == AGENT_RUN_RESTART_ABORT_STOP_REASON;
}

// ── 终态归一化（対齐 agent-run-terminal-outcome.ts:496-563）───────────────

void normalize_agent_wait_result(AgentWaitResult* r) {
    if (!r) return;

    // 対齐:499-503 asNonEmptyString —— trim 后为空即视为不存在。
    // 注意真源保留的是**原串**（非 trim 后的串），故这里只用 trim 判空。
    const std::string stop_reason =
        trim(r->stop_reason).empty() ? std::string() : r->stop_reason;
    const std::string liveness =
        trim(r->liveness_state).empty() ? std::string() : r->liveness_state;
    const std::string raw_error =
        trim(r->error).empty() ? std::string() : r->error;

    // 対齐:504
    const bool restart_cancelled =
        (stop_reason == AGENT_RUN_RESTART_ABORT_STOP_REASON);

    // 対齐:505-509 —— hardTimeout 的两条来源：
    //   ① 归因阶段本身属硬超时；
    //   ② 非 restart 取消、status==timeout **且** providerStarted 明确为 true。
    //      providerStarted 只证明"够到了 provider"，故必须配合 status==timeout。
    const bool provider_started_true =
        r->has_provider_started && r->provider_started;
    const bool hard_timeout =
        is_hard_agent_run_timeout_phase(r->timeout_phase) ||
        (!restart_cancelled && r->status == AgentWaitStatus::Timeout &&
         provider_started_true);

    // 対齐:510
    const bool aborted = is_aborted_agent_stop_reason(stop_reason) && !restart_cancelled;

    // 対齐:511-514 —— ACP/模型的 "stop" 也可能是正常成功收尾，
    // 故只在 status != ok 时才把 rpc/stop 当取消。
    const bool cancelled =
        restart_cancelled ||
        (r->status != AgentWaitStatus::Ok && is_cancellation_stop_reason(stop_reason));

    // 対齐:515-516
    const bool blocked = is_blocked_liveness_state(liveness);
    const bool abandoned = is_abandoned_liveness_state(liveness);

    // 対齐:517-527 —— error 的取值链，顺序即优先级
    std::string error;
    if (hard_timeout) {
        error = raw_error;
    } else if (blocked) {
        error = format_blocked_liveness_error(raw_error);
    } else if (aborted && raw_error.empty()) {
        error = AGENT_RUN_ABORTED_ERROR;
    } else if (aborted || cancelled) {
        error = raw_error;
    } else if (abandoned) {
        error = format_abandoned_liveness_error(raw_error);
    } else {
        error = raw_error;
    }

    // 対齐:528-542 —— reason 的取值链，顺序与 error 链一致
    AgentRunTerminalReason reason;
    if (hard_timeout)                            reason = AgentRunTerminalReason::HardTimeout;
    else if (blocked)                            reason = AgentRunTerminalReason::Blocked;
    else if (aborted)                            reason = AgentRunTerminalReason::Aborted;
    else if (cancelled)                          reason = AgentRunTerminalReason::Cancelled;
    else if (abandoned)                          reason = AgentRunTerminalReason::Abandoned;
    else if (r->status == AgentWaitStatus::Timeout) reason = AgentRunTerminalReason::TimedOut;
    else if (r->status == AgentWaitStatus::Error)   reason = AgentRunTerminalReason::Failed;
    else                                         reason = AgentRunTerminalReason::Completed;

    // 対齐:545-550 —— 由 reason 反推最终 status：
    //   completed              → ok
    //   hard_timeout/timed_out → timeout
    //   其余（含 blocked/aborted/cancelled/abandoned/failed）→ error
    AgentWaitStatus final_status;
    if (reason == AgentRunTerminalReason::Completed) {
        final_status = AgentWaitStatus::Ok;
    } else if (reason == AgentRunTerminalReason::HardTimeout ||
               reason == AgentRunTerminalReason::TimedOut) {
        final_status = AgentWaitStatus::Timeout;
    } else {
        final_status = AgentWaitStatus::Error;
    }

    // 対齐 run-wait.ts:119-131 normalizeTerminalOutcomeForWait
    //   hard_timeout 直接采用 outcome 的 status/error，**跳过** blocked 归一化；
    //   其余情形再过一次 normalizeBlockedLivenessWaitStatus。
    // 两条路径在本实现里已经收敛（blocked 已在上面算进 reason），
    // 这里只需保证 hard_timeout 优先级不被 blocked 覆盖 —— 上面的 if/else
    // 链已把 hard_timeout 排在 blocked 之前，故无需额外处理。
    r->status = final_status;
    r->error = error;
    r->terminal_reason = reason;
}

bool AgentWaitResult::is_pending_error_timeout() const {
    // 対齐 sessions-send-tool.ts:292-296 —— pendingError 为真
    // **且** error 文本 trim 后非空。只有错误文本存在才说明"已发出但确认失败"，
    // 单看 pendingError 会把普通慢 run 误报成投递异常。
    return pending_error && !trim(error).empty();
}

bool is_recoverable_agent_wait_error(const std::string& error) {
    const std::string msg = trim(error);
    if (msg.empty()) return false;  // 対齐:147-149
    // 対齐:150-152 —— "gateway timeout" 明确排除在可恢复之外
    if (msg.find("gateway timeout") != std::string::npos) return false;

    // 対齐:134-142 RECOVERABLE_AGENT_WAIT_ERROR_PATTERNS
    // + hasRetryableConnectionErrorCode 的常见 errno 码
    static const char* const kPatterns[] = {
        "gateway closed (1006",
        "transport close",
        "connection loss",
        "connection closed",
        "gateway not connected",
        "socket hang up",
        "econnreset", "econnrefused", "epipe", "etimedout", "enotfound",
        "eai_again", "ehostunreach", "enetunreach",
    };
    const std::string lower = to_lower(msg);
    for (const char* p : kPatterns) {
        if (lower.find(p) != std::string::npos) return true;
    }
    // 対齐:140 —— /no active .* listener/i
    const size_t no_active = lower.find("no active");
    if (no_active != std::string::npos &&
        lower.find("listener", no_active) != std::string::npos) {
        return true;
    }
    return false;
}

bool has_updated_assistant_reply_snapshot(const AssistantReplySnapshot& latest,
                                          const AssistantReplySnapshot& baseline)
{
    // 対齐:317-319 —— 无文本即视为未更新
    if (latest.empty()) return false;
    // 対齐:320-322 —— 无 baseline 时任何回复都算新的
    if (baseline.text.empty() && baseline.fingerprint.empty()) return true;
    // 対齐:323-325 —— 有指纹优先比指纹（同文本但属新消息也算更新）
    if (!baseline.fingerprint.empty()) {
        return latest.fingerprint != baseline.fingerprint;
    }
    // 対齐:326-328 —— 退回比文本
    return latest.text != baseline.text;
}

}  // namespace agents
}  // namespace hunterclaw
