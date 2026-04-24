#include "hunterclaw/react/abort_primitives.h"
#include <algorithm>
#include <set>

namespace hunterclaw {
namespace react {

// ============================================================================
// 全局 abort memory (对齐 abort-primitives.ts:42-60 abortMemoryStore)
// ============================================================================

static std::mutex abort_memory_mutex;
// 对齐 OC: Map<string, boolean> — 用 ordered map 模拟 insertion-order LRU
static std::map<std::string, bool> abort_memory_store;
static std::vector<std::string> abort_memory_order;  // insertion order for pruning

// 对齐 OC: ABORT_MEMORY_MAX = 2000
static constexpr size_t ABORT_MEMORY_MAX = 2000;

// ============================================================================
// ABORT_TRIGGERS (对齐 abort-primitives.ts:5-40)
// OC 有 40+ 个多语言触发词，HC 严格对齐
// ============================================================================

static const std::set<std::string> ABORT_TRIGGERS = {
    "stop",
    "esc",
    "abort",
    "wait",
    "exit",
    "interrupt",
    "detente",
    "deten",
    "det\xC3\xA9n",       // détén
    "arrete",
    "arr\xC3\xAAte",      // arrête
    "\xE5\x81\x9C\xE6\xAD\xA2",  // 停止
    "\xE3\x82\x84\xE3\x82\x81\xE3\x81\xA6",  // やめて
    "\xE6\xAD\xA2\xE3\x82\x81\xE3\x81\xA6",  // 止めて
    "\xE0\xA4\xB0\xE0\xA5\x81\xE0\xA4\x95\xE0\xA5\x8B",  // रुको
    "\xD8\xAA\xD9\x88\xD9\x82\xD9\x81",  // توقف
    "\xD1\x81\xD1\x82\xD0\xBE\xD0\xBF",  // стоп
    "\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB8\xD1\x81\xD1\x8C",  // остановись
    "\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB8",  // останови
    "\xD0\xBE\xD1\x81\xD1\x82\xD0\xB0\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB8\xD1\x82\xD1\x8C",  // остановить
    "\xD0\xBF\xD1\x80\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB0\xD1\x82\xD0\xB8",  // прекрати
    "halt",
    "anhalten",
    "aufh\xC3\xB6ren",    // aufhören
    "hoer auf",
    "stopp",
    "pare",
    "stop openclaw",
    "openclaw stop",
    "stop action",
    "stop current action",
    "stop run",
    "stop current run",
    "stop agent",
    "stop the agent",
    "stop don't do anything",
    "stop dont do anything",
    "stop do not do anything",
    "stop doing anything",
    "do not do that",
    "please stop",
    "stop please",
};

// 对齐 OC: TRAILING_ABORT_PUNCTUATION_RE = /[.!?…,，。;；:：'"'")\]}]+$/u
static std::string strip_trailing_abort_punctuation(const std::string& s) {
    static const std::string punct = ".!?\xE2\x80\xA6,\xEF\xBC\x8C\xE3\x80\x82;;\xEF\xBC\x9A:'\"\xE2\x80\x98\xE2\x80\x99\xE2\x80\x9C\xE2\x80\x9D)\\]}";
    std::string result = s;
    while (!result.empty()) {
        // Check if last char is ASCII punctuation
        char c = result.back();
        if (c == '.' || c == '!' || c == '?' || c == ',' || c == ';' || c == ':' ||
            c == '\'' || c == '"' || c == ')' || c == ']' || c == '}') {
            result.pop_back();
        } else {
            break;
        }
    }
    return result;
}

// 对齐 OC: normalizeAbortTriggerText
static std::string normalize_abort_trigger_text(const std::string& text) {
    // normalizeLowercaseStringOrEmpty: trim + lowercase
    auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = text.find_last_not_of(" \t\n\r");
    std::string result = text.substr(start, end - start + 1);

    // lowercase
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);

    // replace ['`] with '
    for (auto& c : result) {
        if (c == '`') c = '\'';
    }

    // collapse whitespace
    std::string collapsed;
    bool last_space = false;
    for (char c : result) {
        if (c == ' ' || c == '\t') {
            if (!last_space && !collapsed.empty()) {
                collapsed.push_back(' ');
                last_space = true;
            }
        } else {
            collapsed.push_back(c);
            last_space = false;
        }
    }
    // trim trailing space
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();

