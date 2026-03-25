// Aligned with openclaw: src/channels/status-reactions.ts
#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>

namespace hunterclaw {
namespace channels {

// ============================================================================
// StatusReactions
// Aligned with OpenClaw: status-reactions.ts
// ============================================================================

// Aligned with OpenClaw: StatusReactionEmojis
struct StatusReactionEmojis {
    std::string queued    = "👀";
    std::string thinking  = "🤔";
    std::string tool      = "🔥";
    std::string coding    = "👨‍💻";
    std::string web       = "⚡";
    std::string done      = "👍";
    std::string error     = "😱";
    std::string stall_soft = "🥱";
    std::string stall_hard = "😨";
    std::string compacting = "✍";
};

// Aligned with OpenClaw: StatusReactionTiming
struct StatusReactionTiming {
    int debounce_ms   = 700;
    int stall_soft_ms = 10000;
    int stall_hard_ms = 30000;
    int done_hold_ms  = 1500;
    int error_hold_ms = 2500;
};

// Aligned with OpenClaw: StatusReactionAdapter
struct StatusReactionAdapter {
    std::function<void(const std::string& emoji)> set_reaction;
    std::function<void(const std::string& emoji)> remove_reaction;  // optional
};

// Aligned with OpenClaw: CODING_TOOL_TOKENS
inline const std::vector<std::string>& coding_tool_tokens() {
    static const std::vector<std::string> tokens = {
        "exec", "process", "read", "write", "edit", "session_status", "bash"
    };
    return tokens;
}

// Aligned with OpenClaw: WEB_TOOL_TOKENS
inline const std::vector<std::string>& web_tool_tokens() {
    static const std::vector<std::string> tokens = {
        "web_search", "web-search", "web_fetch", "web-fetch", "browser"
    };
    return tokens;
}

/**
 * Resolve the appropriate emoji for a tool invocation.
 * Aligned with OpenClaw: resolveToolEmoji()
 */
inline std::string resolve_tool_emoji(
    const std::string& tool_name,
    const StatusReactionEmojis& emojis)
{
    std::string normalized = tool_name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    size_t s = normalized.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return emojis.tool;
    size_t e = normalized.find_last_not_of(" \t\r\n");
    normalized = normalized.substr(s, e - s + 1);
    if (normalized.empty()) return emojis.tool;

    for (const auto& token : web_tool_tokens()) {
        if (normalized.find(token) != std::string::npos) return emojis.web;
    }
    for (const auto& token : coding_tool_tokens()) {
        if (normalized.find(token) != std::string::npos) return emojis.coding;
    }
    return emojis.tool;
}

// ── StatusReactionController ───────────────────────────────────────────────

/**
 * Status reaction controller.
 * Aligned with OpenClaw: createStatusReactionController()
 *
 * Features:
 * - Debouncing for intermediate states
 * - Immediate execution for terminal states (done/error)
 * - Stall timers (soft/hard warnings)
 * - Terminal state protection
 */
class StatusReactionController {
public:
    struct Params {
        bool enabled = true;
        StatusReactionAdapter adapter;
        std::string initial_emoji;
        StatusReactionEmojis emojis;
        StatusReactionTiming timing;
        std::function<void(const std::string&)> on_error;
    };

    explicit StatusReactionController(Params params);
    ~StatusReactionController();

    // Aligned with OpenClaw: setQueued()
    void set_queued();
    // Aligned with OpenClaw: setThinking()
    void set_thinking();
    // Aligned with OpenClaw: setTool()
    void set_tool(const std::string& tool_name = "");
    // Aligned with OpenClaw: setCompacting()
    void set_compacting();
    // Aligned with OpenClaw: cancelPending()
    void cancel_pending();
    // Aligned with OpenClaw: setDone()
    void set_done();
    // Aligned with OpenClaw: setError()
    void set_error();
    // Aligned with OpenClaw: clear()
    void clear();
    // Aligned with OpenClaw: restoreInitial()
    void restore_initial();

private:
    Params params_;
    std::string current_emoji_;
    std::string pending_emoji_;
    std::atomic<bool> finished_{false};
    std::mutex mutex_;

    // Debounce timer
    std::thread debounce_thread_;
    std::atomic<bool> debounce_active_{false};
    std::atomic<bool> debounce_cancel_{false};

    // Stall timers
    std::thread stall_soft_thread_;
    std::thread stall_hard_thread_;
    std::atomic<bool> stall_soft_active_{false};
    std::atomic<bool> stall_hard_active_{false};
    std::atomic<bool> stall_cancel_{false};

    void apply_emoji(const std::string& emoji);
    void schedule_emoji(const std::string& emoji, bool immediate = false, bool skip_stall_reset = false);
    void clear_debounce_timer();
    void reset_stall_timers();
    void clear_all_timers();
    void finish_with_emoji(const std::string& emoji);
};

} // namespace channels
} // namespace hunterclaw
