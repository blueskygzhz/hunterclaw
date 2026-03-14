/**
 * HunterClaw Thinking 输出到 Channel 完整示例
 * 
 * 演示如何将 AI 的 thinking（推理过程）实时输出到不同的聊天平台：
 * - Telegram
 * - Discord
 * - WebSocket
 * - 通用 Channel
 */

#include "hunterclaw/react/react_engine.h"
#include "hunterclaw/channels/channel.h"
#include "hunterclaw/gateway/connection_pool.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <thread>

using namespace hunterclaw;
using json = nlohmann::json;

// ============================================================================
// 示例 1: 基础 Thinking 输出（控制台）
// ============================================================================

void example_console_thinking() {
    std::cout << "\n=== 示例 1: 控制台 Thinking 输出 ===\n";
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    llm_config.api_key = std::getenv("ANTHROPIC_API_KEY");
    
    auto llm_client = llm::LLMClient::create(llm_config);
    
    // 配置推理模式
    react::ReactConfig config;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    // 配置回调
    react::StreamCallbacks callbacks;
    
    // ========================================================================
    // 🧠 Thinking 流式输出
    // ========================================================================
    callbacks.on_reasoning_stream = [](const std::string& reasoning) {
        std::cout << "\n╔═══════════════════════════════════╗\n"
                  << "║  🧠 AI 正在思考...              ║\n"
                  << "╚═══════════════════════════════════╝\n"
                  << reasoning << std::flush;
    };
    
    // ========================================================================
    // 📝 最终回复
    // ========================================================================
    callbacks.on_assistant_delta = [](const std::string& delta) {
        std::cout << delta << std::flush;
    };
    
    // ========================================================================
    // 执行
    // ========================================================================
    std::cout << "\n用户问题: 解释快速排序算法的时间复杂度\n\n";
    auto result = engine->run("解释快速排序算法的时间复杂度", {}, callbacks);
    
    std::cout << "\n\n✅ 完成！\n";
}

// ============================================================================
// 示例 2: Telegram 集成（流式更新）
// ============================================================================

class TelegramThinkingBridge {
private:
    std::shared_ptr<channels::ChannelAdapter> telegram_;
    std::string chat_id_;
    std::string thinking_msg_id_;
    std::string thinking_buffer_;
    std::chrono::steady_clock::time_point last_update_;
    
public:
    TelegramThinkingBridge(
        std::shared_ptr<channels::ChannelAdapter> telegram,
        const std::string& chat_id
    ) : telegram_(telegram), chat_id_(chat_id) {}
    
    // ========================================================================
    // 创建 StreamCallbacks
    // ========================================================================
    react::StreamCallbacks create_callbacks() {
        react::StreamCallbacks callbacks;
        
        // 🧠 Thinking 流
        callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
            thinking_buffer_ += reasoning;
            
            // 限流：最多每 500ms 更新一次
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_update_
            );
            
            if (elapsed.count() < 500 && !thinking_buffer_.empty()) {
                return;  // 跳过此次更新
            }
            
            // 构造 Telegram Markdown 格式
            std::string formatted = format_telegram_thinking(thinking_buffer_);
            
            if (thinking_msg_id_.empty()) {
                // 创建新消息
                auto result = telegram_->send_text(chat_id_, formatted);
                thinking_msg_id_ = "msg_" + std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()
                );
            } else {
                // 更新已有消息（流式效果）
                // telegram_->edit_message(thinking_msg_id_, formatted);
                
                // 模拟更新（实际需要调用 Telegram API）
                std::cout << "\n[Telegram 更新消息] ID: " << thinking_msg_id_ 
                         << ", 长度: " << thinking_buffer_.size() << " 字符\n";
            }
            
            last_update_ = now;
        };
        
        // 📝 最终回复
        std::string response_buffer;
        
        callbacks.on_assistant_delta = [&response_buffer](const std::string& delta) {
            response_buffer += delta;
        };
        
        callbacks.on_end = [this, &response_buffer]() {
            // 删除 thinking 消息（可选）
            if (!thinking_msg_id_.empty()) {
                // telegram_->delete_message(thinking_msg_id_);
                std::cout << "[Telegram] 删除 thinking 消息: " << thinking_msg_id_ << "\n";
            }
            
            // 发送最终回复
            std::string formatted = "📝 *回复*\n\n" + response_buffer;
            telegram_->send_text(chat_id_, formatted);
            
            std::cout << "[Telegram] 发送最终回复\n";
        };
        
        return callbacks;
    }
    
