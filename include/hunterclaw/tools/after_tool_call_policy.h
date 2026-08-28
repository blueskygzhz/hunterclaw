// ============================================================================
// after_tool_call_policy.h — 工具执行后的可干预策略阶段
//
// 🎯 对齐 OpenClaw:
//   src/agents/embedded-agent-subscribe.handlers.tools.ts:1885-1917
//     结果先经 sanitizedResult 加工, 才发出并触发 after_tool_call hook。
//   src/agents/agent-tools.before-tool-call.approval.ts
//     审批的四模式 / 三类 denied_reason 语义（本阶段复用同一套）。
//
// ★★ 与既有 after_tool_call hook 的关系 —— 【必须分开, 不能合并】:
//
//   tool->execute()
//         │
//         ▼
//   run_after_tool_call_policy()   ← 本文件。同步, 【可改结果、可拦截】
//         │
//     results[] 写入 / 进 transcript
//         │
//         ▼
//   ToolHookManager::run_after_tool_call()  ← 既有。fire-and-forget, 纯观测
//
//   ⚠ 既有 after_tool_call hook 在 OpenClaw 与 HunterClaw 里都是
//     fire-and-forget（`void hookRunnerAfter.runAfterToolCall(...)`,
//     react_engine.cc:2359 亦然）, 且在结果【已写入 results[]】之后运行。
//     用它做二次确认会得到"确认还在等, 结果已经进了 transcript"。
//     反过来, 把插件观测点改成可改结果的同步阶段, 会让任意插件
//     意外具备篡改工具结果的能力。故两者严格分离。
//
// ── 本阶段解决的问题 ────────────────────────────────────────────────────────
// 有些确认【无法在执行前发起】: 远端 agent 跑到一半才知道"这一步需要用户
// 确认"（A2A 的 input-required）。before_tool_call 的审批问的是
// "允许调这个工具吗", 拦不住"远端跑起来后要确认某个具体操作"。
// ============================================================================

#pragma once

