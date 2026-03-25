// Aligned with openclaw: src/channels/typing.ts
#include "hunterclaw/channels/typing.h"
#include <memory>
#include <atomic>
#include <thread>
#include <chrono>

namespace hunterclaw {
namespace channels {

TypingCallbacks create_typing_callbacks(CreateTypingCallbacksParams params) {
    // Shared state
    struct State {
        std::atomic<bool> stop_sent{false};
        std::atomic<bool> closed{false};

        // TTL timer state
        std::atomic<bool> ttl_active{false};
        std::thread ttl_thread;
        std::atomic<bool> ttl_stop{false};

        std::function<void()> start_fn;
        std::function<void()> stop_fn;
        std::function<void(const std::string&)> on_start_error;
        std::function<void(const std::string&)> on_stop_error;
        int max_duration_ms;
        int keepalive_interval_ms;
        int max_consecutive_failures;

        std::unique_ptr<TypingStartGuard> start_guard;
        std::unique_ptr<TypingKeepaliveLoop> keepalive_loop;
    };

    auto state = std::make_shared<State>();
    state->start_fn = params.start;
    state->stop_fn = params.stop;
    state->on_start_error = params.on_start_error;
    state->on_stop_error = params.on_stop_error;
    state->max_duration_ms = params.max_duration_ms;
    state->keepalive_interval_ms = params.keepalive_interval_ms;
    state->max_consecutive_failures = params.max_consecutive_failures;

    // fire_start: calls start() through the guard
    auto fire_start = [state]() {
        state->start_guard->run([state]() {
            if (state->start_fn) state->start_fn();
        });
    };

    // Create start guard
    TypingStartGuard::Params guard_params;
    guard_params.is_sealed = [state]() { return state->closed.load(); };
    guard_params.on_start_error = params.on_start_error;
    guard_params.max_consecutive_failures = params.max_consecutive_failures;
    guard_params.on_trip = [state]() {
        if (state->keepalive_loop) state->keepalive_loop->stop();
    };
    state->start_guard = std::make_unique<TypingStartGuard>(std::move(guard_params));

    // Create keepalive loop
    state->keepalive_loop = std::make_unique<TypingKeepaliveLoop>(
        params.keepalive_interval_ms,
        fire_start
    );

    // TTL timer helpers
    auto clear_ttl = [state]() {
        state->ttl_stop = true;
        state->ttl_active = false;
    };

    auto fire_stop = [state, clear_ttl]() {
        state->closed = true;
        if (state->keepalive_loop) state->keepalive_loop->stop();
        clear_ttl();
        if (!state->stop_fn || state->stop_sent.exchange(true)) return;
        try {
            state->stop_fn();
        } catch (const std::exception& e) {
            auto& err_fn = state->on_stop_error ? state->on_stop_error : state->on_start_error;
            if (err_fn) err_fn(e.what());
        }
    };

    auto start_ttl = [state, fire_stop, clear_ttl]() {
        if (state->max_duration_ms <= 0) return;
        clear_ttl();
        state->ttl_stop = false;
        state->ttl_active = true;
        if (state->ttl_thread.joinable()) state->ttl_thread.detach();
        state->ttl_thread = std::thread([state, fire_stop]() {
            int elapsed = 0;
            while (!state->ttl_stop && elapsed < state->max_duration_ms) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                elapsed += 50;
            }
            if (!state->ttl_stop && !state->closed) {
                fire_stop();
            }
        });
        state->ttl_thread.detach();
    };

    // on_reply_start: aligned with OpenClaw onReplyStart()
    auto on_reply_start = [state, fire_start, start_ttl, clear_ttl]() {
        if (state->closed) return;
        state->stop_sent = false;
        state->start_guard->reset();
        if (state->keepalive_loop) state->keepalive_loop->stop();
        clear_ttl();
        fire_start();
        if (state->start_guard->is_tripped()) return;
        if (state->keepalive_loop) state->keepalive_loop->start();
        start_ttl();
    };

    TypingCallbacks callbacks;
    callbacks.on_reply_start = on_reply_start;
    callbacks.on_idle = fire_stop;
    callbacks.on_cleanup = fire_stop;
    return callbacks;
}

} // namespace channels
} // namespace hunterclaw
