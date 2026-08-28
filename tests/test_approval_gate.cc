// ============================================================================
// test_approval_gate.cc — before_tool_call 链 Stage 1 (approval gate)
//
// 对齐目标: OpenClaw
//   src/agents/agent-tools.before-tool-call.approval.ts
//   src/agents/agent-tools.before-tool-call.policy.ts:274-294
//
// ★ 本套测试锁死的核心不变量:
//   ① Tool::requires_approval() 必须真的生效 —— 它曾经【零消费】,
//      声明要审批的工具从不审批, 是静默失效的安全缺口。
//   ② approval manager 缺失时【拒绝执行】而非放行。
//   ③ 四种 ApprovalMode 的 kind/disposition/denied_reason 逐字对齐 OpenClaw,
//      包括其中的【不对称】(用户 DENY=Failure, mode=Deny=Veto)。
// ============================================================================

#include "hunterclaw/tools/before_tool_call_policy.h"
#include "hunterclaw/tools/approval.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <thread>

using namespace hunterclaw;
using namespace hunterclaw::tools;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) {                                                        \
            ++g_pass;                                                      \
        } else {                                                           \
            ++g_fail;                                                      \
            std::cout << "  FAIL: " << (msg) << "  @" << __LINE__          \
                      << std::endl;                                        \
        }                                                                  \
    } while (0)

#define SECTION(name) std::cout << "[" << (name) << "]" << std::endl

