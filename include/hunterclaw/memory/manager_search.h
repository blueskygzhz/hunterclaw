#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"  // 添加Result定义
#include <string>
#include <vector>

// 严格对齐 OpenClaw src/memory/manager-search.ts

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteStorage;
class SearchAlgorithms;

// 对齐 SearchSource
using SearchSource = std::string;

// 对齐 SearchRowResult (manager-search.ts:10-18)
struct SearchRowResult {
    std::string id;
    std::string path;
    int start_line;
    int end_line;
    double score;
    std::string snippet;
    SearchSource source;
};

// 对齐 listChunks result (manager-search.ts:96-106)
struct ChunkWithEmbedding {
    std::string id;
    std::string path;
    int start_line;
    int end_line;
    std::string text;
    SearchSource source;
    std::vector<float> embedding;
};

// Source filter helper (对齐 buildSourceFilter)
struct SourceFilter {
    std::string sql;              // WHERE clause fragment
    std::vector<SearchSource> params;  // Bind parameters
};

// ============================================================================
// Manager Search Operations (对齐 manager-search.ts)
// ============================================================================

class ManagerSearchOps {
public:
    // 对齐 searchVector (manager-search.ts:20-94)
    static core::Result<std::vector<SearchRowResult>> search_vector(
        SQLiteStorage* db,
        const std::string& vector_table,
        const std::string& provider_model,
        const std::vector<float>& query_vec,
        int limit,
        int snippet_max_chars,
        const SourceFilter& source_filter_vec,
        const SourceFilter& source_filter_chunks,
        bool vector_ready
    );

    // 对齐 searchKeyword (manager-search.ts:129-171)
    static core::Result<std::vector<SearchRowResult>> search_keyword(
        SQLiteStorage* db,
        const std::string& raw_query,
        int limit,
        int snippet_max_chars,
        const std::string& model_filter,  // Optional
        const SourceFilter& source_filter
    );

    // 对齐 listChunks (manager-search.ts:96-127)
    static core::Result<std::vector<ChunkWithEmbedding>> list_chunks(
        SQLiteStorage* db,
        const std::string& provider_model,
        const SourceFilter& source_filter
    );

    // Helper: Build FTS query (对齐 buildFtsQuery)
    static std::string build_fts_query(const std::string& raw);

    // Helper: Convert BM25 rank to score (对齐 bm25RankToScore)
    static double bm25_rank_to_score(double rank);

    // Helper: Truncate UTF-8 safely (对齐 truncateUtf16Safe)
    static std::string truncate_utf8_safe(const std::string& str, size_t max_chars);

private:
    // Fallback: In-memory cosine similarity search
    static std::vector<SearchRowResult> fallback_memory_search(
        const std::vector<ChunkWithEmbedding>& candidates,
        const std::vector<float>& query_vec,
        int limit,
        int snippet_max_chars
    );
};

// ============================================================================
// Source Filter Builder (对齐 buildSourceFilter, manager.ts:355-366)
// ============================================================================

class SourceFilterBuilder {
public:
    // Build WHERE clause for source filtering
    static SourceFilter build_source_filter(
        const std::vector<SearchSource>& sources
    );

    // Build separate filters for vec and chunks tables
    static std::pair<SourceFilter, SourceFilter> build_dual_source_filters(
        const std::vector<SearchSource>& sources
    );
};

}  // namespace memory
}  // namespace hunterclaw
