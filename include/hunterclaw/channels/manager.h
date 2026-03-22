#pragma once

#include "hunterclaw/channels/plugin.h"
#include "hunterclaw/core/types.h"
#include "hunterclaw/core/json.h"
#include "hunterclaw/tools/tool_hooks.h"   // get_global_hook_manager(), PluginHook* types
#include <spdlog/spdlog.h>

// Forward declaration — avoid circular include with message_dispatcher.h
namespace hunterclaw { namespace auto_reply { class MessageDispatcher; } }
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

/// Channel Activity 追踪 (100% 对齐 OpenClaw: channel-activity.ts)
/// 记录每个 channel:account 的最后收发消息时间
struct ChannelActivityEntry {
    std::optional<int64_t> inbound_at;   // lastInboundAt (ms)
    std::optional<int64_t> outbound_at;  // lastOutboundAt (ms)
};

enum class ChannelDirection { Inbound, Outbound };

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

    // Aligned with openclaw: inject dispatchReplyFromConfig runtime.
    // Call this after MessageDispatcher is constructed so that all subsequent
    // ctx.dispatch_inbound calls are routed to the real dispatcher.
    void set_dispatcher(std::shared_ptr<auto_reply::MessageDispatcher> dispatcher);

    // ========== Channel Activity (对齐 OpenClaw: channel-activity.ts) ==========
    // recordChannelActivity: 记录 channel 收发消息时间
    void record_channel_activity(
        const ChannelId& channel_id,
        const std::string& account_id,
        ChannelDirection direction,
        std::optional<int64_t> at = std::nullopt
    );

    // getChannelActivity: 获取 channel 收发消息时间
    ChannelActivityEntry get_channel_activity(
        const ChannelId& channel_id,
        const std::string& account_id
    ) const;

    // ── Status Issues (对齐 OpenClaw: getStatusIssues) ────────────────────────
    // Aligned with openclaw: getStatusIssues() → collect issues from all plugins
    std::vector<nlohmann::json> get_status_issues() const;

    // ── Reload Config (对齐 OpenClaw: reloadConfig) ───────────────────────────
    // Aligned with openclaw: reloadConfig(changedPrefixes) → restart affected channels
    void reload_config(const std::vector<std::string>& changed_prefixes);

    // ── Execute Action (对齐 OpenClaw: executeChannelAction) ──────────────────
    // Aligned with openclaw: executeChannelAction(channel, action, params, cfg, accountId, ...)
    core::Result<nlohmann::json> execute_action(
        const ChannelId& channel_id,
        const std::string& action,
        const nlohmann::json& params,
        const core::Config& cfg,
        const std::optional<std::string>& account_id = std::nullopt,
        const std::optional<std::string>& requester_sender_id = std::nullopt,
        const std::optional<nlohmann::json>& tool_context = std::nullopt,
        const std::optional<std::vector<std::string>>& media_local_roots = std::nullopt
    );

    // ── List Actions (对齐 OpenClaw: listChannelActions) ──────────────────────
    // Aligned with openclaw: listChannelActions(channel, cfg)
    std::vector<std::string> list_actions(
        const ChannelId& channel_id,
        const core::Config& cfg
    ) const;

    // ── Setup Account (对齐 OpenClaw: setupChannelAccount) ────────────────────
    // Aligned with openclaw: setupChannelAccount(channel, accountId, input, cfg)
    core::Result<core::Config> setup_account(
        const ChannelId& channel_id,
        const std::string& account_id,
        const ChannelSetupInput& input,
        const core::Config& cfg
    );

    // ── Extract Tool Send (对齐 OpenClaw: extractChannelToolSend) ─────────────
    // Aligned with openclaw: extractChannelToolSend(channel, args)
    std::optional<nlohmann::json> extract_tool_send(
        const ChannelId& channel_id,
        const nlohmann::json& args
    ) const;

    // =========================================================================
    // Hook: message_received
    // Aligned with OpenClaw channels/plugins/hooks.ts — fire-and-forget
    // Called by channel plugins when an inbound message is received.
    // Fires the message_received hook so plugins can observe all inbound traffic.
    // =========================================================================
    void fire_message_received(
        const ChannelId& channel_id,
        const std::string& account_id,
        const std::string& from,
        const std::string& content,
        std::optional<int64_t> timestamp = std::nullopt,
        std::optional<std::string> metadata_json = std::nullopt
    );

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
