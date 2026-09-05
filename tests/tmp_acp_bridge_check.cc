// 临时验证：ACP → AgentEvent 总线桥接 + relay acp 分支能收到
#include "hunterclaw/infra/agent_events.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

using namespace hunterclaw::infra;

static std::mutex g_mu;
static std::vector<AgentEvent> g_seen;
static int g_fail = 0;

static void check(bool ok, const char* name, const std::string& detail = "") {
    std::printf("%-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        if (!detail.empty()) std::printf("     %s\n", detail.c_str());
        ++g_fail;
    }
}

int main() {
    auto unsub = on_agent_event([](const AgentEvent& e) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_seen.push_back(e);
    });

    // T1: stream=="acp" 事件能被订阅者收到（relay 的前提）
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seen.clear();
        }
        AgentEvent ev;
        ev.run_id = "run-1";
        ev.stream = "acp";
        ev.data = nlohmann::json{{"phase", "prompt_submitted"}, {"at", 12345}};
        emit_agent_event(ev);

        std::lock_guard<std::mutex> lk(g_mu);
        bool ok = g_seen.size() == 1 && g_seen[0].stream == "acp"
                  && g_seen[0].phase() == "prompt_submitted";
        check(ok, "T1 stream=acp prompt_submitted delivered",
              "seen=" + std::to_string(g_seen.size()));
    }

    // T2: runtime_event 携带 eventType/text/tag，relay 能取出
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seen.clear();
        }
        AgentEvent ev;
        ev.run_id = "run-1";
        ev.stream = "acp";
        ev.data = nlohmann::json{
            {"phase", "runtime_event"},
            {"eventType", "status"},
            {"text", "Reading login.ts"},
        };
        emit_agent_event(ev);

        std::lock_guard<std::mutex> lk(g_mu);
        bool ok = g_seen.size() == 1
                  && g_seen[0].data.value("eventType", std::string{}) == "status"
                  && g_seen[0].data.value("text", std::string{}) == "Reading login.ts"
                  && g_seen[0].data.value("tag", std::string{}).empty();
        check(ok, "T2 runtime_event status fields extractable",
              g_seen.empty() ? "(none)" : g_seen[0].data.dump());
    }

    // T3: run_id 过滤 —— relay 靠它区分自己的子会话
    {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seen.clear();
        }
        for (const char* rid : {"run-A", "run-B", "run-A"}) {
            AgentEvent ev;
            ev.run_id = rid;
            ev.stream = "acp";
            ev.data = nlohmann::json{{"phase", "runtime_event"}, {"eventType", "text_delta"}};
            emit_agent_event(ev);
        }
        std::lock_guard<std::mutex> lk(g_mu);
        int count_a = 0;
        for (const auto& e : g_seen) {
            if (e.run_id == "run-A") ++count_a;
        }
        check(g_seen.size() == 3 && count_a == 2,
              "T3 run_id preserved for relay filtering",
              "total=" + std::to_string(g_seen.size()) + " runA=" + std::to_string(count_a));
    }

    // T4: 多订阅者都能收到（多个 relay 并存场景）
    {
        std::atomic<int> second_count{0};
        auto unsub2 = on_agent_event([&second_count](const AgentEvent&) { ++second_count; });
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seen.clear();
        }
        AgentEvent ev;
        ev.run_id = "run-X";
        ev.stream = "acp";
        ev.data = nlohmann::json{{"phase", "runtime_event"}};
        emit_agent_event(ev);
        unsub2();

        std::lock_guard<std::mutex> lk(g_mu);
        check(g_seen.size() == 1 && second_count.load() == 1,
              "T4 multiple relays each receive the event",
              "first=" + std::to_string(g_seen.size())
                  + " second=" + std::to_string(second_count.load()));
    }

    // T5: unsubscribe 后不再收到
    {
        unsub();
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_seen.clear();
        }
        AgentEvent ev;
        ev.run_id = "run-Z";
        ev.stream = "acp";
        ev.data = nlohmann::json::object();
        emit_agent_event(ev);

        std::lock_guard<std::mutex> lk(g_mu);
        check(g_seen.empty(), "T5 after unsubscribe -> no delivery",
              "seen=" + std::to_string(g_seen.size()));
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
