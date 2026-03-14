#include "hunterclaw/memory/embedding_provider.h"
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <json/json.h>
#include <sstream>

namespace hunterclaw {
namespace memory {

// ============================================================================
// HTTP Client 辅助类
// ============================================================================

class HTTPClient {
public:
    struct Response {
        int status_code = 0;
        std::string body;
        std::map<std::string, std::string> headers;
    };
    
    static core::Result<Response> post(
        const std::string& url,
        const std::string& json_body,
        const std::map<std::string, std::string>& headers
    ) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            return core::Err("Failed to initialize CURL");
        }
        
        Response response;
        std::string response_body;
        
        // 回调函数
        auto write_callback = [](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
            ((std::string*)userp)->append((char*)contents, size * nmemb);
            return size * nmemb;
        };
        
        // 设置 CURL 选项
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        
        // 设置请求头
        struct curl_slist* http_headers = nullptr;
        http_headers = curl_slist_append(http_headers, "Content-Type: application/json");
        
        for (const auto& [key, value] : headers) {
            std::string header = key + ": " + value;
            http_headers = curl_slist_append(http_headers, header.c_str());
        }
        
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, http_headers);
        
        // 执行请求
        CURLcode res = curl_easy_perform(curl);
        
        if (res != CURLE_OK) {
            curl_slist_free_all(http_headers);
            curl_easy_cleanup(curl);
            return core::Err("CURL request failed: " + std::string(curl_easy_strerror(res)));
        }
        
        // 获取状态码
        long status_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        response.status_code = static_cast<int>(status_code);
        response.body = response_body;
        
        curl_slist_free_all(http_headers);
        curl_easy_cleanup(curl);
        
        return core::Ok(response);
    }
};

// ============================================================================
// OpenAI Embedding Provider
// ============================================================================

class OpenAIEmbeddingProvider::Impl {
public:
    std::string api_key_;
    std::string model_;
    std::string base_url_;
    
    Impl(const std::string& api_key, const std::string& model, const std::string& base_url)
        : api_key_(api_key), model_(model), base_url_(base_url) {}
    
    int get_dimension() const {
        if (model_ == "text-embedding-3-large") return 3072;
        if (model_ == "text-embedding-3-small") return 1536;
        if (model_ == "text-embedding-ada-002") return 1536;
        return 1536;  // 默认
    }
    
    core::Result<std::vector<float>> embed(const std::string& text) {
        // 构造请求
        Json::Value request_json;
        request_json["input"] = text;
        request_json["model"] = model_;
        request_json["encoding_format"] = "float";
        
        Json::StreamWriterBuilder writer;
        std::string request_body = Json::writeString(writer, request_json);
        
        // 发送请求
        std::string url = base_url_ + "/embeddings";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + api_key_}
        };
        
        auto response = HTTPClient::post(url, request_body, headers);
        if (!response) {
            return core::Err("HTTP request failed: " + response.error());
        }
        
        auto resp = response.value();
        if (resp.status_code != 200) {
            return core::Err("OpenAI API error (status " + std::to_string(resp.status_code) + "): " + resp.body);
        }
        
        // 解析响应
        Json::CharReaderBuilder reader;
        Json::Value response_json;
        std::istringstream iss(resp.body);
        std::string errs;
        
        if (!Json::parseFromStream(reader, iss, &response_json, &errs)) {
            return core::Err("Failed to parse JSON response: " + errs);
        }
        
        // 提取 embedding
        if (!response_json.isMember("data") || !response_json["data"].isArray() || response_json["data"].empty()) {
            return core::Err("Invalid response format: missing 'data' array");
        }
        
        const auto& data = response_json["data"][0];
        if (!data.isMember("embedding") || !data["embedding"].isArray()) {
            return core::Err("Invalid response format: missing 'embedding' array");
        }
        
        std::vector<float> embedding;
        for (const auto& val : data["embedding"]) {
            embedding.push_back(val.asFloat());
        }
        
        spdlog::debug("[OpenAIEmbeddingProvider] Generated embedding: dim={}", embedding.size());
        
        return core::Ok(embedding);
    }
    
    core::Result<std::vector<std::vector<float>>> embed_batch(const std::vector<std::string>& texts) {
        // 批量请求
        Json::Value request_json;
        Json::Value input_array(Json::arrayValue);
        for (const auto& text : texts) {
            input_array.append(text);
        }
        request_json["input"] = input_array;
        request_json["model"] = model_;
        request_json["encoding_format"] = "float";
        
        Json::StreamWriterBuilder writer;
        std::string request_body = Json::writeString(writer, request_json);
        
        std::string url = base_url_ + "/embeddings";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + api_key_}
        };
        
        auto response = HTTPClient::post(url, request_body, headers);
        if (!response) {
            return core::Err("HTTP request failed: " + response.error());
        }
        
        auto resp = response.value();
        if (resp.status_code != 200) {
            return core::Err("OpenAI API error: " + resp.body);
        }
        
        Json::CharReaderBuilder reader;
        Json::Value response_json;
        std::istringstream iss(resp.body);
        std::string errs;
        
        if (!Json::parseFromStream(reader, iss, &response_json, &errs)) {
            return core::Err("Failed to parse JSON response: " + errs);
        }
        
        std::vector<std::vector<float>> embeddings;
        for (const auto& data : response_json["data"]) {
            std::vector<float> embedding;
            for (const auto& val : data["embedding"]) {
                embedding.push_back(val.asFloat());
            }
            embeddings.push_back(embedding);
        }
        
        spdlog::debug("[OpenAIEmbeddingProvider] Generated {} embeddings", embeddings.size());
        
        return core::Ok(embeddings);
    }
};

