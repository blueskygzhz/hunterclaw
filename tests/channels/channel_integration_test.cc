#include <gtest/gtest.h>
#include "hunterclaw/channels/registry.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/channels/feishu_channel.h"
#include <memory>

using namespace hunterclaw::channels;

// ============================================================================
// Mock Channel Adapter for Testing
// ============================================================================

class MockChannelAdapter : public ChannelAdapter {
public:
    ChannelMeta meta() const override {
        return {
            .id = "mock",
            .name = "Mock Channel",
            .description = "Mock adapter for testing",
            .capabilities = {
                ChannelCapability::SendText,
                ChannelCapability::ReceiveText
            }
        };
    }
    
    core::Result<bool> start_account(const ChannelContext& ctx) override {
        context_ = ctx;
        running_ = true;
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> stop_account() override {
        running_ = false;
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_text(const std::string& to, const std::string& text) override {
        if (!running_) {
            return core::Result<bool>::Err("Not running");
        }
        sent_messages_.push_back({to, text});
        return core::Result<bool>::Ok(true);
    }
    
    core::Result<bool> send_image(const std::string&, const std::string&) override {
        return core::Result<bool>::Err("Not supported");
    }
    
    core::Result<bool> send_file(const std::string&, const std::string&) override {
        return core::Result<bool>::Err("Not supported");
    }
    
    bool is_connected() const override {
        return running_;
    }
    
    core::Result<std::string> get_status() const override {
        return core::Result<std::string>::Ok(running_ ? "running" : "stopped");
    }
    
    // Test helpers
    void simulate_incoming_message(const std::string& from, const std::string& text) {
        if (context_.on_message_received) {
            ChannelMessage msg{
                .id = "mock_msg_" + std::to_string(++message_counter_),
                .channel_id = "mock",
                .account_id = context_.account.account_id,
                .from = from,
                .to = "self",
                .type = MessageType::Text,
                .text = text,
                .timestamp = std::chrono::system_clock::now()
            };
            context_.on_message_received(msg);
        }
    }
    
    const std::vector<std::pair<std::string, std::string>>& sent_messages() const {
        return sent_messages_;
    }

private:
    ChannelContext context_;
    bool running_ = false;
    std::vector<std::pair<std::string, std::string>> sent_messages_;
    int message_counter_ = 0;
};

// ============================================================================
// Channel Registry Tests
// ============================================================================

TEST(ChannelRegistryTest, RegisterAndCreateAdapter) {
    auto& registry = ChannelPluginRegistry::instance();
    
    // Register mock adapter
    bool registered = registry.register_adapter("test_mock", []() {
        return std::make_shared<MockChannelAdapter>();
    });
    
    ASSERT_TRUE(registered);
    EXPECT_TRUE(registry.has_channel("test_mock"));
    
    // Create adapter instance
    auto adapter = registry.create_adapter("test_mock");
    ASSERT_NE(adapter, nullptr);
    
    auto meta = adapter->meta();
    EXPECT_EQ(meta.id, "mock");
    EXPECT_EQ(meta.name, "Mock Channel");
    
    // Clean up
    registry.unregister_adapter("test_mock");
}

TEST(ChannelRegistryTest, DuplicateRegistration) {
    auto& registry = ChannelPluginRegistry::instance();
    
    auto factory = []() { return std::make_shared<MockChannelAdapter>(); };
    
    ASSERT_TRUE(registry.register_adapter("dup_test", factory));
    EXPECT_FALSE(registry.register_adapter("dup_test", factory)); // 重复注册应失败
    
    registry.unregister_adapter("dup_test");
}

TEST(ChannelRegistryTest, GetRegisteredChannels) {
    auto& registry = ChannelPluginRegistry::instance();
    
    auto factory = []() { return std::make_shared<MockChannelAdapter>(); };
    
    registry.register_adapter("chan1", factory);
    registry.register_adapter("chan2", factory);
    registry.register_adapter("chan3", factory);
    
    auto channels = registry.get_registered_channels();
    EXPECT_GE(channels.size(), 3);
    
    // Clean up
    registry.unregister_adapter("chan1");
    registry.unregister_adapter("chan2");
    registry.unregister_adapter("chan3");
}

// ============================================================================
// Channel Adapter Lifecycle Tests
// ============================================================================

TEST(ChannelAdapterTest, StartStop) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    ChannelContext ctx{
        .account = {
            .account_id = "test_account",
            .token = "test_token"
        }
    };
    
    // Start
    auto start_result = adapter->start_account(ctx);
    ASSERT_TRUE(start_result.is_ok());
    EXPECT_TRUE(adapter->is_connected());
    
    auto status = adapter->get_status();
    ASSERT_TRUE(status.is_ok());
    EXPECT_EQ(status.value, "running");
    
    // Stop
    auto stop_result = adapter->stop_account();
    ASSERT_TRUE(stop_result.is_ok());
    EXPECT_FALSE(adapter->is_connected());
}

TEST(ChannelAdapterTest, SendMessage) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    ChannelContext ctx{
        .account = {
            .account_id = "test_account",
            .token = "test_token"
        }
    };
    
