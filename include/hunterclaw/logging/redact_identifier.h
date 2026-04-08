// 🎯 严格对齐 OpenClaw: src/logging/redact-identifier.ts
// SHA-256 不可逆标识符脱敏 — 公共接口
#pragma once

#include <string>

namespace hunterclaw {
namespace logging {

// 🎯 对齐 OpenClaw: sha256HexPrefix(value, len?)
// 返回 SHA-256 hex 前 len（默认 12）个字符
std::string sha256_hex_prefix(const std::string& value, int len = 12);

// 🎯 对齐 OpenClaw: redactIdentifier(value, opts?)
// 返回 "sha256:<hash>" 或 "-"（空值时）
std::string redact_identifier(const std::string& value, int len = 12);

}  // namespace logging
}  // namespace hunterclaw
