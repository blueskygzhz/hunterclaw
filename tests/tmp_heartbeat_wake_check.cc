// 临时验证：heartbeat wake 合并/分组/并发语义
#include "hunterclaw/infra/heartbeat_wake.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

using namespace hunterclaw::infra;

static std::mutex g_mu;
static std::vector<HeartbeatWakeParams> g_calls;
static std::atomic<int> g_concurrent{0};
static std::atomic<int> g_max_concurrent{0};

static void reset() {
    testing::reset_wake_state_for_test();
    std::lock_guard<std::mutex> lk(g_mu);
    g_calls.clear();
    g_concurrent.store(0);
    g_max_concurrent.store(0);
}

static int call_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<int>(g_calls.size());
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name, const char* detail) {
        std::printf("%-58s %s%s%s\n", name, ok ? "PASS" : "FAIL",
                    ok ? "" : "  <- ", ok ? "" : detail);
        if (!ok) ++failures;
    };

    auto disposer = set_heartbeat_wake_handler(
        [](const HeartbeatWakeParams& p) -> HeartbeatRunResult {
            int cur = ++g_concurrent;
            int prev = g_max_concurrent.load();
            while (cur > prev && !g_max_concurrent.compare_exchange_weak(prev, cur)) {}
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_calls.push_back(p);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            --g_concurrent;
            return {HeartbeatRunResult::Status::Ran, 0, std::nullopt};
        });

    // ── T1: 合并窗口内多次请求 → handler 只跑一次 ─────────────────────────
    reset();
    for (int i = 0; i < 20; ++i) {
        request_heartbeat_now("acp:spawn:stream", std::nullopt, "sess-A");
    }
    testing::drain_wakes_for_test(3000);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "expected 1, got %d", call_count());
        check(call_count() == 1, "T1 coalesce: 20 requests -> 1 handler call", buf);
    }

    // ── T2: 不同 session 分属不同 target，各自派发 ────────────────────────
    reset();
    request_heartbeat_now("r", std::nullopt, "sess-A");
    request_heartbeat_now("r", std::nullopt, "sess-B");
    request_heartbeat_now("r", std::nullopt, "sess-C");
    testing::drain_wakes_for_test(3000);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "expected 3, got %d", call_count());
        check(call_count() == 3, "T2 grouping: 3 sessions -> 3 handler calls", buf);
    }

    // ── T3: 同 session 不同 intent 占不同槽 → 3 次 ───────────────────────
    reset();
    request_heartbeat_now("r", std::nullopt, "sess-A", HeartbeatWakeIntent::Task);
    request_heartbeat_now("r", std::nullopt, "sess-A", HeartbeatWakeIntent::Scheduled);
    request_heartbeat_now("r", std::nullopt, "sess-A", HeartbeatWakeIntent::Event);
    testing::drain_wakes_for_test(3000);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "expected 3, got %d", call_count());
        check(call_count() == 3, "T3 slots: task/scheduled/event -> 3 calls", buf);
    }

    // ── T4: 并发上限 4 ───────────────────────────────────────────────────
    reset();
    for (int i = 0; i < 12; ++i) {
        request_heartbeat_now("r", std::nullopt, "sess-" + std::to_string(i));
    }
    testing::drain_wakes_for_test(6000);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "max concurrent=%d (limit 4)",
                      g_max_concurrent.load());
        check(g_max_concurrent.load() <= 4, "T4 concurrency cap <= 4", buf);
        std::snprintf(buf, sizeof(buf), "expected 12, got %d", call_count());
        check(call_count() == 12, "T4 all 12 targets eventually dispatched", buf);
    }

    // ── T5: target key 规则 ──────────────────────────────────────────────
    {
        bool ok =
            resolve_heartbeat_wake_target_key(std::nullopt, std::nullopt) == "::" &&
            resolve_heartbeat_wake_target_key("agentX", std::nullopt) == "agentX::" &&
            resolve_heartbeat_wake_target_key(std::nullopt, "sessY") == "::sessY" &&
            // session 存在时 agentId 不得拆分 target
            resolve_heartbeat_wake_target_key("agentX", "sessY") == "::sessY";
        check(ok, "T5 target key rule matches openclaw", "key mismatch");
    }

    // ── T6: 优先级合并——同槽保留高优先级 reason ─────────────────────────
    reset();
    request_heartbeat_now("interval", std::nullopt, "sess-P",
                          HeartbeatWakeIntent::Event, HeartbeatWakeSource::Interval);
    request_heartbeat_now("manual-action", std::nullopt, "sess-P",
                          HeartbeatWakeIntent::Manual, HeartbeatWakeSource::Manual);
    testing::drain_wakes_for_test(3000);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        bool ok = g_calls.size() == 1 && g_calls[0].reason.has_value() &&
                  *g_calls[0].reason == "manual-action";
        char buf[160];
        std::snprintf(buf, sizeof(buf), "calls=%zu reason=%s", g_calls.size(),
                      g_calls.empty() || !g_calls[0].reason ? "(none)"
                                                            : g_calls[0].reason->c_str());
        check(ok, "T6 merge keeps higher-priority (manual > interval)", buf);
    }

    // ── T7: 全局 wake 不与定向 wake 并行 ─────────────────────────────────
    reset();
    request_heartbeat_now("targeted", std::nullopt, "sess-T");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // 让定向 turn 跑起来
    request_heartbeat_now("global", std::nullopt, std::nullopt);
    testing::drain_wakes_for_test(4000);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "max concurrent=%d, calls=%d",
                      g_max_concurrent.load(), call_count());
        check(call_count() == 2 && g_max_concurrent.load() == 1,
              "T7 global wake never runs beside targeted turn", buf);
    }

    disposer();
    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
