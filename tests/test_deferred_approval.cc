// ============================================================================
// test_deferred_approval.cc — Defer 凭据登记表 + 审批模式解析
//
// 对齐目标: OpenClaw
//   src/agents/harness/native-hook-relay-permissions.ts
//     setNativeHookRelayPreToolUseApproval / resolveNativeHookRelay...
//   src/agents/harness/native-hook-relay-codec.ts:168-176
//     nativeHookRelayParamsWereRewritten
//
// ★ 本套锁死的核心不变量:
//   ① 参数指纹不一致 ⇒ 拒绝执行（防"拿批准 A 的授权执行 B"）
//   ② 收敛是【一次性】的（同一张凭据不能被用两次）
//   ③ 未获批一律不放行（TimedOut / NotFound / ParamsRewritten 全算未获批）
//   ④ 未知 approval_mode 回落 Request（最严），不回落 Report/Deny
// ============================================================================

#include "hunterclaw/tools/deferred_approval_registry.h"
#include "hunterclaw/tools/tool_approval_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    std::string request_tool_approval(const std::string& tool_name,
                                     const std::string&, const std::string&,
                                     const std::string&, int64_t,
                                     const std::vector<std::string>&) override {
        std::lock_guard<std::mutex> lk(mu_);
        last_tool_ = tool_name;
        ++request_calls_;
        // ★ 审批 id 在【收敛点】才生成 —— Defer 的 Stage 1 不再 create。
        //   这里把 id 与 tool_name 绑定, 让测试能在 resolve 之前
        //   用 tool_name 预设决策。
        const std::string id = "appr-" + tool_name + "-" + std::to_string(++seq_);
        // 若测试已按 tool_name 预设过决策, 立刻兑现到这个新 id 上。
        auto pre = pre_decisions_.find(tool_name);
        if (pre != pre_decisions_.end()) {
            decisions_[id] = pre->second;
            cv_.notify_all();
        }
        issued_ids_[tool_name] = id;
        return id;
    }

    std::optional<std::string> wait_tool_approval(const std::string& id,
                                                 int64_t timeout_ms,
                                                 const std::atomic<bool>* aborted
                                                     = nullptr) override {
        std::unique_lock<std::mutex> lk(mu_);
        ++wait_calls_;
        const int64_t w = timeout_ms > 0 ? timeout_ms : 3000;
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

    /// 按【工具名】预设决策 —— 在 resolve 之前调用。
    /// ★ 因为 approval id 直到收敛点才生成, 测试无法预先知道它。
    void set_decision_for_tool(const std::string& tool, const std::string& d) {
        std::lock_guard<std::mutex> lk(mu_);
        pre_decisions_[tool] = d;
        // 若该工具的审批已经发起过, 直接兑现到已发的 id 上。
        auto it = issued_ids_.find(tool);
        if (it != issued_ids_.end()) {
            decisions_[it->second] = d;
        }
        cv_.notify_all();
    }

    void set_decision(const std::string& id, const std::string& d) {
        std::lock_guard<std::mutex> lk(mu_);
        decisions_[id] = d;
        cv_.notify_all();
    }
    int wait_calls() {
        std::lock_guard<std::mutex> lk(mu_);
        return wait_calls_;
    }
    int request_calls() {
        std::lock_guard<std::mutex> lk(mu_);
        return request_calls_;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::string> decisions_;      // id -> decision
    std::map<std::string, std::string> pre_decisions_;  // tool -> decision
    std::map<std::string, std::string> issued_ids_;     // tool -> 已发的 id
    std::string last_tool_;
    int seq_ = 0;
    int wait_calls_ = 0;
    int request_calls_ = 0;
};

/// 构造一张 Defer 凭据。
/// ★ 参数是【工具名】而非 approval_id —— 凭据里已经没有 id 了
///   (对齐 OpenClaw: DeferredPluginToolApproval 持 payload, 不持 id)。
DeferredApproval make_cred(const std::string& tool) {
    DeferredApproval d;
    d.tool_name = tool;
    d.tool_call_id = "c";
    d.agent_id = "a";
    d.session_key = "sk";
    return d;
}

core::JsonValue params_of(const std::string& v) {
    core::JsonValue p = core::JsonValue::object();
    p["arg"] = v;
    return p;
}

}  // namespace

