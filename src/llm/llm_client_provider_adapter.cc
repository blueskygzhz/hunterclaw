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
        
        return core::Result<std::string>::Ok(result.get().content);
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
        
        return core::Result<std::string>::Ok(result.get().content);
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
        
        return core::Result<std::string>::Ok(result.get().content);
    }
    
    core::Result<std::string> generate_stream_from_messages(
        const std::vector<LLMMessage>& messages,
        std::function<void(const std::string&)> callback
    ) override {
        // 旧路径：使用 config_.tools (保留向后兼容)
        GenerateOptions options = build_generate_options(messages, /*explicit_tools=*/nullptr);
        return do_generate_stream(options, callback);
    }

    // 🎯 对齐 OpenClaw pi-embedded-runner/tools-payload.ts:
    //   ReactEngine 传入本次请求使用的工具定义,独立于 config_.tools
    // 这是主路径 (ReactEngine::call_llm 实际走的)
    core::Result<std::string> generate_stream_from_messages_with_tools(
        const std::vector<LLMMessage>& messages,
        const std::vector<core::JsonValue>& tools,
        std::function<void(const std::string&)> callback
    ) override {
        GenerateOptions options = build_generate_options(messages, &tools);
        return do_generate_stream(options, callback);
    }

private:
    // 🎯 对齐 OC: 统一的 options 构造
    // - 若 explicit_tools != nullptr && 非空 → 使用 explicit_tools (覆盖 config_.tools)
    // - 否则使用 config_.tools (兼容老路径)
    GenerateOptions build_generate_options(
        const std::vector<LLMMessage>& messages,
        const std::vector<core::JsonValue>* explicit_tools
    ) {
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

        // 🎯 对齐 OC: 工具配置 —— 优先 explicit_tools (主路径)
        const std::vector<core::JsonValue>* source_tools = nullptr;
        if (explicit_tools && !explicit_tools->empty()) {
            source_tools = explicit_tools;
        } else if (config_.supports_tool_calls && !config_.tools.empty()) {
            source_tools = &config_.tools;
        }

        if (source_tools) {
            for (const auto& tool_json : *source_tools) {
                if (!tool_json.is_object()) continue;
                if (!tool_json.contains("name") || !tool_json.contains("description")) {
                    continue;
                }
                ToolDefinition tool;
                tool.name = tool_json["name"].as_string();
                tool.description = tool_json["description"].as_string();
                if (tool_json.contains("input_schema")) {
                    tool.input_schema = tool_json["input_schema"];
                }
                options.tools.push_back(tool);
            }
            options.tool_choice = "auto";
        }

        return options;
    }

    core::Result<std::string> do_generate_stream(
        const GenerateOptions& options,
        std::function<void(const std::string&)> callback
    ) {
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

        return core::Result<std::string>::Ok(result.get().content);
    }

public:
    
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
// ErrorLLMClient - 未知/创建失败的 provider 的显式错误客户端
// 🎯 对齐 OpenClaw simple-completion-runtime.ts:
//   Unknown model → 返回 error（而非静默 Mock 假回复）。此前adapter 对
//   未知 provider / 创建失败静默降级 Mock，会用假回复掩盖配置错误。
//   改为返回本客户端：任何生成调用都返回明确 Err，让上层暴露真实错误。
// ============================================================================
class ErrorLLMClient : public LLMClient {
public:
    explicit ErrorLLMClient(std::string message, LLMConfig config)
        : message_(std::move(message)), config_(std::move(config)) {}

    core::Result<std::string> generate(const std::string&) override {
        return core::Result<std::string>::Err(message_);
    }
    core::Result<std::string> generate_from_messages(
        const std::vector<LLMMessage>&) override {
        return core::Result<std::string>::Err(message_);
    }
    core::Result<std::string> generate_stream(
        const std::string&, std::function<void(const std::string&)>) override {
        return core::Result<std::string>::Err(message_);
    }
    core::Result<std::string> generate_stream_from_messages(
        const std::vector<LLMMessage>&,
        std::function<void(const std::string&)>) override {
        return core::Result<std::string>::Err(message_);
    }
    core::Result<std::string> generate_stream_from_messages_with_tools(
        const std::vector<LLMMessage>&,
        const std::vector<core::JsonValue>&,
        std::function<void(const std::string&)>) override {
        return core::Result<std::string>::Err(message_);
    }
    const LLMConfig& get_config() const override { return config_; }
    void set_config(const LLMConfig& config) override { config_ = config; }
    int estimate_tokens(const std::string& text) const override {
        return static_cast<int>(text.size() / 4);
    }

private:
    std::string message_;
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
    // mock 显式保留（测试用途）。
    if (config.provider == "mock") {
        return MockLLMClient::create(config);
    }

    // 🎯 统一走 ProviderFactory::create —— 由工厂负责内置 provider
    //   (openai/anthropic/ollama/gemini) 与OpenAI 兼容 provider
    //   (deepseek/openrouter/groq/...) 的路由。此处不再硬编码分支，
    //   使adapter 与 ReactEngine 走同一套 provider 解析逻辑。
    ProviderConfig provider_config;
    provider_config.provider_id = (config.provider == "claude") ? "anthropic" : config.provider;
    provider_config.api_key = config.api_key;
    provider_config.base_url = config.endpoint;  // 空则工厂注入默认 endpoint
    provider_config.model = config.model;
    provider_config.max_tokens = config.max_tokens;
    provider_config.temperature = config.temperature;
    provider_config.top_p = config.top_p;
    provider_config.max_retries = config.max_retries;
    provider_config.retry_delay_ms = config.retry_delay_ms;

    // 默认 model 兜底（对齐此前 adapter 各 provider 分支的默认值）。
    if (provider_config.model.empty()) {
        if (provider_config.provider_id == "openai") provider_config.model = "gpt-4";
        else if (provider_config.provider_id == "anthropic") provider_config.model = "claude-3-5-sonnet-20241022";
        else if (provider_config.provider_id == "ollama") provider_config.model = "llama3.2";
        else if (provider_config.provider_id == "gemini") provider_config.model = "gemini-2.0-flash-exp";
    }
    // ollama 默认本地 endpoint（对齐此前 adapter 行为）。
    if (provider_config.provider_id == "ollama" && provider_config.base_url.empty()) {
        provider_config.base_url = "http://localhost:11434";
    }

    auto provider_result = ProviderFactory::create(provider_config);

    if (!provider_result.is_ok()) {
        // 🎯 对齐 OC: 未知/创建失败 → 返回显式错误客户端，而非静默 Mock。
        std::string msg = "LLM provider unavailable: " + provider_result.error +
            " (provider='" + config.provider + "', model='" + config.model + "')";
        if (auto lg = spdlog::get("default")) {
            lg->error("[LLMClient] {}", msg);
        }
        return std::make_shared<ErrorLLMClient>(msg, config);
    }

    return create_from_provider_impl(provider_result.get(), config);
}

} // namespace llm
} // namespace hunterclaw
