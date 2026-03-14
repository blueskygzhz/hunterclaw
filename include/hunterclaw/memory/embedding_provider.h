#pragma once

#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace hunterclaw {
namespace memory {

/**
 * 🎯 Embedding Provider 接口
 * 
 * 对齐 OpenClaw Embedding 能力:
 * - OpenAI (text-embedding-3-small, text-embedding-3-large, text-embedding-ada-002)
 * - Ollama (nomic-embed-text, mxbai-embed-large)
 * - Gemini (text-embedding-004)
 * - Voyage AI (voyage-3, voyage-3-lite)
 * - Cohere (embed-english-v3.0, embed-multilingual-v3.0)
 * 
 * 使用示例:
 * 
 * // 创建 OpenAI embedding provider
 * auto provider = EmbeddingProviderFactory::create("openai", {
 *     {"model", "text-embedding-3-small"},
 *     {"api_key", "sk-..."}
 * });
 * 
 * // 获取单个文本的 embedding
 * auto result = provider->embed("Hello world");
 * if (result) {
 *     auto embedding = result.value();  // std::vector<float>
 * }
 * 
 * // 批量获取 embeddings
 * auto batch_result = provider->embed_batch({"text1", "text2", "text3"});
 */
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    
    // ========================================
    // 核心 API
    // ========================================
    
    /**
     * 获取单个文本的 embedding
     * @param text 输入文本
     * @return embedding 向量 (float array)
     */
    virtual core::Result<std::vector<float>> embed(const std::string& text) = 0;
    
    /**
     * 批量获取 embeddings
     * @param texts 输入文本列表
     * @return embeddings 向量列表
     */
    virtual core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) = 0;
    
    // ========================================
    // 元信息
    // ========================================
    
    /**
     * Provider 类型
     * @return "openai", "ollama", "gemini", "voyage", "cohere"
     */
    virtual std::string provider_type() const = 0;
    
    /**
     * 模型名称
     * @return 模型名 (例如: "text-embedding-3-small")
     */
    virtual std::string model_name() const = 0;
    
    /**
     * Embedding 维度
     * @return 向量维度 (例如: 1536)
     */
    virtual int dimension() const = 0;
    
    /**
     * 支持的最大 token 数
     * @return 最大 token 数 (例如: 8191)
     */
    virtual int max_tokens() const = 0;
};

/**
 * 🎯 OpenAI Embedding Provider
 * 
 * 支持的模型:
 * - text-embedding-3-small (1536 维, 性价比高)
 * - text-embedding-3-large (3072 维, 性能最佳)
 * - text-embedding-ada-002 (1536 维, 旧版模型)
 */
class OpenAIEmbeddingProvider : public EmbeddingProvider {
public:
    /**
     * 构造函数
     * @param api_key OpenAI API Key
     * @param model 模型名 (默认: text-embedding-3-small)
     * @param base_url API Base URL (默认: https://api.openai.com/v1)
     */
    OpenAIEmbeddingProvider(
        const std::string& api_key,
        const std::string& model = "text-embedding-3-small",
        const std::string& base_url = "https://api.openai.com/v1"
    );
    
    ~OpenAIEmbeddingProvider() override;
    
    core::Result<std::vector<float>> embed(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
    
    std::string provider_type() const override { return "openai"; }
    std::string model_name() const override;
    int dimension() const override;
    int max_tokens() const override { return 8191; }
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * 🎯 Ollama Embedding Provider
 * 
 * 支持的模型:
 * - nomic-embed-text (768 维, 开源最佳)
 * - mxbai-embed-large (1024 维, 高性能)
 * - all-minilm (384 维, 轻量级)
 */
class OllamaEmbeddingProvider : public EmbeddingProvider {
public:
    /**
     * 构造函数
     * @param model 模型名 (默认: nomic-embed-text)
     * @param base_url Ollama Base URL (默认: http://localhost:11434)
     */
    OllamaEmbeddingProvider(
        const std::string& model = "nomic-embed-text",
        const std::string& base_url = "http://localhost:11434"
    );
    
    ~OllamaEmbeddingProvider() override;
    
    core::Result<std::vector<float>> embed(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
    
    std::string provider_type() const override { return "ollama"; }
    std::string model_name() const override;
    int dimension() const override;
    int max_tokens() const override { return 2048; }
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * 🎯 Gemini Embedding Provider
 * 
 * 支持的模型:
 * - text-embedding-004 (768 维, Google 最新模型)
 */
class GeminiEmbeddingProvider : public EmbeddingProvider {
public:
    /**
     * 构造函数
     * @param api_key Gemini API Key
     * @param model 模型名 (默认: text-embedding-004)
     */
    GeminiEmbeddingProvider(
        const std::string& api_key,
        const std::string& model = "text-embedding-004"
    );
    
    ~GeminiEmbeddingProvider() override;
    
    core::Result<std::vector<float>> embed(const std::string& text) override;
    core::Result<std::vector<std::vector<float>>> embed_batch(
        const std::vector<std::string>& texts
    ) override;
    
    std::string provider_type() const override { return "gemini"; }
    std::string model_name() const override;
    int dimension() const override { return 768; }
    int max_tokens() const override { return 2048; }
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * 🎯 Embedding Provider 工厂
 * 
 * 使用示例:
 * 
 * // 创建 OpenAI provider
 * auto provider = EmbeddingProviderFactory::create("openai", {
 *     {"model", "text-embedding-3-small"},
 *     {"api_key", "sk-..."}
 * });
 * 
 * // 创建 Ollama provider
 * auto provider = EmbeddingProviderFactory::create("ollama", {
 *     {"model", "nomic-embed-text"},
 *     {"base_url", "http://localhost:11434"}
 * });
 */
class EmbeddingProviderFactory {
public:
    /**
     * 创建 Embedding Provider
     * @param type Provider 类型 ("openai", "ollama", "gemini", "voyage")
     * @param config 配置参数
     * @return Embedding Provider 实例
     */
    static core::Result<std::shared_ptr<EmbeddingProvider>> create(
        const std::string& type,
        const std::map<std::string, std::string>& config
    );
    
    /**
     * 列出支持的 Provider 类型
     * @return Provider 类型列表
     */
    static std::vector<std::string> list_providers();
};

} // namespace memory
} // namespace hunterclaw
