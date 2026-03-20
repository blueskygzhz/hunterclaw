#pragma once

// 严格对齐 OpenClaw src/memory/hybrid.ts + mmr.ts

#include "hunterclaw/memory/types.h"
#include "hunterclaw/memory/temporal_decay.h"
#include "hunterclaw/core/types.h"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

// ============================================================================
// HybridVectorResult (对齐 hybrid.ts:HybridVectorResult)
// ============================================================================
struct HybridVectorResult {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string source;
    std::string snippet;
    double vector_score = 0.0;
};

// ============================================================================
// HybridKeywordResult (对齐 hybrid.ts:HybridKeywordResult)
// ============================================================================
struct HybridKeywordResult {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string source;
    std::string snippet;
    double text_score = 0.0;
};

// ============================================================================
// HybridMergedResult (对齐 hybrid.ts:mergeHybridResults 返回类型)
// ============================================================================
struct HybridMergedResult {
    std::string path;
    int start_line = 0;
    int end_line = 0;
    double score = 0.0;
    std::string snippet;
    std::string source;
};

// ============================================================================
// MMRConfig (对齐 mmr.ts:MMRConfig)
// ============================================================================
struct MMRConfig {
    bool enabled = false;   // 对齐 openclaw: default false (opt-in)
    double lambda = 0.7;    // 对齐 openclaw: default 0.7
};

const MMRConfig DEFAULT_MMR_CONFIG = {false, 0.7};

// ============================================================================
// MMR 核心函数 (对齐 mmr.ts)
// ============================================================================

/**
 * Tokenize text for Jaccard similarity (对齐 mmr.ts:tokenize)
 * Extracts alphanumeric tokens, normalizes to lowercase.
 */
std::set<std::string> tokenize(const std::string& text);

/**
 * Compute Jaccard similarity between two token sets (对齐 mmr.ts:jaccardSimilarity)
 */
double jaccard_similarity(const std::set<std::string>& set_a, const std::set<std::string>& set_b);

/**
 * Compute text similarity using Jaccard on tokens (对齐 mmr.ts:textSimilarity)
 */
double text_similarity(const std::string& content_a, const std::string& content_b);

/**
 * Compute MMR score (对齐 mmr.ts:computeMMRScore)
 * score = lambda * relevance - (1-lambda) * max_similarity
 */
double compute_mmr_score(double relevance, double max_similarity, double lambda);

/**
 * MMR item for re-ranking (对齐 mmr.ts:MMRItem)
 */
struct MMRItem {
    std::string id;
    double score = 0.0;
    std::string content;  // snippet text for Jaccard similarity
};

/**
 * Re-rank items using MMR (对齐 mmr.ts:mmrRerank)
 * Uses Jaccard similarity on token sets (NOT cosine similarity on embeddings)
 */
std::vector<MMRItem> mmr_rerank(
    std::vector<MMRItem> items,
    const MMRConfig& config = DEFAULT_MMR_CONFIG
);

/**
 * Apply MMR to hybrid search results (对齐 mmr.ts:applyMMRToHybridResults)
 */
std::vector<HybridMergedResult> apply_mmr_to_hybrid_results(
    std::vector<HybridMergedResult> results,
    const MMRConfig& config = DEFAULT_MMR_CONFIG
);

// ============================================================================
// Hybrid search functions (对齐 hybrid.ts)
// ============================================================================

/**
 * Build FTS5 query from raw query string (对齐 hybrid.ts:buildFtsQuery)
 * Returns empty string if no valid tokens.
 */
std::string build_fts_query(const std::string& raw);

/**
 * Convert BM25 rank to normalized score (对齐 hybrid.ts:bm25RankToScore)
 */
double bm25_rank_to_score(double rank);

/**
 * Merge hybrid vector + keyword results (对齐 hybrid.ts:mergeHybridResults)
 * Applies temporal decay and MMR re-ranking.
 */
std::vector<HybridMergedResult> merge_hybrid_results(
    const std::vector<HybridVectorResult>& vector_results,
    const std::vector<HybridKeywordResult>& keyword_results,
    double vector_weight,
    double text_weight,
    const std::string& workspace_dir = "",
    const MMRConfig& mmr = DEFAULT_MMR_CONFIG,
    const TemporalDecayConfig& temporal_decay = DEFAULT_TEMPORAL_DECAY_CONFIG,
    int64_t now_ms = 0
);

// ============================================================================
// SearchPipeline (对齐 manager-search.ts 搜索流程)
// ============================================================================
class SearchPipeline {
public:
    struct Config {
        HybridConfig hybrid;
        int max_results = 6;
        double min_score = 0.35;
    };

    /**
     * Execute full search pipeline:
     * 1. Merge hybrid results (vector + keyword)
     * 2. Apply temporal decay (if enabled)
     * 3. Apply MMR re-ranking (if enabled)
     * 4. Filter by min score + truncate
     */
    static std::vector<MemorySearchResult> execute(
        const std::vector<MemorySearchResult>& vector_results,
        const std::vector<MemorySearchResult>& keyword_results,
        const std::vector<float>& query_embedding,
        const Config& config
    );
};

} // namespace memory
} // namespace hunterclaw
