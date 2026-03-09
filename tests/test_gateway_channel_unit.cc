/**
 * HunterClaw Gateway & Channel 集成单元测试
 * 
 * 测试目标：
 * 1. Gateway Server 基础功能
 * 2. Channel Manager 基础功能
 * 3. Gateway <-> Channel 集成
 * 4. 消息流向（入站/出站）
 * 5. RPC 方法调用
 * 6. 会话管理
 */

#include <gtest/gtest.h>
#include "hunterclaw/gateway/gateway.h"
#include "hunterclaw/gateway/server.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/core/types.h"
#include <thread>
#include <chrono>
#include <atomic>

using namespace hunterclaw;
using namespace std::chrono_literals;

// ============================================================================
// 测试用 Mock Channel Adapter
// ============================================================================

class MockChannelAdapter : public channels::ChannelAdapter {
public:
    MockChannelAdapter() : connected_(false), started_(false) {}
    
    channels::ChannelMeta meta() const override {
        channels::ChannelMeta m;
        m.id = "mock";
        m.label = "Mock Channel";
        m.blurb = "Test channel for unit testing";
        return m;
    }
    
    core::Result<void> start(const std::string& account_id) override {
        started_ = true;
        connected_ = true;
        account_id_ = account_id;
        return core::Result<void>::ok();
    }
    
    core::Result<void> stop() override {
        started_ = false;
        connected_ = false;
        return core::Result<void>::ok();
    }
    
    bool is_connected() const { return connected_; }
    bool is_started() const { return started_; }
    std::string get_account_id() const { return account_id_; }
    
    channels::HealthStatus get_health() override {
        channels::HealthStatus health;
        health.status = connected_ ? channels::HealthStatus::Healthy : 
                                    channels::HealthStatus::Unhealthy;
        return health;
    }
    
    // 模拟接收消息
    void simulate_receive_message(const channels::InboundMessage& msg) {
        if (on_message_received) {
            on_message_received(msg);
        }
    }
    
    std::function<void(const channels::InboundMessage&)> on_message_received;
    
private:
    bool connected_;
    bool started_;
    std::string account_id_;
};

// ============================================================================
// 测试套件 1: Gateway Server 基础功能
// ============================================================================

class GatewayServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        options_.port = 18790;  // 使用测试端口避免冲突
        options_.bind = gateway::GatewayBindMode::Loopback;
        options_.auth.mode = gateway::GatewayAuthMode::None;  // 测试环境无需认证
    }
    
    void TearDown() override {
        if (server_ && server_->is_running()) {
            server_->stop();
        }
    }
    
    gateway::GatewayServerOptions options_;
    std::shared_ptr<gateway::GatewayServer> server_;
};

TEST_F(GatewayServerTest, CreateServer) {
    auto result = gateway::create_gateway_server(options_);
    ASSERT_TRUE(result.is_ok()) << "Failed to create gateway server: " << result.error;
    server_ = result.value;
    ASSERT_NE(server_, nullptr);
}

TEST_F(GatewayServerTest, StartAndStop) {
    auto result = gateway::create_gateway_server(options_);
    ASSERT_TRUE(result.is_ok());
    server_ = result.value;
    
    // 启动
    auto start_result = server_->start();
    ASSERT_TRUE(start_result.is_ok()) << "Failed to start: " << start_result.error;
    EXPECT_TRUE(server_->is_running());
    
    // 等待服务器完全启动
    std::this_thread::sleep_for(100ms);
    
    // 停止
    auto stop_result = server_->stop();
    ASSERT_TRUE(stop_result.is_ok()) << "Failed to stop: " << stop_result.error;
    EXPECT_FALSE(server_->is_running());
}

