// ============================================================================
// before_tool_call_policy.cc - Unified before_tool_call policy chain
//
// Strict port of OpenClaw runBeforeToolCallHook() and its execution wiring.
// See before_tool_call_policy.h for the alignment map.
// ============================================================================

#include "hunterclaw/tools/before_tool_call_policy.h"

#include "hunterclaw/tools/tool_hooks.h"
#include "hunterclaw/tools/tool_approval_runtime.h"  // 进程级审批通道
#include "hunterclaw/security/dangerous_tools.h"
#include "hunterclaw/core/logger.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("before_tool_call");
}

// OpenClaw: mergeParamsWithApprovalOverrides / hook params merge.
// first-defined-wins already handled inside the hook runner; here we simply
// replace when the hook supplied a new params object.
core::JsonValue apply_hook_params(const core::JsonValue& current,
                                  const std::optional<std::string>& params_json) {
    if (!params_json.has_value()) return current;
    auto parsed = core::JsonValue::parse(*params_json);
    if (parsed.ok && parsed.value) return std::move(*parsed.value);
    return current;
}

// ── run_approval_gate ───────────────────────────────────────────────────────
// Strict port of OpenClaw resolveBeforeToolCallApprovalOutcome()
// (agent-tools.before-tool-call.approval.ts:480-545) plus
// requestPluginToolApproval() (:182-260).
//
// ★ 走 ToolApprovalBroker 而非直接用某个 manager: 工具审批的载荷是
//   PluginApprovalRequestPayload (title/description/toolName/toolCallId),
//   由 gateway 侧承载。tools 层不依赖 gateway, 故用接口注入。
//
// 返回 nullopt 表示"无需审批, 继续后续 stage"。
std::optional<HookOutcome> run_approval_gate(const BeforeToolCallContext& ctx,
                                            const core::JsonValue& params) {
    // ★ 两个来源取"或": 工具自我声明(静态) 与 策略对本次参数的判定(动态)。
    //   任一要求审批即需审批 —— 与 OpenClaw 一致(工具自身的 needsApproval
    //   和 trustedPolicyResult.requireApproval 都会触发同一条审批路径)。
    const bool need = ctx.tool_requires_approval || ctx.policy_requires_approval;
    if (!need) return std::nullopt;

    // ── mode=Report: 不等待, 把"需要审批"报回去 ─────────────────────────
    // OpenClaw approval.ts:515-525 → failure{blocked}。
    // ★ 放在 broker 解析之前: Report 本就不发起审批, 不需要 broker。
    //   要求 broker 会让"无 UI 宿主"连报告都做不了。
    if (ctx.approval_mode == ApprovalMode::Report) {
        log()->info("Tool '{}' requires approval; reporting (mode=report)",
                    ctx.tool_name);
        return HookOutcome::failure(
            "Tool '" + ctx.tool_name + "' requires approval",
            disposition::kBlocked, denied_reason::kPluginApproval, params);
    }

    // ── mode=Deny: 需审批即拒绝 ─────────────────────────────────────────
    // OpenClaw approval.ts:526-535 → veto{plugin-approval}, reason 逐字
    // "approval_required"。
    // ⚠ 是 Veto 而非 Failure: 这是【意图性拒绝】(策略说不行),
    //   与"审批跑完结论是拒绝"(Failure+blocked) 语义不同, 审计上要能分开。
    // ★ 同样不需要 broker —— 它不发起审批。
    if (ctx.approval_mode == ApprovalMode::Deny) {
        log()->warn("Tool '{}' denied: approval required but mode=deny",
                    ctx.tool_name);
        return HookOutcome::veto("approval_required",
                                 denied_reason::kPluginApproval, params);
    }

    // ── 解析审批通道 ────────────────────────────────────────────────────
    // ★★ 取【一次】并持有 shared_ptr 到本函数返回。
    //   mode=Request 会在下面阻塞等待(可长达 120s), 期间进程级单例
    //   可能被清空(如 shutdown)。若每次用时重新去单例取, 或持有裸指针,
    //   等待中的调用就会访问已销毁对象。
    //
    // ★ ctx 显式给了就用它(单测注入 fake), 否则回落到进程级单例 ——
    //   与 OpenClaw 从 AsyncLocalStorage/全局单例取 runtime 同构。
    std::shared_ptr<ToolApprovalBroker> broker = ctx.approval_broker;
    if (!broker) {
        broker = ToolApprovalRuntime::instance().broker();
    }

    // ── broker 缺失 → 不可用, 【绝不】放行 ──────────────────────────────
    // OpenClaw resolveUnavailablePluginApprovalSurfaceReason() (:260-272)
    // → failure{failed, plugin-approval-unavailable}。
    //
    // ⚠⚠ 这条分支是本次修复的核心: 此前没有审批通道时需审批的工具会
    //   【静默直通】(Tool::requires_approval() 零消费),
    //   声明方以为受保护, 实际毫无防护且无日志可查。
    //
    // ★ Defer 也要求 broker: 没有它 Defer 会退化成"放行且无凭据",
    //   那等于静默跳过审批 —— 正是我们要防的。
    if (!broker) {
        log()->error("Tool '{}' requires approval but no approval surface is "
                     "available; refusing to execute", ctx.tool_name);
        return HookOutcome::failure(
            "Approval required but no approval surface is available",
            disposition::kFailed, denied_reason::kPluginApprovalUnavail, params);
    }

    const std::string approval_id = broker->request_tool_approval(
        ctx.tool_name, ctx.tool_call_id, ctx.agent_id, ctx.session_key,
        ctx.approval_timeout_ms, ctx.allowed_decisions);
    if (approval_id.empty()) {
        log()->error("Tool '{}' approval request failed", ctx.tool_name);
        return HookOutcome::failure(
            "Approval request could not be created",
            disposition::kFailed, denied_reason::kPluginApprovalUnavail, params);
    }

    // ── mode=Defer: 不阻塞放行, 凭据交调用方收敛 ────────────────────────
    // OpenClaw approval.ts:501-514。
    if (ctx.approval_mode == ApprovalMode::Defer) {
        HookOutcome o = HookOutcome::pass(params);
        DeferredApproval d;
        d.approval_id = approval_id;
        d.tool_name = ctx.tool_name;
        d.tool_call_id = ctx.tool_call_id;
        o.deferred_approval = std::move(d);
        log()->info("Tool '{}' approval deferred (id={})", ctx.tool_name, approval_id);
        return o;
    }

    // ── mode=Request: 等待决策 ──────────────────────────────────────────
    log()->info("Tool '{}' waiting for approval (id={})", ctx.tool_name, approval_id);
    auto decision = broker->wait_tool_approval(approval_id, ctx.approval_timeout_ms);
    if (!decision.has_value()) {
        // ⚠ 超时与"记录不存在"都落这里。两者都【不能放行】——
        //   未决 == 未获批。
        log()->warn("Tool '{}' approval timed out or vanished", ctx.tool_name);
        return HookOutcome::failure(
            "Approval timed out",
            disposition::kTimedOut, denied_reason::kPluginApproval, params);
    }

    const std::string& d = *decision;
    if (d == "allow-once" || d == "allow-always") {
        HookOutcome o = HookOutcome::pass(params);
        // OpenClaw approvalResolution: "allow-once" / "allow-always"
        o.approval_resolution = d;
        log()->info("Tool '{}' approved ({})", ctx.tool_name, d);
        return o;
    }
    if (d == "deny") {
        // ⚠ 是 Failure{blocked} 而非 Veto —— OpenClaw approval.ts:232-240。
        //   审批流程跑完了, 结论是拒绝: 属事故性终止, 需带 disposition。
        log()->warn("Tool '{}' denied by user", ctx.tool_name);
        return HookOutcome::failure(
            "Denied by user",
            disposition::kBlocked, denied_reason::kPluginApproval, params);
    }

    // ⚠ 未知决策字符串 —— 【不放行】。broker 实现若返回了非法值,
    //   宁可拒绝也不能当成批准。
    log()->error("Tool '{}' approval returned unknown decision '{}'",
                 ctx.tool_name, d);
    return HookOutcome::failure(
        "Approval returned an unknown decision",
        disposition::kFailed, denied_reason::kPluginApproval, params);
}

}  // namespace

