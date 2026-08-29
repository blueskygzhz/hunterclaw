// ============================================================================
// ExecApprovalManager 不变量测试
//
// 覆盖此前【零测试】的两条安全不变量:
//   1. gateway 侧 grace —— 等待方比记录多等 10s, 避免同时到期的竞态
//   2. 决策 id 绑定    —— 走错路由的决议不得解锁别的审批
//
// 以及既有但未被锁死的行为: 超时钳制、abort 打断、决策唤醒。
// ============================================================================

#include "hunterclaw/gateway/exec_approval_manager.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace hunterclaw::gateway;

static int g_pass = 0;
static int g_fail = 0;

static void CHECK(bool cond, const std::string& what) {
    if (cond) {
        ++g_pass;
        std::cout << "  PASS: " << what << std::endl;
    } else {
        ++g_fail;
        std::cout << "  FAIL: " << what << std::endl;
    }
}

static void SECTION(const std::string& name) {
    std::cout << "\n[" << name << "]" << std::endl;
}

static PluginApprovalRequestPayload make_payload(const std::string& tool) {
    PluginApprovalRequestPayload p;
    p.title = "工具需要审批: " + tool;
    p.description = "Agent 请求执行工具 '" + tool + "'";
    p.tool_name = tool;
    p.tool_call_id = "call-1";
    p.severity = std::string("warning");
    return p;
}

// ============================================================================
// 超时钳制
//
// 🎯 对齐 OpenClaw resolvePluginToolApprovalTimeoutMs (approval.ts:41-50):
//      非正数 → DEFAULT(120_000); 超上限 → MAX(600_000)
static void test_timeout_clamp() {
    SECTION("超时钳制");

    ExecApprovalManager mgr;

    // <=0 → 默认 120s
    // ⚠ 不钳会让调用方传 0 导致【立即过期】, 表现为"审批刚发出就超时",
    //   而日志里完全看不出是参数问题。
    {
        auto r = mgr.create_plugin(make_payload("t1"), 0);
        CHECK(r.expires_at_ms - r.created_at_ms == DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS,
              "timeout=0 → 默认 120s [不可回退]");
    }
    {
        auto r = mgr.create_plugin(make_payload("t2"), -5);
        CHECK(r.expires_at_ms - r.created_at_ms == DEFAULT_PLUGIN_APPROVAL_TIMEOUT_MS,
              "timeout<0 → 默认 120s [不可回退]");
    }

    // 超上限 → 钳到 600s
    // ⚠ 不钳上限会让一个笔误(多打个 0)把审批挂上几小时,
    //   期间占着 main lane 的唯一 worker。
    {
        auto r = mgr.create_plugin(make_payload("t3"), 99999999);
        CHECK(r.expires_at_ms - r.created_at_ms == MAX_PLUGIN_APPROVAL_TIMEOUT_MS,
              "超上限 → 钳到 600s [不可回退]");
    }

    // 正常值原样保留
    {
        auto r = mgr.create_plugin(make_payload("t4"), 5000);
        CHECK(r.expires_at_ms - r.created_at_ms == 5000, "正常值原样保留");
    }
}

// ============================================================================
// gateway 侧 grace
//
// 🎯 对齐 OpenClaw resolvePluginToolApprovalGatewayTimeoutMs (approval.ts:52-54):
//      addTimerTimeoutGraceMs(timeoutMs, 10_000)
//
// ⚠⚠ 等待方与审批记录【不能同时到期】。同时到期形成竞态边界:
//   用户最后一刻点击 → 等待方已返回 nullopt(报超时), 决策却写进了记录
//   → 用户看到"已批准", agent 看到"超时", 两边各自都认为自己对。
static void test_gateway_grace() {
    SECTION("gateway 侧 grace");

    ExecApprovalManager mgr;
    auto rec = mgr.create_plugin(make_payload("grace_tool"), 300);
    CHECK(mgr.register_record(rec, 300), "注册成功");

    // 在记录名义超时(300ms)【之后】、grace 窗口内送决策
    std::thread late([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        mgr.resolve(rec.id, "allow-once", std::string("u1"));
    });

    const auto t0 = std::chrono::steady_clock::now();
    auto d = mgr.await_decision(rec.id, 300);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    late.join();

    // ★★ [不可回退] 300ms 的名义超时下, 600ms 才到的决策仍能被收到 ——
    //   因为等待方实际等的是 300 + 10000。
    //   ⚠ 若无 grace, 这里会在 ~300ms 返回 nullopt, 用户的点击石沉大海。
    CHECK(d.has_value(), "grace 窗口内的迟到决策仍被收到 [不可回退]");
    if (d.has_value()) {
        CHECK(*d == "allow-once", "决策值正确");
    }
    CHECK(ms >= 550, "确实等过了名义超时（实测 " + std::to_string(ms) + "ms）");
    // ⚠ 但也不能无限等 —— 上限是 名义值 + grace
    CHECK(ms < 300 + APPROVAL_GATEWAY_GRACE_MS + 500,
          "未超出 名义值+grace 的上限 [不可回退]");
}

