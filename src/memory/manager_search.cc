#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/memory_index_manager.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/search_algorithms.h"
#include <algorithm>
#include <sstream>
#include <cmath>

// 严格对齐 OpenClaw src/memory/manager-search.ts

namespace hunterclaw {
namespace memory {

// ============================================================================
// Internal Types
// ============================================================================

// KeywordSearchResult from SQLiteStorage (internal type)
struct KeywordSearchResultWithScore {
    std::string id;
    std::string path;
    int start_line;
    int end_line;
    double text_score;
    double score;
    std::string snippet;
    std::string text;  // full text
    MemorySource source;
    std::optional<int64_t> modified_ms;
};

namespace {

// Truncate UTF-8 string safely (对齐 utils.ts:truncateUtf16Safe)
std::string truncate_utf8_safe(const std::string& str, size_t max_chars) {
    if (str.size() <= max_chars) {
        return str;
    }
    
    // Simple truncation for now (TODO: proper UTF-8 boundary handling)
    std::string result = str.substr(0, max_chars);
    // Ensure we don't cut in the middle of a multi-byte UTF-8 character
    while (!result.empty() && (result.back() & 0xC0) == 0x80) {
        result.pop_back();
    }
    return result;
}

// Build FTS query (对齐 buildFtsQuery)
std::string build_fts_query_impl(const std::string& raw) {
    if (raw.empty()) {
        return "";
    }
    
    // Simple tokenization: split on whitespace and special chars
    std::vector<std::string> tokens;
    std::string current;
    
    for (char c : raw) {
        if (std::isalnum(c) || c == '_') {
            current += c;
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    
    if (tokens.empty()) {
        return "";
    }
    
    // Build FTS5 query: "token1 OR token2 OR ..."
    std::ostringstream oss;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) oss << " OR ";
        oss << tokens[i];
    }
    
    return oss.str();
}

// BM25 rank to score (对齐 bm25RankToScore)
double bm25_rank_to_score(double rank) {
    // BM25 rank is negative, convert to positive score
    // rank typically in [-15, 0], map to [0, 1]
    return std::max(0.0, std::min(1.0, (rank + 15.0) / 15.0));
}

// Source filter builder (对齐 sourceFilter logic)
struct SourceFilter {
    std::string sql;
    std::vector<std::string> params;
};

SourceFilter build_source_filter(const std::vector<MemorySource>& sources) {
    SourceFilter filter;
    
    if (sources.empty()) {
        return filter;  // No filter
    }
    
    // Build " AND source IN (?,?,...)"
    filter.sql = " AND source IN (";
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i > 0) filter.sql += ",";
        filter.sql += "?";
        filter.params.push_back(to_string(sources[i]));
    }
    filter.sql += ")";
    
    return filter;
}

} // anonymous namespace

// ============================================================================
// searchVector (对齐 manager-search.ts:20-94 searchVector)
// ============================================================================
core::Result<std::vector<SearchRowResult>> search_vector_impl(
    SQLiteVectorStorage& db,
    const std::string& vector_table,
    const std::string& provider_model,
    const std::vector<float>& query_vec,
    int limit,
    int snippet_max_chars,
    const std::vector<MemorySource>& source_filter_list
) {
    if (query_vec.empty() || limit <= 0) {
        return core::Result<std::vector<SearchRowResult>>::Ok({});
    }
    
    // Build source filter
    auto source_filter = build_source_filter(source_filter_list);
    
    // TODO: 实现向量搜索
    // 由于SQLiteVectorStorage没有search_vector方法，暂时返回空结果
    std::vector<SearchRowResult> results;
    return core::Result<std::vector<SearchRowResult>>::Ok(results);
}
// ============================================================================
// searchKeyword (对齐 manager-search.ts:136-191 searchKeyword)
// ============================================================================
core::Result<std::vector<KeywordSearchResultWithScore>> search_keyword_impl(
    SQLiteVectorStorage& db,
    const std::string& fts_table,
    const std::optional<std::string>& provider_model,
    const std::string& query,
    int limit,
    int snippet_max_chars,
    const std::vector<MemorySource>& source_filter_list
) {
    (void)db;
    (void)fts_table;
    (void)provider_model;
    (void)query;
    (void)snippet_max_chars;
    (void)source_filter_list;
    
    if (limit <= 0) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }
    
    // Build FTS query
    std::string fts_query = build_fts_query_impl(query);
    if (fts_query.empty()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }
    
    // TODO: 实现关键词搜索
    // 由于SQLiteVectorStorage没有search_keyword方法，暂时返回空结果
    // 需要使用 db.fts_search() 或直接SQL查询实现
    
    std::vector<KeywordSearchResultWithScore> results;
    return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok(results);
}

// ============================================================================
// listChunks (对齐 manager-search.ts:96-134 listChunks)
// ============================================================================
core::Result<std::vector<ChunkWithEmbedding>> list_chunks_impl(
    SQLiteVectorStorage& db,
    const std::string& provider_model,
    const std::vector<MemorySource>& source_filter_list
) {
    // SQL: SELECT id, path, start_line, end_line, text, embedding, source
    //        FROM chunks
    //       WHERE model = ? {source_filter}
    
    // TODO: 实现list_chunks查询
    // auto result = db.list_chunks(provider_model, source_filter_list);
    
    // 暂时返回空列表
    std::vector<ChunkWithEmbedding> chunks;
    return core::Result<std::vector<ChunkWithEmbedding>>::Ok(chunks);
}

} // namespace memory
} // namespace hunterclaw