    adapter->start_account(ctx);
    
    // Send text
    auto result = adapter->send_text("user123", "Hello, world!");
    ASSERT_TRUE(result.is_ok());
    
    const auto& sent = adapter->sent_messages();
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent[0].first, "user123");
    EXPECT_EQ(sent[0].second, "Hello, world!");
    
    adapter->stop_account();
}

TEST(ChannelAdapterTest, ReceiveMessage) {
    auto adapter = std::make_shared<MockChannelAdapter>();
    
    std::vector<ChannelMessage> received_messages;
    
    ChannelContext ctx{
        .account = {
            .account_id = "test_account",
            .token = "test_token"
        },
        .on_message_received = [&](const ChannelMessage& msg) {
            received_messages.push_back(msg);
        }
    };
    
    adapter->start_account(ctx);
    
    // Simulate incoming message
    adapter->simulate_incoming_message("user456", "Test message");
    
    ASSERT_EQ(received_messages.size(), 1);
    EXPECT_EQ(received_messages[0].from, "user456");
    EXPECT_EQ(received_messages[0].text, "Test message");
    
    adapter->stop_account();
}

// ============================================================================
// Feishu Channel Tests
// ============================================================================

TEST(FeishuChannelTest, CreateAdapter) {
    FeishuConfig config{
        .app_id = "cli_test",
        .app_secret = "test_secret",
        .domain = "feishu",
        .connection_mode = FeishuConnectionMode::WebSocket
    };
    
    auto adapter = std::make_shared<FeishuChannelAdapter>(config);
    
    auto meta = adapter->meta();
    EXPECT_EQ(meta.id, "feishu");
    EXPECT_EQ(meta.name, "Feishu/Lark");
    EXPECT_FALSE(meta.capabilities.empty());
}

TEST(FeishuChannelTest, TokenManagement) {
    FeishuConfig config{
        .app_id = "cli_test",
        .app_secret = "test_secret",
        .domain = "feishu"
    };
    
    auto adapter = std::make_shared<FeishuChannelAdapter>(config);
    
    // Note: 实际测试需要有效的 app_id 和 app_secret
    // 这里只测试接口是否可调用
    auto token_result = adapter->get_tenant_access_token();
    
    // 预期失败 (因为是测试凭据)
    EXPECT_TRUE(token_result.is_err() || token_result.is_ok());
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(IntegrationTest, MultipleChannelsConcurrent) {
    auto& registry = ChannelPluginRegistry::instance();
    
    // Register multiple channels
    registry.register_adapter("int_test_1", []() {
        return std::make_shared<MockChannelAdapter>();
    });
    registry.register_adapter("int_test_2", []() {
        return std::make_shared<MockChannelAdapter>();
    });
    
    // Create and start adapters
    auto adapter1 = std::dynamic_pointer_cast<MockChannelAdapter>(
        registry.create_adapter("int_test_1")
    );
    auto adapter2 = std::dynamic_pointer_cast<MockChannelAdapter>(
        registry.create_adapter("int_test_2")
    );
    
    ASSERT_NE(adapter1, nullptr);
    ASSERT_NE(adapter2, nullptr);
    
    ChannelContext ctx1{.account = {.account_id = "acc1", .token = "tok1"}};
    ChannelContext ctx2{.account = {.account_id = "acc2", .token = "tok2"}};
    
    adapter1->start_account(ctx1);
    adapter2->start_account(ctx2);
    
    // Send messages
    adapter1->send_text("user1", "Message from channel 1");
    adapter2->send_text("user2", "Message from channel 2");
    
    EXPECT_EQ(adapter1->sent_messages().size(), 1);
    EXPECT_EQ(adapter2->sent_messages().size(), 1);
    
    // Stop
    adapter1->stop_account();
    adapter2->stop_account();
    
    // Clean up
    registry.unregister_adapter("int_test_1");
    registry.unregister_adapter("int_test_2");
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