OpenAIEmbeddingProvider::OpenAIEmbeddingProvider(
    const std::string& api_key,
    const std::string& model,
    const std::string& base_url
) : impl_(std::make_unique<Impl>(api_key, model, base_url)) {}

OpenAIEmbeddingProvider::~OpenAIEmbeddingProvider() = default;

core::Result<std::vector<float>> OpenAIEmbeddingProvider::embed(const std::string& text) {
    return impl_->embed(text);
}

core::Result<std::vector<std::vector<float>>> OpenAIEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts
) {
    return impl_->embed_batch(texts);
}

std::string OpenAIEmbeddingProvider::model_name() const {
    return impl_->model_;
}

int OpenAIEmbeddingProvider::dimension() const {
    return impl_->get_dimension();
}

// ============================================================================
// Ollama Embedding Provider
// ============================================================================

class OllamaEmbeddingProvider::Impl {
public:
    std::string model_;
    std::string base_url_;
    
    Impl(const std::string& model, const std::string& base_url)
        : model_(model), base_url_(base_url) {}
    
    int get_dimension() const {
        if (model_ == "nomic-embed-text") return 768;
        if (model_ == "mxbai-embed-large") return 1024;
        if (model_ == "all-minilm") return 384;
        return 768;  // 默认
    }
    
    core::Result<std::vector<float>> embed(const std::string& text) {
        Json::Value request_json;
        request_json["model"] = model_;
        request_json["prompt"] = text;
        
        Json::StreamWriterBuilder writer;
        std::string request_body = Json::writeString(writer, request_json);
        
        std::string url = base_url_ + "/api/embeddings";
        
        auto response = HTTPClient::post(url, request_body, {});
        if (!response) {
            return core::Err("HTTP request failed: " + response.error());
        }
        
        auto resp = response.value();
        if (resp.status_code != 200) {
            return core::Err("Ollama API error: " + resp.body);
        }
        
        Json::CharReaderBuilder reader;
        Json::Value response_json;
        std::istringstream iss(resp.body);
        std::string errs;
        
        if (!Json::parseFromStream(reader, iss, &response_json, &errs)) {
            return core::Err("Failed to parse JSON response: " + errs);
        }
        
        if (!response_json.isMember("embedding") || !response_json["embedding"].isArray()) {
            return core::Err("Invalid response format");
        }
        
        std::vector<float> embedding;
        for (const auto& val : response_json["embedding"]) {
            embedding.push_back(val.asFloat());
        }
        
        spdlog::debug("[OllamaEmbeddingProvider] Generated embedding: dim={}", embedding.size());
        
        return core::Ok(embedding);
    }
    
    core::Result<std::vector<std::vector<float>>> embed_batch(const std::vector<std::string>& texts) {
        // Ollama 不支持批量,逐个调用
        std::vector<std::vector<float>> embeddings;
        for (const auto& text : texts) {
            auto result = embed(text);
            if (!result) {
                return core::Err(result.error());
            }
            embeddings.push_back(result.value());
        }
        return core::Ok(embeddings);
    }
};

OllamaEmbeddingProvider::OllamaEmbeddingProvider(
    const std::string& model,
    const std::string& base_url
) : impl_(std::make_unique<Impl>(model, base_url)) {}

OllamaEmbeddingProvider::~OllamaEmbeddingProvider() = default;

core::Result<std::vector<float>> OllamaEmbeddingProvider::embed(const std::string& text) {
    return impl_->embed(text);
}

core::Result<std::vector<std::vector<float>>> OllamaEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts
) {
    return impl_->embed_batch(texts);
}

std::string OllamaEmbeddingProvider::model_name() const {
    return impl_->model_;
}

