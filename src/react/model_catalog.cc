#include "hunterclaw/react/model_catalog.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace react {

namespace {
// Logger
auto log() {
    static auto logger = spdlog::get("react");
    if (!logger) {
        logger = spdlog::stdout_logger_mt("react");
    }
    return logger;
}
} // anonymous namespace

// ============================================================================
// ModelCatalog 实现
// ============================================================================

ModelCatalog::ModelCatalog() {
    register_builtin_models();
}

ModelCatalog& ModelCatalog::instance() {
    static ModelCatalog instance;
    return instance;
}

void ModelCatalog::register_model(const ModelCapabilities& caps) {
    std::string key = make_key(caps.provider, caps.model_id);
    catalog_[key] = caps;
    
    log()->info("[ModelCatalog] Registered model: {}/{} (reasoning={}, tools={})",
        caps.provider, caps.model_id, 
        caps.supports_reasoning, caps.supports_tool_use);
}

ModelCapabilities ModelCatalog::get_capabilities(
    const std::string& provider, 
    const std::string& model_id
) const {
    std::string key = make_key(provider, model_id);
    auto it = catalog_.find(key);
    
    if (it != catalog_.end()) {
        return it->second;
    }
    
    // 未找到,返回默认能力
    log()->warn("[ModelCatalog] Model not found: {}/{}, using defaults",
        provider, model_id);
    return create_default_capabilities(provider, model_id);
}

bool ModelCatalog::supports_reasoning(
    const std::string& provider, 
    const std::string& model_id
) const {
    auto caps = get_capabilities(provider, model_id);
    return caps.supports_reasoning;
}

std::vector<ModelCapabilities> ModelCatalog::list_all_models() const {
    std::vector<ModelCapabilities> models;
    models.reserve(catalog_.size());
    
    for (const auto& [key, caps] : catalog_) {
        models.push_back(caps);
    }
    
    return models;
}

void ModelCatalog::clear() {
    catalog_.clear();
    log()->info("[ModelCatalog] Cleared catalog");
}

bool ModelCatalog::load_from_config(const std::string& config_path) {
    try {
        std::ifstream file(config_path);
        if (!file.is_open()) {
            log()->error("[ModelCatalog] Failed to open config: {}", config_path);
            return false;
        }
        
        json j;
        file >> j;
        
        if (!j.contains("models") || !j["models"].is_array()) {
            log()->error("[ModelCatalog] Invalid config format");
            return false;
        }
        
        int count = 0;
        for (const auto& model_json : j["models"]) {
            ModelCapabilities caps;
            caps.provider = model_json.value("provider", "");
            caps.model_id = model_json.value("model_id", "");
            caps.supports_reasoning = model_json.value("supports_reasoning", false);
            caps.supports_tool_use = model_json.value("supports_tool_use", true);
            caps.supports_streaming = model_json.value("supports_streaming", true);
            caps.max_tokens = model_json.value("max_tokens", 4096);
            caps.context_window = model_json.value("context_window", 200000);
            
            if (!caps.provider.empty() && !caps.model_id.empty()) {
                register_model(caps);
                count++;
            }
        }
        
        log()->info("[ModelCatalog] Loaded {} models from config", count);
        return true;
        
    } catch (const std::exception& e) {
        log()->error("[ModelCatalog] Failed to load config: {}", e.what());
        return false;
    }
}

std::string ModelCatalog::make_key(const std::string& provider, const std::string& model_id) {
    return provider + "/" + model_id;
}

/**
 * 注册内置模型能力 (OpenClaw 对齐)
 * 
 * 基于 OpenClaw 的模型目录,标记支持 reasoning 的模型
 * 参考: openclaw-analysis/src/agents/model-selection.ts
 */