// ── parse_approval_mode ─────────────────────────────────────────────────────
ApprovalMode parse_approval_mode(const std::string& s) {
    if (s == "report") return ApprovalMode::Report;
    if (s == "deny")   return ApprovalMode::Deny;
    if (s == "defer")  return ApprovalMode::Defer;
    // ⚠ 空串与【无法识别】都回落 Request。
    //   不能回落到 Deny/Report —— 一个拼错的配置值不该悄悄放宽审批强度
    //   (Report 会让工具在无人确认的情况下"报个错就算了", 而 Request
    //    至少保证有人看过)。Request 是最严的等待型默认, 也对齐 OpenClaw。
    if (!s.empty() && s != "request") {
        log()->warn("unknown approval mode '{}'; falling back to 'request'", s);
    }
    return ApprovalMode::Request;
}

const char* approval_mode_to_wire(ApprovalMode m) {
    switch (m) {
        case ApprovalMode::Report: return "report";
        case ApprovalMode::Deny:   return "deny";
        case ApprovalMode::Defer:  return "defer";
        case ApprovalMode::Request:
        default:                   return "request";
    }
}

// ── build_blocked_tool_result ───────────────────────────────────────────────
ToolResult build_blocked_tool_result(const std::string& reason,
                                     const std::string& denied,
                                     const std::string& tool_call_id) {
    ToolResult result;
    result.success = false;
    result.error = reason;

    // OpenClaw buildBlockedToolResult(): content carries the reason text,
    // details carries the structured blocked marker.
    ToolResultContent content;
    content.type = ToolResultContent::Type::Text;
    content.text = reason;
    result.content.push_back(std::move(content));

    core::JsonValue details = core::JsonValue::object();
    details["status"] = std::string("blocked");
    details["deniedReason"] = denied;
    details["reason"] = reason;
    if (!tool_call_id.empty()) details["toolCallId"] = tool_call_id;
    result.details = std::move(details);
    return result;
}

