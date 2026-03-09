#pragma once

#include "hunterclaw/channels/channel.h"
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

/// Channel账户状态
enum class ChannelAccountStatus {
    Stopped,
    Starting,
    Connected,
    Disconnected,
    Error
};

/// Channel 运行时快照 (100% 对齐 OpenClaw)
struct ChannelAccountSnapshot {
    std::string account_id;
    bool enabled = true;
    bool configured = true;
    bool running = false;
    bool connected = false;
    ChannelAccountStatus status = ChannelAccountStatus::Stopped;
    
    std::optional<int64_t> last_start_at;
    std::optional<int64_t> last_stop_at;
    std::optional<std::string> last_error;
    int reconnect_attempts = 0;
    std::optional<int64_t> last_message_at;
    
    nlohmann::json to_json() const;
    static core::Result<ChannelAccountSnapshot> from_json(const nlohmann::json& j);
};

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
    int64_t initial_ms = 5000;       // 首次重试 5 秒
    int64_t max_ms = 5 * 60 * 1000;  // 最大 5 分钟
    double factor = 2.0;             // 指数退避因子
    double jitter = 0.1;             // 10% 抖动
};

/// Channel 健康检查配置 (100% 对齐 OpenClaw)
struct ChannelHealthMonitorConfig {
    int64_t check_interval_ms = 5 * 60 * 1000;  // 5 分钟
    
    struct TimingConfig {
        int64_t monitor_startup_grace_ms = 60 * 1000;    // 监控器启动宽限期 60 秒
        int64_t channel_connect_grace_ms = 120 * 1000;   // Channel 连接宽限期 120 秒
        int64_t stale_event_threshold_ms = 30 * 60 * 1000;  // 事件过期阈值 30 分钟
    } timing;
    
    int cooldown_cycles = 2;        // 冷却周期
    int max_restarts_per_hour = 10; // 每小时最大重启次数
};

/// Channel健康状态
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
    using GetStatusFunc = std::function<ChannelAccountSnapshot()>;
    using SetStatusFunc = std::function<void(const ChannelAccountSnapshot&)>;
    
    struct Options {
        LoadConfigFunc load_config;
        std::map<ChannelId, Logger> channel_logs;
        std::map<ChannelId, std::map<std::string, std::string>> channel_runtime_envs;
    };
    
    // Channel Runtime Store (internal)
    struct ChannelRuntimeStore {
        std::map<std::string, ChannelAccountSnapshot> runtimes;
        std::shared_ptr<ChannelAdapter> adapter;
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
        const std::vector<ChannelId>& channel_ids = {}  // 空 = 启动所有
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
    
    // 列出所有已加载的Channel IDs
    std::vector<ChannelId> list_channels() const;
    
    bool is_manually_stopped(ChannelId channel_id, const std::string& account_id) const;
    
    void reset_restart_attempts(ChannelId channel_id, const std::string& account_id);
    
    // 健康监控
    void start_health_monitor(const ChannelHealthMonitorConfig& config = {});
    void stop_health_monitor();
    
private:
    // Pimpl idiom - 实现细节隐藏在 .cc 文件中
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
