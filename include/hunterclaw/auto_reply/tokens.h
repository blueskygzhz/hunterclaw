#ifndef HUNTERCLAW_AUTO_REPLY_TOKENS_H
#define HUNTERCLAW_AUTO_REPLY_TOKENS_H

#include <string>
#include <optional>
#include <regex>

namespace hunterclaw {
namespace auto_reply {

// Token constants (aligned with OpenClaw: tokens.ts)
inline const std::string HEARTBEAT_TOKEN = "HEARTBEAT_OK";
inline const std::string SILENT_REPLY_TOKEN = "NO_REPLY";

/**
 * Check if text is exactly the silent reply token (with optional whitespace).
 * Aligned with OpenClaw: isSilentReplyText()
 *
 * Only matches the exact token with optional surrounding whitespace.
 * Prevents substantive replies ending with NO_REPLY from being suppressed.
 */
inline bool is_silent_reply_text(
    const std::string& text,
    const std::string& token = SILENT_REPLY_TOKEN
) {
    if (text.empty()) return false;
    // Build regex: ^\s*<token>\s*$
    std::string escaped;
    for (char c : token) {
        if (std::string("^$.|?*+()[]{}\\").find(c) != std::string::npos) {
            escaped += '\\';
        }
        escaped += c;
    }
    std::regex exact_re{"^\\s*" + escaped + "\\s*$"};
    return std::regex_match(text, exact_re);
}

/**
 * Strip a trailing silent reply token from mixed-content text.
 * Returns the remaining text with the token removed (trimmed).
 * Aligned with OpenClaw: stripSilentToken()
 */
inline std::string strip_silent_token(
    const std::string& text,
    const std::string& token = SILENT_REPLY_TOKEN
) {
    if (text.empty()) return text;
    // Build regex: (?:^|\s+|\*+)<token>\s*$
    std::string escaped;
    for (char c : token) {
        if (std::string("^$.|?*+()[]{}\\").find(c) != std::string::npos) {
            escaped += '\\';
        }
        escaped += c;
    }
    std::regex trailing_re{"(?:^|\\s+|\\*+)" + escaped + "\\s*$"};
    std::string result = std::regex_replace(text, trailing_re, "");
    // Trim
    size_t s = result.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return "";
    size_t e = result.find_last_not_of(" \t\r\n");
    return result.substr(s, e - s + 1);
}

/**
 * Check if text is a prefix of the silent reply token (for streaming detection).
 * Aligned with OpenClaw: isSilentReplyPrefixText()
 *
 * Guards against suppressing natural-language "No..." text while still
 * catching uppercase lead fragments like "NO" from streamed NO_REPLY.
 */
inline bool is_silent_reply_prefix_text(
    const std::string& text,
    const std::string& token = SILENT_REPLY_TOKEN
) {
    if (text.empty()) return false;

    // Trim start
    size_t s = text.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    std::string trimmed = text.substr(s);
    if (trimmed.empty()) return false;

    // Must be all uppercase
    for (char c : trimmed) {
        if (c != std::toupper(c)) return false;
    }

    std::string normalized = trimmed;
    if (normalized.empty()) return false;
    if (normalized.size() < 2) return false;

    // Must only contain A-Z and _
    for (char c : normalized) {
        if (!std::isupper(c) && c != '_') return false;
    }

    std::string token_upper = token;
    std::transform(token_upper.begin(), token_upper.end(), token_upper.begin(), ::toupper);

    if (token_upper.substr(0, normalized.size()) != normalized) return false;

    // If contains underscore → valid prefix
    if (normalized.find('_') != std::string::npos) return true;

    // Only allow bare "NO" for SILENT_REPLY_TOKEN (NO_REPLY streaming fragment)
    return token_upper == SILENT_REPLY_TOKEN && normalized == "NO";
}

} // namespace auto_reply
} // namespace hunterclaw

#endif // HUNTERCLAW_AUTO_REPLY_TOKENS_H
