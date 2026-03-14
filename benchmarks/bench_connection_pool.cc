#include "hunterclaw/gateway/connection_pool.h"
#include <benchmark/benchmark.h>
#include <thread>
#include <vector>

using namespace hunterclaw::gateway;
using json = nlohmann::json;

// 基准测试: 连接注册
static void BM_ConnectionRegister(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    std::vector<uint64_t> conn_ids;
    
    for (auto _ : state) {
        uint64_t conn_id = pool.register_connection(
            "session-" + std::to_string(state.range(0)),
            "",
            [](const std::string&, const json&) {}
        );
        conn_ids.push_back(conn_id);
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
}
BENCHMARK(BM_ConnectionRegister)->Range(1, 1000);

// 基准测试: 广播到单个 session
static void BM_BroadcastToSession(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册连接
    std::vector<uint64_t> conn_ids;
    for (int i = 0; i < state.range(0); i++) {
        uint64_t conn_id = pool.register_connection(
            "session-test",
            "",
            [](const std::string&, const json&) {}
        );
        conn_ids.push_back(conn_id);
    }
    
    json payload = {
        {"type", "assistant_text"},
        {"text", "Hello, World!"}
    };
    
    for (auto _ : state) {
        pool.broadcast("agent", payload, 
            BroadcastOptions::to_session("session-test"));
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
    
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BroadcastToSession)->Range(1, 1000);

// 基准测试: 广播到所有连接
static void BM_BroadcastToAll(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册多个连接
    std::vector<uint64_t> conn_ids;
    for (int i = 0; i < state.range(0); i++) {
        uint64_t conn_id = pool.register_connection(
            "session-" + std::to_string(i),
            "",
            [](const std::string&, const json&) {}
        );
        conn_ids.push_back(conn_id);
    }
    
    json payload = {
        {"type", "assistant_text"},
        {"text", "Broadcast message"}
    };
    
    for (auto _ : state) {
        pool.broadcast("agent", payload, BroadcastOptions::to_all());
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
    
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_BroadcastToAll)->Range(10, 10000);

// 基准测试: 事件订阅匹配
static void BM_EventSubscription(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册订阅
    std::vector<uint64_t> sub_ids;
    for (int i = 0; i < state.range(0); i++) {
        uint64_t sub_id = pool.subscribe("agent.*", 
            [](const json&) {});
        sub_ids.push_back(sub_id);
    }
    
    // 注册一个连接用于触发事件
    uint64_t conn = pool.register_connection("session-test", "", 
        [](const std::string&, const json&) {});
    
    json payload = {{"type", "test"}};
    
    for (auto _ : state) {
        pool.broadcast("agent.text", payload, BroadcastOptions::to_all());
    }
    
    // 清理
    for (uint64_t sub_id : sub_ids) {
        pool.unsubscribe(sub_id);
    }
    pool.unregister_connection(conn);
    
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_EventSubscription)->Range(1, 100);

// 基准测试: 慢速客户端检测
static void BM_SlowClientDetection(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册快速和慢速连接
    std::vector<uint64_t> conn_ids;
    for (int i = 0; i < state.range(0); i++) {
        uint64_t conn_id = pool.register_connection(
            "session-" + std::to_string(i),
            "",
            [i](const std::string&, const json&) {
                if (i % 10 == 0) {
                    // 10% 的连接是慢速的
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        );
        conn_ids.push_back(conn_id);
    }
    
    BroadcastOptions opts;
    opts.drop_if_slow = true;
    opts.slow_threshold_ms = 50;
    
    json payload = {{"type", "test"}};
    
    for (auto _ : state) {
        pool.broadcast("test_event", payload, opts);
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
}
BENCHMARK(BM_SlowClientDetection)->Range(10, 1000);

// 基准测试: 并发广播
static void BM_ConcurrentBroadcast(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册连接
    std::vector<uint64_t> conn_ids;
    for (int i = 0; i < 100; i++) {
        uint64_t conn_id = pool.register_connection(
            "session-" + std::to_string(i),
            "",
            [](const std::string&, const json&) {}
        );
        conn_ids.push_back(conn_id);
    }
    
    json payload = {{"type", "test"}};
    
    for (auto _ : state) {
        // 多线程并发广播
        std::vector<std::thread> threads;
        for (int i = 0; i < state.range(0); i++) {
            threads.emplace_back([&pool, &payload, i]() {
                pool.broadcast("event-" + std::to_string(i), 
                    payload, BroadcastOptions::to_all());
            });
        }
        
        for (auto& t : threads) {
            t.join();
        }
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
}
BENCHMARK(BM_ConcurrentBroadcast)->Range(1, 10);

// 基准测试: 大 payload 广播
static void BM_LargePayloadBroadcast(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    // 注册连接
    std::vector<uint64_t> conn_ids;
    for (int i = 0; i < 100; i++) {
        uint64_t conn_id = pool.register_connection(
            "session-test",
            "",
            [](const std::string&, const json&) {}
        );
        conn_ids.push_back(conn_id);
    }
    
    // 构造大 payload (模拟 LLM 响应)
    json payload;
    payload["type"] = "assistant_text";
    std::string large_text(state.range(0), 'x');
    payload["text"] = large_text;
    
    for (auto _ : state) {
        pool.broadcast("agent", payload, 
            BroadcastOptions::to_session("session-test"));
    }
    
    // 清理
    for (uint64_t conn_id : conn_ids) {
        pool.unregister_connection(conn_id);
    }
    
    state.SetBytesProcessed(state.iterations() * state.range(0) * 100);
}
BENCHMARK(BM_LargePayloadBroadcast)->Range(100, 10000);

// 性能对比: 旧方式 (通过 Gateway 转发) vs 新方式 (直连)
static void BM_OldGatewayForwarding(benchmark::State& state) {
    // 模拟旧的 Gateway 转发方式
    for (auto _ : state) {
        // 1. Agent → Gateway
        json msg1 = {{"type", "response"}, {"text", "Hello"}};
        std::string serialized1 = msg1.dump();
        
        // 2. Gateway 解析
        json parsed = json::parse(serialized1);
        
        // 3. Gateway → Client
        json msg2 = {{"event", "agent"}, {"payload", parsed}};
        std::string serialized2 = msg2.dump();
        
        // 模拟网络延迟
        benchmark::DoNotOptimize(serialized2);
    }
}
BENCHMARK(BM_OldGatewayForwarding);

static void BM_NewDirectBroadcast(benchmark::State& state) {
    auto& pool = ConnectionPool::instance();
    
    uint64_t conn = pool.register_connection("session-test", "", 
        [](const std::string&, const json&) {});
    
    json payload = {{"type", "assistant_text"}, {"text", "Hello"}};
    
    for (auto _ : state) {
        // 直连推送
        pool.broadcast("agent", payload, 
            BroadcastOptions::to_session("session-test"));
    }
    
    pool.unregister_connection(conn);
}
BENCHMARK(BM_NewDirectBroadcast);

BENCHMARK_MAIN();
