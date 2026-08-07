// ============================================================================
// secret_mask.cc - Credential masking for logs/audit output
// 🎯 严格对齐 OpenClaw: src/security/secret-mask.ts maskApiKey()
// ============================================================================

#include "hunterclaw/security/secret_mask.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hunterclaw {
namespace security {

namespace {

// 🎯 对齐 OpenClaw stripControlCharacters():
// 移除控制字符 (U+0000..U+001F, U+007F..U+009F)。按 Unicode 码点处理。
// 返回按码点拆分的字符串序列 (每个元素是一个完整 UTF-8 码点), 便于按码点切片。
std::vector<std::string> strip_control_characters_to_codepoints(const std::string& value) {
    std::vector<std::string> codepoints;
    size_t i = 0;
    const size_t n = value.size();
    while (i < n) {
        unsigned char lead = static_cast<unsigned char>(value[i]);
        size_t len = 1;
        uint32_t cp = 0;
        if (lead < 0x80) { len = 1; cp = lead; }
        else if ((lead >> 5) == 0x6) { len = 2; cp = lead & 0x1F; }
        else if ((lead >> 4) == 0xE) { len = 3; cp = lead & 0x0F; }
        else if ((lead >> 3) == 0x1E) { len = 4; cp = lead & 0x07; }
        else { len = 1; cp = lead; }  // 非法前导字节: 当作单字节

        if (i + len > n) { len = 1; cp = lead; }  // 截断: 退化为单字节
        for (size_t k = 1; k < len; ++k) {
            unsigned char cont = static_cast<unsigned char>(value[i + k]);
            if ((cont >> 6) != 0x2) { len = 1; cp = lead; break; }  // 非续字节
            cp = (cp << 6) | (cont & 0x3F);
        }

        bool is_control = (cp <= 0x1F) || (cp >= 0x7F && cp <= 0x9F);
        if (!is_control) {
            codepoints.emplace_back(value.substr(i, len));
        }
        i += len;
    }
    return codepoints;
}

// trim 首尾空白 (对齐 JS String.prototype.trim 常见空白: 空格/制表/换行/回车/换页/垂直制表)
bool is_ws(const std::string& cp) {
    if (cp.size() != 1) return false;  // 多字节不视为普通空白
    char c = cp[0];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string join_range(const std::vector<std::string>& cps, size_t begin, size_t end) {
    std::string out;
    for (size_t i = begin; i < end && i < cps.size(); ++i) out += cps[i];
    return out;
}

}  // namespace

// 🎯 对齐 OpenClaw maskApiKey()
std::string mask_api_key(const std::string& value) {
    std::vector<std::string> cps = strip_control_characters_to_codepoints(value);

    // trim
    size_t start = 0, stop = cps.size();
    while (start < stop && is_ws(cps[start])) ++start;
    while (stop > start && is_ws(cps[stop - 1])) --stop;

    const size_t len = (stop > start) ? (stop - start) : 0;
    if (len == 0) {
        return "missing";
    }

    // 切片辅助 (码点边界)
    auto head = [&](size_t count) {
        return join_range(cps, start, start + count);
    };
    auto tail = [&](size_t count) {
        size_t from = (len >= count) ? (stop - count) : start;
        return join_range(cps, from, stop);
    };

    if (len <= 6) {
        return head(1) + "..." + tail(1);
    }
    if (len <= 16) {
        return head(2) + "..." + tail(2);
    }
    return head(8) + "..." + tail(8);
}

}  // namespace security
}  // namespace hunterclaw
