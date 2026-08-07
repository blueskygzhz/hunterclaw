#include "hunterclaw/llm/provider.h"
#include <spdlog/spdlog.h>
#include <cstdlib>

namespace hunterclaw {
namespace llm {

// 外部 Provider 工厂函数声明
extern core::Result<Provider::Ptr> create_openai_provider(const ProviderConfig& config);
extern core::Result<Provider::Ptr> create_anthropic_provider(const ProviderConfig& config);
extern core::Result<Provider::Ptr> create_ollama_provider(const ProviderConfig& config);
extern core::Result<Provider::Ptr> create_gemini_provider(const ProviderConfig& config);

// ============================================================================
// Provider Factory 实现
// ============================================================================

std::map<std::string, std::function<core::Result<Provider::Ptr>(const ProviderConfig&)>> 
ProviderFactory::providers_;

core::Result<Provider::Ptr> ProviderFactory::create(const ProviderConfig& config) {
    // 查找已注册的 Provider
    auto it = providers_.find(config.provider_id);
    if (it != providers_.end()) {
        return it->second(config);
    }
    
    // 内置 Provider
    if (config.provider_id == "openai") {
        return create_openai_provider(config);
    } else if (config.provider_id == "anthropic") {
        return create_anthropic_provider(config);
    } else if (config.provider_id == "ollama") {
        return create_ollama_provider(config);
    } else if (config.provider_id == "gemini") {
        return create_gemini_provider(config);
    }

    // 🎯 对齐 OpenClaw: 按"API 类型"而非 provider 名组织 — 大量 provider 走
    //   OpenAI Chat Completions 兼容 API（openai-completions）。此处把这些
    //   OpenAI 兼容 provider 路由到 OpenAI provider，并在未显式配置 base_url 时
    //   注入其官方默认 endpoint（对齐 models_config.cc resolve_implicit_providers 表）。
    //   这样 deepseek/openrouter/groq/... 不再落到 Unknown → 静默 Mock 的断链。
    {
        static const std::map<std::string, std::string> kOpenAICompatBaseUrls = {
            {"deepseek",   "https://api.deepseek.com"},
            {"openrouter", "https://openrouter.ai/api/v1"},
            {"groq",       "https://api.groq.com/openai/v1"},
            {"mistral",    "https://api.mistral.ai/v1"},
            {"xai",        "https://api.x.ai/v1"},
            {"grok",       "https://api.x.ai/v1"},
            {"together",   "https://api.together.xyz/v1"},
            {"cerebras",   "https://api.cerebras.ai/v1"},
            {"perplexity", "https://api.perplexity.ai"},
            {"fireworks",  "https://api.fireworks.ai/inference/v1"},
            {"moonshot",   "https://api.moonshot.ai/v1"},
            {"kimi",       "https://api.moonshot.ai/v1"},
            {"zai",        "https://api.z.ai/api/paas/v4"},
            {"qwen",       "https://dashscope.aliyuncs.com/compatible-mode/v1"},
            {"dashscope",  "https://dashscope.aliyuncs.com/compatible-mode/v1"},
            {"novita",     "https://api.novita.ai/v3/openai"},
        };
        auto compat_it = kOpenAICompatBaseUrls.find(config.provider_id);
        if (compat_it != kOpenAICompatBaseUrls.end()) {
            ProviderConfig oai = config;
            // 保留调用方显式指定的 base_url；否则用官方默认 endpoint。
            if (oai.base_url.empty()) oai.base_url = compat_it->second;
            // OpenAI 兼容家族统一走 Chat Completions API。
            oai.api_type = ProviderAPI::OpenAICompletions;
            oai.supports_tool_calls = true;
            oai.streaming_enabled = true;
            if (auto lg = spdlog::get("default")) {
                lg->info("[ProviderFactory] Routing OpenAI-compatible provider '{}' -> openai (base_url={})",
                         config.provider_id, oai.base_url);
            }
            return create_openai_provider(oai);
        }
    }
    
    return core::Result<Provider::Ptr>::Err("Unknown provider: " + config.provider_id);
}

core::Result<Provider::Ptr> ProviderFactory::create_openai(
    const std::string& api_key,
    const std::string& model,
    const std::string& base_url
) {
    ProviderConfig config;
    config.provider_id = "openai";
    config.model = model;
    config.api_key = api_key;
    config.base_url = base_url;
    config.api_type = ProviderAPI::OpenAIResponses;
    config.supports_tool_calls = true;
    config.streaming_enabled = true;
    config.context_window = 128000;
    
    return create(config);
}

core::Result<Provider::Ptr> ProviderFactory::create_anthropic(
    const std::string& api_key,
    const std::string& model,
    const std::string& base_url
) {
    ProviderConfig config;
    config.provider_id = "anthropic";
    config.model = model;
    config.api_key = api_key;
    config.base_url = base_url;
    config.api_type = ProviderAPI::AnthropicMessages;
    config.supports_tool_calls = true;
    config.streaming_enabled = true;
    config.context_window = 200000;
    
    return create(config);
}

core::Result<Provider::Ptr> ProviderFactory::create_ollama(
    const std::string& model,
    const std::string& base_url
) {
    ProviderConfig config;
    config.provider_id = "ollama";
    config.model = model;
    config.base_url = base_url;
    config.api_type = ProviderAPI::Ollama;
    config.supports_tool_calls = true;
    config.streaming_enabled = true;
    config.context_window = 128000;
    
    return create(config);
}

core::Result<Provider::Ptr> ProviderFactory::create_gemini(
    const std::string& api_key,
    const std::string& model
) {
    ProviderConfig config;
    config.provider_id = "gemini";
    config.model = model;
    config.api_key = api_key;
    config.base_url = "https://generativelanguage.googleapis.com";
    config.api_type = ProviderAPI::GoogleGenerativeAI;
    config.supports_tool_calls = true;
    config.streaming_enabled = true;
    
    // Gemini 2.0 Flash 有超长上下文
    if (model.find("gemini-2.0") != std::string::npos) {
        config.context_window = 1048576;  // 1M tokens
    } else if (model.find("gemini-1.5-pro") != std::string::npos) {
        config.context_window = 2097152;  // 2M tokens
    } else {
        config.context_window = 32000;
    }
    
    return create(config);
}

void ProviderFactory::register_provider(
    const std::string& provider_id,
    std::function<core::Result<Provider::Ptr>(const ProviderConfig&)> factory
) {
    providers_[provider_id] = factory;
}

std::vector<ProviderConfig> ProviderFactory::discover_providers() {
    std::vector<ProviderConfig> configs;
    
    // 检查环境变量 (对齐 OpenClaw resolveImplicitProviders)
    
    // 1. OpenAI
    const char* openai_key = std::getenv("OPENAI_API_KEY");
    if (openai_key && strlen(openai_key) > 0) {
        ProviderConfig config;
        config.provider_id = "openai";
        config.model = "gpt-4";
        config.api_key = openai_key;
        config.base_url = "https://api.openai.com/v1";
        config.api_type = ProviderAPI::OpenAIResponses;
        config.supports_tool_calls = true;
        configs.push_back(config);
        
    }
    
    // 2. Anthropic
    const char* anthropic_key = std::getenv("ANTHROPIC_API_KEY");
    if (anthropic_key && strlen(anthropic_key) > 0) {
        ProviderConfig config;
        config.provider_id = "anthropic";
        config.model = "claude-3-5-sonnet-20241022";
        config.api_key = anthropic_key;
        config.base_url = "https://api.anthropic.com/v1";
        config.api_type = ProviderAPI::AnthropicMessages;
        config.supports_tool_calls = true;
        configs.push_back(config);
        
    }
    
    // 3. Ollama
    const char* ollama_url = std::getenv("OLLAMA_BASE_URL");
    std::string ollama_base = ollama_url ? ollama_url : "http://127.0.0.1:11434";
    
    // 简单检测 Ollama 是否可用 (TODO: 实际发送 HTTP 请求检测)
    ProviderConfig ollama_config;
    ollama_config.provider_id = "ollama";
    ollama_config.model = "llama3.2";
    ollama_config.base_url = ollama_base;
    ollama_config.api_type = ProviderAPI::Ollama;
    ollama_config.supports_tool_calls = true;
    configs.push_back(ollama_config);
    
    
    // 4. Gemini
    const char* gemini_key = std::getenv("GOOGLE_API_KEY");
    if (gemini_key && strlen(gemini_key) > 0) {
        ProviderConfig config;
        config.provider_id = "gemini";
        config.model = "gemini-2.0-flash-exp";
        config.api_key = gemini_key;
        config.base_url = "https://generativelanguage.googleapis.com";
        config.api_type = ProviderAPI::GoogleGenerativeAI;
        config.supports_tool_calls = true;
        configs.push_back(config);
        
    }
    
    return configs;
}

} // namespace llm
} // namespace hunterclaw
