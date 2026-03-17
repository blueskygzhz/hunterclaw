#pragma once

#include "hunterclaw/core/types.h"  // ✅ Config 定义在 types.h 中
#include <functional>
#include <thread>
#include <atomic>
#include <chrono>

namespace hunterclaw {
namespace config {

/**
 * 🎯 100% 对齐 OpenClaw: 配置热更新
 * 
 * 功能:
 * - 监听配置文件变更
 * - 自动重载配置
 * - 支持 hot/restart/hybrid 模式
 * - 变更检测和差异分析
 * 
 * OpenClaw 参考:
 * - src/gateway/config-reload.ts
 * - src/gateway/config-reload-plan.ts
 */

/**
 * 重载模式
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export type GatewayReloadMode = "off" | "restart" | "hot" | "hybrid";
 * ```
 */
enum class ReloadMode {
    Off,       // 不重载
    Restart,   // 总是重启
    Hot,       // 总是热更新
    Hybrid     // 自动选择 (默认)
};

/**
 * 重载计划
 * 
 * OpenClaw 对齐:
 * ```typescript
 * export type GatewayReloadPlan = {
 *   restartGateway: boolean;
 *   restartReasons: string[];
 *   restartCron: boolean;
 *   restartBrowserControl: boolean;
 *   restartHeartbeat: boolean;
 *   restartChannels: { [K in ChannelKind]?: boolean };
 *   hotReloadableOnly: boolean;
 * };
 * ```
 */
struct ReloadPlan {
    bool restart_gateway = false;
    std::vector<std::string> restart_reasons;
    
    bool restart_cron = false;
    bool restart_heartbeat = false;
    bool restart_auth = false;
    bool restart_channels = false;
    
    bool hot_reloadable_only = false;
    
    // 变更的配置路径
    std::vector<std::string> changed_paths;
};

/**
 * 配置快照
 */
struct ConfigSnapshot {
    bool exists = false;
    std::string content_hash;
    int64_t mtime_ms = 0;
    size_t size = 0;
    
    std::shared_ptr<core::Config> config;
};

/**
 * 重载设置
 * 
 * OpenClaw 对齐:
 * ```typescript
 * const settings = {
 *   mode: "hybrid",           // 自动选择 hot/restart
 *   debounceMs: 300,          // 防抖延迟
 * };
 * ```
 */
struct ReloadSettings {
    ReloadMode mode = ReloadMode::Hybrid;
    int64_t debounce_ms = 300;            // 防抖延迟
    int64_t missing_config_retry_delay_ms = 150;
    int max_missing_config_retries = 2;
};

/**
 * 配置重载器
 */
class ConfigReloader {
public:
    using ReloadCallback = std::function<void(const ReloadPlan&, std::shared_ptr<core::Config>)>;
    using RestartCallback = std::function<void(const ReloadPlan&, std::shared_ptr<core::Config>)>;
    
    explicit ConfigReloader(
        const std::string& config_path,
        std::shared_ptr<core::Config> initial_config,
        const ReloadSettings& settings = {}
    );
    
    ~ConfigReloader();
    
    /**
     * 设置回调函数
     */
    void set_hot_reload_callback(ReloadCallback callback) {
        hot_reload_callback_ = callback;
    }
    
    void set_restart_callback(RestartCallback callback) {
        restart_callback_ = callback;
    }
    
    /**
     * 启动/停止监听
     */
    void start();
    void stop();
    bool is_running() const { return running_; }
    
    /**
     * 手动触发重载检查
     */
    void trigger_reload();
    
    /**
     * 获取当前配置快照
     */
    ConfigSnapshot get_current_snapshot() const;
    
private:
    std::string config_path_;
    ConfigSnapshot current_snapshot_;
    ReloadSettings settings_;
    
    ReloadCallback hot_reload_callback_;
    RestartCallback restart_callback_;
    
    std::atomic<bool> running_{false};
    std::thread watch_thread_;
    std::chrono::steady_clock::time_point last_reload_time_;
    
    mutable std::mutex mutex_;
    
    void watch_loop();
    bool check_file_changed();
    ConfigSnapshot read_snapshot();
    ReloadPlan create_reload_plan(
        const ConfigSnapshot& prev,
        const ConfigSnapshot& next
    );
    std::vector<std::string> diff_config_paths(
        const core::Config& prev,
        const core::Config& next
    );
};

/**
 * 辅助函数: 判断路径是否需要重启 Gateway
 * 
 * OpenClaw 对齐:
 * ```typescript
 * const RESTART_REQUIRED_PATHS = [
 *   "gateway.host",
 *   "gateway.port",
 *   "gateway.auth.mode",
 *   "gateway.trustedProxies",
 * ];
 * ```
 */
inline bool path_requires_restart(const std::string& path) {
    const std::vector<std::string> restart_paths = {
        "gateway.host",
        "gateway.port",
        "gateway.tls_enabled",
        "gateway.auth.mode",
        "gateway.trusted_proxies",
        "agents.defaults.model",  // 模型变更可能需要重启
    };
    
    for (const auto& prefix : restart_paths) {
        if (path.find(prefix) == 0) {
            return true;
        }
    }
    
    return false;
}

/**
 * 辅助函数: 判断路径是否影响 Heartbeat
 */
inline bool path_affects_heartbeat(const std::string& path) {
    return path.find("gateway.heartbeat_interval") == 0 ||
           path.find("agents.defaults.heartbeat") == 0;
}

/**
 * 辅助函数: 判断路径是否影响 Auth
 */
inline bool path_affects_auth(const std::string& path) {
    return path.find("gateway.auth.") == 0;
}

} // namespace config
} // namespace hunterclaw