void ModelCatalog::register_builtin_models() {
    // ========== Anthropic Claude 系列 ==========
    
    // Claude Opus 4 系列 (支持 reasoning) ✅
    register_model({
        .provider = "anthropic",
        .model_id = "claude-opus-4-20250514",
        .supports_reasoning = true,  // ✅ 支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 16384,
        .context_window = 200000
    });
    
    register_model({
        .provider = "anthropic",
        .model_id = "claude-opus-4",
        .supports_reasoning = true,  // ✅ 支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 16384,
        .context_window = 200000
    });
    
    // Claude Sonnet 4 系列 (支持 reasoning) ✅
    register_model({
        .provider = "anthropic",
        .model_id = "claude-sonnet-4-20250514",
        .supports_reasoning = true,  // ✅ 支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 200000
    });
    
    register_model({
        .provider = "anthropic",
        .model_id = "claude-3-5-sonnet-20241022",
        .supports_reasoning = false,  // 旧版本不支持
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 200000
    });
    
    // ========== OpenAI GPT 系列 ==========
    
    // GPT-4o 系列 (部分支持 reasoning)
    register_model({
        .provider = "openai",
        .model_id = "gpt-4o",
        .supports_reasoning = false,  // 标准版不支持
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 4096,
        .context_window = 128000
    });
    
    register_model({
        .provider = "openai",
        .model_id = "gpt-4o-mini",
        .supports_reasoning = false,
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 4096,
        .context_window = 128000
    });
    
    // o1 系列 (支持 reasoning) ✅
    register_model({
        .provider = "openai",
        .model_id = "o1",
        .supports_reasoning = true,  // ✅ o1 系列支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 100000,
        .context_window = 200000
    });
    
    register_model({
        .provider = "openai",
        .model_id = "o1-preview",
        .supports_reasoning = true,  // ✅ o1 系列支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 32768,
        .context_window = 128000
    });
    
    register_model({
        .provider = "openai",
        .model_id = "o1-mini",
        .supports_reasoning = true,  // ✅ o1 系列支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 65536,
        .context_window = 128000
    });
    
    // ========== Google Gemini 系列 ==========
    
    // Gemini 2.0 系列 (支持 reasoning)
    register_model({
        .provider = "google",
        .model_id = "gemini-2.0-flash-thinking-exp",
        .supports_reasoning = true,  // ✅ thinking 模式支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 1000000
    });
    
    register_model({
        .provider = "google",
        .model_id = "gemini-2.0-flash-exp",
        .supports_reasoning = false,
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 1000000
    });
    
    // Gemini 1.5 系列
    register_model({
        .provider = "google",
        .model_id = "gemini-1.5-pro",
        .supports_reasoning = false,
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 2000000
    });
    
    register_model({
        .provider = "google",
        .model_id = "gemini-1.5-flash",
        .supports_reasoning = false,
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 1000000
    });
    
    // ========== Deepseek 系列 ==========
    
    // DeepSeek V3 (支持 reasoning)
    register_model({
        .provider = "deepseek",
        .model_id = "deepseek-chat",
        .supports_reasoning = false,
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 64000
    });
    
    register_model({
        .provider = "deepseek",
        .model_id = "deepseek-reasoner",
        .supports_reasoning = true,  // ✅ reasoner 模式支持推理
        .supports_tool_use = true,
        .supports_streaming = true,
        .max_tokens = 8192,
        .context_window = 64000
    });
    
    log()->info("[ModelCatalog] Registered {} builtin models", catalog_.size());
}

// ============================================================================
// 辅助函数实现
// ============================================================================

void register_common_models() {
    // 全局单例已经在构造函数中注册了
    // 这个函数用于显式触发注册
    ModelCatalog::instance();
}

ModelCapabilities create_default_capabilities(
    const std::string& provider,
    const std::string& model_id
) {
    ModelCapabilities caps;
    caps.provider = provider;
    caps.model_id = model_id;
    caps.supports_reasoning = false;  // 默认不支持推理
    caps.supports_tool_use = true;
    caps.supports_streaming = true;
    caps.max_tokens = 4096;
    caps.context_window = 128000;
    
    return caps;
}

} // namespace react
} // namespace hunterclaw
