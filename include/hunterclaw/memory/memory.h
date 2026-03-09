#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>

namespace hunterclaw {
namespace memory {

// Embedding Provider接口
class EmbeddingProvider {
public:
    virtual ~EmbeddingProvider() = default;
    
    // 生成单个查询的embedding
    virtual std::vector<float> embed_query(const std::string& text) = 0;
    
    // 批量生成embedding
    virtual std::vector<std::vector<float>> embed_batch(const std::vector<std::string>& texts) = 0;
    
    // Provider信息
    virtual std::string id() const = 0;
    virtual std::string model() const = 0;
    virtual int dimensions() const = 0;
};

// 搜索结果
struct SearchResult {
    std::string id;
    std::string content;
    std::string source;  // 来源文件/会话
    double score;
    int64_t timestamp;
    std::map<std::string, std::string> metadata;
    std::vector<float> embedding;  // 用于MMR重排
};

// 搜索配置
struct SearchConfig {
    int max_results = 6;
    double min_score = 0.35;
    
    // 混合检索
    struct HybridConfig {
        bool enabled = true;
        double vector_weight = 0.7;
        double text_weight = 0.3;
        int candidate_multiplier = 4;
        
        // MMR多样性
        struct MMRConfig {
            bool enabled = true;
            double lambda = 0.7;  // 相关性vs多样性权重
        } mmr;
        
        // 时间衰减
        struct TemporalDecayConfig {
            bool enabled = false;
            double half_life_days = 30.0;
        } temporal_decay;
    } hybrid;
    
    // 分块配置
    struct ChunkingConfig {
        int tokens = 400;
        int overlap = 80;
    } chunking;
};

// Memory Storage接口
class MemoryStorage {
public:
    virtual ~MemoryStorage() = default;
    
    // 添加记忆
    virtual std::string add(
        const std::string& content,
        const std::vector<float>& embedding,
        const std::map<std::string, std::string>& metadata
    ) = 0;
    
    // 向量搜索
    virtual std::vector<SearchResult> vector_search(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score
    ) = 0;
    
    // 全文搜索 (BM25)
    virtual std::vector<SearchResult> text_search(
        const std::string& query,
        int limit
    ) = 0;
    
    // 删除记忆
    virtual bool remove(const std::string& id) = 0;
    
    // 清空存储
    virtual void clear() = 0;
};

// Memory Manager
class MemoryManager {
public:
    static std::shared_ptr<MemoryManager> create(
        std::shared_ptr<MemoryStorage> storage,
        std::shared_ptr<EmbeddingProvider> embedding,
        const SearchConfig& config = SearchConfig()
    );
    
    // 添加记忆
    std::string add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    );
    
    // 搜索记忆
    std::vector<SearchResult> search(const std::string& query);
    
    // 删除记忆
    bool remove_memory(const std::string& id);
    
    // 更新配置
    void update_config(const SearchConfig& config);

private:
    std::shared_ptr<MemoryStorage> storage_;
    std::shared_ptr<EmbeddingProvider> embedding_;
    SearchConfig config_;
    
    // 混合排名
    std::vector<SearchResult> hybrid_rank(
        const std::vector<SearchResult>& vector_results,
        const std::vector<SearchResult>& text_results
    );
    
    // MMR重排
    std::vector<SearchResult> maximal_marginal_relevance(
        const std::vector<SearchResult>& candidates,
        const std::vector<float>& query_embedding
    );
    
    // 时间衰减
    std::vector<SearchResult> apply_temporal_decay(
        const std::vector<SearchResult>& results
    );
};

// SQLite Vector Storage (使用sqlite-vec扩展)
class SQLiteVectorStorage : public MemoryStorage {
public:
    static std::shared_ptr<SQLiteVectorStorage> create(
        const std::string& db_path,
        int dimensions = 1536
    );
    
    std::string add(
        const std::string& content,
        const std::vector<float>& embedding,
        const std::map<std::string, std::string>& metadata
    ) override;
    
    std::vector<SearchResult> vector_search(
        const std::vector<float>& query_embedding,
        int limit,
        double min_score
    ) override;
    
    std::vector<SearchResult> text_search(
        const std::string& query,
        int limit
    ) override;
    
    bool remove(const std::string& id) override;
    void clear() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Embedding Provider工厂
class EmbeddingProviderFactory {
public:
    // OpenAI Embeddings
    static std::shared_ptr<EmbeddingProvider> create_openai(
        const std::string& api_key,
        const std::string& model = "text-embedding-3-small",
        const std::string& base_url = "https://api.openai.com/v1"
    );
    
    // Ollama Embeddings
    static std::shared_ptr<EmbeddingProvider> create_ollama(
        const std::string& model = "nomic-embed-text",
        const std::string& base_url = "http://localhost:11434"
    );
    
    // 本地模型 (TODO: 集成sentence-transformers)
    static std::shared_ptr<EmbeddingProvider> create_local(
        const std::string& model_path
    );
};

// 相似度计算工具
namespace similarity {
    // 余弦相似度
    double cosine(const std::vector<float>& a, const std::vector<float>& b);
    
    // 欧式距离
    double euclidean(const std::vector<float>& a, const std::vector<float>& b);
    
    // 点积
    double dot_product(const std::vector<float>& a, const std::vector<float>& b);
}

} // namespace memory
} // namespace hunterclaw
