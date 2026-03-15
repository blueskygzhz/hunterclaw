#pragma once

#include "hunterclaw/memory/qmd_manager.h"
#include <string>
#include <vector>
#include <optional>

namespace hunterclaw {
namespace memory {

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Simple JSON Parser for QMD Query Results
// 
// 这是一个轻量级的JSON解析器,专门用于解析QMD查询结果
// 避免引入额外的JSON库依赖(如nlohmann/json)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

class JsonParser {
public:
    // Parse QMD query result JSON array
    // Format: [{"docid":"...","score":0.85,"collection":"...","file":"...","snippet":"..."}]
    static std::vector<QmdQueryResult> parse_qmd_json_array(const std::string& json);
    
    // Parse single QMD result object
    static std::optional<QmdQueryResult> parse_qmd_result(const std::string& json_object);
    
    // Extract string field from JSON object
    static std::optional<std::string> extract_string_field(
        const std::string& json,
        const std::string& field_name
    );
    
    // Extract number field from JSON object
    static std::optional<double> extract_number_field(
        const std::string& json,
        const std::string& field_name
    );
    
    // Split JSON array into individual objects
    static std::vector<std::string> split_json_array(const std::string& json_array);
    
private:
    // Utility: trim whitespace
    static std::string trim(const std::string& str);
    
    // Utility: unescape JSON string
    static std::string unescape_json_string(const std::string& str);
};

}  // namespace memory
}  // namespace hunterclaw
