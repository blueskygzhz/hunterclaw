// ============================================================================
// system_events.cc
// Aligned with openclaw: src/infra/system-events.ts
// ============================================================================

#include "hunterclaw/infra/system_events.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <algorithm>

namespace hunterclaw {
namespace infra {

// ── Module-level state ───────────────────────────────────────────────────────
// Aligned with openclaw: const queues = new Map<string, SessionQueue>()

static constexpr int MAX_EVENTS = 20;  // Aligned with openclaw: MAX_EVENTS = 20

struct SessionQueue {
    std::vector<SystemEvent> queue;
    std::optional<std::string> last_text;
    std::optional<std::string> last_context_key;
};

static std::mutex g_mutex;
static std::unordered_map<std::string, SessionQueue> g_queues;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string require_session_key(const std::string& key) {
    size_t s = key.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) {
        throw std::runtime_error("system events require a sessionKey");
    }
    size_t e = key.find_last_not_of(" \t\n\r");
    return key.substr(s, e - s + 1);
}

static std::optional<std::string> normalize_context_key(
    const std::optional<std::string>& key
) {
    if (!key.has_value() || key->empty()) return std::nullopt;
    std::string trimmed;
    size_t s = key->find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::nullopt;
    size_t e = key->find_last_not_of(" \t\n\r");
    trimmed = key->substr(s, e - s + 1);
    if (trimmed.empty()) return std::nullopt;
    // Aligned with openclaw: .toLowerCase()
    std::transform(trimmed.begin(), trimmed.end(), trimmed.begin(), ::tolower);
    return trimmed;
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ── enqueue_system_event ─────────────────────────────────────────────────────
// Aligned with openclaw: enqueueSystemEvent

bool enqueue_system_event(
    const std::string& text,
    const std::string& session_key,
    const std::optional<std::string>& context_key
) {
    const std::string key = require_session_key(session_key);

    // Trim text
    size_t s = text.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return false;  // Aligned: if (!cleaned) return false
    size_t e = text.find_last_not_of(" \t\n\r");
    const std::string cleaned = text.substr(s, e - s + 1);
    if (cleaned.empty()) return false;

    const auto normalized_ctx = normalize_context_key(context_key);

    std::lock_guard<std::mutex> lock(g_mutex);
    auto& entry = g_queues[key];

    entry.last_context_key = normalized_ctx;

    // Aligned with openclaw: skip consecutive duplicates
    if (entry.last_text.has_value() && entry.last_text.value() == cleaned) {
        return false;
    }
    entry.last_text = cleaned;

    entry.queue.push_back({cleaned, now_ms(), normalized_ctx});

    // Aligned with openclaw: if (entry.queue.length > MAX_EVENTS) entry.queue.shift()
    if (static_cast<int>(entry.queue.size()) > MAX_EVENTS) {
        entry.queue.erase(entry.queue.begin());
    }
    return true;
}

// ── drain_system_event_entries ───────────────────────────────────────────────
// Aligned with openclaw: drainSystemEventEntries

std::vector<SystemEvent> drain_system_event_entries(const std::string& session_key) {
    const std::string key = require_session_key(session_key);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(key);
    if (it == g_queues.end() || it->second.queue.empty()) return {};
    std::vector<SystemEvent> out = std::move(it->second.queue);
    g_queues.erase(it);
    return out;
}

// ── drain_system_events ──────────────────────────────────────────────────────
// Aligned with openclaw: drainSystemEvents

std::vector<std::string> drain_system_events(const std::string& session_key) {
    auto entries = drain_system_event_entries(session_key);
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const auto& e : entries) result.push_back(e.text);
    return result;
}

// ── consume_selected_system_event_entries ────────────────────────────────────
// Aligned with openclaw: consumeSelectedSystemEventEntries (system-events.ts:268-290)

namespace {

// Aligned with openclaw: areSystemEventsEqual (system-events.ts:218-225)
// 真源还比 deliveryContext；HunterClaw 的 SystemEvent 无该字段，故只比三项。
bool are_system_events_equal(const SystemEvent& left, const SystemEvent& right) {
    if (left.text != right.text) return false;
    if (left.ts != right.ts) return false;
    const bool left_has = left.context_key.has_value();
    const bool right_has = right.context_key.has_value();
    if (left_has != right_has) return false;
    if (left_has && *left.context_key != *right.context_key) return false;
    return true;
}

// Aligned with openclaw: resetQueueState (system-events.ts:227-232)
// 队列空了要连 last_context_key 一起清掉并移除整个 entry，否则残留的
// last_text/last_context_key 会让后续同文本事件被误判为连续重复而丢弃。
// 调用方必须已持有 g_mutex。
void reset_queue_state_locked(const std::string& key) {
    auto it = g_queues.find(key);
    if (it == g_queues.end()) return;
    if (it->second.queue.empty()) {
        g_queues.erase(it);
    }
}

} // namespace

std::vector<SystemEvent> consume_selected_system_event_entries(
    const std::string& session_key,
    const std::vector<SystemEvent>& consumed_entries
) {
    const std::string key = require_session_key(session_key);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(key);
    if (it == g_queues.end() || it->second.queue.empty() || consumed_entries.empty()) {
        return {};
    }
    auto& queue = it->second.queue;
    std::vector<SystemEvent> removed;
    removed.reserve(consumed_entries.size());
    for (const auto& consumed : consumed_entries) {
        auto found = std::find_if(
            queue.begin(), queue.end(),
            [&consumed](const SystemEvent& e) { return are_system_events_equal(e, consumed); });
        if (found == queue.end()) continue;   // 已被别处消费/挤出，跳过
        removed.push_back(*found);
        queue.erase(found);
    }
    reset_queue_state_locked(key);
    return removed;
}

// ── peek_system_event_entries ────────────────────────────────────────────────
// Aligned with openclaw: peekSystemEventEntries

std::vector<SystemEvent> peek_system_event_entries(const std::string& session_key) {
    const std::string key = require_session_key(session_key);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(key);
    if (it == g_queues.end()) return {};
    // Return a copy
    return it->second.queue;
}

// ── peek_system_events ───────────────────────────────────────────────────────
// Aligned with openclaw: peekSystemEvents

std::vector<std::string> peek_system_events(const std::string& session_key) {
    auto entries = peek_system_event_entries(session_key);
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (const auto& e : entries) result.push_back(e.text);
    return result;
}

// ── has_system_events ────────────────────────────────────────────────────────
// Aligned with openclaw: hasSystemEvents

bool has_system_events(const std::string& session_key) {
    const std::string key = require_session_key(session_key);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(key);
    return it != g_queues.end() && !it->second.queue.empty();
}

// ── is_system_event_context_changed ─────────────────────────────────────────
// Aligned with openclaw: isSystemEventContextChanged

bool is_system_event_context_changed(
    const std::string& session_key,
    const std::optional<std::string>& context_key
) {
    const std::string key = require_session_key(session_key);
    const auto normalized = normalize_context_key(context_key);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_queues.find(key);
    const auto& existing_ctx = (it != g_queues.end())
        ? it->second.last_context_key
        : std::optional<std::string>{};
    return normalized != existing_ctx;
}

} // namespace infra
} // namespace hunterclaw
