#ifndef HUNTERCLAW_MEMORY_QMD_QUERY_PARSER_H_
#define HUNTERCLAW_MEMORY_QMD_QUERY_PARSER_H_

#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// QMD Query Result Parsing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 对齐: openclaw-analysis/src/memory/qmd-query-parser.ts

/**
 * Single query result from qmd CLI tool
 */
struct QmdQueryResult {
    std::optional<std::string> docid;       // Document ID
    std::optional<double> score;            // Relevance score
    std::optional<std::string> collection;  // Collection name
    std::optional<std::string> file;        // File path
    std::optional<std::string> snippet;     // Snippet text
    std::optional<std::string> body;        // Full body text
};

/**
 * Parse qmd query JSON output
 * 
 * Handles multiple output formats:
 * - Valid JSON array
 * - "No results found" markers
 * - Noisy output with embedded JSON
 * 
 * @param stdout Standard output from qmd command
 * @param stderr Standard error from qmd command
 * @return Parsed query results
 * @throws std::runtime_error if parsing fails
 */
std::vector<QmdQueryResult> parse_qmd_query_json(
    const std::string& stdout,
    const std::string& stderr
);

/**
 * Check if output contains "no results found" marker
 * 
 * Recognizes various formats:
 * - "no results found"
 * - "[warn] qmd: no results found."
 * - etc.
 */
bool is_qmd_no_results_output(const std::string& raw);

/**
 * Summarize stderr for error messages
 * Truncates to 120 chars
 */
std::string summarize_qmd_stderr(const std::string& raw);

/**
 * Parse a JSON array of query results
 * Returns empty vector if not a valid array
 */
std::vector<QmdQueryResult> parse_qmd_query_result_array(const std::string& raw);

/**
 * Extract first JSON array from noisy output
 * Returns empty string if no array found
 */
std::string extract_first_json_array(const std::string& raw);

/**
 * Check if line contains "no results found" pattern
 */
bool is_qmd_no_results_line(const std::string& line);

}  // namespace memory
}  // namespace hunterclaw

#endif  // HUNTERCLAW_MEMORY_QMD_QUERY_PARSER_H_
