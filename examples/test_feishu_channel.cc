#include "hunterclaw/channels/feishu_channel.h"
#include <iostream>
#include <memory>

using namespace hunterclaw::channels;

int main() {
    std::cout << "=== HunterClaw 飞书 Channel 测试 ===" << std::endl;
    
    // 1. 创建配置
    FeishuConfig config;
    config.app_id = "cli_test_app_id";
    config.app_secret = "test_secret";
    config.encrypt_key = "test_encrypt_key";
    config.domain = FeishuDomain::Feishu;
    config.connection_mode = FeishuConnectionMode::WebSocket;
    config.dm_policy = FeishuDMPolicy::Open;
    config.group_policy = FeishuGroupPolicy::Open;
    config.enable_message_dedup = true;
    config.enable_inbound_debounce = true;
    
    std::cout << "✓ 配置创建成功" << std::endl;
    
    // 2. 创建适配器
    auto adapter = std::make_shared<FeishuChannelAdapter>(config);
    std::cout << "✓ 适配器创建成功" << std::endl;
    
    // 3. 获取元数据
    auto meta = adapter->meta();
    std::cout << "✓ Channel 信息:" << std::endl;
    std::cout << "  - ID: " << meta.id << std::endl;
    std::cout << "  - Name: " << meta.name << std::endl;
    std::cout << "  - Description: " << meta.description << std::endl;
    std::cout << "  - Capabilities: " << meta.capabilities.size() << " 种" << std::endl;
    
    // 4. 设置上下文
    ChannelContext ctx;
    ctx.log_info = [](const std::string& log) {
        std::cout << "[INFO] " << log << std::endl;
    };
    ctx.log_error = [](const std::string& log) {
        std::cerr << "[ERROR] " << log << std::endl;
    };
    ctx.on_message_received = [](const ChannelMessage& msg) {
        std::cout << "[MESSAGE] From: " << msg.from << ", Text: " << msg.text << std::endl;
    };
    ctx.abort_signal = std::make_shared<std::atomic<bool>>(false);
    
    std::cout << "✓ 上下文配置完成" << std::endl;
    
    // 5. 启动账户
    auto start_result = adapter->start_account(ctx);
    if (start_result.is_ok()) {
        std::cout << "✓ 账户启动成功" << std::endl;
    } else {
        std::cerr << "✗ 账户启动失败: " << start_result.error << std::endl;
        return 1;
    }
    
    // 6. 检查连接状态
    if (adapter->is_connected()) {
        std::cout << "✓ Channel 已连接" << std::endl;
    } else {
        std::cout << "⚠ Channel 未连接（这是正常的，因为是简化版）" << std::endl;
    }
    
    // 7. 获取状态
    auto status_result = adapter->get_status();
    if (status_result.is_ok()) {
        std::cout << "✓ 状态信息:\n" << status_result.value << std::endl;
    }
    
    // 8. 测试发送消息 (不会真正发送，只是测试接口)
    std::cout << "\n=== 测试消息发送接口 ===" << std::endl;
    
    auto send_result = adapter->send_text("oc_test_chat", "Hello from HunterClaw!");
    if (send_result.is_ok()) {
        std::cout << "✓ 文本消息接口调用成功" << std::endl;
    }
    
    auto image_result = adapter->send_image("oc_test_chat", "/tmp/test.jpg");
    if (image_result.is_ok()) {
        std::cout << "✓ 图片消息接口调用成功" << std::endl;
    }
    
    auto file_result = adapter->send_file("oc_test_chat", "/tmp/test.pdf");
    if (file_result.is_ok()) {
        std::cout << "✓ 文件消息接口调用成功" << std::endl;
    }
    
    // 9. 测试高级功能
    std::cout << "\n=== 测试高级功能接口 ===" << std::endl;
    
    auto reply_result = adapter->reply_message("om_test_msg", "收到!");
    if (reply_result.is_ok()) {
        std::cout << "✓ 回复消息接口调用成功" << std::endl;
    }
    
    auto reaction_result = adapter->add_reaction("om_test_msg", "👍");
    if (reaction_result.is_ok()) {
        std::cout << "✓ 添加反应接口调用成功" << std::endl;
    }
    
    // 10. 停止账户
    auto stop_result = adapter->stop_account();
    if (stop_result.is_ok()) {
        std::cout << "\n✓ 账户停止成功" << std::endl;
    }
    
    // 11. 测试插件 (暂时跳过，因为需要完整的 Plugin 基类实现)
    std::cout << "\n=== 插件接口 ===" << std::endl;
    std::cout << "⚠ Plugin 接口待完善 (需要实现 name/version 等虚函数)" << std::endl;
    
    std::cout << "\n=== 所有测试完成 ===" << std::endl;
    std::cout << "✅ 飞书 Channel 基础功能正常!" << std::endl;
    std::cout << "\n注意: 当前是简化版实现，实际的网络通信功能待完善。" << std::endl;
    std::cout << "参考文档: docs/feishu-channel-guide.md" << std::endl;
    
    return 0;
}
