#include "hunterclaw/llm/llm_client.h"
#include "hunterclaw/llm/provider.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace llm {

// ============================================================================
// ProviderLLMClient - 使用 Provider 接口实现的 LLMClient
// ============================================================================

class ProviderLLMClient : public LLMClient {
public:
    ProviderLLMClient(std::shared_ptr<llm::Provider> provider, const LLMConfig& config)
        : provider_(std::move(provider)), config_(config) {}
    
    core::Result<std::string> generate(const std::string& prompt) override {
        GenerateOptions options;
        options.messages = {Message::user(prompt)};
        
        auto result = provider_->generate(options);
        if (!result.is_ok()) {
            return core::Result<std::string>::Err(result.error);
        }
        
        return core::Result<std::string>::Ok(result.value.content);
    }
    
    core::Result<std::string> generate_from_messages(
        const std::vector<LLMMessage>& messages
    ) override {
        GenerateOptions options;
        
        // 转换 LLMMessage -> Provider Message
        for (const auto& msg : messages) {
            Message provider_msg;
            
            switch (msg.role) {
                case LLMMessageRole::System:
                    provider_msg = Message::system(msg.content);
                    break;
                case LLMMessageRole::User:
                    provider_msg = Message::user(msg.content);
                    break;
                case LLMMessageRole::Assistant:
                    provider_msg = Message::assistant(msg.content);
                    break;
                case LLMMessageRole::Tool:
                    provider_msg = Message::tool_result(msg.content, msg.tool_call_id, msg.name);
                    break;
            }
            
            // 如果有 tool_calls (Assistant 消息)
            if (!msg.tool_calls.empty()) {
                // 转换 JsonValue tool_calls -> Provider ToolCall
                // 注意: LLMMessage::tool_calls 是 vector<JsonValue> 类型
                for (const auto& call_json : msg.tool_calls) {
                    ToolCall tool_call;
                    tool_call.id = call_json["id"].as_string();
                    tool_call.name = call_json["name"].as_string();
                    tool_call.input = call_json["arguments"];
                    provider_msg.tool_calls.push_back(tool_call);
                }
            }
            
            options.messages.push_back(provider_msg);
        }
        
        // 从 config 设置参数
        options.max_tokens = config_.max_tokens;
        options.temperature = config_.temperature;
        options.top_p = config_.top_p;
        options.top_k = config_.top_k;
        options.stop_sequences = config_.stop_sequences;
        
        // 如果配置了 tools
        if (config_.supports_tool_calls && !config_.tools.empty()) {
            // 转换 JsonValue tools -> Provider ToolDefinition
            for (const auto& tool_json : config_.tools) {
                ToolDefinition tool;
                tool.name = tool_json["name"].as_string();
                tool.description = tool_json["description"].as_string();
                tool.input_schema = tool_json["input_schema"];
                options.tools.push_back(tool);
            }
            options.tool_choice = "auto";
        }
        
        auto result = provider_->generate(options);
        if (!result.is_ok()) {
            return core::Result<std::string>::Err(result.error);
        }
        
        return core::Result<std::string>::Ok(result.value.content);
    }
    
    core::Result<std::string> generate_stream(
        const std::string& prompt,
        std::function<void(const std::string&)> callback
    ) override {
        GenerateOptions options;
        options.messages = {Message::user(prompt)};
        
        // 流式回调适配
        auto stream_callback = [callback](const StreamChunk& chunk) {
            if (callback && !chunk.delta.empty()) {
                callback(chunk.delta);
            }
        };
        
        auto result = provider_->generate_stream(options, stream_callback);
        if (!result.is_ok()) {
            return core::Result<std::string>::Err(result.error);
        }
        
        return core::Result<std::string>::Ok(result.value.content);
    }
    
