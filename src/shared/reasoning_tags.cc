// ============================================================================
// reasoning_tags.cc
// ✅ 严格对齐 OpenClaw: src/shared/text/reasoning-tags.ts (94行)
//
// Reasoning tag 剥离系统：
//   - 多 tag 变体: thinking, antml:thinking, thought, antthinking, antml:think
//   - <final> tag 剥离
//   - Code-region 感知 (跳过代码块内的匹配)
//   - ReasoningTagMode: strict / preserve
//   - ReasoningTagTrim: none / start / both
//   - Stateful open/close 配对追踪 (处理截断流)
// ============================================================================

#include <string>
#include <vector>
#include <regex>
#include <algorithm>

namespace hunterclaw {
namespace shared {
namespace text {

// ============================================================================
// 对齐 reasoning-tags.ts:2-3: Types
// ============================================================================
enum class ReasoningTagMode { Strict, Preserve };
enum class ReasoningTagTrim { None, Start, Both };

// ============================================================================
// 对齐 reasoning-tags.ts:10-18: applyTrim
// ============================================================================
static std::string apply_trim(const std::string& value, ReasoningTagTrim mode) {
    if (mode == ReasoningTagTrim::None) return value;
    if (mode == ReasoningTagTrim::Start) {
        auto pos = value.find_first_not_of(" \t\n\r");
        return (pos == std::string::npos) ? "" : value.substr(pos);
    }
    // Both
    auto s = value.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return "";
    auto e = value.find_last_not_of(" \t\n\r");
    return value.substr(s, e - s + 1);
}

// ============================================================================
// Code region detection (simplified)
// 对齐 OC: findCodeRegions + isInsideCode
// ============================================================================
struct CodeRegion {
    size_t start;
    size_t end;
};

static std::vector<CodeRegion> find_code_regions(const std::string& text) {
    std::vector<CodeRegion> regions;
    // Match ``` code fences
    std::regex fence_re("```");
    auto begin = std::sregex_iterator(text.begin(), text.end(), fence_re);
    auto end = std::sregex_iterator();

    bool in_fence = false;
    size_t fence_start = 0;
    for (auto it = begin; it != end; ++it) {
        size_t pos = it->position();
        if (!in_fence) {
            fence_start = pos;
            in_fence = true;
        } else {
            regions.push_back({fence_start, pos + it->length()});
            in_fence = false;
        }
    }
    // Unclosed fence extends to end
    if (in_fence) {
        regions.push_back({fence_start, text.size()});
    }

    // Match inline code `...`
    std::regex inline_re("`[^`]+`");
    begin = std::sregex_iterator(text.begin(), text.end(), inline_re);
    for (auto it = begin; it != end; ++it) {
        regions.push_back({static_cast<size_t>(it->position()),
                          static_cast<size_t>(it->position() + it->length())});
    }
    return regions;
}

static bool is_inside_code(size_t pos, const std::vector<CodeRegion>& regions) {
    for (const auto& r : regions) {
        if (pos >= r.start && pos < r.end) return true;
    }
    return false;
}

// ============================================================================
// 对齐 reasoning-tags.ts:5-8: Tag regexes
// ============================================================================
// QUICK_TAG_RE: fast pre-check
static const std::regex QUICK_TAG_RE(
    R"(<\s*/?\s*(?:(?:antml:)?(?:think(?:ing)?|thought)|antthinking|final)\b)",
    std::regex_constants::icase);

// FINAL_TAG_RE: <final> and </final>
static const std::regex FINAL_TAG_RE(
    R"(<\s*/?\s*final\b[^<>]*>)",
    std::regex_constants::icase);

// THINKING_TAG_RE: all thinking variants with capture group for close slash
static const std::regex THINKING_TAG_RE(
    R"(<\s*(/?)\s*(?:(?:antml:)?(?:think(?:ing)?|thought)|antthinking)\b[^<>]*>)",
    std::regex_constants::icase);

// ============================================================================
// 对齐 reasoning-tags.ts:20-93: stripReasoningTagsFromText
// ============================================================================
std::string strip_reasoning_tags_from_text(
    const std::string& text,
    ReasoningTagMode mode,
    ReasoningTagTrim trim_mode
) {
    if (text.empty()) return text;

    // Quick pre-check
    if (!std::regex_search(text, QUICK_TAG_RE)) return text;

    std::string cleaned = text;

    // Pass 1: Strip <final> tags (code-region aware)
    {
        auto code_regions = find_code_regions(cleaned);
        std::vector<std::pair<size_t, size_t>> final_matches;
        auto begin = std::sregex_iterator(cleaned.begin(), cleaned.end(), FINAL_TAG_RE);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            size_t pos = it->position();
            if (!is_inside_code(pos, code_regions)) {
                final_matches.emplace_back(pos, it->length());
            }
        }
        // Remove in reverse order
        for (auto it = final_matches.rbegin(); it != final_matches.rend(); ++it) {
            cleaned.erase(it->first, it->second);
        }
    }

    // Pass 2: Strip thinking tags (stateful open/close tracking, code-region aware)
    auto code_regions = find_code_regions(cleaned);
    std::string result;
    size_t last_index = 0;
    bool in_thinking = false;

    auto begin = std::sregex_iterator(cleaned.begin(), cleaned.end(), THINKING_TAG_RE);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t idx = it->position();
        bool is_close = ((*it)[1].length() > 0);

        // Skip if inside code block
        if (is_inside_code(idx, code_regions)) continue;

        if (!in_thinking) {
            result += cleaned.substr(last_index, idx - last_index);
            if (!is_close) {
                in_thinking = true;
            }
        } else if (is_close) {
            in_thinking = false;
        }

        last_index = idx + it->length();
    }

    // Handle tail
    if (!in_thinking || mode == ReasoningTagMode::Preserve) {
        result += cleaned.substr(last_index);
    }

    return apply_trim(result, trim_mode);
}

// Convenience overloads matching OC defaults
std::string strip_reasoning_tags_from_text(const std::string& text) {
    return strip_reasoning_tags_from_text(text, ReasoningTagMode::Strict, ReasoningTagTrim::Both);
}

std::string strip_reasoning_tags_from_text(
    const std::string& text,
    const std::string& mode,
    const std::string& trim
) {
    ReasoningTagMode m = (mode == "preserve") ? ReasoningTagMode::Preserve : ReasoningTagMode::Strict;
    ReasoningTagTrim t = ReasoningTagTrim::Both;
    if (trim == "none") t = ReasoningTagTrim::None;
    else if (trim == "start") t = ReasoningTagTrim::Start;
    return strip_reasoning_tags_from_text(text, m, t);
}

} // namespace text
} // namespace shared
} // namespace hunterclaw