// ============================================================================
static void test_fingerprint_stability() {
    SECTION("参数指纹稳定性");

    // 同内容不同构造顺序 → 指纹必须相同
    core::JsonValue a = core::JsonValue::object();
    a["x"] = std::string("1");
    a["y"] = std::string("2");

    core::JsonValue b = core::JsonValue::object();
    b["y"] = std::string("2");
    b["x"] = std::string("1");

    // ★★ [不可回退] JsonValue 内部用 std::map（types.h:278），键序天然稳定。
    //   若哪天换成 unordered_map，同一份参数会产生不同指纹，
    //   于是【每次】都被判为"被改写"而拒绝执行 ——
    //   表现为"审批批了却还是不执行"，且完全指不到序列化这件事。
    CHECK(params_fingerprint(a) == params_fingerprint(b),
          "键序不影响指纹 [不可回退]");

    // 内容不同 → 指纹必须不同
    CHECK(params_fingerprint(params_of("1")) != params_fingerprint(params_of("2")),
          "内容变化导致指纹变化 [不可回退]");
}

// ============================================================================
static void test_put_and_resolve() {
    SECTION("登记与收敛");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();

    auto broker = std::make_shared<FakeBroker>();
    rt.set_broker(broker);

    const auto p = params_of("v1");
    const std::string fp = params_fingerprint(p);

    // ── 正常批准 ────────────────────────────────────────────────────────
    {
        CHECK(reg.put("run1:call1", make_cred("appr-1"), fp), "登记成功");
        CHECK(reg.size() == 1, "表内 1 条");
        // ★★ [不可回退] 登记本身【不发起审批】——
        //   审批在收敛点才 create(对齐 OpenClaw approval.ts:501-514)。
        //   ⚠ 若 put 就发起, 卡片会在无人等待时弹给用户。
        CHECK(broker->request_calls() == 0, "put 不发起审批 [不可回退]");

        broker->set_decision_for_tool("appr-1", "allow-once");
        auto out = reg.resolve("run1:call1", fp);
        CHECK(out.approved(), "获批");
        CHECK(out.decision == "allow-once", "决策带出");
        // ★★ [不可回退] 审批在【收敛点】发起。
        CHECK(broker->request_calls() == 1, "收敛点发起审批 [不可回退]");
        // ★★ [不可回退] 一次性: 收敛后必须移除。
        //   不移除会让同一张凭据被收敛两次 —— 一次授权被用两次。
        CHECK(reg.size() == 0, "收敛后移除 [不可回退]");

        // 再次收敛 → NotFound
        auto again = reg.resolve("run1:call1", fp);
        CHECK(again.resolution == DeferredResolution::NotFound,
              "同一凭据不能收敛两次 [不可回退]");
        CHECK(!again.approved(), "NotFound 不算获批 [不可回退]");
    }

    // ── 用户拒绝 ────────────────────────────────────────────────────────
    {
        reg.put("run1:call2", make_cred("appr-2"), fp);
        broker->set_decision_for_tool("appr-2", "deny");
        auto out = reg.resolve("run1:call2", fp);
        CHECK(out.resolution == DeferredResolution::Denied, "拒绝");
        CHECK(!out.approved(), "不放行");
        CHECK(reg.size() == 0, "已移除");
    }

    // ── 超时 ────────────────────────────────────────────────────────────
    {
        reg.put("run1:call3", make_cred("appr-3"), fp);
        auto out = reg.resolve("run1:call3", fp, 120);   // 无人决策
        CHECK(out.resolution == DeferredResolution::TimedOut, "超时");
        CHECK(!out.approved(), "超时不放行 [不可回退]");
    }

    // ── 未知决策字符串 → 不放行 ────────────────────────────────────────
    {
        reg.put("run1:call4", make_cred("appr-4"), fp);
        broker->set_decision_for_tool("appr-4", "approve");   // 非法值
        auto out = reg.resolve("run1:call4", fp);
        // ★ "approve" 恰好是 plugin_approval.cc 曾经错误接受的值。
        //   若这里当成放行，一个老客户端就能绕过审批语义。
        CHECK(!out.approved(), "未知决策不放行 [不可回退]");
        // ★ 归为 TimedOut 而非 Denied —— 对齐 OpenClaw
        //   resolvePermittedPluginApprovalResolution 的 fall-through:
        //   未知值既非批准也非明确拒绝, 语义上是"没拿到有效决策"。
        CHECK(out.resolution == DeferredResolution::TimedOut,
              "未知决策归为 TimedOut（对齐 OpenClaw）");
    }

    rt.reset_for_testing();
    reg.reset_for_testing();
}

