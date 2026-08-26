// ============================================================================
// acp_conversation_id.cc
// 🎯 严格对齐 OpenClaw: src/acp/conversation-id.ts
// ============================================================================

#include "hunterclaw/acp/acp_conversation_id.h"

namespace hunterclaw {
namespace acp {

namespace {

/// 対齐 JS String.prototype.trim()
std::string trim_copy(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

}  // namespace

std::string normalize_conversation_text(const std::string& value) {
    // 対齐 conversation-id.ts:3-5
    return trim_copy(value);
}

std::string normalize_conversation_text(const nlohmann::json& value) {
    // 対齐 conversation-id.ts:3-5 —— string 分支
    if (value.is_string()) {
        return trim_copy(value.get<std::string>());
    }
    // 対齐:6-8 —— number / bigint / boolean 分支：`${value}` 再 trim
    if (value.is_boolean()) {
        // JS 的模板字符串产出 "true"/"false"（小写），不是 "1"/"0"
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer()) {
        return trim_copy(std::to_string(value.get<long long>()));
    }
    if (value.is_number_unsigned()) {
        return trim_copy(std::to_string(value.get<unsigned long long>()));
    }
    if (value.is_number_float()) {
        // 用 json::dump() 而非 std::to_string：后者会产出 "1.000000" 这类
        // 尾随零，与 JS `${1}` → "1" 不一致。
        return trim_copy(value.dump());
    }
    // 対齐:9 —— 其余（null / 对象 / 数组 / 缺失）一律空串
    return std::string();
}

} // namespace acp
} // namespace hunterclaw
