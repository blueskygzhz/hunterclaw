// ============================================================================
// Support Tokens 实现 — 严格对齐 OpenClaw sessions-send-tokens.ts
//                                  + auto-reply/tokens.ts
// ============================================================================

#include "hunterclaw/tools/support_tokens.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace hunterclaw {
namespace tools {
namespace support_tokens {

// ── Token 常量 ──────────────────────────────────────────────────────────────
// 対齐 sessions-send-tokens.ts:9,11 与 auto-reply/tokens.ts:5,7。
// 均为裸值，不带 "__" 包裹。

const std::string ANNOUNCE_SKIP_TOKEN = "ANNOUNCE_SKIP";
const std::string REPLY_SKIP_TOKEN    = "REPLY_SKIP";
const std::string SILENT_REPLY_TOKEN  = "NO_REPLY";
const std::string HEARTBEAT_TOKEN     = "HEARTBEAT_OK";

// ── 辅助 ────────────────────────────────────────────────────────────────────

namespace {

bool is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// 対齐 JS String.prototype.trim()（仅处理 ASCII 空白，足够本用途）
std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && is_space(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_space(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

/// 対齐 tokens.ts:47-49 stripEdgePunctuation —— 剥去首尾标点。
/// 真源用 \p{P}（Unicode 标点）；此处覆盖 ASCII 标点 + 模型常用的
/// 全角标点与 Markdown 强调符，足以覆盖 `**NO_REPLY**`、`NO_REPLY。` 等形态。
/// 注意**不能**剥去表情符号等 Symbol 类字符 —— 真源刻意把它们视为实质内容
/// （见 tokens.ts:61-62），剥掉会把 "🎉" 这类回复误判为静默。
bool is_edge_punct_ascii(unsigned char c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/// 常见全角/中文标点的 UTF-8 序列（逐个 3 字节）
const char* const kWidePuncts[] = {
    "，", "。", "、", "；", "：", "？", "！", "“", "”", "‘", "’",
    "（", "）", "【", "】", "《", "》", "—", "…", "·",
};

bool try_strip_wide_punct_prefix(const std::string& s, size_t* pos) {
    for (const char* p : kWidePuncts) {
        const size_t n = std::string(p).size();
        if (s.compare(*pos, n, p) == 0) {
            *pos += n;
            return true;
        }
    }
    return false;
}

bool try_strip_wide_punct_suffix(const std::string& s, size_t* end) {
    for (const char* p : kWidePuncts) {
        const size_t n = std::string(p).size();
        if (*end >= n && s.compare(*end - n, n, p) == 0) {
            *end -= n;
            return true;
        }
    }
    return false;
}

std::string strip_edge_punctuation(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    for (;;) {
        if (b < e && is_edge_punct_ascii(static_cast<unsigned char>(s[b]))) {
            ++b;
            continue;
        }
        if (b < e && try_strip_wide_punct_prefix(s, &b)) continue;
        break;
    }
    for (;;) {
        if (e > b && is_edge_punct_ascii(static_cast<unsigned char>(s[e - 1]))) {
            --e;
            continue;
        }
        if (e > b && try_strip_wide_punct_suffix(s, &e)) continue;
        break;
    }
    return s.substr(b, e - b);
}

/// 対齐 tokens.ts:29 的正则 `^\s*TOKEN(?:\s+TOKEN)*\s*$`（大小写不敏感）
/// 即：整串仅由该 token（可空白分隔重复）构成。
bool matches_token_only(const std::string& text, const std::string& token) {
    if (token.empty()) return false;
    const std::string t = trim(text);
    if (t.empty()) return false;

    const std::string lower_text  = to_lower(t);
    const std::string lower_token = to_lower(token);

    size_t pos = 0;
    bool matched_once = false;
    for (;;) {
        if (lower_text.compare(pos, lower_token.size(), lower_token) != 0) return false;
        pos += lower_token.size();
        matched_once = true;
        if (pos == lower_text.size()) return matched_once;
        // 重复出现之间必须有至少一个空白（対齐 `\s+`）
        size_t ws = pos;
        while (ws < lower_text.size() &&
               is_space(static_cast<unsigned char>(lower_text[ws]))) {
            ++ws;
        }
        if (ws == pos) return false;  // 无分隔 → 后面是别的内容
        pos = ws;
    }
}

}  // namespace

// ── 判定函数 ────────────────────────────────────────────────────────────────

bool is_silent_reply_text(const std::string& text, const std::string& token) {
    // 対齐 tokens.ts:63-66 —— 原串与「剥去首尾标点后的串」任一命中即为静默。
    return matches_token_only(text, token) ||
           matches_token_only(strip_edge_punctuation(trim(text)), token);
}

bool is_announce_skip(const std::string& reply) {
    // 対齐 sessions-send-tokens.ts:22 —— `text.trim() === ANNOUNCE_SKIP_TOKEN`。
    // 真源此处是**严格相等**（区分大小写、不剥标点），比 isSilentReplyText 更严；
    // 刻意保持一致，不要"顺手放宽"。
    return trim(reply) == ANNOUNCE_SKIP_TOKEN;
}

bool is_reply_skip(const std::string& reply) {
    // 対齐 sessions-send-tokens.ts:27 —— 同上，严格相等。
    return trim(reply) == REPLY_SKIP_TOKEN;
}

bool is_silent_reply(const std::string& reply) {
    return is_silent_reply_text(reply, SILENT_REPLY_TOKEN);
}

bool is_heartbeat(const std::string& reply) {
    return is_silent_reply_text(reply, HEARTBEAT_TOKEN);
}

bool is_non_deliverable_sessions_reply(const std::string& reply) {
    // 対齐 sessions-send-tokens.ts:31-33 —— 四个 token 各自做 token-only 判定。
    // 注意这里用的是 isSilentReplyText（宽松版），与 is_announce_skip /
    // is_reply_skip 的严格相等**不同**，这正是真源的行为。
    return is_silent_reply_text(reply, ANNOUNCE_SKIP_TOKEN) ||
           is_silent_reply_text(reply, REPLY_SKIP_TOKEN) ||
           is_silent_reply_text(reply, SILENT_REPLY_TOKEN) ||
           is_silent_reply_text(reply, HEARTBEAT_TOKEN);
}

std::string strip_trailing_silent_token(const std::string& text,
                                        const std::string& token) {
    // 対齐 tokens.ts:42 的正则 `(?:^|\s+|\*+)TOKEN(?:\s+TOKEN)*\s*$`
    // —— 仅剥结尾，且最左侧那个 token 之前必须是串首 / 空白 / `*`（Markdown 强调）。
    // 这个边界很关键：没有它，"NO_REPLYING" 之类的正文会被截断。
    if (token.empty()) return text;

    const std::string lower_text  = to_lower(text);
    const std::string lower_token = to_lower(token);

    // 先跳过尾部空白（対齐正则末尾的 `\s*$`）
    size_t cursor = text.size();
    while (cursor > 0 && is_space(static_cast<unsigned char>(text[cursor - 1]))) --cursor;

    // 反向逐个吃 token，token 之间必须由空白分隔（対齐 `(?:\s+TOKEN)*`）。
    // cut 记录最左侧被吃掉的 token 起点；npos 表示一个都没匹配上。
    size_t cut = std::string::npos;
    for (;;) {
        if (cursor < lower_token.size()) break;
        const size_t start = cursor - lower_token.size();
        if (lower_text.compare(start, lower_token.size(), lower_token) != 0) break;

        // token 之前必须是串首 / 空白 / '*'，否则说明它是正文的一部分
        if (start == 0) {
            cut = 0;
            break;
        }
        const unsigned char prev = static_cast<unsigned char>(text[start - 1]);
        if (!is_space(prev) && prev != '*') break;

        cut = start;
        if (prev == '*') break;  // '*' 边界：Markdown 强调，不再继续向前吃重复 token

        // 跳过分隔空白，尝试继续吃前一个重复 token
        size_t back = start;
        while (back > 0 && is_space(static_cast<unsigned char>(text[back - 1]))) --back;
        cursor = back;
    }

    if (cut == std::string::npos) return text;
    // 剥掉 token 后，把边界处残留的 `*` 一并去掉（対齐正则里的 `\*+`）
    size_t tail = cut;
    while (tail > 0 && text[tail - 1] == '*') --tail;
    return trim(text.substr(0, tail));
}

std::string strip_support_tokens(const std::string& reply) {
    std::string result = reply;
    result = strip_trailing_silent_token(result, ANNOUNCE_SKIP_TOKEN);
    result = strip_trailing_silent_token(result, REPLY_SKIP_TOKEN);
    result = strip_trailing_silent_token(result, SILENT_REPLY_TOKEN);
    result = strip_trailing_silent_token(result, HEARTBEAT_TOKEN);
    return result;
}

std::string select_deliverable_sessions_reply(const std::string& primary,
                                             const std::string& fallback) {
    // 対齐:40 —— primary?.trim()
    const std::string primary_reply = trim(primary);

    // 対齐:41-43 —— primary 有内容且可投递 → 直接用
    if (!primary_reply.empty() && !is_non_deliverable_sessions_reply(primary_reply)) {
        return primary_reply;
    }
    // 対齐:44-46 —— primary 是 non-deliverable 但**不是** NO_REPLY
    // （即 ANNOUNCE_SKIP / REPLY_SKIP / HEARTBEAT_OK）→ 直接放弃。
    // 这三个是明确的"别发"指令，绝不能回落 fallback 绕过它。
    if (!primary_reply.empty() &&
        !is_silent_reply_text(primary_reply, SILENT_REPLY_TOKEN)) {
        return std::string();
    }
    // 対齐:47-48 —— 走到这里说明 primary 为空或恰是 NO_REPLY，可用兜底；
    // 但兜底自身也必须过 non-deliverable 检查。
    const std::string fallback_reply = trim(fallback);
    if (!fallback_reply.empty() && !is_non_deliverable_sessions_reply(fallback_reply)) {
        return fallback_reply;
    }
    return std::string();
}

}  // namespace support_tokens
}  // namespace tools
}  // namespace hunterclaw
