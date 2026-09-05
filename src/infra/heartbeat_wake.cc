// ============================================================================
// heartbeat_wake.cc
// Aligned with openclaw: src/infra/heartbeat-wake.ts
//                      + src/infra/heartbeat-wake-target.ts
//
// 真源是单线程 event loop + setTimeout；C++ 需要显式的调度线程与锁。
// 语义等价点逐条标注在下方各函数注释里。
// ============================================================================

#include "hunterclaw/infra/heartbeat_wake.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace hunterclaw {
namespace infra {

// ── Constants (aligned with openclaw heartbeat-wake.ts:107-118) ──────────────

namespace {

// Aligned with openclaw: const DEFAULT_COALESCE_MS = 250;
constexpr int64_t DEFAULT_COALESCE_MS = 250;

// Aligned with openclaw: const MAX_CONCURRENT_HEARTBEAT_WAKE_TARGETS = 4;
constexpr size_t MAX_CONCURRENT_WAKE_TARGETS = 4;

// Aligned with openclaw: REASON_PRIORITY { RETRY: 0, INTERVAL: 1, DEFAULT: 2, ACTION: 3 }
constexpr int PRIORITY_RETRY = 0;
constexpr int PRIORITY_INTERVAL = 1;
constexpr int PRIORITY_DEFAULT = 2;
constexpr int PRIORITY_ACTION = 3;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string normalize_target(const std::optional<std::string>& value) {
    if (!value) return {};
    const auto& s = *value;
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Aligned with openclaw: resolveWakePriority (heartbeat-wake.ts:119-138)
int resolve_wake_priority(HeartbeatWakeSource source,
                          HeartbeatWakeIntent intent,
                          const std::string& reason) {
    if (intent == HeartbeatWakeIntent::Manual || intent == HeartbeatWakeIntent::Immediate) {
        return PRIORITY_ACTION;
    }
    if (source == HeartbeatWakeSource::Retry || reason == "retry") {
        return PRIORITY_RETRY;
    }
    if (intent == HeartbeatWakeIntent::Scheduled ||
        source == HeartbeatWakeSource::Interval || reason == "interval") {
        return PRIORITY_INTERVAL;
    }
    return PRIORITY_DEFAULT;
}

// Aligned with openclaw: PendingWakeReason
struct PendingWakeReason {
    std::optional<std::string> reason;
    std::optional<std::string> agent_id;
    std::optional<std::string> session_key;
    HeartbeatWakeIntent intent = HeartbeatWakeIntent::Event;
    HeartbeatWakeSource source = HeartbeatWakeSource::Other;
    int priority = PRIORITY_DEFAULT;
    int64_t requested_at = 0;
    int64_t ready_at_ms = 0;
    uint64_t enqueue_sequence = 0;
};

// Aligned with openclaw: PendingWakeGroup { task?, scheduled?, event? }
struct PendingWakeGroup {
    std::optional<PendingWakeReason> task;
    std::optional<PendingWakeReason> scheduled;
    std::optional<PendingWakeReason> event;

    bool empty() const { return !task && !scheduled && !event; }
};

// Aligned with openclaw: mergePendingWakeReasons (heartbeat-wake.ts:144-167)
//   mixedTaskPair → 保留 task 侧；否则按 priority，同 priority 取更晚 requestedAt。
PendingWakeReason merge_pending_wake_reasons(const PendingWakeReason& previous,
                                             const PendingWakeReason& next) {
    const bool previous_is_task = previous.intent == HeartbeatWakeIntent::Task;
    const bool next_is_task = next.intent == HeartbeatWakeIntent::Task;
    if (previous_is_task != next_is_task) {
        PendingWakeReason preferred = previous_is_task ? previous : next;
        // 更早的 ready_at 决定何时可派发。
        preferred.ready_at_ms = std::min(previous.ready_at_ms, next.ready_at_ms);
        preferred.enqueue_sequence = std::min(previous.enqueue_sequence, next.enqueue_sequence);
        return preferred;
    }
    const bool take_next =
        next.priority > previous.priority ||
        (next.priority == previous.priority && next.requested_at >= previous.requested_at);
    PendingWakeReason preferred = take_next ? next : previous;
    preferred.ready_at_ms = std::min(previous.ready_at_ms, next.ready_at_ms);
    preferred.enqueue_sequence = std::min(previous.enqueue_sequence, next.enqueue_sequence);
    return preferred;
}

// Aligned with openclaw: isHeartbeatWakeTargetGroupReady (heartbeat-wake-target.ts)
bool is_group_ready(const PendingWakeGroup& group, int64_t now) {
    const PendingWakeReason* slots[] = {
        group.task ? &*group.task : nullptr,
        group.scheduled ? &*group.scheduled : nullptr,
        group.event ? &*group.event : nullptr,
    };
    for (const auto* slot : slots) {
        if (slot && slot->ready_at_ms <= now) return true;
    }
    return false;
}

HeartbeatWakeParams to_handler_params(const PendingWakeReason& wake) {
    HeartbeatWakeParams params;
    params.reason = wake.reason;
    params.agent_id = wake.agent_id;
    params.session_key = wake.session_key;
    params.intent = wake.intent;
    params.source = wake.source;
    return params;
}

}  // namespace

const char* const GLOBAL_HEARTBEAT_WAKE_TARGET_KEY = "::";

// Aligned with openclaw: resolveHeartbeatWakeTargetKey (heartbeat-wake-target.ts:15-25)
//   一个 canonical session 独占串行化；冗余携带的 agentId 不得把同一 session
//   拆成两个并发 target。
std::string resolve_heartbeat_wake_target_key(
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key) {
    const std::string s = normalize_target(session_key);
    if (!s.empty()) return "::" + s;
    return normalize_target(agent_id) + "::";
}

// ============================================================================
// Scheduler
//
// 真源用单个 setTimeout 承载合并窗口；这里用一个常驻调度线程 + condition_variable
// 等价实现（可被更早的请求抢占唤醒）。
// ============================================================================

namespace {

class WakeScheduler {
public:
    static WakeScheduler& instance() {
        // Leaky singleton：避免静态析构顺序问题（调度线程可能晚于其他静态对象退出）。
        static WakeScheduler* inst = new WakeScheduler();
        return *inst;
    }

