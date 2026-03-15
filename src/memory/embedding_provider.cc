#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/http_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <cmath>
#include <regex>

using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// ============================================================================
// Provider Configuration Structures
// ============================================================================

struct OpenAIConfig {
    std::string model = "text-embedding-3-small";
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;
    std::map<std::string, std::string> headers;
    int dimensions = 1536;
    int max_input_tokens = 8191;
};

struct OllamaConfig {
    std::string model = "nomic-embed-text";
    std::string base_url = "http://localhost:11434";
    int max_input_tokens = 8192;
};

struct GeminiConfig {
    std::string model = "text-embedding-004";
    std::string base_url = "https://generativelanguage.googleapis.com/v1beta";
    std::string api_key;
    std::map<std::string, std::string> headers;
    std::string task_type = "RETRIEVAL_DOCUMENT";
    int output_dimensionality = 768;
    int max_input_tokens = 8192;
};

struct VoyageConfig {
    std::string model = "voyage-2";
    std::string base_url = "https://api.voyageai.com/v1";
    std::string api_key;
    std::map<std::string, std::string> headers;
    int max_input_tokens = 32000;
};

struct MistralConfig {
    std::string model = "mistral-embed";
    std::string base_url = "https://api.mistral.ai/v1";
    std::string api_key;
    std::map<std::string, std::string> headers;
    int max_input_tokens = 8192;
};

// Provider class declarations
class OpenAIEmbeddingProvider : public EmbeddingProvider {
public:
    explicit OpenAIEmbeddingProvider(const OpenAIConfig& config);
    std::string get_id() const override;
    std::string get_model() const override;
    std::optional<int> get_max_input_tokens() const override;
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
private:
    OpenAIConfig config_;
};

class OllamaEmbeddingProvider : public EmbeddingProvider {
public:
    explicit OllamaEmbeddingProvider(const OllamaConfig& config);
    std::string get_id() const override;
    std::string get_model() const override;
    std::optional<int> get_max_input_tokens() const override;
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
private:
    OllamaConfig config_;
};

class GeminiEmbeddingProvider : public EmbeddingProvider {
public:
    explicit GeminiEmbeddingProvider(const GeminiConfig& config);
    std::string get_id() const override;
    std::string get_model() const override;
    std::optional<int> get_max_input_tokens() const override;
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
private:
    GeminiConfig config_;
};

class VoyageEmbeddingProvider : public EmbeddingProvider {
public:
    explicit VoyageEmbeddingProvider(const VoyageConfig& config);
    std::string get_id() const override;
    std::string get_model() const override;
    std::optional<int> get_max_input_tokens() const override;
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
private:
    VoyageConfig config_;
};

class MistralEmbeddingProvider : public EmbeddingProvider {
public:
    explicit MistralEmbeddingProvider(const MistralConfig& config);
    std::string get_id() const override;
    std::string get_model() const override;
    std::optional<int> get_max_input_tokens() const override;
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
private:
    MistralConfig config_;
};

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

std::string OpenAIEmbeddingProvider::get_id() const {
    return "openai";
}

std::string OpenAIEmbeddingProvider::get_model() const {
    return config_.model;
}

std::optional<int> OpenAIEmbeddingProvider::get_max_input_tokens() const {
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
        return core::Result<std::vector<float>>::Err("OpenAI API error: " + result.error);
    }
    
    // 解析响应 (对齐 OpenClaw格式)
    try {
        const auto& resp_json = result.value;
        
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return core::Result<std::vector<float>>::Err("Invalid OpenAI response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return core::Result<std::vector<float>>::Err("Empty embeddings in response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return core::Result<std::vector<float>>::Err("No embedding field in response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        // Normalize embedding
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
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
        return core::Result<std::vector<std::vector<float>>>::Err(
            "OpenAI batch API error: " + result.error
        );
    }
    
    // 解析响应
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "Invalid OpenAI batch response format"
            );
        }
        
        const auto& data = resp_json["data"];
        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(data.size());
        
        for (const auto& item : data) {
            if (!item.contains("embedding")) {
                return core::Result<std::vector<std::vector<float>>>::Err(
                    "Missing embedding in response item"
                );
            }
            
            std::vector<float> embedding = item["embedding"].get<std::vector<float>>();
            embeddings.push_back(normalize_embedding(embedding));
        }
        
        return core::Result<std::vector<std::vector<float>>>::Ok(embeddings);
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            std::string("Failed to parse OpenAI batch response: ") + e.what()
        );
    }
}

// ============================================================================
// Ollama Provider
// ============================================================================

OllamaEmbeddingProvider::OllamaEmbeddingProvider(const OllamaConfig& config)
    : config_(config) {}

std::string OllamaEmbeddingProvider::get_id() const {
    return "ollama";
}

std::string OllamaEmbeddingProvider::get_model() const {
    return config_.model;
}

