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
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

/// 解析 Tool::approval_mode() 返回的字符串。
/// ★ 空串或无法识别 → Request（对齐 OpenClaw 的 fall-through 默认）。
/// ⚠ 无法识别时【不能】回落到 Deny/Report ——
///   一个拼错的配置值不该悄悄放宽审批强度。Request 是最严的等待型默认。
ApprovalMode parse_approval_mode(const std::string& s);
const char* approval_mode_to_wire(ApprovalMode m);

// ── DeferredApproval ────────────────────────────────────────────────────────
// Analog of OpenClaw DeferredPluginToolApproval (approval.ts:458).
//
// ★ Defer 模式下审批【已发起但未决策】, 工具照常执行。
//   调用方必须自己收敛这张凭据 (等决策 / 取消), 否则请求会一直挂在
//   审批 broker 里直到 timeout_ms 到期。
struct DeferredApproval {
    std::string approval_id;   // broker 中的请求 id
    std::string tool_name;
    std::string tool_call_id;
};

// ── ToolApprovalBroker ──────────────────────────────────────────────────────
// 工具审批的传输抽象。
//
// ★★ 为什么是接口而不是直接用某个 manager:
//   OpenClaw 的工具审批走 PluginApprovalRequestPayload
//   (title/description/toolName/toolCallId), 由 gateway 侧的
//   审批 store 承载, 并经 plugin.approval.requested 事件推给客户端。
//   而 tools 层【不依赖】gateway (bazel query 可证 somepath 为空) ——
//   直接引用会形成 tools→gateway→agents→tools 的依赖环。
//
//   故此处只声明"发起 + 等待"两个动作, 由 gateway 侧注入实现。
//   与 remote_a2a 用 std::function 注入路由挂载器是同一手法。
//
// ⚠ 实现方【必须】构造 plugin 形状的载荷 (title+description),
//   不能塞进 exec 的 command 字段 —— OpenClaw 按载荷形状判别归属
//   (resolveApprovalRequestKind, "never from id spelling"),
//   形状错了会让客户端按命令卡片渲染工具审批, 且 allow-always
//   会把工具名当命令写进 exec 白名单。
class ToolApprovalBroker {
public:
    virtual ~ToolApprovalBroker() = default;

    /// 发起一次工具审批。
    /// @param allowed_decisions 为空表示默认全集; 非空时限定可选决策
    ///        (例如不允许 allow-always —— 见 remote_a2a 的用户确认场景)。
    /// @return 审批 id; 空字符串表示发起失败
    virtual std::string request_tool_approval(
        const std::string& tool_name,
        const std::string& tool_call_id,
        const std::string& agent_id,
        const std::string& session_key,
        int64_t timeout_ms,
        const std::vector<std::string>& allowed_decisions) = 0;

    /// 等待决策。
    /// @return "allow-once" / "allow-always" / "deny"；nullopt = 超时/不存在
    virtual std::optional<std::string> wait_tool_approval(
        const std::string& approval_id, int64_t timeout_ms) = 0;
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

    // ── Approval gate (Stage 1) ─────────────────────────────────────────────
    // 🎯 对齐 OpenClaw trustedPolicyResult.requireApproval ->
    //    resolveBeforeToolCallApprovalOutcome() (policy.ts:274-294)。
    //
    // ★★ tool_requires_approval 来自 Tool::requires_approval()。
    //   在此之前该虚函数【定义了却零消费】—— builtin_tools.cc:172/536 两个
    //   工具声明 requires_approval()=true 却从不审批, 是静默失效的安全缺口:
    //   声明方以为受保护, 实际上直接执行, 且没有任何日志能看出来。
    bool tool_requires_approval = false;

    // 命中策略管道 REQUIRE_APPROVAL 时置真 (与 tool_requires_approval 取"或")。
    // ⚠ 两个来源都要, 不能二选一: 前者是工具的自我声明(静态),
    //   后者是策略对本次参数的判定(动态)。任一要求审批即需审批。
    bool policy_requires_approval = false;

    // ⚠ 见 ApprovalMode 的说明: 默认 Request 是为了对齐 OpenClaw,
    //   宿主无审批 UI 时应【显式】改成 Report/Deny, 而不是改默认值。
    ApprovalMode approval_mode = ApprovalMode::Request;

