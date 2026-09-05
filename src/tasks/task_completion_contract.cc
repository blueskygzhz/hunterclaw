// 严格对齐 OpenClaw: src/tasks/task-completion-contract.ts

#include "hunterclaw/tasks/task_completion_contract.h"

#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace tasks {

namespace {

// 对齐 OC: PROGRESS_ONLY_PATTERN
// 注意 OC 用 (?:'|\u2019) 同时匹配 ASCII 撇号与右单引号 U+2019（UTF-8: \xE2\x80\x99）
const char* kProgressOnlyPatternSrc =
    "^(?:i(?:'|\xE2\x80\x99)ll|i will|i(?:'|\xE2\x80\x99)m|i am|"
    "i(?:'|\xE2\x80\x99)m going to|i am going to|let me|i need to)"
    "\\s+(?:now\\s+)?"
    "(?:analyz(?:e|ing)|apply|check(?:ing)?|continue|debug(?:ging)?|"
    "follow(?:ing)?\\s+up|inspect(?:ing)?|investigat(?:e|ing)|"
    "look(?:ing)?(?:\\s+into)?|map(?:ping)?|open(?:ing)?|read(?:ing)?|"
    "report(?:ing)?(?:\\s+back)?|review(?:ing)?|run(?:ning)?|start(?:ing)?|"
    "test(?:ing)?|trace|trac(?:e|ing)|try(?:ing)?|update|verify(?:ing)?|work(?:ing)?)";

// 对齐 OC: BARE_PROGRESS_ONLY_PATTERN
const char* kBareProgressOnlyPatternSrc =
    "^(?:analyz(?:e|ing)|check(?:ing)?|debug(?:ging)?|inspect(?:ing)?|"
    "investigat(?:e|ing)|look(?:ing)?\\s+into|map(?:ping)?|read(?:ing)?|"
    "report(?:ing)?\\s+back|review(?:ing)?|run(?:ning)?|test(?:ing)?|"
    "trac(?:e|ing)|verify(?:ing)?|work(?:ing)?\\s+on)\\b";

// 对齐 OC: FOLLOW_UP_PLANNING_PREFIX_PATTERN
const char* kFollowUpPrefixPatternSrc =
    "^(?:after(?:wards|\\s+that)?|from\\s+there|next|"
    "once\\s+(?:done|that(?:'|\xE2\x80\x99)?s\\s+done|that\\s+is\\s+done)|then)"
    "[,.\\s]+";

const std::regex& progress_only_regex() {
    static const std::regex re(kProgressOnlyPatternSrc,
                               std::regex::icase | std::regex::optimize);
    return re;
}

const std::regex& bare_progress_only_regex() {
    static const std::regex re(kBareProgressOnlyPatternSrc,
                               std::regex::icase | std::regex::optimize);
    return re;
}

const std::regex& follow_up_prefix_regex() {
    static const std::regex re(kFollowUpPrefixPatternSrc,
                               std::regex::icase | std::regex::optimize);
    return re;
}

std::string trim_copy(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && static_cast<unsigned char>(s[begin]) <= 0x20) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && static_cast<unsigned char>(s[end - 1]) <= 0x20) {
        --end;
    }
    return s.substr(begin, end - begin);
}

/// 对齐 OC: matchesProgressOnlyPrefix
bool matches_progress_only_prefix(const std::string& value) {
    if (std::regex_search(value, progress_only_regex()) ||
        std::regex_search(value, bare_progress_only_regex())) {
        return true;
    }
    // OC: value.replace(FOLLOW_UP_PLANNING_PREFIX_PATTERN, "").trim()
    std::string followup =
        trim_copy(std::regex_replace(value, follow_up_prefix_regex(), std::string()));
    if (followup == value) {
        return false;
    }
    return std::regex_search(followup, progress_only_regex()) ||
           std::regex_search(followup, bare_progress_only_regex());
}

/// 对齐 OC: hasNonProgressFollowupSentence
/// 与 is_progress_only_completion_text 互相递归，故此处前置声明
bool has_non_progress_followup_sentence(const std::string& value);

/// 对齐 OC: isProgressOnlyCompletionText（内部版本，输入已归一化）
bool is_progress_only_normalized(const std::string& normalized) {
    if (normalized.empty()) {
        return false;
    }
    if (has_non_progress_followup_sentence(normalized)) {
        return false;
    }
    return matches_progress_only_prefix(normalized);
}

bool has_non_progress_followup_sentence(const std::string& value) {
    // 对齐 OC: /(?:[.!?:]|\s[-\u2013\u2014])\s+\S/
    // \u2013 = EN DASH (UTF-8 \xE2\x80\x93), \u2014 = EM DASH (UTF-8 \xE2\x80\x94)
    static const std::regex boundary_re(
        "(?:[.!?:]|\\s(?:-|\xE2\x80\x93|\xE2\x80\x94))\\s+[^[:space:]]",
        std::regex::optimize);
    std::smatch m;
    if (!std::regex_search(value, m, boundary_re)) {
        return false;
    }
    // OC: separatorEnd = boundary.index + boundary[0].length - 1
    // boundary[0] 末字符是 \S，separatorEnd 指向该 \S 的起始位置
    const size_t match_pos = static_cast<size_t>(m.position(0));
    const size_t match_len = static_cast<size_t>(m.length(0));
    if (match_len == 0) {
        return false;
    }
    // \S 可能是多字节 UTF-8 首字节；OC 按 UTF-16 单元算，此处按字节回退 1
    const size_t separator_end = match_pos + match_len - 1;
    const std::string first_sentence = trim_copy(value.substr(0, separator_end));
    const std::string rest = trim_copy(value.substr(separator_end));
    return matches_progress_only_prefix(first_sentence) &&
           !is_progress_only_normalized(normalize_completion_text(rest));
}

} // namespace

std::string normalize_completion_text(const std::string& value) {
    // 对齐 OC: value.replace(/\s+/g, " ").trim()
    std::string collapsed;
    collapsed.reserve(value.size());
    bool in_space = false;
    for (char c : value) {
        const bool is_space = static_cast<unsigned char>(c) <= 0x20;
        if (is_space) {
            if (!in_space) {
                collapsed.push_back(' ');
                in_space = true;
            }
        } else {
            collapsed.push_back(c);
            in_space = false;
        }
    }
    return trim_copy(collapsed);
}

std::string truncate_utf8_safe(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }
    size_t cut = max_bytes;
    // 回退到 UTF-8 字符边界：continuation byte 形如 10xxxxxx
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return value.substr(0, cut);
}

bool is_progress_only_completion_text(const std::string& value) {
    return is_progress_only_normalized(normalize_completion_text(value));
}

RequiredCompletionTerminalResult resolve_required_completion_terminal_result(
    const std::string& result_text) {
    const std::string normalized = normalize_completion_text(result_text);
    if (normalized.empty()) {
        return RequiredCompletionTerminalResult{
            TaskTerminalOutcome::BLOCKED,
            "Required completion did not produce a final deliverable."};
    }
    if (is_progress_only_normalized(normalized)) {
        return RequiredCompletionTerminalResult{
            TaskTerminalOutcome::BLOCKED,
            "Required completion ended with progress-only text, not a final deliverable."};
    }
    return RequiredCompletionTerminalResult{};
}

} // namespace tasks
} // namespace hunterclaw
