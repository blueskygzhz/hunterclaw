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

#include <algorithm>   // std::find (resolve_permitted_approval_resolution)

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

    // ── mode=Defer: 不发起、不等待, 装好凭据放行 ────────────────────────
    // 🎯 逐字对齐 OpenClaw approval.ts:501-514 —— defer 分支在
    //    `requestPluginToolApproval` 【之前】return。
    //
    // ★★ 顺序是本质而非风格: 若在这里先 create + 广播,
    //   卡片会【立即弹给用户, 而此刻没有任何人在等他的答案】。
    //   put/resolve 紧邻时不暴露(间隔微秒), 一旦把收敛挪到别处
    //   ——而这正是 Defer 存在的理由—— 就会出现"卡片先弹、无人等待",
    //   用户点下时工具可能早已跑完, 审批变成事后表演。
    //
    //   OpenClaw 的顺序保证【用户看到卡片时一定有人在等他】。
    //
    // ★ broker 的可用性在【收敛点】检查(DeferredApprovalRegistry::do_wait)。
    //   这里不检查是刻意的: 与 OpenClaw 一致 —— defer 分支根本不碰 broker。
    //   收敛点拿不到 broker 时会 fail-closed(TimedOut), 不会放行。
    if (ctx.approval_mode == ApprovalMode::Defer) {
        HookOutcome o = HookOutcome::pass(params);
        DeferredApproval d;
        d.tool_name = ctx.tool_name;
        d.tool_call_id = ctx.tool_call_id;
        d.agent_id = ctx.agent_id;
        d.session_key = ctx.session_key;
        // ★ 超时在此固定, 不在收敛点重新取 —— 收敛点可能处于另一上下文,
        //   那里的配置未必相同, 而用户看到的倒计时应与声明时一致。
        d.timeout_ms = ctx.approval_timeout_ms;
        // ★★ 必须随凭据带走 allowed_decisions:
        //   收敛发生在另一条路径, 那里拿不到 ctx, 无从得知本次审批
        //   是否禁用了 allow-always。不带 = Defer 路径静默接受永久授权,
        //   而 Request 路径拒绝它 —— 换个模式就绕过了约束。
        d.allowed_decisions = ctx.allowed_decisions;
        o.deferred_approval = std::move(d);
        log()->info("Tool '{}' approval deferred (not yet requested)", ctx.tool_name);
        return o;
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

    // ── 发起审批 + 等待决策 + 判定结论 ──────────────────────────────────
    // 🎯 委派给【唯一】实现 —— 与 Defer 收敛点共用同一个函数。
    //   对齐 OpenClaw: requestPluginToolApproval 全仓只有两个调用者
    //   (approval.ts:536 本处, :462 requestDeferredPluginToolApproval),
    //   两者拿到同一种 HookOutcome。
    //
    // ⚠⚠ 此前这段逻辑在 Request 分支与 Defer 收敛点各写了一遍,
    //   重复已经咬过两次(allowed_decisions 消费端校验、aborted 透传
    //   都得改两个文件), 且行为已经漂移。收敛后不可能再漂移。
    return request_plugin_tool_approval(
        broker, ctx.tool_name, ctx.tool_call_id, ctx.agent_id, ctx.session_key,
        ctx.approval_timeout_ms, ctx.allowed_decisions, ctx.aborted, params);
}

}  // namespace

