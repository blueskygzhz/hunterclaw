#include "hunterclaw/memory/manager_search.h"
#include "hunterclaw/memory/sqlite_storage.h"
#include "hunterclaw/memory/search_algorithms.h"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <set>
#include <functional>
#include <sqlite3.h>

// 严格对齐 OpenClaw src/memory/manager-search.ts

namespace hunterclaw {
namespace memory {

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
        //   JOIN memory_index_chunks c ON c.id = v.id
        //  WHERE c.model = ? {sourceFilter}
        //  ORDER BY dist ASC
        //  LIMIT ?
        std::string sql =
            "SELECT c.id, c.path, c.start_line, c.end_line, c.text, c.source,"
            "       vec_distance_cosine(v.embedding, ?) AS dist"
            "  FROM " + vector_table + " v"
            "  JOIN memory_index_chunks c ON c.id = v.id"
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
    //   FROM memory_index_chunks
    //  WHERE model = ? {sourceFilter}
    std::string sql =
        "SELECT id, path, start_line, end_line, text, embedding, source"
        "  FROM memory_index_chunks"
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
// scoreFallbackKeywordResult (对齐 manager-search.ts:scoreFallbackKeywordResult)
// Boost FTS-only ranking with lexical overlap signal.
// ============================================================================
double score_fallback_keyword_result(
    const std::string& query,
    const std::string& path,
    const std::string& text,
    double fts_score
) {
    // 对齐 OC: normalizeSearchTokens
    auto normalize_tokens = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> tokens;
        std::string current;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
        const unsigned char* end = p + s.size();
        while (p < end) {
            unsigned char c = *p;
            if (c < 0x80) {
                if (std::isalnum(c) || c == '_') {
                    current.push_back(static_cast<char>(std::tolower(c)));
                } else {
                    if (!current.empty()) { tokens.push_back(current); current.clear(); }
                }
                ++p;
            } else {
                // multi-byte UTF-8: treat as word char
                int len = 1;
                if ((c & 0xE0) == 0xC0) len = 2;
                else if ((c & 0xF0) == 0xE0) len = 3;
                else if ((c & 0xF8) == 0xF0) len = 4;
                for (int i = 0; i < len && p < end; ++i, ++p)
                    current.push_back(static_cast<char>(*p));
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    };

    auto query_tokens_vec = normalize_tokens(query);
    // 对齐 OC: [...new Set(queryTokens)]
    std::vector<std::string> query_tokens;
    std::set<std::string> query_set;
    for (const auto& t : query_tokens_vec) {
        if (query_set.insert(t).second) query_tokens.push_back(t);
    }
    if (query_tokens.empty()) return fts_score;

    auto text_tokens_vec = normalize_tokens(text);
    std::set<std::string> text_token_set(text_tokens_vec.begin(), text_tokens_vec.end());

    std::string path_lower = path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);

    // 对齐 OC: overlap = queryTokens.filter(t => textTokenSet.has(t)).length
    int overlap = 0;
    for (const auto& t : query_tokens) {
        if (text_token_set.count(t)) ++overlap;
    }
    double unique_query_overlap = static_cast<double>(overlap) /
        std::max(static_cast<int>(query_set.size()), 1);
    double density = static_cast<double>(overlap) /
        std::max(static_cast<int>(text_token_set.size()), 1);

    // 对齐 OC: pathBoost
    double path_boost = 0.0;
    for (const auto& t : query_tokens) {
        if (path_lower.find(t) != std::string::npos) path_boost += 0.18;
    }
    // 对齐 OC: textLengthBoost = Math.min(text.length / 160, 0.18)
    double text_length_boost = std::min(static_cast<double>(text.size()) / 160.0, 0.18);

    // 对齐 OC: lexicalBoost = uniqueQueryOverlap * 0.45 + density * 0.2 + pathBoost + textLengthBoost
    double lexical_boost = unique_query_overlap * 0.45 + density * 0.2 + path_boost + text_length_boost;
    return std::min(1.0, fts_score + lexical_boost);
}

// ============================================================================
// plan_keyword_search (对齐 manager-search.ts:planKeywordSearch)
// ============================================================================
KeywordSearchPlan plan_keyword_search(
    const std::string& query,
    const std::string& fts_tokenizer,
    std::function<std::optional<std::string>(const std::string&)> build_fts_query_fn
) {
    KeywordSearchPlan plan;

    if (fts_tokenizer != "trigram") {
        // 对齐 OC: non-trigram path — just call buildFtsQuery
        plan.match_query = build_fts_query_fn(query);
        return plan;
    }

    // 对齐 OC: trigram path — split tokens, separate short CJK (< 3 chars) as substring terms
    // SHORT_CJK_TRIGRAM_RE = /[\u3040-\u30ff\u3400-\u9fff\uac00-\ud7af\u3131-\u3163]/u
    auto is_short_cjk_trigram = [](const std::string& token) -> bool {
        // Check if token contains CJK characters and has fewer than 3 Unicode codepoints
        const unsigned char* p = reinterpret_cast<const unsigned char*>(token.data());
        const unsigned char* end = p + token.size();
        int codepoint_count = 0;
        bool has_cjk = false;
        while (p < end) {
            unsigned char c = *p;
            uint32_t cp = 0;
            int len = 1;
            if (c < 0x80) { cp = c; len = 1; }
            else if ((c & 0xE0) == 0xC0 && end - p >= 2) { cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
            else if ((c & 0xF0) == 0xE0 && end - p >= 3) { cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
            else if ((c & 0xF8) == 0xF0 && end - p >= 4) { cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); len = 4; }
            // Check CJK ranges: \u3040-\u30ff, \u3400-\u9fff, \uac00-\ud7af, \u3131-\u3163
            if ((cp >= 0x3040 && cp <= 0x30FF) ||
                (cp >= 0x3400 && cp <= 0x9FFF) ||
                (cp >= 0xAC00 && cp <= 0xD7AF) ||
                (cp >= 0x3131 && cp <= 0x3163)) {
                has_cjk = true;
            }
            codepoint_count++;
            p += len;
        }
        return has_cjk && codepoint_count < 3;
    };

    // 对齐 OC: FTS_QUERY_TOKEN_RE = /[\p{L}\p{N}_]+/gu
    // Extract tokens (same as build_fts_query tokenization)
    std::vector<std::string> tokens;
    std::string current;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(query.data());
    const unsigned char* end = p + query.size();
    while (p < end) {
        unsigned char c = *p;
        if (c < 0x80) {
            if (std::isalnum(c) || c == '_') {
                current.push_back(static_cast<char>(c));
            } else {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            }
            ++p;
        } else {
            int len = 1;
            if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            for (int i = 0; i < len && p < end; ++i, ++p)
                current.push_back(static_cast<char>(*p));
        }
    }
    if (!current.empty()) tokens.push_back(current);

    if (tokens.empty()) return plan;  // match_query = nullopt, substring_terms = []

    // 对齐 OC: separate matchTerms vs substringTerms
    std::vector<std::string> match_terms;
    for (const auto& token : tokens) {
        if (is_short_cjk_trigram(token)) {
            plan.substring_terms.push_back(token);
        } else {
            match_terms.push_back(token);
        }
    }

    // 对齐 OC: buildMatchQueryFromTerms
    if (!match_terms.empty()) {
        std::string mq;
        for (size_t i = 0; i < match_terms.size(); ++i) {
            if (i > 0) mq += " AND ";
            std::string t = match_terms[i];
            t.erase(std::remove(t.begin(), t.end(), '"'), t.end());
            if (!t.empty()) mq += '"' + t + '"';
        }
        if (!mq.empty()) plan.match_query = mq;
    }

    return plan;
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
    const std::vector<MemorySource>& source_filter_list,
    const std::string& fts_tokenizer,
    bool boost_fallback_ranking
) {
    if (limit <= 0) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }

    // 对齐 OC: planKeywordSearch
    auto plan = plan_keyword_search(
        query,
        fts_tokenizer,
        [](const std::string& raw) -> std::optional<std::string> {
            std::string q = build_fts_query(raw);
            return q.empty() ? std::nullopt : std::make_optional(q);
        }
    );

    // 对齐 OC: if (!plan.matchQuery && plan.substringTerms.length === 0) return []
    if (!plan.match_query && plan.substring_terms.empty()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Ok({});
    }

    auto source_filter = build_source_filter(source_filter_list);

    // 对齐 OC: When providerModel is undefined (FTS-only mode), search all models
    std::string model_clause = provider_model ? " AND model = ?" : "";
    std::vector<std::string> model_params;
    if (provider_model) model_params.push_back(*provider_model);

    // 对齐 OC: substringClause = substringTerms.map(() => " AND text LIKE ? ESCAPE '\\'").join("")
    std::string substring_clause;
    std::vector<std::string> substring_params;
    for (const auto& term : plan.substring_terms) {
        substring_clause += " AND text LIKE ? ESCAPE '\\'";
        // 对齐 OC: escapeLikePattern(term) → `%${escaped}%`
        std::string escaped = term;
        // escape \, %, _
        std::string esc_term;
        for (char c : escaped) {
            if (c == '\\' || c == '%' || c == '_') esc_term += '\\';
            esc_term += c;
        }
        substring_params.push_back("%" + esc_term + "%");
    }

    // 对齐 OC: whereClause
    std::string where_clause;
    if (plan.match_query) {
        where_clause = fts_table + " MATCH ?" + substring_clause + model_clause + source_filter.sql;
    } else {
        // substring-only (all terms were short CJK)
        where_clause = "1=1" + substring_clause + model_clause + source_filter.sql;
    }

    // 对齐 OC: rankExpression = plan.matchQuery ? `bm25(${ftsTable})` : "0"
    std::string rank_expression = plan.match_query ? ("bm25(" + fts_table + ")") : "0";

    // 对齐 OC SQL:
    // SELECT id, path, source, start_line, end_line, text,
    //        {rankExpression} AS rank
    //   FROM {ftsTable}
    //  WHERE {whereClause}
    //  ORDER BY rank ASC
    //  LIMIT ?
    std::string sql =
        "SELECT id, path, source, start_line, end_line, text,"
        "       " + rank_expression + " AS rank"
        "  FROM " + fts_table +
        " WHERE " + where_clause +
        " ORDER BY rank ASC"
        " LIMIT ?";

    auto stmt_result = db.prepare_statement(sql);
    if (stmt_result.is_err()) {
        return core::Result<std::vector<KeywordSearchResultWithScore>>::Err(stmt_result.error);
    }

    sqlite3_stmt* stmt = stmt_result.get();
    int param_idx = 1;
    // 对齐 OC: queryParams = [...(matchQuery ? [matchQuery] : []), ...substringParams, ...modelParams, ...sourceFilter.params, limit]
    if (plan.match_query) {
        sqlite3_bind_text(stmt, param_idx++, plan.match_query->c_str(), -1, SQLITE_TRANSIENT);
    }
    for (const auto& p : substring_params) {
        sqlite3_bind_text(stmt, param_idx++, p.c_str(), -1, SQLITE_TRANSIENT);
    }
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

        // 对齐 OC: textScore = plan.matchQuery ? bm25RankToScore(rank) : 1
        double text_score = plan.match_query ? bm25_rank_to_score(rank) : 1.0;

        // 对齐 OC: score = boostFallbackRanking ? scoreFallbackKeywordResult(...) : textScore
        double score = boost_fallback_ranking
            ? score_fallback_keyword_result(query, row.path, text, text_score)
            : text_score;

        row.text_score = text_score;
        row.score      = score;
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
    //   FROM memory_index_chunks
    //  WHERE model = ? {sourceFilter}
    std::string sql =
        "SELECT id, path, start_line, end_line, text, embedding, source"
        "  FROM memory_index_chunks"
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
