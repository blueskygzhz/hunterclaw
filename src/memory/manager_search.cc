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
        return core::Result<std::vector<SearchRowResult>>::ok_value({});
    }
    
    // Build source filter
    auto source_filter = build_source_filter(source_filter_list);
    
    // Try vector search first (sqlite-vec extension)
    // SQL: SELECT c.id, c.path, c.start_line, c.end_line, c.text, c.source,
    //             vec_distance_cosine(v.embedding, ?) AS dist
    //        FROM {vector_table} v
    //        JOIN chunks c ON c.id = v.id
    //       WHERE c.model = ? {source_filter}
    //       ORDER BY dist ASC
    //       LIMIT ?
    
    auto vec_result = db.search_vector(query_vec, limit, 0.0, provider_model, source_filter_list);
    
    if (vec_result.is_ok() && !vec_result.value().empty()) {
        // Convert VectorSearchResult to SearchRowResult
        std::vector<SearchRowResult> results;
        results.reserve(vec_result.value().size());
        
        for (const auto& vec_res : vec_result.value()) {
            SearchRowResult row;
            row.id = vec_res.id;
            row.path = vec_res.path;
            row.start_line = vec_res.start_line;
            row.end_line = vec_res.end_line;
            row.score = vec_res.score;
            row.snippet = truncate_utf8_safe(vec_res.text, snippet_max_chars);
            row.source = vec_res.source;
            
            results.push_back(std::move(row));
        }
        
        return core::Result<std::vector<SearchRowResult>>::ok_value(results);
    }
    
    // Fallback: load all chunks and compute similarity in-memory
    // (对齐 manager-search.ts:71-93 listChunks fallback)
    
    auto chunks_result = db.list_chunks(provider_model, source_filter_list);
    if (!chunks_result.is_ok()) {
        return core::Result<std::vector<SearchRowResult>>::error(
            "Vector search failed: " + chunks_result.error_message()
        );
    }
    
    // Compute cosine similarity for each chunk
    struct ScoredChunk {
        std::string id;
        std::string path;
        int start_line;
        int end_line;
        std::string text;
        MemorySource source;
        double score;
    };
    
    std::vector<ScoredChunk> scored;
    scored.reserve(chunks_result.value().size());
    
    for (const auto& chunk : chunks_result.value()) {
        double sim = cosine_similarity(query_vec, chunk.embedding);
        if (std::isfinite(sim)) {
            scored.push_back({
                chunk.id,
                chunk.path,
                chunk.start_line,
                chunk.end_line,
                chunk.text,
                chunk.source,
                sim
            });
        }
    }
    
    // Sort by score descending
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return b.score < a.score;
    });
    
    // Take top K
    std::vector<SearchRowResult> results;
    results.reserve(std::min(static_cast<size_t>(limit), scored.size()));
    
    for (size_t i = 0; i < scored.size() && static_cast<int>(i) < limit; ++i) {
        SearchRowResult row;
        row.id = scored[i].id;
        row.path = scored[i].path;
        row.start_line = scored[i].start_line;
        row.end_line = scored[i].end_line;
        row.score = scored[i].score;
        row.snippet = truncate_utf8_safe(scored[i].text, snippet_max_chars);
        row.source = scored[i].source;
        
        results.push_back(std::move(row));
    }
    
    return core::Result<std::vector<SearchRowResult>>::ok_value(results);
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
    if (limit <= 0) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::ok_value({});
    }
    
    // Build FTS query
    std::string fts_query = build_fts_query_impl(query);
    if (fts_query.empty()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::ok_value({});
    }
    
    // Build source filter
    auto source_filter = build_source_filter(source_filter_list);
    
    // Build SQL query (对齐 manager-search.ts:154-167)
    // SELECT id, path, source, start_line, end_line, text, bm25({fts_table}) AS rank
    //   FROM {fts_table}
    //  WHERE {fts_table} MATCH ? {model_clause} {source_filter}
    //  ORDER BY rank ASC
    //  LIMIT ?
    
    std::string model_clause;
    std::vector<std::string> model_params;
    
    if (provider_model.has_value()) {
        model_clause = " AND model = ?";
        model_params.push_back(provider_model.value());
    }
    
    auto kw_result = db.search_keyword(
        fts_query,
        limit,
        provider_model,
        source_filter_list
    );
    
    if (!kw_result.is_ok()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::error(
            "Keyword search failed: " + kw_result.error_message()
        );
    }
    
    // Convert to KeywordSearchResultWithScore (对齐 manager-search.ts:178-190)
    std::vector<KeywordSearchResultWithScore> results;
    results.reserve(kw_result.value().size());
    
    for (const auto& kw : kw_result.value()) {
        KeywordSearchResultWithScore row;
        row.id = kw.id;
        row.path = kw.path;
        row.start_line = kw.start_line;
        row.end_line = kw.end_line;
        row.text_score = kw.text_score;
        row.score = kw.text_score;
        row.snippet = truncate_utf8_safe(kw.text, snippet_max_chars);
        row.source = kw.source;
        
        results.push_back(std::move(row));
    }
    
    return core::Result<std::vector<KeywordSearchResultWithScore>>::ok_value(results);
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
    
    auto result = db.list_chunks(provider_model, source_filter_list);
    
    if (!result.is_ok()) {
        return core::Result<std::vector<ChunkWithEmbedding>>::error(
            "Failed to list chunks: " + result.error_message()
        );
    }
    
    return core::Result<std::vector<ChunkWithEmbedding>>::ok_value(result.value());
}

} // namespace memory
} // namespace hunterclaw
