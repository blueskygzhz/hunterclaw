#include "hunterclaw/gateway/connection_pool.h"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>

using namespace hunterclaw::gateway;
using json = nlohmann::json;

class ConnectionPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 清理可能的残留连接
        auto& pool = ConnectionPool::instance();
        auto stats = pool.get_stats();
        // Note: We can't actually clear the singleton, but tests should be isolated
    }
};

// 测试连接注册和注销
TEST_F(ConnectionPoolTest, RegisterUnregister) {
    auto& pool = ConnectionPool::instance();
    
    bool callback_invoked = false;
    std::string received_event;
    json received_payload;
    
    uint64_t conn_id = pool.register_connection(
        "session-1",
        "channel-1",
        [&](const std::string& event, const json& payload) {
            callback_invoked = true;
            received_event = event;
            received_payload = payload;
        }
    );
    
    EXPECT_GT(conn_id, 0);
    
    // 验证连接已注册
    auto stats = pool.get_stats();
    EXPECT_GE(stats.total_connections, 1);
    
    // 测试广播
    json test_payload = {
        {"type", "test"},
        {"message", "Hello"}
    };
    
    pool.broadcast("test_event", test_payload, 
        BroadcastOptions::to_session("session-1"));
    
    EXPECT_TRUE(callback_invoked);
    EXPECT_EQ(received_event, "test_event");
    EXPECT_EQ(received_payload["message"], "Hello");
    
    // 注销连接
    pool.unregister_connection(conn_id);
    
    stats = pool.get_stats();
    // Cannot assert exact count due to singleton nature
}

// 测试广播到所有连接
TEST_F(ConnectionPoolTest, BroadcastToAll) {
    auto& pool = ConnectionPool::instance();
    
    int callback_count = 0;
    
    auto callback = [&](const std::string& event, const json& payload) {
        callback_count++;
    };
    
    uint64_t conn1 = pool.register_connection("session-1", "", callback);
    uint64_t conn2 = pool.register_connection("session-2", "", callback);
    uint64_t conn3 = pool.register_connection("session-3", "", callback);
    
    json payload = {{"test", "broadcast"}};
    pool.broadcast("global_event", payload, BroadcastOptions::to_all());
    
    EXPECT_EQ(callback_count, 3);
    
    pool.unregister_connection(conn1);
    pool.unregister_connection(conn2);
    pool.unregister_connection(conn3);
}

// 测试按 session 广播
TEST_F(ConnectionPoolTest, BroadcastToSession) {
    auto& pool = ConnectionPool::instance();
    
    int session1_count = 0;
    int session2_count = 0;
    
    uint64_t conn1 = pool.register_connection("session-1", "", 
        [&](const std::string&, const json&) { session1_count++; });
    uint64_t conn2 = pool.register_connection("session-1", "", 
        [&](const std::string&, const json&) { session1_count++; });
    uint64_t conn3 = pool.register_connection("session-2", "", 
        [&](const std::string&, const json&) { session2_count++; });
    
    json payload = {{"test", "session"}};
    pool.broadcast("session_event", payload, 
        BroadcastOptions::to_session("session-1"));
    
    EXPECT_EQ(session1_count, 2);
    EXPECT_EQ(session2_count, 0);
    
    pool.unregister_connection(conn1);
    pool.unregister_connection(conn2);
    pool.unregister_connection(conn3);
}

// 测试按 channel 广播
TEST_F(ConnectionPoolTest, BroadcastToChannel) {
    auto& pool = ConnectionPool::instance();
    
    int discord_count = 0;
    int telegram_count = 0;
    
    uint64_t conn1 = pool.register_connection("session-1", "discord", 
        [&](const std::string&, const json&) { discord_count++; });
    uint64_t conn2 = pool.register_connection("session-2", "discord", 
        [&](const std::string&, const json&) { discord_count++; });
    uint64_t conn3 = pool.register_connection("session-3", "telegram", 
        [&](const std::string&, const json&) { telegram_count++; });
    
    json payload = {{"test", "channel"}};
    pool.broadcast("channel_event", payload, 
        BroadcastOptions::to_channel("discord"));
    
    EXPECT_EQ(discord_count, 2);
    EXPECT_EQ(telegram_count, 0);
    
    pool.unregister_connection(conn1);
    pool.unregister_connection(conn2);
    pool.unregister_connection(conn3);
}

