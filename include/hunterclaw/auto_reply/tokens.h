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
    // Build regex: ^\s*<token>(?:\s+<token>)*\s*$   (case-insensitive)
    // 対齐 tokens.ts:29 getSilentExactRegex —— 三处此前遗漏：
    //   ① `(?:\s+TOKEN)*` 重复 token：模型偶尔连吐两遍 "NO_REPLY NO_REPLY"，
    //      少了这段会被判为实质回复而投递出去；
    //   ② `"i"` 大小写不敏感：真源带 i 标志，"no_reply" 也应命中；
    //   ③ 见下方 stripEdgePunctuation —— `**NO_REPLY**` / "NO_REPLY。"
    //      这类包裹形态需先剥首尾标点再判一次（対齐 tokens.ts:63-66）。
    std::string escaped;
    for (char c : token) {
        if (std::string("^$.|?*+()[]{}\\").find(c) != std::string::npos) {
            escaped += '\\';
        }
        escaped += c;
    }
    const std::regex exact_re{
        "^\\s*" + escaped + "(?:\\s+" + escaped + ")*\\s*$",
        std::regex::icase
    };
    if (std::regex_match(text, exact_re)) return true;

    // 対齐 tokens.ts:47-49 stripEdgePunctuation + :65 的第二次判定。
    // 真源用 \p{P}（Unicode 标点）；std::regex 无该属性，故覆盖 ASCII 标点
    // 与模型常用的全角标点。**刻意不剥 Symbol 类**（emoji 等）——
    // 真源把它们视为实质内容（tokens.ts:61-62），剥掉会让 "🎉" 被误判为静默。
    std::string trimmed = text;
    {
        size_t s = trimmed.find_first_not_of(" \t\r\n\f\v");
        if (s == std::string::npos) return false;
        size_t e = trimmed.find_last_not_of(" \t\r\n\f\v");
        trimmed = trimmed.substr(s, e - s + 1);
    }
    static const std::string kWide[] = {
        "，", "。", "、", "；", "：", "？", "！", "“", "”", "‘", "’",
        "（", "）", "【", "】", "《", "》", "—", "…", "·",
    };
    auto is_ascii_punct = [](unsigned char c) {
        return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
               (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
    };
    size_t b = 0, e2 = trimmed.size();
    for (;;) {
        if (b < e2 && is_ascii_punct(static_cast<unsigned char>(trimmed[b]))) { ++b; continue; }
        bool hit = false;
        for (const auto& w : kWide) {
            if (e2 - b >= w.size() && trimmed.compare(b, w.size(), w) == 0) {
                b += w.size(); hit = true; break;
            }
        }
        if (!hit) break;
    }
    for (;;) {
        if (e2 > b && is_ascii_punct(static_cast<unsigned char>(trimmed[e2 - 1]))) { --e2; continue; }
        bool hit = false;
        for (const auto& w : kWide) {
            if (e2 - b >= w.size() && trimmed.compare(e2 - w.size(), w.size(), w) == 0) {
                e2 -= w.size(); hit = true; break;
            }
        }
        if (!hit) break;
    }
    const std::string depunct = trimmed.substr(b, e2 - b);
    if (depunct == trimmed) return false;  // 无变化则无需重判
    return std::regex_match(depunct, exact_re);
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
    // 対齐 tokens.ts:42 getSilentTrailingRegex —— 正则带 "i" 标志。
    // 此前缺 icase，模型吐小写 "no_reply" 时剥不掉，控制符残留到用户可见文本。
    std::regex trailing_re{
        "(?:^|\\s+|\\*+)" + escaped + "(?:\\s+" + escaped + ")*\\s*$",
        std::regex::icase
    };
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
