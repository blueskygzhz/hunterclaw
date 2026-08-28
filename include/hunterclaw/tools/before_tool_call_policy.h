#pragma once
// ============================================================================
// before_tool_call_policy.h - Unified before_tool_call policy chain
//
// Strictly aligned with OpenClaw:
//   src/agents/agent-tools.before-tool-call.policy.ts   (runBeforeToolCallHook)
//   src/agents/agent-tools.before-tool-call.types.ts    (HookOutcome)
//   src/agents/agent-tools.before-tool-call.approval.ts  (approval transport)
//   src/agents/agent-tools.before-tool-call.wrapper.ts   (execution wiring)
//
// This centralizes the "hook checked -> apply to execution" logic that was
// previously duplicated (and simplified to block/params only) across
// react_engine.cc (sequential + parallel), tool_manager.cc, and exec.cc.
//
// Semantics mirror OpenClaw exactly:
//   HookOutcome = { blocked:false, params }                      -> PASS
//               | { blocked:true, kind:"veto",    deniedReason } -> VETO
//               | { blocked:true, kind:"failure", disposition }  -> FAILURE
//
//   - VETO    : plugin/policy intentionally rejected the call. Surface a
//               *blocked* tool result to the LLM (build_blocked_tool_result).
//   - FAILURE : pre-execution machinery failed (hook threw, approval gateway
//               down, cancelled/timeout). Surface as a terminal error carrying
//               a disposition (blocked/aborted/error/timed_out/failed/cancelled).
//   - PASS    : allowed; params carries the final (possibly hook-adjusted)
//               execution params.
// ============================================================================

#include "hunterclaw/core/types.h"
#include "hunterclaw/plugins/plugin_system.h"
#include "hunterclaw/tools/tool.h"

#include <atomic>
#include <optional>
#include <string>

namespace hunterclaw {

// Forward declarations (avoid heavy includes).
class LoopDetectionState;

namespace tools {

class ToolHookManager;

// ── HookOutcomeKind ─────────────────────────────────────────────────────────
// Aligned with OpenClaw HookOutcome discriminants.
enum class HookOutcomeKind {
    Pass,     // blocked:false
    Veto,     // blocked:true, kind:"veto"
    Failure,  // blocked:true, kind:"failure"
};

// ── denied_reason canonical values (align OpenClaw HookBlockedReason) ────────
// "plugin-before-tool-call" | "plugin-approval" | "plugin-approval-unavailable"
// | "client-voice-confirmation" | "tool-loop" | "owner-only"
namespace denied_reason {
inline constexpr const char* kPluginBeforeToolCall  = "plugin-before-tool-call";
inline constexpr const char* kPluginApproval        = "plugin-approval";
inline constexpr const char* kPluginApprovalUnavail = "plugin-approval-unavailable";
inline constexpr const char* kClientVoice= "client-voice-confirmation";
inline constexpr const char* kToolLoop              = "tool-loop";
// 🎯 对齐 OpenClaw:非 owner 调用 owner-only 工具被 deny (GATEWAY_OWNER_ONLY_CORE_TOOLS)。
inline constexpr const char* kOwnerOnly             = "owner-only";
}  // namespace denied_reason

// ── disposition canonical values (align OpenClaw DiagnosticToolTerminalReason) ─
// "blocked" | "aborted" | "error" | "timed_out" | "failed" | "cancelled"
namespace disposition {
inline constexpr const char* kBlocked   = "blocked";
inline constexpr const char* kAborted   = "aborted";
inline constexpr const char* kError     = "error";
inline constexpr const char* kTimedOut  = "timed_out";
inline constexpr const char* kFailed    = "failed";
inline constexpr const char* kCancelled = "cancelled";
}  // namespace disposition

// ── ApprovalMode ────────────────────────────────────────────────────────────
// Strict port of OpenClaw approvalMode: "request" | "report" | "deny" | "defer"
// (agent-tools.before-tool-call.approval.ts:482).
//
// ★ 存在四态而非一个 bool，是因为【不同宿主对"等用户"的承受能力不同】：
//   · Request : 宿主有交互面, 可以阻塞等决策 (OpenClaw 的 fall-through 默认)
//   · Report  : 宿主无法等 —— 把"需要审批"作为结果报回去, 由上层转述
//   · Deny    : 批处理/无人值守 —— 需审批即等于拒绝
//   · Defer   : 异步渠道 —— 不阻塞放行, 审批凭据交给调用方另行收敛
//
// ⚠ 默认取 Request 以严格对齐 OpenClaw。若宿主没有审批 UI,
//   应【显式】设成 Report 或 Deny —— 不要把默认值改掉,
//   否则所有宿主都跟着变, 而这是安全语义。
enum class ApprovalMode {
    Request,  // 默认: 请求并等待决策
    Report,   // 不等待, 以 failure{blocked} 报回"需要审批"
    Deny,     // 需审批即拒绝 -> veto{plugin-approval}
    Defer,    // 不阻塞放行, 携带 deferred 凭据
};

// ── DeferredApproval ────────────────────────────────────────────────────────
// Analog of OpenClaw DeferredPluginToolApproval (approval.ts:458).
//
// ★ Defer 模式下审批【已发起但未决策】, 工具照常执行。
//   调用方必须自己收敛这张凭据 (等决策 / 取消), 否则请求会一直挂在
//   ApprovalManager 里直到 timeout_ms 到期。
struct DeferredApproval {
    std::string approval_id;   // ApprovalManager 中的请求 id
    std::string tool_name;
    std::string tool_call_id;
};

// ── HookOutcome ─────────────────────────────────────────────────────────────
// Direct analog of OpenClaw's HookOutcome union, flattened into one struct.
struct HookOutcome {
    HookOutcomeKind kind = HookOutcomeKind::Pass;
    bool blocked = false;

