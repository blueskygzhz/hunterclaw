#include "hunterclaw/memory/embedding_provider.h"
#include <sstream>
#include <cmath>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Helper: Vector normalization
// ============================================================================

std::vector<float> normalize_embedding(const std::vector<float>& vec) {
    double sum_sq = 0.0;
    for (float v : vec) {
        if (!std::isfinite(v)) {
            // Sanitize: replace NaN/Inf with 0
            continue;
        }
        sum_sq += v * v;
    }
    
    if (sum_sq == 0.0) {
        return std::vector<float>(vec.size(), 0.0f);
    }
    
    float norm = std::sqrt(sum_sq);
    std::vector<float> normalized;
    normalized.reserve(vec.size());
    
    for (float v : vec) {
        if (!std::isfinite(v)) {
            normalized.push_back(0.0f);
        } else {
            normalized.push_back(v / norm);
        }
    }
    
    return normalized;
}

// ============================================================================
// OpenAI Provider
// ============================================================================

OpenAIEmbeddingProvider::OpenAIEmbeddingProvider(const OpenAIConfig& config)
    : config_(config) {}

std::string OpenAIEmbeddingProvider::id() const {
    return "openai";
}

std::string OpenAIEmbeddingProvider::model() const {
    return config_.model;
}

std::optional<int> OpenAIEmbeddingProvider::max_input_tokens() const {
    return config_.max_input_tokens;
}

