// ============================================================================
// test_after_tool_call_policy.cc — 执行后二次确认策略
//
// 对齐目标: OpenClaw
//   embedded-agent-subscribe.handlers.tools.ts:1885-1917（结果先加工再发出）
//   agent-tools.before-tool-call.approval.ts（四模式 / denied_reason 语义）
//
// ★ 本套锁死的核心不变量:
//   ① 未确认的结果【绝不采信】—— 通道缺失/超时/拒绝全部替换为 blocked
//   ② 拒绝时必须【替换】原结果而非附加说明（原文本会被模型当成事实）
//   ③ 未声明 postExecConfirm 时零开销、结果原样通过
//   ④ Defer 在本阶段降级为 Request（结果已产出，放行等于未确认就采信）
// ============================================================================

#include "hunterclaw/tools/after_tool_call_policy.h"
#include "hunterclaw/tools/tool_approval_runtime.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace hunterclaw;
using namespace hunterclaw::tools;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (cond) { ++g_pass; }                                            \
        else {                                                             \
            ++g_fail;                                                      \
            std::cout << "  FAIL: " << (msg) << "  @" << __LINE__          \
                      << std::endl;                                        \
        }                                                                  \
    } while (0)

#define SECTION(name) std::cout << "[" << (name) << "]" << std::endl

namespace {

class FakeBroker : public ToolApprovalBroker {
public:
    std::string request_tool_approval(const std::string&, const std::string&,
                                     const std::string&, const std::string&,
                                     int64_t timeout_ms,
                                     const std::vector<std::string>& allowed) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_create) return {};
        ++requests_;
        last_allowed_ = allowed;
        last_timeout_ = timeout_ms;
        last_id_ = "appr-" + std::to_string(requests_);
        cv_.notify_all();
        return last_id_;
    }

    std::optional<std::string> wait_tool_approval(const std::string& id,
                                                 int64_t timeout_ms,
                                                 const std::atomic<bool>* aborted
                                                     = nullptr) override {
        std::unique_lock<std::mutex> lk(mu_);
        const int64_t w = timeout_ms > 0 ? timeout_ms : 2000;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(w);
        while (decisions_.count(id) == 0) {
            if (aborted && aborted->load()) return std::nullopt;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return std::nullopt;
            auto slice = std::chrono::milliseconds(20);
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining < slice) slice = remaining;
            if (!aborted) slice = remaining;
            cv_.wait_for(lk, slice, [&]() { return decisions_.count(id) > 0; });
        }
        return decisions_[id];
    }

    /// 预置下一个请求的决策（在 request 之前调用也可）
    void preset(const std::string& d) {
        std::lock_guard<std::mutex> lk(mu_);
        preset_ = d;
    }
    /// 等到出现请求后给决策
    bool decide(const std::string& d, int timeout_ms = 2000) {
        std::unique_lock<std::mutex> lk(mu_);
        const bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                     [&]() { return !last_id_.empty(); });
        if (!got) return false;
        decisions_[last_id_] = d;
        cv_.notify_all();
        return true;
    }

    bool fail_create = false;
    int requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_;
    }
    std::vector<std::string> last_allowed() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_allowed_;
    }
    int64_t last_timeout() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_timeout_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::string> decisions_;
    std::vector<std::string> last_allowed_;
    std::string last_id_;
    std::string preset_;
    int64_t last_timeout_ = 0;
    int requests_ = 0;
};

/// 构造一个"远端说转账已提交"的成功结果 + 确认声明
ToolResult make_result_with_confirm(const std::vector<std::string>& tokens = {},
                                   const std::string& mode = "request") {
    ToolResult r;
    r.success = true;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Text;
    // ★ 刻意用一句【肯定陈述】—— 它是测试②的关键:
    //   若拒绝时保留这段文本，模型会当成已发生的事实继续往下做。
    c.text = "转账 5000 元已提交";
    r.content.push_back(c);

    core::JsonValue d = core::JsonValue::object();
    d["taskId"] = std::string("task-88");
    PostExecConfirmRequest req;
    req.title = "确认转账";
    req.description = "请确认转账 5000 元";
    req.token_ids = tokens;
    req.mode = mode;
    req.allowed_decisions = {"allow-once", "deny"};
    attach_post_exec_confirm(d, req);
    r.details = std::move(d);
    return r;
}

