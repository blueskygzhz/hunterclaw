// 🎯 严格对齐 OpenClaw: src/logging/secret-redaction-registry.ts
// 进程内"精确值"脱敏注册表实现。

#include "hunterclaw/logging/secret_redaction_registry.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunterclaw {
namespace logging {

namespace {

// ============================================================================
// 有界注册表 (LRU 式淘汰) + 首字符集合 + 合成正则缓存
// ============================================================================
std::mutex g_mutex;

// LRU: list 保序(front=最旧, back=最新), map 提供 O(1) 命中
std::list<std::string> g_order;
std::unordered_map<std::string, std::list<std::string>::iterator> g_index;

// 首字符集合 (快速探测: text 不含任何首字符则跳过正则构造)
std::set<char> g_first_chars;

// 合成正则缓存 (脏标记: 注册变化后重建)
bool g_matcher_dirty = true;
std::regex g_matcher;
bool g_matcher_valid = false;

// 🎯 对齐 OpenClaw escapeRegExp
std::string escape_regexp(const std::string& s) {
    static const std::string kSpecial = R"(.*+?^${}()|[]\)";
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        if (kSpecial.find(c) != std::string::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// 🎯 对齐 encodeURIComponent (仅编码需要转义的字节，保留 RFC3986 unreserved)
std::string percent_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '!' || c == '~' || c == '*' ||
                          c == '\'' || c == '(' || c == ')';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// 🎯 对齐 JSON.stringify(value).slice(1,-1):仅做 JSON 字符串体的转义 (不含外层引号)
std::string json_escape_body(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

// 单个精确值登记 (调用方持有锁)
void register_one_locked(const std::string& value) {
    if (value.size() < SECRET_MIN_VALUE_LENGTH) return;

    auto it = g_index.find(value);
    if (it != g_index.end()) {
        // 已存在 → 移到末尾 (标记最新)
        g_order.splice(g_order.end(), g_order, it->second);
        return;
    }

    // 有界淘汰: 超出上限则从最旧端移除
    while (g_order.size() >= SECRET_MAX_VALUES && !g_order.empty()) {
        const std::string& oldest = g_order.front();
        g_index.erase(oldest);
        g_order.pop_front();
    }

    g_order.push_back(value);
    g_index[value] = std::prev(g_order.end());
    g_first_chars.insert(value.front());
    g_matcher_dirty = true;
}

// 重建合成正则 (长度降序, 保证长值优先匹配)
void rebuild_matcher_locked() {
    g_matcher_dirty = false;
    g_matcher_valid = false;
    if (g_order.empty()) return;

    std::vector<std::string> values(g_order.begin(), g_order.end());
    std::sort(values.begin(), values.end(),
              [](const std::string& a, const std::string& b) {
                  return a.size() > b.size();  // 长度降序
              });

    std::string pattern;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) pattern += "|";
        pattern += escape_regexp(values[i]);
    }
    try {
        g_matcher = std::regex(pattern);
        g_matcher_valid = true;
    } catch (...) {
        g_matcher_valid = false;
    }
}

}  // namespace

void register_secret_value_for_redaction(const std::string& value) {
    if (value.size() < SECRET_MIN_VALUE_LENGTH) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    // 🎯 对齐 OpenClaw: 先登记变体形态, 原始值最后登记
    const std::string encoded = percent_encode(value);
    if (encoded != value) register_one_locked(encoded);
    const std::string escaped = json_escape_body(value);
    if (escaped != value) register_one_locked(escaped);
    register_one_locked(value);
}

bool is_secret_value_registered_for_redaction(const std::string& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_index.find(value) != g_index.end();
}

bool has_registered_secret_values_for_redaction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_order.empty();
}

std::string redact_registered_secret_values(
    const std::string& text,
    const std::function<std::string(const std::string&)>& mask
) {
    if (text.empty()) return text;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_order.empty()) return text;

    // 🎯 首字符快速探测: 无任何已注册值的首字符出现则直接返回
    bool possible = false;
    for (char c : g_first_chars) {
        if (text.find(c) != std::string::npos) { possible = true; break; }
    }
    if (!possible) return text;

    if (g_matcher_dirty) rebuild_matcher_locked();
    if (!g_matcher_valid) return text;

    std::string result;
    auto begin = std::sregex_iterator(text.begin(), text.end(), g_matcher);
    auto end = std::sregex_iterator();
    size_t last_pos = 0;
    for (auto it = begin; it != end; ++it) {
        result += text.substr(last_pos, it->position() - last_pos);
        result += mask(it->str(0));
        last_pos = it->position() + it->length();
    }
    result += text.substr(last_pos);
    return result;
}

void clear_registered_secret_values_for_redaction() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_order.clear();
    g_index.clear();
    g_first_chars.clear();
    g_matcher_dirty = true;
    g_matcher_valid = false;
}

}  // namespace logging
}  // namespace hunterclaw
