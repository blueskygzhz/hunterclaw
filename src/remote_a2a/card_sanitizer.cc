// ============================================================================
// remote_a2a/card_sanitizer.cc
// AgentCard 文本消杀实现。纯函数、零 IO。
// ============================================================================

#include "hunterclaw/remote_a2a/card_sanitizer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hunterclaw {
namespace remote_a2a {

namespace {

/// 注入模式（全部小写，匹配时对输入做小写副本）。
/// 命中后从该位置到行尾整段替换。
const char* const kInjectionPatterns[] = {
    // 英文
    "ignore previous", "ignore all previous", "ignore the previous",
    "disregard previous", "disregard all previous",
    "forget previous", "forget all previous",
    "new instructions", "new instruction:",
    "override previous", "override all",
    // 角色伪造
    "system:", "assistant:", "developer:", "human:",
    // 聊天模板标记
    "<|im_start|>", "<|im_end|>", "<|system|>", "<|user|>", "<|assistant|>",
    "[inst]", "[/inst]", "<<sys>>", "<</sys>>",
    // 中文
    "忽略之前", "忽略上面", "忽略以上", "忽略前面", "忽略所有",
    "无视之前", "无视上面", "无视以上",
    "新指令", "新的指令", "新任务指令",
};

std::string to_lower_copy(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim_copy(std::string_view s) {
    size_t b = 0, e = s.size();
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

/// UTF-8 首字节 → 该字符总字节数（0 = 非法首字节）
int utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

/// 解码一个 UTF-8 字符。失败返回 0 并把 len 置 1（跳过坏字节）。
uint32_t utf8_decode(const char* p, size_t avail, int* len) {
    const unsigned char c0 = static_cast<unsigned char>(p[0]);
    const int n = utf8_seq_len(c0);
    if (n == 0 || static_cast<size_t>(n) > avail) {
        *len = 1;
        return 0;
    }
    // 续字节必须是 10xxxxxx
    for (int i = 1; i < n; ++i) {
        if ((static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) {
            *len = 1;
            return 0;
        }
    }
    *len = n;
    switch (n) {
        case 1: return c0;
        case 2: return ((c0 & 0x1Fu) << 6) |
                       (static_cast<unsigned char>(p[1]) & 0x3Fu);
        case 3: return ((c0 & 0x0Fu) << 12) |
                       ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 6) |
                       (static_cast<unsigned char>(p[2]) & 0x3Fu);
        case 4: return ((c0 & 0x07u) << 18) |
                       ((static_cast<unsigned char>(p[1]) & 0x3Fu) << 12) |
                       ((static_cast<unsigned char>(p[2]) & 0x3Fu) << 6) |
                       (static_cast<unsigned char>(p[3]) & 0x3Fu);
        default: return 0;
    }
}

/// 零宽 / 方向控制 / 其他不可见字符
bool is_invisible_codepoint(uint32_t cp) {
    if (cp == 0x00AD) return true;                    // soft hyphen
    if (cp >= 0x200B && cp <= 0x200F) return true;    // ZWSP..RLM
    if (cp >= 0x202A && cp <= 0x202E) return true;    // 方向覆盖
    if (cp >= 0x2060 && cp <= 0x2064) return true;    // word joiner 等
    if (cp >= 0x2066 && cp <= 0x2069) return true;    // 方向隔离
    if (cp == 0xFEFF) return true;                    // BOM
    if (cp >= 0xFFF9 && cp <= 0xFFFB) return true;    // 注释标记
    if (cp >= 0xE0000 && cp <= 0xE007F) return true;  // tag 字符
    return false;
}

/// 剥离控制字符（保留 \n）与不可见字符；非法字节直接丢弃
std::string strip_control_and_invisible(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        int len = 1;
        const uint32_t cp = utf8_decode(s.data() + i, s.size() - i, &len);
        if (cp == 0 && len == 1) {
            ++i;  // 非法字节：丢弃
            continue;
        }
        // ASCII 控制字符：只留 \n，\t 与 \r 转空格
        if (cp < 0x20) {
            if (cp == '\n') {
                out.push_back('\n');
            } else if (cp == '\t' || cp == '\r') {
                out.push_back(' ');
            }
            i += len;
            continue;
        }
        if (cp == 0x7F) {  // DEL
            i += len;
            continue;
        }
        if (is_invisible_codepoint(cp)) {
            i += len;
            continue;
        }
        out.append(s.data() + i, static_cast<size_t>(len));
        i += len;
    }
    return out;
}

/// 剥离 markdown 代码围栏行（``` 或 ~~~ 开头的行）
std::string strip_code_fences(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t eol = s.find('\n', pos);
        const bool last = (eol == std::string::npos);
        const std::string line = s.substr(pos, last ? std::string::npos : eol - pos);
        const std::string t = trim_copy(line);
        const bool is_fence = (t.rfind("```", 0) == 0) || (t.rfind("~~~", 0) == 0);
        if (!is_fence) {
            if (!out.empty()) out.push_back('\n');
            out += line;
        }
        if (last) break;
        pos = eol + 1;
    }
    return out;
}

/// 命中注入模式 → 该处到行尾替换为 "[已过滤]"
std::string filter_injections(std::string s) {
    static const char kMask[] = "[已过滤]";
    bool changed = true;
    int guard = 0;
    // 循环直到无新命中（替换后可能暴露新的命中位置），加轮数上限防病态输入
    while (changed && guard++ < 16) {
        changed = false;
        const std::string lower = to_lower_copy(s);
        for (const char* pat : kInjectionPatterns) {
            const size_t pos = lower.find(pat);
            if (pos == std::string::npos) continue;
            const size_t eol = s.find('\n', pos);
            const size_t len = (eol == std::string::npos ? s.size() : eol) - pos;
            s.replace(pos, len, kMask);
            changed = true;
            break;  // 重新计算 lower
        }
    }
    return s;
}

/// 折叠连续空白：多个空格/制表 → 单空格；3+ 换行 → 2 换行
std::string collapse_whitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int nl_run = 0;
    bool sp_run = false;
    for (char c : s) {
        if (c == '\n') {
            ++nl_run;
            sp_run = false;
            if (nl_run <= 2) out.push_back('\n');
            continue;
        }
        nl_run = 0;
        if (c == ' ') {
            if (!sp_run) out.push_back(' ');
            sp_run = true;
            continue;
        }
        sp_run = false;
        out.push_back(c);
    }
    return out;
}

}  // namespace

