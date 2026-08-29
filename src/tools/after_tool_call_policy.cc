// ============================================================================
// after_tool_call_policy.cc
// 见 after_tool_call_policy.h 的对齐说明与"为何与 after_tool_call hook 分离"。
// ============================================================================

#include "hunterclaw/tools/after_tool_call_policy.h"

#include "hunterclaw/core/logger.h"
#include "hunterclaw/tools/tool_approval_runtime.h"

#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace tools {

namespace {

std::shared_ptr<spdlog::logger> log() {
    return core::get_logger("after_tool_call");
}

constexpr const char* kConfirmKey = "postExecConfirm";

/// 构造"未获确认"的结果。
///
/// ★★ 关键: 必须【替换】原结果而非附加说明。
///   原结果里带着远端产出的内容(可能是"转账已提交"这类文本)。
///   若保留它并只加一句"未确认", 模型看到的仍是那句肯定陈述 ——
///   它会当成已发生的事实继续往下做, 而实际上什么都没确认。
ToolResult make_unconfirmed_result(const std::string& tool_name,
                                  const std::string& reason,
                                  const std::string& denied,
                                  const core::JsonValue& original_details) {
    ToolResult r;
    // ⚠ success=false: 这不是"工具正常返回了一个需确认的结果",
    //   而是"这次调用没有得到有效结果"。置 true 会让模型继续推进。
    r.success = false;
    r.error = reason;

    ToolResultContent c;
    c.type = ToolResultContent::Type::Text;
    c.text = "工具 '" + tool_name + "' 的结果未获用户确认，已作废：" + reason +
             "\n若仍需继续，请重新发起并等待用户确认。";
    r.content.push_back(std::move(c));

    // details 保留原有结构化信息（供终端/审计追溯），并标出未确认状态。
    // ★ details 不进 prompt（react_engine.cc:1076-1088 只读 content），
    //   所以这里保留原 details 不会让模型看到那段肯定陈述。
    core::JsonValue d = original_details.is_object() ? original_details
                                                    : core::JsonValue::object();
    d["status"] = std::string("blocked");
    d["deniedReason"] = denied;
    d["postExecConfirmed"] = false;
    r.details = std::move(d);
    return r;
}

}  // namespace

// ── extract / attach ────────────────────────────────────────────────────────

std::optional<PostExecConfirmRequest> extract_post_exec_confirm(
    const core::JsonValue& details) {
    if (!details.is_object() || !details.has_key(kConfirmKey)) return std::nullopt;
    const core::JsonValue& c = details[kConfirmKey];
    if (!c.is_object()) return std::nullopt;

    PostExecConfirmRequest req;
    if (c.has_key("title") && c["title"].is_string()) req.title = c["title"].as_string();
    if (c.has_key("description") && c["description"].is_string()) {
        req.description = c["description"].as_string();
    }
    // ⚠ title/description 都空时视为无效声明并【忽略】——
    //   一个空确认请求会弹出一张什么都没写的卡片, 用户无从判断,
    //   而拒绝它又会作废一个本可通过的结果。
    if (req.title.empty() && req.description.empty()) {
        log()->warn("postExecConfirm has neither title nor description; ignoring");
        return std::nullopt;
    }
    if (req.title.empty()) req.title = "需要确认";

    if (c.has_key("mode") && c["mode"].is_string()) req.mode = c["mode"].as_string();
    if (c.has_key("timeoutMs") && c["timeoutMs"].is_number()) {
        req.timeout_ms = static_cast<int64_t>(c["timeoutMs"].as_number());
    }
    if (c.has_key("tokenIds") && c["tokenIds"].is_array()) {
        const core::JsonValue& arr = c["tokenIds"];
        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].is_string()) req.token_ids.push_back(arr[i].as_string());
        }
    }
    if (c.has_key("allowedDecisions") && c["allowedDecisions"].is_array()) {
        const core::JsonValue& arr = c["allowedDecisions"];
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_string()) continue;
            const std::string s = arr[i].as_string();
            // ⚠ 只接受已知决策。未知项静默丢弃 —— 若把它带进 allowed_decisions,
            //   会得到一个"任何合法决策都不在允许集里"的审批, 永远无法通过。
            if (s == "allow-once" || s == "allow-always" || s == "deny") {
                req.allowed_decisions.push_back(s);
            }
        }
    }
    return req;
}