// 测试事件订阅
TEST_F(ConnectionPoolTest, EventSubscription) {
    auto& pool = ConnectionPool::instance();
    
    int agent_event_count = 0;
    int memory_event_count = 0;
    
    uint64_t sub1 = pool.subscribe("agent.*", 
        [&](const json& payload) {
            agent_event_count++;
        });
    
    uint64_t sub2 = pool.subscribe("memory.*", 
        [&](const json& payload) {
            memory_event_count++;
        });
    
    // 注册一个连接用于触发事件
    uint64_t conn = pool.register_connection("session-1", "", 
        [](const std::string&, const json&) {});
    
    json payload = {{"type", "test"}};
    
    pool.broadcast("agent.text", payload, BroadcastOptions::to_all());
    pool.broadcast("agent.tool_call", payload, BroadcastOptions::to_all());
    pool.broadcast("memory.recall", payload, BroadcastOptions::to_all());
    
    // 订阅者应该收到匹配的事件
    EXPECT_EQ(agent_event_count, 2);
    EXPECT_EQ(memory_event_count, 1);
    
    pool.unsubscribe(sub1);
    pool.unsubscribe(sub2);
    pool.unregister_connection(conn);
}

// 测试慢速客户端保护
TEST_F(ConnectionPoolTest, SlowClientProtection) {
    auto& pool = ConnectionPool::instance();
    
    int fast_count = 0;
    int slow_count = 0;
    
    uint64_t fast_conn = pool.register_connection("session-1", "", 
        [&](const std::string&, const json&) { fast_count++; });
    
    uint64_t slow_conn = pool.register_connection("session-2", "", 
        [&](const std::string&, const json&) {
            slow_count++;
            // 模拟慢速客户端
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
    
    BroadcastOptions opts;
    opts.drop_if_slow = true;
    opts.slow_threshold_ms = 50;
    
    json payload = {{"test", "slow"}};
    
    // 第一次发送
    pool.broadcast("test_event", payload, BroadcastOptions::to_all());
    
    // 等待慢速客户端完成
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    // 第二次发送 (慢速客户端可能会被跳过)
    pool.broadcast("test_event", payload, opts);
    
    // 快速客户端应该收到所有消息
    EXPECT_GE(fast_count, 2);
    
    pool.unregister_connection(fast_conn);
    pool.unregister_connection(slow_conn);
}

// 测试通配符匹配
TEST_F(ConnectionPoolTest, WildcardMatching) {
    auto& pool = ConnectionPool::instance();
    
    int wildcard_count = 0;
    
    uint64_t sub = pool.subscribe("*", 
        [&](const json& payload) {
            wildcard_count++;
        });
    
    uint64_t conn = pool.register_connection("session-1", "", 
        [](const std::string&, const json&) {});
    
    json payload = {{"test", "wildcard"}};
    
    pool.broadcast("event1", payload, BroadcastOptions::to_all());
    pool.broadcast("event2", payload, BroadcastOptions::to_all());
    pool.broadcast("event3", payload, BroadcastOptions::to_all());
    
    // 通配符订阅应该收到所有事件
    EXPECT_EQ(wildcard_count, 3);
    
    pool.unsubscribe(sub);
    pool.unregister_connection(conn);
}

// 测试统计信息
TEST_F(ConnectionPoolTest, Statistics) {
    auto& pool = ConnectionPool::instance();
    
    auto stats_before = pool.get_stats();
    
    uint64_t conn1 = pool.register_connection("session-1", "discord", 
        [](const std::string&, const json&) {});
    uint64_t conn2 = pool.register_connection("session-2", "telegram", 
        [](const std::string&, const json&) {});
    
    uint64_t sub1 = pool.subscribe("agent.*", [](const json&) {});
    
    auto stats_after = pool.get_stats();
    
    // 验证统计增加
    EXPECT_GT(stats_after.total_connections, stats_before.total_connections);
    EXPECT_GT(stats_after.total_subscriptions, stats_before.total_subscriptions);
    
    pool.unsubscribe(sub1);
    pool.unregister_connection(conn1);
    pool.unregister_connection(conn2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
