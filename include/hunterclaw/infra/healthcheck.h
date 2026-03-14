#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>

namespace hunterclaw {
namespace infra {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

// ============================================================================
// 1. 健康检查状态
// ============================================================================

enum class HealthStatus {
    Healthy,        // 健康
    Degraded,       // 降级 (部分功能不可用)
    Unhealthy,      // 不健康
    Unknown         // 未知状态
};

// ============================================================================
// 2. 组件健康检查接口
// ============================================================================

class ComponentHealthChecker {
public:
    virtual ~ComponentHealthChecker() = default;
    
    struct CheckResult {
        HealthStatus status = HealthStatus::Unknown;
        std::string component_name;
        std::optional<std::string> error_message;
        std::unordered_map<std::string, std::string> metadata;
        std::chrono::milliseconds check_duration{0};
    };
    
    virtual CheckResult check() = 0;
    virtual std::string name() const = 0;
};

// ============================================================================
// 3. Channel 健康检查器
// ============================================================================

class ChannelHealthChecker : public ComponentHealthChecker {
public:
    struct ChannelStatus {
        std::string channel_id;
        bool connected = false;
        std::optional<Clock::time_point> last_event_time;
        std::optional<Clock::time_point> connected_at;
        int reconnect_attempts = 0;
    };
    
    void update_channel_status(const std::string& channel_id, const ChannelStatus& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        channel_status_[channel_id] = status;
    }
    
    CheckResult check() override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto start = Clock::now();
        std::vector<std::string> failing_channels;
        std::vector<std::string> stale_channels;
        
        auto now = Clock::now();
        
        for (const auto& [channel_id, status] : channel_status_) {
            if (!status.connected) {
                failing_channels.push_back(channel_id);
                continue;
            }
            
            // 检查事件活性 (30分钟无事件视为 stale)
            if (status.last_event_time) {
                auto idle_duration = now - *status.last_event_time;
                if (idle_duration > std::chrono::minutes(30)) {
                    stale_channels.push_back(channel_id);
                }
            }
        }
        
        HealthStatus health_status;
        std::string error_msg;
        
        if (failing_channels.empty() && stale_channels.empty()) {
            health_status = HealthStatus::Healthy;
        } else if (failing_channels.empty() && !stale_channels.empty()) {
            health_status = HealthStatus::Degraded;
            error_msg = "Stale channels: " + join(stale_channels, ", ");
        } else {
            health_status = HealthStatus::Unhealthy;
            error_msg = "Failing channels: " + join(failing_channels, ", ");
        }
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);
        
        return CheckResult{
            health_status,
            "channels",
            error_msg.empty() ? std::nullopt : std::make_optional(error_msg),
            {
                {"total_channels", std::to_string(channel_status_.size())},
                {"failing_count", std::to_string(failing_channels.size())},
                {"stale_count", std::to_string(stale_channels.size())}
            },
            duration
        };
    }
    
    std::string name() const override { return "channels"; }
    
    std::vector<std::string> get_failing_channels() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> failing;
        for (const auto& [channel_id, status] : channel_status_) {
            if (!status.connected) {
                failing.push_back(channel_id);
            }
        }
        return failing;
    }

private:
    std::string join(const std::vector<std::string>& vec, const std::string& sep) const {
        if (vec.empty()) return "";
        std::string result = vec[0];
        for (size_t i = 1; i < vec.size(); ++i) {
            result += sep + vec[i];
        }
        return result;
    }
    
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ChannelStatus> channel_status_;
};

// ============================================================================
// 4. 数据库健康检查器
// ============================================================================

class DatabaseHealthChecker : public ComponentHealthChecker {
public:
    CheckResult check() override {
        auto start = Clock::now();
        
        try {
            // 执行简单的 ping 查询: SELECT 1
            // bool success = db_->execute("SELECT 1");
            bool success = true; // 模拟
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start);
            
            if (success) {
                return CheckResult{
                    HealthStatus::Healthy,
                    "database",
                    std::nullopt,
                    {{"ping_ms", std::to_string(duration.count())}},
                    duration
                };
            } else {
                return CheckResult{
                    HealthStatus::Unhealthy,
                    "database",
                    "Database ping failed",
                    {},
                    duration
                };
            }
            
        } catch (const std::exception& e) {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start);
            return CheckResult{
                HealthStatus::Unhealthy,
                "database",
                e.what(),
                {},
                duration
            };
        }
    }
    
    std::string name() const override { return "database"; }
};

// ============================================================================
// 5. 进程资源健康检查器
// ============================================================================

class ProcessHealthChecker : public ComponentHealthChecker {
public:
    struct ResourceLimits {
        float max_cpu_percent = 80.0f;
        uint64_t max_memory_mb = 1024;
        int max_thread_count = 100;
        int max_fd_count = 1024;
    };
    
    explicit ProcessHealthChecker(const ResourceLimits& limits = {})
        : limits_(limits) {}
    
    CheckResult check() override {
        auto start = Clock::now();
        
        // 获取当前资源使用情况
        auto cpu = get_cpu_percent();
        auto memory_mb = get_memory_mb();
        auto thread_count = get_thread_count();
        auto fd_count = get_fd_count();
        
        std::vector<std::string> warnings;
        HealthStatus status = HealthStatus::Healthy;
        
        if (cpu > limits_.max_cpu_percent) {
            warnings.push_back("CPU usage high: " + std::to_string(cpu) + "%");
            status = HealthStatus::Degraded;
        }
        
        if (memory_mb > limits_.max_memory_mb) {
            warnings.push_back("Memory usage high: " + std::to_string(memory_mb) + " MB");
            status = HealthStatus::Degraded;
        }
        
        if (thread_count > limits_.max_thread_count) {
            warnings.push_back("Thread count high: " + std::to_string(thread_count));
            status = HealthStatus::Degraded;
        }
        
        if (fd_count > limits_.max_fd_count) {
            warnings.push_back("FD count high: " + std::to_string(fd_count));
            status = HealthStatus::Degraded;
        }
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start);
        
