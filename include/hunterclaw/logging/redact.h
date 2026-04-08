// 🎯 严格对齐 OpenClaw: src/logging/redact.ts
// 日志敏感信息脱敏引擎 — 公共接口
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace hunterclaw {
namespace logging {

// ============================================================================
// Types — 对齐 OpenClaw: RedactSensitiveMode, RedactOptions
// ============================================================================

enum class RedactSensitiveMode { Off, Tools };

struct RedactOptions {
    std::optional<std::string> mode;                     // "off" | "tools"
    std::optional<std::vector<std::string>> patterns;    // custom patterns override
};

// ============================================================================
// Constants — 对齐 OpenClaw: redact-bounded.ts
// ============================================================================

static constexpr size_t REDACT_REGEX_CHUNK_THRESHOLD = 32768;  // 32KB
static constexpr size_t REDACT_REGEX_CHUNK_SIZE = 16384;       // 16KB

// ============================================================================
// Public API
// ============================================================================

// 🎯 对齐 OpenClaw: redactSensitiveText(text, options?)
// 对文本进行正则模式匹配脱敏，默认 mode="tools"
std::string redact_sensitive_text(
    const std::string& text,
    const RedactOptions& options = {}
);

// 🎯 对齐 OpenClaw: redactToolDetail(detail)
// 仅在 mode=tools 时脱敏工具输出
std::string redact_tool_detail(const std::string& detail);

// 🎯 对齐 OpenClaw: getDefaultRedactPatterns()
// 返回默认正则模式列表副本
std::vector<std::string> get_default_redact_patterns();

}  // namespace logging
}  // namespace hunterclaw
