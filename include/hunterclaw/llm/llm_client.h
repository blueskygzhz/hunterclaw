#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace hunterclaw {
namespace llm {

// ============================================================================
// LLM Message Types (对齐 OpenClaw/LangChain)
// ============================================================================

enum class LLMMessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct LLMMessage {
    LLMMessageRole role;
    std::string content;
    
    // 可选: 工具调用信息 (用于tool_calls模式)
    std::vector<core::JsonValue> tool_calls;
    std::string tool_call_id;  // 用于Tool消息
    std::string name;          // 工具名称
    
    LLMMessage() : role(LLMMessageRole::User) {}
    
    static LLMMessage system(const std::string& content) {
        LLMMessage msg;
        msg.role = LLMMessageRole::System;
        msg.content = content;
        return msg;
    }
    
    static LLMMessage user(const std::string& content) {
        LLMMessage msg;
        msg.role = LLMMessageRole::User;
        msg.content = content;
        return msg;
    }
    
    static LLMMessage assistant(const std::string& content) {
        LLMMessage msg;
        msg.role = LLMMessageRole::Assistant;
        msg.content = content;
        return msg;
    }
    
    static LLMMessage tool(
        const std::string& content,
        const std::string& tool_call_id,
        const std::string& name
    ) {
        LLMMessage msg;
        msg.role = LLMMessageRole::Tool;
        msg.content = content;
        msg.tool_call_id = tool_call_id;
        msg.name = name;
        return msg;
    }
};

// ============================================================================
// LLM Configuration
// ============================================================================

struct LLMConfig {
    std::string provider;          // "openai", "anthropic", "google", etc.
    std::string model;             // Model name
    std::string api_key;           // API key
    std::string endpoint;          // Optional custom endpoint
    
    // Generation parameters
    int max_tokens = 4096;
    double temperature = 0.7;
    double top_p = 1.0;
    int top_k = 0;
    std::vector<std::string> stop_sequences;
    
    // Context window
    int context_window = 200000;
    
    // Tool calling support
    bool supports_tool_calls = false;
    std::vector<core::JsonValue> tools;  // Tool definitions for tool_calls mode
    
    // Retry configuration
    int max_retries = 3;
    int retry_delay_ms = 1000;
};

// ============================================================================
// LLM Client Interface
// ============================================================================

class LLMClient {
public:
    using Ptr = std::shared_ptr<LLMClient>;
    
    virtual ~LLMClient() = default;
    
    /// 生成文本 (简单接口)
    /// @param prompt 输入prompt
    /// @return 生成的文本或错误
    virtual core::Result<std::string> generate(const std::string& prompt) = 0;
    
    /// 生成文本 (消息接口)
    /// @param messages 消息历史
    /// @return 生成的文本或错误
    virtual core::Result<std::string> generate_from_messages(
        const std::vector<LLMMessage>& messages
    ) = 0;
    
    /// 流式生成
    /// @param prompt 输入prompt
    /// @param callback 每个token的回调
    /// @return 完整生成的文本或错误
    virtual core::Result<std::string> generate_stream(
        const std::string& prompt,
        std::function<void(const std::string&)> callback
    ) = 0;
    
    /// 流式生成 (消息接口)
    /// @param messages 消息历史
    /// @param callback 每个token的回调
    /// @return 完整生成的文本或错误
    virtual core::Result<std::string> generate_stream_from_messages(
        const std::vector<LLMMessage>& messages,
        std::function<void(const std::string&)> callback
    ) {
        // 默认实现: 回退到非流式接口
        auto result = generate_from_messages(messages);
        if (result.ok && callback) {
            callback(result.get());
        }
        return result;
    }
    
    /// 获取配置
    virtual const LLMConfig& get_config() const = 0;
    
    /// 设置配置
    virtual void set_config(const LLMConfig& config) = 0;
    
    /// 估算token数量
    virtual int estimate_tokens(const std::string& text) const = 0;
};

// ============================================================================
// Mock LLM Client (用于测试)
// ============================================================================

class MockLLMClient : public LLMClient {
public:
    static Ptr create(const LLMConfig& config = LLMConfig());
    
    MockLLMClient(const LLMConfig& config);
    ~MockLLMClient() override = default;
    
    core::Result<std::string> generate(const std::string& prompt) override;
    
    core::Result<std::string> generate_from_messages(
        const std::vector<LLMMessage>& messages
    ) override;
    
    core::Result<std::string> generate_stream(
        const std::string& prompt,
        std::function<void(const std::string&)> callback
    ) override;
    
    const LLMConfig& get_config() const override { return config_; }
    void set_config(const LLMConfig& config) override { config_ = config; }
    
    int estimate_tokens(const std::string& text) const override;

private:
    LLMConfig config_;
};

// ============================================================================
// OpenAI LLM Client
// ============================================================================
// 注意：OpenAI 的实际实现在 llm::Provider 接口中（openai_provider.cc）
// LLMClient 是高层抽象，内部可以使用任意 Provider（OpenAI/Anthropic/Ollama）
// 如需直接使用 OpenAI，请使用 ProviderFactory::create_openai()

// ============================================================================
// Anthropic LLM Client
// ============================================================================

// Forward declaration
class Provider;

class AnthropicClient : public LLMClient {
public:
    static Ptr create(const LLMConfig& config);
    
    AnthropicClient(const LLMConfig& config);
    ~AnthropicClient() override = default;
    
    core::Result<std::string> generate(const std::string& prompt) override;
    
    core::Result<std::string> generate_from_messages(
        const std::vector<LLMMessage>& messages
    ) override;
    
    core::Result<std::string> generate_stream(
        const std::string& prompt,
        std::function<void(const std::string&)> callback
    ) override;
    
    const LLMConfig& get_config() const override { return config_; }
    void set_config(const LLMConfig& config) override { config_ = config; }
    
    int estimate_tokens(const std::string& text) const override;
    
    // ✅ 对齐 OpenClaw: 设置底层 Provider
    void set_provider(std::shared_ptr<Provider> provider) { provider_ = provider; }

private:
    LLMConfig config_;
    std::shared_ptr<Provider> provider_;  // ✅ 底层 Provider 实现
    
    core::Result<std::string> send_request(const core::JsonValue& payload);
};

// ============================================================================
// LLM Client Factory
// ============================================================================

class LLMClientFactory {
public:
    /// 根据配置创建LLM客户端 (使用 Provider 接口)
    static LLMClient::Ptr create(const LLMConfig& config);
    
    /// 从 Provider 创建 LLMClient (适配器模式)
    static LLMClient::Ptr create_from_provider(
        std::shared_ptr<Provider> provider,
        const LLMConfig& config
    );
    
    /// 注册自定义provider
    static void register_provider(
        const std::string& name,
        std::function<LLMClient::Ptr(const LLMConfig&)> factory
    );

private:
    static std::map<std::string, std::function<LLMClient::Ptr(const LLMConfig&)>> providers_;
};

} // namespace llm
} // namespace hunterclaw
