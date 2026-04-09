#include <optional>
#include "hunterclaw/memory/embedding_provider.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/infra/http_client.h"
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
// Helper: Vector normalization (delegates to sanitize_and_normalize_embedding)
// ============================================================================

static std::vector<float> normalize_embedding(const std::vector<float>& vec) {
    // 对齐 openclaw embedding-vectors.ts:sanitizeAndNormalizeEmbedding
    return sanitize_and_normalize_embedding(vec);
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<float>>::Err("OpenAI API error: " + http_result.error);
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<float>>::Err(
            "OpenAI API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应 (对齐 OpenClaw格式)
    try {
        const nlohmann::json& resp_json = result;
        
        
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 60000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "OpenAI batch API error: " + http_result.error
        );
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "OpenAI batch API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应
    try {
        const auto& resp_json = result;
        
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<float>>::Err("Ollama API error: " + http_result.error);
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<float>>::Err(
            "Ollama API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应: {"embedding": [0.1, 0.2, ...]}
    try {
        const auto& resp_json = result;
        
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
    
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<float>>::Err(
            "gemini embeddings failed: " + http_result.error
        );
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<float>>::Err(
            "gemini embeddings failed " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应: {"embedding": {"values": [0.1, 0.2, ...]}}
    try {
        const auto& resp_json = result;
        
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
    
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "gemini batch embeddings failed: " + http_result.error
        );
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "gemini batch embeddings failed " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应: {"embeddings": [{"values": [...]}, ...]}
    try {
        const auto& resp_json = result;
        
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<float>>::Err("Voyage API error: " + http_result.error);
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<float>>::Err(
            "Voyage API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应: {data: [{embedding: [0.1, 0.2, ...]}]}
    try {
        const auto& resp_json = result;
        
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
    
    // 🎯 严格对齐 OpenClaw: embeddings-voyage.ts batch embedding
    // Voyage AI supports batch embedding via POST /v1/embeddings with input as array
    if (texts.empty()) {
        return core::Result<std::vector<std::vector<float>>>::Ok({});
    }

    // Voyage has a max batch size of 128 texts
    const size_t MAX_BATCH = 128;
    std::vector<std::vector<float>> all_embeddings;

    for (size_t offset = 0; offset < texts.size(); offset += MAX_BATCH) {
        size_t end = std::min(offset + MAX_BATCH, texts.size());
        std::vector<std::string> batch(texts.begin() + offset, texts.begin() + end);

        json request_body = {
            {"model", config_.model},
            {"input", json::array()}
        };
        for (const auto& text : batch) {
            request_body["input"].push_back(text);
        }
        if (config_.input_type.has_value()) {
            request_body["input_type"] = *config_.input_type;
        }

        std::map<std::string, std::string> headers = config_.headers;
        headers["Content-Type"] = "application/json";

        auto http_result = http_client_->post(config_.endpoint, request_body.dump(), headers, 30000);
        if (!http_result.is_ok()) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "Voyage batch embedding failed: " + http_result.error);
        }

        try {
            auto response = json::parse(http_result.get().body);
            if (!response.contains("data") || !response["data"].is_array()) {
                return core::Result<std::vector<std::vector<float>>>::Err(
                    "Invalid Voyage batch response: missing data array");
            }

            for (const auto& item : response["data"]) {
                std::vector<float> embedding;
                if (item.contains("embedding") && item["embedding"].is_array()) {
                    for (const auto& val : item["embedding"]) {
                        embedding.push_back(val.get<float>());
                    }
                }
                all_embeddings.push_back(std::move(embedding));
            }
        } catch (const std::exception& e) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                std::string("Failed to parse Voyage batch response: ") + e.what());
        }
    }

    return core::Result<std::vector<std::vector<float>>>::Ok(all_embeddings);
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 30000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<float>>::Err("Mistral API error: " + http_result.error);
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<float>>::Err(
            "Mistral API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应 (OpenAI格式兼容): {data: [{embedding: [...]}]}
    try {
        const auto& resp_json = result;
        
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
    infra::HttpRequestOptions opts;
    opts.headers = headers;
    opts.body = request_body.dump();
    opts.timeout_ms = 60000;
    auto http_result = infra::HttpClient::create()->post(url, opts);
    if (!http_result.is_ok()) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "Mistral batch API error: " + http_result.error
        );
    }
    if (!http_result.get().ok) {
        return core::Result<std::vector<std::vector<float>>>::Err(
            "Mistral batch API error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
        );
    }
    auto result = nlohmann::json::parse(http_result.get().body);
    
    // 解析响应
    try {
        const auto& resp_json = result;
        
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
// EmbeddingProviderFactory - Factory implementation
// ============================================================================

const std::vector<EmbeddingProviderId> EmbeddingProviderFactory::REMOTE_PROVIDER_IDS = {
    EmbeddingProviderId::OpenAI,
    EmbeddingProviderId::Gemini,
    EmbeddingProviderId::Voyage,
    EmbeddingProviderId::Mistral,
    EmbeddingProviderId::Ollama,
};

core::Result<EmbeddingProviderResult> EmbeddingProviderFactory::create(
    const EmbeddingProviderOptions& options
) {
    EmbeddingProviderResult result;
    result.requested_provider = options.provider;

    // "none" or empty provider means FTS-only mode
    if (options.provider == "none" || options.provider.empty()) {
        result.provider = nullptr;
        return core::Result<EmbeddingProviderResult>::Ok(result);
    }

    // "auto" mode: try providers in priority order
    if (options.provider == "auto") {
        for (auto provider_id : REMOTE_PROVIDER_IDS) {
            auto provider_result = create_provider(provider_id, options);
            if (provider_result.is_ok()) {
                result.provider = provider_result.get().provider;
                return core::Result<EmbeddingProviderResult>::Ok(result);
            }
        }
        // All providers failed - return FTS-only mode
        result.provider = nullptr;
        result.provider_unavailable_reason = "No embedding provider available";
        return core::Result<EmbeddingProviderResult>::Ok(result);
    }

    // Specific provider requested
    auto parsed_id = parse_provider_id(options.provider);
    if (!parsed_id.has_value()) {
        return core::Result<EmbeddingProviderResult>::Err(
            "Unknown embedding provider: " + options.provider
        );
    }

    auto provider_result = create_provider(parsed_id.value(), options);
    if (!provider_result.is_ok()) {
        // Try fallback if configured
        if (!options.fallback.empty() && options.fallback != "none") {
            auto fallback_id = parse_provider_id(options.fallback);
            if (fallback_id.has_value()) {
                auto fallback_result = create_provider(fallback_id.value(), options);
                if (fallback_result.is_ok()) {
                    result.provider = fallback_result.get().provider;
                    result.fallback_from = parsed_id;
                    result.fallback_reason = provider_result.error;
                    return core::Result<EmbeddingProviderResult>::Ok(result);
                }
            }
        }
        return core::Result<EmbeddingProviderResult>::Err(provider_result.error);
    }

    result.provider = provider_result.get().provider;
    return core::Result<EmbeddingProviderResult>::Ok(result);
}

core::Result<EmbeddingProviderResult> EmbeddingProviderFactory::create_provider(
    EmbeddingProviderId id,
    const EmbeddingProviderOptions& options
) {
    EmbeddingProviderResult result;

    switch (id) {
        case EmbeddingProviderId::OpenAI: {
            RemoteEmbeddingProvider::Config config;
            config.provider_id = EmbeddingProviderId::OpenAI;
            config.base_url = "https://api.openai.com/v1";
            config.model = options.model.empty() ? "text-embedding-3-small" : options.model;
            // API key from environment
            const char* api_key = std::getenv("OPENAI_API_KEY");
            if (!api_key || std::string(api_key).empty()) {
                return core::Result<EmbeddingProviderResult>::Err("OPENAI_API_KEY not set");
            }
            config.api_key = api_key;
            auto provider = RemoteEmbeddingProvider::create(config);
            if (!provider.is_ok()) {
                return core::Result<EmbeddingProviderResult>::Err(provider.error);
            }
            result.provider = provider.get();
            return core::Result<EmbeddingProviderResult>::Ok(result);
        }
        case EmbeddingProviderId::Ollama: {
            RemoteEmbeddingProvider::Config config;
            config.provider_id = EmbeddingProviderId::Ollama;
            config.base_url = "http://localhost:11434";
            config.model = options.model.empty() ? "nomic-embed-text" : options.model;
            auto provider = RemoteEmbeddingProvider::create(config);
            if (!provider.is_ok()) {
                return core::Result<EmbeddingProviderResult>::Err(provider.error);
            }
            result.provider = provider.get();
            return core::Result<EmbeddingProviderResult>::Ok(result);
        }
        case EmbeddingProviderId::Gemini: {
            RemoteEmbeddingProvider::Config config;
            config.provider_id = EmbeddingProviderId::Gemini;
            config.base_url = "https://generativelanguage.googleapis.com/v1beta";
            config.model = options.model.empty() ? "text-embedding-004" : options.model;
            const char* api_key = std::getenv("GEMINI_API_KEY");
            if (!api_key || std::string(api_key).empty()) {
                return core::Result<EmbeddingProviderResult>::Err("GEMINI_API_KEY not set");
            }
            config.api_key = api_key;
            auto provider = RemoteEmbeddingProvider::create(config);
            if (!provider.is_ok()) {
                return core::Result<EmbeddingProviderResult>::Err(provider.error);
            }
            result.provider = provider.get();
            return core::Result<EmbeddingProviderResult>::Ok(result);
        }
        case EmbeddingProviderId::Voyage: {
            RemoteEmbeddingProvider::Config config;
            config.provider_id = EmbeddingProviderId::Voyage;
            config.base_url = "https://api.voyageai.com/v1";
            config.model = options.model.empty() ? "voyage-3" : options.model;
            const char* api_key = std::getenv("VOYAGE_API_KEY");
            if (!api_key || std::string(api_key).empty()) {
                return core::Result<EmbeddingProviderResult>::Err("VOYAGE_API_KEY not set");
            }
            config.api_key = api_key;
            auto provider = RemoteEmbeddingProvider::create(config);
            if (!provider.is_ok()) {
                return core::Result<EmbeddingProviderResult>::Err(provider.error);
            }
            result.provider = provider.get();
            return core::Result<EmbeddingProviderResult>::Ok(result);
        }
        case EmbeddingProviderId::Mistral: {
            RemoteEmbeddingProvider::Config config;
            config.provider_id = EmbeddingProviderId::Mistral;
            config.base_url = "https://api.mistral.ai/v1";
            config.model = options.model.empty() ? "mistral-embed" : options.model;
            const char* api_key = std::getenv("MISTRAL_API_KEY");
            if (!api_key || std::string(api_key).empty()) {
                return core::Result<EmbeddingProviderResult>::Err("MISTRAL_API_KEY not set");
            }
            config.api_key = api_key;
            auto provider = RemoteEmbeddingProvider::create(config);
            if (!provider.is_ok()) {
                return core::Result<EmbeddingProviderResult>::Err(provider.error);
            }
            result.provider = provider.get();
            return core::Result<EmbeddingProviderResult>::Ok(result);
        }
        default:
            return core::Result<EmbeddingProviderResult>::Err(
                "Unsupported embedding provider: " + to_string(id)
            );
    }
}

core::Result<std::shared_ptr<EmbeddingProvider>> EmbeddingProviderFactory::create_local_provider(
    const EmbeddingProviderOptions& /*options*/
) {
    // Local provider not yet implemented
    return core::Result<std::shared_ptr<EmbeddingProvider>>::Err(
        "Local embedding provider not yet implemented"
    );
}

bool EmbeddingProviderFactory::can_auto_select_local(
    const EmbeddingProviderOptions& /*options*/
) {
    return false;
}

bool EmbeddingProviderFactory::is_missing_api_key_error(const std::string& error_msg) {
    return error_msg.find("API_KEY") != std::string::npos ||
           error_msg.find("api_key") != std::string::npos ||
           error_msg.find("not set") != std::string::npos;
}

// ============================================================================
// RemoteEmbeddingProvider Implementation
// ============================================================================

namespace {

// Internal implementation class
class RemoteEmbeddingProviderImpl : public RemoteEmbeddingProvider {
public:
    explicit RemoteEmbeddingProviderImpl(const RemoteEmbeddingProvider::Config& config)
        : config_(config) {}

    std::string get_id() const override {
        return to_string(config_.provider_id);
    }

    std::string get_model() const override {
        return config_.model;
    }

    std::optional<int> get_max_input_tokens() const override {
        return config_.max_input_tokens;
    }

    core::Result<std::vector<float>> embed_query(const std::string& text) override {
        auto batch_result = embed_batch({text});
        if (!batch_result.is_ok()) {
            return core::Result<std::vector<float>>::Err(batch_result.error);
        }
        const auto& batch = batch_result.get();
        if (batch.empty()) {
            return core::Result<std::vector<float>>::Err("Empty embedding result");
        }
        return core::Result<std::vector<float>>::Ok(batch[0]);
    }

    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override {
        if (texts.empty()) {
            return core::Result<std::vector<std::vector<float>>>::Ok({});
        }

        // Build request body
        json request_body;
        request_body["model"] = config_.model;
        if (texts.size() == 1) {
            request_body["input"] = texts[0];
        } else {
            request_body["input"] = texts;
        }
        if (config_.output_dimensionality.has_value()) {
            request_body["dimensions"] = config_.output_dimensionality.value();
        }

        // Build headers
        std::map<std::string, std::string> headers = config_.headers;
        if (!config_.api_key.empty()) {
            headers["Authorization"] = "Bearer " + config_.api_key;
        }

        // Make HTTP request using infra::HttpClient
        std::string url = config_.base_url + "/embeddings";
        infra::HttpRequestOptions req_opts;
        req_opts.headers = headers;
        req_opts.body = request_body.dump();
        req_opts.timeout_ms = 30000;
        auto http_result = infra::HttpClient::create()->post(url, req_opts);
        if (!http_result.is_ok()) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "HTTP request failed: " + http_result.error
            );
        }
        if (!http_result.get().ok) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                "HTTP error " + std::to_string(http_result.get().status_code) + ": " + http_result.get().body
            );
        }

        // Parse response
        try {
            const json response = json::parse(http_result.get().body);
            if (!response.contains("data") || !response["data"].is_array()) {
                std::string err_msg = "Invalid response format";
                if (response.contains("error")) {
                    err_msg = response["error"].value("message", err_msg);
                }
                return core::Result<std::vector<std::vector<float>>>::Err(err_msg);
            }

            std::vector<std::vector<float>> embeddings;
            embeddings.reserve(response["data"].size());

            for (const auto& item : response["data"]) {
                if (!item.contains("embedding") || !item["embedding"].is_array()) {
                    return core::Result<std::vector<std::vector<float>>>::Err(
                        "Missing embedding in response item"
                    );
                }
                std::vector<float> embedding;
                embedding.reserve(item["embedding"].size());
                for (const auto& val : item["embedding"]) {
                    embedding.push_back(val.get<float>());
                }
                embeddings.push_back(std::move(embedding));
            }

            return core::Result<std::vector<std::vector<float>>>::Ok(embeddings);
        } catch (const std::exception& e) {
            return core::Result<std::vector<std::vector<float>>>::Err(
                std::string("Failed to parse embedding response: ") + e.what()
            );
        }
    }

private:
    RemoteEmbeddingProvider::Config config_;
};

} // anonymous namespace

// static
core::Result<std::shared_ptr<RemoteEmbeddingProvider>> RemoteEmbeddingProvider::create(
    const Config& config
) {
    if (config.base_url.empty()) {
        return core::Result<std::shared_ptr<RemoteEmbeddingProvider>>::Err(
            "RemoteEmbeddingProvider: base_url is required"
        );
    }
    if (config.model.empty()) {
        return core::Result<std::shared_ptr<RemoteEmbeddingProvider>>::Err(
            "RemoteEmbeddingProvider: model is required"
        );
    }
    auto provider = std::make_shared<RemoteEmbeddingProviderImpl>(config);
    return core::Result<std::shared_ptr<RemoteEmbeddingProvider>>::Ok(provider);
}

} // namespace memory
} // namespace hunterclaw