private:
    std::string format_telegram_thinking(const std::string& thinking) {
        // Telegram Markdown V2 格式
        std::string escaped = escape_markdown(thinking);
        return "🧠 *思考中\\.\\.\\.*\n\n```\n" + escaped + "\n```";
    }
    
    std::string escape_markdown(const std::string& text) {
        // 简化版转义（实际需要转义更多字符）
        std::string result = text;
        // 这里应该实现完整的 Markdown 转义逻辑
        return result;
    }
};

void example_telegram_thinking() {
    std::cout << "\n=== 示例 2: Telegram Thinking 集成 ===\n";
    
    // 创建模拟 Telegram adapter
    // 实际使用时需要真实的 TelegramAdapter
    auto telegram = std::make_shared<channels::ChannelAdapter>(/* ... */);
    
    TelegramThinkingBridge bridge(telegram, "123456789");  // Your Telegram chat ID
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    
    react::ReactConfig config;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    // 使用 bridge 创建的 callbacks
    auto callbacks = bridge.create_callbacks();
    
    std::cout << "模拟发送问题到 Telegram...\n";
    auto result = engine->run("什么是二叉搜索树？", {}, callbacks);
    
    std::cout << "\n✅ Telegram 集成完成！\n";
}

// ============================================================================
// 示例 3: WebSocket Gateway 集成
// ============================================================================

class WebSocketThinkingBridge {
private:
    std::string session_id_;
    
public:
    WebSocketThinkingBridge(const std::string& session_id) 
        : session_id_(session_id) {}
    
    react::StreamCallbacks create_callbacks() {
        react::StreamCallbacks callbacks;
        
        // ====================================================================
        // 1. Thinking 流
        // ====================================================================
        callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
            json payload = {
                {"type", "thinking"},
                {"content", reasoning},
                {"format", "markdown"},
                {"timestamp", get_timestamp()}
            };
            
            send_ws_event("agent.reasoning", payload);
            
            std::cout << "[WS] 发送 thinking: " << reasoning.substr(0, 50) << "...\n";
        };
        
        // ====================================================================
        // 2. 文本增量
        // ====================================================================
        callbacks.on_assistant_delta = [this](const std::string& delta) {
            json payload = {
                {"text", delta}
            };
            
            send_ws_event("agent.text_delta", payload);
        };
        
        // ====================================================================
        // 3. 块回复
        // ====================================================================
        callbacks.on_block_reply = [this](const std::string& block) {
            json payload = {
                {"content", block},
                {"length", block.size()},
                {"timestamp", get_timestamp()}
            };
            
            send_ws_event("agent.block_reply", payload);
            
            std::cout << "[WS] 发送 block_reply: " << block.size() << " 字符\n";
        };
        
        // ====================================================================
        // 4. 工具调用
        // ====================================================================
        callbacks.on_tool_call_start = [this](const std::string& name, const std::string& args) {
            json payload = {
                {"tool_name", name},
                {"args", json::parse(args)},
                {"status", "started"}
            };
            
            send_ws_event("agent.tool_start", payload);
        };
        
        callbacks.on_tool_call_end = [this](const std::string& name, const tools::ToolResult& result) {
            json payload = {
                {"tool_name", name},
                {"success", result.ok},
                {"output", result.output}
            };
            
            send_ws_event("agent.tool_end", payload);
        };
        
