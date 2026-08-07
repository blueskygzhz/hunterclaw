// 🎯 严格对齐 OpenClaw: src/logging/secret-redaction-registry.ts
// 进程内"精确值"脱敏注册表：注册具体 secret 值，后续日志按精确匹配遮蔽。
// 与 redact.cc 的正则脱敏互补——正则处理"形态已知"的凭据，注册表处理
// "值已知但形态任意"的凭据 (如 MCP server URL、Bearer token、query 参数)。
#pragma once

#include <functional>
#include <string>

namespace hunterclaw {
namespace logging {

// 🎯 对齐 OpenClaw MIN_SECRET_VALUE_LENGTH / MAX_SECRET_VALUES
static constexpr size_t SECRET_MIN_VALUE_LENGTH = 6;    // 短于6字符不注册 (避免噪声/误伤)
static constexpr size_t SECRET_MAX_VALUES = 512;        // 有界注册表, LRU 式淘汰

// 🎯 对齐 OpenClaw registerSecretValueForRedaction(value)
// 注册一个密钥的精确值用于日志脱敏。注册时同时登记多种形态:
//   1. percent-encoded (URL egress 会编码)
//   2. JSON-escaped   (结构化 payload 序列化后带转义)
//   3. 原始值 (最后登记, 保证有界淘汰不会先丢原始值)
void register_secret_value_for_redaction(const std::string& value);

// 🎯 对齐 OpenClaw isSecretValueRegisteredForRedaction(value)
bool is_secret_value_registered_for_redaction(const std::string& value);

// 🎯 对齐 OpenClaw hasRegisteredSecretValuesForRedaction()
bool has_registered_secret_values_for_redaction();

// 🎯 对齐 OpenClaw redactRegisteredSecretValues(text, mask)
// 用调用方提供的 mask 回调替换文本中所有已注册的精确值 (长度降序匹配)。
std::string redact_registered_secret_values(
    const std::string& text,
    const std::function<std::string(const std::string&)>& mask
);

// 测试/复位用: 清空注册表
void clear_registered_secret_values_for_redaction();

}  // namespace logging
}  // namespace hunterclaw
