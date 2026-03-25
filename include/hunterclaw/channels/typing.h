// Aligned with openclaw:
//   src/channels/typing.ts
//   src/channels/typing-lifecycle.ts
//   src/channels/typing-start-guard.ts
#pragma once

#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <stdexcept>

namespace hunterclaw {
namespace channels {

// ============================================================================
// TypingKeepaliveLoop
// Aligned with OpenClaw: createTypingKeepaliveLoop (typing-lifecycle.ts)
// ============================================================================

class TypingKeepaliveLoop {
public:
    explicit TypingKeepaliveLoop(int interval_ms, std::function<void()> on_tick)
        : interval_ms_(interval_ms), on_tick_(std::move(on_tick)) {}

    ~TypingKeepaliveLoop() { stop(); }

    // Aligned with OpenClaw: start()
    void start() {
        if (interval_ms_ <= 0 || running_) return;
        stop_flag_ = false;
        running_ = true;
        thread_ = std::thread([this]() {
            while (!stop_flag_) {
                for (int i = 0; i < interval_ms_ / 50 && !stop_flag_; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (stop_flag_) break;
                if (!tick_in_flight_.exchange(true)) {
                    try { on_tick_(); } catch (...) {}
                    tick_in_flight_ = false;
                }
            }
            running_ = false;
        });
        thread_.detach();
    }

    // Aligned with OpenClaw: stop()
    void stop() {
        stop_flag_ = true;
        tick_in_flight_ = false;
        running_ = false;
    }

    // Aligned with OpenClaw: isRunning()
    bool is_running() const { return running_.load(); }

private:
    int interval_ms_;
    std::function<void()> on_tick_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> tick_in_flight_{false};
    std::thread thread_;
};

// ============================================================================
// TypingStartGuard
// Aligned with OpenClaw: createTypingStartGuard (typing-start-guard.ts)
// ============================================================================

class TypingStartGuard {
public:
    struct Params {
        std::function<bool()> is_sealed;
        std::function<bool()> should_block;
        std::function<void(const std::string&)> on_start_error;
        int max_consecutive_failures = 2;
        std::function<void()> on_trip;
    };

    enum class RunResult { Started, Skipped, Failed, Tripped };

    explicit TypingStartGuard(Params params)
        : params_(std::move(params)) {}

    // Aligned with OpenClaw: run()
    RunResult run(std::function<void()> start) {
        if (is_blocked()) return RunResult::Skipped;
        try {
            start();
            consecutive_failures_ = 0;
            return RunResult::Started;
        } catch (const std::exception& e) {
            consecutive_failures_++;
            if (params_.on_start_error) params_.on_start_error(e.what());
            if (params_.max_consecutive_failures > 0 &&
                consecutive_failures_ >= params_.max_consecutive_failures) {
                tripped_ = true;
                if (params_.on_trip) params_.on_trip();
                return RunResult::Tripped;
            }
            return RunResult::Failed;
        }
    }

    // Aligned with OpenClaw: reset()
    void reset() {
        consecutive_failures_ = 0;
        tripped_ = false;
    }

    // Aligned with OpenClaw: isTripped()
    bool is_tripped() const { return tripped_; }

private:
    bool is_blocked() const {
        if (params_.is_sealed && params_.is_sealed()) return true;
        if (tripped_) return true;
        if (params_.should_block && params_.should_block()) return true;
        return false;
    }

    Params params_;
    int consecutive_failures_ = 0;
    bool tripped_ = false;
};

// ============================================================================
// TypingCallbacks / createTypingCallbacks
// Aligned with OpenClaw: createTypingCallbacks (typing.ts)
// ============================================================================

struct TypingCallbacks {
    std::function<void()> on_reply_start;  // async in TS, sync here
    std::function<void()> on_idle;
    std::function<void()> on_cleanup;
};

struct CreateTypingCallbacksParams {
    std::function<void()> start;
    std::function<void()> stop;
    std::function<void(const std::string&)> on_start_error;
    std::function<void(const std::string&)> on_stop_error;
    int keepalive_interval_ms = 3000;
    int max_consecutive_failures = 2;
    int max_duration_ms = 60000;  // 60s TTL
};

// Aligned with OpenClaw: createTypingCallbacks()
TypingCallbacks create_typing_callbacks(CreateTypingCallbacksParams params);

} // namespace channels
} // namespace hunterclaw