namespace {

std::string tmp_cfg_path() {
    return "/tmp/hc_approval_gate_test.json";
}

BeforeToolCallContext base_ctx(const char* tool = "danger_tool") {
    BeforeToolCallContext c;
    c.tool_name = tool;
    c.tool_call_id = "call-1";
    c.agent_id = "agent-1";
    c.session_key = "sess-1";
    // ★ sender_is_owner=true: 否则 Stage 0 (owner-only) 会先拦下来,
    //   我们测的是 Stage 1, 不能被前一个 stage 短路。
    c.sender_is_owner = true;
    return c;
}

core::JsonValue empty_params() {
    core::JsonValue v = core::JsonValue::object();
    v["x"] = std::string("1");
    return v;
}

// ── resolve_next_new ────────────────────────────────────────────────────────
// 等到出现一个【新的】审批请求(id 不在 known 里)并按 decision 决策它。
//
// ⚠⚠ 不能简单地用 get_pending_approvals()[0] ——
//   ApprovalManager::resolve_approval() 【不会】把请求从 pending_requests_
//   移除(只写 resolved_responses_)。因此已决策的请求会永久留在待审列表里,
//   下一个子测试的 approver 会去决策那条【旧的】, 而新请求永远等不到决策
//   → 测试挂死 120s。
//
// ★ 这同时暴露了 ApprovalManager 的一个真实缺陷: 任何列出待审批的 UI
//   都会一直看到已经批过的条目。见文件末尾的说明。
std::string resolve_next_new(ApprovalManager& mgr,
                             const std::set<std::string>& known,
                             ApprovalDecision decision) {
    for (int i = 0; i < 500; ++i) {
        for (const auto& r : mgr.get_pending_approvals()) {
            if (known.count(r.id) == 0) {
                mgr.resolve_approval(r.id, decision, "tester");
                return r.id;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return "";
}

std::set<std::string> snapshot_ids(ApprovalManager& mgr) {
    std::set<std::string> s;
    for (const auto& r : mgr.get_pending_approvals()) s.insert(r.id);
    return s;
}

}  // namespace

// ============================================================================
// 不需要审批时直接放行
// ============================================================================
static void test_no_approval_needed() {
    SECTION("不需要审批");

    auto ctx = base_ctx();
    // 两个来源都为 false
    ctx.tool_requires_approval = false;
    ctx.policy_requires_approval = false;

    auto out = run_before_tool_call_policy(ctx, empty_params());
    CHECK(out.kind == HookOutcomeKind::Pass, "无需审批 → Pass");
    CHECK(!out.blocked, "未被阻塞");
    CHECK(!out.approval_resolution.has_value(), "无审批结论");
    CHECK(!out.deferred_approval.has_value(), "无 deferred 凭据");
    // ★ 无需审批时不该碰 approval manager (这里根本没给)
    CHECK(out.params.contains("x"), "参数原样透传");
}

// ============================================================================
// ★★ manager 缺失 → 拒绝, 【绝不】放行
// ============================================================================
static void test_manager_missing_refuses() {
    SECTION("approval manager 缺失");

    // ── 工具自声明需审批 ────────────────────────────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;   // ← Tool::requires_approval()
        ctx.approval_manager = nullptr;

        auto out = run_before_tool_call_policy(ctx, empty_params());

        // ★★ [不可回退] 这是本次修复的核心。
        //   此前 Tool::requires_approval() 零消费 —— 声明要审批的工具
        //   直接执行, 声明方以为受保护, 实际毫无防护且无日志可查。
        CHECK(out.kind == HookOutcomeKind::Failure,
              "manager 缺失 → Failure, 不放行 [不可回退]");
        CHECK(out.blocked, "blocked=true [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable [不可回退]");
        CHECK(out.disposition.value_or("") == disposition::kFailed,
              "disposition=failed (对齐 OpenClaw)");
    }

    // ── 策略判定需审批（另一个来源）─────────────────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = false;
        ctx.policy_requires_approval = true;  // ← PolicyDecision::REQUIRE_APPROVAL
        ctx.approval_manager = nullptr;

        auto out = run_before_tool_call_policy(ctx, empty_params());
        CHECK(out.kind == HookOutcomeKind::Failure,
              "策略来源同样拒绝 [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "同一 denied_reason");
    }

    // ★ 两个来源取「或」—— 任一为真即需审批, 不能二选一:
    //   前者是工具的静态自我声明, 后者是策略对本次参数的动态判定。
}

// ============================================================================
// mode=Deny → Veto{plugin-approval}
// ============================================================================
static void test_mode_deny() {
    SECTION("mode=Deny");

    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_mode = ApprovalMode::Deny;
    ctx.approval_manager = nullptr;   // Deny 不需要 manager

    auto out = run_before_tool_call_policy(ctx, empty_params());

    // ★★ [不可回退] 是 Veto 而非 Failure —— OpenClaw approval.ts:526-535。
    //   语义: 策略【直接否决】(意图性拒绝), 审批流程根本没跑。
    //   与"审批跑完结论是拒绝"(Failure+blocked) 在审计上必须能分开。
    CHECK(out.kind == HookOutcomeKind::Veto,
          "mode=Deny → Veto (不是 Failure) [不可回退]");
    CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
          "denied_reason=plugin-approval");
    // OpenClaw 的 reason 逐字是 "approval_required"
    CHECK(out.reason.value_or("") == "approval_required",
          "reason 逐字对齐 OpenClaw [不可回退]");
    CHECK(!out.disposition.has_value(), "Veto 不带 disposition");
}

// ============================================================================
// mode=Report → Failure{blocked}, 不阻塞不等待
// ============================================================================
static void test_mode_report() {
    SECTION("mode=Report");

    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_mode = ApprovalMode::Report;
    ctx.approval_manager = nullptr;   // Report 不发起审批, 不需要 manager

    const auto t0 = std::chrono::steady_clock::now();
    auto out = run_before_tool_call_policy(ctx, empty_params());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    CHECK(out.kind == HookOutcomeKind::Failure, "mode=Report → Failure");
    CHECK(out.disposition.value_or("") == disposition::kBlocked,
          "disposition=blocked (对齐 OpenClaw approval.ts:515-525)");
    CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
          "denied_reason=plugin-approval");
    // ★ [不可回退] Report 的全部意义就是"不等" —— 它服务于无交互面的宿主。
    //   若这里阻塞, 无 UI 的通道会卡到审批超时(120s), 表现为整个会话僵死。
    CHECK(ms < 500, "Report 不阻塞 [不可回退]");
    // ★ Report 不需要 manager: 要求 manager 会让"无 UI 宿主"连报告都做不了
    CHECK(out.blocked, "blocked=true (报回给模型/上层)");
}