        std::string error_msg = warnings.empty() ? "" : warnings[0];
        
        return CheckResult{
            status,
            "process",
            error_msg.empty() ? std::nullopt : std::make_optional(error_msg),
            {
                {"cpu_percent", std::to_string(cpu)},
                {"memory_mb", std::to_string(memory_mb)},
                {"threads", std::to_string(thread_count)},
                {"fds", std::to_string(fd_count)}
            },
            duration
        };
    }
    
    std::string name() const override { return "process"; }

private:
    float get_cpu_percent() const {
        // 实现略 (读取 /proc/stat 或使用 getrusage)
        return 15.0f;
    }
    
    uint64_t get_memory_mb() const {
        // 实现略 (读取 /proc/self/status 或使用 getrusage)
        return 256;
    }
    
    int get_thread_count() const {
        // 实现略 (读取 /proc/self/status)
        return 24;
    }
    
    int get_fd_count() const {
        // 实现略 (读取 /proc/self/fd)
        return 64;
    }
    
    ResourceLimits limits_;
};

// ============================================================================
// 6. 综合健康检查管理器
// ============================================================================

class HealthCheckManager {
public:
    struct ReadinessResult {
        bool ready = true;
        std::vector<std::string> failing;
        std::chrono::milliseconds uptime_ms{0};
        std::unordered_map<std::string, ComponentHealthChecker::CheckResult> component_results;
    };
    
    void register_checker(std::shared_ptr<ComponentHealthChecker> checker) {
        std::lock_guard<std::mutex> lock(mutex_);
        checkers_[checker->name()] = checker;
    }
    
    ReadinessResult check_readiness() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        ReadinessResult result;
        result.uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - start_time_);
        
        for (const auto& [name, checker] : checkers_) {
            auto check_result = checker->check();
            result.component_results[name] = check_result;
            
            if (check_result.status == HealthStatus::Unhealthy) {
                result.ready = false;
                result.failing.push_back(name);
            }
        }
        
        return result;
    }
    
    json to_json(const ReadinessResult& result) const {
        json j = {
            {"ready", result.ready},
            {"uptime_ms", result.uptime_ms.count()},
            {"components", json::object()}
        };
        
        if (!result.failing.empty()) {
            j["failing"] = result.failing;
        }
        
        for (const auto& [name, check_result] : result.component_results) {
            j["components"][name] = {
                {"status", health_status_to_string(check_result.status)},
                {"check_duration_ms", check_result.check_duration.count()}
            };
            
            if (check_result.error_message) {
                j["components"][name]["error"] = *check_result.error_message;
            }
            
            if (!check_result.metadata.empty()) {
                j["components"][name]["metadata"] = check_result.metadata;
            }
        }
        
        return j;
    }
    
    // HTTP 端点处理器
    std::pair<int, std::string> handle_healthz_request() {
        auto result = check_readiness();
        int status_code = result.ready ? 200 : 503;
        return {status_code, to_json(result).dump(2)};
    }
    
    std::pair<int, std::string> handle_readyz_request() {
        return handle_healthz_request(); // 相同逻辑
    }

private:
    static std::string health_status_to_string(HealthStatus status) {
        switch (status) {
            case HealthStatus::Healthy: return "healthy";
            case HealthStatus::Degraded: return "degraded";
            case HealthStatus::Unhealthy: return "unhealthy";
            case HealthStatus::Unknown: return "unknown";
            default: return "unknown";
        }
    }
    
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ComponentHealthChecker>> checkers_;
    Clock::time_point start_time_ = Clock::now();
};

// ============================================================================
// 7. 优雅关闭管理器
// ============================================================================

class GracefulShutdownManager {
public:
    using ShutdownCallback = std::function<void()>;
    
    struct ShutdownConfig {
        std::chrono::milliseconds grace_period{30000};  // 30秒宽限期
        bool force_after_timeout = true;                // 超时后强制退出
    };
    
    explicit GracefulShutdownManager(const ShutdownConfig& config = {})
        : config_(config) {}
    
    void register_callback(const std::string& name, ShutdownCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_[name] = std::move(callback);
    }
    
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (shutdown_initiated_) {
            return; // 已经在关闭中
        }
        shutdown_initiated_ = true;
        
        auto start = Clock::now();
        
        // 按顺序执行关闭回调
        std::vector<std::string> callback_order = {
            "http_server",      // 1. 停止接收新连接
            "channels",         // 2. 关闭 Channel 连接
            "agent_sessions",   // 3. 保存 Agent 状态
            "database",         // 4. 关闭数据库连接
            "logger"            // 5. 刷新日志缓冲
        };
        
        for (const auto& name : callback_order) {
            auto it = callbacks_.find(name);
            if (it != callbacks_.end()) {
                try {
                    it->second(); // 执行回调
                } catch (const std::exception& e) {
                    // 记录错误但继续关闭
                }
            }
            
            // 检查超时
            auto elapsed = Clock::now() - start;
            if (elapsed > config_.grace_period) {
                if (config_.force_after_timeout) {
                    break; // 强制退出
                }
            }
        }
    }
    
    bool is_shutting_down() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return shutdown_initiated_;
    }

private:
    ShutdownConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ShutdownCallback> callbacks_;
    bool shutdown_initiated_ = false;
};

} // namespace infra
} // namespace hunterclaw
