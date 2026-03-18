#include <optional>
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
    std::string model = "nomic-embed-text";  // 对齐 DEFAULT_OLLAMA_EMBEDDING_MODEL
    std::string base_url = "http://localhost:11434";
    std::string api_key;  // Optional (对齐 OpenClaw: Authorization header)
    std::map<std::string, std::string> headers;  // Custom headers
    int max_input_tokens = 8192;
};

struct GeminiConfig {
    std::string model = "gemini-embedding-001";  // 对齐 DEFAULT_GEMINI_EMBEDDING_MODEL
    std::string base_url = "https://generativelanguage.googleapis.com/v1beta";
    std::string api_key;
    std::map<std::string, std::string> headers;
    std::string task_type = "RETRIEVAL_DOCUMENT";  // 对齐 GeminiTaskType
    std::optional<int> output_dimensionality;       // gemini-embedding-2 支持
    int max_input_tokens = 2048;                   // text-embedding-004: 2048
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
        const nlohmann::json& resp_json = result.get();
        
        
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
        const auto& resp_json = result.get();
        
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
        const auto& resp_json = result.get();
        
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
    
    // 严格对齐 OpenClaw embeddings-ollama.ts:104-107
    // "Ollama /api/embeddings accepts one prompt per request."
    // Promise.all(texts.map(embedOne))
    
    std::vector<std::vector<float>> results;
    results.reserve(texts.size());
    
    // 顺序调用 embed_query (可优化为并行)
    for (const auto& text : texts) {
        auto res = embed_query(text);
        if (!res.is_ok()) {
            return core::Result<std::vector<std::vector<float>>>::Err(res.error);
        }
        results.push_back(res.get());
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
    // 严格对齐 OpenClaw embeddings-gemini.ts:227-241
    // embedQuery function
    
    if (text.empty() || text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return core::Result<std::vector<float>>::Ok(std::vector<float>());
    }
    
    // 检查是否为 gemini-embedding-2 模型
    bool is_v2 = (config_.model == "gemini-embedding-2-preview");
    
    // 构建 Gemini 请求 (对齐 buildGeminiTextEmbeddingRequest)
    json request_body = {
        {"content", {
            {"parts", json::array({{{"text", text}}})}
        }},
        {"taskType", config_.task_type.empty() ? "RETRIEVAL_QUERY" : config_.task_type}
    };
    
    // gemini-embedding-2 支持 outputDimensionality
    if (is_v2 && config_.output_dimensionality) {
        request_body["outputDimensionality"] = config_.output_dimensionality.value();
    }
    
    // 构建 headers (对齐 parseGeminiAuth)
    std::map<std::string, std::string> headers = config_.headers;
    headers["Content-Type"] = "application/json";
    
    // Gemini API endpoint: /v1beta/models/{model}:embedContent
    std::string url = config_.base_url;
    if (url.back() == '/') {
        url.pop_back();
    }
    
    std::string model_path = config_.model;
    if (model_path.find("models/") != 0) {
        model_path = "models/" + model_path;
    }
    
    url += "/" + model_path + ":embedContent";
    
    // Gemini 使用 ?key= 参数传递 API key (不是 Authorization header)
    if (!config_.api_key.empty()) {
        url += "?key=" + config_.api_key;
    }
    
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return core::Result<std::vector<float>>::Err(
            "gemini embeddings failed: " + result.error
        );
    }
    
    // 解析响应: {"embedding": {"values": [0.1, 0.2, ...]}}
    try {
        const auto& resp_json = result.get();
        
        if (!resp_json.contains("embedding") || !resp_json["embedding"].contains("values")) {
            return core::Result<std::vector<float>>::Err(
                "gemini embeddings response missing embedding.values"
            );
        }
        
        std::vector<float> embedding = resp_json["embedding"]["values"].get<std::vector<float>>();
        
        // 对齐 OpenClaw: sanitizeAndNormalizeEmbedding
        return core::Result<std::vector<float>>::Ok(normalize_embedding(embedding));
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<float>>::Err(
            std::string("Failed to parse Gemini response: ") + e.what()
        );
    }
}

core::Result<std::vector<std::vector<float>>> GeminiEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts) {
    
    // 严格对齐 OpenClaw embeddings-gemini.ts:243-271
    // embedBatch function -> embedBatchInputs
    
    if (texts.empty()) {
        return core::Result<std::vector<std::vector<float>>>::Ok(
            std::vector<std::vector<float>>()
        );
    }
    
    // 检查是否为 gemini-embedding-2 模型
    bool is_v2 = (config_.model == "gemini-embedding-2-preview");
    
    // 构建模型路径
    std::string model_path = config_.model;
    if (model_path.find("models/") != 0) {
        model_path = "models/" + model_path;
    }
    
    // 构建 batch 请求 (对齐 buildGeminiEmbeddingRequest)
    json::array_t requests;
    for (const auto& text : texts) {
        json request = {
            {"model", model_path},
            {"content", {
                {"parts", json::array({{{"text", text}}})}
            }},
            {"taskType", config_.task_type.empty() ? "RETRIEVAL_DOCUMENT" : config_.task_type}
        };
        
        if (is_v2 && config_.output_dimensionality) {
            request["outputDimensionality"] = config_.output_dimensionality.value();
        }
        
        requests.push_back(request);
    }
    
    json request_body = {
        {"requests", requests}
    };
    
    // 构建 headers
    std::map<std::string, std::string> headers = config_.headers;
    headers["Content-Type"] = "application/json";
    
    // Gemini Batch API endpoint: /v1beta/models/{model}:batchEmbedContents
    std::string url = config_.base_url;
    if (url.back() == '/') {
        url.pop_back();
    }
    url += "/" + model_path + ":batchEmbedContents";
    
    if (!config_.api_key.empty()) {
        url += "?key=" + config_.api_key;
    }
    
    auto result = HttpClient::post_json(url, request_body, headers, 30000);
    
    if (!result.is_ok()) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "gemini batch embeddings failed: " + result.error
        );
    }
    
    // 解析响应: {"embeddings": [{"values": [...]}, ...]}
    try {
        const auto& resp_json = result.get();
        
        if (!resp_json.contains("embeddings") || !resp_json["embeddings"].is_array()) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "gemini batch response missing embeddings array"
            );
        }
        
        const auto& embeddings_json = resp_json["embeddings"];
        std::vector<std::vector<float>> results;
        results.reserve(texts.size());
        
        // 对齐 OpenClaw: 按顺序映射结果
        for (size_t i = 0; i < texts.size(); ++i) {
            if (i < embeddings_json.size() && 
                embeddings_json[i].contains("values") &&
                embeddings_json[i]["values"].is_array()) {
                
                std::vector<float> embedding = embeddings_json[i]["values"].get<std::vector<float>>();
                results.push_back(normalize_embedding(embedding));
            } else {
                // 对齐 OpenClaw: 缺失的 embedding 返回空数组
                results.push_back(std::vector<float>());
            }
        }
        
        return core::Result<std::vector<std::vector<float>>>::Ok(results);
        
    } catch (const std::exception& e) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            std::string("Failed to parse Gemini batch response: ") + e.what()
        );
    }
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
        const auto& resp_json = result.get();
        
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
        const auto& resp_json = result.get();
        
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
        const auto& resp_json = result.get();
        
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