    core::Result<std::string> generate_stream_from_messages(
        const std::vector<LLMMessage>& messages,
        std::function<void(const std::string&)> callback
    ) override {
        GenerateOptions options;
        
        // 转换消息 (同 generate_from_messages)
        for (const auto& msg : messages) {
            Message provider_msg;
            
            switch (msg.role) {
                case LLMMessageRole::System:
                    provider_msg = Message::system(msg.content);
                    break;
                case LLMMessageRole::User:
                    provider_msg = Message::user(msg.content);
                    break;
                case LLMMessageRole::Assistant:
                    provider_msg = Message::assistant(msg.content);
                    break;
                case LLMMessageRole::Tool:
                    provider_msg = Message::tool_result(msg.content, msg.tool_call_id, msg.name);
                    break;
            }
            
            // 转换 tool_calls (如果有)
            if (!msg.tool_calls.empty()) {
                for (const auto& call_json : msg.tool_calls) {
                    ToolCall tool_call;
                    tool_call.id = call_json["id"].as_string();
                    tool_call.name = call_json["name"].as_string();
                    tool_call.input = call_json["arguments"];
                    provider_msg.tool_calls.push_back(tool_call);
                }
            }
            
            options.messages.push_back(provider_msg);
        }
        
        // 设置参数
        options.max_tokens = config_.max_tokens;
        options.temperature = config_.temperature;
        options.top_p = config_.top_p;
        options.top_k = config_.top_k;
        options.stop_sequences = config_.stop_sequences;
        
        // 工具配置
        if (config_.supports_tool_calls && !config_.tools.empty()) {
            for (const auto& tool_json : config_.tools) {
                ToolDefinition tool;
                tool.name = tool_json["name"].as_string();
                tool.description = tool_json["description"].as_string();
                tool.input_schema = tool_json["input_schema"];
                options.tools.push_back(tool);
            }
            options.tool_choice = "auto";
        }
        
        // 流式回调适配
        auto stream_callback = [callback](const StreamChunk& chunk) {
            if (callback && !chunk.delta.empty()) {
                callback(chunk.delta);
            }
        };
        
        auto result = provider_->generate_stream(options, stream_callback);
        if (!result.is_ok()) {
            return core::Result<std::string>::Err(result.error);
        }
        
        return core::Result<std::string>::Ok(result.value.content);
    }
    
    const LLMConfig& get_config() const override { 
        return config_; 
    }
    
    void set_config(const LLMConfig& config) override { 
        config_ = config; 
    }
    
    int estimate_tokens(const std::string& text) const override {
        // 简单估算: ~4 字符 = 1 token
        return static_cast<int>(text.size() / 4);
    }

private:
    std::shared_ptr<llm::Provider> provider_;
    LLMConfig config_;
};

// ============================================================================
// LLMClientFactory 增强实现 - 使用 Provider
// ============================================================================

// 实现 create_from_provider_impl (被 llm_client.cc 调用)
LLMClient::Ptr create_from_provider_impl(
    std::shared_ptr<llm::Provider> provider,
    const LLMConfig& config
) {
    return std::make_shared<ProviderLLMClient>(provider, config);
}

// 实现 create_llm_client_impl (被 llm_client.cc 调用)
LLMClient::Ptr create_llm_client_impl(const LLMConfig& config) {
    // Note: providers_ is checked in LLMClientFactory::create() before calling this function
    // So we don't need to check it again here
    
    // 🎯 使用新的 Provider 接口
    ProviderConfig provider_config;
    provider_config.api_key = config.api_key;
    provider_config.base_url = config.endpoint.empty() 
        ? "" 
        : config.endpoint;
    provider_config.model = config.model;
    provider_config.max_tokens = config.max_tokens;
    provider_config.temperature = config.temperature;
    provider_config.top_p = config.top_p;
    provider_config.max_retries = config.max_retries;
    provider_config.retry_delay_ms = config.retry_delay_ms;
    
    // 根据 provider 类型创建对应的 Provider
    core::Result<std::shared_ptr<Provider>> provider_result;
    
    if (config.provider == "openai") {
        provider_result = ProviderFactory::create_openai(
            config.api_key,
            config.model.empty() ? "gpt-4" : config.model,
            config.endpoint
        );
    } else if (config.provider == "anthropic" || config.provider == "claude") {
        provider_result = ProviderFactory::create_anthropic(
            config.api_key,
            config.model.empty() ? "claude-3-5-sonnet-20241022" : config.model
        );
    } else if (config.provider == "ollama") {
        provider_result = ProviderFactory::create_ollama(
            config.model.empty() ? "llama3.2" : config.model,
            config.endpoint.empty() ? "http://localhost:11434" : config.endpoint
        );
    } else if (config.provider == "gemini") {
        provider_result = ProviderFactory::create_gemini(
            config.api_key,
            config.model.empty() ? "gemini-2.0-flash-exp" : config.model
        );
    } else if (config.provider == "mock") {
        // 保留 mock 实现
        return MockLLMClient::create(config);
    } else {
        // Unknown provider, use mock
        return MockLLMClient::create(config);
    }
    
    if (!provider_result.is_ok()) {
        // Provider creation failed, use mock
        return MockLLMClient::create(config);
    }
    
    return create_from_provider_impl(provider_result.value, config);
}

} // namespace llm
} // namespace hunterclaw
