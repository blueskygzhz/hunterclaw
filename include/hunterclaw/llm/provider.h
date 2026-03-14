#pragma once

#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>

namespace hunterclaw {
namespace llm {

// ============================================================================
// Provider Types (对齐 OpenClaw MODEL_APIS)
// ============================================================================

enum class ProviderAPI {
    OpenAICompletions,        // openai-completions (OpenAI 兼容)
    OpenAIResponses,          // openai-responses (新版 OpenAI)
    AnthropicMessages,        // anthropic-messages (Claude)
    GoogleGenerativeAI,       // google-generative-ai (Gemini)
    Ollama,                   // ollama (本地推理)
    BedrockConverseStream,    // bedrock-converse-stream (AWS Bedrock)
    GithubCopilot            // github-copilot
};

// 将字符串转换为 ProviderAPI
inline ProviderAPI api_from_string(const std::string& api) {
    static const std::map<std::string, ProviderAPI> mapping = {
        {"openai-completions", ProviderAPI::OpenAICompletions},
        {"openai-responses", ProviderAPI::OpenAIResponses},
        {"anthropic-messages", ProviderAPI::AnthropicMessages},
        {"google-generative-ai", ProviderAPI::GoogleGenerativeAI},
        {"ollama", ProviderAPI::Ollama},
        {"bedrock-converse-stream", ProviderAPI::BedrockConverseStream},
        {"github-copilot", ProviderAPI::GithubCopilot},
    };
    
    auto it = mapping.find(api);
    if (it != mapping.end()) {
        return it->second;
    }
    return ProviderAPI::OpenAICompletions;  // 默认
}

// ============================================================================
// Message Types
// ============================================================================

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct ToolCall {
    std::string id;           // 工具调用 ID
    std::string name;         // 工具名称
    core::JsonValue input;    // 工具输入参数
};

struct Message {
    MessageRole role;
    std::string content;
    
    // Tool calling 支持
    std::vector<ToolCall> tool_calls;  // Assistant 发起的工具调用
    std::string tool_call_id;          // Tool 消息的关联 ID
    std::string name;                  // 工具名称
    
    Message() : role(MessageRole::User) {}
    
    static Message system(const std::string& content) {
        Message msg;
        msg.role = MessageRole::System;
        msg.content = content;
        return msg;
    }
    
    static Message user(const std::string& content) {
        Message msg;
        msg.role = MessageRole::User;
        msg.content = content;
        return msg;
    }
    
    static Message assistant(const std::string& content) {
        Message msg;
        msg.role = MessageRole::Assistant;
        msg.content = content;
        return msg;
    }
    
    static Message tool_result(
        const std::string& content,
        const std::string& tool_call_id,
        const std::string& name
    ) {
        Message msg;
        msg.role = MessageRole::Tool;
        msg.content = content;
        msg.tool_call_id = tool_call_id;
        msg.name = name;
        return msg;
    }
};

// ============================================================================
// Tool Definition (对齐 OpenClaw Tool Schema)
// ============================================================================

struct ToolDefinition {
    std::string name;
    std::string description;
    core::JsonValue input_schema;  // JSON Schema 格式的输入定义
    
    // OpenAI format 转换
    core::JsonValue to_openai_format() const;
    
    // Anthropic format 转换
    core::JsonValue to_anthropic_format() const;
};

// ============================================================================
// Provider Configuration (对齐 OpenClaw ModelProviderConfig)
// ============================================================================

struct ProviderConfig {
    std::string provider_id;       // "openai", "anthropic", "ollama", etc.
    std::string model;              // 模型名称 (如 "gpt-4", "claude-3.5-sonnet")
    std::string api_key;            // API 密钥
    std::string base_url;           // API 端点
    ProviderAPI api_type;           // API 类型
    
    // 自定义请求头
    std::map<std::string, std::string> headers;
    
    // 生成参数
    int max_tokens = 4096;
    double temperature = 0.7;
    double top_p = 1.0;
    int top_k = 0;
    std::vector<std::string> stop_sequences;
    
    // 上下文窗口
    int context_window = 200000;
    
    // Tool calling 支持
    bool supports_tool_calls = false;
    
    // 重试配置
    int max_retries = 3;
    int retry_delay_ms = 1000;
    
    // 流式配置
    bool streaming_enabled = true;
};

// ============================================================================
// Generation Options
// ============================================================================

struct GenerateOptions {
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    
    // 覆盖 ProviderConfig 中的参数
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    std::optional<double> top_p;
    std::optional<int> top_k;
    std::optional<std::vector<std::string>> stop_sequences;
    
    // Tool choice
    std::optional<std::string> tool_choice;  // "auto", "none", 或具体工具名
};

// ============================================================================
// Generation Response
// ============================================================================

struct GenerateResponse {
    std::string content;               // 生成的文本内容
    std::vector<ToolCall> tool_calls;  // 工具调用列表
    
    // Token 使用统计
    int input_tokens = 0;
    int output_tokens = 0;
    int cache_read_tokens = 0;
    int cache_write_tokens = 0;
    