// ── request_plugin_tool_approval ────────────────────────────────────────────
// 🎯 逐字对齐 OpenClaw requestPluginToolApproval (approval.ts:~250-455)。
//
// ★★ 唯一的"发起 + 等待 + 判定"实现。Request 模式与 Defer 收敛点都调它,
//   因此两条路径的结论形状、审计分类、fail-closed 行为【必然一致】——
//   而不是靠两处各自维护同样的逻辑再祈祷它们不漂移。
HookOutcome request_plugin_tool_approval(
    const std::shared_ptr<ToolApprovalBroker>& broker,
    const std::string& tool_name, const std::string& tool_call_id,
    const std::string& agent_id, const std::string& session_key,
    int64_t timeout_ms, const std::vector<std::string>& allowed_decisions,
    const std::atomic<bool>* aborted, const core::JsonValue& params) {

    // ── broker 缺失 → 不可用, 【绝不】放行 ──────────────────────────────
    // OpenClaw resolveUnavailablePluginApprovalSurfaceReason() (:260-272)
    // → failure{failed, plugin-approval-unavailable}。
    //
    // ⚠⚠ 此前没有审批通道时需审批的工具会【静默直通】
    //   (Tool::requires_approval() 零消费), 声明方以为受保护,
    //   实际毫无防护且无日志可查。
    //
    // ★ Defer 收敛点走到这里时同样 fail-closed —— 收敛拿不到 broker
    //   不再压成 TimedOut, 而是与 Request 一样报 unavailable。
    if (!broker) {
        log()->error("Tool '{}' requires approval but no approval surface is "
                     "available; refusing to execute", tool_name);
        return HookOutcome::failure(
            "Approval required but no approval surface is available",
            disposition::kFailed, denied_reason::kPluginApprovalUnavail, params);
    }

    const std::string approval_id = broker->request_tool_approval(
        tool_name, tool_call_id, agent_id, session_key, timeout_ms,
        allowed_decisions);
    if (approval_id.empty()) {
        log()->error("Tool '{}' approval request failed", tool_name);
        return HookOutcome::failure(
            "Approval request could not be created",
            disposition::kFailed, denied_reason::kPluginApprovalUnavail, params);
    }

    log()->info("Tool '{}' waiting for approval (id={})", tool_name, approval_id);
    // ★ 传入 aborted —— 对齐 OpenClaw approval.ts:340-341 的 abortPromise race:
    //   "Wait for the decision, but abort early if the agent run is cancelled
    //    so the user isn't blocked for the full approval timeout."
    //   ⚠ 不传会让 chat.abort 之后仍挂满 timeout, 而该等待占着 main lane
    //     唯一的 WorkerThread → 全 gateway 推理停摆且用户无法自救。
    auto decision = broker->wait_tool_approval(approval_id, timeout_ms, aborted);

    // ── 取消优先于超时判定 ──────────────────────────────────────────────
    // OpenClaw approval.ts:424-434: abortCancelled 时 reason 为
    // "Approval cancelled (run aborted)"。
    //
    // ⚠ 必须先判 aborted 再判 !decision: 两者都会走到 nullopt,
    //   但"被用户取消"与"等满超时"在审计上是完全不同的事件 ——
    //   混成 timed_out 会让"用户主动放弃"看起来像"审批面没人响应",
    //   进而误导排查方向(去查客户端为什么没收到卡片)。
    if (aborted && aborted->load()) {
        log()->info("Tool '{}' approval cancelled by run abort", tool_name);
        return HookOutcome::failure(
            "Approval cancelled (run aborted)",
            disposition::kCancelled, denied_reason::kPluginApproval, params);
    }

    if (!decision.has_value()) {
        // ⚠ 超时与"记录不存在"都落这里。两者都【不能放行】——
        //   未决 == 未获批。
        log()->warn("Tool '{}' approval timed out or vanished", tool_name);
        return HookOutcome::failure(
            "Approval timed out",
            disposition::kTimedOut, denied_reason::kPluginApproval, params);
    }

    // ── 决策合法性校验 ──────────────────────────────────────────────────
    // 🎯 对齐 OpenClaw resolvePermittedPluginApprovalResolution (approval.ts:378)。
    //
    // ★★ 必须在【消费端】再校验一次, 不能只靠 gateway 方法侧:
    //   决策入口有两条 (approval.resolve 与 plugin.approval.resolve),
    //   任何一条漏校验就整体绕过。而这里是唯一的汇聚点。
    //
    // ⚠ 越权决策降级为 nullopt → 按超时处理 (OpenClaw 归为 TIMEOUT),
    //   【不是】当成拒绝: 它既非批准也非明确拒绝, 语义上就是"无有效决策"。
    auto permitted = resolve_permitted_approval_resolution(*decision, allowed_decisions);
    if (!permitted.has_value()) {
        log()->error("Tool '{}' approval returned decision '{}' outside the "
                     "allowed set; treating as no decision", tool_name, *decision);
        return HookOutcome::failure(
            "Approval returned a decision that is not permitted for this request",
            disposition::kTimedOut, denied_reason::kPluginApproval, params);
    }

    const std::string& d = *permitted;
    if (d == "allow-once" || d == "allow-always") {
        HookOutcome o = HookOutcome::pass(params);
        // OpenClaw approvalResolution: "allow-once" / "allow-always"
        o.approval_resolution = d;
        log()->info("Tool '{}' approved ({})", tool_name, d);
        return o;
    }
    // ⚠ 是 Failure{blocked} 而非 Veto —— OpenClaw approval.ts:390-399。
    //   审批流程跑完了, 结论是拒绝: 属事故性终止, 需带 disposition。
    //   (对比 mode=Deny → Veto: 那是根本没跑审批的策略否决。)
    //
    // ★ 此处不再有"未知决策"分支: resolve_permitted_approval_resolution
    //   已把未知值挡在上面 (它只放行三个已知值)。剩下的必然是 "deny"。
    log()->warn("Tool '{}' denied by user", tool_name);
    return HookOutcome::failure(
        "Denied by user",
        disposition::kBlocked, denied_reason::kPluginApproval, params);
}

// ── resolve_permitted_approval_resolution ───────────────────────────────────
// 逐字对齐 OpenClaw resolvePermittedPluginApprovalResolution (approval.ts:102-115):
//
//   if ((decision === ALLOW_ONCE || decision === ALLOW_ALWAYS || decision === DENY)
//       && allowedDecisions.includes(decision)) return decision;
//   return TIMEOUT;
//
// ★ 两个条件【都要】满足: 既是已知决策, 又在允许集内。
//   OpenClaw 用 && 串联, 这里保持同构。
std::optional<std::string> resolve_permitted_approval_resolution(
    const std::string& decision, const std::vector<std::string>& allowed) {
    // ① 必须是三个已知决策之一。
    //   ⚠ broker 实现返回了非法字符串时不能当批准 —— 宁可判为无决策。
    const bool known = (decision == "allow-once" || decision == "allow-always" ||
                        decision == "deny");
    if (!known) return std::nullopt;

    // ② 允许集为空 = 默认全集(不限制)。
    //   ⚠ 空集【不能】理解成"什么都不允许": Stage 1 与配置侧都用
    //     "空 = 用默认全集"这一约定 (config.cc:352-354 有同样的说明)。
    //     若在这里反过来理解, 未配 allowedDecisions 的审批将永远无法通过。
    if (allowed.empty()) return decision;

    if (std::find(allowed.begin(), allowed.end(), decision) == allowed.end()) {
        return std::nullopt;
    }
    return decision;
}

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