core::Result<std::vector<float>> OpenAIEmbeddingProvider::embed_query(const std::string& text) {
    // 构建请求 (对齐 OpenClaw embeddings-openai.ts)
    json request_body = {
        {"model", config_.model},
        {"input", text}
    };
    
    if (config_.dimensions > 0) {
        request_body["dimensions"] = config_.dimensions;
    }
    
    // 设置headers
    std::map<std::string, std::string> headers = config_.headers;
    headers["Authorization"] = "Bearer " + config_.api_key;
    headers["Content-Type"] = "application/json";
    
    // 发送HTTP请求
    std::string url = config_.base_url + "/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return Result<std::vector<float>>::Err("OpenAI API error: " + result.error);
    }
    
    // 解析响应 (对齐 OpenClaw格式)
    try {
        const auto& resp_json = result.value();
        
        // 响应格式: {"data": [{"embedding": [0.1, 0.2, ...]}]}
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return Result<std::vector<float>>::Err("Invalid OpenAI response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return Result<std::vector<float>>::Err("Empty embeddings in response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return Result<std::vector<float>>::Err("No embedding field in response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        // Normalize embedding
        return Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return Result<std::vector<float>>::Err(
            std::string("Failed to parse OpenAI response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> OpenAIEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    // 构建批量请求 (对齐 OpenClaw)
    json request_body = {
        {"model", config_.model},
        {"input", texts}
    };
    
    if (config_.dimensions > 0) {
        request_body["dimensions"] = config_.dimensions;
    }
    
    // 设置headers
    std::map<std::string, std::string> headers = config_.headers;
    headers["Authorization"] = "Bearer " + config_.api_key;
    headers["Content-Type"] = "application/json";
    
    // 发送HTTP请求
    std::string url = config_.base_url + "/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 60000);
    
    if (!result.is_ok()) {
        return Result<std::vector<std::vector<float>>>::Err(
            "OpenAI batch API error: " + result.error
        );
    }
    
    // 解析响应
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return Result<std::vector<std::vector<float>>>::Err(
                "Invalid OpenAI batch response format"
            );
        }
        
        const auto& data = resp_json["data"];
        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(data.size());
        
        for (const auto& item : data) {
            if (!item.contains("embedding")) {
                return Result<std::vector<std::vector<float>>>::Err(
                    "Missing embedding in response item"
                );
            }
            
            std::vector<float> embedding = item["embedding"].get<std::vector<float>>();
            embeddings.push_back(normalize_embedding(embedding));
        }
        
        return Result<std::vector<std::vector<float>>>::Ok(embeddings);
        
    } catch (const std::exception& e) {
        return Result<std::vector<std::vector<float>>>::Err(
            std::string("Failed to parse OpenAI batch response: ") + e.what()
        );
    }
}

// ============================================================================
// Ollama Provider
// ============================================================================

OllamaEmbeddingProvider::OllamaEmbeddingProvider(const OllamaConfig& config)
    : config_(config) {}

std::string OllamaEmbeddingProvider::id() const {
    return "ollama";
}

std::string OllamaEmbeddingProvider::model() const {
    return config_.model;
}

std::optional<int> OllamaEmbeddingProvider::max_input_tokens() const {
    return config_.max_input_tokens;
}

core::Result<std::vector<float>> OllamaEmbeddingProvider::embed_query(const std::string& text) {
    // 构建Ollama请求 (对齐 embeddings-ollama.ts)
    json request_body = {
        {"model", config_.model},
        {"prompt", text}
    };
    
    std::map<std::string, std::string> headers = {
        {"Content-Type", "application/json"}
    };
    
    // Ollama API endpoint
    std::string url = config_.base_url + "/api/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return Result<std::vector<float>>::Err("Ollama API error: " + result.error);
    }
    
    // 解析响应: {"embedding": [0.1, 0.2, ...]}
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("embedding")) {
            return Result<std::vector<float>>::Err("No embedding field in Ollama response");
        }
        
        std::vector<float> embedding = resp_json["embedding"].get<std::vector<float>>();
        
        return Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return Result<std::vector<float>>::Err(
            std::string("Failed to parse Ollama response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> OllamaEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());
    
    for (const auto& text : texts) {
        auto res = embed_query(text);
        if (!res.is_ok()) {
            return Result<std::vector<std::vector<float>>>::Err(res.error_message());
        }
        results.push_back(res.value());
    }
    
    return Result<std::vector<std::vector<float>>>::Ok(results);
}

// ============================================================================
// Gemini Provider
// ============================================================================

GeminiEmbeddingProvider::GeminiEmbeddingProvider(const GeminiConfig& config)
    : config_(config) {}

std::string GeminiEmbeddingProvider::id() const {
    return "gemini";
}

std::string GeminiEmbeddingProvider::model() const {
    return config_.model;
}

std::optional<int> GeminiEmbeddingProvider::max_input_tokens() const {
    return config_.max_input_tokens;
}

core::Result<std::vector<float>> GeminiEmbeddingProvider::embed_query(const std::string& text) {
    // 构建Gemini请求 (对齐 embeddings-gemini.ts)
    json request_body = {
        {"model", "models/" + config_.model},
        {"content", {
            {"parts", {{{"text", text}}}}
        }}
    };
    
    // 添加task_type和outputDimensionality
    if (!config_.task_type.empty()) {
        request_body["taskType"] = config_.task_type;
    }
    if (config_.output_dimensionality > 0) {
        request_body["outputDimensionality"] = config_.output_dimensionality;
    }
    
    std::map<std::string, std::string> headers = config_.headers;
    headers["Content-Type"] = "application/json";
    
    // Gemini API endpoint (含API key)
    std::string url = config_.base_url + "/models/" + config_.model + ":embedContent";
    if (!config_.api_key.empty()) {
        url += "?key=" + config_.api_key;
    }
    
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return Result<std::vector<float>>::Err("Gemini API error: " + result.error);
    }
    
    // 解析响应: {"embedding": {"values": [0.1, 0.2, ...]}}
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("embedding") || !resp_json["embedding"].contains("values")) {
            return Result<std::vector<float>>::Err("Invalid Gemini response format");
        }
        
        std::vector<float> embedding = resp_json["embedding"]["values"].get<std::vector<float>>();
        
        return Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return Result<std::vector<float>>::Err(
            std::string("Failed to parse Gemini response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> GeminiEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    return Result<std::vector<std::vector<float>>>::Err("Gemini batch not yet implemented");
}

// ============================================================================
// Voyage Provider
// ============================================================================

VoyageEmbeddingProvider::VoyageEmbeddingProvider(const VoyageConfig& config)
    : config_(config) {}

std::string VoyageEmbeddingProvider::id() const {
    return "voyage";
}

std::string VoyageEmbeddingProvider::model() const {
    return config_.model;
}

std::optional<int> VoyageEmbeddingProvider::max_input_tokens() const {
    return config_.max_input_tokens;
}

core::Result<std::vector<float>> VoyageEmbeddingProvider::embed_query(const std::string& text) {
    // 构建Voyage请求 (对齐 embeddings-voyage.ts)
    json request_body = {
        {"model", config_.model},
        {"input", json::array({text})},
        {"input_type", "query"}
    };
    
    std::map<std::string, std::string> headers = config_.headers;
    headers["Authorization"] = "Bearer " + config_.api_key;
    headers["Content-Type"] = "application/json";
    
    // Voyage API endpoint
    std::string url = config_.base_url + "/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return Result<std::vector<float>>::Err("Voyage API error: " + result.error);
    }
    
    // 解析响应: {data: [{embedding: [0.1, 0.2, ...]}]}
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return Result<std::vector<float>>::Err("Invalid Voyage response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return Result<std::vector<float>>::Err("Empty data in Voyage response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return Result<std::vector<float>>::Err("No embedding field in Voyage response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        return Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return Result<std::vector<float>>::Err(
            std::string("Failed to parse Voyage response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> VoyageEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    return Result<std::vector<std::vector<float>>>::Err("Voyage batch not yet implemented");
}

// ============================================================================
// Mistral Provider
// ============================================================================

MistralEmbeddingProvider::MistralEmbeddingProvider(const MistralConfig& config)
    : config_(config) {}

std::string MistralEmbeddingProvider::id() const {
    return "mistral";
}

std::string MistralEmbeddingProvider::model() const {
    return config_.model;
}

std::optional<int> MistralEmbeddingProvider::max_input_tokens() const {
    return config_.max_input_tokens;
}

core::Result<std::vector<float>> MistralEmbeddingProvider::embed_query(const std::string& text) {
    // 构建Mistral请求 (对齐 embeddings-mistral.ts)
    json request_body = {
        {"model", config_.model},
        {"input", json::array({text})}
    };
    
    std::map<std::string, std::string> headers = config_.headers;
    headers["Authorization"] = "Bearer " + config_.api_key;
    headers["Content-Type"] = "application/json";
    
    // Mistral API endpoint
    std::string url = config_.base_url + "/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return Result<std::vector<float>>::Err("Mistral API error: " + result.error);
    }
    
    // 解析响应 (OpenAI格式兼容): {data: [{embedding: [...]}]}
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return Result<std::vector<float>>::Err("Invalid Mistral response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return Result<std::vector<float>>::Err("Empty data in Mistral response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return Result<std::vector<float>>::Err("No embedding field in Mistral response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        return Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return Result<std::vector<float>>::Err(
            std::string("Failed to parse Mistral response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> MistralEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    // 构建批量请求 (对齐 Mistral API格式)
    json request_body = {
        {"model", config_.model},
        {"input", texts}
    };
    
    std::map<std::string, std::string> headers = config_.headers;
    headers["Authorization"] = "Bearer " + config_.api_key;
    headers["Content-Type"] = "application/json";
    
    std::string url = config_.base_url + "/embeddings";
    auto result = HttpClient::post_json(url, request_body, headers, 60000);
    
    if (!result.is_ok()) {
        return Result<std::vector<std::vector<float>>>::Err(
            "Mistral batch API error: " + result.error
        );
    }
    
    // 解析响应
    try {
        const auto& resp_json = result.value();
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return Result<std::vector<std::vector<float>>>::Err(
                "Invalid Mistral batch response format"
            );
        }
        
        const auto& data = resp_json["data"];
        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(data.size());
        
        for (const auto& item : data) {
            if (!item.contains("embedding")) {
                return Result<std::vector<std::vector<float>>>::Err(
                    "Missing embedding in Mistral response item"
                );
            }
            
            std::vector<float> embedding = item["embedding"].get<std::vector<float>>();
            embeddings.push_back(normalize_embedding(embedding));
        }
        
        return Result<std::vector<std::vector<float>>>::Ok(embeddings);
        
    } catch (const std::exception& e) {
        return Result<std::vector<std::vector<float>>>::Err(
            std::string("Failed to parse Mistral batch response: ") + e.what()
        );
    }
}

// ============================================================================
// Provider Factory - 严格对齐 OpenClaw embeddings.ts::createEmbeddingProvider
// ============================================================================

core::Result<EmbeddingProviderResult> create_embedding_provider(const EmbeddingProviderOptions& options) {
    EmbeddingProviderResult result;
    result.requested_provider = options.provider;
    
    auto try_create_provider = [&](const std::string& provider_id) -> Result<std::unique_ptr<EmbeddingProvider>> {
        if (provider_id == "openai") {
            OpenAIConfig config;
            config.model = options.model.value_or("text-embedding-3-small");
            config.base_url = options.remote_base_url.value_or("https://api.openai.com/v1");
            config.api_key = options.remote_api_key.value_or("");
            config.max_input_tokens = 8191;
            
            if (config.api_key.empty()) {
                return Result<std::unique_ptr<EmbeddingProvider>>::Err("No API key for OpenAI");
            }
            
            return Result<std::unique_ptr<EmbeddingProvider>>::Ok(
                std::make_unique<OpenAIEmbeddingProvider>(config)
            );
        }
        
        if (provider_id == "ollama") {
            OllamaConfig config;
            config.model = options.model.value_or("nomic-embed-text");
            config.base_url = options.remote_base_url.value_or("http://localhost:11434");
            
            return Result<std::unique_ptr<EmbeddingProvider>>::Ok(
                std::make_unique<OllamaEmbeddingProvider>(config)
            );
        }
        
        if (provider_id == "gemini") {
            GeminiConfig config;
            config.model = options.model.value_or("text-embedding-004");
            config.api_key = options.remote_api_key.value_or("");
            
            if (config.api_key.empty()) {
                return Result<std::unique_ptr<EmbeddingProvider>>::Err("No API key for Gemini");
            }
            
            return Result<std::unique_ptr<EmbeddingProvider>>::Ok(
                std::make_unique<GeminiEmbeddingProvider>(config)
            );
        }
        
        if (provider_id == "voyage") {
            VoyageConfig config;
            config.model = options.model.value_or("voyage-2");
            config.api_key = options.remote_api_key.value_or("");
            
            if (config.api_key.empty()) {
                return Result<std::unique_ptr<EmbeddingProvider>>::Err("No API key for Voyage");
            }
            
            return Result<std::unique_ptr<EmbeddingProvider>>::Ok(
                std::make_unique<VoyageEmbeddingProvider>(config)
            );
        }
        
        if (provider_id == "mistral") {
            MistralConfig config;
            config.model = options.model.value_or("mistral-embed");
            config.api_key = options.remote_api_key.value_or("");
            
            if (config.api_key.empty()) {
                return Result<std::unique_ptr<EmbeddingProvider>>::Err("No API key for Mistral");
            }
            
            return Result<std::unique_ptr<EmbeddingProvider>>::Ok(
                std::make_unique<MistralEmbeddingProvider>(config)
            );
        }
        
        return Result<std::unique_ptr<EmbeddingProvider>>::Err("Unknown provider: " + provider_id);
    };
    
    // Try primary provider
    std::string primary_provider = options.provider;
    if (primary_provider == "auto") {
        // Auto-select: try OpenAI first
        primary_provider = "openai";
    }
    
    auto primary_result = try_create_provider(primary_provider);
    
    if (primary_result.is_ok()) {
        result.provider = std::move(primary_result.value());
        return core::Result<EmbeddingProviderResult>::Ok(std::move(result));
    }
    
    // Try fallback if configured
    if (!options.fallback.empty() && options.fallback != "none") {
        auto fallback_result = try_create_provider(options.fallback);
        
        if (fallback_result.is_ok()) {
            result.provider = std::move(fallback_result.value());
            result.fallback_from = primary_provider;
            result.fallback_reason = primary_result.error_message();
            return core::Result<EmbeddingProviderResult>::Ok(std::move(result));
        }
    }
    
    // No provider available
    result.provider = nullptr;
    result.unavailable_reason = primary_result.error_message();
    
    return core::Result<EmbeddingProviderResult>::Ok(std::move(result));
}

} // namespace memory
} // namespace hunterclaw
