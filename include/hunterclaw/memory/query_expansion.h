#pragma once

#include <string>
#include <vector>
#include <set>
#include <unordered_set>
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
bool is_query_stop_word_token(const std::string& token);

// Tokenize text into keywords (handles EN/ZH/JA/KO/AR/ES/PT)
std::vector<std::string> tokenize_query(const std::string& text);

// Extract keywords from conversational query
std::vector<std::string> extract_keywords(const std::string& query);

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
