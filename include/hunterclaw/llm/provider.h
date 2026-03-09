#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>

namespace hunterclaw {
namespace llm {

// Message类型
enum class MessageRole {
    System,
    User,
    Assistant,
    Tool
};

struct Message {
    MessageRole role;
    std::string content;
    std::optional<std::string> name;  // 用于tool消息
    std::optional<std::string> tool_call_id;  // 用于tool response
};

// Tool Call结构
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;  // JSON字符串
};

// 流式响应块
struct StreamChunk {
    enum class Type {
        Text,
        ToolCall,
        Done
    };
    
    Type type;
    std::string text;  // type=Text时使用
    std::optional<ToolCall> tool_call;  // type=ToolCall时使用
};

// LLM响应
struct Response {
    std::string content;
    std::vector<ToolCall> tool_calls;
    
    struct Usage {
        int64_t input_tokens = 0;
        int64_t output_tokens = 0;
        int64_t cache_read_tokens = 0;
        int64_t cache_write_tokens = 0;
        
        struct Cost {
            double input = 0.0;
            double output = 0.0;
            double cache_read = 0.0;
            double cache_write = 0.0;
            double total = 0.0;
        } cost;
    } usage;
    
    std::string model;
    std::string finish_reason;  // "stop", "tool_calls", "length", etc.
};

// 模型配置
struct ModelConfig {
    std::string id;
    std::string name;
    bool reasoning = false;
    std::vector<std::string> input_types;  // "text", "image"
    
    struct CostConfig {
        double input = 0.0;       // per 1M tokens
        double output = 0.0;
        double cache_read = 0.0;
        double cache_write = 0.0;
    } cost;
    
    int64_t context_window = 128000;
    int64_t max_tokens = 4096;
};

// 生成参数
struct GenerateOptions {
    std::string model;
    std::vector<Message> messages;
    
    // 可选参数
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<double> top_p;
    std::optional<double> frequency_penalty;
    std::optional<double> presence_penalty;
    std::optional<std::vector<std::string>> stop;
    
    // Tool配置
    struct ToolConfig {
        std::string name;
        std::string description;
        std::string parameters;  // JSON Schema字符串
    };
    std::vector<ToolConfig> tools;
    
    // 流式回调
    std::function<void(const StreamChunk&)> on_chunk;
    bool stream = false;
};

// Provider接口
class Provider {
public:
    virtual ~Provider() = default;
    
    // 同步生成
    virtual Response generate(const GenerateOptions& options) = 0;
    
    // 流式生成
    virtual Response stream_generate(const GenerateOptions& options) = 0;
    
    // 获取支持的模型列表
    virtual std::vector<ModelConfig> list_models() = 0;
    
    // Provider名称
    virtual std::string name() const = 0;
};

// Provider工厂
class ProviderFactory {
public:
    static std::shared_ptr<Provider> create_openai(
        const std::string& api_key,
        const std::string& base_url = "https://api.openai.com/v1"
    );
    
    static std::shared_ptr<Provider> create_anthropic(
        const std::string& api_key,
        const std::string& base_url = "https://api.anthropic.com/v1"
    );
    
    static std::shared_ptr<Provider> create_ollama(
        const std::string& base_url = "http://localhost:11434"
    );
    
    static std::shared_ptr<Provider> create_custom(
        const std::string& name,
        const std::string& base_url,
        const std::string& api_key = ""
    );
};

// Provider管理器
class ProviderManager {
public:
    static std::shared_ptr<ProviderManager> create();
    
    // 注册Provider
    void register_provider(const std::string& name, std::shared_ptr<Provider> provider);
    
    // 获取Provider
    std::shared_ptr<Provider> get_provider(const std::string& name);
    
    // 列出所有Provider
    std::vector<std::string> list_providers() const;
    
    // 从配置创建Provider
    void load_from_config(const std::map<std::string, std::string>& config);

private:
    std::map<std::string, std::shared_ptr<Provider>> providers_;
};

} // namespace llm
} // namespace hunterclaw