    std::function<void()> set_handler(HeartbeatWakeHandler handler) {
        std::lock_guard<std::mutex> lock(mu_);
        // Aligned with openclaw: handlerGeneration += 1
        ++handler_generation_;
        const uint64_t generation = handler_generation_;
        handler_ = std::move(handler);
        return [this, generation]() {
            std::lock_guard<std::mutex> lock(mu_);
            // Aligned with openclaw: if (handlerGeneration !== generation) return;
            if (handler_generation_ != generation) return;
            ++handler_generation_;
            handler_ = nullptr;
        };
    }

    // Aligned with openclaw: requestHeartbeat → queuePendingWakeReason + schedule
    void request(const HeartbeatWakeRequest& req) {
        const int64_t requested_at = now_ms();
        const int64_t coalesce_ms = std::max<int64_t>(
            0, req.coalesce_ms.value_or(DEFAULT_COALESCE_MS));

        std::unique_lock<std::mutex> lock(mu_);
        if (!handler_) {
            // 真源在无 handler 时同样会入队（等待 handler 注册后 handOff）。
            // 但 HunterClaw 无 handOff 路径，入队会永久堆积，故直接丢弃。
            return;
        }

        PendingWakeReason wake;
        wake.reason = req.reason;
        wake.agent_id = req.agent_id;
        wake.session_key = req.session_key;
        wake.intent = req.intent;
        wake.source = req.source;
        wake.priority = resolve_wake_priority(req.source, req.intent,
                                              req.reason.value_or(std::string{}));
        wake.requested_at = requested_at;
        wake.ready_at_ms = requested_at + coalesce_ms;
        wake.enqueue_sequence = ++enqueue_sequence_;

        const std::string target_key =
            resolve_heartbeat_wake_target_key(req.agent_id, req.session_key);

        // Aligned with openclaw: slot = intent === "task" ? "task"
        //                             : intent === "scheduled" ? "scheduled" : "event"
        PendingWakeGroup& group = pending_[target_key];
        std::optional<PendingWakeReason>* slot = nullptr;
        switch (req.intent) {
            case HeartbeatWakeIntent::Task:      slot = &group.task; break;
            case HeartbeatWakeIntent::Scheduled: slot = &group.scheduled; break;
            default:                             slot = &group.event; break;
        }
        if (*slot) {
            *slot = merge_pending_wake_reasons(**slot, wake);
        } else {
            *slot = wake;
        }

        schedule_locked(wake.ready_at_ms, lock);
    }

