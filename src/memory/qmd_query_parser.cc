#include "hunterclaw/memory/qmd_query_parser.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Helper Functions
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static std::vector<std::string> split_lines(const std::string& str) {
    std::vector<std::string> lines;
    std::istringstream iss(str);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// No Results Detection
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool is_qmd_no_results_line(const std::string& line) {
    // 对齐 openclaw: input is already trimmed/lowercased/collapsed
    if (line == "no results found" || line == "no results found.") {
        return true;
    }
    
    // Pattern: [prefix] warn/info/error/qmd: no results found
    std::regex pattern(
        R"(^(?:\[[^\]]+\]\s*)?(?:(?:warn(?:ing)?|info|error|qmd)\s*:\s*)+no results found\.?$)",
        std::regex::icase
    );
    
    return std::regex_search(line, pattern);
}

bool is_qmd_no_results_output(const std::string& raw) {
    // 对齐 openclaw: split -> trim/lower/collapse -> filter empty -> some(isQmdNoResultsLine)
    auto lines = split_lines(raw);
    for (const auto& line : lines) {
        // trim + lower
        std::string lower = to_lower(trim(line));
        if (lower.empty()) continue;
        // collapse whitespace
        std::string collapsed;
        bool prev_space = false;
        for (char c : lower) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!prev_space) collapsed += ' ';
                prev_space = true;
            } else {
                collapsed += c;
                prev_space = false;
            }
        }
        if (is_qmd_no_results_line(collapsed)) {
            return true;
        }
    }
    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// JSON Array Extraction
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string extract_first_json_array(const std::string& raw) {
    size_t start = raw.find('[');
    if (start == std::string::npos) {
        return "";
    }
    
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    
    for (size_t i = start; i < raw.length(); ++i) {
        char c = raw[i];
        
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        
        if (c == '"') {
            in_string = true;
            continue;
        }
        
        if (c == '[') {
            depth++;
        } else if (c == ']') {
            depth--;
            if (depth == 0) {
                return raw.substr(start, i - start + 1);
            }
        }
    }
    
    return "";
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// JSON Parsing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<QmdQueryResult> parse_qmd_query_result_array(const std::string& raw) {
    // 对齐 openclaw: parseQmdQueryResultArray returns null on failure (we throw to signal null)
    try {
        auto j = json::parse(raw);
        if (!j.is_array()) {
            return {};  // signals null
        }
        
        std::vector<QmdQueryResult> results;
        for (const auto& item : j) {
            QmdQueryResult result;
            
            if (item.contains("docid") && !item["docid"].is_null()) {
                result.docid = item["docid"].get<std::string>();
            }
            if (item.contains("score") && !item["score"].is_null()) {
                result.score = item["score"].get<double>();
            }
            if (item.contains("collection") && !item["collection"].is_null()) {
                result.collection = item["collection"].get<std::string>();
            }
            if (item.contains("file") && !item["file"].is_null()) {
                result.file = item["file"].get<std::string>();
            }
            if (item.contains("snippet") && !item["snippet"].is_null()) {
                result.snippet = item["snippet"].get<std::string>();
            }
            if (item.contains("body") && !item["body"].is_null()) {
                result.body = item["body"].get<std::string>();
            }
            
            results.push_back(result);
        }
        
        return results;
    } catch (...) {
        return {};  // signals null (parse failure)
    }
}

// Returns true if the result is a valid non-null parse (array was found)
static bool is_valid_parse_result(const std::vector<QmdQueryResult>& results, const std::string& raw) {
    // We distinguish "null" (parse failure) from "empty array" by trying to parse again
    try {
        auto j = json::parse(raw);
        return j.is_array();
    } catch (...) {
        return false;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Error Summarization
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string summarize_qmd_stderr(const std::string& raw) {
    if (raw.length() <= 120) {
        return raw;
    }
    return raw.substr(0, 117) + "...";
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main Parsing Function
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::vector<QmdQueryResult> parse_qmd_query_json(
    const std::string& stdout,
    const std::string& stderr
) {
    std::string trimmed_stdout = trim(stdout);
    std::string trimmed_stderr = trim(stderr);
    
    // Check for "no results found" markers
    bool stdout_is_marker = !trimmed_stdout.empty() && 
                            is_qmd_no_results_output(trimmed_stdout);
    bool stderr_is_marker = !trimmed_stderr.empty() && 
                            is_qmd_no_results_output(trimmed_stderr);
    
    if (stdout_is_marker || (trimmed_stdout.empty() && stderr_is_marker)) {
        return {};
    }
    
    // Require non-empty stdout (对齐 openclaw: log.warn + throw)
    if (trimmed_stdout.empty()) {
        std::string context = !trimmed_stderr.empty() 
            ? " (stderr: " + summarize_qmd_stderr(trimmed_stderr) + ")"
            : "";
        std::string message = "stdout empty" + context;
        // log.warn equivalent
        throw std::runtime_error("qmd query returned invalid JSON: " + message);
    }
    
    try {
        // Try direct parsing
        if (is_valid_parse_result({}, trimmed_stdout)) {
            return parse_qmd_query_result_array(trimmed_stdout);
        }
        
        // Try extracting JSON array from noisy output
        std::string noisy_payload = extract_first_json_array(trimmed_stdout);
        if (noisy_payload.empty()) {
            throw std::runtime_error("qmd query JSON response was not an array");
        }
        
        if (is_valid_parse_result({}, noisy_payload)) {
            return parse_qmd_query_result_array(noisy_payload);
        }
        
        throw std::runtime_error("qmd query JSON response was not an array");
        
    } catch (const std::exception& e) {
        // 对齐 openclaw: log.warn + re-throw
        std::string message = e.what();
        // Avoid double-wrapping
        if (message.find("qmd query returned invalid JSON: ") == 0) {
            throw;
        }
        throw std::runtime_error("qmd query returned invalid JSON: " + message);
    }
}

}  // namespace memory
}  // namespace hunterclaw
