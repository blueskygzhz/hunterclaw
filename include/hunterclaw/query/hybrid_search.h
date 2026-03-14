#pragma once

#include "hunterclaw/query/parser.h"
#include "hunterclaw/core/result.h"
#include <vector>
#include <string>
#include <functional>

namespace hunterclaw {
namespace query {

// ============================================================================
// QMD Hybrid Search - BM25 + Vector + MMR
// ============================================================================

struct SearchResult {
    std::string doc_id;
    std::string content;
    float score;
    std::map<std::string, std::string> metadata;
    int64_t timestamp;
};

struct HybridSearchConfig {
    float vector_weight = 0.7f;
    float text_weight = 0.3f;
    int candidate_multiplier = 3;  // Expand candidate pool
    
    // Temporal decay
    bool temporal_decay_enabled = false;
    int64_t half_life_ms = 7 * 24 * 3600 * 1000;  // 1 week
    
    // MMR diversity
    bool mmr_enabled = false;
    float mmr_lambda = 0.7f;  // Balance relevance vs diversity
};

// BM25 ranker
class BM25Ranker {
public:
    BM25Ranker(float k1 = 1.2f, float b = 0.75f);
    
    // Calculate BM25 score
    float score(
        const std::string& query,
        const std::string& document,
        float avg_doc_length,
        const std::map<std::string, int>& term_frequencies
    ) const;
    
    // Normalize Chinese query (extract keywords)
    static std::string normalize_han_query(const std::string& query);
    
private:
    float k1_;
    float b_;
};

// Vector similarity calculator
class VectorSimilarity {
public:
    // Cosine similarity
    static float cosine(
        const std::vector<float>& vec1,
        const std::vector<float>& vec2
    );
    
    // Dot product
    static float dot_product(
        const std::vector<float>& vec1,
        const std::vector<float>& vec2
    );
    
    // L2 distance
    static float l2_distance(
        const std::vector<float>& vec1,
        const std::vector<float>& vec2
    );
};

// MMR (Maximal Marginal Relevance) reranker
class MMRReranker {
public:
    // Rerank results to maximize diversity
    static std::vector<SearchResult> rerank(
        const std::vector<SearchResult>& candidates,
        const std::vector<std::vector<float>>& embeddings,
        float lambda,  // Relevance vs diversity trade-off
        size_t top_n
    );
    
private:
    static float calculate_max_similarity(
        const std::vector<float>& candidate_embedding,
        const std::vector<std::vector<float>>& selected_embeddings
    );
};

// Hybrid search engine
class HybridSearchEngine {
public:
    HybridSearchEngine(const HybridSearchConfig& config = HybridSearchConfig());
    
    // Perform hybrid search (BM25 + Vector)
    core::Result<std::vector<SearchResult>> search(
        const std::string& query,
        std::function<std::vector<SearchResult>(const std::string&, size_t)> bm25_search,
        std::function<std::vector<SearchResult>(const std::vector<float>&, size_t)> vector_search,
        std::function<std::vector<float>(const std::string&)> get_embedding,
        size_t top_n
    );
    
    // Three search modes
    core::Result<std::vector<SearchResult>> search_bm25_only(
        const std::string& query,
        std::function<std::vector<SearchResult>(const std::string&, size_t)> bm25_search,
        size_t top_n
    );
    
    core::Result<std::vector<SearchResult>> search_vector_only(
        const std::string& query,
        std::function<std::vector<float>(const std::string&)> get_embedding,
        std::function<std::vector<SearchResult>(const std::vector<float>&, size_t)> vector_search,
        size_t top_n
    );
    
    core::Result<std::vector<SearchResult>> search_hybrid(
        const std::string& query,
        std::function<std::vector<SearchResult>(const std::string&, size_t)> bm25_search,
        std::function<std::vector<SearchResult>(const std::vector<float>&, size_t)> vector_search,
        std::function<std::vector<float>(const std::string&)> get_embedding,
        size_t top_n
    );
    
    // Configuration
    void set_config(const HybridSearchConfig& config) { config_ = config; }
    const HybridSearchConfig& config() const { return config_; }
    
private:
    HybridSearchConfig config_;
    
    // Merge and rerank results
    std::vector<SearchResult> merge_results(
        const std::vector<SearchResult>& bm25_results,
        const std::vector<SearchResult>& vector_results,
        int64_t current_time_ms
    );
    
    // Apply temporal decay
    float apply_temporal_decay(float score, int64_t timestamp, int64_t current_time_ms);
};

}  // namespace query
}  // namespace hunterclaw
