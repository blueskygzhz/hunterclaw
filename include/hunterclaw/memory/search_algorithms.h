#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/types.h"
#include <vector>
#include <string>

namespace hunterclaw {
namespace memory {

// 严格对齐 OpenClaw src/memory/hybrid.ts

// ============================================================================
// HybridSearchEngine - 混合检索引擎
// ============================================================================
class HybridSearchEngine {
public:
    // ========================================================================
    // FTS Query构建 (对齐 buildFtsQuery)
    // ========================================================================
    
    // 从查询字符串构建FTS5查询
    // 支持: 词项、短语、AND/OR/NOT逻辑
    static std::string build_fts_query(const std::string& query);
    
    // 提取关键词 (对齐 extractKeywords in query-expansion.ts)
    static std::vector<std::string> extract_keywords(const std::string& query);
    
    // ========================================================================
    // 结果合并 (对齐 mergeHybridResults)
    // ========================================================================
    
    // 使用RRF (Reciprocal Rank Fusion) 合并向量和全文搜索结果
    static std::vector<MemorySearchResult> merge_results(
        const std::vector<MemorySearchResult>& vector_results,
        const std::vector<MemorySearchResult>& keyword_results,
        const HybridConfig& config
    );
    
    // ========================================================================
    // RRF算法 (Reciprocal Rank Fusion)
    // ========================================================================
    
    // 计算RRF分数
    // score = 1 / (k + rank), 其中k通常取60
    static double reciprocal_rank_fusion_score(int rank, double k = 60.0);
    
    // ========================================================================
    // BM25分数转换 (对齐 bm25RankToScore)
    // ========================================================================
    
    // 将BM25 rank转换为归一化分数 [0, 1]
    static double bm25_rank_to_score(double rank);
    
private:
    // 辅助方法: 规范化分词
    static std::vector<std::string> tokenize(const std::string& text);
    
    // 辅助方法: 去除停用词
    static std::vector<std::string> remove_stopwords(const std::vector<std::string>& tokens);
};

// ============================================================================
// MMRReranker - MMR多样性重排 (对齐 mmr.ts)
// ============================================================================
class MMRReranker {
public:
    // ========================================================================
    // MMR重排算法
    // ========================================================================
    
    // 对搜索结果应用MMR重排
    // lambda: 相关性vs多样性权重 (0.0-1.0, 越大越重视相关性)
    // - lambda = 1.0: 纯相关性排序
    // - lambda = 0.0: 纯多样性排序
    // - lambda = 0.7: OpenClaw默认值
    static std::vector<MemorySearchResult> rerank(
        const std::vector<MemorySearchResult>& candidates,
        const std::vector<float>& query_embedding,
        double lambda = 0.7,
        int max_results = 6
    );
    
    // ========================================================================
    // 相似度计算
    // ========================================================================
    
    // 余弦相似度
    static double cosine_similarity(
        const std::vector<float>& a,
        const std::vector<float>& b
    );
    
    // 点积
    static double dot_product(
        const std::vector<float>& a,
        const std::vector<float>& b
    );
    
    // 欧式距离
    static double euclidean_distance(
        const std::vector<float>& a,
        const std::vector<float>& b
    );
    
    // ========================================================================
    // 向量归一化
    // ========================================================================
    
    // L2归一化
    static std::vector<float> normalize(const std::vector<float>& vec);
    
    // 检查是否已归一化
    static bool is_normalized(const std::vector<float>& vec, double tolerance = 1e-6);
    
private:
    // 计算候选结果与已选结果的最大相似度
    static double max_similarity_to_selected(
        const MemorySearchResult& candidate,
        const std::vector<MemorySearchResult>& selected
    );
    
    // MMR分数计算
    // score = lambda * relevance - (1 - lambda) * max_similarity
    static double compute_mmr_score(
        double relevance,
        double max_similarity,
        double lambda
    );
};

// ============================================================================
// TemporalDecayFilter - 时间衰减过滤器 (对齐 temporal-decay.ts)
// ============================================================================
class TemporalDecayFilter {
public:
    // ========================================================================
    // 时间衰减应用
    // ========================================================================
    
    // 对搜索结果应用时间衰减
    // half_life_days: 半衰期(天数)
    // - 例如 half_life_days=30, 30天前的结果分数衰减50%
    static std::vector<MemorySearchResult> apply_decay(
        const std::vector<MemorySearchResult>& results,
        double half_life_days = 30.0
    );
    
    // ========================================================================
    // 衰减因子计算
    // ========================================================================
    
    // 计算时间衰减因子
    // decay_factor = 2^(-days_elapsed / half_life_days)
    // - 0天前: factor = 1.0 (100%)
    // - half_life天前: factor = 0.5 (50%)
    // - 2*half_life天前: factor = 0.25 (25%)
    static double compute_decay_factor(
        int64_t timestamp,
        double half_life_days
    );
    
    // 获取当前时间戳(秒)
    static int64_t current_timestamp();
    
    // 计算两个时间戳之间的天数差
    static double days_between(int64_t timestamp1, int64_t timestamp2);
    
private:
    // 应用衰减因子到分数
    static double apply_decay_to_score(double original_score, double decay_factor);
};

// ============================================================================
// 完整搜索管道 (对齐 manager-search.ts 的搜索流程)
// ============================================================================
class SearchPipeline {
public:
    struct Config {
        // 混合检索配置
        HybridConfig hybrid;
        
        // 最大结果数
        int max_results = 6;
        
        // 最小分数阈值
        double min_score = 0.35;
    };
    
    // 执行完整搜索管道
    // 1. 向量搜索 + 全文搜索
    // 2. 混合结果合并 (RRF)
    // 3. MMR重排 (如果启用)
    // 4. 时间衰减 (如果启用)
    // 5. 分数过滤 + 截断
    static std::vector<MemorySearchResult> execute(
        const std::vector<MemorySearchResult>& vector_results,
        const std::vector<MemorySearchResult>& keyword_results,
        const std::vector<float>& query_embedding,
        const Config& config
    );
    
private:
    // 应用MMR重排
    static std::vector<MemorySearchResult> apply_mmr_if_enabled(
        const std::vector<MemorySearchResult>& results,
        const std::vector<float>& query_embedding,
        const HybridConfig::MMRConfig& mmr_config,
        int max_results
    );
    
    // 应用时间衰减
    static std::vector<MemorySearchResult> apply_temporal_decay_if_enabled(
        const std::vector<MemorySearchResult>& results,
        const HybridConfig::TemporalDecayConfig& decay_config
    );
    
    // 过滤和截断
    static std::vector<MemorySearchResult> filter_and_truncate(
        const std::vector<MemorySearchResult>& results,
        double min_score,
        int max_results
    );
};

} // namespace memory
} // namespace hunterclaw