// ============================================================================
// 决策 id 绑定
//
// 🎯 对齐 OpenClaw approval.ts:376 注释原文:
//      "Bind the verdict to the request that parked this call.
//       A stale or misrouted reply must never release a different tool gate."
static void test_decision_id_binding() {
    SECTION("决策 id 绑定");

    ExecApprovalManager mgr;
    auto a = mgr.create_plugin(make_payload("tool_a"), 3000);
    auto b = mgr.create_plugin(make_payload("tool_b"), 3000);
    mgr.register_record(a, 3000);
    mgr.register_record(b, 3000);

    CHECK(a.id != b.id, "两条审批 id 不同");

    // 只决策 b
    mgr.resolve(b.id, "allow-once", std::string("u1"));

    // ★★ [不可回退] a 【不能】因为 b 被批准而放行。
    //   放行属于别的审批的批准 = 让 A 的授权解锁 B 的操作。
    auto da = mgr.await_decision(a.id, 200);
    CHECK(!da.has_value(), "A 不被 B 的决策解锁 [不可回退]");

    // b 自己正常拿到决策
    auto db = mgr.await_decision(b.id, 200);
    CHECK(db.has_value() && *db == "allow-once", "B 拿到自己的决策");

    // 不存在的 id → nullopt（而非阻塞或崩溃）
    auto dz = mgr.await_decision("no-such-id", 200);
    CHECK(!dz.has_value(), "未知 id → nullopt");
}

// ============================================================================
// abort 打断等待
//
// ⚠⚠ 没有这条: 用户点 chat.abort 后等待仍挂满 timeout, 而该等待占着
//   main lane 唯一的 WorkerThread → 全 gateway 推理停摆, 用户无法自救。
static void test_abort_interrupts() {
    SECTION("abort 打断等待");

    ExecApprovalManager mgr;
    auto rec = mgr.create_plugin(make_payload("abort_tool"), 10000);
    mgr.register_record(rec, 10000);

    std::atomic<bool> aborted{false};
    std::thread setter([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        aborted.store(true);
    });

    const auto t0 = std::chrono::steady_clock::now();
    auto d = mgr.await_decision(rec.id, 10000, &aborted);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    setter.join();

    // ★★ [不可回退] 必须【远早于】10s 返回
    CHECK(ms < 2000, "abort 后迅速返回（实测 " + std::to_string(ms) + "ms）[不可回退]");
    CHECK(!d.has_value(), "abort 不放行 [不可回退]");
}

// ============================================================================
// 正常决策唤醒
static void test_normal_wakeup() {
    SECTION("正常决策唤醒");

    ExecApprovalManager mgr;
    auto rec = mgr.create_plugin(make_payload("ok_tool"), 5000);
    mgr.register_record(rec, 5000);

    std::thread resolver([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        mgr.resolve(rec.id, "deny", std::string("u1"));
    });

    const auto t0 = std::chrono::steady_clock::now();
    auto d = mgr.await_decision(rec.id, 5000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    resolver.join();

    CHECK(d.has_value() && *d == "deny", "拿到 deny 决策 [不可回退]");
    // ★ 决策一到就返回, 不等满超时
    CHECK(ms < 1000, "决策到达即返回（实测 " + std::to_string(ms) + "ms）");
}

// ============================================================================
int main() {
    std::cout << "===== ExecApprovalManager 测试 =====" << std::endl;

    test_timeout_clamp();
    test_gateway_grace();
    test_decision_id_binding();
    test_abort_interrupts();
    test_normal_wakeup();

    std::cout << "\n-----------------------------------" << std::endl;
    std::cout << "PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}
