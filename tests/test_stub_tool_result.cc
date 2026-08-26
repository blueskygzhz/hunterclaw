// ============================================================================
// test_stub_tool_result.cc
// 测试用最小 stub：只提供 remote_a2a 工具依赖的 tools:: 符号。
//
// 为什么用 stub 而不链 src/tools/tool_manager.cc：
//   后者会拖入 MCP / hook / policy / approval / loop_detector 一整条依赖链
//   （15 个 include，跨 8 个库），单测因此需要几乎整个二进制。
//   本文件的实现逐字复制 tool_manager.cc:712-760 的语义，
//   ★ 若那边改了行为，这里必须同步 —— 已在下方逐处标注对应行号。
// ============================================================================

#include "hunterclaw/tools/tool.h"

#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace tools {

// ── ToolParams（対标 src/tools/tool_params.cc 语义）────────────────────────

std::string ToolParams::read_string(const std::string& key, bool required,
                                   const std::string& default_val) const {
    if (!params_.is_object() || !params_.contains(key)) {
        return default_val;
    }
    const auto& v = params_[key];
    if (v.is_string()) return v.as_string();
    return default_val;
}

double ToolParams::read_number(const std::string& key, bool required,
                              double default_val) const {
    if (!params_.is_object() || !params_.contains(key)) return default_val;
    const auto& v = params_[key];
    if (v.is_number()) return v.as_number();
    return default_val;
}

int64_t ToolParams::read_int(const std::string& key, bool required,
                            int64_t default_val) const {
    return static_cast<int64_t>(read_number(key, required,
                                           static_cast<double>(default_val)));
}

bool ToolParams::read_bool(const std::string& key, bool required,
                          bool default_val) const {
    if (!params_.is_object() || !params_.contains(key)) return default_val;
    const auto& v = params_[key];
    if (v.is_bool()) return v.as_bool();
    return default_val;
}

std::vector<std::string> ToolParams::read_string_array(const std::string& key,
                                                       bool required) const {
    std::vector<std::string> out;
    if (!params_.is_object() || !params_.contains(key)) return out;
    const auto& v = params_[key];
    if (!v.is_array()) return out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i].is_string()) out.push_back(v[i].as_string());
    }
    return out;
}

bool ToolParams::has(const std::string& key) const {
    return params_.is_object() && params_.contains(key);
}

// ── tool_result（対标 tool_manager.cc:712-760）─────────────────────────────

namespace tool_result {

/// 対标 tool_manager.cc:712-722
ToolResult text(const std::string& content) {
    ToolResult result;
    result.success = true;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Text;
    c.text = content;
    result.content.push_back(c);
    return result;
}

/// 対标 tool_manager.cc:724-...
ToolResult json(const core::JsonValue& data) {
    ToolResult result;
    result.success = true;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Json;
    c.text = data.to_string();
    result.content.push_back(c);
    return result;
}

ToolResult image(const std::string& base64_data, const std::string& mime_type) {
    ToolResult result;
    result.success = true;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Image;
    c.data = base64_data;
    c.mime_type = mime_type;
    result.content.push_back(c);
    return result;
}

/// ★ success=false 且 error 有值 —— remote_a2a 的多处断言依赖这一点
ToolResult error(const std::string& message) {
    ToolResult result;
    result.success = false;
    result.error = message;
    ToolResultContent c;
    c.type = ToolResultContent::Type::Text;
    c.text = message;
    result.content.push_back(c);
    return result;
}

}  // namespace tool_result

}  // namespace tools
}  // namespace hunterclaw
