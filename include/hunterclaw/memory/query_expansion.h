#pragma once

#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <optional>
#include <functional>

namespace hunterclaw {
namespace memory {

/**
 * Query expansion for FTS-only search mode.
 *
 * When no embedding provider is available, we fall back to FTS (full-text search).
 * FTS works best with specific keywords, but users often ask conversational queries.
 * This module extracts meaningful keywords from such queries to improve FTS results.
 */

// Check if a token is a stop word (multi-language)
// (对齐 query-expansion.ts:isQueryStopWordToken)
bool is_query_stop_word_token(const std::string& token);

// Strip Korean trailing particle from a UTF-32 token
// (对齐 query-expansion.ts:stripKoreanTrailingParticle)
std::optional<std::u32string> strip_korean_trailing_particle(const std::u32string& token);

// Check if a Korean stem is a useful keyword
// (对齐 query-expansion.ts:isUsefulKoreanStem)
bool is_useful_korean_stem(const std::u32string& stem);

// Tokenize text into keywords (handles EN/ZH/JA/KO/AR/ES/PT)
// (对齐 query-expansion.ts:tokenize)
// fts_tokenizer: "unicode61" (default) or "trigram" — in trigram mode, CJK chars
// are kept as contiguous blocks instead of unigrams+bigrams.
std::vector<std::string> tokenize_query(
    const std::string& text,
    const std::string& fts_tokenizer = "unicode61"
);

// Extract keywords from conversational query
// (对齐 query-expansion.ts:extractKeywords)
// fts_tokenizer: passed through to tokenize_query
std::vector<std::string> extract_keywords(
    const std::string& query,
    const std::string& fts_tokenizer = "unicode61"
);

/**
 * Expand query result structure
 */
struct ExpandedQuery {
    std::string original;           // Original query
    std::vector<std::string> keywords; // Extracted keywords
    std::string expanded;           // Expanded query (original OR keywords)
};

/**
 * Expand a query for FTS search.
 * Returns both the original query and extracted keywords for OR-matching.
 */
ExpandedQuery expand_query_for_fts(const std::string& query);

/**
 * Type for optional LLM-based query expander.
 * Can be provided to enhance keyword extraction with semantic understanding.
 */
using LlmQueryExpander = std::function<std::vector<std::string>(const std::string&)>;

/**
 * Expand query with optional LLM assistance.
 * Falls back to local extraction if LLM is unavailable or fails.
 */
std::vector<std::string> expand_query_with_llm(
    const std::string& query,
    const LlmQueryExpander& llm_expander = nullptr
);

} // namespace memory
} // namespace hunterclaw