    // 原始 API 响应 (用于调试)
    core::JsonValue raw_response;
};

// ============================================================================
// Stream Chunk (流式响应块)
// ============================================================================

struct StreamChunk {
    std::string delta;                 // 增量文本
    bool is_tool_call = false;         // 是否为工具调用
    std::optional<ToolCall> tool_call; // 工具调用信息 (如果有)
    bool is_final = false;             // 是否为最后一个块
};

// ============================================================================
// LLM Provider Interface (核心抽象)
// ============================================================================

class Provider {
public:
    using Ptr = std::shared_ptr<Provider>;
    using StreamCallback = std::function<void(const StreamChunk&)>;
    
    virtual ~Provider() = default;
    
    /// 生成文本 (同步)
    /// @param options 生成选项
    /// @return 生成结果或错误
    virtual core::Result<GenerateResponse> generate(const GenerateOptions& options) = 0;
    
    /// 生成文本 (流式)
    /// @param options 生成选项
    /// @param callback 流式回调
    /// @return 完整生成结果或错误
    virtual core::Result<GenerateResponse> generate_stream(
        const GenerateOptions& options,
        StreamCallback callback
    ) = 0;
    
    /// 估算 token 数量
    /// @param text 文本
    /// @return token 数量
    virtual int estimate_tokens(const std::string& text) const = 0;
    
    /// 获取 Provider 配置
    virtual const ProviderConfig& config() const = 0;
    
    /// 获取 Provider ID
    virtual std::string provider_id() const = 0;
    
    /// 获取 API 类型
    virtual ProviderAPI api_type() const = 0;
    
    /// 是否支持 tool calling
    virtual bool supports_tools() const = 0;
    
    /// 是否支持流式
    virtual bool supports_streaming() const = 0;
};

// ============================================================================
// Provider Factory (对齐 OpenClaw resolveImplicitProviders)
// ============================================================================

class ProviderFactory {
public:
    /// 创建 Provider 实例
    /// @param config Provider 配置
    /// @return Provider 实例或错误
    static core::Result<Provider::Ptr> create(const ProviderConfig& config);
    
    /// 创建 OpenAI Provider
    /// @param api_key API 密钥
    /// @param model 模型名称
    /// @param base_url 可选的自定义端点
    /// @return Provider 实例或错误
    static core::Result<Provider::Ptr> create_openai(
        const std::string& api_key,
        const std::string& model = "gpt-4",
        const std::string& base_url = "https://api.openai.com/v1"
    );
    
    /// 创建 Anthropic Provider
    /// @param api_key API 密钥
    /// @param model 模型名称
    /// @param base_url 可选的自定义端点
    /// @return Provider 实例或错误
    static core::Result<Provider::Ptr> create_anthropic(
        const std::string& api_key,
        const std::string& model = "claude-3-5-sonnet-20241022",
        const std::string& base_url = "https://api.anthropic.com/v1"
    );
    
    /// 创建 Ollama Provider
    /// @param model 模型名称
    /// @param base_url Ollama 服务地址
    /// @return Provider 实例或错误
    static core::Result<Provider::Ptr> create_ollama(
        const std::string& model = "llama3.2",
        const std::string& base_url = "http://127.0.0.1:11434"
    );
    
    /// 创建 Gemini Provider
    /// @param api_key API 密钥
    /// @param model 模型名称
    /// @return Provider 实例或错误
    static core::Result<Provider::Ptr> create_gemini(
        const std::string& api_key,
        const std::string& model = "gemini-2.0-flash-exp"
    );
    
    /// 注册自定义 Provider
    /// @param provider_id Provider ID
    /// @param factory 工厂函数
    static void register_provider(
        const std::string& provider_id,
        std::function<core::Result<Provider::Ptr>(const ProviderConfig&)> factory
    );
    
    /// 从环境变量自动发现 Provider
    /// @return 发现的 Provider 列表
    static std::vector<ProviderConfig> discover_providers();

private:
    static std::map<std::string, std::function<core::Result<Provider::Ptr>(const ProviderConfig&)>> providers_;
};

// ============================================================================
// Provider Capabilities (对齐 OpenClaw provider-capabilities.ts)
// ============================================================================

struct ProviderCapabilities {
    bool supports_tools = false;
    bool supports_streaming = false;
    bool supports_vision = false;
    bool supports_json_mode = false;
    bool supports_thinking = false;  // Anthropic thinking blocks
    
    // Tool schema 格式
    enum class ToolSchemaMode {
        Native,            // 原生格式
        OpenAIFunctions    // OpenAI functions 格式
    } tool_schema_mode = ToolSchemaMode::Native;
    
    // Tool choice 模式
    enum class ToolChoiceMode {
        Native,            // 原生格式
        OpenAIStringModes  // OpenAI string modes (auto/none/required)
    } tool_choice_mode = ToolChoiceMode::Native;
    
    // Provider 家族
    enum class ProviderFamily {
        Default,
        OpenAI,
        Anthropic,
        Google
    } provider_family = ProviderFamily::Default;
};

/// 获取 Provider 能力
/// @param provider_id Provider ID
/// @return Provider 能力
ProviderCapabilities get_provider_capabilities(const std::string& provider_id);

} // namespace llm
} // namespace hunterclaw
