// Aligned with openclaw: src/channels/draft-stream-controls.ts
#pragma once

#include <string>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace channels {

// ============================================================================
// DraftStreamControls
// Aligned with OpenClaw: draft-stream-controls.ts
// ============================================================================

// Aligned with OpenClaw: FinalizableDraftStreamState
struct FinalizableDraftStreamState {
    bool stopped = false;
    bool final = false;
};

// ── DraftStreamLoop ────────────────────────────────────────────────────────

/**
 * Throttled draft stream loop.
 * Aligned with OpenClaw: createDraftStreamLoop (draft-stream-loop.ts)
 */
class DraftStreamLoop {
public:
    struct Params {
        int throttle_ms = 1000;
        std::function<bool()> is_stopped;
        std::function<bool(const std::string&)> send_or_edit_stream_message;
    };

    explicit DraftStreamLoop(Params params);
    ~DraftStreamLoop();

    // Aligned with OpenClaw: update()
    void update(const std::string& text);
    // Aligned with OpenClaw: flush()
    void flush();
    // Aligned with OpenClaw: stop()
    void stop();
    // Aligned with OpenClaw: waitForInFlight()
    void wait_for_in_flight();

private:
    Params params_;
    std::string pending_text_;
    std::atomic<bool> in_flight_{false};
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::thread throttle_thread_;
    std::atomic<bool> throttle_active_{false};
    std::atomic<bool> throttle_cancel_{false};

    void send_pending();
};

// ── FinalizableDraftStreamControls ────────────────────────────────────────

/**
 * Finalizable draft stream controls.
 * Aligned with OpenClaw: createFinalizableDraftStreamControls()
 */
struct FinalizableDraftStreamControls {
    DraftStreamLoop* loop = nullptr;
    std::function<void(const std::string&)> update;
    std::function<void()> stop;
    std::function<void()> stop_for_clear;
};

FinalizableDraftStreamControls create_finalizable_draft_stream_controls(
    int throttle_ms,
    std::function<bool()> is_stopped,
    std::function<bool()> is_final,
    std::function<void()> mark_stopped,
    std::function<void()> mark_final,
    std::function<bool(const std::string&)> send_or_edit_stream_message);

/**
 * Create controls bound to a FinalizableDraftStreamState.
 * Aligned with OpenClaw: createFinalizableDraftStreamControlsForState()
 */
FinalizableDraftStreamControls create_finalizable_draft_stream_controls_for_state(
    int throttle_ms,
    FinalizableDraftStreamState& state,
    std::function<bool(const std::string&)> send_or_edit_stream_message);

/**
 * Stop and take message ID.
 * Aligned with OpenClaw: takeMessageIdAfterStop()
 */
template <typename T>
std::optional<T> take_message_id_after_stop(
    std::function<void()> stop_for_clear,
    std::function<std::optional<T>()> read_message_id,
    std::function<void()> clear_message_id)
{
    stop_for_clear();
    auto id = read_message_id();
    clear_message_id();
    return id;
}

/**
 * Clear a finalizable draft message.
 * Aligned with OpenClaw: clearFinalizableDraftMessage()
 */
template <typename T>
void clear_finalizable_draft_message(
    std::function<void()> stop_for_clear,
    std::function<std::optional<T>()> read_message_id,
    std::function<void()> clear_message_id,
    std::function<bool(const T&)> is_valid_message_id,
    std::function<void(const T&)> delete_message,
    std::function<void(const T&)> on_delete_success = nullptr,
    std::function<void(const std::string&)> warn = nullptr,
    const std::string& warn_prefix = "")
{
    auto id = take_message_id_after_stop<T>(stop_for_clear, read_message_id, clear_message_id);
    if (!id || !is_valid_message_id(*id)) return;
    try {
        delete_message(*id);
        if (on_delete_success) on_delete_success(*id);
    } catch (const std::exception& e) {
        if (warn) warn(warn_prefix + ": " + e.what());
    }
}

} // namespace channels
} // namespace hunterclaw
