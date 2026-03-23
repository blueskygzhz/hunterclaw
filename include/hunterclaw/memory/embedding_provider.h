#pragma once

#include <optional>
#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/chunking.h"
#include "hunterclaw/core/types.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

namespace hunterclaw {
namespace memory {

// EmbeddingInput is defined in chunking.h
// (no alias needed, use directly)

// 严格对齐 OpenClaw src/memory/embeddings.ts

// ============================================================================
// EmbeddingProvider - 基础接口 (embeddings.ts:29-36)
// ============================================================================
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    
    // Provider标识
    virtual std::string get_id() const = 0;
    
    // Model名称
    virtual std::string get_model() const = 0;
    
    // 最大输入token数 (可选)
    virtual std::optional<int> get_max_input_tokens() const { return std::nullopt; }
    
    // 单个文本embedding (embeddings.ts:33)
    virtual core::Result<std::vector<float>> embed_query(const std::string& text) = 0;
    
    // 批量文本embedding (embeddings.ts:34)
    virtual core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) = 0;
    
    // 批量输入embedding (可选,对齐embeddings.ts:35)
    virtual core::Result<std::vector<std::vector<float>>> embed_batch_inputs(
        const std::vector<EmbeddingInput>& inputs
    ) {
        // 默认实现: 提取text并调用embed_batch
        std::vector<std::string> texts;
        texts.reserve(inputs.size());
        for (const auto& input : inputs) {
            texts.push_back(input.text);
        }
        return embed_batch(texts);
    }
};

// ============================================================================
// EmbeddingProviderId (embeddings.ts:38-40)
// ============================================================================
enum class EmbeddingProviderId {
    OpenAI,   // "openai"
    Local,    // "local"
    Gemini,   // "gemini"
    Voyage,   // "voyage"
    Mistral,  // "mistral"
    Ollama    // "ollama"
};

inline std::string to_string(EmbeddingProviderId id) {
    switch (id) {
        case EmbeddingProviderId::OpenAI: return "openai";
        case EmbeddingProviderId::Local: return "local";
        case EmbeddingProviderId::Gemini: return "gemini";
        case EmbeddingProviderId::Voyage: return "voyage";
        case EmbeddingProviderId::Mistral: return "mistral";
        case EmbeddingProviderId::Ollama: return "ollama";
    }
    return "unknown";
}

inline std::optional<EmbeddingProviderId> parse_provider_id(const std::string& str) {
    if (str == "openai") return EmbeddingProviderId::OpenAI;
    if (str == "local") return EmbeddingProviderId::Local;
    if (str == "gemini") return EmbeddingProviderId::Gemini;
    if (str == "voyage") return EmbeddingProviderId::Voyage;
    if (str == "mistral") return EmbeddingProviderId::Mistral;
    if (str == "ollama") return EmbeddingProviderId::Ollama;
    return std::nullopt;
}

// ============================================================================
// GeminiTaskType (embeddings-gemini.ts)
// ============================================================================
enum class GeminiTaskType {
    RetrievalQuery,      // "retrieval_query"
    RetrievalDocument,   // "retrieval_document"
    SemanticSimilarity,  // "semantic_similarity"
    Classification,      // "classification"
    Clustering          // "clustering"
};

// Forward declarations for client types
class OpenAiEmbeddingClient;
class GeminiEmbeddingClient;
class OllamaEmbeddingClient;
class VoyageEmbeddingClient;
class MistralEmbeddingClient;

// EmbeddingProviderResult 在 types.h 中定义

// ============================================================================
// EmbeddingProviderOptions (embeddings.ts:60-79)
// ============================================================================
struct EmbeddingProviderOptions {
    std::string agent_dir;
    std::string provider;  // "auto" or specific provider id
    std::string model;
    std::string fallback;  // provider id or "none"
    
    // ✅ 对齐 OpenClaw: RemoteEmbeddingOptions (embeddings.ts:64-67)
    struct RemoteEmbeddingOptions {
        std::string base_url;   // API base URL
        std::string api_key;    // API key
        std::string model;      // model override
        int dimensions = 0;     // embedding dimensions
    };
    std::optional<RemoteEmbeddingOptions> remote;
    
    // ✅ 对齐 OpenClaw: LocalEmbeddingOptions (embeddings.ts:71-74)
    struct LocalEmbeddingOptions {
        std::string model_path;  // path to local model
        std::string model_type;  // "onnx", "llama", etc.
        int threads = 4;         // inference threads
    };
    std::optional<LocalEmbeddingOptions> local;
    
    // Gemini-specific options
    std::optional<int> output_dimensionality;  // 768, 1536, or 3072
    std::optional<GeminiTaskType> task_type;
};

// ============================================================================
// OpenAI Provider (embeddings-openai.ts)
// ============================================================================
class OpenAiEmbeddingClient {
public:
    struct Config {
        std::string base_url = "https://api.openai.com/v1";
        std::string api_key;
        std::string model = "text-embedding-3-small";
        std::map<std::string, std::string> headers;
        int dimensions = 1536;
        int max_input_tokens = 8191;
    };
    
    static core::Result<std::shared_ptr<OpenAiEmbeddingClient>> create(const Config& config);
    
    virtual ~OpenAiEmbeddingClient() = default;
    
    // 获取Provider接口
    virtual std::shared_ptr<EmbeddingProvider> get_provider() = 0;
    
    // Batch API (embeddings-openai.ts)
    virtual core::Result<std::string> create_batch(
        const std::vector<std::string>& texts,
        const std::string& description = ""
    ) = 0;
    
    virtual core::Result<std::string> get_batch_status(const std::string& batch_id) = 0;
    
