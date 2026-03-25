// Aligned with openclaw: src/channels/draft-stream-controls.ts + draft-stream-loop.ts
#include "hunterclaw/channels/draft_stream_controls.h"
#include <memory>

namespace hunterclaw {
namespace channels {

// ── DraftStreamLoop ────────────────────────────────────────────────────────

DraftStreamLoop::DraftStreamLoop(Params params)
    : params_(std::move(params)) {}

DraftStreamLoop::~DraftStreamLoop() {
    stop();
}

void DraftStreamLoop::send_pending() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        text = pending_text_;
    }
    if (text.empty()) return;
    in_flight_ = true;
    try {
        if (params_.send_or_edit_stream_message) {
            params_.send_or_edit_stream_message(text);
        }
    } catch (...) {}
    in_flight_ = false;
}

void DraftStreamLoop::update(const std::string& text) {
    if (stopped_ || (params_.is_stopped && params_.is_stopped())) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_text_ = text;
    }
    if (throttle_active_) return;

    throttle_cancel_ = false;
    throttle_active_ = true;
    if (throttle_thread_.joinable()) throttle_thread_.detach();
    throttle_thread_ = std::thread([this]() {
        for (int i = 0; i < params_.throttle_ms / 50 && !throttle_cancel_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!throttle_cancel_) send_pending();
        throttle_active_ = false;
    });
    throttle_thread_.detach();
}

void DraftStreamLoop::flush() {
    throttle_cancel_ = true;
    throttle_active_ = false;
    send_pending();
}

void DraftStreamLoop::stop() {
    stopped_ = true;
    throttle_cancel_ = true;
    throttle_active_ = false;
}

void DraftStreamLoop::wait_for_in_flight() {
    while (in_flight_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ── FinalizableDraftStreamControls ────────────────────────────────────────

FinalizableDraftStreamControls create_finalizable_draft_stream_controls(
    int throttle_ms,
    std::function<bool()> is_stopped,
    std::function<bool()> is_final,
    std::function<void()> mark_stopped,
    std::function<void()> mark_final,
    std::function<bool(const std::string&)> send_or_edit_stream_message)
{
    auto loop = std::make_shared<DraftStreamLoop>(DraftStreamLoop::Params{
        throttle_ms,
        is_stopped,
        send_or_edit_stream_message
    });

    FinalizableDraftStreamControls controls;
    controls.loop = loop.get();

    controls.update = [loop, is_stopped, is_final](const std::string& text) {
        if (is_stopped() || is_final()) return;
        loop->update(text);
    };

    controls.stop = [loop, mark_final]() {
        mark_final();
        loop->flush();
    };

    controls.stop_for_clear = [loop, mark_stopped]() {
        mark_stopped();
        loop->stop();
        loop->wait_for_in_flight();
    };

    // Keep loop alive via shared_ptr captured in lambdas
    return controls;
}

FinalizableDraftStreamControls create_finalizable_draft_stream_controls_for_state(
    int throttle_ms,
    FinalizableDraftStreamState& state,
    std::function<bool(const std::string&)> send_or_edit_stream_message)
{
    return create_finalizable_draft_stream_controls(
        throttle_ms,
        [&state]() { return state.stopped; },
        [&state]() { return state.final; },
        [&state]() { state.stopped = true; },
        [&state]() { state.final = true; },
        std::move(send_or_edit_stream_message)
    );
}

} // namespace channels
} // namespace hunterclaw