// ============================================================================
static void test_params_rewritten() {
    SECTION("参数指纹比对");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();
    auto broker = std::make_shared<FakeBroker>();
    rt.set_broker(broker);

    const std::string fp_old = params_fingerprint(params_of("original"));
    const std::string fp_new = params_fingerprint(params_of("rewritten"));

    reg.put("run2:call1", make_cred("appr-9"), fp_old);
    broker->set_decision_for_tool("appr-9", "allow-once");   // 用户【已批准】

    auto out = reg.resolve("run2:call1", fp_new);   // 但参数变了

    // ★★ [不可回退] 这是权限提升的防线。
    //   用户批准的是 "original" 那份参数。若沿用这次批准执行 "rewritten"，
    //   等于"拿着批准 A 的授权执行 B"。
    //   OpenClaw 的处置同样是拒绝 (codec.ts:168-176 + permissions.ts:158-167)。
    CHECK(out.resolution == DeferredResolution::ParamsRewritten,
          "参数被改写 → ParamsRewritten [不可回退]");
    CHECK(!out.approved(), "即便用户已批准也不放行 [不可回退]");

    // ★★ [不可回退] 比对必须在【等待之前】——
    //   放在等待之后也能挡住，但会白等一次用户操作:
    //   用户点了同意却被告知拒绝，体验上无法解释。
    CHECK(broker->wait_calls() == 0, "未进入等待 [不可回退]");
    // ★★ [不可回退] 连审批都【不该发起】——
    //   收敛点发起审批后, 指纹不符意味着这张卡片本就不该存在。
    //   ⚠ 旧实现(Stage 1 就 create)下卡片已经弹出去了, 只能事后作废;
    //     现在能做到根本不打扰用户。
    CHECK(broker->request_calls() == 0, "未发起审批（不打扰用户）[不可回退]");

    // 传空指纹 = 跳过比对
    {
        reg.put("run2:call2", make_cred("appr-10"), fp_old);
        broker->set_decision_for_tool("appr-10", "allow-once");
        auto o2 = reg.resolve("run2:call2", "");
        CHECK(o2.approved(), "空指纹跳过比对");
    }

    rt.reset_for_testing();
    reg.reset_for_testing();
}

