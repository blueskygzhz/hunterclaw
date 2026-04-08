// Aligned with openclaw: src/web/auto-reply/monitor/echo.ts
// Echo detection — tracks recently sent messages to prevent self-reply loops.

#include "hunterclaw/auto_reply/echo_tracker.h"

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// EchoTracker Implementation
// Aligned with openclaw: createEchoTracker (echo.ts)
//
// Uses a bounded Set with LRU eviction (insertion-order via list + set).
// When the set exceeds max_items, the oldest entry is evicted.
// ============================================================================

EchoTracker::EchoTracker(size_t max_items)
    : max_items_(std::max<size_t>(1, max_items)) {}

// Aligned with openclaw: buildCombinedKey
std::string EchoTracker::build_combined_key(
    const std::string& session_key,
    const std::string& combined_body
) {
    return "combined:" + session_key + ":" + combined_body;
}

// Aligned with openclaw: rememberText
void EchoTracker::remember_text(
    const std::string& text,
    const std::string& combined_body,
    const std::string& combined_body_session_key
) {
    if (text.empty()) return;

    insert(text);

    if (!combined_body.empty() && !combined_body_session_key.empty()) {
        insert(build_combined_key(combined_body_session_key, combined_body));
    }
}

// Aligned with openclaw: has
bool EchoTracker::has(const std::string& key) const {
    return set_.count(key) > 0;
}

// Aligned with openclaw: forget
void EchoTracker::forget(const std::string& key) {
    auto it = set_.find(key);
    if (it == set_.end()) return;

    // Find and erase from order list
    for (auto lit = order_.begin(); lit != order_.end(); ++lit) {
        if (*lit == key) {
            order_.erase(lit);
            break;
        }
    }
    set_.erase(it);
}

size_t EchoTracker::size() const {
    return set_.size();
}

// ============================================================================
// Internal: insert with LRU eviction
// Aligned with openclaw: recentlySent.add(text) + trim()
// ============================================================================

void EchoTracker::insert(const std::string& key) {
    if (set_.count(key) > 0) {
        // Already present — refresh position (move to end)
        for (auto it = order_.begin(); it != order_.end(); ++it) {
            if (*it == key) {
                order_.erase(it);
                break;
            }
        }
        order_.push_back(key);
        return;
    }

    set_.insert(key);
    order_.push_back(key);

    // Trim: evict oldest entries (aligned with openclaw: while (size > maxItems))
    while (set_.size() > max_items_) {
        const std::string& oldest = order_.front();
        set_.erase(oldest);
        order_.pop_front();
    }
}

} // namespace auto_reply
} // namespace hunterclaw