TEST_F(GatewayServerTest, RegisterRpcHandler) {
    auto result = gateway::create_gateway_server(options_);
    ASSERT_TRUE(result.is_ok());
    server_ = result.value;
    
    bool handler_called = false;
    
    server_->register_handler("test.ping", 
        [&handler_called](const std::string& conn_id, const nlohmann::json& params) 
        -> core::Result<nlohmann::json> {
            handler_called = true;
            return core::Result<nlohmann::json>::ok(nlohmann::json{{"pong", true}});
        });
    
    auto start_result = server_->start();
    ASSERT_TRUE(start_result.is_ok());
    
    // TODO: 实际调用 RPC 方法并验证
    // 这里只验证注册成功
    EXPECT_TRUE(server_->is_running());
    
    server_->stop();
}

TEST_F(GatewayServerTest, BroadcastEvent) {
    auto result = gateway::create_gateway_server(options_);
    ASSERT_TRUE(result.is_ok());
    server_ = result.value;
    
    auto start_result = server_->start();
    ASSERT_TRUE(start_result.is_ok());
    
    nlohmann::json payload = {
        {"message", "test broadcast"},
        {"timestamp", 123456789}
    };
    
    // 广播应该不会报错（即使没有客户端连接）
    EXPECT_NO_THROW(server_->broadcast("test.event", payload));
    
    server_->stop();
}

TEST_F(GatewayServerTest, GetStatistics) {
    auto result = gateway::create_gateway_server(options_);
    ASSERT_TRUE(result.is_ok());
    server_ = result.value;
    
    auto start_result = server_->start();
    ASSERT_TRUE(start_result.is_ok());
    
    auto stats = server_->get_statistics();
    EXPECT_EQ(stats.active_connections, 0);
    EXPECT_GE(stats.uptime_seconds, 0);
    
    server_->stop();
}

// ============================================================================
// 测试套件 2: Channel Manager 基础功能
// ============================================================================

class ChannelManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = channels::ChannelManager::create();
        ASSERT_NE(manager_, nullptr);
    }
    
    std::shared_ptr<channels::ChannelManager> manager_;
};

TEST_F(ChannelManagerTest, RegisterChannel) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    auto result = manager_->register_channel("mock", adapter);
    ASSERT_TRUE(result.is_ok()) << "Failed to register: " << result.error;
    
    auto channels = manager_->list_channels();
    EXPECT_EQ(channels.size(), 1);
    EXPECT_EQ(channels[0], "mock");
}

TEST_F(ChannelManagerTest, DuplicateRegistration) {
    auto adapter1 = std::make_shared<MockChannelAdapter>();
    auto adapter2 = std::make_shared<MockChannelAdapter>();
    
    auto result1 = manager_->register_channel("mock", adapter1);
    ASSERT_TRUE(result1.is_ok());
    
    auto result2 = manager_->register_channel("mock", adapter2);
    EXPECT_TRUE(result2.is_err());
    EXPECT_NE(result2.error.find("already registered"), std::string::npos);
}

TEST_F(ChannelManagerTest, UnregisterChannel) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    manager_->register_channel("mock", adapter);
    
    auto result = manager_->unregister_channel("mock");
    ASSERT_TRUE(result.is_ok());
    
    auto channels = manager_->list_channels();
    EXPECT_EQ(channels.size(), 0);
}

TEST_F(ChannelManagerTest, StartAndStopChannel) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    manager_->register_channel("mock", adapter);
    
    auto start_result = manager_->start_channel("mock", "test_account");
    ASSERT_TRUE(start_result.is_ok()) << "Failed to start: " << start_result.error;
    EXPECT_TRUE(adapter->is_started());
    EXPECT_TRUE(adapter->is_connected());
    
    auto stop_result = manager_->stop_channel("mock", "test_account");
    ASSERT_TRUE(stop_result.is_ok());
    EXPECT_FALSE(adapter->is_started());
}

TEST_F(ChannelManagerTest, GetChannel) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    manager_->register_channel("mock", adapter);
    
    auto retrieved = manager_->get_channel("mock");
    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved, adapter);
    
    auto not_found = manager_->get_channel("nonexistent");
    EXPECT_EQ(not_found, nullptr);
}