    // Final (possibly adjusted) execution params. Always meaningful; on
    // veto/failure it carries the pre-adjustment params (OpenClaw parity).
    core::JsonValue params;

    // Present when blocked.
    std::optional<std::string> reason;
    std::optional<std::string> denied_reason;// veto + failure
    std::optional<std::string> disposition;    // failure only

    // ── Approval 相关 (OpenClaw: approvalResolution / deferredApproval) ─────
    // 审批放行时记录决策 ("allow-once" / "allow-always"), 供审计与
    // 上层"是否已批"判断使用。未经审批的 Pass 此字段为空。
    std::optional<std::string> approval_resolution;

    // ★ 仅 ApprovalMode::Defer 且确实发起了审批时有值。
    //   有值意味着 "已放行但审批仍悬空", 调用方【必须】收敛它。
    std::optional<DeferredApproval> deferred_approval;

    static HookOutcome pass(core::JsonValue params) {
        HookOutcome o;
        o.kind = HookOutcomeKind::Pass;
        o.blocked = false;
        o.params = std::move(params);
        return o;
    }
    static HookOutcome veto(std::string reason_str,
                            std::string denied,
                            core::JsonValue params) {
        HookOutcome o;
        o.kind = HookOutcomeKind::Veto;
        o.blocked = true;
        o.reason = std::move(reason_str);
        o.denied_reason = std::move(denied);
        o.params = std::move(params);
        return o;
    }
    static HookOutcome failure(std::string reason_str,
                               std::string disposition_str,
                               std::string denied,
                               core::JsonValue params) {
        HookOutcome o;
        o.kind = HookOutcomeKind::Failure;
        o.blocked = true;
        o.reason = std::move(reason_str);
        o.disposition = std::move(disposition_str);
        o.denied_reason = std::move(denied);
        o.params = std::move(params);
        return o;
    }
};

// ── BeforeToolCallContext ───────────────────────────────────────────────────
// Minimal context needed by the policy chain (subset of OpenClaw HookContext).
struct BeforeToolCallContext {
    std::string tool_name;
    std::string tool_call_id;
    std::string run_id;
    std::string session_id;
    std::string session_key;
    std::string agent_id;

    // 🎯 对齐 OpenClaw senderIsOwner + GATEWAY_OWNER_ONLY_CORE_TOOLS gate:
    // sender_is_owner=false 且工具为 owner-only (tool_is_owner_only 或命中
    // GATEWAY_OWNER_ONLY_CORE_TOOLS 名单) 时, Stage 0 直接 veto{owner-only}。
    bool sender_is_owner = false;
    bool tool_is_owner_only = false;

    // Cancellation flag observed after the hook, before execution
    // (OpenClaw: signal.throwIfAborted()).
    const std::atomic<bool>* aborted = nullptr;

    // Optional loop-detection state (OpenClaw: admitSingleToolCallLoop).
    // When null, loop admission is skipped.
    LoopDetectionState* loop_state = nullptr;

    // Global/private hook manager. When null, hook stage is skipped.
    ToolHookManager* hook_manager = nullptr;
};

// ── run_before_tool_call_policy ─────────────────────────────────────────────
// Strict port of OpenClaw runBeforeToolCallHook(). Ordered stages:
//   0. owner-only gate                  -> veto{owner-only}
//   1. tool-loop admission              -> veto{tool-loop}
//   2. before_tool_call hook runner     -> veto{plugin-before-tool-call}
//                -> params merge (adjusted params)
//   3. hook threw                       -> failure{disposition}
// (Approval / voice stages are represented via require_approval on the hook
//  result and resolved by the injected approval resolver when present.)
HookOutcome run_before_tool_call_policy(
    const BeforeToolCallContext& ctx,
    const core::JsonValue& original_params);

// ── build_blocked_tool_result ───────────────────────────────────────────────
// Aligned with OpenClaw buildBlockedToolResult(): produces a *blocked*-shaped
// ToolResult (not a plain error) so downstream can distinguish policy denial
// from execution error via details.status == "blocked".
ToolResult build_blocked_tool_result(
    const std::string& reason,
    const std::string& denied = denied_reason::kPluginBeforeToolCall,
    const std::string& tool_call_id = "");

// ── is_pre_execution_blocked_result ─────────────────────────────────────────
// Aligned with OpenClaw isPreExecutionBlockedToolResult().
bool is_pre_execution_blocked_result(const ToolResult& result);

// ── validate_tool_execution_params ──────────────────────────────────────────
// Aligned with OpenClaw validateToolExecutionParams(): re-validate the
// hook-adjusted params against the tool schema (required fields present).
// Returns empty string when OK, otherwise a human-readable error.
std::string validate_tool_execution_params(
    const Tool& tool,
    const core::JsonValue& params);

}  // namespace tools
}  // namespace hunterclaw