// ============================================================================
static void test_replace_and_evict() {
    SECTION("同键替换与容量淘汰");

    auto& reg = DeferredApprovalRegistry::instance();
    reg.reset_for_testing();

    const std::string fp = params_fingerprint(params_of("v"));

    // ── 同键替换 ────────────────────────────────────────────────────────
    reg.put("k", make_cred("old"), fp);
    reg.put("k", make_cred("new"), fp);
    // ★ 替换而非并存 —— 对齐 OpenClaw setNativeHookRelay...:86-88。
    //   并存会让用户看到两张同样的卡片，其中一张永远无人收敛。
    CHECK(reg.size() == 1, "同键替换不并存 [不可回退]");

    // ── 空键被拒 ────────────────────────────────────────────────────────
    // ⚠ 空键会让所有凭据挤在同一条记录上互相覆盖。
    CHECK(!reg.put("", make_cred("x"), fp), "空键被拒 [不可回退]");
    CHECK(reg.size() == 1, "表未受影响");

    // ── 容量淘汰 ────────────────────────────────────────────────────────
    reg.reset_for_testing();
    for (size_t i = 0; i < DeferredApprovalRegistry::kMaxEntries + 10; ++i) {
        reg.put("k" + std::to_string(i), make_cred("a" + std::to_string(i)), fp);
    }
    // ★ 淘汰而非拒绝新登记: 表满通常意味着旧凭据泄漏。
    //   拒绝新登记会把泄漏扩大成"Defer 整体不可用"，淘汰能自愈。
    CHECK(reg.size() <= DeferredApprovalRegistry::kMaxEntries,
          "不超过容量上限 [不可回退]");
    // 最新的一定还在
    CHECK(reg.discard("k" + std::to_string(
              DeferredApprovalRegistry::kMaxEntries + 9)),
          "最新条目未被淘汰 [不可回退]");

    reg.reset_for_testing();
}

// ============================================================================
static void test_discard_prefix() {
    SECTION("按 run 批量清理");

    auto& reg = DeferredApprovalRegistry::instance();
    reg.reset_for_testing();
    const std::string fp = params_fingerprint(params_of("v"));

    reg.put("runA:1", make_cred("a1"), fp);
    reg.put("runA:2", make_cred("a2"), fp);
    reg.put("runB:1", make_cred("b1"), fp);

    CHECK(reg.discard_prefix("runA:") == 2, "清理 runA 的两条");
    CHECK(reg.size() == 1, "runB 保留 [不可回退]");

    // ★★ [不可回退] 空前缀会匹配全部 —— 那是 reset 而不是"清理某个 run"。
    //   静默清空会让其他 run 的在途凭据全部失效。
    CHECK(reg.discard_prefix("") == 0, "空前缀被拒 [不可回退]");
    CHECK(reg.size() == 1, "表未被清空 [不可回退]");

    reg.reset_for_testing();
}

// ============================================================================
static void test_no_broker() {
    SECTION("收敛时通道已消失");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();

    const std::string fp = params_fingerprint(params_of("v"));
    reg.put("k", make_cred("appr-x"), fp);
    // 通道在 Defer 发起之后消失（如 shutdown）
    auto out = reg.resolve("k", fp);

    // ★★ [不可回退] 【不能】当成批准 —— 未决 == 未获批。
    CHECK(!out.approved(), "无通道时不放行 [不可回退]");
    // ★★ [不可回退] 归为 Unavailable 而非 TimedOut。
    //
    //   "审批面挂了"与"用户没响应"是两回事:
    //     · Unavailable → 去查 broker 注入/gateway 状态
    //     · TimedOut    → 去查用户为什么没点
    //   压成同一个值会把排查引向错误方向。
    //
    //   ⚠ 这条断言此前锁的是 TimedOut —— 那是 do_wait 自己实现
    //     create+wait 时留下的漂移: Stage 1 的 Request 路径同样场景
    //     报的是 Failure{failed, plugin-approval-unavailable}。
    //     收敛到 request_plugin_tool_approval 后两条路径必然一致。
    CHECK(out.resolution == DeferredResolution::Unavailable,
          "归为 Unavailable（与 Request 路径一致）[不可回退]");
    CHECK(reg.size() == 0, "凭据仍被移除（不泄漏）");

    reg.reset_for_testing();
}