// ── is_pre_execution_blocked_result ─────────────────────────────────────────
bool is_pre_execution_blocked_result(const ToolResult& result) {
    if (result.success) return false;
    if (!result.details.is_object()) return false;
    if (!result.details.contains("status")) return false;
    const auto& status = result.details["status"];
    return status.is_string() && status.as_string() == "blocked";
}

// ── validate_tool_execution_params ──────────────────────────────────────────
std::string validate_tool_execution_params(const Tool& tool,
                                            const core::JsonValue& params) {
    Tool::Schema schema = tool.parameters();
    if (schema.required.empty()) return "";
    if (!params.is_object()) {
        // OpenClaw treats non-object params where an object is expected as invalid
        // only when required fields exist.
        return "invalid params: expected object with required fields";
    }
    for (const auto& key : schema.required) {
        if (!params.contains(key)) {
            return "missing required parameter: " + key;
        }
    }
    return "";
}

// ── run_before_tool_call_policy ─────────────────────────────────────────────
HookOutcome run_before_tool_call_policy(const BeforeToolCallContext& ctx,
                                        const core::JsonValue& original_params) {
    // ★ 审批通过后的凭据/结论需要一路带到最终 Pass —— 审批发生在 Stage 1,
    //   而返回点在 Stage 5 之后, 中间不能丢。
    std::optional<HookOutcome> approval_pass;

    // ── Stage 0: owner-only gate ─────────────────────────────────────────────
    // 🎯 对齐 OpenClaw: senderIsOwner === false 时对GATEWAY_OWNER_ONLY_CORE_TOOLS
    // (以及 ownerOnly:true 的工具) 注入 deny。非 owner 调用 owner-only 工具 => veto。
    if (!ctx.sender_is_owner) {
        const bool owner_only =
            ctx.tool_is_owner_only ||
            security::is_gateway_owner_only_core_tool(ctx.tool_name);
        if (owner_only) {
            std::string reason =
                "Tool '" + ctx.tool_name + "' requires owner identity";
            log()->warn("Tool '{}' denied: owner-only, sender is not owner",
                        ctx.tool_name);
            return HookOutcome::veto(reason, denied_reason::kOwnerOnly,
                                     original_params);
        }
    }

    // ── Stage 1: approval gate ───────────────────────────────────────────────
    // 🎯 对齐 OpenClaw policy.ts:274-294 (trustedPolicyResult.requireApproval
    //    -> resolveBeforeToolCallApprovalOutcome)。
    //
    // ★ 位置在 owner-only 之后、tool-loop 之前:
    //   · 身份不对连问都不该问 (owner-only 先)
    //   · 审批是安全门禁, 应在 loop/hook 等业务判定之前 (tool-loop 后)
    //
    // ⚠⚠ 【不受任何 enable 开关控制】。此前审批逻辑在 tool_manager.cc 的
    //   `if (policy_enabled_ && policy_pipeline_)` 内, 而 policy_enabled_
    //   默认 false —— 默认部署下审批链路根本不存在。
    if (auto approval_outcome = run_approval_gate(ctx, original_params)) {
        // Defer 会返回 Pass + deferred_approval, 需要继续后续 stage;
        // 其余 (Veto/Failure) 直接短路返回。
        if (approval_outcome->kind != HookOutcomeKind::Pass) {
            return std::move(*approval_outcome);
        }
        // Pass: 记下审批结论/凭据, 继续后续 stage。
        // ⚠ 不能在这里 return —— 否则 tool-loop 与 before_tool_call hook
        //   会被审批过的调用整个跳过, 等于"批准即免检"。
        approval_pass = std::move(*approval_outcome);
    }

    // ── Stage 2: tool-loop admission ─────────────────────────────────────────
    // OpenClaw policy.ts:131-144 admitSingleToolCallLoop -> critical => veto{tool-loop}.
    // In HunterClaw, loop detection/record is performed by the caller (react_engine
    // owns LoopDetectionState) to avoid a tools<->react link cycle. Callers therefore
    // pass loop_state == nullptr; this stage is intentionally a no-op here.
    (void)ctx.loop_state;

    // ── Stage 3: before_tool_call hook runner ────────────────────────────────
    core::JsonValue effective_params = original_params;
    if (ctx.hook_manager && ctx.hook_manager->has_before_tool_call_hooks()) {
        try {
            PluginHookBeforeToolCallEvent event;
            event.tool_name = ctx.tool_name;
            event.params_json = effective_params.to_string();
            if (!ctx.run_id.empty()) event.run_id = ctx.run_id;
            if (!ctx.tool_call_id.empty()) event.tool_call_id = ctx.tool_call_id;

            PluginHookToolContext tool_ctx;
            tool_ctx.tool_name = ctx.tool_name;
            if (!ctx.agent_id.empty()) tool_ctx.agent_id = ctx.agent_id;
            if (!ctx.session_key.empty()) tool_ctx.session_key = ctx.session_key;
            if (!ctx.session_id.empty()) tool_ctx.session_id = ctx.session_id;
            if (!ctx.run_id.empty()) tool_ctx.run_id = ctx.run_id;
            if (!ctx.tool_call_id.empty()) tool_ctx.tool_call_id = ctx.tool_call_id;

            auto hook_result = ctx.hook_manager->run_before_tool_call(event, tool_ctx);
            if (hook_result.has_value()) {
                // OpenClaw policy.ts:331-339 hookResult?.block -> veto{plugin-before-tool-call}
                if (hook_result->block) {
                    std::string reason = hook_result->block_reason.value_or(
                        "Tool call blocked by plugin hook");
                    log()->warn("Tool '{}' vetoed by before_tool_call hook: {}",
                                ctx.tool_name, reason);
                    return HookOutcome::veto(reason, denied_reason::kPluginBeforeToolCall,
                                             effective_params);
                }
                // OpenClaw policy.ts:365-367 hookResult?.params -> merge.
                effective_params = apply_hook_params(effective_params, hook_result->params_json);
            }
        } catch (const std::exception& e) {
            // ── Stage 4: hook threw ──────────────────────────────────────────
            // OpenClaw policy.ts:387-398: hook threw -> failure{disposition}.
            // Without an abort signal we classify as generic "error".
            log()->error("before_tool_call hook failed: tool={} error={}",
                         ctx.tool_name, e.what());
            return HookOutcome::failure(
                "Tool call blocked because before_tool_call hook failed",
                disposition::kError,
                denied_reason::kPluginBeforeToolCall,
                original_params);
        }
    }

    // ── Stage 5: post-hook cancellation check ────────────────────────────────
    // OpenClaw wrapper.ts: signal.throwIfAborted() after hook, before execute.
    //
    // ⚠ 若此前已发起 Defer 审批, 这里取消【不会】自动撤销那张凭据 ——
    //   ApprovalManager 侧会等到 timeout_ms 自然过期。这是已知取舍:
    //   主动 cancel_approval 需要把 approval_id 带进取消路径, 而取消是
    //   异步信号, 竞态下可能撤销一张已被批准的凭据。宁可让它过期。
    if (ctx.aborted && ctx.aborted->load()) {
        return HookOutcome::failure(
            "Tool call cancelled before execution",
            disposition::kCancelled,
            denied_reason::kPluginBeforeToolCall,
            effective_params);
    }

    HookOutcome out = HookOutcome::pass(std::move(effective_params));
    // ★ 把 Stage 1 的审批结论/Defer 凭据接到最终结果上。
    //   ⚠ 不能直接返回 approval_pass —— 它携带的是【审批时】的 params,
    //     而 hook 可能在 Stage 3 改写过参数。必须用 effective_params。
    if (approval_pass) {
        out.approval_resolution = std::move(approval_pass->approval_resolution);
        out.deferred_approval = std::move(approval_pass->deferred_approval);
    }
    return out;
}

}  // namespace tools
}  // namespace hunterclaw
