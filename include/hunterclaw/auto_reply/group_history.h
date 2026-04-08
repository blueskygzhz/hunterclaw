// Aligned with openclaw: src/auto-reply/reply/history.ts
#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>
#include <algorithm>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Constants (aligned with openclaw)
// ============================================================================

constexpr int DEFAULT_GROUP_HISTORY_LIMIT = 50;
constexpr size_t MAX_HISTORY_KEYS = 1000;

extern const std::string HISTORY_CONTEXT_MARKER;
extern const std::string CURRENT_MESSAGE_MARKER;

// ============================================================================
// GroupHistoryEntry (aligned with openclaw: HistoryEntry + GroupHistoryEntry)
// ============================================================================

struct GroupHistoryEntry {
    std::string sender;
    std::string body;
    int64_t timestamp = 0;
    std::string message_id;
    std::string sender_jid;
};

// ============================================================================
// History management functions
// ============================================================================

// Evict oldest keys when map exceeds max_keys (LRU)
void evict_old_history_keys(
    std::map<std::string, std::vector<GroupHistoryEntry>>& history_map,
    std::vector<std::string>& key_order,
    size_t max_keys = MAX_HISTORY_KEYS
);

// Append a history entry, evict if exceeding limit
void record_pending_history_entry(
    std::map<std::string, std::vector<GroupHistoryEntry>>& history_map,
    const std::string& history_key,
    const GroupHistoryEntry& entry,
    int limit = DEFAULT_GROUP_HISTORY_LIMIT,
    std::vector<std::string>* key_order = nullptr
);

// Build formatted history context string
std::string build_history_context(
    const std::string& history_text,
    const std::string& current_message,
    const std::string& line_break = "\n"
);

// Build history context from entries with custom formatter
std::string build_history_context_from_entries(
    const std::vector<GroupHistoryEntry>& entries,
    const std::string& current_message,
    std::function<std::string(const GroupHistoryEntry&)> format_entry,
    bool exclude_last = false
);

} // namespace auto_reply
} // namespace hunterclaw