// ============================================================================
static void test_parse_mode() {
    SECTION("审批模式解析");

    CHECK(parse_approval_mode("request") == ApprovalMode::Request, "request");
    CHECK(parse_approval_mode("report") == ApprovalMode::Report, "report");
    CHECK(parse_approval_mode("deny") == ApprovalMode::Deny, "deny");
    CHECK(parse_approval_mode("defer") == ApprovalMode::Defer, "defer");

    // ★★ [不可回退] 空串与未知值都回落 Request（最严的等待型）。
    //   回落到 Report/Deny 会让一个拼错的配置值悄悄【放宽】审批强度:
    //   Report 是"报个错就算了"（无人确认），Request 至少保证有人看过。
    CHECK(parse_approval_mode("") == ApprovalMode::Request,
          "空串 → Request [不可回退]");
    CHECK(parse_approval_mode("DEFER") == ApprovalMode::Request,
          "大写不识别 → Request [不可回退]");
    CHECK(parse_approval_mode("defe") == ApprovalMode::Request,
          "拼错 → Request（不放宽）[不可回退]");
    CHECK(parse_approval_mode("off") == ApprovalMode::Request,
          "未知值 → Request（不是关闭审批）[不可回退]");

    // 往返一致
    for (auto m : {ApprovalMode::Request, ApprovalMode::Report,
                   ApprovalMode::Deny, ApprovalMode::Defer}) {
        CHECK(parse_approval_mode(approval_mode_to_wire(m)) == m, "往返一致");
    }
}

// ============================================================================
// 并发收敛共享同一次等待
//
// 🎯 逐字对齐 OpenClaw permissions.ts:129 的 `resolutionPromise ??=`
//    及其测试 native-hook-relay.test.ts:2415-2434:
//      · requester 只被调用 1 次
//      · 两个并发调用者【都】拿到 approved-once
static void test_concurrent_resolve_shares_wait() {
    SECTION("并发收敛共享等待");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();
    auto broker = std::make_shared<FakeBroker>();
    rt.set_broker(broker);

    const std::string fp = params_fingerprint(params_of("v"));
    reg.put("runC:1", make_cred("appr-c"), fp);

    // 两个线程同时收敛同一张凭据
    std::vector<DeferredOutcome> results(2);
    std::thread t1([&]() { results[0] = reg.resolve("runC:1", fp, 3000); });
    std::thread t2([&]() { results[1] = reg.resolve("runC:1", fp, 3000); });

    // 等两个线程都进入等待后再给决策
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    broker->set_decision_for_tool("appr-c", "allow-once");

    t1.join();
    t2.join();

    // ★★ [不可回退] 两个调用者拿到【相同结论】。
    //   若第二个拿到 NotFound/拒绝, 则"能否执行"取决于哪条入站通道先到 ——
    //   用户明确批准过的操作会随机失败。
    CHECK(results[0].approved(), "第一个收敛者获批 [不可回退]");
    CHECK(results[1].approved(), "第二个收敛者【也】获批 [不可回退]");
    CHECK(results[0].decision == results[1].decision, "结论一致 [不可回退]");

    // ★★ [不可回退] 只发起一次等待 —— 对齐 OpenClaw 的
    //   `toHaveBeenCalledTimes(1)`。发起两次意味着两个调用者各自等,
    //   决策只能满足一个。
    CHECK(broker->wait_calls() == 1, "只等待一次 [不可回退]");
    // ★★ [不可回退] 也只 create 一次审批 —— 两张卡片会让用户困惑,
    //   且其中一张永远不会被收敛。
    CHECK(broker->request_calls() == 1, "只发起一次审批 [不可回退]");

    // 完成后移除；此后的第三次收敛 → NotFound
    CHECK(reg.size() == 0, "完成后移除");
    auto third = reg.resolve("runC:1", fp);
    CHECK(third.resolution == DeferredResolution::NotFound,
          "决议后的第三次 → NotFound");

    rt.reset_for_testing();
    reg.reset_for_testing();
}

