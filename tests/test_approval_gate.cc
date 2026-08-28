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
//   ② 审批通道缺失时【拒绝执行】而非放行。
//   ③ 四种 ApprovalMode 的 kind/disposition/denied_reason 逐字对齐 OpenClaw,
//      包括其中的【不对称】(用户 DENY=Failure, mode=Deny=Veto)。
//
// ⚠ 用 fake broker 而非真实 gateway: Stage 1 只依赖 ToolApprovalBroker 接口,
//   这也顺带验证了"tools 层不依赖 gateway"这条边界 ——
//   若哪天 Stage 1 直接引用了 gateway 类型, 本测试会立刻编译失败。
// ============================================================================

#include "hunterclaw/tools/before_tool_call_policy.h"
#include "hunterclaw/tools/tool_approval_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

// ── FakeBroker ──────────────────────────────────────────────────────────────
// 可控的审批通道: 测试线程可以决定"何时、以什么决策"回应。
class FakeBroker : public ToolApprovalBroker {
public:
    struct Req {
        std::string id;
        std::string tool_name;
        std::string tool_call_id;
        std::string agent_id;
        std::string session_key;
        int64_t timeout_ms = 0;
        std::vector<std::string> allowed;
    };

    /// fail_create=true 时模拟"发起失败"(返回空 id)
    bool fail_create = false;

    std::string request_tool_approval(const std::string& tool_name,
                                     const std::string& tool_call_id,
                                     const std::string& agent_id,
                                     const std::string& session_key,
                                     int64_t timeout_ms,
                                     const std::vector<std::string>& allowed) override {
        if (fail_create) return {};
        std::lock_guard<std::mutex> lk(mu_);
        Req r;
        r.id = "appr-" + std::to_string(++seq_);
        r.tool_name = tool_name;
        r.tool_call_id = tool_call_id;
        r.agent_id = agent_id;
        r.session_key = session_key;
        r.timeout_ms = timeout_ms;
        r.allowed = allowed;
        requests_.push_back(r);
        cv_.notify_all();
        return r.id;
    }

    std::optional<std::string> wait_tool_approval(const std::string& approval_id,
                                                 int64_t timeout_ms) override {
        std::unique_lock<std::mutex> lk(mu_);
        const int64_t wait_ms = timeout_ms > 0 ? timeout_ms : 5000;
        const bool got = cv_.wait_for(
            lk, std::chrono::milliseconds(wait_ms),
            [&]() { return decisions_.count(approval_id) > 0; });
        if (!got) return std::nullopt;   // 模拟超时
        return decisions_[approval_id];
    }

    /// 测试侧: 等到出现一个请求并给出决策
    bool decide_next(const std::string& decision, int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lk(mu_);
        const bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                     [&]() { return !requests_.empty(); });
        if (!got) return false;
        decisions_[requests_.back().id] = decision;
        cv_.notify_all();
        return true;
    }

    std::vector<Req> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }
    size_t request_count() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_.size();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Req> requests_;
    std::map<std::string, std::string> decisions_;
    int seq_ = 0;
};

// ── FakeHolder ──────────────────────────────────────────────────────────────
// 把 FakeBroker 包成 shared_ptr 并保持 `holder.xxx()` 的调用手感。
//
// ⚠ BeforeToolCallContext::approval_broker 是 shared_ptr（不是裸指针）——
//   mode=Request 时策略链会持有它跨越一次阻塞等待（可达 120s），
//   裸指针在此期间若被替换/销毁就是悬垂访问。
struct FakeHolder {
    std::shared_ptr<FakeBroker> p = std::make_shared<FakeBroker>();

    FakeBroker* operator->() const { return p.get(); }
    std::shared_ptr<ToolApprovalBroker> as_broker() const { return p; }
};

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

}  // namespace

// ============================================================================
// 不需要审批时直接放行
// ============================================================================
static void test_no_approval_needed() {
    SECTION("不需要审批");

    FakeHolder broker;
    auto ctx = base_ctx();
    // 两个来源都为 false
    ctx.tool_requires_approval = false;
    ctx.policy_requires_approval = false;
    ctx.approval_broker = broker.as_broker();

    auto out = run_before_tool_call_policy(ctx, empty_params());
    CHECK(out.kind == HookOutcomeKind::Pass, "无需审批 → Pass");
    CHECK(!out.blocked, "未被阻塞");
    CHECK(!out.approval_resolution.has_value(), "无审批结论");
    CHECK(!out.deferred_approval.has_value(), "无 deferred 凭据");
    CHECK(out.params.contains("x"), "参数原样透传");
    // ★ [不可回退] 无需审批时【不能】发起审批请求 ——
    //   否则每次工具调用都会给用户弹一次窗
    CHECK(broker->request_count() == 0, "未发起审批请求 [不可回退]");
}

