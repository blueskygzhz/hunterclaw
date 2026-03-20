#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/search_algorithms.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <sqlite3.h>

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
    std::string text;
    MemorySource source;
};

namespace {

// Truncate UTF-8 string safely (对齐 utils.ts:truncateUtf16Safe)
std::string truncate_utf8_safe(const std::string& str, size_t max_chars) {
    if (str.size() <= max_chars) {
        return str;
    }
    std::string result = str.substr(0, max_chars);
    // Ensure we don't cut in the middle of a multi-byte UTF-8 character
    while (!result.empty() && (result.back() & 0xC0) == 0x80) {
        result.pop_back();
    }
    return result;
}

// Source filter builder (对齐 sourceFilter logic in manager.ts)
struct SourceFilter {
    std::string sql;
    std::vector<std::string> params;
};

SourceFilter build_source_filter(const std::vector<MemorySource>& sources) {
    SourceFilter filter;
    if (sources.empty()) {
        return filter;
    }
    filter.sql = " AND source IN (";
    for (size_t i = 0; i < sources.size(); ++i) {
        if (i > 0) filter.sql += ",";
        filter.sql += "?";
        filter.params.push_back(to_string(sources[i]));
    }
    filter.sql += ")";
    return filter;
}

// Convert blob bytes to float vector (对齐 parseEmbedding)
std::vector<float> parse_embedding_blob(const void* data, int bytes) {
    if (!data || bytes <= 0) return {};
    size_t count = bytes / sizeof(float);
    const float* ptr = reinterpret_cast<const float*>(data);
    return std::vector<float>(ptr, ptr + count);
}

// Convert float vector to blob (对齐 vectorToBlob)
std::vector<uint8_t> vector_to_blob(const std::vector<float>& embedding) {
    std::vector<uint8_t> blob(embedding.size() * sizeof(float));
    std::memcpy(blob.data(), embedding.data(), blob.size());
    return blob;
}

} // anonymous namespace

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
) {
    if (query_vec.empty() || limit <= 0) {
        return core::Result<std::vector<SearchRowResult>>::Ok({});
    }

    auto source_filter_vec = build_source_filter(source_filter_list);

    // 1. 尝试 sqlite-vec 向量搜索 (对齐 ensureVectorReady 分支)
    if (db.is_vec_available()) {
        // 对齐 openclaw SQL:
        // SELECT c.id, c.path, c.start_line, c.end_line, c.text,
        //        c.source,
        //        vec_distance_cosine(v.embedding, ?) AS dist
        //   FROM {vectorTable} v
        //   JOIN chunks c ON c.id = v.id
        //  WHERE c.model = ? {sourceFilter}
        //  ORDER BY dist ASC
        //  LIMIT ?
        std::string sql =
            "SELECT c.id, c.path, c.start_line, c.end_line, c.text, c.source,"
            "       vec_distance_cosine(v.embedding, ?) AS dist"
            "  FROM " + vector_table + " v"
            "  JOIN chunks c ON c.id = v.id"
            " WHERE c.model = ?" + source_filter_vec.sql +
            " ORDER BY dist ASC"
            " LIMIT ?";

        auto stmt_result = db.prepare_statement(sql);
        if (stmt_result.is_ok()) {
            sqlite3_stmt* stmt = stmt_result.get();

            // Bind query vector as blob
            auto blob = vector_to_blob(query_vec);
            sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, provider_model.c_str(), -1, SQLITE_TRANSIENT);

            int param_idx = 3;
            for (const auto& p : source_filter_vec.params) {
                sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
            }
            sqlite3_bind_int(stmt, param_idx, limit);

            std::vector<SearchRowResult> results;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SearchRowResult row;
                row.id         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                row.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                row.start_line = sqlite3_column_int(stmt, 2);
                row.end_line   = sqlite3_column_int(stmt, 3);
                std::string text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                row.source     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                double dist    = sqlite3_column_double(stmt, 6);
                // 对齐 openclaw: score = 1 - dist
                row.score      = 1.0 - dist;
                row.snippet    = truncate_utf8_safe(text, snippet_max_chars);
                results.push_back(std::move(row));
            }
            sqlite3_finalize(stmt);
            return core::Result<std::vector<SearchRowResult>>::Ok(std::move(results));
        }
    }

    // 2. Fallback: in-memory cosine similarity (对齐 openclaw fallback 分支)
    // 对齐 openclaw SQL:
    // SELECT id, path, start_line, end_line, text, embedding, source
    //   FROM chunks
    //  WHERE model = ? {sourceFilter}
    std::string sql =
        "SELECT id, path, start_line, end_line, text, embedding, source"
        "  FROM chunks"
        " WHERE model = ?" + source_filter_vec.sql;

    auto stmt_result = db.prepare_statement(sql);
    if (stmt_result.is_err()) {
        return core::Result<std::vector<SearchRowResult>>::Err(stmt_result.error);
    }

    sqlite3_stmt* stmt = stmt_result.get();
    sqlite3_bind_text(stmt, 1, provider_model.c_str(), -1, SQLITE_TRANSIENT);
    int param_idx = 2;
    for (const auto& p : source_filter_vec.params) {
        sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }

    // 对齐 openclaw: cosineSimilarity
    auto cosine_similarity = [](const std::vector<float>& a, const std::vector<float>& b) -> double {
        if (a.size() != b.size() || a.empty()) return 0.0;
        double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            dot    += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }
        if (norm_a == 0.0 || norm_b == 0.0) return 0.0;
        return dot / (std::sqrt(norm_a) * std::sqrt(norm_b));
    };

    struct Candidate {
        SearchRowResult row;
        double score;
    };
    std::vector<Candidate> candidates;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string id         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        int start_line         = sqlite3_column_int(stmt, 2);
        int end_line           = sqlite3_column_int(stmt, 3);
        std::string text       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        std::string source_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));

        // 对齐 openclaw: parseEmbedding(row.embedding)
        // openclaw 存储为 JSON array TEXT
        std::vector<float> embedding;
        const char* emb_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        if (emb_text) {
            std::string emb_str(emb_text);
            if (!emb_str.empty() && emb_str[0] == '[') {
                std::string s = emb_str.substr(1, emb_str.size() - 2);
                std::istringstream iss(s);
                std::string token;
                while (std::getline(iss, token, ',')) {
                    try { embedding.push_back(std::stof(token)); } catch (...) {}
                }
            } else {
                const void* emb_data = sqlite3_column_blob(stmt, 5);
                int emb_bytes        = sqlite3_column_bytes(stmt, 5);
                embedding = parse_embedding_blob(emb_data, emb_bytes);
            }
        }
        double score   = cosine_similarity(query_vec, embedding);

        if (!std::isfinite(score)) continue;

        SearchRowResult row;
        row.id         = std::move(id);
        row.path       = std::move(path);
        row.start_line = start_line;
        row.end_line   = end_line;
        row.source     = std::move(source_str);
        row.snippet    = truncate_utf8_safe(text, snippet_max_chars);
        row.score      = score;
        candidates.push_back({std::move(row), score});
    }
    sqlite3_finalize(stmt);

    // 对齐 openclaw: toSorted((a, b) => b.score - a.score).slice(0, limit)
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::vector<SearchRowResult> results;
    results.reserve(std::min(candidates.size(), static_cast<size_t>(limit)));
    for (size_t i = 0; i < candidates.size() && static_cast<int>(i) < limit; ++i) {
        results.push_back(std::move(candidates[i].row));
    }
    return core::Result<std::vector<SearchRowResult>>::Ok(std::move(results));
}

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
) {
    if (limit <= 0) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }

    // 对齐 openclaw: buildFtsQuery
    std::string fts_query = build_fts_query(query);
    if (fts_query.empty()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }

    auto source_filter = build_source_filter(source_filter_list);

    // 对齐 openclaw: When providerModel is undefined (FTS-only mode), search all models
    std::string model_clause  = provider_model ? " AND model = ?" : "";
    std::vector<std::string> model_params;
    if (provider_model) model_params.push_back(*provider_model);

    // 对齐 openclaw SQL:
    // SELECT id, path, source, start_line, end_line, text,
    //        bm25({ftsTable}) AS rank
    //   FROM {ftsTable}
    //  WHERE {ftsTable} MATCH ? {modelClause} {sourceFilter}
    //  ORDER BY rank ASC
    //  LIMIT ?
    std::string sql =
        "SELECT id, path, source, start_line, end_line, text,"
        "       bm25(" + fts_table + ") AS rank"
        "  FROM " + fts_table +
        " WHERE " + fts_table + " MATCH ?" + model_clause + source_filter.sql +
        " ORDER BY rank ASC"
        " LIMIT ?";

    auto stmt_result = db.prepare_statement(sql);
    if (stmt_result.is_err()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Err(stmt_result.error);
    }

    sqlite3_stmt* stmt = stmt_result.get();
    int param_idx = 1;
    sqlite3_bind_text(stmt, param_idx++, fts_query.c_str(), -1, SQLITE_TRANSIENT);
    for (const auto& p : model_params) {
        sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& p : source_filter.params) {
        sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, param_idx, limit);

    std::vector<KeywordSearchResultWithScore> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        KeywordSearchResultWithScore row;
        row.id         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        row.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string src_str = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        row.source     = (src_str == "sessions") ? MemorySource::Sessions : MemorySource::Memory;
        row.start_line = sqlite3_column_int(stmt, 3);
        row.end_line   = sqlite3_column_int(stmt, 4);
        std::string text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        double rank    = sqlite3_column_double(stmt, 6);

        // 对齐 openclaw: bm25RankToScore
        double text_score = bm25_rank_to_score(rank);
        row.text_score = text_score;
        row.score      = text_score;
        row.snippet    = truncate_utf8_safe(text, snippet_max_chars);
        row.text       = std::move(text);
        results.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok(std::move(results));
}

