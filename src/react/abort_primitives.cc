#include "hunterclaw/react/abort_primitives.h"
#include <algorithm>
#include <regex>

namespace hunterclaw {
namespace react {

// ============================================================================
// 全局 abort memory (对齐 abort-primitives.ts:42-60 abortMemoryStore)
// ============================================================================

static std::mutex abort_memory_mutex;
static std::map<std::string, bool> abort_memory_store;

// ============================================================================
// isAbortTrigger (对齐 abort-primitives.ts:5-22)
// ============================================================================

// 对齐: ABORT_TRIGGER_RE = /^\/(?:abort|stop)$/i
static const std::regex ABORT_TRIGGER_RE(R"(^\/(?:abort|stop)$)", std::regex_constants::icase);

// 对齐: ABORT_BARE_RE = /^(?:stop|abort)$/i
static const std::regex ABORT_BARE_RE(R"(^(?:stop|abort)$)", std::regex_constants::icase);

bool is_abort_trigger(const std::string& text) {
    // 对齐: text.trim()
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);

    if (trimmed.empty()) return false;

    // 对齐: ABORT_TRIGGER_RE.test(trimmed) || ABORT_BARE_RE.test(trimmed)
    return std::regex_match(trimmed, ABORT_TRIGGER_RE) ||
           std::regex_match(trimmed, ABORT_BARE_RE);
}

// ============================================================================
// isAbortRequestText (对齐 abort-primitives.ts:24-40)
// ============================================================================

bool is_abort_request_text(const std::string& text) {
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);

    if (trimmed.empty()) return false;

    // 转小写
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // 对齐: /^\/(?:abort|stop)$/i or bare "stop"/"abort"
    return lower == "/abort" || lower == "/stop" ||
           lower == "stop" || lower == "abort";
}

// ============================================================================
// Abort Memory (对齐 abort-primitives.ts:42-80)
// ============================================================================

std::optional<bool> get_abort_memory(const std::string& key) {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    auto it = abort_memory_store.find(key);
    if (it == abort_memory_store.end()) return std::nullopt;
    return it->second;
}

void set_abort_memory(const std::string& key, bool aborted) {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    abort_memory_store[key] = aborted;
}

size_t get_abort_memory_size() {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    return abort_memory_store.size();
}

void reset_abort_memory() {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    abort_memory_store.clear();
}

}  // namespace react
}  // namespace hunterclaw
