// 🎯 严格对齐 OpenClaw: src/logging/redact-identifier.ts
// SHA-256 不可逆标识符脱敏 — 将 PII（电话号码、JID 等）转换为 sha256 哈希前缀
// 用于日志中不可逆脱敏

#include "hunterclaw/logging/redact_identifier.h"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace hunterclaw {
namespace logging {

// ============================================================================
// sha256_hex_prefix — 严格对齐 OpenClaw: sha256HexPrefix(value, len?)
// 返回 SHA-256 hex 前 len（默认 12）个字符
// ============================================================================

std::string sha256_hex_prefix(const std::string& value, int len) {
    int safe_len = (len > 0) ? len : 12;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);

    std::ostringstream hex_stream;
    hex_stream << std::hex << std::setfill('0');
    // SHA-256 输出 32 字节 = 64 hex chars，safe_len 最大 64
    int bytes_needed = (safe_len + 1) / 2;
    for (int i = 0; i < bytes_needed && i < SHA256_DIGEST_LENGTH; ++i) {
        hex_stream << std::setw(2) << static_cast<int>(hash[i]);
    }

    std::string hex_str = hex_stream.str();
    return hex_str.substr(0, static_cast<size_t>(safe_len));
}

// ============================================================================
// redact_identifier — 严格对齐 OpenClaw: redactIdentifier(value, opts?)
// 返回 "sha256:<hash>" 或 "-"（空值时）
// ============================================================================

std::string redact_identifier(const std::string& value, int len) {
    // Trim whitespace
    std::string trimmed = value;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "-";  // 🎯 对齐 OpenClaw: empty → "-"
    }
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    trimmed = trimmed.substr(start, end - start + 1);

    if (trimmed.empty()) {
        return "-";
    }

    return "sha256:" + sha256_hex_prefix(trimmed, len);
}

}  // namespace logging
}  // namespace hunterclaw
