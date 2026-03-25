// Aligned with openclaw: src/channels/run-state-machine.ts
#include "hunterclaw/channels/run_state_machine.h"
#include <algorithm>
#include <chrono>

namespace hunterclaw {
namespace channels {

RunStateMachine::RunStateMachine(Params params)
    : params_(std::move(params))
{
    // Reset inherited status from previous process lifecycle
    if (lifecycle_active_ && params_.set_status) {
        RunStateStatusPatch patch;
        patch.active_runs = 0;
        patch.busy = false;
        params_.set_status(patch);
    }
}

RunStateMachine::~RunStateMachine() {
    deactivate();
}

int64_t RunStateMachine::now_ms() const {
    if (params_.now) return params_.now();
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

void RunStateMachine::publish() {
    if (!lifecycle_active_) return;
    if (!params_.set_status) return;
    int runs = active_runs_.load();
    RunStateStatusPatch patch;
    patch.active_runs = runs;
    patch.busy = (runs > 0);
    patch.last_run_activity_at = now_ms();
    params_.set_status(patch);
}

void RunStateMachine::heartbeat_loop() {
    while (!stop_heartbeat_) {
        // Sleep in small increments to allow fast shutdown
        for (int i = 0; i < params_.heartbeat_ms / 50 && !stop_heartbeat_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (stop_heartbeat_) break;
        if (!lifecycle_active_ || active_runs_.load() <= 0) {
            heartbeat_running_ = false;
            return;
        }
        publish();
    }
    heartbeat_running_ = false;
}

void RunStateMachine::ensure_heartbeat() {
    if (heartbeat_running_) return;
    if (active_runs_.load() <= 0) return;
    if (!lifecycle_active_) return;

    std::lock_guard<std::mutex> lock(heartbeat_mutex_);
    if (heartbeat_running_) return;

    stop_heartbeat_ = false;
    heartbeat_running_ = true;
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    heartbeat_thread_ = std::thread([this]() { heartbeat_loop(); });
    heartbeat_thread_.detach();
}

void RunStateMachine::clear_heartbeat() {
    stop_heartbeat_ = true;
}

void RunStateMachine::on_run_start() {
    active_runs_.fetch_add(1);
    publish();
    ensure_heartbeat();
}

void RunStateMachine::on_run_end() {
    int prev = active_runs_.fetch_sub(1);
    if (prev <= 1) {
        active_runs_.store(0);
        clear_heartbeat();
    }
    publish();
}

void RunStateMachine::deactivate() {
    lifecycle_active_ = false;
    clear_heartbeat();
}

bool RunStateMachine::is_active() const {
    return lifecycle_active_.load();
}

} // namespace channels
} // namespace hunterclaw