// ============================================================================
// ★★ broker 缺失 → 拒绝, 【绝不】放行
// ============================================================================
static void test_broker_missing_refuses() {
    SECTION("审批通道缺失");

    // ── 工具自声明需审批 ────────────────────────────────────────────────
    {
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;   // ← Tool::requires_approval()
        ctx.approval_broker = nullptr;

        auto out = run_before_tool_call_policy(ctx, empty_params());

        // ★★ [不可回退] 这是本次修复的核心。
        //   此前 Tool::requires_approval() 零消费 —— 声明要审批的工具
        //   直接执行, 声明方以为受保护, 实际毫无防护且无日志可查。
        CHECK(out.kind == HookOutcomeKind::Failure,
              "通道缺失 → Failure, 不放行 [不可回退]");
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
        ctx.approval_broker = nullptr;

        auto out = run_before_tool_call_policy(ctx, empty_params());
        CHECK(out.kind == HookOutcomeKind::Failure,
              "策略来源同样拒绝 [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "同一 denied_reason");
    }

    // ── 发起失败 → 同样不放行 ──────────────────────────────────────────
    {
        FakeHolder broker;
        broker->fail_create = true;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();

        auto out = run_before_tool_call_policy(ctx, empty_params());
        // ★ [不可回退] 发起失败绝不能当成"批准"。
        CHECK(out.kind == HookOutcomeKind::Failure, "发起失败 → 不放行 [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable");
    }

    // ★ 两个来源取「或」—— 任一为真即需审批, 不能二选一:
    //   前者是工具的静态自我声明, 后者是策略对本次参数的动态判定。
}

// ============================================================================
// mode=Deny → Veto{plugin-approval}
// ============================================================================
static void test_mode_deny() {
    SECTION("mode=Deny");

    FakeHolder broker;
    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_mode = ApprovalMode::Deny;
    ctx.approval_broker = broker.as_broker();

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
    // ★ [不可回退] Deny 不该发起审批 —— 结论已定, 弹窗只会骚扰用户
    CHECK(broker->request_count() == 0, "未发起审批请求 [不可回退]");
}

// ============================================================================
// mode=Report → Failure{blocked}, 不阻塞不等待
// ============================================================================
static void test_mode_report() {
    SECTION("mode=Report");

    FakeHolder broker;
    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_mode = ApprovalMode::Report;
    ctx.approval_broker = broker.as_broker();

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
    CHECK(out.blocked, "blocked=true (报回给模型/上层)");
    // ★ Report 不发起审批 —— 它只是报告
    CHECK(broker->request_count() == 0, "未发起审批请求 [不可回退]");
}

// ============================================================================
// mode=Request → 等待决策；覆盖 ALLOW / DENY / TIMEOUT
// ============================================================================
static void test_mode_request() {
    SECTION("mode=Request");

    // ── ALLOW_ONCE → Pass + approval_resolution ────────────────────────
    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_mode = ApprovalMode::Request;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 5000;

        std::thread approver([&broker]() { broker->decide_next("allow-once"); });
        auto out = run_before_tool_call_policy(ctx, empty_params());
        approver.join();

        CHECK(out.kind == HookOutcomeKind::Pass, "批准 → Pass");
        CHECK(!out.blocked, "未阻塞");
        // OpenClaw approvalResolution: "allow-once" / "allow-always"
        CHECK(out.approval_resolution.value_or("") == "allow-once",
              "approval_resolution=allow-once [不可回退]");
        CHECK(out.params.contains("x"), "参数带出");

        // ★★ [不可回退] 请求载荷必须带上 tool_name / tool_call_id ——
        //   这两个字段是 PluginApprovalRequestPayload 的核心,
        //   缺了客户端 UI 无法告诉用户"在批什么工具"。
        auto reqs = broker->requests();
        CHECK(reqs.size() == 1, "发起了一次审批");
        if (!reqs.empty()) {
            CHECK(reqs[0].tool_name == "danger_tool", "载荷含 tool_name [不可回退]");
            CHECK(reqs[0].tool_call_id == "call-1", "载荷含 tool_call_id [不可回退]");
            CHECK(reqs[0].agent_id == "agent-1", "载荷含 agent_id");
            CHECK(reqs[0].session_key == "sess-1", "载荷含 session_key");
        }
    }

    // ── ALLOW_ALWAYS → resolution 区分 ─────────────────────────────────
    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 5000;

        std::thread approver([&broker]() { broker->decide_next("allow-always"); });
        auto out = run_before_tool_call_policy(ctx, empty_params());
        approver.join();

        CHECK(out.kind == HookOutcomeKind::Pass, "allow-always → Pass");
        // ★ 必须与 allow-once 区分: 前者会进白名单, 审计语义不同
        CHECK(out.approval_resolution.value_or("") == "allow-always",
              "resolution 区分 once/always [不可回退]");
    }

    // ── 用户 DENY → Failure{blocked} (★ 不是 Veto) ─────────────────────
    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 5000;

        std::thread denier([&broker]() { broker->decide_next("deny"); });
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
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 150;   // 无人决策

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

    // ── 未知决策字符串 → 不放行 ────────────────────────────────────────
    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 3000;

        std::thread t([&broker]() { broker->decide_next("approve"); });  // 非法值
        auto out = run_before_tool_call_policy(ctx, empty_params());
        t.join();

        // ★★ [不可回退] broker 返回非法决策时宁可拒绝也不能当批准。
        //   "approve" 恰好是 plugin_approval.cc 曾经错误接受的值,
        //   若这里当成放行, 一个老客户端就能绕过审批语义。
        CHECK(out.kind == HookOutcomeKind::Failure,
              "未知决策 → 不放行 [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
              "denied_reason=plugin-approval");
    }

    // ── allowed_decisions 透传 ─────────────────────────────────────────
    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_broker = broker.as_broker();
        ctx.approval_timeout_ms = 3000;
        // ★ 限定只允许一次性授权 —— 用于"确认转账"这类绝不能永久授权的操作
        ctx.allowed_decisions = {"allow-once", "deny"};

        std::thread t([&broker]() { broker->decide_next("allow-once"); });
        auto out = run_before_tool_call_policy(ctx, empty_params());
        t.join();

        CHECK(out.kind == HookOutcomeKind::Pass, "限定决策集下仍可批准");
        auto reqs = broker->requests();
        CHECK(!reqs.empty() && reqs[0].allowed.size() == 2,
              "allowed_decisions 透传到载荷 [不可回退]");
    }
}