// ── UTF-8 工具 ──────────────────────────────────────────────────────────────

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        int len = 1;
        const uint32_t cp = utf8_decode(s.data() + i, s.size() - i, &len);
        if (cp == 0 && len == 1 && static_cast<unsigned char>(s[i]) != 0) {
            return false;
        }
        i += static_cast<size_t>(len);
    }
    return true;
}

std::string utf8_truncate(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);

    size_t i = 0;
    size_t last_good = 0;
    while (i < s.size()) {
        int len = 1;
        utf8_decode(s.data() + i, s.size() - i, &len);
        if (i + static_cast<size_t>(len) > max_bytes) break;
        i += static_cast<size_t>(len);
        last_good = i;
    }
    return std::string(s.substr(0, last_good));
}

// ── 主消杀 ──────────────────────────────────────────────────────────────────

std::string sanitize_card_text(std::string_view raw, size_t max_len) {
    if (raw.empty() || max_len == 0) return {};

    std::string s = strip_control_and_invisible(raw);
    s = strip_code_fences(s);
    s = filter_injections(std::move(s));
    s = collapse_whitespace(s);
    s = trim_copy(s);

    if (s.size() > max_len) {
        // ★ UTF-8 安全截断，不切半个字符
        s = utf8_truncate(s, max_len);
        s = trim_copy(s);
    }
    return s;
}

AgentCard sanitize_agent_card(const AgentCard& raw, const SanitizeLimits& lim) {
    AgentCard c;

    // 文本字段：消杀
    c.name = sanitize_card_text(raw.name, lim.max_name);
    c.description = sanitize_card_text(raw.description, lim.max_description);
    c.version = sanitize_card_text(raw.version, lim.max_version);
    c.protocol_version = sanitize_card_text(raw.protocol_version, lim.max_version);

    // URL / capabilities / security：原样保留
    //   URL 由 validate_card_rpc_targets 负责；security 不进 prompt
    c.url = raw.url;
    c.interfaces = raw.interfaces;
    c.capabilities = raw.capabilities;
    c.security_schemes = raw.security_schemes;
    c.security = raw.security;

    // modes：短枚举串，只做长度截断
    for (const auto& m : raw.default_input_modes) {
        c.default_input_modes.push_back(sanitize_card_text(m, lim.max_tag_len));
    }
    for (const auto& m : raw.default_output_modes) {
        c.default_output_modes.push_back(sanitize_card_text(m, lim.max_tag_len));
    }

    // skills：★ 条数上限 + 逐字段消杀
    const size_t n = std::min(raw.skills.size(), lim.max_skills);
    for (size_t i = 0; i < n; ++i) {
        const AgentSkill& rs = raw.skills[i];
        AgentSkill s;
        s.id = sanitize_card_text(rs.id, lim.max_skill_id);
        s.name = sanitize_card_text(rs.name, lim.max_skill_name);
        s.description = sanitize_card_text(rs.description, lim.max_skill_description);

        const size_t tn = std::min(rs.tags.size(), lim.max_tags_per_skill);
        for (size_t k = 0; k < tn; ++k) {
            s.tags.push_back(sanitize_card_text(rs.tags[k], lim.max_tag_len));
        }
        // examples 不进 prompt（只在 a2a_list_agents 里可能展示），仍做截断
        const size_t en = std::min<size_t>(rs.examples.size(), 3);
        for (size_t k = 0; k < en; ++k) {
            s.examples.push_back(sanitize_card_text(rs.examples[k],
                                                    lim.max_skill_description));
        }
        s.input_modes = rs.input_modes;
        s.output_modes = rs.output_modes;
        c.skills.push_back(std::move(s));
    }
    return c;
}

}  // namespace remote_a2a
}  // namespace hunterclaw