    // ★★ 工具审批的传输通道 (plugin 形状载荷 + gateway 广播)。
    //
    // ⚠ 用 shared_ptr 而非裸指针: mode=Request 时策略链会持有它跨越一次
    //   阻塞等待(可长达 120s)。若期间进程级单例被替换/清空,
    //   裸指针就在访问已销毁对象。
    //
    // ★ 为空时按【不可用】处理 -> failure{plugin-approval-unavailable},
    //   【绝不】放行。对齐 OpenClaw
    //   resolveUnavailablePluginApprovalSurfaceReason()。
    //   ⚠ 放行是安全漏洞: 没配 broker 的部署会让所有需审批工具静默直通。
    //
    // ★ 调用方一般不手填此字段 —— 留空时策略链会自动从
    //   ToolApprovalRuntime::instance() 取(见 run_before_tool_call_policy)。
    //   显式赋值仅用于单测注入 fake。
    std::shared_ptr<ToolApprovalBroker> approval_broker;

    // 限定本次审批允许的决策集合; 为空用默认全集。
    // ★ 用途: 某些操作不该允许 allow-always
    //   (例如"确认转账 5000 元" —— 永久授权绝不行)。
    std::vector<std::string> allowed_decisions;

    // 审批等待上限。<=0 时由 broker 用默认值 (120s)。
    int64_t approval_timeout_ms = 0;

    // Optional loop-detection state (OpenClaw: admitSingleToolCallLoop).
    // When null, loop admission is skipped.
    LoopDetectionState* loop_state = nullptr;

    // Global/private hook manager. When null, hook stage is skipped.
    ToolHookManager* hook_manager = nullptr;
};

// ── run_before_tool_call_policy ─────────────────────────────────────────────
// Strict port of OpenClaw runBeforeToolCallHook(). Ordered stages:
//   0. owner-only gate                  -> veto{owner-only}
//   1. approval gate                    -> 见下表 (OpenClaw policy.ts:274-294)
//   2. tool-loop admission              -> veto{tool-loop}
//   3. before_tool_call hook runner     -> veto{plugin-before-tool-call}
//                -> params merge (adjusted params)
//   4. hook threw                       -> failure{disposition}
//   5. post-hook cancellation check     -> failure{cancelled}
//
// ★ Approval 在 Stage 1 (owner-only 之后、tool-loop 之前) —— 与 OpenClaw
//   一致: 身份门禁最先(连问都不该问), 审批在业务判定之前。
//
// ⚠⚠ Approval 【不受任何 enable 开关控制】。它曾经被放在
//   tool_manager.cc 的 `if (policy_enabled_ && policy_pipeline_)` 里,
//   而 policy_enabled_ 默认 false —— 等于默认部署下审批链路根本不存在。
//   安全门禁不能挂在一个默认关闭的开关后面。
//
// Approval 结果映射 (逐字对齐 approval.ts, ⚠ 注意其中的不对称):
//   ┌──────────────────────┬─────────┬────────────┬───────────────────────────┐
//   │ 场景                 │ kind    │disposition │ denied_reason             │
//   ├──────────────────────┼─────────┼────────────┼───────────────────────────┤
//   │ ALLOW_ONCE/ALWAYS    │ Pass    │ —          │ —                         │
//   │ 用户 DENY            │ Failure │ blocked    │ plugin-approval           │
//   │ 超时                 │ Failure │ timed_out  │ plugin-approval           │
//   │ manager 缺失         │ Failure │ failed     │ plugin-approval-unavailable│
//   │ mode=Report          │ Failure │ blocked    │ plugin-approval           │
//   │ mode=Deny            │ Veto    │ —          │ plugin-approval           │
//   │ mode=Defer           │ Pass    │ —          │ — (+deferred_approval)    │
//   └──────────────────────┴─────────┴────────────┴───────────────────────────┘
//   ★ "用户 DENY 是 Failure" 而 "mode=Deny 是 Veto" 【不是笔误】——
//     OpenClaw approval.ts:232-240 vs :526-535 原样如此。
//     语义差别: 前者是"审批流程跑完了, 结论是拒绝"(事故性终止, 带 disposition);
//     后者是"根本没跑审批, 策略直接否决"(意图性拒绝)。
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
