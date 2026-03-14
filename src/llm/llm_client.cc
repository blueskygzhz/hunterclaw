#include "hunterclaw/llm/llm_client.h"
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace llm {

// ============================================================================
// MockLLMClient Implementation
// ============================================================================

LLMClient::Ptr MockLLMClient::create(const LLMConfig& config) {
    return std::make_shared<MockLLMClient>(config);
}

MockLLMClient::MockLLMClient(const LLMConfig& config)
    : config_(config)
{}

core::Result<std::string> MockLLMClient::generate(const std::string& prompt) {
    // 简单的mock实现
    std::ostringstream response;
    response << "Thought: I need to analyze this query and decide on an action.\n";
    response << "Action: search\n";
    response << "Action Input: information about the query\n";
    
    return core::Result<std::string>::Ok(response.str());
}

core::Result<std::string> MockLLMClient::generate_from_messages(
    const std::vector<LLMMessage>& messages
) {
    // 简化实现: 提取最后一条用户消息
    std::string last_user_msg;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == LLMMessageRole::User) {
            last_user_msg = it->content;
            break;
        }
    }
    
    return generate(last_user_msg);
}

core::Result<std::string> MockLLMClient::generate_stream(
    const std::string& prompt,
    std::function<void(const std::string&)> callback
) {
    auto result = generate(prompt);
    if (result.is_ok() && callback) {
        // 模拟流式输出
        const auto& text = result.value;
        for (size_t i = 0; i < text.size(); i += 10) {
            std::string chunk = text.substr(i, std::min(size_t(10), text.size() - i));
            callback(chunk);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    return result;
}

int MockLLMClient::estimate_tokens(const std::string& text) const {
    // 简单估算: ~4字符 = 1 token
    return static_cast<int>(text.size() / 4);
}

// ============================================================================
// OpenAI Integration (已迁移到 Provider 接口)
// ============================================================================
// OpenAI 的完整实现见 openai_provider.cc (使用 Provider 接口)
// LLMClient 应该内部使用 Provider，而不是每个厂商实现一个 Client 子类
// 使用示例：
//   auto provider = ProviderFactory::create_openai(api_key, base_url);
//   provider->generate(options);

// ============================================================================
// AnthropicClient Implementation (Placeholder)
// ============================================================================

LLMClient::Ptr AnthropicClient::create(const LLMConfig& config) {
    return std::make_shared<AnthropicClient>(config);
}

AnthropicClient::AnthropicClient(const LLMConfig& config)
    : config_(config)
{}

core::Result<std::string> AnthropicClient::generate(const std::string& prompt) {
    // TODO: 实现真实的Anthropic API调用
    std::ostringstream response;
    response << "[Anthropic/Claude Response]\n";
    response << "Prompt: " << prompt.substr(0, 100) << "...\n";
    response << "\nNOTE: Anthropic API integration not yet implemented.\n";
    response << "To integrate, use libcurl or HTTP client to call:\n";
    response << "POST https://api.anthropic.com/v1/messages\n";
    
    return core::Result<std::string>::Ok(response.str());
}

core::Result<std::string> AnthropicClient::generate_from_messages(
    const std::vector<LLMMessage>& messages
) {
    // TODO: 实现消息API
    return generate("Combined message context");
}

core::Result<std::string> AnthropicClient::generate_stream(
    const std::string& prompt,
    std::function<void(const std::string&)> callback
) {
    return generate(prompt);
}

int AnthropicClient::estimate_tokens(const std::string& text) const {
    // Claude的token估算
    return static_cast<int>(text.size() / 3.5);
}

core::Result<std::string> AnthropicClient::send_request(const core::JsonValue& payload) {
    // TODO: 实现HTTP请求
    return core::Result<std::string>::Err("HTTP client not implemented");
}

// ============================================================================
// LLMClientFactory Implementation
// ============================================================================
// 注意: 实际实现已迁移到 llm_client_provider_adapter.cc
// 该文件实现了 LLMClient 到 Provider 的适配

std::map<std::string, std::function<LLMClient::Ptr(const LLMConfig&)>> 
    LLMClientFactory::providers_;

// 声明外部实现 (定义在 llm_client_provider_adapter.cc)
extern LLMClient::Ptr create_llm_client_impl(const LLMConfig& config);
extern LLMClient::Ptr create_from_provider_impl(
    std::shared_ptr<llm::Provider> provider,
    const LLMConfig& config
);

LLMClient::Ptr LLMClientFactory::create(const LLMConfig& config) {
    // 检查自定义provider
    if (providers_.count(config.provider)) {
        return providers_[config.provider](config);
    }
    
    // ✅ 使用新的 Provider 接口实现
    // 实际逻辑在 llm_client_provider_adapter.cc
    return create_llm_client_impl(config);
}

LLMClient::Ptr LLMClientFactory::create_from_provider(
    std::shared_ptr<llm::Provider> provider,
    const LLMConfig& config
) {
    return create_from_provider_impl(provider, config);
}

void LLMClientFactory::register_provider(
    const std::string& name,
    std::function<LLMClient::Ptr(const LLMConfig&)> factory
) {
    providers_[name] = factory;
}

} // namespace llm
} // namespace hunterclaw
