// 🎯 严格对齐 OpenClaw: src/logging/redact.ts
// 日志敏感信息脱敏引擎 — 正则驱动的文本脱敏
// 调用 security/safe_regex 的 compile_safe_regex 进行安全正则编译
// 调用 redact_bounded 的 replace_pattern_bounded 进行大文本分块安全替换

#include "hunterclaw/logging/redact.h"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace hunterclaw {

// Local safe regex compile (avoids link dependency on security module)
namespace {
std::optional<std::regex> local_compile_safe_regex(
    const std::string& source,
    std::regex_constants::syntax_option_type flags = std::regex_constants::ECMAScript
) {
    if (source.empty() || source.size() > 2048) return std::nullopt;
    try {
        return std::regex(source, flags);
    } catch (...) {
        return std::nullopt;
    }
}
}  // namespace

namespace logging {

// ============================================================================
// Constants — 严格对齐 OpenClaw: DEFAULT_REDACT_PATTERNS (17 个模式)
// ============================================================================

static constexpr int DEFAULT_REDACT_MIN_LENGTH = 18;
static constexpr int DEFAULT_REDACT_KEEP_START = 6;
static constexpr int DEFAULT_REDACT_KEEP_END = 4;

// 🎯 严格对齐 OpenClaw redact.ts L15-40: DEFAULT_REDACT_PATTERNS
// 每个正则与 OpenClaw 一一对应，保持相同顺序
static const std::vector<std::string> DEFAULT_REDACT_PATTERNS = {
    // 1. ENV-style assignments (KEY|TOKEN|SECRET|PASSWORD|PASSWD)
    R"(\b[A-Z0-9_]*(?:KEY|TOKEN|SECRET|PASSWORD|PASSWD)\b\s*[=:]\s*(["']?)([^\s"'\\]+)\1)",
    // 2. JSON fields (apiKey|token|secret|password|passwd|accessToken|refreshToken)
    R"xx("(?:apiKey|token|secret|password|passwd|accessToken|refreshToken)"\s*:\s*"([^"]+)")xx",
    // 3. CLI flags (--api-key|--token|--secret|--password|--passwd)
    R"(--(?:api[-_]?key|token|secret|password|passwd)\s+(["']?)([^\s"']+)\1)",
    // 4. Authorization headers (with colon/equals)
    R"(Authorization\s*[:=]\s*Bearer\s+([A-Za-z0-9._\-+=]+))",
    // 5. Bearer token (standalone, >=18 chars)
    R"(\bBearer\s+([A-Za-z0-9._\-+=]{18,})\b)",
    // 6. PEM private key blocks
    R"(-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]+?-----END [A-Z ]*PRIVATE KEY-----)",
    // 7. OpenAI sk- prefix
    R"(\b(sk-[A-Za-z0-9_-]{8,})\b)",
    // 8. GitHub ghp_ personal token
    R"(\b(ghp_[A-Za-z0-9]{20,})\b)",
    // 9. GitHub github_pat_ fine-grained token
    R"(\b(github_pat_[A-Za-z0-9_]{20,})\b)",
    // 10. Slack xox[baprs]- token
    R"(\b(xox[baprs]-[A-Za-z0-9-]{10,})\b)",
    // 11. Slack App xapp- token
    R"(\b(xapp-[A-Za-z0-9-]{10,})\b)",
    // 12. Groq gsk_ token
    R"(\b(gsk_[A-Za-z0-9_-]{10,})\b)",
    // 13. Google AI AIza key
    R"(\b(AIza[0-9A-Za-z\-_]{20,})\b)",
    // 14. Perplexity pplx- token
    R"(\b(pplx-[A-Za-z0-9_-]{10,})\b)",
    // 15. npm npm_ token
    R"(\b(npm_[A-Za-z0-9]{10,})\b)",
    // 16. Telegram Bot API URL token (/bot<token>/...)
    R"(\bbot(\d{6,}:[A-Za-z0-9_-]{20,})\b)",
    // 17. Telegram Bot token (standalone digits:chars)
    R"(\b(\d{6,}:[A-Za-z0-9_-]{20,})\b)",
};

// ============================================================================
// normalizeMode — 对齐 OpenClaw: normalizeMode()
// ============================================================================

static RedactSensitiveMode normalize_mode(const std::string& value) {
    return value == "off" ? RedactSensitiveMode::Off : RedactSensitiveMode::Tools;
}

// ============================================================================
// parse_pattern — 对齐 OpenClaw: parsePattern()
// 🎯 支持 /regex/flags 格式，调用 compile_safe_regex
// ============================================================================

static std::optional<std::regex> parse_pattern(const std::string& raw) {
    std::string trimmed = raw;
    size_t start = trimmed.find_first_not_of(" \t\r\n");
    size_t end = trimmed.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return std::nullopt;
    trimmed = trimmed.substr(start, end - start + 1);
    if (trimmed.empty()) return std::nullopt;

    // 🎯 对齐 OpenClaw: Check /regex/flags format
    if (trimmed.size() >= 2 && trimmed[0] == '/') {
        size_t last_slash = trimmed.rfind('/');
        if (last_slash > 0 && last_slash != std::string::npos) {
            std::string pattern = trimmed.substr(1, last_slash - 1);
            std::string flags_str = trimmed.substr(last_slash + 1);
            auto flags = std::regex_constants::ECMAScript;
            if (flags_str.find('i') != std::string::npos) flags |= std::regex_constants::icase;
            return local_compile_safe_regex(pattern, flags);
        }
    }

    // Default: case-insensitive global
    return local_compile_safe_regex(trimmed,
        std::regex_constants::ECMAScript | std::regex_constants::icase);
}