    // strip trailing punctuation
    collapsed = strip_trailing_abort_punctuation(collapsed);

    // final trim
    auto s2 = collapsed.find_first_not_of(" \t\n\r");
    if (s2 == std::string::npos) return "";
    auto e2 = collapsed.find_last_not_of(" \t\n\r");
    return collapsed.substr(s2, e2 - s2 + 1);
}

// ============================================================================
// isAbortTrigger (对齐 abort-primitives.ts:50-56)
// OC: ABORT_TRIGGERS.has(normalizeAbortTriggerText(text))
// ============================================================================

bool is_abort_trigger(const std::string& text) {
    if (text.empty()) return false;
    auto normalized = normalize_abort_trigger_text(text);
    if (normalized.empty()) return false;
    return ABORT_TRIGGERS.count(normalized) > 0;
}

// ============================================================================
// isAbortRequestText (对齐 abort-primitives.ts:58-70)
// ============================================================================

bool is_abort_request_text(const std::string& text) {
    if (text.empty()) return false;

    // 对齐 OC: normalizeCommandBody(text, options).trim()
    std::string trimmed = text;
    auto s = trimmed.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;
    auto e = trimmed.find_last_not_of(" \t\n\r");
    trimmed = trimmed.substr(s, e - s + 1);
    if (trimmed.empty()) return false;

    // 对齐 OC: normalizeLowercaseStringOrEmpty
    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // 对齐 OC: normalizedLower === "/stop" || normalizeAbortTriggerText(normalizedLower) === "/stop" || isAbortTrigger(normalizedLower)
    if (lower == "/stop") return true;
    if (normalize_abort_trigger_text(lower) == "/stop") return true;
    return is_abort_trigger(lower);
}

// ============================================================================
// Abort Memory (对齐 abort-primitives.ts:72-120)
// ============================================================================

// 对齐 OC: pruneAbortMemory — 删除最旧的 excess 条目
static void prune_abort_memory_locked() {
    if (abort_memory_store.size() <= ABORT_MEMORY_MAX) return;
    size_t excess = abort_memory_store.size() - ABORT_MEMORY_MAX;
    size_t removed = 0;
    while (removed < excess && !abort_memory_order.empty()) {
        auto key = abort_memory_order.front();
        abort_memory_order.erase(abort_memory_order.begin());
        abort_memory_store.erase(key);
        removed++;
    }
}

std::optional<bool> get_abort_memory(const std::string& key) {
    // 对齐 OC: const normalized = key.trim()
    auto s = key.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::nullopt;
    auto e = key.find_last_not_of(" \t\n\r");
    std::string normalized = key.substr(s, e - s + 1);
    if (normalized.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    auto it = abort_memory_store.find(normalized);
    if (it == abort_memory_store.end()) return std::nullopt;
    return it->second;
}

void set_abort_memory(const std::string& key, bool aborted) {
    // 对齐 OC: const normalized = key.trim()
    auto s = key.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return;
    auto e = key.find_last_not_of(" \t\n\r");
    std::string normalized = key.substr(s, e - s + 1);
    if (normalized.empty()) return;

    std::lock_guard<std::mutex> lock(abort_memory_mutex);

    // 对齐 OC: if (!value) { ABORT_MEMORY.delete(normalized); return; }
    if (!aborted) {
        abort_memory_store.erase(normalized);
        auto it = std::find(abort_memory_order.begin(), abort_memory_order.end(), normalized);
        if (it != abort_memory_order.end()) abort_memory_order.erase(it);
        return;
    }

    // 对齐 OC: if (ABORT_MEMORY.has(normalized)) { ABORT_MEMORY.delete(normalized); }
    // (re-insert to update insertion order)
    if (abort_memory_store.count(normalized)) {
        abort_memory_store.erase(normalized);
        auto it = std::find(abort_memory_order.begin(), abort_memory_order.end(), normalized);
        if (it != abort_memory_order.end()) abort_memory_order.erase(it);
    }

    abort_memory_store[normalized] = true;
    abort_memory_order.push_back(normalized);

    // 对齐 OC: pruneAbortMemory()
    prune_abort_memory_locked();
}

size_t get_abort_memory_size() {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    return abort_memory_store.size();
}

void reset_abort_memory() {
    std::lock_guard<std::mutex> lock(abort_memory_mutex);
    abort_memory_store.clear();
    abort_memory_order.clear();
}

}  // namespace react
}  // namespace hunterclaw
