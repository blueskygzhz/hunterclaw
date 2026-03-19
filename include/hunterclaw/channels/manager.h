#pragma once

#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include <spdlog/spdlog.h>
#include <string>
#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>
#include <set>

namespace hunterclaw {
namespace channels {

// Type alias for channel identifier
using ChannelId = std::string;

/// ChannelAccountSnapshot is defined in plugin.h

/// Channel 运行时快照汇总 (100% 对齐 OpenClaw)
struct ChannelRuntimeSnapshot {
    // 默认账户快照 (每个 channel 一个)
    std::map<ChannelId, ChannelAccountSnapshot> channels;

    // 所有账户快照 (支持多账户)
    std::map<ChannelId, std::map<std::string, ChannelAccountSnapshot>> channel_accounts;

    nlohmann::json to_json() const;
};

/// Channel 启动选项 (100% 对齐 OpenClaw)
struct StartChannelOptions {
    bool preserve_restart_attempts = false;
    bool preserve_manual_stop = false;
    bool force = false;  // 强制重启即使已在运行
};

/// 重启策略 (100% 对齐 OpenClaw)
struct BackoffPolicy {
    int64_t initial_ms = 5000;
    int64_t max_ms = 5 * 60 * 1000;
    double factor = 2.0;
    double jitter = 0.1;
};

/// Channel 健康检查配置 (100% 对齐 OpenClaw)
struct ChannelHealthMonitorConfig {
    int64_t check_interval_ms = 5 * 60 * 1000;

    struct TimingConfig {
        int64_t monitor_startup_grace_ms = 60 * 1000;
        int64_t channel_connect_grace_ms = 120 * 1000;
        int64_t stale_event_threshold_ms = 30 * 60 * 1000;
    } timing;

    int cooldown_cycles = 2;
    int max_restarts_per_hour = 10;
};

/// Channel 账户状态枚举 (内部使用)
enum class ChannelAccountStatus {
    Stopped,
    Starting,
    Connected,
    Disconnected,
    Error
};

/// Channel 健康状态
struct ChannelHealth {
    ChannelId channel_id;
    std::string account_id;
    ChannelAccountStatus status;
    bool is_healthy;
    std::chrono::system_clock::time_point last_check_time;
};

// Forward declaration
class ChannelManagerImpl;

/// Channel Manager (100% 对齐 OpenClaw)
class ChannelManager {
public:
    using Logger = std::shared_ptr<spdlog::logger>;
    using LoadConfigFunc = std::function<core::Config()>;

    struct Options {
        LoadConfigFunc load_config;
        std::map<ChannelId, Logger> channel_logs;
        std::map<ChannelId, std::map<std::string, std::string>> channel_runtime_envs;
    };

    // Channel Runtime Store (internal)
    struct ChannelRuntimeStore {
        std::map<std::string, ChannelAccountSnapshot> runtimes;
        // Plugin descriptor (replaces raw ChannelAdapter)
        std::optional<ChannelPlugin> plugin;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> aborts;
    };

    // Factory method
    static std::shared_ptr<ChannelManager> create();

    explicit ChannelManager(const Options& options);
    ~ChannelManager();

    // Channel 生命周期 (100% 对齐 OpenClaw)
    core::Result<void> start_channel(
        ChannelId channel_id,
        const std::string& account_id = "",
        const StartChannelOptions& options = {}
    );

    core::Result<void> start_channels(
        const std::vector<ChannelId>& channel_ids = {}
    );

    core::Result<void> stop_channel(
        ChannelId channel_id,
        const std::string& account_id = ""
    );

    core::Result<void> mark_channel_logged_out(
        ChannelId channel_id,
        const std::string& account_id = ""
    );

    // 状态查询 (100% 对齐 OpenClaw)
    ChannelRuntimeSnapshot get_runtime_snapshot() const;

    std::vector<ChannelId> list_channels() const;

    bool is_manually_stopped(ChannelId channel_id, const std::string& account_id) const;

    void reset_restart_attempts(ChannelId channel_id, const std::string& account_id);

    // 健康监控
    void start_health_monitor(const ChannelHealthMonitorConfig& config = {});
    void stop_health_monitor();

private:
    std::unique_ptr<ChannelManagerImpl> impl_;
};

/// Channel 健康监控器 (独立组件,100% 对齐 OpenClaw)
class ChannelHealthMonitor {
public:
    explicit ChannelHealthMonitor(
        std::shared_ptr<ChannelManager> manager,
        const ChannelHealthMonitorConfig& config = {}
    );
    ~ChannelHealthMonitor();

    void start();
    void stop();
    bool is_running() const;

    std::vector<ChannelHealth> get_all_health() const;

private:
    void monitor_loop();
    void check_all_channels();

    std::shared_ptr<ChannelManager> manager_;
    ChannelHealthMonitorConfig config_;

    std::thread monitor_thread_;
    std::atomic<bool> running_{false};

    int64_t start_time_;
};

} // namespace channels
} // namespace hunterclaw