// ============================================================================
// mode=Defer → Pass + deferred 凭据, 不阻塞
// ============================================================================
static void test_mode_defer() {
    SECTION("mode=Defer");

    {
        FakeHolder broker;
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_mode = ApprovalMode::Defer;
        ctx.approval_broker = broker.as_broker();
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
        }
        // ★ 审批确实已发起（可被后续收敛）
        CHECK(broker->request_count() == 1, "审批已发起 [不可回退]");
    }

    // ── Defer 但 broker 缺失 → 不能放行 ────────────────────────────────
    {
        auto c2 = base_ctx();
        c2.tool_requires_approval = true;
        c2.approval_mode = ApprovalMode::Defer;
        c2.approval_broker = nullptr;

        auto o2 = run_before_tool_call_policy(c2, empty_params());
        // ★ [不可回退] 没有通道时 Defer 会退化成"放行且无凭据",
        //   那等于静默跳过审批。必须落到不可用分支。
        CHECK(o2.kind == HookOutcomeKind::Failure,
              "Defer 无通道 → 不放行 [不可回退]");
        CHECK(o2.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable");
        CHECK(!o2.deferred_approval.has_value(), "无凭据");
    }
}

// ============================================================================
// Stage 顺序: owner-only 必须在 approval 之前
// ============================================================================
static void test_stage_order() {
    SECTION("Stage 顺序");

    FakeHolder broker;
    auto ctx = base_ctx();
    ctx.sender_is_owner = false;        // 非 owner
    ctx.tool_is_owner_only = true;      // owner-only 工具
    ctx.tool_requires_approval = true;  // 同时也需审批
    ctx.approval_mode = ApprovalMode::Request;
    ctx.approval_broker = broker.as_broker();
    ctx.approval_timeout_ms = 300;

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
    CHECK(ms < 200, "未发起审批(未等待) [不可回退]");
    CHECK(broker->request_count() == 0, "没有产生审批请求 [不可回退]");
}

