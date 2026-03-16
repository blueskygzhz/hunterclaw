#include "hunterclaw/mcp/json_converter.h"
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace mcp {
namespace converters {

namespace {
    auto log() {
        static auto logger = spdlog::get("mcp");
        if (!logger) {
            logger = spdlog::stdout_logger_mt("mcp");
        }
        return logger;
    }
}

// ============================================================================
// JsonValue -> nlohmann::json
// ============================================================================

nlohmann::json JsonConverter::to_nlohmann(const core::JsonValue& jv) {
    // 使用 JsonValue 自带的 to_json() 方法 (对齐 OpenClaw)
    return jv.to_json();
}

// ============================================================================
// nlohmann::json -> JsonValue
// ============================================================================

core::JsonValue JsonConverter::from_nlohmann(const nlohmann::json& j) {
    // 使用 JsonValue 的 nlohmann::json 构造函数 (对齐 OpenClaw)
    return core::JsonValue(j);
}

// ============================================================================
// 辅助函数
// ============================================================================

nlohmann::json JsonConverter::parse_safe(const std::string& json_str) {
    using json = nlohmann::json;
    
    try {
        return json::parse(json_str);
    } catch (const json::parse_error& e) {
        log()->error("JSON parse error: {} (input: {})", e.what(), json_str);
        return json();
    } catch (const std::exception& e) {
        log()->error("Unexpected error parsing JSON: {} (input: {})", e.what(), json_str);
        return json();
    }
}

std::string JsonConverter::stringify(const nlohmann::json& j, int indent) {
    try {
        return j.dump(indent);
    } catch (const std::exception& e) {
        log()->error("JSON stringify error: {}", e.what());
        return "{}";
    }
}

} // namespace converters
} // namespace mcp
} // namespace hunterclaw