AfterToolCallContext make_ctx(std::shared_ptr<ToolApprovalBroker> b) {
    AfterToolCallContext c;
    c.tool_name = "remote_a2a__wx";
    c.tool_call_id = "call-1";
    c.run_id = "run-1";
    c.agent_id = "a1";
    c.session_key = "s1";
    c.approval_broker = std::move(b);
    return c;
}

std::string first_text(const ToolResult& r) {
    for (const auto& c : r.content) {
        if (c.type == ToolResultContent::Type::Text) return c.text;
    }
    return {};
}

}  // namespace

// ============================================================================
static void test_no_confirm_declared() {
    SECTION("未声明确认");

    auto broker = std::make_shared<FakeBroker>();

    ToolResult r;
    r.success = true;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Text;
    c.text = "普通结果";
    r.content.push_back(c);
    r.details = core::JsonValue::object();

    auto out = run_after_tool_call_policy(make_ctx(broker), r);

    CHECK(out.kind == AfterToolCallOutcomeKind::Unchanged, "Unchanged");
    CHECK(out.result.success, "结果原样通过");
    CHECK(first_text(out.result) == "普通结果", "文本未变 [不可回退]");
    // ★★ [不可回退] 绝大多数工具调用走这条 —— 不能发起任何审批,
    //   否则每次工具调用都弹窗。
    CHECK(broker->requests() == 0, "未发起审批 [不可回退]");
    CHECK(!out.needs_compensation(), "无需补偿");
}

// ============================================================================
static void test_confirmed() {
    SECTION("用户确认");

    auto broker = std::make_shared<FakeBroker>();
    auto r = make_result_with_confirm({"tk-1", "tk-2"});

    std::thread t([broker]() { broker->decide("allow-once"); });
    auto out = run_after_tool_call_policy(make_ctx(broker), r);
    t.join();

    CHECK(out.kind == AfterToolCallOutcomeKind::Confirmed, "Confirmed");
    CHECK(out.result.success, "结果通过");
    CHECK(first_text(out.result) == "转账 5000 元已提交", "原文本保留");
    CHECK(out.decision.value_or("") == "allow-once", "决策带出");
    // 审计标记
    CHECK(out.result.details.has_key("postExecConfirmed"), "details 标注已确认");
    CHECK(out.result.details["postExecConfirmed"].as_bool(), "标注为 true");
    CHECK(!out.needs_compensation(), "确认后无需补偿");

    // ★ allowed_decisions 透传 —— 不允许 allow-always。
    //   一次误点永久授权会让后续同类操作全部免确认。
    auto allowed = broker->last_allowed();
    CHECK(allowed.size() == 2, "allowed_decisions 透传 [不可回退]");
    bool has_always = false;
    for (const auto& a : allowed) if (a == "allow-always") has_always = true;
    CHECK(!has_always, "不含 allow-always [不可回退]");
}

// ============================================================================
static void test_denied_replaces_result() {
    SECTION("用户拒绝");

    auto broker = std::make_shared<FakeBroker>();
    auto r = make_result_with_confirm();

    std::thread t([broker]() { broker->decide("deny"); });
    auto out = run_after_tool_call_policy(make_ctx(broker), r);
    t.join();

    CHECK(out.kind == AfterToolCallOutcomeKind::Rejected, "Rejected");
    // ★★★ [不可回退] 这是本套最关键的断言。
    //   必须【替换】原结果而非附加说明: 原文本是"转账 5000 元已提交",
    //   若保留它并只加一句"未确认", 模型看到的仍是那句肯定陈述 ——
    //   它会当成已发生的事实继续往下做（比如去发通知说转账成功）,
    //   而实际上什么都没确认。
    CHECK(!out.result.success, "success=false [不可回退]");
    CHECK(first_text(out.result).find("转账 5000 元已提交") == std::string::npos,
          "原肯定陈述被移除 [不可回退]");
    CHECK(first_text(out.result).find("未获用户确认") != std::string::npos,
          "文本明确说明未确认 [不可回退]");
    CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApproval,
          "denied_reason=plugin-approval");
    // ★ 需要补偿（远端任务还在跑，应回收）
    CHECK(out.needs_compensation(), "标记需补偿 [不可回退]");
    // details 保留原结构化信息供审计，但标出未确认
    CHECK(out.result.details.has_key("taskId"), "details 保留 taskId（供补偿用）");
    CHECK(!out.result.details["postExecConfirmed"].as_bool(), "标注未确认");
}

