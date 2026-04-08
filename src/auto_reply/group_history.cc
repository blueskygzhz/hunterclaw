// Aligned with openclaw: src/auto-reply/reply/history.ts
// Group chat history management with LRU eviction and context building.

#include "hunterclaw/auto_reply/group_history.h"
#include <sstream>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// Constants (aligned with openclaw)
// ============================================================================

const std::string HISTORY_CONTEXT_MARKER =
    "[Chat messages since your last reply - for context]";
const std::string CURRENT_MESSAGE_MARKER =
    "[Current message - respond to this]";

// ============================================================================
// evict_old_history_keys
// Aligned with openclaw: evictOldHistoryKeys (history.ts)
//
// Uses map insertion order is NOT guaranteed in std::map, so we track
// access order separately via a list.
// ============================================================================

void evict_old_history_keys(
    std::map<std::string, std::vector<GroupHistoryEntry>>& history_map,
    std::vector<std::string>& key_order,
    size_t max_keys
) {
    while (history_map.size() > max_keys && !key_order.empty()) {
        const std::string& oldest = key_order.front();
        history_map.erase(oldest);
        key_order.erase(key_order.begin());
    }
}

// ============================================================================
// record_pending_history_entry
// Aligned with openclaw: recordPendingHistoryEntry / appendHistoryEntry (history.ts)
//
// Appends entry to the history for the given key. Evicts oldest entries
// when exceeding limit. Refreshes key access order for LRU.
// ============================================================================

void record_pending_history_entry(
    std::map<std::string, std::vector<GroupHistoryEntry>>& history_map,
    const std::string& history_key,
    const GroupHistoryEntry& entry,
    int limit,
    std::vector<std::string>* key_order
) {
    if (limit <= 0) return;

    auto& history = history_map[history_key];
    history.push_back(entry);

    // Evict oldest entries if exceeding limit
    // Aligned with openclaw: while (history.length > limit) { history.shift(); }
    while (static_cast<int>(history.size()) > limit) {
        history.erase(history.begin());
    }

    // Refresh key access order (aligned with openclaw: delete+set for insertion order)
    if (key_order) {
        auto it = std::find(key_order->begin(), key_order->end(), history_key);
        if (it != key_order->end()) {
            key_order->erase(it);
        }
        key_order->push_back(history_key);

        // Evict oldest keys if map exceeds MAX_HISTORY_KEYS
        evict_old_history_keys(history_map, *key_order, MAX_HISTORY_KEYS);
    }
}

// ============================================================================
// build_history_context
// Aligned with openclaw: buildHistoryContext (history.ts)
//
// Produces:
//   [Chat messages since your last reply - for context]
//   <history text>
//
//   [Current message - respond to this]
//   <current message>
// ============================================================================

std::string build_history_context(
    const std::string& history_text,
    const std::string& current_message,
    const std::string& line_break
) {
    if (history_text.empty()) {
        // No history — just return the current message
        return current_message;
    }

    return HISTORY_CONTEXT_MARKER + line_break +
           history_text + line_break +
           line_break +
           CURRENT_MESSAGE_MARKER + line_break +
           current_message;
}

// ============================================================================
// build_history_context_from_entries
// Aligned with openclaw: buildHistoryContextFromEntries (history.ts)
//
// Formats history entries using a format function and combines with current message.
// ============================================================================

std::string build_history_context_from_entries(
    const std::vector<GroupHistoryEntry>& entries,
    const std::string& current_message,
    std::function<std::string(const GroupHistoryEntry&)> format_entry,
    bool exclude_last
) {
    if (entries.empty()) return current_message;

    std::ostringstream history_text;
    size_t count = exclude_last && entries.size() > 0
        ? entries.size() - 1
        : entries.size();

    for (size_t i = 0; i < count; ++i) {
        if (i > 0) history_text << "\n";
        history_text << format_entry(entries[i]);
    }

    return build_history_context(history_text.str(), current_message);
}

} // namespace auto_reply
} // namespace hunterclaw