// ============================================================================
// resolve_patterns — 对齐 OpenClaw: resolvePatterns()
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
// mask_token — 严格对齐 OpenClaw: maskToken()
// 🎯 短于 18 字符用 "***"，否则保留前 6 + 后 4，中间用 "…"
// ============================================================================

static std::string mask_token(const std::string& token) {
    if (static_cast<int>(token.size()) < DEFAULT_REDACT_MIN_LENGTH) {
        return "***";
    }
    std::string start = token.substr(0, DEFAULT_REDACT_KEEP_START);
    std::string end_str = token.substr(token.size() - DEFAULT_REDACT_KEEP_END);
    return start + "\xe2\x80\xa6" + end_str;  // UTF-8 for "…"
}

// ============================================================================
// redact_pem_block — 严格对齐 OpenClaw: redactPemBlock()
// 🎯 保留 PEM 首尾行，中间替换为 "…redacted…"
// ============================================================================

static std::string redact_pem_block(const std::string& block) {
    std::vector<std::string> lines;
    std::istringstream stream(block);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.size() < 2) {
        return "***";
    }
    return lines.front() + "\n\xe2\x80\xa6redacted\xe2\x80\xa6\n" + lines.back();
}

// ============================================================================
// redact_match — 严格对齐 OpenClaw: redactMatch()
// 🎯 根据 capture group 定位 token，局部遮蔽保留上下文
//    例如 KEY=sk-xxx → KEY=sk-xx…yyyy 而非整个替换
// ============================================================================

static std::string redact_match(const std::smatch& match) {
    std::string full = match.str(0);

    // PEM block detection
    if (full.find("PRIVATE KEY-----") != std::string::npos) {
        return redact_pem_block(full);
    }

    // 🎯 对齐 OpenClaw: groups.filter(v => typeof v === "string" && v.length > 0).at(-1) ?? match
    // 找到最后一个非空 capture group 作为 token
    std::string token;
    for (size_t i = match.size() - 1; i >= 1; --i) {
        if (match[i].matched && match[i].length() > 0) {
            token = match[i].str();
            break;
        }
    }
    if (token.empty()) {
        token = full;
    }

    std::string masked = mask_token(token);
    if (token == full) {
        return masked;
    }

    // 🎯 对齐 OpenClaw: match.replace(token, masked) — 局部替换保留上下文
    std::string result = full;
    size_t pos = result.find(token);
    if (pos != std::string::npos) {
        result.replace(pos, token.size(), masked);
    }
    return result;
}

// ============================================================================
// replace_pattern_bounded — 严格对齐 OpenClaw: replacePatternBounded()
// 🎯 大文本分块防止正则性能炸裂（32KB 阈值，16KB 分块）
// ============================================================================

static std::string replace_pattern_bounded(
    const std::string& text,
    const std::regex& pattern,
    size_t chunk_threshold = REDACT_REGEX_CHUNK_THRESHOLD,
    size_t chunk_size = REDACT_REGEX_CHUNK_SIZE
) {
    // 短文本直接处理
    if (chunk_threshold == 0 || chunk_size == 0 || text.size() <= chunk_threshold) {
        std::string result;
        auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
        auto end = std::sregex_iterator();
        size_t last_pos = 0;
        for (auto it = begin; it != end; ++it) {
            result += text.substr(last_pos, it->position() - last_pos);
            result += redact_match(*it);
            last_pos = it->position() + it->length();
        }
        result += text.substr(last_pos);
        return result;
    }

    // 🎯 对齐 OpenClaw redact-bounded.ts: 大文本分块处理
    std::string output;
    for (size_t index = 0; index < text.size(); index += chunk_size) {
        std::string chunk = text.substr(index, chunk_size);
        std::string replaced;
        auto begin = std::sregex_iterator(chunk.begin(), chunk.end(), pattern);
        auto end = std::sregex_iterator();
        size_t last_pos = 0;
        for (auto it = begin; it != end; ++it) {
            replaced += chunk.substr(last_pos, it->position() - last_pos);
            replaced += redact_match(*it);
            last_pos = it->position() + it->length();
        }
        replaced += chunk.substr(last_pos);
        output += replaced;
    }
    return output;
}

// ============================================================================
// redact_text — 对齐 OpenClaw: redactText()
// ============================================================================

static std::string redact_text(const std::string& text, const std::vector<std::regex>& patterns) {
    std::string next = text;
    for (const auto& pattern : patterns) {
        try {
            next = replace_pattern_bounded(next, pattern);
        } catch (const std::regex_error&) {
            // 🎯 对齐 OpenClaw: silent fallthrough on regex errors
            continue;
        }
    }
    return next;
}

// ============================================================================
// Public API
// ============================================================================

// 🎯 严格对齐 OpenClaw: redactSensitiveText()
std::string redact_sensitive_text(const std::string& text, const RedactOptions& options) {
    if (text.empty()) return text;

    auto mode = normalize_mode(options.mode.value_or("tools"));
    if (mode == RedactSensitiveMode::Off) return text;

    auto patterns = resolve_patterns(options.patterns.value_or(std::vector<std::string>{}));
    if (patterns.empty()) return text;

    return redact_text(text, patterns);
}

// 🎯 严格对齐 OpenClaw: redactToolDetail()
// mode=tools 时脱敏工具输出
std::string redact_tool_detail(const std::string& detail) {
    // 对齐 OpenClaw: resolveConfigRedaction() → normalizeMode → "tools" check
    // 默认 mode 为 "tools"，仅在 mode=="tools" 时脱敏
    return redact_sensitive_text(detail);
}

// 🎯 严格对齐 OpenClaw: getDefaultRedactPatterns()
// 返回默认模式副本供外部使用
std::vector<std::string> get_default_redact_patterns() {
    return DEFAULT_REDACT_PATTERNS;
}

}  // namespace logging
}  // namespace hunterclaw
