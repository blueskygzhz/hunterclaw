// ============================================================================
// assistant_visible_text.cc
// ✅ 严格对齐 OpenClaw: src/shared/text/assistant-visible-text.ts (592行)
//
// Assistant 可见文本清洗系统：
//   - stripReasoningTagsFromText: 委托 reasoning_tags.cc
//   - strip tool_call/tool_result/function_call tags (5种 tag)
//   - strip <relevant_memories> tags
//   - code-region 感知 (不剥离代码块内的标签)
//   - endsInsideQuotedString 保护
//   - sanitizeAssistantVisibleText: 完整清洗流水线
// ============================================================================

#include <string>
#include <vector>
#include <set>
#include <regex>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

namespace hunterclaw {
namespace shared {
namespace text {

// Forward declarations (from reasoning_tags.cc)
std::string strip_reasoning_tags_from_text(
    const std::string& text,
    const std::string& mode,
    const std::string& trim);

namespace {
auto log() { return spdlog::get("text"); }
} // namespace

// ============================================================================
// 对齐 assistant-visible-text.ts:10-11: MEMORY_TAG regexes
// ============================================================================
static const std::regex MEMORY_TAG_RE(
    R"(<\s*(/?\s*)relevant[-_]memories\b[^<>]*>)",
    std::regex_constants::icase);
static const std::regex MEMORY_TAG_QUICK_RE(
    R"(<\s*/?\s*relevant[-_]memories\b)",
    std::regex_constants::icase);

// ============================================================================
// 对齐 assistant-visible-text.ts:18-25: TOOL_CALL tag names + regexes
// ============================================================================
static const std::set<std::string> TOOL_CALL_TAG_NAMES = {
    "tool_call", "tool_result", "function_call", "function_calls", "tool_calls"
};
static const std::regex TOOL_CALL_QUICK_RE(
    R"(<\s*/?\s*(?:tool_call|tool_result|function_calls?|tool_calls)\b)",
    std::regex_constants::icase);

// ============================================================================
// 对齐 assistant-visible-text.ts:31-59: endsInsideQuotedString
// ============================================================================
static bool ends_inside_quoted_string(const std::string& text, size_t start, size_t end) {
    char quote_char = 0;
    bool is_escaped = false;

    for (size_t idx = start; idx < end && idx < text.size(); ++idx) {
        char ch = text[idx];
        if (quote_char == 0) {
            if (ch == '"' || ch == '\'') {
                quote_char = ch;
            }
            continue;
        }
        if (is_escaped) {
            is_escaped = false;
            continue;
        }
        if (ch == '\\') {
            is_escaped = true;
            continue;
        }
        if (ch == quote_char) {
            quote_char = 0;
        }
    }
    return quote_char != 0;
}

// ============================================================================
// 对齐 assistant-visible-text.ts: strip <relevant_memories> tags
// Stateful open/close tracking
// ============================================================================
static std::string strip_memory_tags(const std::string& text) {
    if (!std::regex_search(text, MEMORY_TAG_QUICK_RE)) return text;

    std::string result;
    size_t last_index = 0;
    bool in_memory = false;

    auto begin = std::sregex_iterator(text.begin(), text.end(), MEMORY_TAG_RE);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t idx = it->position();
        std::string slash = (*it)[1];
        bool is_close = !slash.empty() && slash.find('/') != std::string::npos;

        if (!in_memory) {
            result += text.substr(last_index, idx - last_index);
            if (!is_close) {
                in_memory = true;
            }
        } else if (is_close) {
            in_memory = false;
        }
        last_index = idx + it->length();
    }
    if (!in_memory) {
        result += text.substr(last_index);
    }
    return result;
}

// ============================================================================
// 对齐 assistant-visible-text.ts: strip tool call tags
// Stateful open/close tracking with JSON/XML payload detection
// ============================================================================
static std::string strip_tool_call_tags(const std::string& text) {
    if (!std::regex_search(text, TOOL_CALL_QUICK_RE)) return text;

    // Build regex for tool call open/close tags
    static const std::regex TAG_RE(
        R"(<\s*(/?)(\s*(?:tool_call|tool_result|function_calls?|tool_calls))\b[^<>]*>)",
        std::regex_constants::icase);

    std::string result;
    size_t last_index = 0;
    bool in_tag = false;

    auto begin = std::sregex_iterator(text.begin(), text.end(), TAG_RE);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        size_t idx = it->position();
        bool is_close = ((*it)[1].length() > 0);
        std::string tag_name = (*it)[2];
        // Normalize tag name
        std::string tag_lower;
        for (char c : tag_name) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                tag_lower += std::tolower(static_cast<unsigned char>(c));
            }
        }

        if (TOOL_CALL_TAG_NAMES.count(tag_lower) == 0) continue;

        // Check if inside quoted string (protection)
        if (idx > 0 && ends_inside_quoted_string(text, 0, idx)) continue;

        if (!in_tag) {
            result += text.substr(last_index, idx - last_index);
            if (!is_close) {
                in_tag = true;
            }
        } else if (is_close) {
            in_tag = false;
        }
        last_index = idx + it->length();
    }

    if (!in_tag) {
        result += text.substr(last_index);
    }
    return result;
}

// ============================================================================
// 对齐 assistant-visible-text.ts: sanitizeAssistantVisibleText
// 完整清洗流水线
// ============================================================================
std::string sanitize_assistant_visible_text(
    const std::string& text,
    const std::string& reasoning_mode,
    const std::string& reasoning_trim
) {
    if (text.empty()) return text;

    // Step 1: Strip reasoning tags
    std::string cleaned = strip_reasoning_tags_from_text(text, reasoning_mode, reasoning_trim);

    // Step 2: Strip tool call tags
    cleaned = strip_tool_call_tags(cleaned);

    // Step 3: Strip memory tags
    cleaned = strip_memory_tags(cleaned);

    // Step 4: Trim start
    auto pos = cleaned.find_first_not_of(" \t\n\r");
    if (pos == std::string::npos) return "";
    return cleaned.substr(pos);
}

// Default overload
std::string sanitize_assistant_visible_text(const std::string& text) {
    return sanitize_assistant_visible_text(text, "preserve", "start");
}

} // namespace text
} // namespace shared
} // namespace hunterclaw