    size_t pending_group_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return pending_.size();
    }

    size_t active_target_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return active_targets_.size();
    }

    bool drain_for_test(int64_t timeout_ms) {
        std::unique_lock<std::mutex> lock(mu_);
        return idle_cv_.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [this]() { return pending_.empty() && active_targets_.empty(); });
    }

    void reset_for_test() {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.clear();
        active_targets_.clear();
        timer_due_at_.reset();
    }

private:
    WakeScheduler() {
        worker_ = std::thread([this]() { run(); });
        worker_.detach();
    }

    // Aligned with openclaw: schedule(coalesceMs) (heartbeat-wake.ts:543-556)
    //   已有 timer 更早或同时到期 → 保留；新请求更早 → 抢占。
    void schedule_locked(int64_t due_at, std::unique_lock<std::mutex>& /*lock*/) {
        if (timer_due_at_ && *timer_due_at_ <= due_at) {
            return;  // keep the existing, sooner timer
        }
        timer_due_at_ = due_at;
        cv_.notify_all();
    }

    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        for (;;) {
            if (!timer_due_at_) {
                cv_.wait(lock);
                continue;
            }
            const int64_t due = *timer_due_at_;
            const int64_t now = now_ms();
            if (now < due) {
                cv_.wait_for(lock, std::chrono::milliseconds(due - now));
                continue;
            }
            timer_due_at_.reset();
            fire_locked(lock);
        }
    }

    // Aligned with openclaw: setTimeout callback (heartbeat-wake.ts:557-586)
    void fire_locked(std::unique_lock<std::mutex>& lock) {
        if (!handler_) {
            pending_.clear();
            idle_cv_.notify_all();
            return;
        }
        const uint64_t generation = handler_generation_;
        HeartbeatWakeHandler active = handler_;

        const size_t available_slots =
            MAX_CONCURRENT_WAKE_TARGETS > active_targets_.size()
                ? MAX_CONCURRENT_WAKE_TARGETS - active_targets_.size()
                : 0;

        auto batch = take_pending_batch_locked(available_slots, now_ms());
        for (auto& entry : batch) {
            active_targets_.insert(entry.first);
            // Aligned with openclaw: void dispatchPendingWakeGroup(...)
            //   每个 target 独立异步执行，互不阻塞。
            std::thread([this, active, generation, target_key = entry.first,
                         wakes = std::move(entry.second)]() {
                dispatch_group(active, generation, target_key, wakes);
            }).detach();
        }

        // Aligned with openclaw: if (pendingWakes.size > 0) schedulePendingWakes(delay)
        if (!pending_.empty()) {
            schedule_locked(now_ms() + DEFAULT_COALESCE_MS, lock);
        } else if (active_targets_.empty()) {
            idle_cv_.notify_all();
        }
    }

    // Aligned with openclaw: takePendingWakeBatch (heartbeat-wake.ts:215-...)
    //   - 全局 target 正在执行 → 本轮不派发任何 target
    //   - 全局 target 就绪且有定向 turn 在跑 → 让路，等定向 turn 退出
    std::vector<std::pair<std::string, std::vector<PendingWakeReason>>>
    take_pending_batch_locked(size_t max_groups, int64_t now) {
        std::vector<std::pair<std::string, std::vector<PendingWakeReason>>> out;
        if (max_groups == 0) return out;

        const std::string global_key = GLOBAL_HEARTBEAT_WAKE_TARGET_KEY;
        if (active_targets_.count(global_key) > 0) {
            return out;
        }
        auto global_it = pending_.find(global_key);
        const bool global_ready =
            global_it != pending_.end() && is_group_ready(global_it->second, now);
        if (global_ready && !active_targets_.empty()) {
            // 无作用域的 wake 会扇出到所有 agent，绝不与定向 turn 并行。
            return out;
        }

        std::vector<std::string> ordered_keys;
        ordered_keys.reserve(pending_.size());
        if (global_ready) {
            ordered_keys.push_back(global_key);
        } else {
            for (const auto& kv : pending_) {
                if (kv.first == global_key) continue;
                ordered_keys.push_back(kv.first);
            }
        }

        for (const auto& key : ordered_keys) {
            if (out.size() >= max_groups) break;
            if (active_targets_.count(key) > 0) continue;  // 该 target 已有 turn 在跑
            auto it = pending_.find(key);
            if (it == pending_.end()) continue;
            if (!is_group_ready(it->second, now)) continue;

            std::vector<PendingWakeReason> wakes;
            auto take = [&](std::optional<PendingWakeReason>& slot) {
                if (slot && slot->ready_at_ms <= now) {
                    wakes.push_back(*slot);
                    slot.reset();
                }
            };
            take(it->second.task);
            take(it->second.scheduled);
            take(it->second.event);

            if (it->second.empty()) {
                pending_.erase(it);
            }
            if (!wakes.empty()) {
                out.emplace_back(key, std::move(wakes));
            }
        }
        return out;
    }

    // Aligned with openclaw: dispatchPendingWakeGroup
    //   同一 target 的多个 wake 串行执行，执行完释放 target 槽并重排调度。
    void dispatch_group(const HeartbeatWakeHandler& active,
                        uint64_t generation,
                        const std::string& target_key,
                        const std::vector<PendingWakeReason>& wakes) {
        for (const auto& wake : wakes) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                // handler 已被替换 → 放弃剩余工作（真源走 handOffPendingWakeBatch，
                // HunterClaw 无该路径，直接丢弃以免用旧 handler 执行）。
                if (handler_generation_ != generation) break;
            }
            try {
                active(to_handler_params(wake));
            } catch (...) {
                // Aligned with openclaw: errors are logged by the runner, not here
            }
        }

        std::unique_lock<std::mutex> lock(mu_);
        active_targets_.erase(target_key);
        if (!pending_.empty()) {
            schedule_locked(now_ms(), lock);
        } else if (active_targets_.empty()) {
            idle_cv_.notify_all();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::thread worker_;

    HeartbeatWakeHandler handler_;
    uint64_t handler_generation_ = 0;
    uint64_t enqueue_sequence_ = 0;

    std::map<std::string, PendingWakeGroup> pending_;
    std::set<std::string> active_targets_;
    // Aligned with openclaw: let timerDueAt: number | null = null;
    std::optional<int64_t> timer_due_at_;
};

}  // namespace

