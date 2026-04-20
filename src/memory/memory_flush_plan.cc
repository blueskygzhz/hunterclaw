// ============================================================================
// memory_flush_plan.cc — 严格对齐 OC extensions/memory-core/src/flush-plan.ts
// ============================================================================

#include "hunterclaw/memory/memory_flush_plan.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <sstream>

namespace hunterclaw {
namespace memory {

namespace {

// 对齐 OC: flush-plan.ts L13-23
constexpr const char* kMemoryFlushTargetHint =
    "Store durable memories only in memory/YYYY-MM-DD.md (create memory/ if needed).";
constexpr const char* kMemoryFlushAppendOnlyHint =
    "If memory/YYYY-MM-DD.md already exists, APPEND new content only and do not overwrite existing entries.";
constexpr const char* kMemoryFlushReadOnlyHint =
    "Treat workspace bootstrap/reference files such as MEMORY.md, DREAMS.md, SOUL.md, TOOLS.md, "
    "and AGENTS.md as read-only during this flush; never overwrite, replace, or edit them.";

// 对齐 OC: DEFAULT_MEMORY_FLUSH_PROMPT (L25-32)
std::string default_memory_flush_prompt() {
    std::ostringstream oss;
    oss << "Pre-compaction memory flush. "
        << kMemoryFlushTargetHint << " "
        << kMemoryFlushReadOnlyHint << " "
        << kMemoryFlushAppendOnlyHint << " "
        << "Do NOT create timestamped variant files (e.g., YYYY-MM-DD-HHMM.md); "
           "always use the canonical YYYY-MM-DD.md filename. "
        << "If nothing to store, reply with " << kSilentReplyToken << ".";
    return oss.str();
}

// 对齐 OC: DEFAULT_MEMORY_FLUSH_SYSTEM_PROMPT (L34-41)
std::string default_memory_flush_system_prompt() {
    std::ostringstream oss;
    oss << "Pre-compaction memory flush turn. "
        << "The session is near auto-compaction; capture durable memories to disk. "
        << kMemoryFlushTargetHint << " "
        << kMemoryFlushReadOnlyHint << " "
        << kMemoryFlushAppendOnlyHint << " "
        << "You may reply, but usually " << kSilentReplyToken << " is correct.";
    return oss.str();
}

// 对齐 OC: formatDateStampInTimezone (L43-57)，HC 直接用 localtime
std::string format_date_stamp(int64_t now_ms) {
    std::time_t t;
    if (now_ms > 0) {
        t = static_cast<std::time_t>(now_ms / 1000);
    } else {
        t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 对齐 OC: ensureNoReplyHint (L67-72)
std::string ensure_no_reply_hint(const std::string& text) {
    if (contains(text, kSilentReplyToken)) return text;
    std::ostringstream oss;
    oss << text << "\n\nIf no user-visible reply is needed, start with " << kSilentReplyToken << ".";
    return oss.str();
}

// 对齐 OC: ensureMemoryFlushSafetyHints (L74-82)
std::string ensure_safety_hints(const std::string& input) {
    std::string next = trim(input);
    const char* required[] = {
        kMemoryFlushTargetHint,
        kMemoryFlushAppendOnlyHint,
        kMemoryFlushReadOnlyHint,
    };
    for (auto* hint : required) {
        if (!contains(next, hint)) {
            if (next.empty()) next = hint;
            else next = next + "\n\n" + hint;
        }
    }
    return next;
}

// 对齐 OC: L118 timeLine（HC 简化用 localtime/UTC 组合串）
std::string format_time_line(int64_t now_ms) {
    std::time_t t;
    if (now_ms > 0) {
        t = static_cast<std::time_t>(now_ms / 1000);
    } else {
        t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }
    std::tm local_tm{}, utc_tm{};
    localtime_r(&t, &local_tm);
    gmtime_r(&t, &utc_tm);
    char local_buf[64], utc_buf[32];
    std::strftime(local_buf, sizeof(local_buf), "%A, %B %d, %Y - %H:%M", &local_tm);
    std::strftime(utc_buf, sizeof(utc_buf), "%Y-%m-%d %H:%M UTC", &utc_tm);
    return std::string("Current time: ") + local_buf + " / " + utc_buf;
}

// 对齐 OC: appendCurrentTimeLine (L84-93)
std::string append_time_line(const std::string& text, const std::string& time_line) {
    // trimEnd
    auto end = text.find_last_not_of(" \t\n\r");
    std::string trimmed = (end == std::string::npos) ? "" : text.substr(0, end + 1);
    if (trimmed.empty()) return time_line;
    if (contains(trimmed, "Current time:")) return trimmed;
    return trimmed + "\n" + time_line;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

} // anon

std::optional<MemoryFlushPlan> build_memory_flush_plan(
    const MemoryFlushPlanOverrides& overrides,
    int64_t now_ms
) {
    // 对齐 OC L105-107: enabled === false → null
    if (overrides.enabled.has_value() && *overrides.enabled == false) {
        return std::nullopt;
    }

    MemoryFlushPlan plan;
    plan.soft_threshold_tokens = overrides.soft_threshold_tokens.value_or(
        kDefaultMemoryFlushSoftThresholdTokens);
    plan.force_flush_transcript_bytes = overrides.force_flush_transcript_bytes.value_or(
        kDefaultMemoryFlushForceTranscriptBytes);
    plan.reserve_tokens_floor = overrides.reserve_tokens_floor.value_or(
        kDefaultMemoryFlushReserveTokensFloor);

    // 对齐 OC L118-120: dateStamp + relativePath
    std::string date_stamp = format_date_stamp(now_ms);
    plan.relative_path = "memory/" + date_stamp + ".md";

    // 对齐 OC L122-124: prompt = ensureNoReply(ensureSafety(custom || DEFAULT))
    std::string prompt_base = overrides.custom_prompt.has_value() &&
                              !trim(*overrides.custom_prompt).empty()
        ? trim(*overrides.custom_prompt)
        : default_memory_flush_prompt();
    prompt_base = ensure_no_reply_hint(ensure_safety_hints(prompt_base));

    std::string system_base = overrides.custom_system_prompt.has_value() &&
                              !trim(*overrides.custom_system_prompt).empty()
        ? trim(*overrides.custom_system_prompt)
        : default_memory_flush_system_prompt();
    system_base = ensure_no_reply_hint(ensure_safety_hints(system_base));

    // 对齐 OC L135: prompt.replaceAll("YYYY-MM-DD", dateStamp)
    prompt_base = replace_all(prompt_base, "YYYY-MM-DD", date_stamp);
    system_base = replace_all(system_base, "YYYY-MM-DD", date_stamp);

    // 对齐 OC L135: appendCurrentTimeLine
    std::string time_line = format_time_line(now_ms);
    plan.prompt = append_time_line(prompt_base, time_line);

    // 对齐 OC L136: systemPrompt 不 append time line
    plan.system_prompt = system_base;

    return plan;
}

} // namespace memory
} // namespace hunterclaw