TEST_F(ChannelManagerTest, MessageCallback) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    manager_->register_channel("mock", adapter);
    
    std::atomic<int> message_count{0};
    channels::InboundMessage received_msg;
    
    manager_->set_message_callback(
        [&message_count, &received_msg](const channels::InboundMessage& msg) {
            message_count++;
            received_msg = msg;
        });
    
    // 模拟接收消息
    channels::InboundMessage test_msg;
    test_msg.message_id = "msg_123";
    test_msg.text = "Hello World";
    test_msg.from_id = "user_456";
    
    adapter->simulate_receive_message(test_msg);
    
    // 等待回调执行
    std::this_thread::sleep_for(10ms);
    
    EXPECT_EQ(message_count.load(), 1);
    EXPECT_EQ(received_msg.message_id, "msg_123");
    EXPECT_EQ(received_msg.text, "Hello World");
}

// ============================================================================
// 测试套件 3: Gateway & Channel 集成测试
// ============================================================================

class GatewayChannelIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建 Gateway
        gateway::GatewayServerOptions options;
        options.port = 18791;
        options.bind = gateway::GatewayBindMode::Loopback;
        options.auth.mode = gateway::GatewayAuthMode::None;
        
        auto result = gateway::create_gateway_server(options);
        ASSERT_TRUE(result.is_ok());
        gateway_ = result.value;
        
        // 创建 Channel Manager
        channel_manager_ = channels::ChannelManager::create();
        ASSERT_NE(channel_manager_, nullptr);
        
        // 创建 Mock Channel
        mock_adapter_ = std::make_shared<MockChannelAdapter>();
    }
    
    void TearDown() override {
        if (gateway_ && gateway_->is_running()) {
            gateway_->stop();
        }
        
        if (channel_manager_ && mock_adapter_) {
            channel_manager_->stop_channel("mock", "test_account");
        }
    }
    
    std::shared_ptr<gateway::GatewayServer> gateway_;
    std::shared_ptr<channels::ChannelManager> channel_manager_;
    std::shared_ptr<MockChannelAdapter> mock_adapter_;
};

TEST_F(GatewayChannelIntegrationTest, StartBothServices) {
    // 启动 Gateway
    auto gateway_result = gateway_->start();
    ASSERT_TRUE(gateway_result.is_ok());
    EXPECT_TRUE(gateway_->is_running());
    
    // 注册并启动 Channel
    auto register_result = channel_manager_->register_channel("mock", mock_adapter_);
    ASSERT_TRUE(register_result.is_ok());
    
    auto channel_result = channel_manager_->start_channel("mock", "test_account");
    ASSERT_TRUE(channel_result.is_ok());
    EXPECT_TRUE(mock_adapter_->is_started());
}

TEST_F(GatewayChannelIntegrationTest, InboundMessageFlow) {
    // 启动服务
    gateway_->start();
    channel_manager_->register_channel("mock", mock_adapter_);
    
    std::atomic<int> messages_received{0};
    channels::InboundMessage captured_msg;
    
    // 设置消息回调（Channel -> Gateway）
    channel_manager_->set_message_callback(
        [&messages_received, &captured_msg](const channels::InboundMessage& msg) {
            messages_received++;
            captured_msg = msg;
        });
    
    channel_manager_->start_channel("mock", "test_account");
    
    // 模拟外部消息进入
    channels::InboundMessage test_msg;
    test_msg.message_id = "msg_001";
    test_msg.channel_id = "mock";
    test_msg.account_id = "test_account";
    test_msg.text = "Hello from external platform";
    test_msg.from_id = "user_external";
    
    mock_adapter_->simulate_receive_message(test_msg);
    
    // 等待消息传递
    std::this_thread::sleep_for(50ms);
    
    EXPECT_EQ(messages_received.load(), 1);
    EXPECT_EQ(captured_msg.text, "Hello from external platform");
}