// ============================================================================
// mode=Request → 等待决策；覆盖 ALLOW / DENY / TIMEOUT
// ============================================================================
static void test_mode_request() {
    SECTION("mode=Request");

    auto mgr = ApprovalManager::create(tmp_cfg_path());

    // ── ALLOW_ONCE → Pass + approval_resolution ────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_mode = ApprovalMode::Request;
        ctx.approval_manager = mgr.get();
        ctx.approval_timeout_ms = 5000;

        // ⚠ 必须只决策【新出现】的请求 —— 已决策的请求不会从
        //   pending_requests_ 移除(ApprovalManager 的既有缺陷)。
        const auto known = snapshot_ids(*mgr);
        std::thread approver([&mgr, known]() {
            resolve_next_new(*mgr, known, ApprovalDecision::ALLOW_ONCE);
        });

        auto out = run_before_tool_call_policy(ctx, empty_params());
        approver.join();

        CHECK(out.kind == HookOutcomeKind::Pass, "批准 → Pass");
        CHECK(!out.blocked, "未阻塞");
        // OpenClaw approvalResolution: "allow-once" / "allow-always"
        CHECK(out.approval_resolution.value_or("") == "allow-once",
              "approval_resolution=allow-once [不可回退]");
        // ★ 参数必须原样带出（供后续 stage / 执行使用）
        CHECK(out.params.contains("x"), "参数带出");
    }

    // ── ALLOW_ALWAYS → resolution 区分 ─────────────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_manager = mgr.get();
        ctx.approval_timeout_ms = 5000;

        const auto known = snapshot_ids(*mgr);
        std::thread approver([&mgr, known]() {
            resolve_next_new(*mgr, known, ApprovalDecision::ALLOW_ALWAYS);
        });

        auto out = run_before_tool_call_policy(ctx, empty_params());
        approver.join();

        CHECK(out.kind == HookOutcomeKind::Pass, "allow-always → Pass");
        // ★ 必须与 allow-once 区分: 前者会进白名单, 审计语义不同
        CHECK(out.approval_resolution.value_or("") == "allow-always",
              "resolution 区分 once/always [不可回退]");
    }

    // ── 用户 DENY → Failure{blocked} (★ 不是 Veto) ─────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_manager = mgr.get();
        ctx.approval_timeout_ms = 5000;

        const auto known = snapshot_ids(*mgr);
        std::thread denier([&mgr, known]() {
            resolve_next_new(*mgr, known, ApprovalDecision::DENY);
        });

        auto out = run_before_tool_call_policy(ctx, empty_params());
        denier.join();

        // ★★ [不可回退] 用户 DENY 是 Failure 而 mode=Deny 是 Veto ——
        //   这【不是笔误】, OpenClaw approval.ts:232-240 vs :526-535 原样如此。
        //   区别: 此处审批流程跑完了(有 disposition 可记), 那里根本没跑。
        CHECK(out.kind == HookOutcomeKind::Failure,
              "用户 DENY → Failure (不是 Veto) [不可回退]");
        CHECK(out.disposition.value_or("") == disposition::kBlocked,
              "disposition=blocked [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
              "denied_reason=plugin-approval");
        CHECK(!out.approval_resolution.has_value(), "拒绝时无 resolution");
    }

    // ── TIMEOUT → Failure{timed_out} ───────────────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_manager = mgr.get();
        ctx.approval_timeout_ms = 120;   // 故意很短, 无人决策

        auto out = run_before_tool_call_policy(ctx, empty_params());

        CHECK(out.kind == HookOutcomeKind::Failure, "超时 → Failure");
        // ★ [不可回退] disposition 必须是 timed_out 而非 blocked ——
        //   "没人来批" 与 "被明确拒绝" 在运维上是两件事:
        //   前者要查审批通道是否可达, 后者是正常业务决策。
        CHECK(out.disposition.value_or("") == disposition::kTimedOut,
              "disposition=timed_out (区别于 blocked) [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
              "denied_reason=plugin-approval");
    }

    std::remove(tmp_cfg_path().c_str());
}

