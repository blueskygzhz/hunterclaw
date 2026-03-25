// Aligned with openclaw: src/channels/status-reactions.ts
#include "hunterclaw/channels/status_reactions.h"
#include <algorithm>

namespace hunterclaw {
namespace channels {

StatusReactionController::StatusReactionController(Params params)
    : params_(std::move(params))
{
    // Set initial emoji as queued emoji if not overridden
    if (params_.emojis.queued.empty()) {
        params_.emojis.queued = params_.initial_emoji;
    }
}

StatusReactionController::~StatusReactionController() {
    clear_all_timers();
}

void StatusReactionController::apply_emoji(const std::string& emoji) {
    if (!params_.enabled) return;
    try {
        std::string previous = current_emoji_;
        if (params_.adapter.set_reaction) {
            params_.adapter.set_reaction(emoji);
        }
        // Remove previous if adapter supports it
        if (params_.adapter.remove_reaction && !previous.empty() && previous != emoji) {
            try { params_.adapter.remove_reaction(previous); } catch (...) {}
        }
        current_emoji_ = emoji;
    } catch (const std::exception& e) {
        if (params_.on_error) params_.on_error(e.what());
    }
}

void StatusReactionController::clear_debounce_timer() {
    debounce_cancel_ = true;
    debounce_active_ = false;
    if (debounce_thread_.joinable()) debounce_thread_.detach();
}

void StatusReactionController::reset_stall_timers() {
    stall_cancel_ = true;
    stall_soft_active_ = false;
    stall_hard_active_ = false;
    if (stall_soft_thread_.joinable()) stall_soft_thread_.detach();
    if (stall_hard_thread_.joinable()) stall_hard_thread_.detach();

    stall_cancel_ = false;
    stall_soft_active_ = true;
    stall_hard_active_ = true;

    stall_soft_thread_ = std::thread([this]() {
        for (int i = 0; i < params_.timing.stall_soft_ms / 50 && !stall_cancel_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stall_cancel_ && !finished_) {
            schedule_emoji(params_.emojis.stall_soft, true, true);
        }
        stall_soft_active_ = false;
    });
    stall_soft_thread_.detach();

    stall_hard_thread_ = std::thread([this]() {
        for (int i = 0; i < params_.timing.stall_hard_ms / 50 && !stall_cancel_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stall_cancel_ && !finished_) {
            schedule_emoji(params_.emojis.stall_hard, true, true);
        }
        stall_hard_active_ = false;
    });
    stall_hard_thread_.detach();
}

void StatusReactionController::clear_all_timers() {
    clear_debounce_timer();
    stall_cancel_ = true;
    stall_soft_active_ = false;
    stall_hard_active_ = false;
}

void StatusReactionController::schedule_emoji(
    const std::string& emoji,
    bool immediate,
    bool skip_stall_reset)
{
    if (!params_.enabled || finished_) return;

    // Deduplicate
    if (emoji == current_emoji_ || emoji == pending_emoji_) {
        if (!skip_stall_reset) reset_stall_timers();
        return;
    }

    pending_emoji_ = emoji;
    clear_debounce_timer();

    if (immediate) {
        apply_emoji(emoji);
        pending_emoji_.clear();
    } else {
        debounce_cancel_ = false;
        debounce_active_ = true;
        debounce_thread_ = std::thread([this, emoji]() {
            for (int i = 0; i < params_.timing.debounce_ms / 50 && !debounce_cancel_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!debounce_cancel_) {
                apply_emoji(emoji);
                pending_emoji_.clear();
            }
            debounce_active_ = false;
        });
        debounce_thread_.detach();
    }

    if (!skip_stall_reset) reset_stall_timers();
}

void StatusReactionController::finish_with_emoji(const std::string& emoji) {
    if (!params_.enabled) return;
    finished_ = true;
    clear_all_timers();
    apply_emoji(emoji);
    pending_emoji_.clear();
}

void StatusReactionController::set_queued() {
    schedule_emoji(params_.emojis.queued, true);
}

void StatusReactionController::set_thinking() {
    schedule_emoji(params_.emojis.thinking);
}

void StatusReactionController::set_tool(const std::string& tool_name) {
    schedule_emoji(resolve_tool_emoji(tool_name, params_.emojis));
}

void StatusReactionController::set_compacting() {
    schedule_emoji(params_.emojis.compacting);
}

void StatusReactionController::cancel_pending() {
    clear_debounce_timer();
    pending_emoji_.clear();
}

void StatusReactionController::set_done() {
    finish_with_emoji(params_.emojis.done);
}

void StatusReactionController::set_error() {
    finish_with_emoji(params_.emojis.error);
}

void StatusReactionController::clear() {
    if (!params_.enabled) return;
    clear_all_timers();
    finished_ = true;
    if (params_.adapter.remove_reaction) {
        // Remove all known emojis
        std::set<std::string> known = {
            params_.initial_emoji,
            params_.emojis.queued, params_.emojis.thinking,
            params_.emojis.tool, params_.emojis.coding, params_.emojis.web,
            params_.emojis.done, params_.emojis.error,
            params_.emojis.stall_soft, params_.emojis.stall_hard,
            params_.emojis.compacting
        };
        for (const auto& e : known) {
            if (e.empty()) continue;
            try { params_.adapter.remove_reaction(e); } catch (...) {}
        }
    }
    current_emoji_.clear();
    pending_emoji_.clear();
}

void StatusReactionController::restore_initial() {
    if (!params_.enabled) return;
    clear_all_timers();
    apply_emoji(params_.initial_emoji);
    pending_emoji_.clear();
}

} // namespace channels
} // namespace hunterclaw
