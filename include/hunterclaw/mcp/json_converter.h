#pragma once

#include "hunterclaw/core/types.h"
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace mcp {
namespace converters {

/// JsonValue ↔ nlohmann::json 双向转换器
/// 对齐 OpenClaw 的 JSON 处理模式
class JsonConverter {
public:
    /// 将 JsonValue 转换为 nlohmann::json
    /// 
    /// 支持所有 JSON 类型:
    /// - null
    /// - boolean
    /// - number (int64_t / double)
    /// - string
    /// - array
    /// - object
    /// 
    /// 对齐 OpenClaw 的递归转换策略
    static nlohmann::json to_nlohmann(const core::JsonValue& jv);
    
    /// 将 nlohmann::json 转换为 JsonValue
    /// 
    /// 镜像实现 to_nlohmann() 的逆操作
    /// 确保双向转换的幂等性: from(to(x)) == x
    static core::JsonValue from_nlohmann(const nlohmann::json& j);
    
    /// 辅助: 安全解析 JSON 字符串
    /// 
    /// @param json_str JSON 字符串
    /// @return 解析后的 nlohmann::json,失败返回 null
    static nlohmann::json parse_safe(const std::string& json_str);
    
    /// 辅助: 将 nlohmann::json 美化打印为字符串
    /// 
    /// @param j nlohmann::json 对象
    /// @param indent 缩进空格数 (默认 2)
    /// @return 美化后的 JSON 字符串
    static std::string stringify(const nlohmann::json& j, int indent = 2);
};

} // namespace converters
} // namespace mcp
} // namespace hunterclaw
