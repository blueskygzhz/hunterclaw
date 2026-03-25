// Aligned with openclaw: src/channels/run-state-machine.ts
#pragma once

#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <optional>

namespace hunterclaw {
namespace channels {

// ============================================================================
// RunStateMachine
// Aligned with OpenClaw: createRunStateMachine (run-state-machine.ts)
//
// Tracks active runs, publishes busy/activeRuns/lastRunActivityAt status,
// and maintains a heartbeat interval while runs are active.
// ============================================================================

struct RunStateStatusPatch {
    std::optional<bool> busy;
    std::optional<int> active_runs;
    std::optional<int64_t> last_run_activity_at;  // ms since epoch, -1 = null
};

using RunStateStatusSink = std::function<void(const RunStateStatusPatch&)>;

class RunStateMachine {
public:
    struct Params {
        RunStateStatusSink set_status;
        int heartbeat_ms = 60000;  // DEFAULT_RUN_ACTIVITY_HEARTBEAT_MS
        std::function<int64_t()> now;  // optional clock override
    };

    explicit RunStateMachine(Params params);
    ~RunStateMachine();

    // Non-copyable, non-movable (owns a thread)
    RunStateMachine(const RunStateMachine&) = delete;
    RunStateMachine& operator=(const RunStateMachine&) = delete;

    // Aligned with OpenClaw: onRunStart()
    void on_run_start();

    // Aligned with OpenClaw: onRunEnd()
    void on_run_end();

    // Aligned with OpenClaw: deactivate()
    void deactivate();

    // Aligned with OpenClaw: isActive()
    bool is_active() const;

private:
    Params params_;
    std::atomic<int> active_runs_{0};
    std::atomic<bool> lifecycle_active_{true};

    // Heartbeat
    std::mutex heartbeat_mutex_;
    std::thread heartbeat_thread_;
    std::atomic<bool> heartbeat_running_{false};
    std::atomic<bool> stop_heartbeat_{false};

    int64_t now_ms() const;
    void publish();
    void ensure_heartbeat();
    void clear_heartbeat();
    void heartbeat_loop();
};

} // namespace channels
} // namespace hunterclaw