// ============================================================================
// abort 必须能打断收敛等待
//
// ⚠⚠ 没有这条: 用户点 chat.abort 后等待仍挂满 timeout, 而该等待占着
//   main lane 唯一槽位(command_queue.cc:54 max_concurrent=1) ——
//   全 gateway 推理停摆, 用户无法自救。
static void test_abort_interrupts_resolve() {
    SECTION("abort 打断收敛等待");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();
    auto broker = std::make_shared<FakeBroker>();
    rt.set_broker(broker);

    const std::string fp = params_fingerprint(params_of("v"));
    reg.put("runD:1", make_cred("appr-d"), fp);

    std::atomic<bool> aborted{false};
    const auto t0 = std::chrono::steady_clock::now();

    DeferredOutcome out;
    // 给一个很长的超时(10s), 但 100ms 后置位 abort
    std::thread worker([&]() { out = reg.resolve("runD:1", fp, 10000, &aborted); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    aborted.store(true);
    worker.join();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // ★★ [不可回退] 必须【远早于】10s 返回。
    CHECK(elapsed < 2000, "abort 后迅速返回（实测 " +
                              std::to_string(elapsed) + "ms）[不可回退]");
    CHECK(!out.approved(), "abort 不放行 [不可回退]");
    // ★★ [不可回退] 归为 Cancelled 而非 TimedOut ——
    //   "用户主动放弃"与"等满超时"在审计上是不同事件。
    //   与 Stage 1 Request 路径的 Failure{cancelled} 对应。
    CHECK(out.resolution == DeferredResolution::Cancelled,
          "abort 归为 Cancelled（与 Request 路径一致）[不可回退]");

    rt.reset_for_testing();
    reg.reset_for_testing();
}

// ============================================================================
// 越权决策不放行
//
// ★★ 凭据必须带着 allowed_decisions 走。remote_a2a 的二次确认声明
//   allowed={allow-once, deny} 正是为了禁掉 allow-always ——
//   若 Defer 路径不校验, 换个模式就绕过了这个约束。
static void test_disallowed_decision() {
    SECTION("越权决策不放行");

    auto& rt = ToolApprovalRuntime::instance();
    auto& reg = DeferredApprovalRegistry::instance();
    rt.reset_for_testing();
    reg.reset_for_testing();
    auto broker = std::make_shared<FakeBroker>();
    rt.set_broker(broker);

    const std::string fp = params_fingerprint(params_of("v"));

    DeferredApproval cred = make_cred("appr-e");
    cred.allowed_decisions = {"allow-once", "deny"};   // 禁用 allow-always
    reg.put("runE:1", cred, fp);

    broker->set_decision_for_tool("appr-e", "allow-always");    // 越权
    auto out = reg.resolve("runE:1", fp);

    // ★★ [不可回退] allow-always 被禁时【绝不能】当批准。
    CHECK(!out.approved(), "越权的 allow-always 不放行 [不可回退]");
    // 归为 TimedOut（OpenClaw 的 fall-through 就是 TIMEOUT）
    CHECK(out.resolution == DeferredResolution::TimedOut,
          "越权决策归为 TimedOut（对齐 OpenClaw）");

    // 允许集内的决策正常通过
    {
        DeferredApproval c2 = make_cred("appr-f");
        c2.allowed_decisions = {"allow-once", "deny"};
        reg.put("runE:2", c2, fp);
        broker->set_decision_for_tool("appr-f", "allow-once");
        auto o2 = reg.resolve("runE:2", fp);
        CHECK(o2.approved(), "允许集内的决策通过");
    }

    // 空允许集 = 默认全集（不限制）
    // ⚠ 若把空集理解成"什么都不允许", 未配 allowedDecisions 的审批
    //   将永远无法通过。
    {
        reg.put("runE:3", make_cred("appr-g"), fp);   // allowed 为空
        broker->set_decision_for_tool("appr-g", "allow-always");
        auto o3 = reg.resolve("runE:3", fp);
        CHECK(o3.approved(), "空允许集 = 全集 [不可回退]");
    }

    rt.reset_for_testing();
    reg.reset_for_testing();
}

// ============================================================================
int main() {
    std::cout << "===== Defer 审批凭据 + 模式解析 测试 =====" << std::endl;

    test_fingerprint_stability();
    test_put_and_resolve();
    test_params_rewritten();
    test_replace_and_evict();
    test_discard_prefix();
    test_no_broker();
    test_parse_mode();
    test_concurrent_resolve_shares_wait();
    test_abort_interrupts_resolve();
    test_disallowed_decision();

    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