int OllamaEmbeddingProvider::dimension() const {
    return impl_->get_dimension();
}

// ============================================================================
// Gemini Embedding Provider
// ============================================================================

class GeminiEmbeddingProvider::Impl {
public:
    std::string api_key_;
    std::string model_;
    
    Impl(const std::string& api_key, const std::string& model)
        : api_key_(api_key), model_(model) {}
    
    core::Result<std::vector<float>> embed(const std::string& text) {
        Json::Value request_json;
        request_json["content"]["parts"][0]["text"] = text;
        
        Json::StreamWriterBuilder writer;
        std::string request_body = Json::writeString(writer, request_json);
        
        std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" + 
            model_ + ":embedContent?key=" + api_key_;
        
        auto response = HTTPClient::post(url, request_body, {});
        if (!response) {
            return core::Err("HTTP request failed: " + response.error());
        }
        
        auto resp = response.value();
        if (resp.status_code != 200) {
            return core::Err("Gemini API error: " + resp.body);
        }
        
        Json::CharReaderBuilder reader;
        Json::Value response_json;
        std::istringstream iss(resp.body);
        std::string errs;
        
        if (!Json::parseFromStream(reader, iss, &response_json, &errs)) {
            return core::Err("Failed to parse JSON response: " + errs);
        }
        
        if (!response_json.isMember("embedding") || !response_json["embedding"].isMember("values")) {
            return core::Err("Invalid response format");
        }
        
        std::vector<float> embedding;
        for (const auto& val : response_json["embedding"]["values"]) {
            embedding.push_back(val.asFloat());
        }
        
        spdlog::debug("[GeminiEmbeddingProvider] Generated embedding: dim={}", embedding.size());
        
        return core::Ok(embedding);
    }
    
    core::Result<std::vector<std::vector<float>>> embed_batch(const std::vector<std::string>& texts) {
        // Gemini 批量调用
        std::vector<std::vector<float>> embeddings;
        for (const auto& text : texts) {
            auto result = embed(text);
            if (!result) {
                return core::Err(result.error());
            }
            embeddings.push_back(result.value());
        }
        return core::Ok(embeddings);
    }
};

GeminiEmbeddingProvider::GeminiEmbeddingProvider(
    const std::string& api_key,
    const std::string& model
) : impl_(std::make_unique<Impl>(api_key, model)) {}

GeminiEmbeddingProvider::~GeminiEmbeddingProvider() = default;

core::Result<std::vector<float>> GeminiEmbeddingProvider::embed(const std::string& text) {
    return impl_->embed(text);
}

core::Result<std::vector<std::vector<float>>> GeminiEmbeddingProvider::embed_batch(
    const std::vector<std::string>& texts
) {
    return impl_->embed_batch(texts);
}

std::string GeminiEmbeddingProvider::model_name() const {
    return impl_->model_;
}

// ============================================================================
// Embedding Provider Factory
// ============================================================================

core::Result<std::shared_ptr<EmbeddingProvider>> EmbeddingProviderFactory::create(
    const std::string& type,
    const std::map<std::string, std::string>& config
) {
    if (type == "openai") {
        auto api_key_it = config.find("api_key");
        if (api_key_it == config.end()) {
            return core::Err("OpenAI provider requires 'api_key'");
        }
        
        std::string model = config.count("model") ? config.at("model") : "text-embedding-3-small";
        std::string base_url = config.count("base_url") ? config.at("base_url") : "https://api.openai.com/v1";
        
        return core::Ok<std::shared_ptr<EmbeddingProvider>>(
            std::make_shared<OpenAIEmbeddingProvider>(api_key_it->second, model, base_url)
        );
    }
    else if (type == "ollama") {
        std::string model = config.count("model") ? config.at("model") : "nomic-embed-text";
        std::string base_url = config.count("base_url") ? config.at("base_url") : "http://localhost:11434";
        
        return core::Ok<std::shared_ptr<EmbeddingProvider>>(
            std::make_shared<OllamaEmbeddingProvider>(model, base_url)
        );
    }
    else if (type == "gemini") {
        auto api_key_it = config.find("api_key");
        if (api_key_it == config.end()) {
            return core::Err("Gemini provider requires 'api_key'");
        }
        
        std::string model = config.count("model") ? config.at("model") : "text-embedding-004";
        
        return core::Ok<std::shared_ptr<EmbeddingProvider>>(
            std::make_shared<GeminiEmbeddingProvider>(api_key_it->second, model)
        );
    }
    else {
        return core::Err("Unknown embedding provider type: " + type);
    }
}

std::vector<std::string> EmbeddingProviderFactory::list_providers() {
    return {"openai", "ollama", "gemini"};
}

} // namespace memory
} // namespace hunterclaw
