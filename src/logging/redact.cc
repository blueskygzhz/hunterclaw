// 🎯 对齐 OpenClaw: src/logging/redact.ts
// 实现日志敏感信息脱敏功能
// 调用 security/safe_regex 的 compile_safe_regex 进行安全正则编译

#include <string>
#include <vector>
#include <regex>
#include <optional>
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace logging {

// Forward declarations from security module
namespace security {
    std::optional<std::regex> compile_safe_regex(
        const std::string& source,
        std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript
    );
}

// ============================================================================
// Constants - 对齐 OpenClaw logging/redact.ts
// ============================================================================

enum class RedactSensitiveMode { Off, Tools };

static constexpr int DEFAULT_REDACT_MIN_LENGTH = 18;
static constexpr int DEFAULT_REDACT_KEEP_START = 6;

// 🎯 对齐 OpenClaw: DEFAULT_REDACT_PATTERNS
static const std::vector<std::string> DEFAULT_REDACT_PATTERNS = {
    "sk-[A-Za-z0-9]{20,}",           // OpenAI keys
    "xoxb-[A-Za-z0-9-]+",            // Slack bot tokens
    "xoxp-[A-Za-z0-9-]+",            // Slack user tokens
    "ghp_[A-Za-z0-9]{36,}",          // GitHub PATs
    "glpat-[A-Za-z0-9_-]{20,}",      // GitLab PATs
    "AIza[A-Za-z0-9_-]{35}",         // Google API keys
    "AKIA[A-Z0-9]{16}",              // AWS Access Key IDs
    "[a-f0-9]{64}",                   // Generic 64-char hex (API keys)
};

// ============================================================================
// resolve_redact_mode - 对齐 OpenClaw: resolveRedactMode
// ============================================================================

static RedactSensitiveMode resolve_redact_mode(const std::string& value) {
    return value == "off" ? RedactSensitiveMode::Off : RedactSensitiveMode::Tools;
}

// ============================================================================
// parse_pattern - 对齐 OpenClaw: parsePattern
// 🎯 调用 compile_safe_regex 进行安全正则编译
// ============================================================================

static std::optional<std::regex> parse_pattern(const std::string& raw) {
    std::string trimmed = raw;
    // Trim whitespace
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return std::nullopt;
    trimmed = trimmed.substr(start, end - start + 1);
    if (trimmed.empty()) return std::nullopt;

    // 🎯 对齐 OpenClaw: Check if pattern is /regex/flags format
    if (trimmed.size() >= 2 && trimmed[0] == '/') {
        size_t last_slash = trimmed.rfind('/');
        if (last_slash > 0 && last_slash != std::string::npos) {
            std::string pattern = trimmed.substr(1, last_slash - 1);
            std::string flags_str = trimmed.substr(last_slash + 1);
            auto flags = std::regex_constants::ECMAScript;
            if (flags_str.find('i') != std::string::npos) flags |= std::regex_constants::icase;
            // 🎯 对齐 OpenClaw: compileSafeRegex(match[1], flags)
            return security::compile_safe_regex(pattern, flags);
        }
    }

    // 🎯 对齐 OpenClaw: compileSafeRegex(raw, "gi")
    return security::compile_safe_regex(trimmed, 
        std::regex_constants::ECMAScript | std::regex_constants::icase);
}

// ============================================================================
// resolve_patterns - 对齐 OpenClaw: resolvePatterns
// ============================================================================

static std::vector<std::regex> resolve_patterns(const std::vector<std::string>& custom_patterns = {}) {
    const auto& source = custom_patterns.empty() ? DEFAULT_REDACT_PATTERNS : custom_patterns;
    std::vector<std::regex> result;
    for (const auto& raw : source) {
        auto re = parse_pattern(raw);
        if (re.has_value()) {
            result.push_back(*re);
        }
    }
    return result;
}

// ============================================================================
// mask_token - 对齐 OpenClaw: maskToken
// ============================================================================

static std::string mask_token(const std::string& token) {
    if ((int)token.size() < DEFAULT_REDACT_MIN_LENGTH) {
        return "***";
    }
    return token.substr(0, DEFAULT_REDACT_KEEP_START) + "***";
}

// ============================================================================
// redact_sensitive - 对齐 OpenClaw: redactSensitive
// 🎯 使用通过 compile_safe_regex 编译的安全正则进行脱敏
// ============================================================================

std::string redact_sensitive(
    const std::string& text,
    const std::string& mode_str,
    const std::vector<std::string>& custom_patterns
) {
    auto mode = resolve_redact_mode(mode_str);
    if (mode == RedactSensitiveMode::Off) {
        return text;
    }

    auto patterns = resolve_patterns(custom_patterns);
    if (patterns.empty()) {
        return text;
    }

    std::string result = text;
    for (const auto& pattern : patterns) {
        try {
            std::string replaced;
            auto begin = std::sregex_iterator(result.begin(), result.end(), pattern);
            auto end = std::sregex_iterator();
            size_t last_pos = 0;
            
            for (auto it = begin; it != end; ++it) {
                replaced += result.substr(last_pos, it->position() - last_pos);
                replaced += mask_token(it->str());
                last_pos = it->position() + it->length();
            }
            replaced += result.substr(last_pos);
            result = replaced;
        } catch (const std::regex_error&) {
            // 🎯 对齐 OpenClaw: silent fallthrough on regex errors
            continue;
        }
    }
    return result;
}

}  // namespace logging
}  // namespace hunterclaw