// ============================================================================
// 审批通过后仍需继续后续 stage（批准 ≠ 免检）
// ============================================================================
static void test_approval_not_bypass() {
    SECTION("批准不等于免检");

    FakeHolder broker;
    auto ctx = base_ctx();
    ctx.tool_requires_approval = true;
    ctx.approval_broker = broker.as_broker();
    ctx.approval_timeout_ms = 5000;

    // 审批通过, 但随即置取消标志 → 应被最后的取消检查拦下
    std::atomic<bool> aborted{false};
    ctx.aborted = &aborted;

    std::thread approver([&broker, &aborted]() {
        if (broker->decide_next("allow-once")) {
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
}

// ============================================================================
// 进程级单例回落 + 注入语义
// ============================================================================
static void test_runtime_singleton() {
    SECTION("进程级单例");

    auto& rt = ToolApprovalRuntime::instance();
    rt.reset_for_testing();

    // ── 未注入 → Stage 1 拒绝（fail-closed）─────────────────────────────
    {
        CHECK(!rt.has_broker(), "初始未注入");
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        // ★ 刻意不设 ctx.approval_broker —— 走单例回落路径
        auto out = run_before_tool_call_policy(ctx, empty_params());
        // ★★ [不可回退] 单例未注入时必须拒绝, 【绝不】放行。
        //   这正是 Tool::requires_approval() 零消费时代的缺口:
        //   声明要审批的工具直接执行, 且无日志可查。
        CHECK(out.kind == HookOutcomeKind::Failure,
              "单例未注入 → 拒绝执行 [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable [不可回退]");
    }

    // ── 注入后 → 自动回落到单例（ctx 不填也能用）────────────────────────
    {
        auto fake = std::make_shared<FakeBroker>();
        CHECK(rt.set_broker(fake), "首次注入成功");
        CHECK(rt.has_broker(), "has_broker 为真");

        auto ctx = base_ctx("tool_via_singleton");
        ctx.tool_requires_approval = true;
        ctx.approval_timeout_ms = 3000;
        // ★ 仍然不设 ctx.approval_broker

        std::thread approver([fake]() { fake->decide_next("allow-once"); });
        auto out = run_before_tool_call_policy(ctx, empty_params());
        approver.join();

        // ★★ [不可回退] 这是整条接线的闭环: react_engine / tool_manager
        //   都【不再】按会话注入 broker（那会产生 N 个待审列表），
        //   而是靠这里的单例回落。若回落失效, 所有需审批工具会被拒绝,
        //   表现为"审批配好了但工具全不可用"。
        CHECK(out.kind == HookOutcomeKind::Pass,
              "自动回落到单例并获批 [不可回退]");
        CHECK(out.approval_resolution.value_or("") == "allow-once",
              "决策带出");
        auto reqs = fake->requests();
        CHECK(!reqs.empty() && reqs[0].tool_name == "tool_via_singleton",
              "审批确实经由单例 broker 发起 [不可回退]");
    }

    // ── 重复注入被拒 ────────────────────────────────────────────────────
    {
        auto other = std::make_shared<FakeBroker>();
        // ★★ [不可回退] 重复注入会让先前已发起的审批变成孤儿:
        //   等待方仍在旧 broker 上等, 新决策落到新 broker,
        //   于是那批审批必然等满超时。
        CHECK(!rt.set_broker(other), "重复注入被拒 [不可回退]");
        CHECK(rt.has_broker(), "原 broker 仍在位");
        // 验证生效的仍是第一个（新的没收到任何请求）
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_mode = ApprovalMode::Defer;   // 不阻塞
        auto out = run_before_tool_call_policy(ctx, empty_params());
        CHECK(out.kind == HookOutcomeKind::Pass, "Defer 放行");
        CHECK(other->request_count() == 0,
              "第二个 broker 未被使用 [不可回退]");
    }

    // ── 拒绝注入 null ───────────────────────────────────────────────────
    {
        rt.reset_for_testing();
        // ★ null 与"未注入"无法区分, 却会占掉唯一的注入机会 ——
        //   之后真正的 broker 就再也设不进来了。
        CHECK(!rt.set_broker(nullptr), "拒绝注入 null [不可回退]");
        CHECK(!rt.has_broker(), "仍视为未注入");
        auto real = std::make_shared<FakeBroker>();
        CHECK(rt.set_broker(real), "null 未占掉注入机会 [不可回退]");
    }

    // ── ctx 显式给的 broker 优先于单例 ──────────────────────────────────
    {
        rt.reset_for_testing();
        auto singleton_broker = std::make_shared<FakeBroker>();
        rt.set_broker(singleton_broker);

        auto explicit_broker = std::make_shared<FakeBroker>();
        auto ctx = base_ctx();
        ctx.tool_requires_approval = true;
        ctx.approval_mode = ApprovalMode::Defer;
        ctx.approval_broker = explicit_broker;

        auto out = run_before_tool_call_policy(ctx, empty_params());
        CHECK(out.kind == HookOutcomeKind::Pass, "Defer 放行");
        // ★ 显式注入优先 —— 这是单测能隔离运行的前提
        CHECK(explicit_broker->request_count() == 1, "用了显式 broker");
        CHECK(singleton_broker->request_count() == 0, "未用单例 broker");
    }

    rt.reset_for_testing();
}

// ============================================================================
int main() {
    std::cout << "===== before_tool_call Stage 1 (approval gate) 测试 ====="
              << std::endl;

    test_no_approval_needed();
    test_broker_missing_refuses();
    test_mode_deny();
    test_mode_report();
    test_mode_request();
    test_mode_defer();
    test_stage_order();
    test_approval_not_bypass();
    test_runtime_singleton();

    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
