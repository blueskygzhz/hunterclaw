#include <optional>
#include "hunterclaw/memory/json_parser.h"
#include <sstream>
#include <regex>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Simple JSON Parser for QMD Query Results
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

std::string JsonParser::trim(const std::string& str) {
    size_t start = 0;
    while (start < str.length() && std::isspace(str[start])) {
        start++;
    }
    
    size_t end = str.length();
    while (end > start && std::isspace(str[end - 1])) {
        end--;
    }
    
    return str.substr(start, end - start);
}

std::string JsonParser::unescape_json_string(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '\\' && i + 1 < str.length()) {
            char next = str[i + 1];
            switch (next) {
                case 'n': result += '\n'; i++; break;
                case 'r': result += '\r'; i++; break;
                case 't': result += '\t'; i++; break;
                case '\\': result += '\\'; i++; break;
                case '"': result += '"'; i++; break;
                default: result += str[i]; break;
            }
        } else {
            result += str[i];
        }
    }
    
    return result;
}

std::optional<std::string> JsonParser::extract_string_field(
    const std::string& json,
    const std::string& field_name
) {
    // Pattern: "field_name":"value" or "field_name": "value"
    std::string pattern = "\"" + field_name + "\"\\s*:\\s*\"([^\"]*)\"";
    std::regex re(pattern);
    std::smatch match;
    
    if (std::regex_search(json, match, re) && match.size() > 1) {
        return unescape_json_string(match[1].str());
    }
    
    return std::nullopt;
}

std::optional<double> JsonParser::extract_number_field(
    const std::string& json,
    const std::string& field_name
) {
    // Pattern: "field_name":123.45 or "field_name": 123.45
    std::string pattern = "\"" + field_name + "\"\\s*:\\s*([0-9.eE+-]+)";
    std::regex re(pattern);
    std::smatch match;
    
    if (std::regex_search(json, match, re) && match.size() > 1) {
        try {
            return std::stod(match[1].str());
        } catch (...) {
            return std::nullopt;
        }
    }
    
    return std::nullopt;
}

std::vector<std::string> JsonParser::split_json_array(const std::string& json_array) {
    std::vector<std::string> results;
    
    std::string trimmed = trim(json_array);
    
    // Remove outer brackets
    if (trimmed.empty() || trimmed[0] != '[' || trimmed[trimmed.length() - 1] != ']') {
        return results;  // Not a valid array
    }
    
    std::string content = trimmed.substr(1, trimmed.length() - 2);
    
    // Split by top-level commas (ignoring nested objects/arrays)
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t start = 0;
    
    for (size_t i = 0; i < content.length(); i++) {
        char ch = content[i];
        
        if (escaped) {
            escaped = false;
            continue;
        }
        
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        
        if (ch == '"' && !escaped) {
            in_string = !in_string;
            continue;
        }
        
        if (in_string) {
            continue;
        }
        
        if (ch == '{' || ch == '[') {
            depth++;
        } else if (ch == '}' || ch == ']') {
            depth--;
        } else if (ch == ',' && depth == 0) {
            // Found a top-level comma
            std::string item = trim(content.substr(start, i - start));
            if (!item.empty()) {
                results.push_back(item);
            }
            start = i + 1;
        }
    }
    
    // Add last item
    std::string last_item = trim(content.substr(start));
    if (!last_item.empty()) {
        results.push_back(last_item);
    }
    
    return results;
}

std::optional<QmdQueryResult> JsonParser::parse_qmd_result(const std::string& json_object) {
    QmdQueryResult result;
    
    // Extract required fields
    auto docid = extract_string_field(json_object, "docid");
    if (!docid) {
        return std::nullopt;  // docid is required
    }
    result.docid = *docid;
    
    // Extract score (required)
    auto score = extract_number_field(json_object, "score");
    if (!score) {
        return std::nullopt;
    }
    result.score = *score;
    
    // Extract optional fields
    auto collection = extract_string_field(json_object, "collection");
    result.collection = collection.value_or("");
    
    auto file = extract_string_field(json_object, "file");
    result.file = file.value_or("");
    
    auto snippet = extract_string_field(json_object, "snippet");
    result.snippet = snippet.value_or("");
    
    auto body = extract_string_field(json_object, "body");
    result.body = body.value_or("");
    
    return result;
}

std::vector<QmdQueryResult> JsonParser::parse_qmd_json_array(const std::string& json) {
    std::vector<QmdQueryResult> results;
    
    // Check for empty result markers
    std::string lower_json = json;
    std::transform(lower_json.begin(), lower_json.end(), lower_json.begin(), ::tolower);
    
    if (lower_json.find("no results found") != std::string::npos ||
        lower_json.find("[]") != std::string::npos) {
        return results;
    }
    
    // Split into individual JSON objects
    auto json_objects = split_json_array(json);
    
    for (const auto& obj : json_objects) {
        auto result = parse_qmd_result(obj);
        if (result) {
            results.push_back(*result);
        }
    }
    
    return results;
}

}  // namespace memory
}  // namespace hunterclaw