// ============================================================================
static void test_timeout_and_unavailable() {
    SECTION("超时与通道缺失");

    // ── 超时 ────────────────────────────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        auto r = make_result_with_confirm();
        // 缩短等待
        core::JsonValue d = r.details;
        PostExecConfirmRequest req;
        req.title = "确认转账";
        req.description = "请确认";
        req.mode = "request";
        req.timeout_ms = 120;
        attach_post_exec_confirm(d, req);
        r.details = d;

        auto out = run_after_tool_call_policy(make_ctx(broker), r);
        // ★★ [不可回退] 超时 == 未确认。不能因为"等过了"就采信。
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected, "超时 → Rejected");
        CHECK(!out.result.success, "超时不采信 [不可回退]");
        CHECK(first_text(out.result).find("转账 5000 元已提交") == std::string::npos,
              "原文本被移除 [不可回退]");
    }

    // ── 通道缺失（ctx 与单例都没有）───────────────────────────────────
    {
        ToolApprovalRuntime::instance().reset_for_testing();
        auto r = make_result_with_confirm();
        AfterToolCallContext c;
        c.tool_name = "t";
        // approval_broker 留空 → 回落单例 → 也没有

        auto out = run_after_tool_call_policy(c, r);
        // ★★ [不可回退] fail-closed。放行未确认的结果比拒绝危险得多:
        //   它会进 transcript 并进入下一轮 prompt。
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected,
              "无通道 → Rejected [不可回退]");
        CHECK(out.denied_reason.value_or("") == denied_reason::kPluginApprovalUnavail,
              "denied_reason=plugin-approval-unavailable");
        CHECK(!out.result.success, "不采信 [不可回退]");
    }

    // ── 发起失败 ────────────────────────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        broker->fail_create = true;
        auto out = run_after_tool_call_policy(make_ctx(broker),
                                            make_result_with_confirm());
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected,
              "发起失败 → Rejected [不可回退]");
        CHECK(!out.result.success, "不采信");
    }

    // ── 未知决策 ────────────────────────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        std::thread t([broker]() { broker->decide("approve"); });   // 非法值
        auto out = run_after_tool_call_policy(make_ctx(broker),
                                            make_result_with_confirm());
        t.join();
        // ★ "approve" 曾是 plugin_approval.cc 错误接受的值。
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected,
              "未知决策 → Rejected [不可回退]");
    }
}

// ============================================================================
static void test_mode_variants() {
    SECTION("模式变体");

    // ── Defer 被降级为 Request ─────────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        auto r = make_result_with_confirm({}, "defer");

        std::thread t([broker]() { broker->decide("allow-once"); });
        auto out = run_after_tool_call_policy(make_ctx(broker), r);
        t.join();

        // ★★ [不可回退] Defer 在本阶段语义可疑: 结果已产出,
        //   放行等于"未确认就采信" —— 那份结果会立刻进 transcript,
        //   模型据此继续往下做, 等确认回来时后续动作可能已经发生。
        //   故必须降级为 Request（真的等）。
        CHECK(out.kind == AfterToolCallOutcomeKind::Confirmed,
              "Defer 降级为 Request 并真的等 [不可回退]");
        CHECK(broker->requests() == 1, "确实发起并等待了");
    }

    // ── Deny：不发起，直接作废 ─────────────────────────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        auto out = run_after_tool_call_policy(make_ctx(broker),
                                            make_result_with_confirm({}, "deny"));
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected, "Deny → Rejected");
        CHECK(!out.result.success, "结果作废 [不可回退]");
        // ★ 不发起审批（结论已定，弹窗只会骚扰）
        CHECK(broker->requests() == 0, "未发起审批 [不可回退]");
    }

    // ── Report：同样作废（没人确认过就是不能采信）───────────────────────
    {
        auto broker = std::make_shared<FakeBroker>();
        auto out = run_after_tool_call_policy(make_ctx(broker),
                                            make_result_with_confirm({}, "report"));
        // ★★ [不可回退] 与 before 阶段不同 —— 那里 Report 是"报回需审批",
        //   这里结果已经产出, "没人确认"就必须作废。
        CHECK(out.kind == AfterToolCallOutcomeKind::Rejected, "Report → Rejected");
        CHECK(!out.result.success, "结果作废 [不可回退]");
        CHECK(broker->requests() == 0, "未发起审批");
    }
}