// ── Module-level state ───────────────────────────────────────────────────────

static std::atomic<bool> g_heartbeats_enabled{true};

// ── are_heartbeats_enabled ───────────────────────────────────────────────────
// Aligned with openclaw: areHeartbeatsEnabled

bool are_heartbeats_enabled() {
    return g_heartbeats_enabled.load();
}

// ── set_heartbeats_enabled ───────────────────────────────────────────────────
// Aligned with openclaw: setHeartbeatsEnabled

void set_heartbeats_enabled(bool enabled) {
    g_heartbeats_enabled.store(enabled);
}

// ── set_heartbeat_wake_handler ───────────────────────────────────────────────
// Aligned with openclaw: setHeartbeatWakeHandler

std::function<void()> set_heartbeat_wake_handler(HeartbeatWakeHandler handler) {
    return WakeScheduler::instance().set_handler(std::move(handler));
}

// ── request_heartbeat ────────────────────────────────────────────────────────
// Aligned with openclaw: requestHeartbeat (heartbeat-wake.ts:716-744)

void request_heartbeat(const HeartbeatWakeRequest& request) {
    WakeScheduler::instance().request(request);
}

// ── request_heartbeat_now (legacy positional shim) ───────────────────────────

void request_heartbeat_now(
    const std::optional<std::string>& reason,
    const std::optional<std::string>& agent_id,
    const std::optional<std::string>& session_key,
    HeartbeatWakeIntent intent,
    HeartbeatWakeSource source
) {
    HeartbeatWakeRequest request;
    request.reason = reason;
    request.agent_id = agent_id;
    request.session_key = session_key;
    request.intent = intent;
    request.source = source;
    request_heartbeat(request);
}

// ── Test hooks ───────────────────────────────────────────────────────────────

namespace testing {

size_t pending_wake_group_count() {
    return WakeScheduler::instance().pending_group_count();
}

size_t active_wake_target_count() {
    return WakeScheduler::instance().active_target_count();
}

bool drain_wakes_for_test(int64_t timeout_ms) {
    return WakeScheduler::instance().drain_for_test(timeout_ms);
}

void reset_wake_state_for_test() {
    WakeScheduler::instance().reset_for_test();
}

}  // namespace testing

} // namespace infra
} // namespace hunterclaw