TEST_F(GatewayChannelIntegrationTest, MultipleChannels) {
    auto adapter1 = std::make_shared<MockChannelAdapter>();
    auto adapter2 = std::make_shared<MockChannelAdapter>();
    
    channel_manager_->register_channel("mock1", adapter1);
    channel_manager_->register_channel("mock2", adapter2);
    
    auto channels = channel_manager_->list_channels();
    EXPECT_EQ(channels.size(), 2);
    
    // 启动所有 channel
    channel_manager_->start_channel("mock1", "account1");
    channel_manager_->start_channel("mock2", "account2");
    
    EXPECT_TRUE(adapter1->is_started());
    EXPECT_TRUE(adapter2->is_started());
    
    // 清理
    channel_manager_->stop_channel("mock1", "account1");
    channel_manager_->stop_channel("mock2", "account2");
}

TEST_F(GatewayChannelIntegrationTest, ChannelHealthMonitoring) {
    gateway_->start();
    channel_manager_->register_channel("mock", mock_adapter_);
    channel_manager_->start_channel("mock", "test_account");
    
    // Channel 应该是健康的
    auto health = mock_adapter_->get_health();
    EXPECT_EQ(health.status, channels::HealthStatus::Healthy);
    
    // 停止 channel
    mock_adapter_->stop();
    health = mock_adapter_->get_health();
    EXPECT_EQ(health.status, channels::HealthStatus::Unhealthy);
}

TEST_F(GatewayChannelIntegrationTest, GatewayRpcToChannel) {
    gateway_->start();
    channel_manager_->register_channel("mock", mock_adapter_);
    channel_manager_->start_channel("mock", "test_account");
    
    // 注册 RPC 方法：channels.list
    gateway_->register_handler("channels.list",
        [this](const std::string& conn_id, const nlohmann::json& params)
        -> core::Result<nlohmann::json> {
            auto channels = channel_manager_->list_channels();
            nlohmann::json result;
            result["channels"] = channels;
            return core::Result<nlohmann::json>::ok(result);
        });
    
    // TODO: 实际通过 WebSocket 客户端调用 RPC
    // 这里只验证注册成功
    EXPECT_TRUE(gateway_->is_running());
}

// ============================================================================
// 测试套件 4: 压力测试
// ============================================================================

class GatewayChannelStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        gateway::GatewayServerOptions options;
        options.port = 18792;
        options.bind = gateway::GatewayBindMode::Loopback;
        options.auth.mode = gateway::GatewayAuthMode::None;
        
        auto result = gateway::create_gateway_server(options);
        ASSERT_TRUE(result.is_ok());
        gateway_ = result.value;
        
        channel_manager_ = channels::ChannelManager::create();
        mock_adapter_ = std::make_shared<MockChannelAdapter>();
    }
    
    void TearDown() override {
        if (gateway_ && gateway_->is_running()) {
            gateway_->stop();
        }
    }
    
    std::shared_ptr<gateway::GatewayServer> gateway_;
    std::shared_ptr<channels::ChannelManager> channel_manager_;
    std::shared_ptr<MockChannelAdapter> mock_adapter_;
};

TEST_F(GatewayChannelStressTest, HighVolumeMessages) {
    gateway_->start();
    channel_manager_->register_channel("mock", mock_adapter_);
    
    std::atomic<int> total_received{0};
    
    channel_manager_->set_message_callback(
        [&total_received](const channels::InboundMessage& msg) {
            total_received++;
        });
    
    channel_manager_->start_channel("mock", "test_account");
    
    // 发送 1000 条消息
    const int MESSAGE_COUNT = 1000;
    auto start_time = std::chrono::steady_clock::now();
    
    for (int i = 0; i < MESSAGE_COUNT; i++) {
        channels::InboundMessage msg;
        msg.message_id = "msg_" + std::to_string(i);
        msg.text = "Test message #" + std::to_string(i);
        msg.from_id = "user_stress";
        
        mock_adapter_->simulate_receive_message(msg);
    }
    
    // 等待所有消息处理完成
    std::this_thread::sleep_for(500ms);
    
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time
    ).count();
    
    EXPECT_EQ(total_received.load(), MESSAGE_COUNT);
    
    double throughput = (MESSAGE_COUNT * 1000.0) / duration;
    std::cout << "Throughput: " << throughput << " messages/second" << std::endl;
    std::cout << "Latency: " << (duration / (double)MESSAGE_COUNT) << " ms/message" << std::endl;
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
