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
    std::string request_tool_approval(const std::string& tool_name,
                                     const std::string&, const std::string&,
                                     const std::string&, int64_t,
                                     const std::vector<std::string>&) override {
        std::lock_guard<std::mutex> lk(mu_);
        last_tool_ = tool_name;
        return "appr-" + std::to_string(++seq_);
    }

    std::optional<std::string> wait_tool_approval(const std::string& id,
                                                 int64_t timeout_ms) override {
        std::unique_lock<std::mutex> lk(mu_);
        ++wait_calls_;
        const int64_t w = timeout_ms > 0 ? timeout_ms : 3000;
        const bool got = cv_.wait_for(lk, std::chrono::milliseconds(w),
                                     [&]() { return decisions_.count(id) > 0; });
        if (!got) return std::nullopt;
        return decisions_[id];
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

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::string> decisions_;
    std::string last_tool_;
    int seq_ = 0;
    int wait_calls_ = 0;
};

DeferredApproval make_cred(const std::string& id) {
    DeferredApproval d;
    d.approval_id = id;
    d.tool_name = "t";
    d.tool_call_id = "c";
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

        broker->set_decision("appr-1", "allow-once");
        auto out = reg.resolve("run1:call1", fp);
        CHECK(out.approved(), "获批");
        CHECK(out.decision == "allow-once", "决策带出");
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
        broker->set_decision("appr-2", "deny");
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
        broker->set_decision("appr-4", "approve");   // 非法值
        auto out = reg.resolve("run1:call4", fp);
        // ★ "approve" 恰好是 plugin_approval.cc 曾经错误接受的值。
        //   若这里当成放行，一个老客户端就能绕过审批语义。
        CHECK(!out.approved(), "未知决策不放行 [不可回退]");
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
    broker->set_decision("appr-9", "allow-once");   // 用户【已批准】

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

    // 传空指纹 = 跳过比对
    {
        reg.put("run2:call2", make_cred("appr-10"), fp_old);
        broker->set_decision("appr-10", "allow-once");
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
    CHECK(out.resolution == DeferredResolution::TimedOut, "归为 TimedOut");
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
int main() {
    std::cout << "===== Defer 审批凭据 + 模式解析 测试 =====" << std::endl;

    test_fingerprint_stability();
    test_put_and_resolve();
    test_params_rewritten();
    test_replace_and_evict();
    test_discard_prefix();
    test_no_broker();
    test_parse_mode();

    std::cout << "-----------------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