// ============================================================================
// mode=Defer → Pass + deferred 凭据, 不阻塞
// ============================================================================
static void test_mode_defer() {
    SECTION("mode=Defer");

    auto mgr = ApprovalManager::create(tmp_cfg_path());

    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_mode = ApprovalMode::Defer;
    ctx.approval_manager = mgr.get();
    ctx.approval_timeout_ms = 5000;

    const auto t0 = std::chrono::steady_clock::now();
    auto out = run_before_tool_call_policy(ctx, empty_params());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    CHECK(out.kind == HookOutcomeKind::Pass, "Defer → Pass (放行)");
    // ★ [不可回退] Defer 的全部意义是"不在这里等"。
    //   这正是 remote_a2a 用户确认需要的模式: 会话不被独占。
    CHECK(ms < 500, "Defer 不阻塞 [不可回退]");
    // ★★ [不可回退] 必须带出凭据 —— 否则就是"静默跳过审批":
    //   放行了、审批却没人收敛, 等于审批形同虚设。
    CHECK(out.deferred_approval.has_value(), "带出 deferred 凭据 [不可回退]");
    if (out.deferred_approval) {
        CHECK(!out.deferred_approval->approval_id.empty(), "凭据含 approval_id");
        CHECK(out.deferred_approval->tool_name == "danger_tool", "凭据含工具名");
        CHECK(out.deferred_approval->tool_call_id == "call-1", "凭据含 tool_call_id");

        // 审批确实已登记在 manager 里（可被后续收敛）
        auto st = mgr->get_approval_status(out.deferred_approval->approval_id);
        CHECK(st.is_ok(), "审批已登记, 可被查询 [不可回退]");
    }

    // ── Defer 但 manager 缺失 → 不能放行 ───────────────────────────────
    {
        auto c2 = base_ctx();
        c2.tool_requires_approval = true;
        c2.approval_mode = ApprovalMode::Defer;
        c2.approval_manager = nullptr;

        auto o2 = run_before_tool_call_policy(c2, empty_params());
        // ★ [不可回退] 没有 manager 时 Defer 会退化成"放行且无凭据",
        //   那等于静默跳过审批。必须落到不可用分支。
        CHECK(o2.kind == HookOutcomeKind::Failure,
              "Defer 无 manager → 不放行 [不可回退]");
        CHECK(o2.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable");
        CHECK(!o2.deferred_approval.has_value(), "无凭据");
    }

    std::remove(tmp_cfg_path().c_str());
}

// ============================================================================
// Stage 顺序: owner-only 必须在 approval 之前
// ============================================================================
static void test_stage_order() {
    SECTION("Stage 顺序");

    auto mgr = ApprovalManager::create(tmp_cfg_path());

    auto ctx = base_ctx();
    ctx.sender_is_owner = false;        // 非 owner
    ctx.tool_is_owner_only = true;      // owner-only 工具
    ctx.tool_requires_approval = true;  // 同时也需审批
    ctx.approval_mode = ApprovalMode::Request;
    ctx.approval_manager = mgr.get();
    ctx.approval_timeout_ms = 300;

    const auto before = snapshot_ids(*mgr);
    const auto t0 = std::chrono::steady_clock::now();
    auto out = run_before_tool_call_policy(ctx, empty_params());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    // ★★ [不可回退] owner-only 必须先拦 —— 身份不对连问都不该问。
    //   若顺序反了, 一个无权用户会触发弹窗去骚扰 owner,
    //   而且他本来就没资格执行这个工具。
    CHECK(out.kind == HookOutcomeKind::Veto, "owner-only 先拦 → Veto");
    CHECK(out.denied_reason.value_or("") == denied_reason::kOwnerOnly,
          "denied_reason=owner-only (不是 plugin-approval) [不可回退]");
    // 没有走审批 → 不会等待
    CHECK(ms < 200, "未发起审批(未等待) [不可回退]");
    // ⚠ 用增量比对而非 empty()：已决策的请求不会从 pending 移除
    CHECK(snapshot_ids(*mgr) == before,
          "没有产生新的审批请求 [不可回退]");

    std::remove(tmp_cfg_path().c_str());
}

// ============================================================================
// 审批通过后仍需继续后续 stage（批准 ≠ 免检）
// ============================================================================
static void test_approval_not_bypass() {
    SECTION("批准不等于免检");

    auto mgr = ApprovalManager::create(tmp_cfg_path());

    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_manager = mgr.get();
    ctx.approval_timeout_ms = 5000;

    // 审批通过, 但随即置取消标志 → 应被 Stage 5 拦下
    std::atomic<bool> aborted{false};
    ctx.aborted = &aborted;

    std::thread approver([&mgr, &aborted]() {
        // ⚠ 只决策新请求（同上：已决策项不会从 pending 移除）
        std::set<std::string> known;
        for (const auto& r : mgr->get_pending_approvals()) known.insert(r.id);
        if (!resolve_next_new(*mgr, known, ApprovalDecision::ALLOW_ONCE).empty()) {
            // 批准后立刻取消
            aborted.store(true);
        }
    });

    auto out = run_before_tool_call_policy(ctx, empty_params());
    approver.join();

    // ★★ [不可回退] 审批通过【不能】短路后续 stage。
    //   若 Stage 1 通过后直接 return, 取消检查/hook/loop 全被跳过 ——
    //   等于"批准即免检"。
    CHECK(out.kind == HookOutcomeKind::Failure,
          "批准后仍被取消检查拦下 [不可回退]");
    CHECK(out.disposition.value_or("") == disposition::kCancelled,
          "disposition=cancelled");

    std::remove(tmp_cfg_path().c_str());
}

// ============================================================================
int main() {
    std::cout << "===== before_tool_call Stage 1 (approval gate) 测试 ====="
              << std::endl;

    test_no_approval_needed();
    test_manager_missing_refuses();
    test_mode_deny();
    test_mode_report();
    test_mode_request();
    test_mode_defer();
    test_stage_order();
    test_approval_not_bypass();

    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