        // ====================================================================
        // 5. 生命周期
        // ====================================================================
        callbacks.on_start = [this]() {
            send_ws_event("agent.start", {{"status", "running"}});
            std::cout << "[WS] Agent 开始执行\n";
        };
        
        callbacks.on_end = [this]() {
            send_ws_event("agent.end", {{"status", "completed"}});
            std::cout << "[WS] Agent 执行完成\n";
        };
        
        callbacks.on_error = [this](const std::string& error) {
            send_ws_event("agent.error", {{"error", error}});
            std::cerr << "[WS] Agent 错误: " << error << "\n";
        };
        
        return callbacks;
    }
    
private:
    void send_ws_event(const std::string& event_type, const json& payload) {
        // 使用 HunterClaw 的 ConnectionPool 发送事件
        gateway::ConnectionPool::instance().send_event(
            session_id_,
            event_type,
            core::JsonValue::from_json(payload.dump())
        );
    }
    
    int64_t get_timestamp() {
        return std::chrono::system_clock::now().time_since_epoch().count();
    }
};

void example_websocket_thinking() {
    std::cout << "\n=== 示例 3: WebSocket Thinking 集成 ===\n";
    
    WebSocketThinkingBridge bridge("session-12345");
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    
    react::ReactConfig config;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
    config.block_reply.enabled = true;
    config.block_reply.min_chars = 100;
    config.block_reply.max_chars = 500;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    auto callbacks = bridge.create_callbacks();
    
    std::cout << "模拟 WebSocket 连接...\n";
    auto result = engine->run("分析归并排序的空间复杂度", {}, callbacks);
    
    std::cout << "\n✅ WebSocket 集成完成！\n";
}

// ============================================================================
// 示例 4: 通用 Channel 适配器
// ============================================================================

class UniversalChannelBridge {
public:
    enum class ChannelType {
        Telegram,
        Discord,
        WhatsApp,
        Slack,
        Console
    };
    
    struct ChannelCapabilities {
        bool supports_message_edit;
        bool supports_threading;
        bool supports_markdown;
        bool supports_code_blocks;
        std::string thinking_emoji;
        std::string response_emoji;
    };
    
    static ChannelCapabilities get_capabilities(ChannelType type) {
        switch (type) {
            case ChannelType::Telegram:
                return {
                    .supports_message_edit = true,
                    .supports_threading = false,
                    .supports_markdown = true,
                    .supports_code_blocks = true,
                    .thinking_emoji = "🧠",
                    .response_emoji = "📝"
                };
                
            case ChannelType::Discord:
                return {
                    .supports_message_edit = true,
                    .supports_threading = true,
                    .supports_markdown = true,
                    .supports_code_blocks = true,
                    .thinking_emoji = "💭",
                    .response_emoji = "📝"
                };
                
            case ChannelType::WhatsApp:
                return {
                    .supports_message_edit = false,
                    .supports_threading = false,
                    .supports_markdown = false,
                    .supports_code_blocks = false,
                    .thinking_emoji = "🧠",
                    .response_emoji = "📝"
                };
                
            case ChannelType::Slack:
                return {
                    .supports_message_edit = true,
                    .supports_threading = true,
                    .supports_markdown = true,
                    .supports_code_blocks = true,
                    .thinking_emoji = "🧠",
                    .response_emoji = "📝"
                };
                
            default:  // Console
                return {
                    .supports_message_edit = false,
                    .supports_threading = false,
                    .supports_markdown = false,
                    .supports_code_blocks = false,
                    .thinking_emoji = "[思考]",
                    .response_emoji = "[回复]"
                };
        }
    }
    