#include "hunterclaw/core/types.h"
#include "hunterclaw/tools/before_tool_call_policy.h"  // ApprovalMode / denied_reason
#include "hunterclaw/tools/tool.h"                     // ToolResult

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hunterclaw {
namespace tools {

// ── PostExecConfirmRequest ──────────────────────────────────────────────────
// 工具在执行后声明"这次结果需要用户确认才能采信"。
//
// ★ 由工具自己产出（它才知道远端要不要确认）, 策略层只负责【发起审批
//   并按结果加工 ToolResult】—— 策略层不认识 remote_a2a / 企微。
struct PostExecConfirmRequest {
    /// 展示给用户的确认标题（简短）
    std::string title;
    /// 确认的具体内容（远端的追问原文）
    std::string description;

    /// 远端侧的待确认凭据（如企微二次确认的 tokenId 列表）。
    ///
    /// ★ 策略层【只做透传】, 不解析其含义 —— 它是远端与终端之间的约定。
    ///   放在这里是为了能进 details 供终端消费（拉起对应页面）。
    std::vector<std::string> token_ids;

    /// 审批模式。空 = 用工具的 approval_mode()，再空则 Request。
    /// ★ 二次确认通常应为 Request（必须等到用户答复才能采信结果）。
    ///   Defer 在这里语义可疑：结果已经产出，放行等于"未确认就采信"。
    std::string mode;

    /// 限定可选决策；空 = 默认全集。
    /// ⚠ 高风险确认应设为 {"allow-once","deny"} ——
    ///   "确认转账 5000 元"允许 allow-always 意味着后续同类转账全部免确认。
    std::vector<std::string> allowed_decisions;

    /// 等待上限；<=0 用 broker 默认（120s）
    int64_t timeout_ms = 0;
};

// ── AfterToolCallContext ────────────────────────────────────────────────────
struct AfterToolCallContext {
    std::string tool_name;
    std::string tool_call_id;
    std::string run_id;
    std::string session_id;
    std::string session_key;
    std::string agent_id;

    /// 取消标志（等待期间被置位则中止等待）
    const std::atomic<bool>* aborted = nullptr;

    /// 审批通道。留空则回落 ToolApprovalRuntime::instance()。
    /// ⚠ 为空时【不采信】需确认的结果（fail-closed），见 kind 说明。
    std::shared_ptr<ToolApprovalBroker> approval_broker;
};

// ── AfterToolCallOutcomeKind ────────────────────────────────────────────────
enum class AfterToolCallOutcomeKind {
    Unchanged,   // 无需确认，结果原样通过
    Confirmed,   // 用户确认，结果通过（并在 details 记下决策）
    Rejected,    // 用户拒绝 / 超时 / 通道不可用 → 结果被替换为 blocked
};

struct AfterToolCallOutcome {
    AfterToolCallOutcomeKind kind = AfterToolCallOutcomeKind::Unchanged;

    /// 加工后的结果（始终有效，调用方应改用它）
    ToolResult result;

    /// Confirmed 时为 "allow-once" / "allow-always"
    std::optional<std::string> decision;
    /// Rejected 时的原因
    std::optional<std::string> reason;
    /// Rejected 时的 denied_reason（复用 before 阶段的常量）
    std::optional<std::string> denied_reason;

    /// ★ 用户拒绝（含超时）时为真，供调用方做补偿动作。
    ///
    ///   典型补偿：远端任务已在运行，拒绝后应发 tasks/cancel 回收 ——
    ///   否则远端留下一个永远等不到确认的僵尸任务并占着配额。
    ///
    /// ⚠ 策略层【不做】这个补偿：它不知道这是个 remote_a2a 任务，
    ///   也不该知道。补偿由工具侧（拿到本 outcome 的地方）自己完成。
    bool needs_compensation() const {
        return kind == AfterToolCallOutcomeKind::Rejected;
    }
};

// ── extract_post_exec_confirm ───────────────────────────────────────────────
// 从 ToolResult.details 里读出工具声明的确认请求。
//
// ★ 走 details 而非新增虚函数, 原因: 确认请求是【本次调用的产物】
//   (取决于远端这次返回了什么), 不是工具的静态属性。
//   虚函数只能表达静态属性（如 requires_approval()）。
//
// 约定的 details 结构:
//   {
//     "postExecConfirm": {
//       "title": "...", "description": "...",
//       "tokenIds": ["..."],            // 可选
//       "mode": "request",              // 可选
//       "allowedDecisions": ["..."],    // 可选
//       "timeoutMs": 120000             // 可选
//     }
//   }
//
// @return nullopt 表示本次无需确认
std::optional<PostExecConfirmRequest> extract_post_exec_confirm(
    const core::JsonValue& details);

/// 反向：把确认请求写进 details（供工具侧构造结果时使用）
void attach_post_exec_confirm(core::JsonValue& details,
                              const PostExecConfirmRequest& req);

// ── run_after_tool_call_policy ──────────────────────────────────────────────
// 在工具结果产出后、写入 transcript 前运行。
//
// 阶段:
//   0. 提取 details.postExecConfirm  → 无则 Unchanged（零开销）
//   1. 解析审批模式
//   2. 发起审批（plugin 形状载荷，与 before 阶段同一通道）
//   3. 等待决策
//   4. 按决策加工结果
//
// ⚠⚠ 失败一律【不采信结果】(fail-closed): 通道缺失 / 发起失败 / 超时 /
//   拒绝 —— 全部替换为 blocked 结果。
//   放行未确认的结果比拒绝危险得多: 那份结果会进 transcript 并进入
//   下一轮 prompt，模型会当成"已确认的事实"继续往下做。
//
// @param result 工具原始结果（会被读取，不修改）
AfterToolCallOutcome run_after_tool_call_policy(const AfterToolCallContext& ctx,
                                               const ToolResult& result);

}  // namespace tools
}  // namespace hunterclaw