    virtual core::Result<std::vector<std::vector<float>>> retrieve_batch_results(
        const std::string& batch_id
    ) = 0;
};

// ============================================================================
// Ollama Provider (embeddings-ollama.ts)
// ============================================================================
class OllamaEmbeddingClient {
public:
    struct Config {
        std::string base_url = "http://localhost:11434";
        std::string model = "nomic-embed-text";
        int max_input_tokens = 8192;
    };
    
    static core::Result<std::shared_ptr<OllamaEmbeddingClient>> create(const Config& config);
    
    virtual ~OllamaEmbeddingClient() = default;
    
    virtual std::shared_ptr<EmbeddingProvider> get_provider() = 0;
};

// ============================================================================
// Gemini Provider (embeddings-gemini.ts)
// ============================================================================
class GeminiEmbeddingClient {
public:
    struct Config {
        std::string base_url = "https://generativelanguage.googleapis.com/v1beta";
        std::string api_key;
        std::string model = "text-embedding-004";
        std::map<std::string, std::string> headers;
        int output_dimensionality = 768;
        GeminiTaskType task_type = GeminiTaskType::RetrievalDocument;
    };
    
    static core::Result<std::shared_ptr<GeminiEmbeddingClient>> create(const Config& config);
    
    virtual ~GeminiEmbeddingClient() = default;
    
    virtual std::shared_ptr<EmbeddingProvider> get_provider() = 0;
    
    // Batch API
    virtual core::Result<std::string> create_batch(
        const std::vector<std::string>& texts
    ) = 0;
    
    virtual core::Result<std::vector<std::vector<float>>> retrieve_batch_results(
        const std::string& batch_name
    ) = 0;
};

// ============================================================================
// Voyage Provider (embeddings-voyage.ts)
// ============================================================================
class VoyageEmbeddingClient {
public:
    struct Config {
        std::string base_url = "https://api.voyageai.com/v1";
        std::string api_key;
        std::string model = "voyage-3";
        std::map<std::string, std::string> headers;
        std::optional<int> output_dimensionality;
        std::string input_type = "document";
        int max_input_tokens = 32000;
    };
    
    static core::Result<std::shared_ptr<VoyageEmbeddingClient>> create(const Config& config);
    
    virtual ~VoyageEmbeddingClient() = default;
    
    virtual std::shared_ptr<EmbeddingProvider> get_provider() = 0;
};

// ============================================================================
// Mistral Provider (embeddings-mistral.ts)
// ============================================================================
class MistralEmbeddingClient {
public:
    struct Config {
        std::string base_url = "https://api.mistral.ai/v1";
        std::string api_key;
        std::string model = "mistral-embed";
        std::map<std::string, std::string> headers;
        int max_input_tokens = 8192;
    };
    
    static core::Result<std::shared_ptr<MistralEmbeddingClient>> create(const Config& config);
    
    virtual ~MistralEmbeddingClient() = default;
    
    virtual std::shared_ptr<EmbeddingProvider> get_provider() = 0;
};

// ============================================================================
// RemoteEmbeddingProvider - 统一的远程Provider (通用HTTP客户端)
// ============================================================================
class RemoteEmbeddingProvider : public EmbeddingProvider {
public:
    struct Config {
        EmbeddingProviderId provider_id;
        std::string base_url;
        std::string api_key;
        std::string model;
        std::map<std::string, std::string> headers;
        int dimensions = 1536;
        std::optional<int> max_input_tokens;
        std::optional<int> output_dimensionality;
    };
    
    static core::Result<std::shared_ptr<RemoteEmbeddingProvider>> create(const Config& config);

    virtual ~RemoteEmbeddingProvider() = default;

    // EmbeddingProvider interface - pure virtual, implemented by internal Impl class
    std::string get_id() const override = 0;
    std::string get_model() const override = 0;
    std::optional<int> get_max_input_tokens() const override = 0;
    core::Result<std::vector<float>> embed_query(const std::string& text) override = 0;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override = 0;

protected:
    RemoteEmbeddingProvider() = default;
};

// ============================================================================
// EmbeddingProviderFactory - 工厂类 (embeddings.ts:168-269)
// ============================================================================
class EmbeddingProviderFactory {
public:
    // 创建Provider (对齐 createEmbeddingProvider)
    static core::Result<EmbeddingProviderResult> create(
        const EmbeddingProviderOptions& options
    );
    
private:
    // "auto" 模式的远程Provider优先级 (embeddings.ts:45)
    static const std::vector<EmbeddingProviderId> REMOTE_PROVIDER_IDS;
    
    // 创建特定Provider
    static core::Result<EmbeddingProviderResult> create_provider(
        EmbeddingProviderId id,
        const EmbeddingProviderOptions& options
    );
    
    // Local provider
    static core::Result<std::shared_ptr<EmbeddingProvider>> create_local_provider(
        const EmbeddingProviderOptions& options
    );
    
    // 检查是否可以自动选择Local
    static bool can_auto_select_local(const EmbeddingProviderOptions& options);
    
    // 错误检测
    static bool is_missing_api_key_error(const std::string& error_msg);
};

// ============================================================================
// 工具函数
// ============================================================================

// Default local model (embeddings.ts:81-82)
constexpr const char* DEFAULT_LOCAL_MODEL = 
    "hf:ggml-org/embeddinggemma-300m-qat-q8_0-GGUF/embeddinggemma-300m-qat-Q8_0.gguf";

// Sanitize and normalize embedding vector (embedding-vectors.ts)
std::vector<float> sanitize_and_normalize_embedding(const std::vector<float>& embedding);

}  // namespace memory
}  // namespace hunterclaw