std::optional<int> OllamaEmbeddingProvider::get_max_input_tokens() const {
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
        return core::Result<std::vector<float>>::Err("Ollama API error: " + result.error);
    }
    
    // 解析响应: {"embedding": [0.1, 0.2, ...]}
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("embedding")) {
            return core::Result<std::vector<float>>::Err("No embedding field in Ollama response");
        }
        
        std::vector<float> embedding = resp_json["embedding"].get<std::vector<float>>();
        
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
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
            return core::Result<std::vector<std::vector<float>>>::Err(res.error);
        }
        results.push_back(res.value);
    }
    
    return core::Result<std::vector<std::vector<float>>>::Ok(results);
}

// ============================================================================
// Gemini Provider
// ============================================================================

GeminiEmbeddingProvider::GeminiEmbeddingProvider(const GeminiConfig& config)
    : config_(config) {}

std::string GeminiEmbeddingProvider::get_id() const {
    return "gemini";
}

std::string GeminiEmbeddingProvider::get_model() const {
    return config_.model;
}

std::optional<int> GeminiEmbeddingProvider::get_max_input_tokens() const {
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
        return core::Result<std::vector<float>>::Err("Gemini API error: " + result.error);
    }
    
    // 解析响应: {"embedding": {"values": [0.1, 0.2, ...]}}
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("embedding") || !resp_json["embedding"].contains("values")) {
            return core::Result<std::vector<float>>::Err("Invalid Gemini response format");
        }
        
        std::vector<float> embedding = resp_json["embedding"]["values"].get<std::vector<float>>();
        
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
            std::string("Failed to parse Gemini response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> GeminiEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    return core::Result<std::vector<std::vector<float>>>::Err("Gemini batch not yet implemented");
}

// ============================================================================
// Voyage Provider
// ============================================================================

VoyageEmbeddingProvider::VoyageEmbeddingProvider(const VoyageConfig& config)
    : config_(config) {}

std::string VoyageEmbeddingProvider::get_id() const {
    return "voyage";
}

std::string VoyageEmbeddingProvider::get_model() const {
    return config_.model;
}

std::optional<int> VoyageEmbeddingProvider::get_max_input_tokens() const {
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
        return core::Result<std::vector<float>>::Err("Voyage API error: " + result.error);
    }
    
    // 解析响应: {data: [{embedding: [0.1, 0.2, ...]}]}
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return core::Result<std::vector<float>>::Err("Invalid Voyage response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return core::Result<std::vector<float>>::Err("Empty data in Voyage response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return core::Result<std::vector<float>>::Err("No embedding field in Voyage response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
            std::string("Failed to parse Voyage response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> VoyageEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    return core::Result<std::vector<std::vector<float>>>::Err("Voyage batch not yet implemented");
}

// ============================================================================
// Mistral Provider
// ============================================================================

MistralEmbeddingProvider::MistralEmbeddingProvider(const MistralConfig& config)
    : config_(config) {}

std::string MistralEmbeddingProvider::get_id() const {
    return "mistral";
}

std::string MistralEmbeddingProvider::get_model() const {
    return config_.model;
}

std::optional<int> MistralEmbeddingProvider::get_max_input_tokens() const {
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
        return core::Result<std::vector<float>>::Err("Mistral API error: " + result.error);
    }
    
    // 解析响应 (OpenAI格式兼容): {data: [{embedding: [...]}]}
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return core::Result<std::vector<float>>::Err("Invalid Mistral response format");
        }
        
        const auto& data = resp_json["data"];
        if (data.empty()) {
            return core::Result<std::vector<float>>::Err("Empty data in Mistral response");
        }
        
        const auto& first_item = data[0];
        if (!first_item.contains("embedding")) {
            return core::Result<std::vector<float>>::Err("No embedding field in Mistral response");
        }
        
        std::vector<float> embedding = first_item["embedding"].get<std::vector<float>>();
        
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
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
        return core::Result<std::vector<std::vector<float>>>::Err(
            "Mistral batch API error: " + result.error
        );
    }
    
    // 解析响应
    try {
        const auto& resp_json = result.value;
        
        if (!resp_json.contains("data") || !resp_json["data"].is_array()) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "Invalid Mistral batch response format"
            );
        }
        
        const auto& data = resp_json["data"];
        std::vector<std::vector<float>> embeddings;
        embeddings.reserve(data.size());
        
        for (const auto& item : data) {
            if (!item.contains("embedding")) {
                return core::Result<std::vector<std::vector<float>>>::Err(
                    "Missing embedding in Mistral response item"
                );
            }
            
            std::vector<float> embedding = item["embedding"].get<std::vector<float>>();
            embeddings.push_back(normalize_embedding(embedding));
        }
        
        return core::Result<std::vector<std::vector<float>>>::Ok(embeddings);
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            std::string("Failed to parse Mistral batch response: ") + e.what()
        );
    }
}

// ============================================================================
// Provider Factory - TODO: 实现与 types.h 中 EmbeddingProviderResult 兼容的工厂函数
// ============================================================================

// 暂时注释掉：这个实现与 types.h 中的 EmbeddingProviderResult 定义不匹配
// EmbeddingProviderResult 应该只包含 provider_name、model 等元数据，
// 而不是 unique_ptr<EmbeddingProvider>。需要重新设计工厂模式。

} // namespace memory
} // namespace hunterclaw
