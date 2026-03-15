#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>

namespace hunterclaw {
namespace memory {

// Forward declarations
class EmbeddingProvider;
class MemoryStorage;

// ============================================================================
// SearchResult - 搜索结果
// ============================================================================
struct SearchResult {
    std::string id;                    // Memory ID
    std::string content;               // 内容
    double score;                      // 相关性分数 (0-1)
    std::map<std::string, std::string> metadata;  // 元数据
    std::optional<std::vector<float>> embedding;  // Embedding向量
    int64_t timestamp;                 // 时间戳
};

// ============================================================================
// HybridSearchConfig - 混合搜索配置
// ============================================================================
struct HybridSearchConfig {
    bool enabled = true;
    double vector_weight = 0.7;        // 向量搜索权重
    double text_weight = 0.3;          // 全文搜索权重
    int candidate_multiplier = 4;      // 候选结果倍数
    
    struct MMRConfig {
        bool enabled = true;
        double lambda = 0.7;           // 相关性 vs 多样性 (0=多样性, 1=相关性)
    } mmr;
    
    struct TemporalDecayConfig {
        bool enabled = false;
        double half_life_days = 30.0;  // 半衰期(天)
    } temporal_decay;
};

// ============================================================================
// SearchConfig - 搜索配置
// ============================================================================
struct SearchConfig {
    int max_results = 6;               // 最大返回结果数
    double min_score = 0.35;           // 最小相关性分数
    HybridSearchConfig hybrid;         // 混合搜索配置
};

// ============================================================================
// MemoryManager - 统一的记忆管理器
// 
// 整合向量搜索、全文搜索、混合排名、MMR重排、时间衰减等功能
// ============================================================================
class MemoryManager {
public:
    // 创建MemoryManager实例
    static std::shared_ptr<MemoryManager> create(
        std::shared_ptr<MemoryStorage> storage,
        std::shared_ptr<EmbeddingProvider> embedding,
        const SearchConfig& config = SearchConfig{}
    );
    
    // ========================================
    // Memory CRUD Operations
    // ========================================
    
    // 添加记忆
    // @param content 记忆内容
    // @param metadata 元数据
    // @return Memory ID
    std::string add_memory(
        const std::string& content,
        const std::map<std::string, std::string>& metadata = {}
    );
    
    // 搜索记忆 (混合搜索: 向量 + 全文 + MMR + 时间衰减)
    // @param query 查询字符串
    // @return 搜索结果列表(按相关性排序)
    std::vector<SearchResult> search(const std::string& query);
    
    // 删除记忆
    // @param id Memory ID
    // @return 是否成功
    bool remove_memory(const std::string& id);
    
    // 更新搜索配置
    void update_config(const SearchConfig& config);
    
private:
    MemoryManager() = default;
    
    // 混合排名 (向量 + 文本)
    std::vector<SearchResult> hybrid_rank(
        const std::vector<SearchResult>& vector_results,
        const std::vector<SearchResult>& text_results
    );
    
    // Maximal Marginal Relevance (增加多样性)
    std::vector<SearchResult> maximal_marginal_relevance(
        const std::vector<SearchResult>& candidates,
        const std::vector<float>& query_embedding
    );
    
    // 时间衰减
    std::vector<SearchResult> apply_temporal_decay(
        const std::vector<SearchResult>& results
    );
    
    std::shared_ptr<MemoryStorage> storage_;
    std::shared_ptr<EmbeddingProvider> embedding_;
    SearchConfig config_;
};

// ============================================================================
// MemoryStorage Interface - 底层存储接口
// ============================================================================
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
    
    // 获取记忆
    virtual std::optional<SearchResult> get(const std::string& id) = 0;
};

// ============================================================================
// Similarity Functions - 相似度计算函数
// ============================================================================
namespace similarity {

// 余弦相似度 (0-1, 1=最相似)
double cosine(const std::vector<float>& a, const std::vector<float>& b);

// 欧氏距离 (0-∞, 0=最相似)
double euclidean(const std::vector<float>& a, const std::vector<float>& b);

// 点积 (-∞ to +∞)
double dot_product(const std::vector<float>& a, const std::vector<float>& b);

} // namespace similarity

} // namespace memory
} // namespace hunterclaw