void attach_post_exec_confirm(core::JsonValue& details,
                              const PostExecConfirmRequest& req) {
    core::JsonValue c = core::JsonValue::object();
    c["title"] = req.title;
    c["description"] = req.description;
    if (!req.mode.empty()) c["mode"] = req.mode;
    if (req.timeout_ms > 0) c["timeoutMs"] = static_cast<int64_t>(req.timeout_ms);
    if (!req.token_ids.empty()) {
        core::JsonValue arr = core::JsonValue::array();
        for (const auto& t : req.token_ids) arr.push_back(core::JsonValue(t));
        c["tokenIds"] = arr;
    }
    if (!req.allowed_decisions.empty()) {
        core::JsonValue arr = core::JsonValue::array();
        for (const auto& d : req.allowed_decisions) arr.push_back(core::JsonValue(d));
        c["allowedDecisions"] = arr;
    }
    details[kConfirmKey] = std::move(c);
}

// ── run_after_tool_call_policy ──────────────────────────────────────────────

AfterToolCallOutcome run_after_tool_call_policy(const AfterToolCallContext& ctx,
                                               const ToolResult& result) {
    AfterToolCallOutcome out;
    out.kind = AfterToolCallOutcomeKind::Unchanged;
    out.result = result;

    // ── Stage 0: 是否声明了确认需求 ─────────────────────────────────────
    // ★ 绝大多数工具调用走这条 —— 零开销（一次 has_key）。
    auto req = extract_post_exec_confirm(result.details);
    if (!req.has_value()) return out;

    // ⚠ 工具执行失败时【不发起确认】: 没有有效结果可确认,
    //   弹一张"确认失败的操作"的卡片只会让用户困惑。
    if (!result.success) {
        log()->info("tool '{}' failed; skipping post-exec confirmation",
                    ctx.tool_name);
        return out;
    }

    // ── Stage 1: 解析模式 ───────────────────────────────────────────────
    const ApprovalMode mode = parse_approval_mode(req->mode);

    // ⚠ Defer 在本阶段【语义可疑并被拒绝使用】:
    //   结果已经产出, Defer 意味着"放行且稍后确认" —— 那份未确认的结果
    //   会立刻进 transcript 并进入下一轮 prompt, 模型据此继续往下做。
    //   等确认结果回来时, 后续动作可能已经发生。
    //   故降级为 Request（必须等到答复才采信）。
    ApprovalMode effective = mode;
    if (mode == ApprovalMode::Defer) {
        log()->warn("post-exec confirmation does not support 'defer' "
                    "(result would be trusted before confirmation); using 'request'");
        effective = ApprovalMode::Request;
    }

    // Deny / Report: 不发起审批, 直接判为未确认。
    // ★ 与 before 阶段的语义一致 —— 但这里两者的【结果】都是作废,
    //   因为"没有人确认过"就是"不能采信"。
    //   差别只在 denied_reason 与日志（审计需要能分开）。
    if (effective == ApprovalMode::Deny || effective == ApprovalMode::Report) {
        const char* why = (effective == ApprovalMode::Deny)
                              ? "confirmation required but approval mode is 'deny'"
                              : "confirmation required but no approval surface is "
                                "configured to wait on (mode='report')";
        log()->warn("tool '{}' post-exec confirmation refused: {}", ctx.tool_name, why);
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApproval;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApproval,
                                            result.details);
        return out;
    }

    // ── Stage 2: 取通道 ────────────────────────────────────────────────
    // ★ 取一次并持有 shared_ptr 到返回（等待可达 120s，期间单例可能被清空）。
    std::shared_ptr<ToolApprovalBroker> broker = ctx.approval_broker;
    if (!broker) broker = ToolApprovalRuntime::instance().broker();

    if (!broker) {
        // ⚠⚠ fail-closed。放行未确认的结果比拒绝危险得多:
        //   它会进 transcript 并进入下一轮 prompt,
        //   模型会当成"已确认的事实"继续往下做。
        const char* why = "post-exec confirmation required but no approval "
                          "surface is available";
        log()->error("tool '{}' {}", ctx.tool_name, why);
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApprovalUnavail;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApprovalUnavail,
                                            result.details);
        return out;
    }

    // ── Stage 3: 发起 + 等待 ───────────────────────────────────────────
    // ★ 复用 before 阶段的同一通道（plugin 形状载荷）—— 客户端无需区分
    //   "执行前审批"与"执行后确认", 它们在 UI 上是同一种卡片。
    const std::string approval_id = broker->request_tool_approval(
        ctx.tool_name, ctx.tool_call_id, ctx.agent_id, ctx.session_key,
        req->timeout_ms, req->allowed_decisions);
    if (approval_id.empty()) {
        const char* why = "post-exec confirmation could not be created";
        log()->error("tool '{}' {}", ctx.tool_name, why);
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApprovalUnavail;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApprovalUnavail,
                                            result.details);
        return out;
    }

    log()->info("tool '{}' waiting for post-exec confirmation (id={} tokens={})",
                ctx.tool_name, approval_id, req->token_ids.size());

    // ★ 传入 aborted —— 与 before 阶段一致 (OpenClaw approval.ts:340-341)。
    //   ⚠ 原实现只在 wait 【返回后】查 aborted, 那时 120s 已经等完了,
    //     检查形同虚设。abort 必须能真的打断等待。
    auto decision = broker->wait_tool_approval(approval_id, req->timeout_ms,
                                               ctx.aborted);

    // 取消检查：等待期间被中止
    if (ctx.aborted && ctx.aborted->load()) {
        const char* why = "cancelled while waiting for confirmation";
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApproval;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApproval,
                                            result.details);
        return out;
    }

    if (!decision.has_value()) {
        // ⚠ 超时 == 未确认。不能因为"等过了"就采信。
        const char* why = "confirmation timed out";
        log()->warn("tool '{}' {}", ctx.tool_name, why);
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApproval;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApproval,
                                            result.details);
        return out;
    }

    // ── 决策合法性校验 ──────────────────────────────────────────────────
    // 🎯 与 before 阶段同一函数 (OpenClaw resolvePermittedPluginApprovalResolution)。
    //
    // ⚠⚠ 这一步在本阶段比 before 阶段更关键: remote_a2a 的二次确认声明
    //   allowed={allow-once, deny} 正是为了【禁掉 allow-always】——
    //   一次误点永久授权会让该远端后续所有需确认操作全部免确认。
    //   若不在此校验, 那个禁令就只是一句注释。
    auto permitted =
        resolve_permitted_approval_resolution(*decision, req->allowed_decisions);
    if (!permitted.has_value()) {
        const char* why = "confirmation returned a decision that is not permitted";
        log()->error("tool '{}' post-exec confirmation decision '{}' outside "
                     "allowed set", ctx.tool_name, *decision);
        out.kind = AfterToolCallOutcomeKind::Rejected;
        out.reason = why;
        out.denied_reason = denied_reason::kPluginApproval;
        out.result = make_unconfirmed_result(ctx.tool_name, why,
                                            denied_reason::kPluginApproval,
                                            result.details);
        return out;
    }

    const std::string& d = *permitted;
    if (d == "allow-once" || d == "allow-always") {
        out.kind = AfterToolCallOutcomeKind::Confirmed;
        out.decision = d;
        // 结果原样通过，仅在 details 标注已确认（供审计）
        core::JsonValue det = result.details.is_object() ? result.details
                                                        : core::JsonValue::object();
        det["postExecConfirmed"] = true;
        det["postExecDecision"] = d;
        out.result = result;
        out.result.details = std::move(det);
        log()->info("tool '{}' post-exec confirmation granted ({})", ctx.tool_name, d);
        return out;
    }

    // ★ 只剩 "deny" —— 未知/越权已被 resolve_permitted_... 挡在上面。
    const char* why = "confirmation denied by user";
    log()->info("tool '{}' post-exec confirmation denied", ctx.tool_name);
    out.kind = AfterToolCallOutcomeKind::Rejected;
    out.reason = why;
    out.denied_reason = denied_reason::kPluginApproval;
    out.result = make_unconfirmed_result(ctx.tool_name, why,
                                        denied_reason::kPluginApproval,
                                        result.details);
    return out;
}

}  // namespace tools
}  // namespace hunterclaw
