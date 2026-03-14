#pragma once

#include "hunterclaw/core/types.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// Embedding Provider Interface
// ============================================================================

struct EmbeddingInput {
    std::string text;
    // 多模态支持 (预留)
    // std::vector<MultimodalPart> parts;
};

class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    
    // 提供商信息
    virtual std::string id() const = 0;
    virtual std::string model() const = 0;
    virtual int max_input_tokens() const { return 8192; }
    
    // 单个查询嵌入
    virtual core::Result<std::vector<float>> embed_query(const std::string& text) = 0;
    
    // 批量嵌入
    virtual core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) = 0;
};

// ============================================================================
// OpenAI Embedding Provider
// ============================================================================

class OpenAIEmbeddingProvider : public EmbeddingProvider {
public:
    explicit OpenAIEmbeddingProvider(
        const std::string& api_key,
        const std::string& model = "text-embedding-3-small",
        const std::string& api_base = "https://api.openai.com/v1"
    );
    
    std::string id() const override { return "openai"; }
    std::string model() const override { return model_; }
    int max_input_tokens() const override { return 8192; }
    
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
    
private:
    std::string api_key_;
    std::string model_;
    std::string api_base_;
};

// ============================================================================
// Ollama Embedding Provider (Local)
// ============================================================================

class OllamaEmbeddingProvider : public EmbeddingProvider {
public:
    explicit OllamaEmbeddingProvider(
        const std::string& model = "nomic-embed-text",
        const std::string& api_base = "http://localhost:11434"
    );
    
    std::string id() const override { return "ollama"; }
    std::string model() const override { return model_; }
    int max_input_tokens() const override { return 2048; }
    
    core::Result<std::vector<float>> embed_query(const std::string& text) override;
    
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
    
private:
    std::string model_;
    std::string api_base_;
};

// ============================================================================
// Embedding Cache
// ============================================================================

class EmbeddingCache {
public:
    explicit EmbeddingCache(size_t max_entries = 50000);
    
    // 获取缓存
    core::Result<std::vector<float>> get(
        const std::string& provider_id,
        const std::string& model,
        const std::string& text_hash
    );
    
    // 设置缓存
    void put(
        const std::string& provider_id,
        const std::string& model,
        const std::string& text_hash,
        const std::vector<float>& embedding
    );
    
    // 清除缓存
    void clear();
    
    // 统计
    size_t size() const;
    
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================================
// Vector Utilities
// ============================================================================

namespace vector_utils {

// 余弦相似度
double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b);

// 欧氏距离
double euclidean_distance(const std::vector<float>& a, const std::vector<float>& b);

// L2 归一化
std::vector<float> normalize(const std::vector<float>& vec);

// 向量序列化 (to BLOB)
std::vector<uint8_t> serialize(const std::vector<float>& vec);

// 向量反序列化 (from BLOB)
std::vector<float> deserialize(const uint8_t* data, size_t size);

// 计算文本哈希 (SHA256)
std::string compute_hash(const std::string& text);

} // namespace vector_utils

// ============================================================================
// Embedding Provider Registry
// ============================================================================

class EmbeddingProviderRegistry {
public:
    static EmbeddingProviderRegistry& instance();
    
    // 注册提供商
    void register_provider(const std::string& id, std::shared_ptr<EmbeddingProvider> provider);
    
    // 获取提供商
    core::Result<std::shared_ptr<EmbeddingProvider>> get_provider(const std::string& id);
    
    // 列出所有提供商
    std::vector<std::string> list_providers() const;
    
private:
    EmbeddingProviderRegistry() = default;
    
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace memory
} // namespace hunterclaw