    static react::StreamCallbacks create_callbacks(
        ChannelType type,
        std::shared_ptr<channels::ChannelAdapter> channel,
        const std::string& target_id
    ) {
        auto caps = get_capabilities(type);
        react::StreamCallbacks callbacks;
        
        std::string thinking_msg_id;
        std::string thinking_buffer;
        auto last_update = std::chrono::steady_clock::now();
        
        // ====================================================================
        // Thinking 流
        // ====================================================================
        callbacks.on_reasoning_stream = [=, &thinking_msg_id, &thinking_buffer, &last_update]
            (const std::string& reasoning) mutable {
            
            thinking_buffer += reasoning;
            
            // 限流
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_update
            );
            
            if (elapsed.count() < 500) {
                return;
            }
            
            // 格式化
            std::string formatted = format_thinking(thinking_buffer, caps);
            
            // 发送
            if (caps.supports_message_edit) {
                if (thinking_msg_id.empty()) {
                    auto result = channel->send_text(target_id, formatted);
                    thinking_msg_id = "msg_" + std::to_string(now.time_since_epoch().count());
                } else {
                    // channel->edit_message(thinking_msg_id, formatted);
                    std::cout << "[更新消息] " << thinking_msg_id << "\n";
                }
            } else {
                channel->send_text(target_id, formatted);
            }
            
            last_update = now;
        };
        
        // ====================================================================
        // 最终回复
        // ====================================================================
        std::string response_buffer;
        
        callbacks.on_assistant_delta = [&response_buffer](const std::string& delta) {
            response_buffer += delta;
        };
        
        callbacks.on_end = [=, &response_buffer]() {
            std::string formatted = caps.response_emoji + " *回复*\n\n" + response_buffer;
            channel->send_text(target_id, formatted);
        };
        
        return callbacks;
    }
    
private:
    static std::string format_thinking(
        const std::string& thinking,
        const ChannelCapabilities& caps
    ) {
        std::string result = caps.thinking_emoji + " ";
        
        if (caps.supports_markdown) {
            result += "*思考中...*\n\n";
        } else {
            result += "思考中...\n\n";
        }
        
        if (caps.supports_code_blocks) {
            result += "```\n" + thinking + "\n```";
        } else {
            result += thinking;
        }
        
        return result;
    }
};

void example_universal_channel() {
    std::cout << "\n=== 示例 4: 通用 Channel 适配器 ===\n";
    
    // 测试不同的 channel 类型
    std::vector<UniversalChannelBridge::ChannelType> types = {
        UniversalChannelBridge::ChannelType::Telegram,
        UniversalChannelBridge::ChannelType::Discord,
        UniversalChannelBridge::ChannelType::WhatsApp
    };
    
    for (auto type : types) {
        std::string type_name;
        switch (type) {
            case UniversalChannelBridge::ChannelType::Telegram:
                type_name = "Telegram";
                break;
            case UniversalChannelBridge::ChannelType::Discord:
                type_name = "Discord";
                break;
            case UniversalChannelBridge::ChannelType::WhatsApp:
                type_name = "WhatsApp";
                break;
            default:
                type_name = "Unknown";
        }
        
        std::cout << "\n--- 测试 " << type_name << " ---\n";
        
        auto caps = UniversalChannelBridge::get_capabilities(type);
        std::cout << "支持消息编辑: " << (caps.supports_message_edit ? "✅" : "❌") << "\n";
        std::cout << "支持 Markdown: " << (caps.supports_markdown ? "✅" : "❌") << "\n";
        std::cout << "支持代码块: " << (caps.supports_code_blocks ? "✅" : "❌") << "\n";
    }
    
    std::cout << "\n✅ 通用适配器完成！\n";
}

// ============================================================================
// 示例 5: 混合输出（控制台 + 日志 + WebSocket）
// ============================================================================

class HybridThinkingOutput {
private:
    std::ofstream log_file_;
    WebSocketThinkingBridge ws_bridge_;
    
public:
    HybridThinkingOutput(
        const std::string& log_path,
        const std::string& session_id
    ) : ws_bridge_(session_id) {
        log_file_.open(log_path, std::ios::app);
        
        // 写入日志头
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        log_file_ << "\n=== Session Start: " << std::ctime(&time) << " ===\n";
    }
    
