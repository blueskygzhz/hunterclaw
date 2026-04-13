// ============================================================================
// gateway_channel_health.cc
// ✅ 严格对齐 OpenClaw:
//   - src/gateway/channel-health-policy.ts (150行)
//   - src/gateway/channel-health-monitor.ts (206行)
//
// 通道健康系统：
//   - ChannelHealthSnapshot: 通道运行时快照 (13字段)
//   - evaluateChannelHealth: 多层级健康评估策略
//     (unmanaged→not-running→busy/stuck→startup-grace→disconnected→stale-socket)
//   - resolveChannelRestartReason: 评估结果→重启原因映射
//   - startChannelHealthMonitor: 后台定时健康巡检 + 自动重启
//     (冷却机制 + 每小时频率限制 + AbortSignal 优雅停止)
// ============================================================================

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace hunterclaw {
namespace gateway {

namespace {
auto log() { return spdlog::get("gateway/health-monitor"); }
} // namespace

// ============================================================================
// 常量 (对齐 channel-health-policy.ts:52-56)
// ============================================================================

static constexpr int64_t BUSY_ACTIVITY_STALE_THRESHOLD_MS = 25 * 60 * 1000;      // 25min
static constexpr int64_t DEFAULT_CHANNEL_STALE_EVENT_THRESHOLD_MS = 30 * 60 * 1000; // 30min
static constexpr int64_t DEFAULT_CHANNEL_CONNECT_GRACE_MS = 120 * 1000;            // 120s

static constexpr int64_t DEFAULT_CHECK_INTERVAL_MS = 5 * 60 * 1000;    // 5min
static constexpr int64_t DEFAULT_MONITOR_STARTUP_GRACE_MS = 60 * 1000;  // 60s
static constexpr int DEFAULT_COOLDOWN_CYCLES = 2;
static constexpr int DEFAULT_MAX_RESTARTS_PER_HOUR = 10;
static constexpr int64_t ONE_HOUR_MS = 60 * 60 * 1000;

// ============================================================================
// 🎯 ChannelHealthSnapshot (对齐 channel-health-policy.ts:3-16)
// ============================================================================

struct ChannelHealthSnapshot {
    bool running = false;
    bool connected = false;
    bool enabled = true;
    bool configured = true;
    bool restart_pending = false;
    bool busy = false;
    int active_runs = 0;
    std::optional<int64_t> last_run_activity_at;
    std::optional<int64_t> last_event_at;
    std::optional<int64_t> last_start_at;
    int reconnect_attempts = 0;
    std::string mode;
};

// ============================================================================
// 🎯 ChannelHealthEvaluationReason (对齐 channel-health-policy.ts:18-26)
// ============================================================================

enum class ChannelHealthReason {
    Healthy,
    Unmanaged,
    NotRunning,
    Busy,
    Stuck,
    StartupConnectGrace,
    Disconnected,
    StaleSocket
};

static std::string reason_to_string(ChannelHealthReason r) {
    switch (r) {
        case ChannelHealthReason::Healthy: return "healthy";
        case ChannelHealthReason::Unmanaged: return "unmanaged";
        case ChannelHealthReason::NotRunning: return "not-running";
        case ChannelHealthReason::Busy: return "busy";
        case ChannelHealthReason::Stuck: return "stuck";
        case ChannelHealthReason::StartupConnectGrace: return "startup-connect-grace";
        case ChannelHealthReason::Disconnected: return "disconnected";
        case ChannelHealthReason::StaleSocket: return "stale-socket";
    }
    return "unknown";
}

struct ChannelHealthEvaluation {
    bool healthy = true;
    ChannelHealthReason reason = ChannelHealthReason::Healthy;
};

struct ChannelHealthPolicy {
    std::string channel_id;
    int64_t now = 0;
    int64_t stale_event_threshold_ms = DEFAULT_CHANNEL_STALE_EVENT_THRESHOLD_MS;
    int64_t channel_connect_grace_ms = DEFAULT_CHANNEL_CONNECT_GRACE_MS;
    bool skip_stale_socket_check = false;
};

// ============================================================================
// 🎯 ChannelRestartReason (对齐 channel-health-policy.ts:41-46)
// ============================================================================

enum class ChannelRestartReason {
    GaveUp,
    Stopped,
    StaleSocket,
    Stuck,
    Disconnected
};

static std::string restart_reason_to_string(ChannelRestartReason r) {
    switch (r) {
        case ChannelRestartReason::GaveUp: return "gave-up";
        case ChannelRestartReason::Stopped: return "stopped";
        case ChannelRestartReason::StaleSocket: return "stale-socket";
        case ChannelRestartReason::Stuck: return "stuck";
        case ChannelRestartReason::Disconnected: return "disconnected";
    }
    return "unknown";
}

// ============================================================================
// 🎯 evaluateChannelHealth (对齐 channel-health-policy.ts:58-133)
// ============================================================================

ChannelHealthEvaluation evaluate_channel_health(
    const ChannelHealthSnapshot& snapshot,
    const ChannelHealthPolicy& policy
) {
    // isManagedAccount — 对齐 :48-50
    if (!snapshot.enabled || !snapshot.configured) {
        return {true, ChannelHealthReason::Unmanaged};
    }

    if (!snapshot.running) {
        return {false, ChannelHealthReason::NotRunning};
    }

    int active_runs = std::max(0, snapshot.active_runs);
    bool is_busy = snapshot.busy || active_runs > 0;

    // 生命周期感知的 busy 状态检测 — 对齐 :81-82
    bool busy_state_initialized_for_lifecycle =
        !snapshot.last_start_at.has_value() ||
        (snapshot.last_run_activity_at.has_value() &&
         snapshot.last_run_activity_at.value() >= snapshot.last_start_at.value());

    // 对齐 :87-100 — patch-merged 快照下的生命周期隔离
    if (is_busy) {
        if (!busy_state_initialized_for_lifecycle) {
            // Fall through to normal startup/disconnect checks
        } else {
            int64_t run_activity_age = snapshot.last_run_activity_at.has_value()
                ? std::max(int64_t(0), policy.now - snapshot.last_run_activity_at.value())
                : std::numeric_limits<int64_t>::max();

            if (run_activity_age < BUSY_ACTIVITY_STALE_THRESHOLD_MS) {
                return {true, ChannelHealthReason::Busy};
            }
            return {false, ChannelHealthReason::Stuck};
        }
    }

    // 启动宽限期 — 对齐 :101-106
    if (snapshot.last_start_at.has_value()) {
        int64_t up_duration = policy.now - snapshot.last_start_at.value();
        if (up_duration < policy.channel_connect_grace_ms) {
            return {true, ChannelHealthReason::StartupConnectGrace};
        }
    }

    // 连接断开检测 — 对齐 :107-109
    if (!snapshot.connected) {
        return {false, ChannelHealthReason::Disconnected};
    }

    // stale-socket 检测 — 对齐 :114-131
    // webhook 模式和声明了 skipStaleSocketHealthCheck 的通道跳过
    if (!policy.skip_stale_socket_check &&
        snapshot.mode != "webhook" &&
        snapshot.connected &&
        snapshot.last_event_at.has_value())
    {
        if (snapshot.last_start_at.has_value() &&
            snapshot.last_event_at.value() < snapshot.last_start_at.value())
        {
            int64_t lifecycle_event_gap = std::max(
                int64_t(0), policy.now - snapshot.last_start_at.value());
            if (lifecycle_event_gap <= policy.stale_event_threshold_ms) {
                return {true, ChannelHealthReason::Healthy};
            }
            return {false, ChannelHealthReason::StaleSocket};
        }

        int64_t event_age = policy.now - snapshot.last_event_at.value();
        if (event_age > policy.stale_event_threshold_ms) {
            return {false, ChannelHealthReason::StaleSocket};
        }
    }

    return {true, ChannelHealthReason::Healthy};
}

// ============================================================================
// 🎯 resolveChannelRestartReason (对齐 channel-health-policy.ts:135-149)
// ============================================================================

ChannelRestartReason resolve_channel_restart_reason(
    const ChannelHealthSnapshot& snapshot,
    const ChannelHealthEvaluation& evaluation
) {
    if (evaluation.reason == ChannelHealthReason::StaleSocket) {
        return ChannelRestartReason::StaleSocket;
    }
    if (evaluation.reason == ChannelHealthReason::NotRunning) {
        return (snapshot.reconnect_attempts >= 10)
            ? ChannelRestartReason::GaveUp
            : ChannelRestartReason::Stopped;
    }
    if (evaluation.reason == ChannelHealthReason::Disconnected) {
        return ChannelRestartReason::Disconnected;
    }
    return ChannelRestartReason::Stuck;
}

// ============================================================================
// 🎯 ChannelHealthTimingPolicy (对齐 channel-health-monitor.ts:27-31)
// ============================================================================

struct ChannelHealthTimingPolicy {
    int64_t monitor_startup_grace_ms = DEFAULT_MONITOR_STARTUP_GRACE_MS;
    int64_t channel_connect_grace_ms = DEFAULT_CHANNEL_CONNECT_GRACE_MS;
    int64_t stale_event_threshold_ms = DEFAULT_CHANNEL_STALE_EVENT_THRESHOLD_MS;
};

// ============================================================================
// 🎯 RestartRecord (对齐 channel-health-monitor.ts:52-55)
// ============================================================================

struct RestartRecord {
    int64_t last_restart_at = 0;
    std::vector<int64_t> restarts_this_hour;
};

// ============================================================================
// 🎯 ChannelHealthMonitor (对齐 channel-health-monitor.ts:33-50)
// ============================================================================

// Callback interfaces for decoupling from channel manager
using GetSnapshotFn = std::function<std::unordered_map<std::string,
    std::unordered_map<std::string, ChannelHealthSnapshot>>()>;
using IsHealthMonitorEnabledFn = std::function<bool(const std::string&, const std::string&)>;
using IsManuallyStopped = std::function<bool(const std::string&, const std::string&)>;
using StopChannelFn = std::function<void(const std::string&, const std::string&)>;
using ResetRestartAttemptsFn = std::function<void(const std::string&, const std::string&)>;
using StartChannelFn = std::function<void(const std::string&, const std::string&)>;
using SkipStaleSocketCheckFn = std::function<bool(const std::string&)>;

struct ChannelHealthMonitorDeps {
    GetSnapshotFn get_runtime_snapshot;
    IsHealthMonitorEnabledFn is_health_monitor_enabled;
    IsManuallyStopped is_manually_stopped;
    StopChannelFn stop_channel;
    ResetRestartAttemptsFn reset_restart_attempts;
    StartChannelFn start_channel;
    SkipStaleSocketCheckFn skip_stale_socket_check;

    int64_t check_interval_ms = DEFAULT_CHECK_INTERVAL_MS;
    ChannelHealthTimingPolicy timing;
    int cooldown_cycles = DEFAULT_COOLDOWN_CYCLES;
    int max_restarts_per_hour = DEFAULT_MAX_RESTARTS_PER_HOUR;
};

class ChannelHealthMonitor {
public:
    explicit ChannelHealthMonitor(ChannelHealthMonitorDeps deps)
        : deps_(std::move(deps))
        , cooldown_ms_(deps_.cooldown_cycles * deps_.check_interval_ms)
        , started_at_(now_ms())
    {
        if (log()) {
            log()->info("started (interval: {}s, startup-grace: {}s, channel-connect-grace: {}s)",
                deps_.check_interval_ms / 1000,
                deps_.timing.monitor_startup_grace_ms / 1000,
                deps_.timing.channel_connect_grace_ms / 1000);
        }
        monitor_thread_ = std::thread([this]() { run_loop(); });
    }

    ~ChannelHealthMonitor() { stop(); }

    void stop() {
        stopped_.store(true);
        cv_.notify_all();
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

private:
    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void run_loop() {
        while (!stopped_.load()) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(deps_.check_interval_ms),
                    [this] { return stopped_.load(); });
            }
            if (stopped_.load()) break;
            run_check();
        }
    }

    // 🎯 runCheck — 对齐 channel-health-monitor.ts:100-181
    void run_check() {
        if (check_in_flight_.exchange(true)) return;

        try {
            int64_t now = now_ms();

            // 启动宽限期 — 对齐 :108-110
            if (now - started_at_ < deps_.timing.monitor_startup_grace_ms) {
                check_in_flight_.store(false);
                return;
            }

            auto snapshot = deps_.get_runtime_snapshot();

            for (auto& [channel_id, accounts] : snapshot) {
                for (auto& [account_id, status] : accounts) {
                    if (!deps_.is_health_monitor_enabled(channel_id, account_id)) continue;
                    if (deps_.is_manually_stopped(channel_id, account_id)) continue;

                    ChannelHealthPolicy policy;
                    policy.channel_id = channel_id;
                    policy.now = now;
                    policy.stale_event_threshold_ms = deps_.timing.stale_event_threshold_ms;
                    policy.channel_connect_grace_ms = deps_.timing.channel_connect_grace_ms;
                    policy.skip_stale_socket_check = deps_.skip_stale_socket_check(channel_id);

                    auto health = evaluate_channel_health(status, policy);
                    if (health.healthy) continue;

                    std::string key = channel_id + ":" + account_id;
                    auto& record = restart_records_[key];

                    // 冷却检查 — 对齐 :146-148
                    if (now - record.last_restart_at <= cooldown_ms_) continue;

                    // 修剪旧记录 — 对齐 :96-98
                    record.restarts_this_hour.erase(
                        std::remove_if(record.restarts_this_hour.begin(),
                            record.restarts_this_hour.end(),
                            [now](int64_t at) { return now - at >= ONE_HOUR_MS; }),
                        record.restarts_this_hour.end());

                    // 频率限制 — 对齐 :151-156
                    if (static_cast<int>(record.restarts_this_hour.size()) >=
                        deps_.max_restarts_per_hour)
                    {
                        if (log()) {
                            log()->warn("[{}:{}] health-monitor: hit {} restarts/hour limit, skipping",
                                channel_id, account_id, deps_.max_restarts_per_hour);
                        }
                        continue;
                    }

                    auto reason = resolve_channel_restart_reason(status, health);

                    if (log()) {
                        log()->info("[{}:{}] health-monitor: restarting (reason: {})",
                            channel_id, account_id, restart_reason_to_string(reason));
                    }

                    // 执行重启 — 对齐 :162-175
                    try {
                        if (status.running) {
                            deps_.stop_channel(channel_id, account_id);
                        }
                        deps_.reset_restart_attempts(channel_id, account_id);
                        deps_.start_channel(channel_id, account_id);

                        record.last_restart_at = now;
                        record.restarts_this_hour.push_back(now);
                    } catch (const std::exception& e) {
                        if (log()) {
                            log()->error("[{}:{}] health-monitor: restart failed: {}",
                                channel_id, account_id, e.what());
                        }
                    }
                }
            }
        } catch (...) {}

        check_in_flight_.store(false);
    }

    ChannelHealthMonitorDeps deps_;
    int64_t cooldown_ms_;
    int64_t started_at_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> check_in_flight_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread monitor_thread_;
    std::unordered_map<std::string, RestartRecord> restart_records_;
};

} // namespace gateway
} // namespace hunterclaw
