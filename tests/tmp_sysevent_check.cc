// 临时验证：consume_selected_system_event_entries 语义
#include "hunterclaw/infra/system_events.h"
#include <cstdio>
#include <thread>

using namespace hunterclaw::infra;

static const char* K = "agent:main:main";

int main() {
    int fail = 0;
    auto check = [&](bool ok, const char* name, const char* detail) {
        std::printf("%-58s %s%s\n", name, ok ? "PASS" : "FAIL",
                    ok ? "" : (std::string("  <- ") + detail).c_str());
        if (!ok) ++fail;
    };

    // T1: 精确消费——只移除指定条目，新入队的保留
    {
        drain_system_event_entries(K);
        enqueue_system_event("evt-A", K, std::string("acp-spawn:r1:stream"));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        enqueue_system_event("evt-B", K, std::string("acp-spawn:r1:stream"));
        auto peeked = peek_system_event_entries(K);

        // 模拟 turn 期间又来一条
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        enqueue_system_event("evt-C", K, std::string("acp-spawn:r1:stream"));

        auto removed = consume_selected_system_event_entries(K, peeked);
        auto left = peek_system_event_entries(K);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "removed=%zu left=%zu leftText=%s",
                      removed.size(), left.size(),
                      left.empty() ? "(none)" : left[0].text.c_str());
        check(removed.size() == 2 && left.size() == 1 && left[0].text == "evt-C",
              "T1 consume removes only inspected, keeps newcomer", buf);
    }

    // T2: 队列排空后 entry 被清理，同文本可再次入队（去重状态已复位）
    {
        drain_system_event_entries(K);
        enqueue_system_event("same-text", K, std::nullopt);
        auto peeked = peek_system_event_entries(K);
        consume_selected_system_event_entries(K, peeked);

        bool re_added = enqueue_system_event("same-text", K, std::nullopt);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "re_added=%s", re_added ? "true" : "false");
        check(re_added, "T2 queue drained -> same text can re-enqueue", buf);
    }

    // T3: 消费不存在的条目 -> 安全跳过
    {
        drain_system_event_entries(K);
        enqueue_system_event("real", K, std::nullopt);
        SystemEvent fake;
        fake.text = "never-enqueued";
        fake.ts = 12345;
        auto removed = consume_selected_system_event_entries(K, {fake});
        auto left = peek_system_event_entries(K);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "removed=%zu left=%zu", removed.size(), left.size());
        check(removed.empty() && left.size() == 1,
              "T3 unknown entry -> skipped, queue intact", buf);
    }

    // T4: ts 不同则不匹配（防止同文本误删）
    {
        drain_system_event_entries(K);
        enqueue_system_event("dup-text", K, std::nullopt);
        auto peeked = peek_system_event_entries(K);
        SystemEvent wrong_ts = peeked[0];
        wrong_ts.ts += 999;
        auto removed = consume_selected_system_event_entries(K, {wrong_ts});
        char buf[128];
        std::snprintf(buf, sizeof(buf), "removed=%zu (expect 0)", removed.size());
        check(removed.empty(), "T4 ts mismatch -> not removed", buf);
    }

    // T5: context_key 不同则不匹配
    {
        drain_system_event_entries(K);
        enqueue_system_event("ctx-text", K, std::string("ctx-1"));
        auto peeked = peek_system_event_entries(K);
        SystemEvent wrong_ctx = peeked[0];
        wrong_ctx.context_key = std::string("ctx-2");
        auto removed = consume_selected_system_event_entries(K, {wrong_ctx});
        char buf[128];
        std::snprintf(buf, sizeof(buf), "removed=%zu (expect 0)", removed.size());
        check(removed.empty(), "T5 context_key mismatch -> not removed", buf);
    }

    drain_system_event_entries(K);
    std::printf("\n%s (%d failure%s)\n", fail == 0 ? "ALL PASS" : "FAILED", fail,
                fail == 1 ? "" : "s");
    return fail == 0 ? 0 : 1;
}
