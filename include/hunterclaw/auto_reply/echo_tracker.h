// Aligned with openclaw: src/web/auto-reply/monitor/echo.ts
#pragma once

#include <string>
#include <set>
#include <list>

namespace hunterclaw {
namespace auto_reply {

// ============================================================================
// EchoTracker
// Aligned with openclaw: createEchoTracker (echo.ts)
//
// Tracks recently sent messages to detect echo (self-reply) and prevent loops.
// Uses a bounded Set with LRU eviction (max_items, default 100).
// ============================================================================

class EchoTracker {
public:
    explicit EchoTracker(size_t max_items = 100);

    // Build a combined echo key (aligned with openclaw: buildCombinedKey)
    static std::string build_combined_key(
        const std::string& session_key,
        const std::string& combined_body
    );

    // Remember recently sent text (aligned with openclaw: rememberText)
    void remember_text(
        const std::string& text,
        const std::string& combined_body = "",
        const std::string& combined_body_session_key = ""
    );

    // Check if a key is in the echo set (aligned with openclaw: has)
    bool has(const std::string& key) const;

    // Forget a key (aligned with openclaw: forget)
    void forget(const std::string& key);

    // Current size of the echo set
    size_t size() const;

private:
    void insert(const std::string& key);

    size_t max_items_;
    std::set<std::string> set_;
    std::list<std::string> order_;  // insertion order for LRU eviction
};

} // namespace auto_reply
} // namespace hunterclaw