// ============================================================================
static void test_failed_tool_skips_confirm() {
    SECTION("工具失败时跳过确认");

    auto broker = std::make_shared<FakeBroker>();
    auto r = make_result_with_confirm();
    r.success = false;          // 工具本身失败
    r.error = "远端不可达";

    auto out = run_after_tool_call_policy(make_ctx(broker), r);

    CHECK(out.kind == AfterToolCallOutcomeKind::Unchanged, "Unchanged");
    // ★ [不可回退] 没有有效结果可确认。弹一张"确认失败的操作"的卡片
    //   只会让用户困惑，且用户无论点什么都改变不了失败这件事。
    CHECK(broker->requests() == 0, "失败结果不发起确认 [不可回退]");
    CHECK(!out.result.success, "仍是失败");
}

// ============================================================================
static void test_extract_attach() {
    SECTION("提取与写入");

    // 往返一致
    {
        core::JsonValue d = core::JsonValue::object();
        PostExecConfirmRequest in;
        in.title = "T";
        in.description = "D";
        in.token_ids = {"a", "b"};
        in.mode = "request";
        in.allowed_decisions = {"allow-once", "deny"};
        in.timeout_ms = 5000;
        attach_post_exec_confirm(d, in);

        auto out = extract_post_exec_confirm(d);
        CHECK(out.has_value(), "可提取");
        if (out) {
            CHECK(out->title == "T", "title 往返");
            CHECK(out->description == "D", "description 往返");
            CHECK(out->token_ids.size() == 2, "tokenIds 往返");
            CHECK(out->mode == "request", "mode 往返");
            CHECK(out->allowed_decisions.size() == 2, "allowedDecisions 往返");
            CHECK(out->timeout_ms == 5000, "timeoutMs 往返");
        }
    }

    // 无声明
    CHECK(!extract_post_exec_confirm(core::JsonValue::object()).has_value(),
          "无声明 → nullopt");

    // ★ [不可回退] title/description 都空 → 视为无效并忽略。
    //   一个空确认请求会弹出一张什么都没写的卡片，用户无从判断；
    //   而拒绝它又会作废一个本可通过的结果。
    {
        core::JsonValue d = core::JsonValue::object();
        core::JsonValue c = core::JsonValue::object();
        c["tokenIds"] = core::JsonValue::array();
        d["postExecConfirm"] = c;
        CHECK(!extract_post_exec_confirm(d).has_value(),
              "空 title+description 被忽略 [不可回退]");
    }

    // ★ 未知决策项被丢弃 —— 若带进 allowed_decisions，会得到
    //   "任何合法决策都不在允许集里"的审批，永远无法通过。
    {
        core::JsonValue d = core::JsonValue::object();
        core::JsonValue c = core::JsonValue::object();
        c["title"] = std::string("T");
        c["description"] = std::string("D");
        core::JsonValue arr = core::JsonValue::array();
        arr.push_back(core::JsonValue(std::string("approve")));   // 非法
        arr.push_back(core::JsonValue(std::string("deny")));
        c["allowedDecisions"] = arr;
        d["postExecConfirm"] = c;

        auto out = extract_post_exec_confirm(d);
        CHECK(out.has_value(), "可提取");
        if (out) {
            CHECK(out->allowed_decisions.size() == 1,
                  "非法决策项被丢弃 [不可回退]");
            CHECK(out->allowed_decisions[0] == "deny", "保留合法项");
        }
    }
}

// ============================================================================
int main() {
    std::cout << "===== after_tool_call 执行后确认 测试 =====" << std::endl;

    test_no_confirm_declared();
    test_confirmed();
    test_denied_replaces_result();
    test_timeout_and_unavailable();
    test_mode_variants();
    test_failed_tool_skips_confirm();
    test_extract_attach();

    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