    ~HybridThinkingOutput() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
    }
    
    react::StreamCallbacks create_callbacks() {
        react::StreamCallbacks callbacks;
        
        // ====================================================================
        // Thinking 流：三路输出
        // ====================================================================
        callbacks.on_reasoning_stream = [this](const std::string& reasoning) {
            // 1. 控制台（开发调试）
            std::cout << "[🧠 思考] " << reasoning << std::flush;
            
            // 2. 日志文件（持久化）
            log_file_ << "[THINKING] " << reasoning << "\n";
            log_file_.flush();
            
            // 3. WebSocket（前端实时显示）
            json payload = {
                {"type", "thinking"},
                {"content", reasoning}
            };
            // ws_bridge_.send_ws_event("agent.reasoning", payload);
        };
        
        // ====================================================================
        // 文本增量：三路输出
        // ====================================================================
        callbacks.on_assistant_delta = [this](const std::string& delta) {
            std::cout << delta << std::flush;
            log_file_ << delta;
            
            json payload = {{"text", delta}};
            // ws_bridge_.send_ws_event("agent.text_delta", payload);
        };
        
        // ====================================================================
        // 工具调用：记录日志
        // ====================================================================
        callbacks.on_tool_call_start = [this](const std::string& name, const std::string& args) {
            std::string msg = "[工具调用] " + name + "\n";
            std::cout << msg;
            log_file_ << msg;
        };
        
        callbacks.on_tool_call_end = [this](const std::string& name, const tools::ToolResult& result) {
            std::string msg = "[工具完成] " + name + " - " + 
                             (result.ok ? "成功" : "失败") + "\n";
            std::cout << msg;
            log_file_ << msg;
        };
        
        return callbacks;
    }
};

void example_hybrid_output() {
    std::cout << "\n=== 示例 5: 混合输出（控制台 + 日志 + WebSocket）===\n";
    
    HybridThinkingOutput hybrid("agent_thinking.log", "session-67890");
    
    auto tool_mgr = tools::ToolManager::create();
    auto llm_config = llm::LLMConfig{};
    llm_config.provider = "anthropic";
    llm_config.model = "claude-3-5-sonnet-20241022";
    auto llm_client = llm::LLMClient::create(llm_config);
    
    react::ReactConfig config;
    config.reasoning_mode = react::ReactConfig::ReasoningMode::Stream;
    
    auto engine = react::ReactEngine::create(tool_mgr, llm_client, config);
    
    auto callbacks = hybrid.create_callbacks();
    
    std::cout << "开始混合输出...\n";
    auto result = engine->run("比较堆排序和快速排序", {}, callbacks);
    
    std::cout << "\n\n✅ 混合输出完成！请查看 agent_thinking.log\n";
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║  HunterClaw Thinking → Channel 输出示例             ║\n"
              << "║                                                      ║\n"
              << "║  演示如何将 AI 的 thinking 过程输出到不同的 channel  ║\n"
              << "╚══════════════════════════════════════════════════════╝\n";
    
    try {
        // 示例 1: 控制台输出
        example_console_thinking();
        
        // 示例 2: Telegram 集成
        // example_telegram_thinking();  // 需要真实的 Telegram adapter
        
        // 示例 3: WebSocket 集成
        // example_websocket_thinking();  // 需要启动 Gateway
        
        // 示例 4: 通用适配器
        example_universal_channel();
        
        // 示例 5: 混合输出
        // example_hybrid_output();
        
        std::cout << "\n\n╔══════════════════════════════════════════════════════╗\n"
                  << "║  ✅ 所有示例执行完成！                              ║\n"
                  << "╚══════════════════════════════════════════════════════╝\n";
        
    } catch (const std::exception& e) {
        std::cerr << "❌ 错误: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