// ============================================================================
// listChunks (对齐 manager-search.ts:listChunks)
// ============================================================================
core::Result<std::vector<ChunkWithEmbedding>> list_chunks_impl(
    SQLiteVectorStorage& db,
    const std::string& provider_model,
    const std::vector<MemorySource>& source_filter_list
) {
    auto source_filter = build_source_filter(source_filter_list);

    // 对齐 openclaw SQL:
    // SELECT id, path, start_line, end_line, text, embedding, source
    //   FROM chunks
    //  WHERE model = ? {sourceFilter}
    std::string sql =
        "SELECT id, path, start_line, end_line, text, embedding, source"
        "  FROM chunks"
        " WHERE model = ?" + source_filter.sql;

    auto stmt_result = db.prepare_statement(sql);
    if (stmt_result.is_err()) {
        return core::Result<std::vector<ChunkWithEmbedding>>::Err(stmt_result.error);
    }

    sqlite3_stmt* stmt = stmt_result.get();
    sqlite3_bind_text(stmt, 1, provider_model.c_str(), -1, SQLITE_TRANSIENT);
    int param_idx = 2;
    for (const auto& p : source_filter.params) {
        sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ChunkWithEmbedding> chunks;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ChunkWithEmbedding chunk;
        chunk.id         = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        chunk.path       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        chunk.start_line = sqlite3_column_int(stmt, 2);
        chunk.end_line   = sqlite3_column_int(stmt, 3);
        chunk.text       = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        // 对齐 openclaw: parseEmbedding(row.embedding)
        // openclaw 存储为 JSON array TEXT，需要 JSON 解析
        const char* emb_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        if (emb_text) {
            std::string emb_str(emb_text);
            if (!emb_str.empty() && emb_str[0] == '[') {
                // JSON array format (对齐 openclaw parseEmbedding)
                std::string s = emb_str.substr(1, emb_str.size() - 2);
                std::istringstream iss(s);
                std::string token;
                while (std::getline(iss, token, ',')) {
                    try { chunk.embedding.push_back(std::stof(token)); } catch (...) {}
                }
            } else {
                // Blob format fallback
                const void* emb_data = sqlite3_column_blob(stmt, 5);
                int emb_bytes        = sqlite3_column_bytes(stmt, 5);
                chunk.embedding = parse_embedding_blob(emb_data, emb_bytes);
            }
        }

        chunk.source     = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        chunks.push_back(std::move(chunk));
    }
    sqlite3_finalize(stmt);

    return core::Result<std::vector<ChunkWithEmbedding>>::Ok(std::move(chunks));
}

} // namespace memory
} // namespace hunterclaw
