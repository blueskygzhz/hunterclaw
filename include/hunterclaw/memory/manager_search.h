#pragma once

#include "hunterclaw/memory/types.h"
#include "hunterclaw/core/result.h"
#include <string>
#include <vector>
#include <optional>

// 严格对齐 OpenClaw src/memory/manager-search.ts

namespace hunterclaw {
namespace memory {

// Forward declarations
class SQLiteVectorStorage;

// ============================================================================
// SearchSource (对齐 manager-search.ts:SearchSource)
// ============================================================================
using SearchSource = std::string;

// ============================================================================
// SearchRowResult (对齐 manager-search.ts:SearchRowResult)
// ============================================================================
struct SearchRowResult {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    double score = 0.0;
    std::string snippet;
    SearchSource source;
};

// ============================================================================
// ChunkWithEmbedding (对齐 manager-search.ts:listChunks 返回类型)
// ============================================================================
struct ChunkWithEmbedding {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    std::string text;
    SearchSource source;
    std::vector<float> embedding;
};

// ============================================================================
// KeywordSearchResultWithScore (内部类型，对齐 searchKeyword 返回类型)
// ============================================================================
struct KeywordSearchResultWithScore {
    std::string id;
    std::string path;
    int start_line = 0;
    int end_line = 0;
    double text_score = 0.0;
    double score = 0.0;
    std::string snippet;
    std::string text;
    MemorySource source;
};

// ============================================================================
// searchVector (对齐 manager-search.ts:searchVector)
// ============================================================================
core::Result<std::vector<SearchRowResult>> search_vector_impl(
    SQLiteVectorStorage& db,
    const std::string& vector_table,
    const std::string& provider_model,
    const std::vector<float>& query_vec,
    int limit,
    int snippet_max_chars,
    const std::vector<MemorySource>& source_filter_list
);

// ============================================================================
// searchKeyword (对齐 manager-search.ts:searchKeyword)
// ============================================================================
core::Result<std::vector<KeywordSearchResultWithScore>> search_keyword_impl(
    SQLiteVectorStorage& db,
    const std::string& fts_table,
    const std::optional<std::string>& provider_model,
    const std::string& query,
    int limit,
    int snippet_max_chars,
    const std::vector<MemorySource>& source_filter_list
);

// ============================================================================
// listChunks (对齐 manager-search.ts:listChunks)
// ============================================================================
core::Result<std::vector<ChunkWithEmbedding>> list_chunks_impl(
    SQLiteVectorStorage& db,
    const std::string& provider_model,
    const std::vector<MemorySource>& source_filter_list
);

} // namespace memory
} // namespace hunterclaw
